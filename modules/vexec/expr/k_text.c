/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_text.c
 *	  Kernels over text, varchar and bpchar (pg_vector_executor.md §3.7,
 *	  H7 of §3.14): comparisons, and LIKE.
 *
 * Collations.  A kernel is bound only under a deterministic collation; a
 * nondeterministic one keeps PostgreSQL's function (§3.7, "Collations").
 * Under one whose collate_is_c is set, comparisons are on bytes; under the
 * other deterministic ones, equality is still on bytes, as texteq() and
 * bpchareq() compare (PG19:src/backend/utils/adt/varlena.c, varchar.c), and
 * order goes through varstr_cmp(), PostgreSQL's own, a value at a time.
 *
 * bpchar compares its values without their trailing blanks, as bcTruelen()
 * does (varchar.c:746-763), and its LIKE matches them with their blanks, as
 * bpcharlike, which is textlike, does.
 *
 * LIKE with a constant pattern.  Where the pattern is a literal, a literal
 * then %, % then a literal, or % around a literal -- no _ and no % within
 * -- and the database's encoding is UTF8 or a single-byte one, it is an
 * equality, a prefix, a suffix or a substring test on the value's bytes:
 * exact there, since PostgreSQL's LIKE compares a deterministic
 * collation's characters byte for byte (like_match.c), and in UTF8 a run
 * of whole characters matches only at a character's start.  Any other
 * pattern is matched by textlike() itself, a value at a time.  A pattern
 * that ends in its escape character binds no kernel: PostgreSQL raises on
 * it only when matching reaches the end, which depends on the value.
 *
 * Values are read through their layout: a Datum's bytes after its header,
 * a view's, or an offset's (§3.4.3).  A compressed or external Datum is
 * detoasted into the work batch first.  None of these kernels can raise.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/varlena.h"
#include "varatt.h"

#include "vexec.h"
#include "expr/expr.h"
#include "expr/kernel.h"

typedef enum TextOp
{
	TOP_EQ,
	TOP_NE,
	TOP_LT,
	TOP_LE,
	TOP_GT,
	TOP_GE
} TextOp;

typedef struct TextInfo
{
	uint8		op;
	bool		bpchar;			/* compared without trailing blanks */
} TextInfo;

/* What a call keeps: its operator, and whether its collation orders bytes. */
typedef struct TextCall
{
	TextInfo	info;
	bool		bytes_order;
} TextCall;

/* ---- reading values ---- */

/*
 * A varlena argument whose active values can be read as bytes: a Datum
 * column whose compressed or external values are detoasted into the work
 * batch.  Views and offsets hold plain bytes already.
 */
static VexecVec *
plain(VexecKernelCall *kc, VexecVec *v)
{
	const Datum *d;
	Datum	   *copy = NULL;
	int			n = v->encoding == VEXEC_CONST ? 1 : kc->nrows;
	int			i;

	if (v->shape.layout != VEXEC_DATUM || v->type->typlen == -2)
		return v;
	d = v->values;
	for (i = 0; i < n; i++)
	{
		varlena    *vl;

		if (v->encoding != VEXEC_CONST && !vexec_bit(kc->active, i))
			continue;
		if (vexec_vec_isnull(v, i))
			continue;
		vl = (varlena *) DatumGetPointer(d[i]);
		if (!VARATT_IS_EXTERNAL(vl) && !VARATT_IS_COMPRESSED(vl))
			continue;
		if (copy == NULL)
		{
			copy = vexec_batch_alloc(kc->work, sizeof(Datum) * n);
			memcpy(copy, d, sizeof(Datum) * n);
		}
		{
			MemoryContext old = MemoryContextSwitchTo(kc->work->mcxt);

			copy[i] = PointerGetDatum(detoast_attr(vl));
			MemoryContextSwitchTo(old);
		}
	}
	if (copy != NULL)
	{
		VexecVec   *c = vexec_batch_alloc(kc->work, sizeof(VexecVec));

		*c = *v;
		c->values = copy;
		return c;
	}
	return v;
}

static inline void
bytes_at(const VexecVec *v, int row, const char **p, int *len)
{
	Size		l;

	vexec_vec_value_bytes(v, vexec_arg_row(v, row), p, &l);
	*len = (int) l;
}

/* bpchar's length without its trailing blanks (bcTruelen, varchar.c) */
static inline int
truelen(const char *s, int len)
{
	while (len > 0 && s[len - 1] == ' ')
		len--;
	return len;
}

static inline bool
op_result(int c, int op)
{
	switch (op)
	{
		case TOP_EQ:
			return c == 0;
		case TOP_NE:
			return c != 0;
		case TOP_LT:
			return c < 0;
		case TOP_LE:
			return c <= 0;
		case TOP_GT:
			return c > 0;
		default:
			return c >= 0;
	}
}

/* ---- comparisons ---- */

static void
text_cmp(VexecKernelCall *kc)
{
	const TextCall *tc = kc->call->extra;
	const TextInfo *info = &tc->info;
	const VexecVec *l = kc->args[0];
	const VexecVec *r = kc->args[1];
	bool		bytes_order = tc->bytes_order;
	bool		equality = info->op == TOP_EQ || info->op == TOP_NE;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		const char *a;
		const char *b;
		int			alen;
		int			blen;
		int			c;

		bytes_at(l, i, &a, &alen);
		bytes_at(r, i, &b, &blen);
		if (info->bpchar)
		{
			alen = truelen(a, alen);
			blen = truelen(b, blen);
		}
		if (equality)
			c = (alen == blen && memcmp(a, b, alen) == 0) ? 0 : 1;
		else if (bytes_order)
		{
			c = memcmp(a, b, Min(alen, blen));
			if (c == 0 && alen != blen)
				c = alen < blen ? -1 : 1;
		}
		else
			c = varstr_cmp(a, alen, b, blen, kc->call->collation);
		vexec_result_bit(kc->result, i, op_result(c, info->op));
	}
}

/* Under a deterministic collation only (above). */
static bool
text_bind(VexecExpr *call, const void *info)
{
	Oid			cls = vexec_collation_class(call->collation);
	TextCall   *tc;

	if (cls == InvalidOid)
		return false;
	tc = palloc(sizeof(TextCall));
	tc->info = *(const TextInfo *) info;
	tc->bytes_order = cls == C_COLLATION_OID;
	call->extra = tc;
	return true;
}

static bool
text_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	args[0] = plain(kc, args[0]);
	args[1] = plain(kc, args[1]);
	v->fn = text_cmp;
	v->result = vexec_bool_bits;
	return true;
}

static const VexecKernelDef text_cmp_def = {"text compare", false, text_bind, text_variant};

/* ---- LIKE ---- */

typedef enum LikeKind
{
	LIKE_EXACT,					/* 'abc' */
	LIKE_PREFIX,				/* 'abc%' */
	LIKE_SUFFIX,				/* '%abc' */
	LIKE_CONTAINS,				/* '%abc%' */
	LIKE_ANY,					/* '%' */
	LIKE_GENERAL				/* anything else: textlike() */
} LikeKind;

typedef struct LikeInfo
{
	bool		negate;
} LikeInfo;

typedef struct LikePattern
{
	bool		negate;
	int			kind;
	char	   *lit;			/* the literal, escapes removed */
	int			litlen;
	Datum		pattern;		/* LIKE_GENERAL's, as textlike() takes it */
} LikePattern;

/*
 * A pattern's kind: a literal, unescaped, with at most a % at each end and
 * no other wildcard.  False for a pattern that ends in its escape.
 */
static bool
classify(const char *p, int plen, LikePattern *lp)
{
	bool		lead = false;
	bool		trail = false;
	int			start = 0;
	int			end = plen;
	int			i;
	char	   *lit;
	int			n = 0;

	/* a pattern ending in an unescaped backslash raises, depending on the value */
	for (i = 0; i < plen; i++)
	{
		if (p[i] == '\\')
		{
			if (i + 1 >= plen)
				return false;
			i++;
		}
	}

	while (start < end && p[start] == '%')
	{
		lead = true;
		start++;
	}
	if (start == end)
	{
		lp->kind = lead ? LIKE_ANY : LIKE_EXACT;
		lp->lit = "";
		lp->litlen = 0;
		return true;
	}
	/* trailing %s, unless the last one is escaped */
	while (end > start && p[end - 1] == '%')
	{
		int			bs = 0;

		for (i = end - 2; i >= start && p[i] == '\\'; i--)
			bs++;
		if (bs % 2 == 1)
			break;
		trail = true;
		end--;
	}

	lit = palloc(end - start + 1);
	for (i = start; i < end; i++)
	{
		if (p[i] == '\\')
		{
			i++;
			lit[n++] = p[i];
			continue;
		}
		if (p[i] == '%' || p[i] == '_')
		{
			lp->kind = LIKE_GENERAL;
			return true;
		}
		lit[n++] = p[i];
	}
	lit[n] = '\0';
	lp->lit = lit;
	lp->litlen = n;
	lp->kind = lead ? (trail ? LIKE_CONTAINS : LIKE_SUFFIX) : (trail ? LIKE_PREFIX : LIKE_EXACT);
	return true;
}

static bool
like_bind(VexecExpr *call, const void *info)
{
	List	   *args = IsA(call->expr, OpExpr) ? ((OpExpr *) call->expr)->args
		: ((FuncExpr *) call->expr)->args;
	Const	   *pat;
	text	   *t;
	LikePattern *lp;

	/* x LIKE ANY (array) keeps PostgreSQL's function */
	if (call->kind != VE_CALL)
		return false;
	if (list_length(args) != 2 || !IsA(lsecond(args), Const))
		return false;
	pat = lsecond_node(Const, args);
	if (pat->constisnull)
		return false;
	if (vexec_collation_class(call->collation) == InvalidOid)
		return false;
	t = DatumGetTextPP(pat->constvalue);
	lp = palloc0(sizeof(LikePattern));
	lp->negate = ((const LikeInfo *) info)->negate;
	if (!classify(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), lp))
		return false;
	/* byte tests only where whole characters match at their starts */
	if (lp->kind != LIKE_GENERAL && lp->kind != LIKE_ANY &&
		GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1)
		lp->kind = LIKE_GENERAL;
	lp->pattern = PointerGetDatum(t);
	call->extra = lp;
	return true;
}

static void
like_kernel(VexecKernelCall *kc)
{
	const LikePattern *lp = kc->call->extra;
	const VexecVec *s = kc->args[0];
	int			ll = lp->litlen;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		const char *a;
		int			alen;
		bool		m;

		bytes_at(s, i, &a, &alen);
		switch (lp->kind)
		{
			case LIKE_EXACT:
				m = alen == ll && memcmp(a, lp->lit, ll) == 0;
				break;
			case LIKE_PREFIX:
				m = alen >= ll && memcmp(a, lp->lit, ll) == 0;
				break;
			case LIKE_SUFFIX:
				m = alen >= ll && memcmp(a + alen - ll, lp->lit, ll) == 0;
				break;
			case LIKE_CONTAINS:
				m = ll == 0 || (alen >= ll && memmem(a, alen, lp->lit, ll) != NULL);
				break;
			case LIKE_ANY:
				m = true;
				break;
			default:
				{
					bool		isnull;
					Datum		d = vexec_vec_datum(kc->work, s, vexec_arg_row(s, i), &isnull);

					m = DatumGetBool(DirectFunctionCall2Coll(textlike, kc->call->collation,
															 d, lp->pattern));
					break;
				}
		}
		vexec_result_bit(kc->result, i, lp->negate ? !m : m);
	}
}

static bool
like_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	args[0] = plain(kc, args[0]);
	v->fn = like_kernel;
	v->result = vexec_bool_bits;
	return true;
}

static const VexecKernelDef like_def = {"like", false, like_bind, like_variant};

/* ---- the table ---- */

static const struct
{
	Oid			funcid;
	TextInfo	info;
}			text_funcs[] =
{
	{F_TEXTEQ, {TOP_EQ, false}}, {F_TEXTNE, {TOP_NE, false}},
	{F_TEXT_LT, {TOP_LT, false}}, {F_TEXT_LE, {TOP_LE, false}},
	{F_TEXT_GT, {TOP_GT, false}}, {F_TEXT_GE, {TOP_GE, false}},
	{F_BPCHAREQ, {TOP_EQ, true}}, {F_BPCHARNE, {TOP_NE, true}},
	{F_BPCHARLT, {TOP_LT, true}}, {F_BPCHARLE, {TOP_LE, true}},
	{F_BPCHARGT, {TOP_GT, true}}, {F_BPCHARGE, {TOP_GE, true}},
};

static const LikeInfo like_pos = {false};
static const LikeInfo like_neg = {true};

void
vexec_kernels_text(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	int			i;

	for (i = 0; i < lengthof(text_funcs); i++)
		add(text_funcs[i].funcid, &text_cmp_def, &text_funcs[i].info);
	add(F_TEXTLIKE, &like_def, &like_pos);
	add(F_TEXTNLIKE, &like_def, &like_neg);
	add(F_BPCHARLIKE, &like_def, &like_pos);
	add(F_BPCHARNLIKE, &like_def, &like_neg);
	add(F_LIKE_TEXT_TEXT, &like_def, &like_pos);
	add(F_NOTLIKE_TEXT_TEXT, &like_def, &like_neg);
}
