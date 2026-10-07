/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_text.c
 *	  Kernels over text, varchar and bpchar (pg_vector_executor.md §3.7,
 *	  H7 of §3.14): comparisons, LIKE, and length; and, for an INSERT's
 *	  values (§3.16), the length coercions of char(n) and varchar(n).
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
 * detoasted into the work batch first.  None of these kernels can raise:
 * a row PostgreSQL's function would raise on fails, and the function
 * raises it (§3.7).
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

/* ---- length ---- */

/*
 * length(text), char_length() and character_length(): characters, as
 * text_length() counts them (varlena.c).  In a single-byte encoding, the
 * bytes.  In UTF8, pg_mbstrlen_with_len()'s walk (mbutils.c): each
 * character as long as pg_utf_mblen() reads from its first byte, a NUL
 * where a character begins ending the value, and a character running past
 * the value's end an error -- the row fails, and PostgreSQL raises it.  A
 * corrupt value's count is the walk's, not its bytes'.  Another multibyte
 * encoding binds no kernel.  octet_length(text): the bytes.
 */
typedef struct LenInfo
{
	bool		octets;
} LenInfo;

static bool
len_bind(VexecExpr *call, const void *info)
{
	const LenInfo *li = info;

	if (!li->octets && GetDatabaseEncoding() != PG_UTF8 &&
		pg_database_encoding_max_length() != 1)
		return false;
	call->extra = (void *) info;
	return true;
}

/* pg_utf_mblen() (wchar.c), inline */
static inline int
utf8_char_len(unsigned char c)
{
	if ((c & 0x80) == 0)
		return 1;
	if ((c & 0xe0) == 0xc0)
		return 2;
	if ((c & 0xf0) == 0xe0)
		return 3;
	if ((c & 0xf8) == 0xf0)
		return 4;
	return 1;
}

/*
 * The characters of a UTF8 value, or -1 where a character runs past its
 * end.  Eight bytes at a time while they are ASCII and none is a NUL.
 */
static int32
utf8_chars(const char *a, int alen)
{
	const uint64 ones = UINT64CONST(0x0101010101010101);
	const uint64 highs = UINT64CONST(0x8080808080808080);
	int32		n = 0;
	int			j = 0;

	for (;;)
	{
		unsigned char c;
		int			l;

		while (j + 8 <= alen)
		{
			uint64		w;

			memcpy(&w, a + j, 8);
			if (((w | ((w - ones) & ~w)) & highs) != 0)
				break;
			j += 8;
			n += 8;
		}
		if (j >= alen)
			break;
		c = (unsigned char) a[j];
		if (c == 0)
			break;
		l = utf8_char_len(c);
		if (l > alen - j)
			return -1;
		j += l;
		n++;
	}
	return n;
}

static void
len_kernel(VexecKernelCall *kc)
{
	const LenInfo *li = kc->call->extra;
	const VexecVec *s = kc->args[0];
	bool		bytes = li->octets || pg_database_encoding_max_length() == 1;
	int32	   *out = (int32 *) kc->result->values;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		const char *a;
		int			alen;

		bytes_at(s, i, &a, &alen);
		if (bytes)
			out[i] = alen;
		else
		{
			int32		n = utf8_chars(a, alen);

			if (n < 0)
			{
				vexec_fail(kc, i);
				n = 0;
			}
			out[i] = n;
		}
	}
}

static bool
len_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	args[0] = plain(kc, args[0]);
	memset(&v->result, 0, sizeof(VexecShape));
	v->result.layout = VEXEC_FIXED;
	v->result.width = 4;
	v->result.stride = 4;
	v->fn = len_kernel;
	return true;
}

static const VexecKernelDef len_def = {"length", true, len_bind, len_variant};
static const VexecKernelDef octet_len_def = {"octet_length", false, len_bind, len_variant};

/* ---- the length coercions ---- */

/*
 * bpchar(bpchar, int4, bool) and varchar(varchar, int4, bool): a value
 * coerced to its typmod's length, as varchar.c's bpchar() and varchar()
 * coerce it -- the casts an INSERT gives a value for a char(n) or
 * varchar(n) column, after text's binary coercion.  The typmod and whether
 * the cast is explicit are constants.
 *
 *	bpchar	a value of n characters is kept; a shorter one is padded with
 *			blanks to n; a longer one is cut at n characters, as
 *			pg_mbcharcliplen() cuts it, where what is cut is all blanks or
 *			the cast is explicit
 *	varchar	a value of n bytes or fewer is kept, as varchar() keeps it
 *			before counting characters; a longer one is cut as bpchar's
 *			is, without padding
 *
 * A value too long whose cut bytes are not all blanks fails, for the
 * function to raise "value too long".  Characters are counted as
 * pg_mbstrlen_with_len() counts them in UTF8 (utf8_chars()), a character
 * running past the value's end failing the row; in a single-byte encoding
 * they are its bytes.  Another multibyte encoding binds no kernel.  The
 * result is OFFSETS, its bytes in the work batch.
 */
typedef struct CoerceInfo
{
	bool		bpchar;
} CoerceInfo;

typedef struct CoerceCall
{
	bool		bpchar;
	int32		maxlen;			/* characters; -1: the typmod is none */
	bool		isexplicit;
} CoerceCall;

static bool
coerce_bind(VexecExpr *call, const void *info)
{
	List	   *args;
	Const	   *typmod;
	Const	   *isexplicit;
	CoerceCall *cc;

	if (call->kind != VE_CALL || !IsA(call->expr, FuncExpr))
		return false;
	args = ((FuncExpr *) call->expr)->args;
	if (list_length(args) != 3 || !IsA(lsecond(args), Const) || !IsA(lthird(args), Const))
		return false;
	typmod = lsecond_node(Const, args);
	isexplicit = lthird_node(Const, args);
	if (typmod->constisnull || isexplicit->constisnull)
		return false;
	if (GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1)
		return false;
	cc = palloc0(sizeof(CoerceCall));
	cc->bpchar = ((const CoerceInfo *) info)->bpchar;
	cc->maxlen = DatumGetInt32(typmod->constvalue) < (int32) VARHDRSZ ? -1 :
		DatumGetInt32(typmod->constvalue) - (int32) VARHDRSZ;
	cc->isexplicit = DatumGetBool(isexplicit->constvalue);
	call->extra = cc;
	return true;
}

/*
 * The bytes of a value's first n characters, as pg_mbcharcliplen() cuts it:
 * a NUL where a character begins ends the value.  The value's characters
 * were counted already, none running past its end.
 */
static int
clip_chars(const char *a, int alen, int n, bool bytes)
{
	int			j = 0;
	int			nch = 0;

	if (bytes)
	{
		alen = Min(alen, n);
		while (j < alen && a[j] != '\0')
			j++;
		return j;
	}
	while (j < alen && a[j] != '\0')
	{
		int			l = utf8_char_len((unsigned char) a[j]);

		if (++nch > n)
			break;
		j += l;
	}
	return j;
}

static void
coerce_kernel(VexecKernelCall *kc)
{
	const CoerceCall *cc = kc->call->extra;
	const VexecVec *s = kc->args[0];
	VexecVec   *out = kc->result;
	bool		bytes = pg_database_encoding_max_length() == 1;
	int32	   *offsets = (int32 *) out->values;
	int64		size = 64;
	int64		used = 0;
	char	   *data;

	/* the input's bytes, and a row's padding, as the buffer's first size */
	for (int i = 0; i < kc->nrows; i++)
	{
		const char *a;
		int			alen;

		if (!vexec_bit(kc->active, i))
			continue;
		bytes_at(s, i, &a, &alen);
		size += alen + (cc->bpchar && cc->maxlen > 0 ? cc->maxlen : 0);
	}
	data = vexec_batch_alloc(kc->work, size);
	out->nbuffers = 1;
	out->buffers = vexec_batch_alloc0(kc->work, sizeof(char *));
	out->buffer_sizes = vexec_batch_alloc0(kc->work, sizeof(int64));

	offsets[0] = 0;
	for (int i = 0; i < kc->nrows; i++)
	{
		const char *a;
		int			alen;
		int			keep;
		int			pad = 0;

		offsets[i + 1] = (int32) used;
		if (!vexec_bit(kc->active, i))
			continue;
		bytes_at(s, i, &a, &alen);
		keep = alen;
		if (cc->maxlen >= 0 && !(!cc->bpchar && alen <= cc->maxlen))
		{
			int32		chars = bytes ? alen : utf8_chars(a, alen);

			if (chars < 0)
			{
				vexec_fail(kc, i);
				continue;
			}
			if (chars > cc->maxlen)
			{
				keep = clip_chars(a, alen, cc->maxlen, bytes);
				if (!cc->isexplicit)
				{
					int			j;

					for (j = keep; j < alen && a[j] == ' '; j++)
						;
					if (j < alen)
					{
						vexec_fail(kc, i);
						continue;
					}
				}
			}
			else if (cc->bpchar)
				pad = cc->maxlen - chars;
		}
		if (used + keep + pad > size)
		{
			/* a cut kept fewer bytes, a pad no more than counted: never */
			elog(ERROR, "vexec: a length coercion's result outgrew its buffer");
		}
		if (used + keep + pad > PG_INT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("a batch's offsets column cannot hold more than %d bytes",
							PG_INT32_MAX)));
		memcpy(data + used, a, keep);
		memset(data + used + keep, ' ', pad);
		used += keep + pad;
		offsets[i + 1] = (int32) used;
	}
	out->buffers[0] = data;
	out->buffer_sizes[0] = size;
}

static bool
coerce_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	args[0] = plain(kc, args[0]);
	memset(&v->result, 0, sizeof(VexecShape));
	v->result.layout = VEXEC_OFFSETS;
	v->fn = coerce_kernel;
	return true;
}

static const VexecKernelDef bpchar_coerce_def = {"bpchar", true, coerce_bind, coerce_variant};
static const VexecKernelDef varchar_coerce_def = {"varchar", true, coerce_bind, coerce_variant};

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
static const LenInfo len_chars = {false};
static const LenInfo len_octets = {true};
static const CoerceInfo coerce_bpchar = {true};
static const CoerceInfo coerce_varchar = {false};

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
	add(F_LENGTH_TEXT, &len_def, &len_chars);
	add(F_CHAR_LENGTH_TEXT, &len_def, &len_chars);
	add(F_CHARACTER_LENGTH_TEXT, &len_def, &len_chars);
	add(F_OCTET_LENGTH_TEXT, &octet_len_def, &len_octets);
	add(F_BPCHAR_BPCHAR_INT4_BOOL, &bpchar_coerce_def, &coerce_bpchar);
	add(F_VARCHAR_VARCHAR_INT4_BOOL, &varchar_coerce_def, &coerce_varchar);
}
