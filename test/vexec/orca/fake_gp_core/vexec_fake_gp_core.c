/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vexec_fake_gp_core: a module of V6's tests alone (test/vexec/orca.sh).
 * Preloaded, it publishes gp_core's rendezvous variable, as gp_core's
 * _PG_init does, with nothing behind it: what gp_orca's single-node build
 * looks at to refuse a server where gp_core is loaded, before it or after
 * it.  Nothing reads what it publishes but that check, since the server
 * does not start.
 */
#include "postgres.h"

#include "fmgr.h"

#include "cb_module.h"

PG_MODULE_MAGIC;

static int	fake_api;

void
_PG_init(void)
{
	*find_rendezvous_variable(CB_CORE_RENDEZVOUS) = &fake_api;
}
