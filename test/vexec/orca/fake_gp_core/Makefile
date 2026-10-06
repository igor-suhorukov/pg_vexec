# SPDX-License-Identifier: Apache-2.0
#
# vexec_fake_gp_core, V6's tests' alone (vexec_fake_gp_core.c), built by
# PGXS against the server it runs on; cb_module.h is vexec's copy of the
# port's.

MODULES = vexec_fake_gp_core
PGFILEDESC = "vexec_fake_gp_core - gp_core's rendezvous, for V6's tests"

PG_CPPFLAGS = -I$(VEXEC_PGXS_INCLUDE)

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
