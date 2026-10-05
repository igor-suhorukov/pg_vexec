/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * export.c
 *	  A batch as an Arrow record batch, through the C Data Interface
 *	  (pg_vector_executor.md §3.4.2, "Arrow export").
 *
 * The export points into the batch wherever the column's bytes already are
 * what Arrow's type means, and converts the rest into memory of its own:
 *
 *	the Arrow format	as it is, in Arrow's standard types, except where a
 *						column holds what Arrow's type cannot mean -- a
 *						batch kept in PostgreSQL's layout, ±infinity, and
 *						time's 24:00:00, which PostgreSQL allows
 *						(PG19:src/backend/utils/adt/date.c:1530-1548) and
 *						time64 does not (Schema.fbs:266-268).  Such a column
 *						goes under postgresql.*, with PostgreSQL's values.
 *	the PostgreSQL		the shared layouts as they are; bools as arrow.bool8;
 *	format				temporal values under postgresql.* over int32 and
 *						int64; views built over the varlena bytes.  A
 *						consumer that needs Arrow's standard types gets a
 *						converted copy from a batch in the Arrow format.
 *
 * Where the Arrow format keeps a PostgreSQL type, or a value Arrow's type
 * cannot mean, the column carries the extension name postgresql.<type> over
 * Arrow storage, with the type's OID and typmod in its metadata
 * (Columnar.rst:1688-1704); a consumer that does not know the name sees the
 * storage type.  Every column's metadata also names its PostgreSQL type
 * under "pg_type", as PG-Strom's Arrow files do
 * (pg-strom/src/arrow_fdw.c:273-330), its typmod, collation and layout.
 *
 * Ownership follows the interface (CDataInterface.rst:577-704): the caller
 * owns the two base structures; everything they point to is the export's,
 * in a memory context of its own under the caller's current one, freed by
 * the last release, children's included, so that a consumer may move a
 * child out.  The caller releases the export before that context, or the
 * batch, is reset or deleted: the buffers the export points into are the
 * batch's.  The base structures are given their release callbacks last, so
 * that an error part way leaves them released, and the export's memory to
 * its parent context.  The export compacts the batch's selection first,
 * since Arrow has none.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/arrow_abi.h"
#include "batch/batch.h"
#include "batch/export.h"

/*
 * What every structure of one export shares: its memory, a context of its
 * own, and how many of its structures are not yet released.  It lives in
 * that context, which the last release deletes.
 */
typedef struct ExportPrivate
{
	MemoryContext ecxt;
	int			refs;
} ExportPrivate;

typedef struct Export
{
	VexecBatch *batch;
	MemoryContext ecxt;
	ExportPrivate *priv;
} Export;

static void
export_unref(ExportPrivate *priv)
{
	if (--priv->refs == 0)
		MemoryContextDelete(priv->ecxt);	/* priv with it */
}

/*
 * A release releases the structure's children and dictionary that a
 * consumer has not moved out, then the structure; the last one frees the
 * export's memory.
 */
static void
release_schema(struct ArrowSchema *s)
{
	ExportPrivate *priv = s->private_data;
	int64_t		i;

	if (s->release == NULL)
		return;
	for (i = 0; i < s->n_children; i++)
		if (s->children[i]->release)
			s->children[i]->release(s->children[i]);
	if (s->dictionary && s->dictionary->release)
		s->dictionary->release(s->dictionary);
	s->release = NULL;
	export_unref(priv);
}

static void
release_array(struct ArrowArray *a)
{
	ExportPrivate *priv = a->private_data;
	int64_t		i;

	if (a->release == NULL)
		return;
	for (i = 0; i < a->n_children; i++)
		if (a->children[i]->release)
			a->children[i]->release(a->children[i]);
	if (a->dictionary && a->dictionary->release)
		a->dictionary->release(a->dictionary);
	a->release = NULL;
	export_unref(priv);
}

static void *
ealloc0(Export *ex, Size size)
{
	return MemoryContextAllocZero(ex->ecxt, Max(size, 1));
}

static void *
ealloc_aligned(Export *ex, Size size)
{
	return MemoryContextAllocAligned(ex->ecxt, Max(size, 1), VEXEC_ALIGN,
									 MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
}

static char *
estrdup(Export *ex, const char *s)
{
	return MemoryContextStrdup(ex->ecxt, s);
}

static void
schema_init(Export *ex, struct ArrowSchema *s, const char *format, const char *name)
{
	memset(s, 0, sizeof(*s));
	s->format = estrdup(ex, format);
	s->name = estrdup(ex, name ? name : "");
	s->flags = ARROW_FLAG_NULLABLE;
	s->release = release_schema;
	s->private_data = ex->priv;
	ex->priv->refs++;
}

static void
array_init(Export *ex, struct ArrowArray *a, int64 length, int64 null_count, int nbuffers)
{
	memset(a, 0, sizeof(*a));
	a->length = length;
	a->null_count = null_count;
	a->offset = 0;
	a->n_buffers = nbuffers;
	a->buffers = ealloc0(ex, sizeof(void *) * Max(nbuffers, 1));
	a->release = release_array;
	a->private_data = ex->priv;
	ex->priv->refs++;
}

/*
 * Field metadata in the interface's encoding (CDataInterface.rst:353-383):
 * int32 N, then N times int32 key length, key, int32 value length, value,
 * in native byte order; NULL when there is none.
 */
typedef struct Meta
{
	int			n;
	const char *keys[8];
	const char *values[8];
} Meta;

static void
meta_add(Meta *m, const char *key, const char *value)
{
	Assert(m->n < 8);
	m->keys[m->n] = key;
	m->values[m->n] = value;
	m->n++;
}

static const char *
meta_encode(Export *ex, const Meta *m)
{
	Size		len = sizeof(int32);
	char	   *buf;
	char	   *p;
	int32		n = m->n;
	int			i;

	if (m->n == 0)
		return NULL;
	for (i = 0; i < m->n; i++)
		len += 2 * sizeof(int32) + strlen(m->keys[i]) + strlen(m->values[i]);
	buf = ealloc0(ex, len);
	p = buf;
	memcpy(p, &n, sizeof(int32));
	p += sizeof(int32);
	for (i = 0; i < m->n; i++)
	{
		int32		kl = (int32) strlen(m->keys[i]);
		int32		vl = (int32) strlen(m->values[i]);

		memcpy(p, &kl, sizeof(int32));
		p += sizeof(int32);
		memcpy(p, m->keys[i], kl);
		p += kl;
		memcpy(p, &vl, sizeof(int32));
		p += sizeof(int32);
		memcpy(p, m->values[i], vl);
		p += vl;
	}
	return buf;
}

/* How one column goes out: Arrow's format string and an extension name. */
typedef struct Target
{
	const char *format;
	const char *extension;		/* NULL: none */
	const char *ext_metadata;	/* the extension's own metadata */
} Target;

static const char *
pg_extension_name(Export *ex, const VexecType *type)
{
	return MemoryContextStrdup(ex->ecxt, psprintf("postgresql.%s", type->name));
}

static const char *
pg_extension_metadata(Export *ex, const VexecType *type)
{
	return MemoryContextStrdup(ex->ecxt,
							   psprintf("{\"typoid\":%u,\"typmod\":%d}", type->typid, type->typmod));
}

/* Arrow's integer of a width, for a by-value type that has no type of its own. */
static const char *
int_format(int width)
{
	switch (width)
	{
		case 1:
			return "c";
		case 2:
			return "s";
		case 4:
			return "i";
		default:
			return "l";
	}
}

/* any valid value equal to one of two sentinels */
static bool
has_value32(const VexecVec *v, int n, int32 a, int32 b)
{
	const int32 *x = v->values;
	int			i;

	for (i = 0; i < n; i++)
		if ((v->validity == NULL || vexec_bit(v->validity, i)) && (x[i] == a || x[i] == b))
			return true;
	return false;
}

static bool
has_value64(const VexecVec *v, int n, int64 a, int64 b)
{
	const int64 *x = v->values;
	int			i;

	for (i = 0; i < n; i++)
		if ((v->validity == NULL || vexec_bit(v->validity, i)) && (x[i] == a || x[i] == b))
			return true;
	return false;
}

/*
 * Views over a DATUM column's values, built at export: into the arena's
 * chunks where the bytes are, into a buffer of the export's own where they
 * are not (detoasted values, values a source keeps elsewhere).  The buffers
 * go into *bufs as the interface lays them out after the views.
 */
static void
views_over_datums(Export *ex, const VexecVec *v, int n,
				  VexecView **views_out, const void ***data_out, int64 **sizes_out, int *ndata_out)
{
	VexecBatch *batch = ex->batch;
	VexecView  *views = ealloc_aligned(ex, sizeof(VexecView) * Max(n, 1));
	const Datum *datums = v->values;
	int			nchunks = batch->arena.nchunks;
	StringInfoData extra;
	int			i;

	initStringInfoExt(&extra, 1024);
	for (i = 0; i < n; i++)
	{
		const char *p;
		Size		len;
		int			chunk;
		int32		offset;
		VexecView  *view = &views[i];

		if (v->validity != NULL && !vexec_bit(v->validity, i))
			continue;
		if (v->type->typlen == -2)
		{
			p = DatumGetCString(datums[i]);
			len = strlen(p);
		}
		else
		{
			varlena    *vl = (varlena *) DatumGetPointer(datums[i]);

			if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
			{
				MemoryContext old = MemoryContextSwitchTo(ex->ecxt);

				vl = detoast_attr(vl);
				MemoryContextSwitchTo(old);
			}
			p = VARDATA_ANY(vl);
			len = VARSIZE_ANY_EXHDR(vl);
		}
		view->inlined.size = (int32) len;
		if (len <= VEXEC_VIEW_INLINE)
		{
			memcpy(view->inlined.data, p, len);
			continue;
		}
		memcpy(view->ref.prefix, p, 4);
		if (vexec_arena_find(&batch->arena, p, len, &chunk, &offset))
		{
			view->ref.buffer_index = chunk;
			view->ref.offset = offset;
		}
		else
		{
			view->ref.buffer_index = nchunks;	/* the export's own buffer */
			view->ref.offset = extra.len;
			appendBinaryStringInfo(&extra, p, (int) len);
		}
	}

	*ndata_out = nchunks + (extra.len > 0 ? 1 : 0);
	*data_out = ealloc0(ex, sizeof(void *) * Max(*ndata_out, 1));
	*sizes_out = ealloc0(ex, sizeof(int64) * Max(*ndata_out, 1));
	for (i = 0; i < nchunks; i++)
	{
		(*data_out)[i] = batch->arena.chunks[i].data;
		(*sizes_out)[i] = batch->arena.chunks[i].used;
	}
	if (extra.len > 0)
	{
		char	   *own = ealloc_aligned(ex, extra.len);

		memcpy(own, extra.data, extra.len);
		(*data_out)[nchunks] = own;
		(*sizes_out)[nchunks] = extra.len;
	}
	pfree(extra.data);
	*views_out = views;
}

/* The target for a type whose values are held as varlena bytes. */
static Target
varlena_target(Export *ex, const VexecType *type, bool view)
{
	Target		t = {NULL, NULL, NULL};
	bool		utf8 = type->utf8;

	if (utf8)
		t.format = view ? "vu" : "u";
	else
		t.format = view ? "vz" : "z";
	if (type->basetype == JSONOID && utf8)
	{
		t.extension = "arrow.json";
		t.ext_metadata = "";
	}
	else if (type->basetype == BYTEAOID || (utf8 && type->basetype != JSONOID))
		;						/* binary, utf8: Arrow's types mean these */
	else
	{
		t.extension = pg_extension_name(ex, type);
		t.ext_metadata = pg_extension_metadata(ex, type);
	}
	return t;
}

/*
 * One column's schema and array, for a flat column's n values; a
 * dictionary column is its codes, with its dictionary exported the same
 * way.
 */
static void
export_vec(Export *ex, const VexecVec *v, int n, const char *name,
		   struct ArrowSchema *cs, struct ArrowArray *ca)
{
	const VexecType *type = v->type;
	VexecVec	col = *v;
	Target		t = {NULL, NULL, NULL};
	int64		null_count;
	Meta		meta = {0};
	const void *values = col.values;
	char	   *layout_name;

	if (col.encoding == VEXEC_CONST)
		vexec_vec_flatten(ex->batch, &col);
	null_count = col.validity ? vexec_vec_null_count(ex->batch, &col) : 0;
	values = col.values;
	layout_name = vexec_shape_name(type, &col.shape);

	meta_add(&meta, "pg_type", estrdup(ex, psprintf("%s.%s", type->nspname, type->name)));
	meta_add(&meta, "pg_typmod", estrdup(ex, psprintf("%d", type->typmod)));
	if (OidIsValid(type->collation))
	{
		char	   *coll = get_collation_name(type->collation);

		if (coll)
			meta_add(&meta, "pg_collation", estrdup(ex, coll));
	}
	meta_add(&meta, "vexec:layout",
			 estrdup(ex, col.encoding == VEXEC_DICT ? psprintf("dictionary of %s", layout_name)
					 : layout_name));

	if (col.encoding == VEXEC_DICT)
	{
		/* int32 codes; the value type, extension and all, on the dictionary */
		schema_init(ex, cs, "i", name);
		cs->metadata = meta_encode(ex, &meta);
		array_init(ex, ca, n, null_count, 2);
		ca->buffers[0] = null_count ? col.validity : NULL;
		ca->buffers[1] = col.codes;
		cs->dictionary = ealloc0(ex, sizeof(struct ArrowSchema));
		ca->dictionary = ealloc0(ex, sizeof(struct ArrowArray));
		export_vec(ex, col.dictionary, col.dictionary->nvalues, "", cs->dictionary, ca->dictionary);
		return;
	}

	switch (col.shape.layout)
	{
		case VEXEC_BYTE_BOOL:
			t.format = "c";
			t.extension = "arrow.bool8";
			t.ext_metadata = "";
			break;
		case VEXEC_BIT_BOOL:
			t.format = "b";
			break;
		case VEXEC_SCALED:
			t.format = estrdup(ex, col.shape.width == 8
							   ? psprintf("d:%d,%d,64", type->digits, col.shape.scale)
							   : psprintf("d:%d,%d", type->digits, col.shape.scale));
			break;
		case VEXEC_FIXED:
			switch (type->tclass)
			{
				case VEXEC_TC_DATE:
					if (col.shape.arrow_values &&
						!has_value32(&col, n, DATEVAL_NOBEGIN, DATEVAL_NOEND))
						t.format = "tdD";
					else
					{
						t.format = "i";
						t.extension = pg_extension_name(ex, type);
						t.ext_metadata = pg_extension_metadata(ex, type);
						if (col.shape.arrow_values)
						{
							/* ±infinity: PostgreSQL's values, a copy */
							int32	   *out = ealloc_aligned(ex, sizeof(int32) * n);
							int			i;

							for (i = 0; i < n; i++)
							{
								int32		d = ((const int32 *) col.values)[i];

								out[i] = DATE_NOT_FINITE(d) ? d : d - (int32) VEXEC_EPOCH_DAYS;
							}
							values = out;
						}
					}
					break;
				case VEXEC_TC_TIMESTAMP:
					if (col.shape.arrow_values &&
						!has_value64(&col, n, DT_NOBEGIN, DT_NOEND))
						t.format = type->basetype == TIMESTAMPTZOID ? "tsu:UTC" : "tsu:";
					else
					{
						t.format = "l";
						t.extension = pg_extension_name(ex, type);
						t.ext_metadata = pg_extension_metadata(ex, type);
						if (col.shape.arrow_values)
						{
							int64	   *out = ealloc_aligned(ex, sizeof(int64) * n);
							int			i;

							for (i = 0; i < n; i++)
							{
								int64		ts = ((const int64 *) col.values)[i];

								out[i] = TIMESTAMP_NOT_FINITE(ts) ? ts : ts - VEXEC_EPOCH_USECS;
							}
							values = out;
						}
					}
					break;
				case VEXEC_TC_INTERVAL:
					if (col.shape.arrow_values)
						t.format = "tin";
					else
					{
						t.format = "w:16";
						t.extension = pg_extension_name(ex, type);
						t.ext_metadata = pg_extension_metadata(ex, type);
					}
					break;
				case VEXEC_TC_BYREF:
					t.format = estrdup(ex, psprintf("w:%d", type->typlen));
					if (type->basetype == UUIDOID)
					{
						t.extension = "arrow.uuid";
						t.ext_metadata = "";
					}
					else
					{
						t.extension = pg_extension_name(ex, type);
						t.ext_metadata = pg_extension_metadata(ex, type);
					}
					if (col.shape.stride != type->typlen)
					{
						/* PostgreSQL's array stride: packed for Arrow, a copy */
						char	   *out = ealloc_aligned(ex, (Size) type->typlen * n);
						int			i;

						for (i = 0; i < n; i++)
							memcpy(out + (Size) i * type->typlen,
								   (const char *) col.values + (Size) i * col.shape.stride,
								   type->typlen);
						values = out;
					}
					break;
				default:		/* by value */
					switch (type->basetype)
					{
						case INT2OID:
							t.format = "s";
							break;
						case INT4OID:
							t.format = "i";
							break;
						case INT8OID:
							t.format = "l";
							break;
						case FLOAT4OID:
							t.format = "f";
							break;
						case FLOAT8OID:
							t.format = "g";
							break;
						case OIDOID:
							t.format = "I";
							break;
						case TIMEOID:
							/* 24:00:00 is PostgreSQL's alone */
							if (has_value64(&col, n, USECS_PER_DAY, USECS_PER_DAY))
							{
								t.format = "l";
								t.extension = pg_extension_name(ex, type);
								t.ext_metadata = pg_extension_metadata(ex, type);
							}
							else
								t.format = "ttu";
							break;
						default:
							t.format = int_format(type->typlen);
							t.extension = pg_extension_name(ex, type);
							t.ext_metadata = pg_extension_metadata(ex, type);
							break;
					}
					break;
			}
			break;
		case VEXEC_DATUM:
			{
				VexecView  *views;
				const void **data;
				int64	   *sizes;
				int			ndata;
				int			i;

				t = varlena_target(ex, type, true);
				views_over_datums(ex, &col, n, &views, &data, &sizes, &ndata);
				schema_init(ex, cs, t.format, name);
				array_init(ex, ca, n, null_count, ndata + 3);
				ca->buffers[0] = null_count ? col.validity : NULL;
				ca->buffers[1] = views;
				for (i = 0; i < ndata; i++)
					ca->buffers[2 + i] = data[i];
				ca->buffers[2 + ndata] = sizes;
				goto finish;
			}
		case VEXEC_VIEW:
			{
				int			i;
				int64	   *sizes;

				t = varlena_target(ex, type, true);
				schema_init(ex, cs, t.format, name);
				array_init(ex, ca, n, null_count, col.nbuffers + 3);
				ca->buffers[0] = null_count ? col.validity : NULL;
				ca->buffers[1] = col.values;
				sizes = ealloc0(ex, sizeof(int64) * Max(col.nbuffers, 1));
				for (i = 0; i < col.nbuffers; i++)
				{
					ca->buffers[2 + i] = col.buffers[i];
					sizes[i] = col.buffer_sizes[i];
				}
				ca->buffers[2 + col.nbuffers] = sizes;
				goto finish;
			}
		case VEXEC_OFFSETS:
			t = varlena_target(ex, type, false);
			schema_init(ex, cs, t.format, name);
			array_init(ex, ca, n, null_count, 3);
			ca->buffers[0] = null_count ? col.validity : NULL;
			ca->buffers[1] = col.values;
			ca->buffers[2] = col.buffers[0];
			goto finish;
	}

	/* the layouts of one data buffer: validity, values */
	schema_init(ex, cs, t.format, name);
	array_init(ex, ca, n, null_count, 2);
	ca->buffers[0] = null_count ? col.validity : NULL;
	ca->buffers[1] = values;

finish:
	if (t.extension)
	{
		meta_add(&meta, "ARROW:extension:name", t.extension);
		meta_add(&meta, "ARROW:extension:metadata", t.ext_metadata ? t.ext_metadata : "");
	}
	cs->metadata = meta_encode(ex, &meta);
}

/*
 * Export a batch as a record batch: a struct array whose children are its
 * columns, named by names[] (or c1, c2, ...).
 */
void
vexec_batch_export(VexecBatch *batch, const char *const *names,
				   struct ArrowSchema *schema, struct ArrowArray *array)
{
	Export		ex;
	int			i;

	memset(schema, 0, sizeof(*schema));
	memset(array, 0, sizeof(*array));
	vexec_batch_compact(batch);

	ex.batch = batch;
	ex.ecxt = AllocSetContextCreate(CurrentMemoryContext, "vexec export", ALLOCSET_DEFAULT_SIZES);
	ex.priv = MemoryContextAllocZero(ex.ecxt, sizeof(ExportPrivate));
	ex.priv->ecxt = ex.ecxt;

	schema->format = estrdup(&ex, "+s");
	schema->name = estrdup(&ex, "");
	schema->n_children = batch->ncols;
	schema->children = ealloc0(&ex, sizeof(struct ArrowSchema *) * Max(batch->ncols, 1));

	array->length = batch->nrows;
	array->n_buffers = 1;
	array->buffers = ealloc0(&ex, sizeof(void *));
	array->n_children = batch->ncols;
	array->children = ealloc0(&ex, sizeof(struct ArrowArray *) * Max(batch->ncols, 1));

	for (i = 0; i < batch->ncols; i++)
	{
		char		defname[32];

		schema->children[i] = ealloc0(&ex, sizeof(struct ArrowSchema));
		array->children[i] = ealloc0(&ex, sizeof(struct ArrowArray));
		snprintf(defname, sizeof(defname), "c%d", i + 1);
		export_vec(&ex, &batch->cols[i], batch->nrows,
				   names && names[i] ? names[i] : defname,
				   schema->children[i], array->children[i]);
	}

	/* every child is made: the base structures are live */
	schema->private_data = ex.priv;
	array->private_data = ex.priv;
	ex.priv->refs += 2;
	schema->release = release_schema;
	array->release = release_array;
}
