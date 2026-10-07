/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_test.h
 *	  What vexec_test.c's checks share with ipc_test.c's: a query's rows a
 *	  batch at a time, the batch settings by name, and the check of views.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_TEST_H
#define VEXEC_TEST_H

#include "fmgr.h"
#include "utils/array.h"
#include "utils/portal.h"

#include "batch/arrow_abi.h"
#include "batch/batch.h"

/* A query's rows, a batch at a time, through an SPI cursor. */
typedef struct Rows
{
	Portal		portal;
	TupleDesc	desc;
	int			natts;
	VexecType **types;
	Datum	   *values;			/* [row * natts + col] */
	bool	   *nulls;
	int			nrows;
} Rows;

extern void rows_open(Rows *rows, const char *query, ArrayType *typmods);
extern bool rows_next(Rows *rows);
extern void rows_fill(Rows *rows, VexecBatch *batch);

/* The batch settings' values, in their enums' order. */
extern const char *const format_names[2];
extern const char *const varlena_names[4];
extern const char *const bool_names[3];
extern const char *const temporal_names[3];
extern const char *const numeric_names[3];
extern int	setting_value(const char *name, const char *value, const char *const *names, int n);

/* A configuration from five text arguments from "arg" on: format, varlena, bool, temporal, numeric. */
extern void layout_config(FunctionCallInfo fcinfo, int arg, VexecLayoutConfig *cfg);

extern void check_views(struct ArrowArray *a, int col);

#endif							/* VEXEC_TEST_H */
