/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * prefilter.c
 *	  The quals heap's page reader checks on a tuple before deforming it
 *	  (X3 of pg_vector_executor.md §3.18).
 *
 * A VecScan deforms every visible tuple to the last attribute it reads, its
 * quals' and its targets' alike, before its quals run; PostgreSQL deforms a
 * tuple to its quals' attributes, and the targets' only for the rows the
 * quals keep (execExpr.c, EEOP_SCAN_FETCHSOME).  On a wide table whose text
 * columns end the attributes' fixed offsets early -- ClickBench's hits at
 * its third -- reaching an attribute walks every one before it.
 *
 * The plan proposed deforming a batch's tuples a qual's attributes at a
 * time, each qual run over the rows the one before kept.  That needs every
 * page of the batch pinned between the steps, and a scan of a large table
 * holds only VEXEC_HEAP_PINS pages past its current one (heap.c: its read
 * stream may pin the whole ring).  So the reader checks the quals on each
 * tuple instead, while its page is pinned: it walks the tuple as far as the
 * quals' attributes, checks each as the walk reaches it, and drops the
 * tuple at the first that fails, before anything of it is deformed; a tuple
 * that passes is deformed from where the walk stopped.  On the share of
 * rows ClickBench's filters keep -- 0.001% to 0.7% -- nearly every tuple is
 * walked only to the first attribute that rejects it.
 *
 * Which quals.  The longest prefix of the scan's quals, in PostgreSQL's
 * order, whose every qual is one of these, over a column of the scan's
 * relation:
 *
 *	column op constant		=, <>, <, <=, >, >= of int2, int4, int8 (and
 *							their cross types), oid, bool, date, timestamp
 *							and timestamptz, by their btree families;
 *							text and bytea = and <>, under a deterministic
 *							collation, which compare their bytes
 *	column = ANY (array)	of those types' equalities, the array a
 *							constant
 *	column LIKE constant	a literal, a prefix, a suffix or a substring,
 *							H7's kinds (expr/k_text.c), NOT LIKE too
 *	column IS [NOT] NULL
 *
 * None of these can raise, so a tuple one of them rejects is one
 * PostgreSQL rejects -- at that qual, or at an earlier one of the prefix --
 * and no qual PostgreSQL would have evaluated before it can raise; within
 * the prefix any order gives the same rows, and the reader takes them by
 * their attributes' depth, the cheapest walk first (§3.18, X3's order on
 * heap until it is measured).  A qual past the prefix may raise, and keeps
 * its place: the node evaluates every qual on the tuples that pass, the
 * checked ones again, so a row's answer and its errors are PostgreSQL's.  A
 * value the reader cannot read as it lies -- a compressed or external text,
 * an attribute the tuple lacks -- passes the check, and the node decides it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "varatt.h"

#include "vexec.h"
#include "expr/expr.h"
#include "source/source.h"

#define DATETIME_BTREE_FAM_OID	434		/* datetime_ops, which has no symbol */

/* The column a qual reads: a Var of the scan's relation, through relabels. */
static Var *
column_of(Node *node, Index scanrelid, Relation rel)
{
	Var		   *var;

	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (node == NULL || !IsA(node, Var))
		return NULL;
	var = (Var *) node;
	if (var->varno != scanrelid || var->varlevelsup != 0 || var->varattno <= 0 ||
		var->varattno > RelationGetDescr(rel)->natts ||
		TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attisdropped)
		return NULL;
	return var;
}

/* An integer-like type's value of a constant, as the check compares it. */
static bool
int_type(Oid type, bool *is_unsigned)
{
	*is_unsigned = type == OIDOID;
	switch (type)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case BOOLOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		default:
			return false;
	}
}

static int64
int_value(Datum d, Oid type)
{
	switch (type)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		case OIDOID:
			return (int64) DatumGetObjectId(d);
		case BOOLOID:
			return DatumGetBool(d) ? 1 : 0;
		default:
			return DatumGetInt64(d);
	}
}

/*
 * How an operator over a column of `coltype` and a constant of `consttype`
 * compares, where the check can compute it: its comparison, from its btree
 * family's interpretation.  The integers' family compares across their
 * widths; the others, a type with itself.
 */
static CompareType
int_compare(Oid opno, Oid coltype, Oid consttype)
{
	List	   *interps = get_op_index_interpretation(opno);
	ListCell   *lc;

	foreach(lc, interps)
	{
		OpIndexInterpretation *oi = lfirst(lc);

		if (oi->oplefttype != coltype || oi->oprighttype != consttype)
			continue;
		if (oi->opfamily_id == INTEGER_BTREE_FAM_OID ||
			((oi->opfamily_id == OID_BTREE_FAM_OID || oi->opfamily_id == BOOL_BTREE_FAM_OID ||
			  oi->opfamily_id == DATETIME_BTREE_FAM_OID) && coltype == consttype))
		{
			if (coltype == BOOLOID && oi->cmptype != COMPARE_EQ && oi->cmptype != COMPARE_NE)
				continue;
			return oi->cmptype;
		}
	}
	return COMPARE_INVALID;
}

/* Whether a text operator's function compares bytes: =, <>, under a deterministic collation. */
static bool
bytes_equality(Oid funcid, Oid collation, bool *negate)
{
	switch (funcid)
	{
		case F_TEXTEQ:
		case F_BYTEAEQ:
			*negate = false;
			break;
		case F_TEXTNE:
		case F_BYTEANE:
			*negate = true;
			break;
		default:
			return false;
	}
	return funcid == F_BYTEAEQ || funcid == F_BYTEANE ||
		vexec_collation_class(collation) != InvalidOid;
}

/* The text column types whose values the reader reads as bytes. */
static bool
bytes_type(Oid type)
{
	return type == TEXTOID || type == VARCHAROID || type == BYTEAOID;
}

/* A constant's bytes, kept in the check. */
static void
const_bytes(Datum d, const char **p, int *len)
{
	varlena    *v = (varlena *) PG_DETOAST_DATUM_PACKED(d);

	*p = VARDATA_ANY(v);
	*len = VARSIZE_ANY_EXHDR(v);
}

static bool
check_opexpr(OpExpr *op, Index scanrelid, Relation rel, VexecHeapCheck *k)
{
	Node	   *left;
	Node	   *right;
	Var		   *var;
	Const	   *c;
	Oid			opno = op->opno;
	Form_pg_attribute att;

	if (list_length(op->args) != 2)
		return false;
	left = linitial(op->args);
	right = lsecond(op->args);
	if ((var = column_of(left, scanrelid, rel)) != NULL && IsA(right, Const))
		c = (Const *) right;
	else if ((var = column_of(right, scanrelid, rel)) != NULL && IsA(left, Const))
	{
		c = (Const *) left;
		opno = get_commutator(opno);
		if (!OidIsValid(opno))
			return false;
	}
	else
		return false;
	if (c->constisnull)
		return false;
	set_opfuncid(op);
	att = TupleDescAttr(RelationGetDescr(rel), var->varattno - 1);
	k->attnum = var->varattno;
	k->attlen = att->attlen;
	k->nvalues = 1;

	/* LIKE, NOT LIKE: the column on the left, the pattern a constant */
	if ((op->opfuncid == F_TEXTLIKE || op->opfuncid == F_TEXTNLIKE) && (Node *) c == right)
	{
		const char *p;
		int			plen;
		char	   *lit;
		int			litlen;

		if (!bytes_type(att->atttypid) || att->atttypid == BYTEAOID ||
			vexec_collation_class(op->inputcollid) == InvalidOid)
			return false;
		const_bytes(c->constvalue, &p, &plen);
		if (!vexec_like_classify(p, plen, &k->like_kind, &lit, &litlen) ||
			k->like_kind == VEXEC_LIKE_GENERAL || k->like_kind == VEXEC_LIKE_ANY)
			return false;
		/* byte tests only where whole characters match at their starts (H7) */
		if (GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1)
			return false;
		k->kind = VEXEC_CHECK_LIKE;
		k->negate = op->opfuncid == F_TEXTNLIKE;
		k->bytes = palloc(sizeof(char *));
		k->lens = palloc(sizeof(int));
		k->bytes[0] = lit;
		k->lens[0] = litlen;
		return true;
	}

	/* =, <> on bytes */
	if (bytes_type(att->atttypid) && bytes_type(c->consttype))
	{
		if (!bytes_equality(get_opcode(opno), op->inputcollid, &k->negate) ||
			(att->atttypid == BYTEAOID) != (c->consttype == BYTEAOID))
			return false;
		k->kind = VEXEC_CHECK_BYTES;
		k->bytes = palloc(sizeof(char *));
		k->lens = palloc(sizeof(int));
		const_bytes(c->constvalue, &k->bytes[0], &k->lens[0]);
		return true;
	}

	/* comparisons of integers and the like */
	if (int_type(att->atttypid, &k->is_unsigned) && att->attbyval &&
		int_type(c->consttype, &k->is_unsigned))
	{
		CompareType cmp = int_compare(opno, att->atttypid, c->consttype);

		k->is_unsigned = att->atttypid == OIDOID;
		if (cmp == COMPARE_INVALID)
			return false;
		k->kind = VEXEC_CHECK_INT;
		k->negate = cmp == COMPARE_NE;
		k->cmp = cmp == COMPARE_NE ? COMPARE_EQ : cmp;
		k->ints = palloc(sizeof(int64));
		k->ints[0] = int_value(c->constvalue, c->consttype);
		return true;
	}
	return false;
}

/* column = ANY (constant array), of the equalities above */
static bool
check_saop(ScalarArrayOpExpr *saop, Index scanrelid, Relation rel, VexecHeapCheck *k)
{
	Var		   *var;
	Const	   *c;
	ArrayType  *arr;
	Oid			elemtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	Form_pg_attribute att;

	if (!saop->useOr || list_length(saop->args) != 2 ||
		(var = column_of(linitial(saop->args), scanrelid, rel)) == NULL ||
		!IsA(lsecond(saop->args), Const))
		return false;
	c = lsecond_node(Const, saop->args);
	if (c->constisnull)
		return false;
	att = TupleDescAttr(RelationGetDescr(rel), var->varattno - 1);
	arr = DatumGetArrayTypeP(c->constvalue);
	elemtype = ARR_ELEMTYPE(arr);
	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign, &elems, &nulls, &nelems);
	k->attnum = var->varattno;
	k->attlen = att->attlen;
	k->negate = false;
	k->nvalues = 0;

	if (bytes_type(att->atttypid) && bytes_type(elemtype))
	{
		bool		negate;

		set_sa_opfuncid(saop);
		if (!bytes_equality(saop->opfuncid, saop->inputcollid, &negate) || negate ||
			(att->atttypid == BYTEAOID) != (elemtype == BYTEAOID))
			return false;
		k->kind = VEXEC_CHECK_BYTES;
		k->bytes = palloc(sizeof(char *) * Max(nelems, 1));
		k->lens = palloc(sizeof(int) * Max(nelems, 1));
		for (int i = 0; i < nelems; i++)
		{
			/* a NULL element matches nothing: the qual is NULL or true */
			if (nulls[i])
				continue;
			const_bytes(elems[i], &k->bytes[k->nvalues], &k->lens[k->nvalues]);
			k->nvalues++;
		}
		return true;
	}
	if (int_type(att->atttypid, &k->is_unsigned) && att->attbyval &&
		int_type(elemtype, &k->is_unsigned) &&
		int_compare(saop->opno, att->atttypid, elemtype) == COMPARE_EQ)
	{
		k->kind = VEXEC_CHECK_INT;
		k->is_unsigned = att->atttypid == OIDOID;
		k->cmp = COMPARE_EQ;
		k->ints = palloc(sizeof(int64) * Max(nelems, 1));
		for (int i = 0; i < nelems; i++)
			if (!nulls[i])
				k->ints[k->nvalues++] = int_value(elems[i], elemtype);
		return true;
	}
	return false;
}

/* One qual as a check, or false where it is none of the kinds above. */
static bool
check_from_qual(Node *qual, Index scanrelid, Relation rel, VexecHeapCheck *k)
{
	memset(k, 0, sizeof(VexecHeapCheck));
	if (IsA(qual, OpExpr))
		return check_opexpr((OpExpr *) qual, scanrelid, rel, k);
	if (IsA(qual, ScalarArrayOpExpr))
		return check_saop((ScalarArrayOpExpr *) qual, scanrelid, rel, k);
	if (IsA(qual, NullTest))
	{
		NullTest   *nt = (NullTest *) qual;
		Var		   *var;

		if (nt->argisrow || (var = column_of((Node *) nt->arg, scanrelid, rel)) == NULL)
			return false;
		k->kind = VEXEC_CHECK_NULL;
		k->attnum = var->varattno;
		k->attlen = TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attlen;
		k->negate = nt->nulltesttype == IS_NOT_NULL;
		return true;
	}
	return false;
}

static int
check_cmp(const void *a, const void *b)
{
	return pg_cmp_s16(((const VexecHeapCheck *) a)->attnum, ((const VexecHeapCheck *) b)->attnum);
}

/*
 * The checks of a scan's quals (above), in the order the reader checks
 * them: by their attributes' depth.  *nquals: how many of the quals, the
 * prefix, they stand for.  NULL where none.
 */
VexecHeapCheck *
vexec_heap_checks(Relation rel, Index scanrelid, List *quals, int *ncheck, int *nquals)
{
	VexecHeapCheck *checks = palloc0(sizeof(VexecHeapCheck) * Max(list_length(quals), 1));
	int			n = 0;
	ListCell   *lc;

	foreach(lc, quals)
	{
		if (!check_from_qual(lfirst(lc), scanrelid, rel, &checks[n]))
			break;
		n++;
	}
	*ncheck = n;
	*nquals = n;
	if (n == 0)
	{
		pfree(checks);
		return NULL;
	}
	/* a stable order by depth: qsort's is not, so ties break by position */
	for (int i = 1; i < n; i++)
	{
		VexecHeapCheck t = checks[i];
		int			j = i - 1;

		while (j >= 0 && check_cmp(&checks[j], &t) > 0)
		{
			checks[j + 1] = checks[j];
			j--;
		}
		checks[j + 1] = t;
	}
	return checks;
}
