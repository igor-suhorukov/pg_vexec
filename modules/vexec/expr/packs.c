/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * packs.c
 *	  vexec's side of the kernel packs' registry: a call bound to a pack's
 *	  declaration (pg_vector_executor.md §3.17; the contract is
 *	  vexec_kernels.h).
 *
 * A function binds to a declaration in each database, since an extension's
 * functions have OIDs of their own there, when all of these hold:
 *
 *	- it belongs to an extension (getExtensionOfObject), and a registered
 *	  pack serves that extension, by name;
 *	- the extension's installed version, pg_extension.extversion, is one the
 *	  pack names;
 *	- the pack declares a function of its name and argument types, each type
 *	  of that name in pg_catalog or a member of the same extension;
 *	- it is the extension's own C function: its language is C, and its
 *	  prosrc and probin are the declaration's symbol and library.
 *	  Membership, name and signature alone prove nothing, since ALTER
 *	  EXTENSION ... ADD FUNCTION makes any function a member.
 *
 * and, at each lookup, as for a built-in kernel (kernels.c): it is not
 * security definer, has no proconfig, and no plugin claims it through
 * needs_fmgr_hook.  The EXECUTE permission check and
 * InvokeFunctionExecuteHook are made when the node begins, by
 * ExecInitCustomScan's own compilation of its expressions.
 *
 * Bindings are kept per backend, by function OID, as the built-in table is:
 * a hash by OID alone, made at first use, the functions no pack declares
 * among them, so that a call of any other function costs a probe.  They are
 * dropped, every one, when pg_proc or pg_extension changes in a way this
 * backend's syscaches hear of -- DROP EXTENSION, ALTER EXTENSION UPDATE,
 * which changes extversion, ALTER FUNCTION, CREATE OR REPLACE FUNCTION --
 * through syscache callbacks on PROCOID and EXTENSIONOID, in every session.
 * A program keeps what it bound while it runs; a node compiles its
 * expressions when it begins, so a statement's next execution binds anew.
 * ALTER EXTENSION ... DROP FUNCTION changes pg_depend alone, and leaves a
 * binding made before in place until the next such change: the function is
 * still the C function of the version it was bound for.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "vexec_kernels.h"

#include "vexec.h"
#include "expr/kernel.h"

PG_FUNCTION_INFO_V1(vexec_kernel_packs);
PG_FUNCTION_INFO_V1(vexec_declared_calls);

/* A function's binding: its declaration, or none. */
typedef struct DeclEntry
{
	Oid			funcid;			/* the key */
	const VexecKernelPack *pack;	/* NULL: no pack declares it */
	const VexecKernelDecl *decl;
} DeclEntry;

static HTAB *decl_table = NULL;
static uint64 decl_generation = 0;
static bool callbacks_registered = false;

/*
 * The registry, made when vexec loads so that a pack loaded after it finds
 * it, and a pack loaded before it has made it.
 */
void
vexec_kernel_packs_install(void)
{
	(void) vexec_kernel_registry();
}

/* pg_proc or pg_extension changed: every binding goes. */
static void
decl_invalidate(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	(void) arg;
	(void) cacheid;
	(void) hashvalue;
	decl_generation++;
	if (decl_table != NULL)
	{
		hash_destroy(decl_table);
		decl_table = NULL;
	}
}

static void
decl_table_make(void)
{
	HASHCTL		ctl;

	if (!callbacks_registered)
	{
		CacheRegisterSyscacheCallback(PROCOID, decl_invalidate, (Datum) 0);
		CacheRegisterSyscacheCallback(EXTENSIONOID, decl_invalidate, (Datum) 0);
		callbacks_registered = true;
	}
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(DeclEntry);
	ctl.hcxt = CacheMemoryContext;
	decl_table = hash_create("vexec declared calls", 128, &ctl,
							 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static bool
version_named(const VexecKernelPack *pack, const char *version)
{
	const char *const *v;

	for (v = pack->versions; *v != NULL; v++)
		if (strcmp(*v, version) == 0)
			return true;
	return false;
}

/*
 * Whether a type is the one a declaration names: of that name, and in
 * pg_catalog or a member of the function's extension.
 */
static bool
type_named(Oid typid, const char *name, Oid extoid)
{
	HeapTuple	tup;
	Form_pg_type typ;
	bool		same;
	Oid			nsp;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		return false;
	typ = (Form_pg_type) GETSTRUCT(tup);
	same = strcmp(NameStr(typ->typname), name) == 0;
	nsp = typ->typnamespace;
	ReleaseSysCache(tup);
	if (!same)
		return false;
	return nsp == PG_CATALOG_NAMESPACE || getExtensionOfObject(TypeRelationId, typid) == extoid;
}

/*
 * The declaration a function binds to (above), from the catalogs; *pack and
 * *decl NULL when none.
 */
static void
bind_function(Oid funcid, const VexecKernelPack **pack, const VexecKernelDecl **decl)
{
	const VexecKernelRegistry *reg = vexec_find_kernel_registry();
	HeapTuple	tup;
	Form_pg_proc proc;
	char	   *proname;
	int			nargs;
	Oid		   *argtypes;
	char	   *prosrc;
	char	   *probin = NULL;
	Datum		d;
	bool		isnull;
	Oid			extoid;
	char	   *extname;
	char	   *extversion;
	int			p,
				i,
				a;

	*pack = NULL;
	*decl = NULL;
	if (reg == NULL || reg->npacks == 0)
		return;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tup))
		return;
	proc = (Form_pg_proc) GETSTRUCT(tup);
	if (proc->prolang != ClanguageId || proc->prokind != PROKIND_FUNCTION || proc->proretset)
	{
		ReleaseSysCache(tup);
		return;
	}
	proname = pstrdup(NameStr(proc->proname));
	nargs = proc->pronargs;
	argtypes = palloc(sizeof(Oid) * Max(nargs, 1));
	memcpy(argtypes, proc->proargtypes.values, sizeof(Oid) * nargs);
	prosrc = TextDatumGetCString(SysCacheGetAttrNotNull(PROCOID, tup, Anum_pg_proc_prosrc));
	d = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
	if (!isnull)
		probin = TextDatumGetCString(d);
	ReleaseSysCache(tup);
	if (probin == NULL)
		return;

	extoid = getExtensionOfObject(ProcedureRelationId, funcid);
	if (!OidIsValid(extoid))
		return;
	tup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(extoid));
	if (!HeapTupleIsValid(tup))
		return;
	extname = pstrdup(NameStr(((Form_pg_extension) GETSTRUCT(tup))->extname));
	extversion = TextDatumGetCString(SysCacheGetAttrNotNull(EXTENSIONOID, tup,
															Anum_pg_extension_extversion));
	ReleaseSysCache(tup);

	for (p = 0; p < reg->npacks; p++)
	{
		const VexecKernelPack *pk = reg->packs[p];

		if (strcmp(pk->extension, extname) != 0 || !version_named(pk, extversion))
			continue;
		for (i = 0; i < pk->ndecls; i++)
		{
			const VexecKernelDecl *dc = &pk->decls[i];

			if (dc->nargs != nargs || strcmp(dc->name, proname) != 0 ||
				strcmp(dc->symbol, prosrc) != 0 || strcmp(dc->library, probin) != 0)
				continue;
			for (a = 0; a < nargs; a++)
				if (!type_named(argtypes[a], dc->argtypes[a], extoid))
					break;
			if (a < nargs)
				continue;
			*pack = pk;
			*decl = dc;
			return;
		}
	}
}

/*
 * The declaration a function binds to in this database, kept per backend
 * (above).  The catalogs may tell of a change while it is read -- a lookup
 * takes locks, which reads the invalidations sent -- so a binding is kept
 * only when none came while it was made.
 */
static void
decl_lookup(Oid funcid, const VexecKernelPack **pack, const VexecKernelDecl **decl)
{
	DeclEntry  *e;
	bool		found;
	uint64		generation;

	if (decl_table == NULL)
		decl_table_make();
	e = hash_search(decl_table, &funcid, HASH_FIND, NULL);
	if (e != NULL)
	{
		*pack = e->pack;
		*decl = e->decl;
		return;
	}
	do
	{
		generation = decl_generation;
		bind_function(funcid, pack, decl);
	} while (generation != decl_generation);
	if (decl_table == NULL)
		decl_table_make();
	e = hash_search(decl_table, &funcid, HASH_ENTER, &found);
	e->pack = *pack;
	e->decl = *decl;
}

/*
 * Whether a call of a function binds to a pack's declaration here, and
 * which: none for a function a built-in kernel runs, which is no
 * extension's.  Not cached: whether it may run anywhere but through fmgr
 * (vexec_kernel_permitted()).
 */
bool
vexec_declared_find(Oid funcid, VexecDeclared *out)
{
	const VexecKernelPack *pack;
	const VexecKernelDecl *decl;

	decl_lookup(funcid, &pack, &decl);
	if (decl == NULL || !vexec_kernel_permitted(funcid))
		return false;
	if (out != NULL)
	{
		out->pack = pack;
		out->decl = decl;
	}
	return true;
}

/*
 * Whether a function is declared never to raise here: then PostgreSQL's
 * evaluator may call it ahead of PostgreSQL's order too, in a fallback step
 * (compile.c), as a leakproof one.
 */
bool
vexec_declared_never_raises(Oid funcid)
{
	VexecDeclared d;

	return vexec_declared_find(funcid, &d) && d.decl->kind == VEXEC_DECL_NEVER_RAISES;
}

const char *
vexec_decl_kind_name(int kind)
{
	switch (kind)
	{
		case VEXEC_DECL_NEVER_RAISES:
			return "never raises";
		case VEXEC_DECL_CHECK:
			return "check";
		case VEXEC_DECL_PREFILTER:
			return "prefilter";
	}
	return "?";
}

/*
 * vexec.kernel_packs(): the packs registered in this server, each with the
 * extension it serves, the versions of it it names, and its declarations.
 */
Datum
vexec_kernel_packs(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VexecKernelRegistry *reg = vexec_find_kernel_registry();
	int			p;

	InitMaterializedSRF(fcinfo, 0);
	if (reg == NULL)
		PG_RETURN_VOID();
	for (p = 0; p < reg->npacks; p++)
	{
		const VexecKernelPack *pk = reg->packs[p];
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};
		int			nv = 0;
		Datum	   *versions;

		while (pk->versions[nv] != NULL)
			nv++;
		versions = palloc(sizeof(Datum) * nv);
		for (int i = 0; i < nv; i++)
			versions[i] = CStringGetTextDatum(pk->versions[i]);
		values[0] = CStringGetTextDatum(pk->name);
		values[1] = CStringGetTextDatum(pk->extension);
		values[2] = PointerGetDatum(construct_array_builtin(versions, nv, TEXTOID));
		values[3] = Int32GetDatum(pk->ndecls);
		values[4] = Int32GetDatum(pk->minor);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	PG_RETURN_VOID();
}

/*
 * vexec.declared_calls(): the functions of this database that bind to a
 * pack's declaration, as a call of each would bind now: in the packs' order
 * and their declarations'.
 */
Datum
vexec_declared_calls(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VexecKernelRegistry *reg = vexec_find_kernel_registry();
	int			p,
				i,
				k;

	InitMaterializedSRF(fcinfo, 0);
	if (reg == NULL)
		PG_RETURN_VOID();
	for (p = 0; p < reg->npacks; p++)
	{
		const VexecKernelPack *pk = reg->packs[p];

		for (i = 0; i < pk->ndecls; i++)
		{
			const VexecKernelDecl *dc = &pk->decls[i];
			CatCList   *list;
			Oid		   *funcids;
			int			n;

			/* the functions of the declaration's name, in any schema */
			list = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(dc->name));
			n = list->n_members;
			funcids = palloc(sizeof(Oid) * Max(n, 1));
			for (k = 0; k < n; k++)
				funcids[k] = ((Form_pg_proc) GETSTRUCT(&list->members[k]->tuple))->oid;
			ReleaseSysCacheList(list);

			for (k = 0; k < n; k++)
			{
				VexecDeclared bound;
				Datum		values[6];
				bool		nulls[6] = {false, false, false, false, false, false};
				Oid			extoid;
				HeapTuple	tup;

				if (!vexec_declared_find(funcids[k], &bound) || bound.decl != dc)
					continue;
				extoid = getExtensionOfObject(ProcedureRelationId, funcids[k]);
				values[0] = ObjectIdGetDatum(funcids[k]);
				values[1] = CStringGetTextDatum(format_procedure(funcids[k]));
				values[2] = CStringGetTextDatum(pk->extension);
				tup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(extoid));
				if (HeapTupleIsValid(tup))
				{
					values[3] = SysCacheGetAttrNotNull(EXTENSIONOID, tup, Anum_pg_extension_extversion);
					values[3] = PointerGetDatum(PG_DETOAST_DATUM_COPY(values[3]));
					ReleaseSysCache(tup);
				}
				else
					nulls[3] = true;
				values[4] = CStringGetTextDatum(pk->name);
				values[5] = CStringGetTextDatum(vexec_decl_kind_name(dc->kind));
				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
			}
		}
	}
	PG_RETURN_VOID();
}
