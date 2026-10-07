/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_pgvector.c
 *	  vexec's kernel pack for pgvector (pg_vector_executor.md §3.17, VK).
 *
 * What it declares.  Every distance of pgvector's vector and halfvec raises
 * an error in one place alone: CheckDims(), when the two arguments'
 * dimensions differ, ERRCODE_DATA_EXCEPTION "different vector dimensions %d
 * and %d" -- "different halfvec dimensions" for halfvec -- and in nothing
 * else of their bodies or of the loops they call, which sum in float and
 * never test their values (read in pgvector 0.8.6, src/vector.c:71-76,
 * 579-753; src/halfvec.c:75-80, 560-689; src/halfutils.c, and unchanged in
 * 0.8.7).  So each is declared with a check, that the two dimensions are
 * equal: vexec calls pgvector's own function on the rows it passes, a batch
 * at a time, with one prepared call frame, and sends the others to
 * PostgreSQL's evaluator, where pgvector raises its own error in row order.
 * A zero vector under cosine distance gives NaN there as here, since every
 * result is pgvector's.
 *
 * The functions, as pgvector's extension script creates them, all
 * IMMUTABLE STRICT from '$libdir/vector':
 *
 *	vector:  l2_distance, inner_product, cosine_distance, l1_distance, under
 *	         their own C symbols; vector_l2_squared_distance,
 *	         vector_negative_inner_product (the operator <#>) and
 *	         vector_spherical_distance;
 *	halfvec: l2_distance, inner_product, cosine_distance and l1_distance
 *	         under halfvec_<name>; halfvec_l2_squared_distance,
 *	         halfvec_negative_inner_product (<#>) and
 *	         halfvec_spherical_distance.
 *
 * <->, <=> and <+> are l2_distance, cosine_distance and l1_distance.
 *
 * The check reads the dimensions from the layout pgvector documents in its
 * headers (src/vector.h:18-24, src/halfvec.h:67-73): a 4-byte varlena
 * header, then int16 dim, the same for both types.  vexec hands a check
 * its arguments detoasted (vexec_kernels.h); a value that comes otherwise
 * -- a call from SQL -- is read through a slice of its first bytes.  A
 * value too short to hold its dimensions fails the check, and goes to
 * pgvector.
 *
 * Results are pgvector's own, by construction: the pack has no loop of its
 * own.  Kernels over batches -- a dense matrix, loops across rows -- wait
 * for VK's measurement (§3.17).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#include "vexec_kernels.h"

PG_MODULE_MAGIC_EXT(
					.name = "vexec_pgvector",
					.version = "1.0"
);

PG_FUNCTION_INFO_V1(vexec_pgvector_dims_check);
PG_FUNCTION_INFO_V1(vexec_pgvector_declarations);

/* A vector's or a halfvec's dimensions: after the varlena header, int16. */
#define DIMS_OFFSET		VARHDRSZ
#define DIMS_END		(VARHDRSZ + sizeof(int16))

/*
 * The dimensions of a vector or a halfvec, or false where the value is too
 * short to hold them.  Never raises: a toasted value -- which vexec does not
 * hand a check -- is read through a slice of its first bytes.
 */
static bool
dims_of(Datum d, int16 *dims)
{
	struct varlena *v = (struct varlena *) DatumGetPointer(d);

	if (VARATT_IS_EXTENDED(v))
		v = PG_DETOAST_DATUM_SLICE(d, 0, DIMS_END);
	if (VARSIZE(v) < DIMS_END)
		return false;
	memcpy(dims, (const char *) v + DIMS_OFFSET, sizeof(int16));
	return true;
}

/*
 * The check of every distance: the two dimensions are equal, so that
 * pgvector's CheckDims() passes them.  NULLs never come for a strict
 * function; one that came would fail the check.
 */
Datum
vexec_pgvector_dims_check(PG_FUNCTION_ARGS)
{
	int16		a;
	int16		b;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(dims_of(PG_GETARG_DATUM(0), &a) && dims_of(PG_GETARG_DATUM(1), &b) && a == b);
}

static const char *const vector_vector[] = {"vector", "vector"};
static const char *const halfvec_halfvec[] = {"halfvec", "halfvec"};

#define DISTANCE(name, args, symbol) \
	{name, 2, args, symbol, "$libdir/vector", VEXEC_DECL_CHECK, vexec_pgvector_dims_check}

static const VexecKernelDecl decls[] = {
	DISTANCE("l2_distance", vector_vector, "l2_distance"),
	DISTANCE("inner_product", vector_vector, "inner_product"),
	DISTANCE("cosine_distance", vector_vector, "cosine_distance"),
	DISTANCE("l1_distance", vector_vector, "l1_distance"),
	DISTANCE("vector_l2_squared_distance", vector_vector, "vector_l2_squared_distance"),
	DISTANCE("vector_negative_inner_product", vector_vector, "vector_negative_inner_product"),
	DISTANCE("vector_spherical_distance", vector_vector, "vector_spherical_distance"),
	DISTANCE("l2_distance", halfvec_halfvec, "halfvec_l2_distance"),
	DISTANCE("inner_product", halfvec_halfvec, "halfvec_inner_product"),
	DISTANCE("cosine_distance", halfvec_halfvec, "halfvec_cosine_distance"),
	DISTANCE("l1_distance", halfvec_halfvec, "halfvec_l1_distance"),
	DISTANCE("halfvec_l2_squared_distance", halfvec_halfvec, "halfvec_l2_squared_distance"),
	DISTANCE("halfvec_negative_inner_product", halfvec_halfvec, "halfvec_negative_inner_product"),
	DISTANCE("halfvec_spherical_distance", halfvec_halfvec, "halfvec_spherical_distance"),
};

/* The pgvector releases whose code the declarations were checked against. */
static const char *const versions[] = {"0.8.6", "0.8.7", NULL};

static const VexecKernelPack pack = {
	.size = sizeof(VexecKernelPack),
	.minor = VEXEC_KERNELS_MINOR,
	.name = "vexec_pgvector",
	.extension = "vector",
	.versions = versions,
	.ndecls = lengthof(decls),
	.decls = decls,
};

/*
 * Registered while the postmaster preloads libraries, on every server, in
 * any order with vexec.  Loaded otherwise -- by a call of its SQL functions
 * -- the pack registers nothing, and declares nothing in that backend.
 */
void
_PG_init(void)
{
	if (process_shared_preload_libraries_in_progress)
		vexec_register_kernel_pack(&pack);
}

/*
 * vexec_pgvector.declarations(): what the pack declares -- each function by
 * its name, its argument types and its C symbol, its declaration, and the
 * pgvector versions it holds for -- and whether the pack registered in this
 * server.
 */
Datum
vexec_pgvector_declarations(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VexecKernelRegistry *reg = vexec_find_kernel_registry();
	Datum		vers[lengthof(versions)];
	int			nvers = 0;
	bool		registered = false;
	int			i;

	InitMaterializedSRF(fcinfo, 0);
	while (versions[nvers] != NULL)
	{
		vers[nvers] = CStringGetTextDatum(versions[nvers]);
		nvers++;
	}
	for (i = 0; reg != NULL && i < reg->npacks; i++)
		registered |= reg->packs[i] == &pack;
	for (i = 0; i < (int) lengthof(decls); i++)
	{
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};
		Datum		args[2];

		args[0] = CStringGetTextDatum(decls[i].argtypes[0]);
		args[1] = CStringGetTextDatum(decls[i].argtypes[1]);
		values[0] = CStringGetTextDatum(decls[i].name);
		values[1] = PointerGetDatum(construct_array_builtin(args, 2, TEXTOID));
		values[2] = CStringGetTextDatum(decls[i].symbol);
		values[3] = CStringGetTextDatum("check: equal dimensions");
		values[4] = PointerGetDatum(construct_array_builtin(vers, nvers, TEXTOID));
		values[5] = BoolGetDatum(registered);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	PG_RETURN_VOID();
}
