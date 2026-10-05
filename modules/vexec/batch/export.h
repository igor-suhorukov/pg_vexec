/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * export.h
 *	  A batch as an Arrow record batch, through the C Data Interface.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_BATCH_EXPORT_H
#define VEXEC_BATCH_EXPORT_H

#include "batch/arrow_abi.h"
#include "batch/batch.h"

extern VEXEC_API void vexec_batch_export(VexecBatch *batch, const char *const *names,
										 struct ArrowSchema *schema,
										 struct ArrowArray *array);

#endif							/* VEXEC_BATCH_EXPORT_H */
