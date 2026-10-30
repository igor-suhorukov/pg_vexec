/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_nested.c
 *	  Kernels over the nested layouts (pg_vector_executor.md §3.40, PQ21): a
 *	  list's cardinality() and x = ANY of it, a map's hstore -> key, ? key
 *	  and ?| keys, read where a scan's source handed them (vexec_source.h's
 *	  VEXEC_LIST and VEXEC_MAP) -- the offsets and the children, no array
 *	  and no hstore made of a row.
 *
 * Their answers are PostgreSQL's functions' of the values the row path
 * makes of the same rows -- a list's elements an array's, a map's pairs
 * hstore's -- and so are their errors:
 *
 *	- hstore keeps one of a key given twice, which one hstoreUniquePairs()'s
 *	  sort decides: a row whose map has the key -> asks for twice is
 *	  PostgreSQL's evaluator's, failed and redone;
 *	- the row path checks every value of a row's text as it makes it, so a
 *	  batch of a child whose source does not vouch for its text (F5) is
 *	  checked at once (vexec_text_batch_whole()), and a batch not whole goes
 *	  the row path -- fmgr over the made values, or, for x = ANY, PostgreSQL's
 *	  evaluator -- where the made value raises PostgreSQL's error;
 *	- a database not of UTF8 converts its text on the row path: the same.
 *
 * Over a Datum's layout -- an array or hstore of another source, a heap's --
 * cardinality() and hstore's functions run through fmgr, a row at a time;
 * x = ANY reads each row's array, as ScalarArrayOpExpr's evaluation does.
 *
 * hstore's functions are another extension's, bound as the kernel packs'
 * are (packs.c, vexec_extension_kernel_find()): the extension's name and a
 * version below, the function's name, arguments' types, symbol and library.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"
#include "expr/kernel.h"

/* A NULL row of a kernel's result */
static void
set_null(VexecKernelCall *kc, int i)
{
	VexecVec   *r = kc->result;

	if (r->validity == NULL)
		r->validity = vexec_bitmap_alloc(kc->work, kc->nrows, true);
	vexec_bit_clear(r->validity, i);
}

/* Whether a child of text is read where it lies: offsets of one buffer, the batch's text whole */
static bool
text_child_whole(const VexecVec *c)
{
	bool		ascii;

	if (c == NULL || c->shape.layout != VEXEC_OFFSETS || c->nbuffers != 1 || c->encoding != VEXEC_FLAT)
		return false;
	if (c->nvalues == 0)
		return true;
	return vexec_text_batch_whole(c, c->nvalues, PG_UTF8, &ascii);
}

/* A map whose keys and values are read where they lie (above) */
static bool
map_readable(const VexecVec *m)
{
	return m->shape.layout == VEXEC_MAP && m->encoding == VEXEC_FLAT && GetDatabaseEncoding() == PG_UTF8 &&
		text_child_whole(m->children[0]) && text_child_whole(m->children[1]);
}

/* A constant text argument, its value detoasted: false where it is not one */
static bool
const_text(VexecKernelCall *kc, VexecVec **arg)
{
	if ((*arg)->encoding != VEXEC_CONST || vexec_vec_isnull(*arg, 0))
		return false;
	*arg = vexec_text_plain(kc, *arg);
	return true;
}

/* The key a kernel's constant argument holds */
static void
const_key(const VexecVec *arg, const char **p, int *len)
{
	Size		l;

	vexec_vec_value_bytes(arg, 0, p, &l);
	*len = (int) l;
}

/* ---- cardinality() of a list: its offsets ---- */

static void
cardinality_kernel(VexecKernelCall *kc)
{
	const int32 *off = (const int32 *) kc->args[0]->values;
	int32	   *out = (int32 *) kc->result->values;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
		out[i] = off[i + 1] - off[i];
}

static bool
cardinality_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	const VexecVec *l = args[0];
	const VexecVec *e;

	if (l->shape.layout != VEXEC_LIST || l->encoding != VEXEC_FLAT)
		return false;
	/* the row path checks a text element's encoding as it makes the array */
	e = l->children[0];
	if (e->type->tclass == VEXEC_TC_VARLENA &&
		(GetDatabaseEncoding() != PG_UTF8 || !text_child_whole(e)))
		return false;
	memset(&v->result, 0, sizeof(VexecShape));
	v->result.layout = VEXEC_FIXED;
	v->result.width = sizeof(int32);
	v->result.stride = sizeof(int32);
	v->fn = cardinality_kernel;
	return true;
}

static const VexecKernelDef cardinality_def = {
	"cardinality", false, NULL, cardinality_variant, 0, VEXEC_LIST
};

/* ---- hstore's -> key, ? key and ?| keys over a map ---- */

/* The place of a key in a map's row among its keys, -1 none, -2 twice */
static inline int32
map_find(const int32 *moff, const int32 *koff, const char *kb, int row, const char *key, int keylen)
{
	int32		found = -1;

	for (int32 j = moff[row]; j < moff[row + 1]; j++)
		if (koff[j + 1] - koff[j] == keylen && memcmp(kb + koff[j], key, keylen) == 0)
		{
			if (found >= 0)
				return -2;
			found = j;
		}
	return found;
}

typedef struct MapKey
{
	const char *key;
	int			len;
} MapKey;

static void
fetchval_kernel(VexecKernelCall *kc)
{
	const VexecVec *m = kc->args[0];
	const VexecVec *keys = m->children[0];
	const VexecVec *vals = m->children[1];
	MapKey		key;
	const MapKey *k = &key;
	const int32 *moff = (const int32 *) m->values;
	const int32 *koff = (const int32 *) keys->values;
	const int32 *voff = (const int32 *) vals->values;
	const char *kb = keys->buffers[0];
	const char *vb = vals->buffers[0];
	VexecVec   *out = kc->result;
	int32	   *offsets = (int32 *) out->values;
	int32	   *at = vexec_batch_alloc(kc->work, sizeof(int32) * Max(kc->nrows, 1));
	int64		size = 0;
	int64		used = 0;
	char	   *data;

	const_key(kc->args[1], &key.key, &key.len);
	for (int i = 0; i < kc->nrows; i++)
		at[i] = -1;
	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		int32		j = map_find(moff, koff, kb, i, k->key, k->len);

		if (j == -2)
		{
			/* hstore keeps one of the two, which one its sort decides: PostgreSQL's evaluator's */
			vexec_fail(kc, i);
			continue;
		}
		if (j >= 0 && vexec_vec_isnull(vals, j))
			j = -1;
		at[i] = j;
		if (j >= 0)
			size += voff[j + 1] - voff[j];
	}
	if (size > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a batch's offsets column cannot hold more than %d bytes", PG_INT32_MAX)));
	data = vexec_batch_alloc(kc->work, Max(size, 1));
	out->nbuffers = 1;
	out->buffers = vexec_batch_alloc0(kc->work, sizeof(char *));
	out->buffer_sizes = vexec_batch_alloc0(kc->work, sizeof(int64));
	offsets[0] = 0;
	for (int i = 0; i < kc->nrows; i++)
	{
		int32		j = at[i];

		if (j >= 0)
		{
			int32		len = voff[j + 1] - voff[j];

			memcpy(data + used, vb + voff[j], len);
			used += len;
		}
		else if (vexec_bit(kc->active, i))
			set_null(kc, i);
		offsets[i + 1] = (int32) used;
	}
	out->buffers[0] = data;
	out->buffer_sizes[0] = size;
}

static bool
fetchval_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	if (!map_readable(args[0]) || !const_text(kc, &args[1]))
		return false;
	memset(&v->result, 0, sizeof(VexecShape));
	v->result.layout = VEXEC_OFFSETS;
	v->fn = fetchval_kernel;
	return true;
}

static void
exist_kernel(VexecKernelCall *kc)
{
	const VexecVec *m = kc->args[0];
	const VexecVec *keys = m->children[0];
	const int32 *moff = (const int32 *) m->values;
	const int32 *koff = (const int32 *) keys->values;
	MapKey		k;

	const_key(kc->args[1], &k.key, &k.len);
	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
		vexec_result_bit(kc->result, i, map_find(moff, koff, keys->buffers[0], i, k.key, k.len) != -1);
}

static bool
exist_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	if (!map_readable(args[0]) || !const_text(kc, &args[1]))
		return false;
	v->result = vexec_bool_bits;
	v->fn = exist_kernel;
	return true;
}

/* ?| keys: the constant array's keys, its NULLs left out as hstoreArrayToPairs() leaves them */
typedef struct MapKeys
{
	int			n;
	MapKey	   *keys;
} MapKeys;

/* The constant array's keys, made in the work batch */
static MapKeys *
array_keys(VexecKernelCall *kc, const VexecVec *arg)
{
	MapKeys    *ks = vexec_batch_alloc(kc->work, sizeof(MapKeys));
	MemoryContext old = MemoryContextSwitchTo(kc->work->mcxt);
	ArrayType  *arr = DatumGetArrayTypeP(((const Datum *) arg->values)[0]);
	Datum	   *elems;
	bool	   *nulls;
	int			n;

	deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &n);
	ks->keys = palloc(sizeof(MapKey) * Max(n, 1));
	ks->n = 0;
	for (int a = 0; a < n; a++)
	{
		text	   *t;

		if (nulls[a])
			continue;
		t = DatumGetTextPP(elems[a]);
		ks->keys[ks->n].key = VARDATA_ANY(t);
		ks->keys[ks->n].len = VARSIZE_ANY_EXHDR(t);
		ks->n++;
	}
	MemoryContextSwitchTo(old);
	return ks;
}

static void
exists_any_kernel(VexecKernelCall *kc)
{
	const VexecVec *m = kc->args[0];
	const VexecVec *keys = m->children[0];
	const MapKeys *ks = array_keys(kc, kc->args[1]);
	const int32 *moff = (const int32 *) m->values;
	const int32 *koff = (const int32 *) keys->values;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		bool		any = false;

		for (int a = 0; a < ks->n && !any; a++)
			any = map_find(moff, koff, keys->buffers[0], i, ks->keys[a].key, ks->keys[a].len) != -1;
		vexec_result_bit(kc->result, i, any);
	}
}

static bool
exists_any_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	if (!map_readable(args[0]) || args[1]->encoding != VEXEC_CONST || vexec_vec_isnull(args[1], 0) ||
		args[1]->shape.layout != VEXEC_DATUM)
		return false;
	v->result = vexec_bool_bits;
	v->fn = exists_any_kernel;
	return true;
}

static const VexecKernelDef fetchval_def = {"hstore ->", true, NULL, fetchval_variant, 0, VEXEC_MAP};
static const VexecKernelDef exist_def = {"hstore ?", false, NULL, exist_variant, 0, VEXEC_MAP};
static const VexecKernelDef exists_any_def = {"hstore ?|", false, NULL, exists_any_variant, 0, VEXEC_MAP};

/* hstore's versions whose C functions these are (contrib/hstore, 1.4 to 1.8) */
static const char *const hstore_versions[] = {"1.4", "1.5", "1.6", "1.7", "1.8", NULL};

static const VexecExtensionKernel extension_kernels[] = {
	{"hstore", hstore_versions, "fetchval", 2, {"hstore", "text"}, "hstore_fetchval", "$libdir/hstore",
	&fetchval_def, NULL},
	{"hstore", hstore_versions, "exist", 2, {"hstore", "text"}, "hstore_exists", "$libdir/hstore",
	&exist_def, NULL},
	{"hstore", hstore_versions, "exists_any", 2, {"hstore", "_text"}, "hstore_exists_any", "$libdir/hstore",
	&exists_any_def, NULL},
};

const VexecExtensionKernel *
vexec_extension_kernels(int *n)
{
	*n = lengthof(extension_kernels);
	return extension_kernels;
}

/* ---- x = ANY of a list column, or of each row's array ---- */

typedef enum AnyKind
{
	ANY_INT,					/* integers of 2, 4 or 8 bytes: the values widened */
	ANY_TEXT					/* text of a deterministic collation: the bytes */
} AnyKind;

typedef struct AnyInfo
{
	AnyKind		kind;
	int			xwidth;			/* ANY_INT: the scalar's bytes */
	int			ewidth;			/* and an element's */
	Oid			elemtype;
} AnyInfo;

static inline int64
int_at(const void *values, int width, int64 i)
{
	switch (width)
	{
		case 2:
			return ((const int16 *) values)[i];
		case 4:
			return ((const int32 *) values)[i];
		default:
			return ((const int64 *) values)[i];
	}
}

/* The scalar of row i as an integer, or its bytes */
static inline void
scalar_at(const VexecVec *x, const AnyInfo *ai, int i, int64 *iv, const char **p, int *len)
{
	int			r = vexec_arg_row(x, i);

	if (ai->kind == ANY_INT)
		*iv = int_at(x->values, ai->xwidth, r);
	else
	{
		Size		l;

		vexec_vec_value_bytes(x, r, p, &l);
		*len = (int) l;
	}
}

/* x = ANY over a list's elements where they lie */
static void
any_list_kernel(VexecKernelCall *kc)
{
	const AnyInfo *ai = kc->call->extra;
	const VexecVec *x = kc->args[0];
	const VexecVec *l = kc->args[1];
	const VexecVec *e = l->children[0];
	const int32 *off = (const int32 *) l->values;
	const int32 *eoff = ai->kind == ANY_TEXT ? (const int32 *) e->values : NULL;
	const char *eb = ai->kind == ANY_TEXT ? e->buffers[0] : NULL;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		int64		xi = 0;
		const char *xp = NULL;
		int			xlen = 0;
		bool		found = false;
		bool		null_elem = false;

		/* ANY's own NULLs: of the array, NULL; of none, false; of the scalar, NULL */
		if (vexec_vec_isnull(l, i))
		{
			set_null(kc, i);
			continue;
		}
		if (off[i + 1] == off[i])
		{
			vexec_result_bit(kc->result, i, false);
			continue;
		}
		if (vexec_vec_isnull(x, vexec_arg_row(x, i)))
		{
			set_null(kc, i);
			continue;
		}
		scalar_at(x, ai, i, &xi, &xp, &xlen);
		for (int32 j = off[i]; j < off[i + 1] && !found; j++)
		{
			if (vexec_vec_isnull(e, j))
				null_elem = true;
			else if (ai->kind == ANY_INT)
				found = int_at(e->values, ai->ewidth, j) == xi;
			else
				found = eoff[j + 1] - eoff[j] == xlen && memcmp(eb + eoff[j], xp, xlen) == 0;
		}
		if (found)
			vexec_result_bit(kc->result, i, true);
		else if (null_elem)
			set_null(kc, i);
		else
			vexec_result_bit(kc->result, i, false);
	}
}

/* x = ANY over each row's array, a Datum: ScalarArrayOpExpr's evaluation of one row's */
static void
any_datum_kernel(VexecKernelCall *kc)
{
	const AnyInfo *ai = kc->call->extra;
	const VexecVec *x = kc->args[0];
	const VexecVec *l = kc->args[1];
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;

	get_typlenbyvalalign(ai->elemtype, &elmlen, &elmbyval, &elmalign);
	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		int			r = vexec_arg_row(l, i);
		ArrayType  *arr;
		Datum	   *elems;
		bool	   *nulls;
		int			n;
		int64		xi = 0;
		const char *xp = NULL;
		int			xlen = 0;
		bool		found = false;
		bool		null_elem = false;

		if (vexec_vec_isnull(l, r))
		{
			set_null(kc, i);
			continue;
		}
		{
			MemoryContext old = MemoryContextSwitchTo(kc->work->mcxt);

			arr = DatumGetArrayTypeP(((const Datum *) l->values)[r]);
			deconstruct_array(arr, ai->elemtype, elmlen, elmbyval, elmalign, &elems, &nulls, &n);
			MemoryContextSwitchTo(old);
		}
		if (n == 0)
		{
			vexec_result_bit(kc->result, i, false);
			continue;
		}
		if (vexec_vec_isnull(x, vexec_arg_row(x, i)))
		{
			set_null(kc, i);
			continue;
		}
		scalar_at(x, ai, i, &xi, &xp, &xlen);
		for (int j = 0; j < n && !found; j++)
		{
			if (nulls[j])
				null_elem = true;
			else if (ai->kind == ANY_INT)
				found = (ai->ewidth == 2 ? DatumGetInt16(elems[j]) :
						 ai->ewidth == 4 ? DatumGetInt32(elems[j]) : DatumGetInt64(elems[j])) == xi;
			else
			{
				text	   *t = DatumGetTextPP(elems[j]);

				found = VARSIZE_ANY_EXHDR(t) == (Size) xlen && memcmp(VARDATA_ANY(t), xp, xlen) == 0;
			}
		}
		if (found)
			vexec_result_bit(kc->result, i, true);
		else if (null_elem)
			set_null(kc, i);
		else
			vexec_result_bit(kc->result, i, false);
	}
}

/* Every active row PostgreSQL's evaluator's: a batch this kernel does not read */
static void
any_redo_kernel(VexecKernelCall *kc)
{
	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		vexec_fail(kc, i);
		vexec_result_bit(kc->result, i, false);
	}
}

static bool
any_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	const AnyInfo *ai = kc->call->extra;
	const VexecVec *x;
	const VexecVec *l = args[1];
	bool		x_ok;

	v->result = vexec_bool_bits;
	if (ai->kind == ANY_TEXT)
		args[0] = vexec_text_plain(kc, args[0]);
	x = args[0];
	x_ok = ai->kind == ANY_INT ? x->shape.layout == VEXEC_FIXED && x->shape.width == ai->xwidth :
		x->shape.layout == VEXEC_DATUM || x->shape.layout == VEXEC_VIEW || x->shape.layout == VEXEC_OFFSETS;
	if (!x_ok || x->encoding == VEXEC_DICT)
	{
		v->fn = any_redo_kernel;
		return true;
	}
	if (l->shape.layout == VEXEC_LIST && l->encoding == VEXEC_FLAT)
	{
		const VexecVec *e = l->children[0];
		bool		e_ok = ai->kind == ANY_INT ?
			e->shape.layout == VEXEC_FIXED && e->shape.width == ai->ewidth :
			GetDatabaseEncoding() == PG_UTF8 && text_child_whole(e);

		v->fn = e_ok ? any_list_kernel : any_redo_kernel;
		return true;
	}
	if (l->shape.layout == VEXEC_DATUM && l->encoding != VEXEC_DICT)
	{
		v->fn = any_datum_kernel;
		return true;
	}
	v->fn = any_redo_kernel;
	return true;
}

/* can_fail: a batch it does not read is PostgreSQL's evaluator's, every row redone */
static const VexecKernelDef any_list_def = {"= ANY of an array column", true, NULL, any_variant, 1, VEXEC_LIST};

static const AnyInfo any_infos[] = {
	{ANY_INT, 2, 2, INT2OID}, {ANY_INT, 2, 4, INT4OID}, {ANY_INT, 2, 8, INT8OID},
	{ANY_INT, 4, 2, INT2OID}, {ANY_INT, 4, 4, INT4OID}, {ANY_INT, 4, 8, INT8OID},
	{ANY_INT, 8, 2, INT2OID}, {ANY_INT, 8, 4, INT4OID}, {ANY_INT, 8, 8, INT8OID},
	{ANY_TEXT, 0, 0, TEXTOID},
};

static VexecKernelEntry any_entries[lengthof(any_infos)];

/* An integer type's bytes, 0 for another */
static int
int_width(Oid t)
{
	return t == INT2OID ? 2 : t == INT4OID ? 4 : t == INT8OID ? 8 : 0;
}

/*
 * The kernel of x = ANY(array) where the array is no constant: of integers
 * by the integers' equality of their widths, of text by texteq under a
 * deterministic collation; NULL for any other
 */
const VexecKernelEntry *
vexec_any_list_kernel(Oid opfuncid, Oid scalar_type, Oid array_type, Oid collation)
{
	Oid			elem = get_element_type(array_type);
	int			xw = int_width(scalar_type);
	int			ew = int_width(elem);
	int			k = -1;
	static const Oid int_eq[3][3] = {
		{F_INT2EQ, F_INT24EQ, F_INT28EQ},
		{F_INT42EQ, F_INT4EQ, F_INT48EQ},
		{F_INT82EQ, F_INT84EQ, F_INT8EQ},
	};

	if (!OidIsValid(elem))
		return NULL;
	if (xw > 0 && ew > 0)
	{
		int			xi = xw == 2 ? 0 : xw == 4 ? 1 : 2;
		int			ei = ew == 2 ? 0 : ew == 4 ? 1 : 2;

		if (opfuncid != int_eq[xi][ei])
			return NULL;
		k = xi * 3 + ei;
	}
	else if (scalar_type == TEXTOID && elem == TEXTOID && opfuncid == F_TEXTEQ)
	{
		if (!OidIsValid(collation) || !get_collation_isdeterministic(collation))
			return NULL;
		k = 9;
	}
	if (k < 0 || !vexec_kernel_permitted(opfuncid))
		return NULL;
	any_entries[k].funcid = opfuncid;
	any_entries[k].def = &any_list_def;
	any_entries[k].info = &any_infos[k];
	return &any_entries[k];
}

void
vexec_kernels_nested(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	add(F_CARDINALITY, &cardinality_def, NULL);
}
