/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * eval.c
 *	  Vector programs over a batch (pg_vector_executor.md §3.7).
 *
 * vexec_eval() evaluates a compiled expression over the batch's `active`
 * rows and returns its column, valid at those rows; a boolean result is a
 * bit a row, its validity beside.  What PostgreSQL does not evaluate, it
 * does not evaluate:
 *
 *	- AND looks at its next operand only for the rows still undecided --
 *	  true or NULL so far -- and OR for those false or NULL so far, as
 *	  ExecEvalAnd and ExecEvalOr stop at the first false or true
 *	  (PG19:src/backend/executor/execExprInterp.c);
 *	- a strict function is not called for a row with a NULL argument
 *	  (execExprInterp.c:944-1000): its kernel computes only the rows whose
 *	  arguments are all valid, and the result there is NULL;
 *	- a kernel that meets a row PostgreSQL's function would raise on marks
 *	  it in the batch's redo rows, and the rows evaluated after it leave it
 *	  out: the node evaluates it with PostgreSQL's evaluator, in row order,
 *	  when it is asked for (exec/node.c);
 *	- a call bound to a kernel pack's declaration (vexec_kernels.h) calls
 *	  the extension's own function through fmgr only where its declaration
 *	  says it cannot raise, and sends the other rows to redo in the same
 *	  way (call_declared()).
 *
 * A parameter is read once a batch.  A PARAM_EXEC whose InitPlan has not
 * run yet (ParamExecData.execPlan) is read only where the rows evaluated
 * are exactly those PostgreSQL would reach -- the qual list's eager prefix,
 * with no redo row before -- since reading it runs the InitPlan, which may
 * raise or have side effects; elsewhere its rows go to PostgreSQL's
 * evaluator, which runs the InitPlan where PostgreSQL would.
 *
 * Registers -- the columns a program makes -- are allocated in the work
 * batch, reset with the input batch.
 *
 * A subtree that reads one input column and constants alone, where that
 * column is a dictionary's codes, is evaluated over the dictionary's values
 * and its rows take their values' results (VC5; Velox's peeling,
 * pg_vector_executor.md §3.18, X8).  Its results are kept for the
 * dictionary's identity, so that every batch with that dictionary maps its
 * codes and computes nothing; a dictionary without one is evaluated only
 * where its values are fewer than the rows they answer.  A value whose
 * kernel failed sends the rows holding it to redo; a value no row asked for
 * raises nothing, as a kernel's failure at a row nobody asked for.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "executor/executor.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "vexec_kernels.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "expr/kernel.h"

const VexecShape vexec_bool_bits = {VEXEC_BIT_BOOL, false, 0, 1, 1};

/* ---- bitmaps ---- */

uint64 *
vexec_bits_copy(VexecBatch *work, const uint64 *bits, int nrows)
{
	uint64	   *r = vexec_bitmap_alloc(work, nrows, bits == NULL);

	if (bits != NULL)
		memcpy(r, bits, sizeof(uint64) * VEXEC_WORDS(nrows));
	return r;
}

/* a & b; a NULL bitmap is every row */
uint64 *
vexec_bits_and(VexecBatch *work, const uint64 *a, const uint64 *b, int nrows)
{
	uint64	   *r;
	int			w;

	if (a == NULL)
		return vexec_bits_copy(work, b, nrows);
	if (b == NULL)
		return vexec_bits_copy(work, a, nrows);
	r = vexec_bitmap_alloc(work, nrows, false);
	for (w = 0; w < VEXEC_WORDS(nrows); w++)
		r[w] = a[w] & b[w];
	return r;
}

/* a & ~b */
uint64 *
vexec_bits_andnot(VexecBatch *work, const uint64 *a, const uint64 *b, int nrows)
{
	uint64	   *r = vexec_bits_copy(work, a, nrows);
	int			w;

	if (b == NULL)
	{
		memset(r, 0, sizeof(uint64) * VEXEC_WORDS(nrows));
		return r;
	}
	for (w = 0; w < VEXEC_WORDS(nrows); w++)
		r[w] &= ~b[w];
	return r;
}

void
vexec_bits_or_into(uint64 *dst, const uint64 *src, int nrows)
{
	int			w;

	if (src == NULL)
		return;
	for (w = 0; w < VEXEC_WORDS(nrows); w++)
		dst[w] |= src[w];
}

bool
vexec_bits_any(const uint64 *bits, int nrows)
{
	int			w;

	if (bits == NULL)
		return nrows > 0;
	for (w = 0; w < VEXEC_WORDS(nrows); w++)
		if (bits[w] != 0)
			return true;
	return false;
}

int
vexec_bits_count(const uint64 *bits, int nrows)
{
	int			n = 0;
	int			w;

	if (bits == NULL)
		return nrows;
	for (w = 0; w < VEXEC_WORDS(nrows); w++)
		n += pg_popcount64(bits[w]);
	return n;
}

/* The first set bit at or after `from`, or -1. */
int
vexec_bits_next(const uint64 *bits, int nrows, int from)
{
	int			w;

	if (from >= nrows)
		return -1;
	if (bits == NULL)
		return from;
	w = from >> 6;
	{
		uint64		word = bits[w] & (~UINT64CONST(0) << (from & 63));

		while (true)
		{
			if (word != 0)
			{
				int			i = w * 64 + pg_rightmost_one_pos64(word);

				return i < nrows ? i : -1;
			}
			if (++w >= VEXEC_WORDS(nrows))
				return -1;
			word = bits[w];
		}
	}
}

/* ---- registers ---- */

/* A flat register of a type's build shape, every row valid, values zero. */
static VexecVec *
new_register(VexecEval *ev, VexecType *type, const VexecShape *shape)
{
	VexecVec   *v = vexec_batch_alloc0(ev->work, sizeof(VexecVec));
	VexecShape	s;

	if (shape == NULL)
	{
		vexec_type_build_shape(type, &s);
		shape = &s;
	}
	v->type = type;
	vexec_vec_init(ev->work, v, shape, ev->nrows);
	return v;
}

/* Mark row r of a register NULL, making its validity on the first. */
static void
register_set_null(VexecEval *ev, VexecVec *v, int r)
{
	if (v->validity == NULL)
		v->validity = vexec_bitmap_alloc(ev->work, ev->nrows, true);
	vexec_bit_clear(v->validity, r);
}

/* Store a Datum at row r of a register in its type's build shape. */
static void
register_store(VexecEval *ev, VexecVec *v, int r, Datum value, bool isnull)
{
	const VexecType *type = v->type;

	if (isnull)
	{
		register_set_null(ev, v, r);
		return;
	}
	if (v->validity)
		vexec_bit_set(v->validity, r);
	switch (v->shape.layout)
	{
		case VEXEC_BYTE_BOOL:
			((uint8 *) v->values)[r] = DatumGetBool(value) ? 1 : 0;
			break;
		case VEXEC_FIXED:
			{
				char	   *p = (char *) v->values + (Size) r * v->shape.stride;

				if (type->typbyval)
					store_att_byval(p, value, type->typlen);
				else
					memcpy(p, DatumGetPointer(value), type->typlen);
				break;
			}
		case VEXEC_DATUM:
			((Datum *) v->values)[r] = vexec_varlena_copy(ev->work, type, value);
			break;
		default:
			elog(ERROR, "vexec: a register of layout %d takes no Datum", v->shape.layout);
	}
}

/* A constant register in the work batch. */
static VexecVec *
const_register(VexecEval *ev, VexecType *type, Datum value, bool isnull)
{
	VexecVec   *v = vexec_batch_alloc0(ev->work, sizeof(VexecVec));
	VexecShape	s;
	int			save = ev->nrows;

	vexec_type_build_shape(type, &s);
	v->type = type;
	vexec_vec_init(ev->work, v, &s, 1);
	ev->nrows = 1;
	register_store(ev, v, 0, value, isnull);
	ev->nrows = save;
	v->encoding = VEXEC_CONST;
	v->nvalues = 1;
	return v;
}

/* A boolean register: a bit a row, and its validity. */
static VexecVec *
bool_register(VexecEval *ev, uint64 *truth, uint64 *valid)
{
	VexecVec   *v = vexec_batch_alloc0(ev->work, sizeof(VexecVec));
	static VexecType *booltype = NULL;

	/*
	 * Made once a backend and kept for every statement after: in
	 * TopMemoryContext, not in the statement's memory, which is gone when
	 * the next statement's registers point at it.
	 */
	if (booltype == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		booltype = vexec_type_make(BOOLOID, -1, InvalidOid);
		MemoryContextSwitchTo(old);
	}
	v->type = booltype;
	v->shape = vexec_bool_bits;
	v->encoding = VEXEC_FLAT;
	v->nvalues = ev->nrows;
	v->values = truth;
	v->validity = valid;
	return v;
}

/*
 * A boolean column as full bitmaps of its truth and validity, whatever its
 * layout and encoding; *valid NULL when every row is valid.
 */
static void
bool_bits(VexecEval *ev, VexecVec *v, uint64 **truth, uint64 **valid)
{
	int			n = ev->nrows;
	int			i;

	if (v->encoding == VEXEC_CONST)
	{
		bool		isnull = v->validity != NULL && !vexec_bit(v->validity, 0);
		bool		val = !isnull && (v->shape.layout == VEXEC_BIT_BOOL ?
									  vexec_bit((uint64 *) v->values, 0) :
									  ((uint8 *) v->values)[0] != 0);

		*truth = vexec_bitmap_alloc(ev->work, n, val);
		*valid = isnull ? vexec_bitmap_alloc(ev->work, n, false) : NULL;
		return;
	}
	if (v->encoding == VEXEC_DICT)
	{
		VexecVec   *flat = vexec_batch_alloc(ev->work, sizeof(VexecVec));

		*flat = *v;
		vexec_vec_flatten(ev->work, flat);
		v = flat;
	}
	*valid = v->validity;
	if (v->shape.layout == VEXEC_BIT_BOOL)
		*truth = (uint64 *) v->values;
	else
	{
		const uint8 *bytes = v->values;

		*truth = vexec_bitmap_alloc(ev->work, n, false);
		for (i = 0; i < n; i++)
			if (bytes[i])
				vexec_bit_set(*truth, i);
	}
}

/* The rows a step computes: active, less those already sent to redo. */
static uint64 *
live_rows(VexecEval *ev, const uint64 *active)
{
	return vexec_bits_andnot(ev->work, active ? active : NULL, ev->redo, ev->nrows);
}

/*
 * A kernel's failures among the rows it was asked for, to redo.  A kernel
 * computes every row of the batch (kernel.h); a row it was not asked for --
 * one a qual before it, or a vector child's quals, removed -- is one
 * PostgreSQL never evaluates, and its failure is no error.
 */
static void
keep_failures(VexecEval *ev, const uint64 *failed, const uint64 *asked)
{
	int			w;

	for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
		ev->redo[w] |= failed[w] & asked[w];
}

/* A column a kernel can read: flat or constant, never a dictionary's codes. */
static VexecVec *
kernel_arg(VexecEval *ev, VexecVec *v)
{
	VexecVec   *c;

	if (v->encoding != VEXEC_DICT)
		return v;
	c = vexec_batch_alloc(ev->work, sizeof(VexecVec));
	*c = *v;
	vexec_vec_flatten(ev->work, c);
	return c;
}

/* ---- the steps ---- */

static VexecVec *eval_step(VexecEval *ev, VexecExpr *e, const uint64 *active);

/*
 * VC5: a subtree's results over a dictionary's values, kept for the
 * dictionary's identity while batches come with it.
 */
typedef struct VexecDictMemo
{
	uint64		id;				/* the dictionary's; 0: none kept */
	bool		refused;		/* its result is no column of values: never again */
	VexecBatch *mem;			/* where the kept results live */
	VexecVec   *result;			/* a value for each dictionary value */
	uint64	   *redo;			/* the values whose rows PostgreSQL redoes */
	bool		any_redo;
	uint8	   *bools;			/* a boolean result's: bit 0 its truth, bit 1
								 * its validity, a byte a value; or NULL */
	uint64		result_id;		/* the results' identity, as a dictionary */
	bool		have_null;		/* the result for a NULL computed: */
	VexecVec   *null_result;
	bool		null_redo;
} VexecDictMemo;

/*
 * A subtree over `values`, nvals of them, in place of its input column's
 * rows, its registers in `work`; *redo the values its kernels could not
 * compute.
 */
static VexecVec *
eval_over_values(VexecEval *ev, VexecExpr *e, VexecBatch *work, const VexecVec *values,
				 int nvals, uint64 **redo)
{
	VexecEval	dev = *ev;
	VexecBatch	din = *ev->in;
	VexecVec   *r;

	din.nrows = nvals;
	din.selection = NULL;
	din.cols = vexec_batch_alloc(work, sizeof(VexecVec) * Max(din.ncols, 1));
	memcpy(din.cols, ev->in->cols, sizeof(VexecVec) * din.ncols);
	din.cols[e->dict_col] = *values;
	din.cols[e->dict_col].nvalues = nvals;
	dev.in = &din;
	dev.work = work;
	dev.nrows = nvals;
	dev.redo = vexec_bitmap_alloc(work, nvals, false);
	dev.exact = false;
	dev.over_dict = true;
	r = eval_step(&dev, e, NULL);
	*redo = dev.redo;
	return r;
}

/* Value i of a boolean result, and whether it is valid */
static inline bool
result_bool(const VexecVec *r, int i, bool *valid)
{
	if (r->encoding == VEXEC_CONST)
		i = 0;
	*valid = r->validity == NULL || vexec_bit(r->validity, i);
	if (r->shape.layout == VEXEC_BIT_BOOL)
		return vexec_bit((const uint64 *) r->values, i);
	return ((const uint8 *) r->values)[i] != 0;
}

/* A boolean result over nvals values as a byte a value: bit 0 true, bit 1 valid */
static uint8 *
result_bools(VexecBatch *work, const VexecVec *r, int nvals)
{
	uint8	   *b = vexec_batch_alloc(work, Max(nvals, 1));
	int			i;

	for (i = 0; i < nvals; i++)
	{
		bool		valid;
		bool		t = result_bool(r, i, &valid);

		b[i] = (t ? 1 : 0) | (valid ? 2 : 0);
	}
	return b;
}

/*
 * A subtree over a dictionary column's values, its rows mapped to their
 * values' results; NULL where it is better evaluated over the rows.
 */
static VexecVec *
eval_dict(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *in = &ev->in->cols[e->dict_col];
	VexecVec   *dict = in->dictionary;
	VexecDictMemo *m = e->dict_memo;
	int			n = ev->nrows;
	uint64	   *live;
	VexecVec   *result;
	uint64	   *redo;
	bool		any_redo;
	uint8	   *bools;
	VexecVec   *null_result = NULL;
	bool		null_redo = false;
	bool		null_rows = false;
	int			r;

	if (m != NULL && m->refused)
		return NULL;
	live = live_rows(ev, active);
	if (in->validity != NULL)
	{
		int			w;

		for (w = 0; w < VEXEC_WORDS(n) && !null_rows; w++)
			null_rows = (live[w] & ~in->validity[w]) != 0;
	}

	if (in->dict_id != 0 && m != NULL && m->id == in->dict_id)
	{
		result = m->result;
		redo = m->redo;
		any_redo = m->any_redo;
		bools = m->bools;
	}
	else
	{
		VexecBatch *work;

		/* without an identity, only where its values are fewer than the rows */
		if (in->dict_id == 0 && dict->nvalues > vexec_bits_count(live, n))
			return NULL;
		if (m == NULL)
		{
			VexecType  *none = NULL;

			m = MemoryContextAllocZero(ev->node->mcxt, sizeof(VexecDictMemo));
			m->mem = vexec_batch_create(ev->node->mcxt, 0, &none);
			e->dict_memo = m;
		}
		m->id = 0;
		m->have_null = false;
		if (in->dict_id != 0)
		{
			vexec_batch_reset(m->mem);
			work = m->mem;
		}
		else
			work = ev->work;
		result = eval_over_values(ev, e, work, dict, dict->nvalues, &redo);
		if (result->encoding == VEXEC_DICT)
		{
			m->refused = true;
			return NULL;
		}
		ev->node->stats.dict_values += dict->nvalues;
		any_redo = vexec_bits_any(redo, dict->nvalues);
		bools = result->shape.layout == VEXEC_BIT_BOOL || result->shape.layout == VEXEC_BYTE_BOOL ?
			result_bools(work, result, dict->nvalues) : NULL;
		if (in->dict_id != 0)
		{
			m->id = in->dict_id;
			m->result = result;
			m->redo = redo;
			m->any_redo = any_redo;
			m->bools = bools;
			m->result_id = vexec_dict_new_id();
		}
	}

	/* a NULL row's result: the subtree over a NULL alone */
	if (null_rows)
	{
		if (m->id != 0 && m->have_null)
		{
			null_result = m->null_result;
			null_redo = m->null_redo;
		}
		else
		{
			VexecBatch *work = m->id != 0 ? m->mem : ev->work;
			VexecVec	nv;
			VexecShape	shape;
			uint64	   *nredo;

			nv.type = dict->type;
			vexec_type_build_shape(dict->type, &shape);
			vexec_vec_init(work, &nv, &shape, 1);
			nv.validity = vexec_bitmap_alloc(work, 1, false);
			null_result = eval_over_values(ev, e, work, &nv, 1, &nredo);
			if (null_result->encoding == VEXEC_DICT)
			{
				m->refused = true;
				return NULL;
			}
			null_redo = vexec_bit(nredo, 0);
			if (m->id != 0)
			{
				m->have_null = true;
				m->null_result = null_result;
				m->null_redo = null_redo;
			}
		}
	}

	/* the rows asked for whose value's evaluation PostgreSQL redoes */
	if (any_redo || null_redo)
		for (r = vexec_bits_next(live, n, 0); r >= 0; r = vexec_bits_next(live, n, r + 1))
			if (vexec_vec_isnull(in, r) ? null_redo : vexec_bit(redo, in->codes[r]))
				vexec_bit_set(ev->redo, r);
	ev->node->stats.dict_rows += vexec_bits_count(live, n);

	if (bools != NULL)
	{
		/*
		 * The rows' bits from their values' bytes, a word of rows at a time;
		 * a NULL row's from the result over a NULL, where it was computed
		 */
		uint64	   *truth = vexec_bitmap_alloc(ev->work, n, false);
		uint64	   *valid = vexec_bitmap_alloc(ev->work, n, false);
		uint64		null_truth = 0;
		uint64		null_valid = 0;
		bool		all_valid = true;
		const int32 *codes = in->codes;
		int			w;

		if (null_result != NULL)
		{
			bool		ok;

			null_truth = result_bool(null_result, 0, &ok) ? ~UINT64CONST(0) : 0;
			null_valid = ok ? ~UINT64CONST(0) : 0;
		}
		for (w = 0; w < VEXEC_WORDS(n); w++)
		{
			int			base = w * 64;
			int			end = Min(n - base, 64);
			uint64		tw = 0;
			uint64		vw = 0;
			uint64		mask = end == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << end) - 1;
			int			b;

			for (b = 0; b < end; b++)
			{
				uint8		x = bools[codes[base + b]];

				tw |= (uint64) (x & 1) << b;
				vw |= (uint64) ((x >> 1) & 1) << b;
			}
			if (in->validity != NULL)
			{
				uint64		rv = in->validity[w];

				tw = (tw & rv) | (null_truth & ~rv);
				vw = (vw & rv) | (null_valid & ~rv);
			}
			truth[w] = tw & mask;
			valid[w] = vw & mask;
			if ((vw & mask) != mask)
				all_valid = false;
		}
		return bool_register(ev, truth, all_valid ? NULL : valid);
	}

	/*
	 * Any other result: the rows' codes into the results, as a dictionary
	 * column -- a NULL row's result must be NULL, as a strict call's is -- or
	 * flattened where a result is NULL, so that a dictionary never holds one
	 */
	if (result->encoding != VEXEC_FLAT ||
		(null_result != NULL && !vexec_vec_isnull(null_result, 0)))
		return NULL;
	{
		VexecVec   *v = vexec_batch_alloc0(ev->work, sizeof(VexecVec));
		bool		entry_nulls = result->validity != NULL &&
			vexec_vec_null_count(NULL, result) > 0;

		v->type = e->type;
		v->shape = result->shape;
		v->encoding = VEXEC_DICT;
		v->nvalues = n;
		v->codes = in->codes;
		v->dictionary = result;
		v->dict_id = m->id != 0 && !entry_nulls ? m->result_id : 0;
		if (in->validity != NULL || entry_nulls)
		{
			v->validity = vexec_bitmap_alloc(ev->work, n, false);
			for (r = 0; r < n; r++)
				if (!vexec_vec_isnull(in, r) &&
					(!entry_nulls || vexec_bit(result->validity, in->codes[r])))
					vexec_bit_set(v->validity, r);
		}
		if (entry_nulls)
			vexec_vec_flatten(ev->work, v);
		else if (result->validity != NULL)
		{
			/* a dictionary's values have no validity of their own */
			VexecVec   *d = vexec_batch_alloc(ev->work, sizeof(VexecVec));

			*d = *result;
			d->validity = NULL;
			v->dictionary = d;
		}
		return v;
	}
}

/*
 * A parameter, once a batch.  An InitPlan not yet run is run here only
 * where the rows are exactly those PostgreSQL would reach (above).
 */
static VexecVec *
eval_param(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	Datum		value;
	bool		isnull;

	/*
	 * No row reaches the parameter: PostgreSQL reads it for none, and an
	 * InitPlan behind it must not run.
	 */
	if (!vexec_bits_any(live_rows(ev, active), ev->nrows))
		return const_register(ev, e->type, (Datum) 0, true);
	if (e->paramid >= 0)
	{
		ParamExecData *prm = &ev->econtext->ecxt_param_exec_vals[e->paramid];

		if (prm->execPlan != NULL && (!ev->exact || vexec_bits_any(ev->redo, ev->nrows)))
		{
			vexec_bits_or_into(ev->redo, live_rows(ev, active), ev->nrows);
			return const_register(ev, e->type, (Datum) 0, true);
		}
	}
	value = ExecEvalExprSwitchContext(e->param_state, ev->econtext, &isnull);
	return const_register(ev, e->type, value, isnull);
}

/* The fallback: PostgreSQL's evaluator, a live row at a time. */
static VexecVec *
eval_fallback(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	uint64	   *rows = live_rows(ev, active);
	VexecVec   *v = new_register(ev, e->type, NULL);
	int			r;

	for (r = vexec_bits_next(rows, ev->nrows, 0); r >= 0; r = vexec_bits_next(rows, ev->nrows, r + 1))
	{
		Datum		value;
		bool		isnull;

		vexec_node_load_input(ev->node, r);
		value = ExecEvalExprSwitchContext(e->state, ev->econtext, &isnull);
		register_store(ev, v, r, value, isnull);
		ResetExprContext(ev->econtext);
	}
	vexec_node_count_fallback(ev->node, vexec_bits_count(rows, ev->nrows));
	return v;
}

/* ---- CASE, COALESCE and NULLIF (pg_vector_executor.md §3.22's T5) ---- */

/* A word of a register's validity: a constant's spread over the batch. */
static inline uint64
valid_word(const VexecVec *v, int w)
{
	if (v->validity == NULL)
		return ~UINT64CONST(0);
	if (v->encoding == VEXEC_CONST)
		return vexec_bit(v->validity, 0) ? ~UINT64CONST(0) : 0;
	return v->validity[w];
}

/*
 * Rows of a register into a result in its type's build shape, a Datum a
 * row, whatever the register's layout and encoding.  A Datum by reference
 * is kept as it lies, in the input's or the work batch's memory, which
 * outlive the batch's registers.
 */
static void
copy_rows(VexecEval *ev, VexecVec *out, const VexecVec *src, const uint64 *rows)
{
	int			r;

	for (r = vexec_bits_next(rows, ev->nrows, 0); r >= 0; r = vexec_bits_next(rows, ev->nrows, r + 1))
	{
		bool		isnull;
		Datum		value = vexec_vec_datum(ev->work, src, r, &isnull);

		if (isnull)
			register_set_null(ev, out, r);
		else if (out->shape.layout == VEXEC_DATUM)
		{
			if (out->validity)
				vexec_bit_set(out->validity, r);
			((Datum *) out->values)[r] = value;
		}
		else
			register_store(ev, out, r, value, false);
	}
}

/* The rows of a mask NULL in a result. */
static void
null_rows(VexecEval *ev, VexecVec *out, const uint64 *rows)
{
	int			r;

	for (r = vexec_bits_next(rows, ev->nrows, 0); r >= 0; r = vexec_bits_next(rows, ev->nrows, r + 1))
		register_set_null(ev, out, r);
}

/*
 * CASE: each WHEN's condition over the rows no condition before it decided,
 * each result over the rows its condition took, and the ELSE over the rows
 * left -- the rows PostgreSQL evaluates each for, so that a result that
 * would raise on a row its condition does not take is never computed
 * there.  A row a step sent to redo is decided by PostgreSQL's evaluator,
 * the whole expression again (vexec_resolve_row()).  A simple CASE's
 * argument is computed once, and read by its conditions alone, as
 * PostgreSQL gives it to them (execExpr.c's innermost_caseval); a searched
 * CASE's conditions have none.
 */
static VexecVec *
eval_case(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	uint64	   *undecided = live_rows(ev, active);
	VexecVec   *out = new_register(ev, e->type, NULL);
	VexecVec   *outer_value = ev->case_value;
	VexecVec   *value = NULL;
	int			first = e->case_arg ? 1 : 0;
	int			nwhen = (e->nargs - first - 1) / 2;
	int			i,
				w;

	if (e->case_arg)
		value = vexec_eval(ev, e->args[0], undecided);
	for (i = 0; i < nwhen; i++)
	{
		VexecExpr  *cond = e->args[first + 2 * i];
		VexecExpr  *result = e->args[first + 2 * i + 1];
		uint64	   *take;
		uint64	   *t;
		uint64	   *va;
		VexecVec   *c;

		for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
			undecided[w] &= ~ev->redo[w];
		if (!vexec_bits_any(undecided, ev->nrows))
			break;
		ev->case_value = value;
		c = vexec_eval(ev, cond, undecided);
		ev->case_value = outer_value;
		bool_bits(ev, c, &t, &va);
		take = vexec_bitmap_alloc(ev->work, ev->nrows, false);
		for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
		{
			uint64		u = undecided[w] & ~ev->redo[w];

			take[w] = u & t[w] & (va ? va[w] : ~UINT64CONST(0));
			undecided[w] = u & ~take[w];
		}
		if (vexec_bits_any(take, ev->nrows))
			copy_rows(ev, out, vexec_eval(ev, result, take), take);
	}
	for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
		undecided[w] &= ~ev->redo[w];
	if (vexec_bits_any(undecided, ev->nrows))
		copy_rows(ev, out, vexec_eval(ev, e->args[e->nargs - 1], undecided), undecided);
	vexec_node_count_kernel(ev->node);
	return out;
}

/*
 * COALESCE: each argument over the rows every argument before it left NULL,
 * as PostgreSQL computes them; NULL where all of them are.
 */
static VexecVec *
eval_coalesce(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	uint64	   *undecided = live_rows(ev, active);
	VexecVec   *out = new_register(ev, e->type, NULL);
	int			i,
				w;

	for (i = 0; i < e->nargs && vexec_bits_any(undecided, ev->nrows); i++)
	{
		VexecVec   *a = vexec_eval(ev, e->args[i], undecided);
		uint64	   *take = vexec_bitmap_alloc(ev->work, ev->nrows, false);

		for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
		{
			uint64		u = undecided[w] & ~ev->redo[w];

			take[w] = u & valid_word(a, w);
			undecided[w] = u & ~take[w];
		}
		if (vexec_bits_any(take, ev->nrows))
			copy_rows(ev, out, a, take);
	}
	for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
		undecided[w] &= ~ev->redo[w];
	null_rows(ev, out, undecided);
	vexec_node_count_kernel(ev->node);
	return out;
}

/* NULLIF(a, b): a, NULL where a = b holds. */
static VexecVec *
eval_nullif(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	uint64	   *live = live_rows(ev, active);
	VexecVec   *a = vexec_eval(ev, e->args[0], live);
	VexecVec   *eq = vexec_eval(ev, e->args[1], live);
	VexecVec   *out = new_register(ev, e->type, NULL);
	uint64	   *keep = vexec_bitmap_alloc(ev->work, ev->nrows, false);
	uint64	   *equal = vexec_bitmap_alloc(ev->work, ev->nrows, false);
	uint64	   *t;
	uint64	   *va;
	int			w;

	bool_bits(ev, eq, &t, &va);
	for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
	{
		uint64		l = live[w] & ~ev->redo[w];

		equal[w] = l & t[w] & (va ? va[w] : ~UINT64CONST(0));
		keep[w] = l & ~equal[w];
	}
	copy_rows(ev, out, a, keep);
	null_rows(ev, out, equal);
	vexec_node_count_kernel(ev->node);
	return out;
}

static VexecVec *
eval_and_or(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	bool		is_and = e->kind == VE_AND;
	int			n = ev->nrows;
	uint64	   *undecided = live_rows(ev, active);
	uint64	   *decided = vexec_bitmap_alloc(ev->work, n, false);	/* false for AND,
																	 * true for OR */
	uint64	   *nulls = vexec_bitmap_alloc(ev->work, n, false);
	uint64	   *truth;
	uint64	   *valid = NULL;
	int			i,
				w;

	for (i = 0; i < e->nargs && vexec_bits_any(undecided, n); i++)
	{
		VexecVec   *a = vexec_eval(ev, e->args[i], undecided);
		uint64	   *t;
		uint64	   *va;

		bool_bits(ev, a, &t, &va);
		for (w = 0; w < VEXEC_WORDS(n); w++)
		{
			uint64		u = undecided[w] & ~ev->redo[w];
			uint64		vv = va ? va[w] : ~UINT64CONST(0);
			uint64		hit = u & vv & (is_and ? ~t[w] : t[w]);

			decided[w] |= hit;
			nulls[w] |= u & ~vv;
			undecided[w] = u & ~hit;
		}
	}

	/*
	 * AND: false where decided, else NULL where any operand was NULL, else
	 * true.  OR: true where decided, else NULL, else false.
	 */
	truth = vexec_bitmap_alloc(ev->work, n, false);
	for (w = 0; w < VEXEC_WORDS(n); w++)
	{
		uint64		null_only = nulls[w] & ~decided[w];

		if (null_only)
		{
			if (valid == NULL)
				valid = vexec_bitmap_alloc(ev->work, n, true);
			valid[w] &= ~null_only;
		}
		if (is_and)
			truth[w] = ~decided[w] & ~null_only;
		else
			truth[w] = decided[w];
	}
	return bool_register(ev, truth, valid);
}

static VexecVec *
eval_not(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *a = vexec_eval(ev, e->args[0], active);
	uint64	   *t;
	uint64	   *va;
	uint64	   *truth;
	int			w;

	bool_bits(ev, a, &t, &va);
	truth = vexec_bitmap_alloc(ev->work, ev->nrows, false);
	for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
		truth[w] = ~t[w];
	return bool_register(ev, truth, va ? vexec_bits_copy(ev->work, va, ev->nrows) : NULL);
}

/* IS [NOT] NULL: never NULL itself. */
static VexecVec *
eval_nulltest(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *a = vexec_eval(ev, e->args[0], active);
	uint64	   *truth = vexec_bitmap_alloc(ev->work, ev->nrows, false);
	int			r;

	for (r = 0; r < ev->nrows; r++)
	{
		bool		isnull = vexec_vec_isnull(a, r);

		if (e->testtype == IS_NULL ? isnull : !isnull)
			vexec_bit_set(truth, r);
	}
	return bool_register(ev, truth, NULL);
}

/* IS [NOT] TRUE / FALSE / UNKNOWN: never NULL itself. */
static VexecVec *
eval_booltest(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *a = vexec_eval(ev, e->args[0], active);
	uint64	   *t;
	uint64	   *va;
	uint64	   *truth = vexec_bitmap_alloc(ev->work, ev->nrows, false);
	int			w;

	bool_bits(ev, a, &t, &va);
	for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
	{
		uint64		vv = va ? va[w] : ~UINT64CONST(0);
		uint64		r;

		switch (e->testtype)
		{
			case IS_TRUE:
				r = vv & t[w];
				break;
			case IS_NOT_TRUE:
				r = ~(vv & t[w]);
				break;
			case IS_FALSE:
				r = vv & ~t[w];
				break;
			case IS_NOT_FALSE:
				r = ~(vv & ~t[w]);
				break;
			case IS_UNKNOWN:
				r = ~vv;
				break;
			default:			/* IS_NOT_UNKNOWN */
				r = vv;
				break;
		}
		truth[w] = r;
	}
	return bool_register(ev, truth, NULL);
}

/*
 * The rows a strict call computes, and its result's validity: the live
 * rows whose arguments are all valid.  *none when a constant argument is
 * NULL.
 */
static uint64 *
strict_rows(VexecEval *ev, VexecVec **args, int nargs, const uint64 *live, uint64 **validity)
{
	uint64	   *valid = NULL;
	int			i;

	for (i = 0; i < nargs; i++)
	{
		VexecVec   *a = args[i];

		if (a->validity == NULL)
			continue;
		if (a->encoding == VEXEC_CONST)
		{
			if (!vexec_bit(a->validity, 0))
			{
				valid = vexec_bitmap_alloc(ev->work, ev->nrows, false);
				break;
			}
			continue;
		}
		valid = valid == NULL ? vexec_bits_copy(ev->work, a->validity, ev->nrows)
			: vexec_bits_and(ev->work, valid, a->validity, ev->nrows);
	}
	*validity = valid;
	return vexec_bits_and(ev->work, live, valid, ev->nrows);
}

/* A declared call's kernel: no variant, so call_rows() runs it. */
static bool
declared_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	(void) kc;
	(void) args;
	(void) v;
	return false;
}

const VexecKernelDef vexec_declared_kernel = {"declared", false, NULL, declared_variant};

/*
 * A call bound to a kernel pack's declaration (vexec_kernels.h, packs.c):
 * the extension's own function through fmgr, a row at a time over the rows
 * it computes, with one prepared call frame, where its declaration lets it
 * run ahead of PostgreSQL's order:
 *
 *	never raises	the function, on every row;
 *	check			the pack's check, then the function where the check
 *					passes; the call's varlena arguments detoasted once a
 *					row first -- a constant once a batch -- and handed to
 *					both, so that neither reads a toasted value again;
 *	prefilter		the pack's prefilter, under an ErrorSaveContext: its
 *					answer where it gives one; the function is never called.
 *
 * A row the check fails or the prefilter leaves undecided goes to redo:
 * PostgreSQL's evaluator calls the function there, in row order, when the
 * row is asked for, and raises its error, if any, where PostgreSQL would.
 * The pack's function has a call frame of its own, its flinfo fn_extra NULL
 * at every row: it keeps nothing from row to row.  Each row's memory -- the
 * detoasted values, what the functions allocate -- is reset after it.
 */
static VexecVec *
call_declared(VexecEval *ev, VexecExpr *e, VexecVec **args, const uint64 *compute,
			  uint64 *validity)
{
	const VexecKernelDecl *decl = e->decl;
	MemoryContext rowcxt = ev->econtext->ecxt_per_tuple_memory;
	VexecVec   *v = new_register(ev, e->type, NULL);
	LOCAL_FCINFO(fcinfo, VEXEC_KERNEL_MAXARGS);
	LOCAL_FCINFO(pfcinfo, VEXEC_KERNEL_MAXARGS);
	FmgrInfo	pflinfo;
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	bool		detoast[VEXEC_KERNEL_MAXARGS];
	NullableDatum constargs[VEXEC_KERNEL_MAXARGS];
	int			computed = 0;
	int			r,
				i;

	if (validity)
		v->validity = vexec_bits_copy(ev->work, validity, ev->nrows);
	InitFunctionCallInfoData(*fcinfo, e->finfo, e->nargs, e->collation, NULL, NULL);
	/* the pack's function's frame: no catalog entry, no state */
	MemSet(&pflinfo, 0, sizeof(pflinfo));
	pflinfo.fn_addr = decl->fn;
	pflinfo.fn_oid = InvalidOid;
	pflinfo.fn_nargs = e->nargs;
	pflinfo.fn_mcxt = rowcxt;
	InitFunctionCallInfoData(*pfcinfo, &pflinfo, e->nargs, e->collation,
							 decl->kind == VEXEC_DECL_PREFILTER ? (Node *) &escontext : NULL,
							 NULL);

	/* a check's varlena arguments are detoasted; a constant's, once */
	for (i = 0; i < e->nargs; i++)
	{
		detoast[i] = decl->kind == VEXEC_DECL_CHECK && args[i]->type->typlen == -1;
		if (args[i]->encoding != VEXEC_CONST)
			continue;
		constargs[i].value = vexec_vec_datum(ev->work, args[i], 0, &constargs[i].isnull);
		if (detoast[i] && !constargs[i].isnull)
		{
			MemoryContext old = MemoryContextSwitchTo(ev->work->mcxt);

			constargs[i].value = PointerGetDatum(PG_DETOAST_DATUM(constargs[i].value));
			MemoryContextSwitchTo(old);
		}
	}

	for (r = vexec_bits_next(compute, ev->nrows, 0); r >= 0;
		 r = vexec_bits_next(compute, ev->nrows, r + 1))
	{
		MemoryContext old = MemoryContextSwitchTo(rowcxt);
		Datum		result = (Datum) 0;
		bool		isnull = true;
		bool		decided = true;

		for (i = 0; i < e->nargs; i++)
		{
			if (args[i]->encoding == VEXEC_CONST)
				fcinfo->args[i] = constargs[i];
			else
			{
				fcinfo->args[i].value = vexec_vec_datum(ev->work, args[i], r,
														&fcinfo->args[i].isnull);
				if (detoast[i] && !fcinfo->args[i].isnull)
					fcinfo->args[i].value = PointerGetDatum(PG_DETOAST_DATUM(fcinfo->args[i].value));
			}
		}
		switch (decl->kind)
		{
			case VEXEC_DECL_NEVER_RAISES:
				fcinfo->isnull = false;
				result = FunctionCallInvoke(fcinfo);
				isnull = fcinfo->isnull;
				break;
			case VEXEC_DECL_CHECK:
				{
					Datum		ok;

					for (i = 0; i < e->nargs; i++)
						pfcinfo->args[i] = fcinfo->args[i];
					pflinfo.fn_extra = NULL;
					pfcinfo->isnull = false;
					ok = FunctionCallInvoke(pfcinfo);
					decided = !pfcinfo->isnull && DatumGetBool(ok);
					if (decided)
					{
						fcinfo->isnull = false;
						result = FunctionCallInvoke(fcinfo);
						isnull = fcinfo->isnull;
					}
					break;
				}
			case VEXEC_DECL_PREFILTER:
				for (i = 0; i < e->nargs; i++)
					pfcinfo->args[i] = fcinfo->args[i];
				pflinfo.fn_extra = NULL;
				pfcinfo->isnull = false;
				escontext.error_occurred = false;
				result = FunctionCallInvoke(pfcinfo);
				decided = !SOFT_ERROR_OCCURRED(&escontext);
				isnull = pfcinfo->isnull;
				break;
			default:
				elog(ERROR, "vexec: unknown declaration %d of pack \"%s\"",
					 decl->kind, e->pack->name);
		}
		MemoryContextSwitchTo(old);
		if (decided)
		{
			register_store(ev, v, r, result, isnull);
			computed++;
		}
		else
		{
			vexec_bit_set(ev->redo, r);
			register_store(ev, v, r, (Datum) 0, true);
		}
		ResetExprContext(ev->econtext);
	}
	vexec_node_count_declared(ev->node, computed);
	return v;
}

/*
 * The row-by-row variant: the function through fmgr, for one that cannot
 * raise; for one that can, its rows go to PostgreSQL's evaluator.  A call
 * bound to a kernel pack's declaration has its own (call_declared()).
 */
static VexecVec *
call_rows(VexecEval *ev, VexecExpr *e, VexecVec **args, const uint64 *compute,
		  uint64 *validity)
{
	VexecVec   *v;
	LOCAL_FCINFO(fcinfo, VEXEC_KERNEL_MAXARGS);
	int			r;

	if (e->decl != NULL)
		return call_declared(ev, e, args, compute, validity);
	if (e->kernel->can_fail)
	{
		vexec_bits_or_into(ev->redo, compute, ev->nrows);
		v = new_register(ev, e->type, NULL);
		v->validity = vexec_bitmap_alloc(ev->work, ev->nrows, false);
		return v;
	}
	v = new_register(ev, e->type, NULL);
	if (validity)
		v->validity = vexec_bits_copy(ev->work, validity, ev->nrows);
	InitFunctionCallInfoData(*fcinfo, e->finfo, e->nargs, e->collation, NULL, NULL);
	for (r = vexec_bits_next(compute, ev->nrows, 0); r >= 0;
		 r = vexec_bits_next(compute, ev->nrows, r + 1))
	{
		Datum		result;
		int			i;
		MemoryContext old;

		for (i = 0; i < e->nargs; i++)
		{
			fcinfo->args[i].value = vexec_vec_datum(ev->work, args[i], vexec_arg_row(args[i], r),
													&fcinfo->args[i].isnull);
		}
		old = MemoryContextSwitchTo(ev->econtext->ecxt_per_tuple_memory);
		fcinfo->isnull = false;
		result = FunctionCallInvoke(fcinfo);
		MemoryContextSwitchTo(old);
		register_store(ev, v, r, result, fcinfo->isnull);
		ResetExprContext(ev->econtext);
	}
	return v;
}

/* A call with a kernel. */
static VexecVec *
eval_call(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *args[VEXEC_KERNEL_MAXARGS];
	uint64	   *live = live_rows(ev, active);
	uint64	   *validity = NULL;
	uint64	   *compute;
	VexecKernelCall kc;
	VexecVariant variant;
	VexecVec   *result;
	int			i;

	for (i = 0; i < e->nargs; i++)
		args[i] = kernel_arg(ev, vexec_eval(ev, e->args[i], live));
	/* the arguments' evaluation may have sent rows to redo */
	live = live_rows(ev, live);

	if (e->strict)
		compute = strict_rows(ev, args, e->nargs, live, &validity);
	else
		compute = live;

	memset(&kc, 0, sizeof(kc));
	kc.work = ev->work;
	kc.nrows = ev->nrows;
	kc.active = compute;
	kc.nargs = e->nargs;
	kc.call = e;
	for (i = 0; i < e->nargs; i++)
		kc.args[i] = args[i];
	memset(&variant, 0, sizeof(variant));
	if (!e->kernel->variant(&kc, args, &variant))
		return call_rows(ev, e, args, compute, validity);
	for (i = 0; i < e->nargs; i++)
		kc.args[i] = args[i];

	result = vexec_batch_alloc0(ev->work, sizeof(VexecVec));
	result->type = e->type;
	vexec_vec_init(ev->work, result, &variant.result, ev->nrows);
	if (e->strict)
		result->validity = validity ? vexec_bits_copy(ev->work, validity, ev->nrows) : NULL;
	kc.result = result;
	kc.failed = vexec_bitmap_alloc(ev->work, ev->nrows, false);
	if (vexec_bits_any(compute, ev->nrows))
		variant.fn(&kc);
	if (e->kernel->can_fail)
		keep_failures(ev, kc.failed, compute);
	vexec_node_count_kernel(ev->node);
	return result;
}

/* IS DISTINCT FROM, through the equality's kernel. */
static VexecVec *
eval_distinct(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *args[VEXEC_KERNEL_MAXARGS];
	uint64	   *live = live_rows(ev, active);
	uint64	   *both;
	uint64	   *validity = NULL;
	uint64	   *truth;
	VexecKernelCall kc;
	VexecVariant variant;
	VexecVec   *eq;
	int			r;

	for (r = 0; r < 2; r++)
		args[r] = kernel_arg(ev, vexec_eval(ev, e->args[r], live));
	live = live_rows(ev, live);
	both = strict_rows(ev, args, 2, live, &validity);

	memset(&kc, 0, sizeof(kc));
	kc.work = ev->work;
	kc.nrows = ev->nrows;
	kc.active = both;
	kc.nargs = 2;
	kc.call = e;
	kc.args[0] = args[0];
	kc.args[1] = args[1];
	if (e->kernel->variant(&kc, args, &variant))
	{
		kc.args[0] = args[0];
		kc.args[1] = args[1];
		eq = vexec_batch_alloc0(ev->work, sizeof(VexecVec));
		eq->type = e->type;
		vexec_vec_init(ev->work, eq, &variant.result, ev->nrows);
		kc.result = eq;
		kc.failed = vexec_bitmap_alloc(ev->work, ev->nrows, false);
		if (vexec_bits_any(both, ev->nrows))
			variant.fn(&kc);
		if (e->kernel->can_fail)
			keep_failures(ev, kc.failed, both);
	}
	else
		eq = call_rows(ev, e, args, both, NULL);

	/* distinct: exactly one NULL, or both valid and not equal */
	truth = vexec_bitmap_alloc(ev->work, ev->nrows, false);
	for (r = 0; r < ev->nrows; r++)
	{
		bool		n0 = vexec_vec_isnull(args[0], r);
		bool		n1 = vexec_vec_isnull(args[1], r);
		bool		d;

		if (n0 || n1)
			d = n0 != n1;
		else if (eq->shape.layout == VEXEC_BIT_BOOL)
			d = !vexec_bit((uint64 *) eq->values, r);
		else
			d = ((uint8 *) eq->values)[r] == 0;
		if (d)
			vexec_bit_set(truth, r);
	}
	return bool_register(ev, truth, NULL);
}

/*
 * x op ANY / ALL (array): the comparison's kernel once per element, its
 * results combined as ExecEvalScalarArrayOp combines them
 * (PG19:src/backend/executor/execExprInterp.c): an empty array gives false
 * for ANY and true for ALL whatever x is; a NULL x gives NULL; otherwise
 * ANY is true where any element's comparison is true, else NULL where any
 * was NULL, else false, and ALL the reverse.
 */
static VexecVec *
eval_saop(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecSaop  *s = (VexecSaop *) e->saop;
	uint64	   *live = live_rows(ev, active);
	VexecVec   *x;
	uint64	   *xvalid = NULL;
	uint64	   *compute;
	uint64	   *decided;
	uint64	   *nulls;
	uint64	   *truth;
	uint64	   *valid = NULL;
	int			n = ev->nrows;
	int			i,
				w;

	if (s->array_null)
		return bool_register(ev, vexec_bitmap_alloc(ev->work, n, false),
							 vexec_bitmap_alloc(ev->work, n, false));
	if (s->nelems == 0)
		return bool_register(ev, vexec_bitmap_alloc(ev->work, n, !e->useOr), NULL);

	x = kernel_arg(ev, vexec_eval(ev, e->args[0], live));
	live = live_rows(ev, live);
	compute = strict_rows(ev, &x, 1, live, &xvalid);
	decided = vexec_bitmap_alloc(ev->work, n, false);
	nulls = vexec_bitmap_alloc(ev->work, n, false);

	for (i = 0; i < s->nelems && vexec_bits_any(compute, n); i++)
	{
		VexecVec   *args[2];
		VexecKernelCall kc;
		VexecVariant variant;
		VexecVec   *r;

		if (s->elems[i] == NULL)
		{
			/* a NULL element: NULL for every row still undecided */
			vexec_bits_or_into(nulls, compute, n);
			continue;
		}
		args[0] = x;
		args[1] = s->elems[i];
		memset(&kc, 0, sizeof(kc));
		kc.work = ev->work;
		kc.nrows = n;
		kc.active = compute;
		kc.nargs = 2;
		kc.call = e;
		kc.args[0] = args[0];
		kc.args[1] = args[1];
		if (e->kernel->variant(&kc, args, &variant))
		{
			kc.args[0] = args[0];
			kc.args[1] = args[1];
			r = vexec_batch_alloc0(ev->work, sizeof(VexecVec));
			r->type = e->type;
			vexec_vec_init(ev->work, r, &variant.result, n);
			kc.result = r;
			kc.failed = vexec_bitmap_alloc(ev->work, n, false);
			variant.fn(&kc);
			if (e->kernel->can_fail)
				keep_failures(ev, kc.failed, compute);
		}
		else
			r = call_rows(ev, e, args, compute, NULL);
		{
			uint64	   *t;
			uint64	   *va;

			bool_bits(ev, r, &t, &va);
			for (w = 0; w < VEXEC_WORDS(n); w++)
			{
				uint64		vv = va ? va[w] : ~UINT64CONST(0);
				uint64		hit = compute[w] & vv & (e->useOr ? t[w] : ~t[w]);

				decided[w] |= hit;
				nulls[w] |= compute[w] & ~vv;
				compute[w] &= ~hit & ~ev->redo[w];
			}
		}
	}

	truth = vexec_bitmap_alloc(ev->work, n, false);
	for (w = 0; w < VEXEC_WORDS(n); w++)
	{
		uint64		xnull = xvalid ? ~xvalid[w] : 0;
		uint64		null_only = (nulls[w] & ~decided[w]) | xnull;

		if (null_only)
		{
			if (valid == NULL)
				valid = vexec_bitmap_alloc(ev->work, n, true);
			valid[w] &= ~null_only;
		}
		truth[w] = e->useOr ? decided[w] & ~xnull : ~decided[w] & ~null_only;
	}
	return bool_register(ev, truth, valid);
}

/*
 * Evaluate a compiled expression over the active rows (NULL: every row of
 * the batch).  Its column is valid at the active rows not in redo.
 */
static VexecVec *eval_step(VexecEval *ev, VexecExpr *e, const uint64 *active);

/*
 * X10 (§3.18): a shared subtree's result, kept for the batch: again where
 * it is asked over the rows it was computed over, else computed again.
 */
static VexecVec *
eval_shared(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *v;

	if (e->memo != NULL && e->memo_work == ev->work &&
		e->memo_generation == ev->work->generation &&
		(e->memo_active == active ||
		 (active != NULL && e->memo_active != NULL &&
		  memcmp(e->memo_active, active, sizeof(uint64) * VEXEC_WORDS(ev->nrows)) == 0)))
		return e->memo;
	v = eval_step(ev, e, active);
	e->memo = v;
	e->memo_work = ev->work;
	e->memo_generation = ev->work->generation;
	e->memo_active = active == NULL ? NULL : vexec_bits_copy(ev->work, active, ev->nrows);
	return v;
}

VexecVec *
vexec_eval(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	if (e->shared && !ev->over_dict)
		return eval_shared(ev, e, active);
	return eval_step(ev, e, active);
}

static VexecVec *
eval_step(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	check_stack_depth();

	/* VC5: over a dictionary's values, where the subtree reads its column alone */
	if (e->dict_pure && e->dict_col >= 0 && e->kind != VE_VAR && e->kind != VE_CONST &&
		e->kind != VE_RELABEL && ev->in != NULL &&
		ev->in->cols[e->dict_col].encoding == VEXEC_DICT)
	{
		VexecVec   *v = eval_dict(ev, e, active);

		if (v != NULL)
			return v;
	}

	switch (e->kind)
	{
		case VE_VAR:
			return &ev->in->cols[e->col];
		case VE_CONST:
			return e->constvec;
		case VE_PARAM:
			return eval_param(ev, e, active);
		case VE_RELABEL:
			{
				VexecVec   *a = vexec_eval(ev, e->args[0], active);
				VexecVec   *v = vexec_batch_alloc(ev->work, sizeof(VexecVec));

				*v = *a;
				v->type = e->type;
				return v;
			}
		case VE_AND:
		case VE_OR:
			return eval_and_or(ev, e, active);
		case VE_NOT:
			return eval_not(ev, e, active);
		case VE_NULLTEST:
			return eval_nulltest(ev, e, active);
		case VE_BOOLTEST:
			return eval_booltest(ev, e, active);
		case VE_CALL:
			return eval_call(ev, e, active);
		case VE_DISTINCT:
			return eval_distinct(ev, e, active);
		case VE_SAOP:
			return eval_saop(ev, e, active);
		case VE_CASE:
			return eval_case(ev, e, active);
		case VE_CASETEST:
			if (ev->case_value == NULL)
				elog(ERROR, "vexec: a CASE's argument read outside its conditions");
			return ev->case_value;
		case VE_COALESCE:
			return eval_coalesce(ev, e, active);
		case VE_NULLIF:
			return eval_nullif(ev, e, active);
		case VE_FALLBACK:
			return eval_fallback(ev, e, active);
	}
	elog(ERROR, "vexec: unknown step %d", e->kind);
	return NULL;				/* keep the compiler quiet */
}

/*
 * A qual over the active rows: the rows where it is true, less those sent
 * to redo.  NULL and false both reject, as ExecQual's do.
 */
uint64 *
vexec_eval_qual(VexecEval *ev, VexecExpr *e, const uint64 *active)
{
	VexecVec   *v;
	uint64	   *t;
	uint64	   *va;
	uint64	   *pass;
	int			w;

	/* no row reaches the qual: nothing of it is evaluated */
	pass = live_rows(ev, active);
	if (!vexec_bits_any(pass, ev->nrows))
		return pass;
	v = vexec_eval(ev, e, active);
	bool_bits(ev, v, &t, &va);
	for (w = 0; w < VEXEC_WORDS(ev->nrows); w++)
		pass[w] &= t[w] & (va ? va[w] : ~UINT64CONST(0)) & ~ev->redo[w];
	return pass;
}
