/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * filter.c
 *	  Pruning: whether a unit -- a row group -- may hold a row a scan's
 *	  quals and keys keep, by its statistics.  A unit no row of which can
 *	  meet them is passed over unread; every row of a unit read is still
 *	  judged by the scan's own quals, so pruning only saves reading.
 *
 * What prunes: a column against a constant by an operator of its type's
 * default btree family (<, <=, =, >=, >, cross-type ones of the family
 * among them), = ANY of an array constant, IS NULL, IS NOT NULL, a boolean
 * column itself, a PostGIS geometry column && a constant geometry (PQ18,
 * pg_vector_executor.md §3.39: by a unit's bounds, geo.c), and AND and OR
 * of them; vexec's keys (the spec's, set_keys()'s, set_join_keys()'s), each
 * a btree scan key.  Anything else prunes nothing.
 *
 * Kept least and greatest values are compared by the btree family's own
 * comparison, under the operator's collation; a string's and a binary's
 * are kept bytewise, and so prune a text column's equality under any
 * deterministic collation, its ranges under "C" alone, and a bytea's and a
 * uuid's always (both compare bytewise).  A float's statistics leave its
 * NaNs out, which PostgreSQL orders above every number: where a unit may
 * hold one, > and >= prune nothing, nor a NaN constant.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/nbtree.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"
#include "varatt.h"

#include "vexec_parquet.h"

#define ATOM_ISNULL		100
#define ATOM_NOTNULL	101
#define ATOM_GEO_OVERLAPS	102	/* PQ18: a geometry column && a constant: box its bounds */

typedef struct Atom
{
	AttrNumber	attnum;
	int			strategy;		/* BT*StrategyNumber or ATOM_* */
	bool		in_list;		/* = ANY: values[] */
	int			nvalues;
	Datum	   *values;
	bool		isnull;			/* a NULL constant: no row meets it */
	FmgrInfo	cmp;			/* the family's comparison of (column, constant) */
	Oid			collation;
	bool		bytes_range;	/* bytewise statistics answer its ranges */
	bool		bytes_equal;	/* and its equality */
	bool		is_float;
	bool		uuid;			/* a uuid's constant: its 16 bytes, no header */
	Oid			right_type;		/* the constant's type */
	double		box[4];			/* ATOM_GEO_OVERLAPS: xmin, xmax, ymin, ymax */
} Atom;

typedef enum NodeKind
{
	NODE_ATOM,
	NODE_AND,
	NODE_OR
} NodeKind;

typedef struct PNode
{
	NodeKind	kind;
	List	   *args;			/* PNode * */
	Atom	   *atom;
} PNode;

#define NKEYSETS 3

struct VpqPruner
{
	VpqTable   *t;
	MemoryContext cxt;
	PNode	   *quals;			/* AND of the quals' prunable parts, or NULL */
	List	   *keysets[NKEYSETS];	/* Atom *: the spec's keys, set_keys()'s, set_join_keys()'s */
};

static Oid
column_family(VpqColumn *col)
{
	Oid			opclass = GetDefaultOpClass(col->basetype, BTREE_AM_OID);

	return OidIsValid(opclass) ? get_opclass_family(opclass) : InvalidOid;
}

static bool
collation_is_c(Oid collation)
{
	if (!OidIsValid(collation))
		return true;
	if (collation == C_COLLATION_OID)
		return true;
	return pg_newlocale_from_collation(collation)->collate_is_c;
}

static bool
collation_deterministic(Oid collation)
{
	if (!OidIsValid(collation))
		return true;
	return pg_newlocale_from_collation(collation)->deterministic;
}

/* An atom of a column against values of right_type by a strategy, or NULL where none prunes */
static Atom *
make_atom(VpqPruner *p, AttrNumber attnum, int strategy, Oid right_type, Oid collation)
{
	VpqTable   *t = p->t;
	VpqColumn  *col;
	Oid			family;
	Oid			proc;
	Atom	   *a;

	if (attnum < 1 || attnum > t->natts)
		return NULL;
	col = &t->cols[attnum - 1];
	if ((col->kind == VPQ_K_DROPPED || col->kind == VPQ_K_BPCHAR || col->kind == VPQ_K_JSON ||
		 col->kind == VPQ_K_FIXED || col->kind == VPQ_K_VARLENA) && strategy < ATOM_ISNULL)
		return NULL;
	if (col->kind == VPQ_K_DROPPED)
		return NULL;
	a = MemoryContextAllocZero(p->cxt, sizeof(Atom));
	a->attnum = attnum;
	a->strategy = strategy;
	a->collation = collation;
	a->is_float = col->kind == VPQ_K_FLOAT4 || col->kind == VPQ_K_FLOAT8;
	a->uuid = col->kind == VPQ_K_UUID;
	a->right_type = OidIsValid(right_type) ? right_type : col->basetype;
	if (strategy >= ATOM_ISNULL)
		return a;
	if (col->kind == VPQ_K_TEXT)
	{
		a->bytes_equal = collation_deterministic(collation);
		a->bytes_range = collation_is_c(collation);
		if (!a->bytes_equal)
			return NULL;
	}
	else if (col->kind == VPQ_K_BYTEA || col->kind == VPQ_K_UUID)
	{
		a->bytes_equal = true;
		a->bytes_range = true;
	}
	family = column_family(col);
	if (!OidIsValid(family))
		return NULL;
	if (!OidIsValid(right_type))
		right_type = col->basetype;
	proc = get_opfamily_proc(family, col->basetype, right_type, BTORDER_PROC);
	if (!OidIsValid(proc))
		return NULL;
	fmgr_info_cxt(proc, &a->cmp, p->cxt);
	return a;
}

static PNode *
atom_node(VpqPruner *p, Atom *a)
{
	PNode	   *n;

	if (a == NULL)
		return NULL;
	n = MemoryContextAllocZero(p->cxt, sizeof(PNode));
	n->kind = NODE_ATOM;
	n->atom = a;
	return n;
}

/* The attribute a Var of the scanned relation names, or 0 */
static AttrNumber
var_attnum(Node *node)
{
	while (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (node && IsA(node, Var) && ((Var *) node)->varattno > 0 && ((Var *) node)->varlevelsup == 0)
		return ((Var *) node)->varattno;
	return 0;
}

static Const *
as_const(Node *node)
{
	while (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node && IsA(node, Const) ? (Const *) node : NULL;
}

static PNode *compile(VpqPruner *p, Node *node);

/*
 * PQ18: a geometry column && a constant geometry, by the constant's bounds;
 * NULL where the operator is another, or the constant empty
 */
static PNode *
compile_geo(VpqPruner *p, AttrNumber attnum, Oid opno, Const *c)
{
	Atom	   *a;

	if (!vpq_geo_overlaps_func(get_opcode(opno)) || vpq_geo_type(c->consttype) != VPQ_GEO_GEOMETRY)
		return NULL;
	a = MemoryContextAllocZero(p->cxt, sizeof(Atom));
	a->attnum = attnum;
	a->strategy = ATOM_GEO_OVERLAPS;
	a->isnull = c->constisnull;
	if (!a->isnull && !vpq_geo_const_box(c->constvalue, c->consttype, a->box))
		return NULL;
	return atom_node(p, a);
}

static PNode *
compile_opexpr(VpqPruner *p, OpExpr *op)
{
	Node	   *l;
	Node	   *r;
	AttrNumber	attnum;
	Const	   *c;
	Oid			opno = op->opno;
	VpqColumn  *col;
	Oid			family;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;
	Atom	   *a;

	if (list_length(op->args) != 2)
		return NULL;
	l = linitial(op->args);
	r = lsecond(op->args);
	attnum = var_attnum(l);
	c = as_const(r);
	if (attnum == 0 || c == NULL)
	{
		attnum = var_attnum(r);
		c = as_const(l);
		if (attnum == 0 || c == NULL)
			return NULL;
		opno = get_commutator(opno);
		if (!OidIsValid(opno))
			return NULL;
	}
	if (attnum > p->t->natts)
		return NULL;
	col = &p->t->cols[attnum - 1];
	if (col->kind == VPQ_K_DROPPED)
		return NULL;
	if (col->geo == VPQ_GEO_GEOMETRY)
		return compile_geo(p, attnum, opno, c);
	family = column_family(col);
	if (!OidIsValid(family) || !op_in_opfamily(opno, family))
		return NULL;
	get_op_opfamily_properties(opno, family, false, &strategy, &lefttype, &righttype);
	if (lefttype != col->basetype || strategy == 0)
		return NULL;
	a = make_atom(p, attnum, strategy, righttype, op->inputcollid);
	if (a == NULL)
		return NULL;
	a->nvalues = 1;
	a->values = MemoryContextAlloc(p->cxt, sizeof(Datum));
	a->isnull = c->constisnull;
	a->values[0] = c->constisnull ? (Datum) 0 : c->constvalue;
	return atom_node(p, a);
}

static PNode *
compile_saop(VpqPruner *p, ScalarArrayOpExpr *op)
{
	AttrNumber	attnum;
	Const	   *c;
	VpqColumn  *col;
	Oid			family;
	int			strategy;
	Oid			lefttype;
	Oid			righttype;
	Atom	   *a;
	ArrayType  *arr;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			n;

	if (!op->useOr || list_length(op->args) != 2)
		return NULL;
	attnum = var_attnum(linitial(op->args));
	c = as_const(lsecond(op->args));
	if (attnum == 0 || c == NULL || attnum > p->t->natts)
		return NULL;
	col = &p->t->cols[attnum - 1];
	family = column_family(col);
	if (!OidIsValid(family) || !op_in_opfamily(op->opno, family))
		return NULL;
	get_op_opfamily_properties(op->opno, family, false, &strategy, &lefttype, &righttype);
	if (strategy != BTEqualStrategyNumber || lefttype != col->basetype)
		return NULL;
	a = make_atom(p, attnum, strategy, righttype, op->inputcollid);
	if (a == NULL)
		return NULL;
	if (c->constisnull)
	{
		a->isnull = true;
		return atom_node(p, a);
	}
	arr = DatumGetArrayTypeP(c->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, ARR_ELEMTYPE(arr), elmlen, elmbyval, elmalign, &elems, &nulls, &n);
	a->in_list = true;
	a->values = MemoryContextAlloc(p->cxt, sizeof(Datum) * Max(n, 1));
	for (int i = 0; i < n; i++)
		if (!nulls[i])
			a->values[a->nvalues++] = elems[i];
	return atom_node(p, a);
}

static PNode *
compile(VpqPruner *p, Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, OpExpr))
		return compile_opexpr(p, (OpExpr *) node);
	if (IsA(node, ScalarArrayOpExpr))
		return compile_saop(p, (ScalarArrayOpExpr *) node);
	if (IsA(node, NullTest))
	{
		NullTest   *nt = (NullTest *) node;
		AttrNumber	attnum = var_attnum((Node *) nt->arg);

		if (attnum == 0 || nt->argisrow)
			return NULL;
		return atom_node(p, make_atom(p, attnum, nt->nulltesttype == IS_NULL ? ATOM_ISNULL : ATOM_NOTNULL,
									  InvalidOid, InvalidOid));
	}
	if (IsA(node, Var) && exprType(node) == BOOLOID)
	{
		AttrNumber	attnum = var_attnum(node);
		Atom	   *a;

		if (attnum == 0)
			return NULL;
		a = make_atom(p, attnum, BTEqualStrategyNumber, BOOLOID, InvalidOid);
		if (a == NULL)
			return NULL;
		a->nvalues = 1;
		a->values = MemoryContextAlloc(p->cxt, sizeof(Datum));
		a->values[0] = BoolGetDatum(true);
		return atom_node(p, a);
	}
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		PNode	   *n;
		ListCell   *lc;

		if (b->boolop == NOT_EXPR)
		{
			Node	   *arg = linitial(b->args);
			AttrNumber	attnum = var_attnum(arg);
			Atom	   *a;

			if (attnum == 0 || exprType(arg) != BOOLOID)
				return NULL;
			a = make_atom(p, attnum, BTEqualStrategyNumber, BOOLOID, InvalidOid);
			if (a == NULL)
				return NULL;
			a->nvalues = 1;
			a->values = MemoryContextAlloc(p->cxt, sizeof(Datum));
			a->values[0] = BoolGetDatum(false);
			return atom_node(p, a);
		}
		n = MemoryContextAllocZero(p->cxt, sizeof(PNode));
		n->kind = b->boolop == AND_EXPR ? NODE_AND : NODE_OR;
		foreach(lc, b->args)
		{
			PNode	   *arg = compile(p, lfirst(lc));

			if (arg == NULL)
			{
				/* an OR with an arm that prunes nothing prunes nothing */
				if (n->kind == NODE_OR)
					return NULL;
				continue;
			}
			n->args = lappend(n->args, arg);
		}
		if (n->args == NIL)
			return NULL;
		return n;
	}
	return NULL;
}

/* A btree scan key as an atom: =, ranges, = ANY, IS [NOT] NULL */
static Atom *
key_atom(VpqPruner *p, const ScanKeyData *k)
{
	Atom	   *a;

	if (k->sk_flags & SK_SEARCHNULL)
		return make_atom(p, k->sk_attno, ATOM_ISNULL, InvalidOid, InvalidOid);
	if (k->sk_flags & SK_SEARCHNOTNULL)
		return make_atom(p, k->sk_attno, ATOM_NOTNULL, InvalidOid, InvalidOid);
	if (k->sk_strategy < BTLessStrategyNumber || k->sk_strategy > BTGreaterStrategyNumber)
		return NULL;
	a = make_atom(p, k->sk_attno, k->sk_strategy, k->sk_subtype, k->sk_collation);
	if (a == NULL)
		return NULL;
	if (k->sk_flags & SK_ISNULL)
	{
		a->isnull = true;
		return a;
	}
	if (k->sk_flags & SK_SEARCHARRAY)
	{
		ArrayType  *arr = DatumGetArrayTypeP(k->sk_argument);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *elems;
		bool	   *nulls;
		int			n;

		if (k->sk_strategy != BTEqualStrategyNumber)
			return NULL;
		get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arr, ARR_ELEMTYPE(arr), elmlen, elmbyval, elmalign, &elems, &nulls, &n);
		a->in_list = true;
		a->values = MemoryContextAlloc(p->cxt, sizeof(Datum) * Max(n, 1));
		for (int i = 0; i < n; i++)
			if (!nulls[i])
			{
				a->values[a->nvalues++] = elmbyval ? elems[i] :
					datumCopy(elems[i], false, elmlen);
			}
		return a;
	}
	a->nvalues = 1;
	a->values = MemoryContextAlloc(p->cxt, sizeof(Datum));
	{
		VpqColumn  *col = &p->t->cols[k->sk_attno - 1];
		Oid			rtype = OidIsValid(k->sk_subtype) ? k->sk_subtype : col->basetype;
		int16		len;
		bool		byval;

		get_typlenbyval(rtype, &len, &byval);
		a->values[0] = datumCopy(k->sk_argument, byval, len);
	}
	return a;
}

VpqPruner *
vpq_pruner_make(VpqTable *t, List *quals, int nkeys, const ScanKeyData *keys)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "vexec_parquet pruner",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	VpqPruner  *p = palloc0(sizeof(VpqPruner));
	ListCell   *lc;
	PNode	   *and;

	p->t = t;
	p->cxt = cxt;
	and = palloc0(sizeof(PNode));
	and->kind = NODE_AND;
	foreach(lc, quals)
	{
		PNode	   *n = compile(p, lfirst(lc));

		if (n)
			and->args = lappend(and->args, n);
	}
	p->quals = and->args != NIL ? and : NULL;
	MemoryContextSwitchTo(old);
	vpq_pruner_set_keys(p, 0, nkeys, keys);
	return p;
}

void
vpq_pruner_set_keys(VpqPruner *p, int which, int nkeys, const ScanKeyData *keys)
{
	MemoryContext old = MemoryContextSwitchTo(p->cxt);

	p->keysets[which] = NIL;
	for (int i = 0; i < nkeys; i++)
	{
		Atom	   *a = key_atom(p, &keys[i]);

		if (a)
			p->keysets[which] = lappend(p->keysets[which], a);
	}
	MemoryContextSwitchTo(old);
}

bool
vpq_pruner_active(VpqPruner *p)
{
	if (p == NULL || !vpq_enable_pruning)
		return false;
	if (p->quals != NULL)
		return true;
	for (int i = 0; i < NKEYSETS; i++)
		if (p->keysets[i] != NIL)
			return true;
	return false;
}

/* The stat's value against the atom's constant: <0, 0, >0 */
static int
compare(Atom *a, const VpqStat *s, Datum stat, Datum value)
{
	if (s->bytewise)
	{
		bytea	   *sb = DatumGetByteaPP(stat);

		if (a->uuid)
			return vpq_bytes_cmp(VARDATA_ANY(sb), VARSIZE_ANY_EXHDR(sb), DatumGetPointer(value), 16);
		else
		{
			struct varlena *vb = (struct varlena *) PG_DETOAST_DATUM_PACKED(value);

			return vpq_bytes_cmp(VARDATA_ANY(sb), VARSIZE_ANY_EXHDR(sb), VARDATA_ANY(vb),
								 VARSIZE_ANY_EXHDR(vb));
		}
	}
	return DatumGetInt32(FunctionCall2Coll(&a->cmp, a->collation, stat, value));
}

static bool
value_is_nan(Atom *a, Datum v)
{
	if (!a->is_float)
		return false;
	if (a->right_type == FLOAT4OID)
		return isnan((double) DatumGetFloat4(v));
	if (a->right_type == FLOAT8OID)
		return isnan(DatumGetFloat8(v));
	return true;
}

static bool
one_may_match(Atom *a, const VpqStat *s, Datum v)
{
	int			strategy = a->strategy;
	bool		range = strategy != BTEqualStrategyNumber;

	if (s->bytewise && ((range && !a->bytes_range) || (!range && !a->bytes_equal)))
		return true;
	if (s->may_nan && (strategy == BTGreaterStrategyNumber || strategy == BTGreaterEqualStrategyNumber ||
					   value_is_nan(a, v)))
		return true;
	switch (strategy)
	{
		case BTLessStrategyNumber:
			return compare(a, s, s->min, v) < 0;
		case BTLessEqualStrategyNumber:
			return compare(a, s, s->min, v) <= 0;
		case BTEqualStrategyNumber:
			return compare(a, s, s->min, v) <= 0 && compare(a, s, s->max, v) >= 0;
		case BTGreaterEqualStrategyNumber:
			return compare(a, s, s->max, v) >= 0;
		case BTGreaterStrategyNumber:
			return compare(a, s, s->max, v) > 0;
	}
	return true;
}

static bool
atom_may_match(Atom *a, VpqStatFn stat, void *arg, int64 rows)
{
	const VpqStat *s = stat(arg, a->attnum);

	if (s == NULL)
		return true;
	if (a->strategy == ATOM_ISNULL)
		return s->nulls != 0;
	if (a->strategy == ATOM_NOTNULL)
		return s->nulls < 0 || s->nulls < rows;
	if (a->isnull)
		return false;
	/* a unit of NULLs alone meets no comparison */
	if (s->nulls >= 0 && s->nulls >= rows)
		return false;
	if (a->strategy == ATOM_GEO_OVERLAPS)
		return !s->has_box || vpq_geo_boxes_overlap(s->box, a->box);
	if (!s->has_minmax)
		return true;
	if (a->in_list)
	{
		for (int i = 0; i < a->nvalues; i++)
			if (one_may_match(a, s, a->values[i]))
				return true;
		return false;
	}
	return one_may_match(a, s, a->values[0]);
}

static bool
node_may_match(PNode *n, VpqStatFn stat, void *arg, int64 rows)
{
	ListCell   *lc;

	switch (n->kind)
	{
		case NODE_ATOM:
			return atom_may_match(n->atom, stat, arg, rows);
		case NODE_AND:
			foreach(lc, n->args)
				if (!node_may_match(lfirst(lc), stat, arg, rows))
					return false;
			return true;
		case NODE_OR:
			foreach(lc, n->args)
				if (node_may_match(lfirst(lc), stat, arg, rows))
					return true;
			return false;
	}
	return true;
}

/* Whether a unit of rows rows, its statistics by stat(), may hold a row the quals and keys keep */
bool
vpq_pruner_may_match(VpqPruner *p, VpqStatFn stat, void *arg, int64 rows)
{
	if (!vpq_pruner_active(p))
		return true;
	if (p->quals != NULL && !node_may_match(p->quals, stat, arg, rows))
		return false;
	for (int i = 0; i < NKEYSETS; i++)
	{
		ListCell   *lc;

		foreach(lc, p->keysets[i])
			if (!atom_may_match(lfirst(lc), stat, arg, rows))
				return false;
	}
	return true;
}
