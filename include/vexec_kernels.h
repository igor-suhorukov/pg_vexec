/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_kernels.h
 *	  The kernel packs' registry: what a pack declares of another
 *	  extension's functions, so that vexec may call them ahead of
 *	  PostgreSQL's order (pg_vector_executor.md §3.17, VK).
 *
 * A vector node evaluates its quals and targets a batch at a time, ahead of
 * the rows being asked for, only where that can neither change an answer nor
 * raise an error PostgreSQL would not raise: a call with one of vexec's
 * kernels, or of a leakproof function.  Any other call makes its whole qual
 * or target lazy: PostgreSQL's evaluator runs it a row at a time, in row
 * order, for the rows asked for.  So a qual over pgvector's distances or
 * PostGIS's predicates, none of them leakproof, is wholly row by row.
 *
 * A kernel pack, an extension of its own, declares what vexec cannot know of
 * an extension's functions, for the versions of that extension whose code
 * its authors read, in one of three ways:
 *
 *	VEXEC_DECL_NEVER_RAISES   the function raises no error on any value the
 *							  extension stores.  vexec calls it on every row
 *							  it computes.
 *	VEXEC_DECL_CHECK		  a function of the call's arguments that returns
 *							  true only where the function cannot raise.
 *							  vexec calls the function on the rows it passes.
 *	VEXEC_DECL_PREFILTER	  a function of the call's arguments that gives
 *							  the function's own result where a cheap test
 *							  decides it, and reports every other row
 *							  undecided.  vexec takes its answers, and calls
 *							  the function on no row itself.
 *
 * vexec calls the extension's own function through fmgr, with one prepared
 * call frame, a batch's rows at a time.  A row a check fails, or a prefilter
 * leaves undecided, goes to PostgreSQL's evaluator, in row order, when it is
 * asked for: the function is called there as PostgreSQL calls it, and its
 * error, if it raises one, is PostgreSQL's own, where PostgreSQL would raise
 * it.  So every result vexec does not take from a prefilter is the
 * extension's own function's, and exact by construction.
 *
 * No kernel ABI.  The boundary between vexec and a pack is PostgreSQL's
 * own: C functions of fmgr's version-1 convention, and PostgreSQL's soft
 * errors.  A pack includes PostgreSQL's headers and this one; it never sees
 * vexec's batches, and never calls into vexec by symbol -- there is no order
 * between the modules in "shared_preload_libraries".
 *
 * A pack's functions -- its checks and prefilters -- are called:
 *
 *	- with the call's arguments, as the function would be called: Datums in
 *	  a FunctionCallInfo, with the call's collation.  Only on the rows the
 *	  function would be called for: for a strict function, never with a NULL
 *	  argument;
 *	- for a check, its varlena arguments detoasted, with 4-byte headers --
 *	  vexec detoasts them once a row, and hands the function the same Datums,
 *	  so that it detoasts nothing again; a prefilter is handed them as they
 *	  are stored, and reads what it needs itself, a toasted value's header
 *	  through a slice, say;
 *	- with a call frame of their own whose flinfo has fn_oid InvalidOid and
 *	  fn_extra NULL at every row: they keep no state from row to row, and
 *	  need no catalog entry;
 *	- in a memory context reset after each row.
 *
 * A check returns a bool, never NULL, and never raises.  A prefilter returns
 * the function's result for a row it decides, and for a row it does not,
 * reports it by a soft error -- ereturn(fcinfo->context, (Datum) 0, ...) --
 * which vexec reads with SOFT_ERROR_OCCURRED; it raises nothing else.
 * Called without vexec's ErrorSaveContext, a prefilter's ereturn() raises,
 * as ereport(ERROR) does, which is what a pack's own tests of it expect.
 *
 * A pack's rules (§3.17):
 *
 *	- a declaration is written only for a function whose code was read, for
 *	  each version of the extension it names;
 *	- a prefilter gives the function's own answer for the rows it decides,
 *	  only where the function would raise no error, and leaves every other
 *	  row to the function; a pack never reimplements what it cannot
 *	  reproduce exactly;
 *	- nothing is marked LEAKPROOF with ALTER FUNCTION, which would change
 *	  what the planner moves past row-level security: a declaration holds
 *	  only for vexec's own evaluation ahead of PostgreSQL's order;
 *	- the pack ships a differential suite, which compares each prefilter's
 *	  answers with the function's own, and tries each never-raising
 *	  declaration and each check, over random inputs and edge cases.
 *
 * Binding (vexec's, in each database, since an extension's functions have
 * OIDs of their own there).  A call binds to a declaration where the
 * function belongs to the extension the pack serves, by name; its installed
 * version, pg_extension.extversion, is one the pack names; its name and
 * argument types are the declaration's, each type of that name in
 * pg_catalog or a member of the same extension; and it is the extension's
 * own C function -- language C, its prosrc and probin the declaration's
 * symbol and library.  Otherwise the call runs as it does without the pack.
 * So a server without the pack, or with another version of the extension,
 * gives the same answers, more slowly.
 *
 * Registration.  A pack registers from its _PG_init while the postmaster
 * preloads libraries, on every server -- the coordinator, each segment, one
 * node -- before or after vexec: whichever comes first makes the registry.
 * The pack and everything it points to must stay valid for the life of the
 * process: static data.
 *
 * Versions: as vexec_source.h's.  The major version is in the rendezvous
 * name; minor additions go at the end of VexecKernelPack, whose size says how
 * much of it the pack was built with.
 *
 * Kernels over batches, if they come, are a minor version of their own,
 * over arrays of the Arrow C Data Interface (§3.17): none in this one.
 *
 * vexec installs this header with itself, into the server's include
 * directory under extension/vexec/, so that a pack built elsewhere can
 * include it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_KERNELS_H
#define VEXEC_KERNELS_H

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#define VEXEC_KERNELS_RENDEZVOUS	"vexec/kernels_v1"	/* the major version is
														 * in the name */
#define VEXEC_KERNELS_MINOR			0
#define VEXEC_KERNELS_MAGIC			0x56584B31	/* "VXK1" */
#define VEXEC_KERNELS_MAX			32	/* packs in one server */

/* What a declaration says of a function. */
typedef enum VexecDeclKind
{
	VEXEC_DECL_NEVER_RAISES = 1,	/* no pack function */
	VEXEC_DECL_CHECK = 2,			/* fn: may the function be called? */
	VEXEC_DECL_PREFILTER = 3		/* fn: the function's answer, or
									 * undecided */
} VexecDeclKind;

/*
 * One function of the extension, as pg_proc holds it in a database where the
 * extension is installed: its name, its arguments' types by name, and the C
 * function it is -- the symbol and the library CREATE FUNCTION named,
 * pg_proc's prosrc and probin.  library is as the extension's script stored
 * it, MODULE_PATHNAME replaced: '$libdir/vector', '$libdir/postgis-3'.
 */
typedef struct VexecKernelDecl
{
	const char *name;			/* pg_proc.proname */
	int			nargs;
	const char *const *argtypes;	/* nargs pg_type.typname */
	const char *symbol;			/* pg_proc.prosrc */
	const char *library;		/* pg_proc.probin */
	int			kind;			/* VexecDeclKind */
	PGFunction	fn;				/* the check or the prefilter; NULL for
								 * VEXEC_DECL_NEVER_RAISES */
} VexecKernelDecl;

/* A pack: the extension it serves, its versions, its declarations. */
typedef struct VexecKernelPack
{
	Size		size;			/* sizeof as the pack was built */
	int			minor;			/* VEXEC_KERNELS_MINOR it was built with */
	const char *name;			/* the pack's, for EXPLAIN: "vexec_pgvector" */
	const char *extension;		/* pg_extension.extname: "vector" */
	const char *const *versions;	/* pg_extension.extversion values its
									 * declarations were written for,
									 * NULL-terminated */
	int			ndecls;
	const VexecKernelDecl *decls;
} VexecKernelPack;

/*
 * The registry the rendezvous variable points at.  Whichever module comes
 * first while the postmaster loads libraries makes it; it is never freed.
 * Its layout is fixed for the major version.
 */
typedef struct VexecKernelRegistry
{
	uint32		magic;
	int			npacks;
	const VexecKernelPack *packs[VEXEC_KERNELS_MAX];
} VexecKernelRegistry;

/* The registry, made if no module has made it yet. */
static inline VexecKernelRegistry *
vexec_kernel_registry(void)
{
	VexecKernelRegistry **rv;

	rv = (VexecKernelRegistry **) find_rendezvous_variable(VEXEC_KERNELS_RENDEZVOUS);
	if (*rv == NULL)
	{
		VexecKernelRegistry *reg;

		reg = (VexecKernelRegistry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(VexecKernelRegistry));
		reg->magic = VEXEC_KERNELS_MAGIC;
		*rv = reg;
	}
	else if ((*rv)->magic != VEXEC_KERNELS_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("the rendezvous variable \"%s\" does not hold vexec's kernel registry",
						VEXEC_KERNELS_RENDEZVOUS)));
	return *rv;
}

/*
 * Register a pack, from its _PG_init.  Only while the postmaster preloads
 * libraries, as vexec_register_source() refuses otherwise: a backend that
 * loaded the pack later would bind calls its siblings do not.  A pack loaded
 * otherwise -- by a call of one of its SQL functions -- registers nothing,
 * so it tests whether it may (process_shared_preload_libraries_in_progress)
 * before calling this.  Each declaration is checked here, so that a pack's
 * mistake fails the server's start rather than a statement.
 */
static inline void
vexec_register_kernel_pack(const VexecKernelPack *pack)
{
	VexecKernelRegistry *reg;
	int			i;

	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a vexec kernel pack can only be registered while \"shared_preload_libraries\" are loaded")));
	if (pack == NULL || pack->size < offsetof(VexecKernelPack, decls) + sizeof(pack->decls) ||
		pack->name == NULL || pack->extension == NULL || pack->versions == NULL ||
		pack->versions[0] == NULL || pack->ndecls < 0 ||
		(pack->ndecls > 0 && pack->decls == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a vexec kernel pack must name itself, its extension, a version of it, and its declarations")));
	for (i = 0; i < pack->ndecls; i++)
	{
		const VexecKernelDecl *d = &pack->decls[i];
		int			a;
		bool		ok;

		ok = d->name != NULL && d->symbol != NULL && d->library != NULL &&
			d->nargs >= 0 && d->nargs <= FUNC_MAX_ARGS &&
			(d->nargs == 0 || d->argtypes != NULL) &&
			(d->kind == VEXEC_DECL_NEVER_RAISES ? d->fn == NULL :
			 (d->kind == VEXEC_DECL_CHECK || d->kind == VEXEC_DECL_PREFILTER) && d->fn != NULL);
		for (a = 0; ok && a < d->nargs; a++)
			ok = d->argtypes[a] != NULL;
		if (!ok)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("declaration %d of vexec kernel pack \"%s\" is malformed", i + 1, pack->name)));
	}

	reg = vexec_kernel_registry();
	for (i = 0; i < reg->npacks; i++)
		if (strcmp(reg->packs[i]->name, pack->name) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("a vexec kernel pack named \"%s\" is already registered", pack->name)));
	if (reg->npacks >= VEXEC_KERNELS_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many vexec kernel packs"),
				 errdetail("At most %d can be registered.", VEXEC_KERNELS_MAX)));
	reg->packs[reg->npacks++] = pack;
}

/* The registry, or NULL where no module has made it. */
static inline const VexecKernelRegistry *
vexec_find_kernel_registry(void)
{
	VexecKernelRegistry **rv;

	rv = (VexecKernelRegistry **) find_rendezvous_variable(VEXEC_KERNELS_RENDEZVOUS);
	if (*rv == NULL || (*rv)->magic != VEXEC_KERNELS_MAGIC)
		return NULL;
	return *rv;
}

/* Whether a pack has a member: it was built with a struct that long. */
#define VEXEC_KERNEL_PACK_HAS(pack, member) \
	((pack)->size >= offsetof(VexecKernelPack, member) + sizeof((pack)->member))

#endif							/* VEXEC_KERNELS_H */
