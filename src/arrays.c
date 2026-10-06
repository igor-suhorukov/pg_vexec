/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * arrays.c
 *	  Arrow arrays the endpoint makes itself, in the C Data Interface's
 *	  structures, for results no statement makes: Flight SQL's server
 *	  information, whose value is a dense union, and its type information,
 *	  which holds lists (arrow/format/FlightSql.proto, CommandGetSqlInfo and
 *	  CommandGetXdbcTypeInfo).  vexec's egress API writes them as IPC
 *	  messages (vexec_egress.h, arrays).
 *
 * Everything is palloc'd in the current memory context; the release
 * callbacks only mark a structure released.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"

#include "arrays.h"

static void
release_schema(struct ArrowSchema *s)
{
	s->release = NULL;
}

static void
release_array(struct ArrowArray *a)
{
	a->release = NULL;
}

/* A column of a format, its buffers and children to follow. */
static ArrowCol *
col_new(const char *name, const char *format, bool nullable, int64 n, int nbuffers, int nchildren)
{
	ArrowCol   *c = palloc0(sizeof(ArrowCol));

	c->schema.format = pstrdup(format);
	c->schema.name = pstrdup(name ? name : "");
	c->schema.flags = nullable ? ARROW_FLAG_NULLABLE : 0;
	c->schema.release = release_schema;
	c->schema.n_children = nchildren;
	c->schema.children = palloc0(sizeof(struct ArrowSchema *) * Max(nchildren, 1));
	c->array.length = n;
	c->array.n_buffers = nbuffers;
	c->array.buffers = (const void **) palloc0(sizeof(void *) * Max(nbuffers, 1));
	c->array.n_children = nchildren;
	c->array.children = palloc0(sizeof(struct ArrowArray *) * Max(nchildren, 1));
	c->array.release = release_array;
	return c;
}

static void
col_child(ArrowCol *c, int i, ArrowCol *child)
{
	c->schema.children[i] = &child->schema;
	c->array.children[i] = &child->array;
}

/* A validity bitmap from nulls[], or NULL with no NULL; the count of them. */
static const void *
validity(const bool *nulls, int64 n, int64 *null_count)
{
	uint8	   *bits;
	int64		i;

	*null_count = 0;
	if (nulls == NULL)
		return NULL;
	for (i = 0; i < n; i++)
		if (nulls[i])
			(*null_count)++;
	if (*null_count == 0)
		return NULL;
	bits = palloc0((n + 7) / 8);
	for (i = 0; i < n; i++)
		if (!nulls[i])
			bits[i / 8] |= 1 << (i % 8);
	return bits;
}

ArrowCol *
arrays_utf8(const char *name, bool nullable, const char *const *values, int64 n)
{
	ArrowCol   *c = col_new(name, "u", nullable, n, 3, 0);
	int32	   *offsets = palloc((n + 1) * sizeof(int32));
	bool	   *nulls = palloc(Max(n, 1) * sizeof(bool));
	StringInfoData data;
	int64		i;

	initStringInfo(&data);
	offsets[0] = 0;
	for (i = 0; i < n; i++)
	{
		nulls[i] = values[i] == NULL;
		if (values[i])
			appendStringInfoString(&data, values[i]);
		offsets[i + 1] = data.len;
	}
	c->array.buffers[0] = validity(nulls, n, &c->array.null_count);
	c->array.buffers[1] = offsets;
	c->array.buffers[2] = data.data;
	return c;
}

static ArrowCol *
fixed(const char *name, const char *format, bool nullable, const void *values, int width,
	  const bool *nulls, int64 n)
{
	ArrowCol   *c = col_new(name, format, nullable, n, 2, 0);
	void	   *copy = palloc0(Max(n * width, 1));

	if (n > 0)
		memcpy(copy, values, n * width);
	c->array.buffers[0] = validity(nulls, n, &c->array.null_count);
	c->array.buffers[1] = copy;
	return c;
}

ArrowCol *
arrays_int32(const char *name, bool nullable, const int32 *values, const bool *nulls, int64 n)
{
	return fixed(name, "i", nullable, values, 4, nulls, n);
}

ArrowCol *
arrays_uint32(const char *name, bool nullable, const uint32 *values, const bool *nulls, int64 n)
{
	return fixed(name, "I", nullable, values, 4, nulls, n);
}

ArrowCol *
arrays_int64(const char *name, bool nullable, const int64 *values, const bool *nulls, int64 n)
{
	return fixed(name, "l", nullable, values, 8, nulls, n);
}

ArrowCol *
arrays_bool(const char *name, bool nullable, const bool *values, const bool *nulls, int64 n)
{
	ArrowCol   *c = col_new(name, "b", nullable, n, 2, 0);
	uint8	   *bits = palloc0(Max((n + 7) / 8, 1));
	int64		i;

	for (i = 0; i < n; i++)
		if (values[i])
			bits[i / 8] |= 1 << (i % 8);
	c->array.buffers[0] = validity(nulls, n, &c->array.null_count);
	c->array.buffers[1] = bits;
	return c;
}

/* list<utf8 not null>: lists[i] of lens[i] strings, or a NULL list. */
ArrowCol *
arrays_list_utf8(const char *name, bool nullable, const char *const *const *lists,
				 const int *lens, int64 n)
{
	ArrowCol   *c = col_new(name, "+l", nullable, n, 2, 1);
	int32	   *offsets = palloc((n + 1) * sizeof(int32));
	bool	   *nulls = palloc(Max(n, 1) * sizeof(bool));
	const char **items;
	int64		nitems = 0;
	int64		i;

	for (i = 0; i < n; i++)
		nitems += lists[i] ? lens[i] : 0;
	items = palloc(Max(nitems, 1) * sizeof(char *));
	nitems = 0;
	offsets[0] = 0;
	for (i = 0; i < n; i++)
	{
		int			j;

		nulls[i] = lists[i] == NULL;
		for (j = 0; lists[i] && j < lens[i]; j++)
			items[nitems++] = lists[i][j];
		offsets[i + 1] = (int32) nitems;
	}
	c->array.buffers[0] = validity(nulls, n, &c->array.null_count);
	c->array.buffers[1] = offsets;
	col_child(c, 0, arrays_utf8("item", false, items, nitems));
	return c;
}

/* An empty list<int32>, for a type a union must name. */
static ArrowCol *
empty_list_int32(const char *name, bool nullable)
{
	ArrowCol   *c = col_new(name, "+l", nullable, 0, 2, 1);

	c->array.buffers[1] = palloc0(sizeof(int32));
	col_child(c, 0, fixed("item", "i", false, NULL, 4, NULL, 0));
	return c;
}

/* An empty map<int32, list<int32>>, as Flight SQL's server information names it. */
ArrowCol *
arrays_empty_map_int32_list(const char *name, bool nullable)
{
	ArrowCol   *c = col_new(name, "+m", nullable, 0, 2, 1);
	ArrowCol   *entries = col_new("entries", "+s", false, 0, 1, 2);

	c->array.buffers[1] = palloc0(sizeof(int32));
	col_child(entries, 0, fixed("key", "i", false, NULL, 4, NULL, 0));
	col_child(entries, 1, empty_list_int32("value", true));
	col_child(c, 0, entries);
	return c;
}

/* A dense union of children, type ids 0 to nchildren - 1, a type id and an offset a row. */
ArrowCol *
arrays_dense_union(const char *name, ArrowCol **children, int nchildren,
				   const int8 *type_ids, const int32 *offsets, int64 n)
{
	StringInfoData format;
	ArrowCol   *c;
	int			i;

	initStringInfo(&format);
	appendStringInfoString(&format, "+ud:");
	for (i = 0; i < nchildren; i++)
		appendStringInfo(&format, "%s%d", i ? "," : "", i);
	c = col_new(name, format.data, false, n, 2, nchildren);
	c->array.buffers[0] = memcpy(palloc(Max(n, 1)), type_ids, n);
	c->array.buffers[1] = memcpy(palloc(Max(n * 4, 1)), offsets, n * 4);
	for (i = 0; i < nchildren; i++)
		col_child(c, i, children[i]);
	return c;
}

/* A record batch: a struct of the columns, n rows each. */
void
arrays_batch(ArrowCol **cols, int ncols, int64 n, struct ArrowSchema *schema, struct ArrowArray *array)
{
	ArrowCol   *root = col_new("", "+s", false, n, 1, ncols);
	int			i;

	for (i = 0; i < ncols; i++)
		col_child(root, i, cols[i]);
	*schema = root->schema;
	*array = root->array;
}
