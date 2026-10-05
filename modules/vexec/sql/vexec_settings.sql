-- SPDX-License-Identifier: Apache-2.0
--
-- vexec's settings (pg_vector_executor.md §3.12): every one, its default and
-- its context; the values each takes; the prefix reserved; and the
-- planner's among those EXPLAIN (SETTINGS) shows.  GPU execution is off at
-- every level by default (§3.11).

SELECT name,
       CASE WHEN name = 'vexec.debug_check_plans' THEN '(on where assertions are)' ELSE setting END AS setting,
       unit, context, vartype, min_val, max_val, enumvals,
       (pg_settings_get_flags(name) @> ARRAY['EXPLAIN']) AS explain
FROM pg_settings WHERE name LIKE 'vexec.%' ORDER BY name;

-- the modes
SET vexec.mode = explain;
SHOW vexec.mode;
SET vexec.mode = auto;
SET vexec.mode = force;
SET vexec.mode = bogus;
RESET vexec.mode;
SHOW vexec.mode;

-- the formats, and one structure at a time
SET vexec.batch_format = arrow;
SET vexec.batch_varlena_layout = offsets;
SET vexec.batch_bool_layout = byte;
SET vexec.batch_temporal_layout = postgres;
SET vexec.batch_numeric_layout = varlena;
SELECT current_setting('vexec.batch_format') AS format,
       current_setting('vexec.batch_varlena_layout') AS varlena,
       current_setting('vexec.batch_bool_layout') AS bool,
       current_setting('vexec.batch_temporal_layout') AS temporal,
       current_setting('vexec.batch_numeric_layout') AS numeric;
SET vexec.batch_varlena_layout = arrow;
SET vexec.batch_bool_layout = view;
RESET ALL;

-- factors, and their bounds
SET vexec.cpu_tuple_factor = 0.5;
SET vexec.cpu_operator_factor = -1;
SET vexec.compact_threshold = 1.5;
RESET ALL;

-- the prefix is vexec's: a name it does not define is refused
SET vexec.no_such_setting = 1;

-- the GPU: off by default; a server's devices are its own
SHOW vexec.gpu;
SHOW vexec.gpu_devices;
SET vexec.gpu = auto;
RESET vexec.gpu;
SET vexec.gpu_devices = '0';
SET vexec.gpu_segments = on;

-- the debug settings are a superuser's
SET vexec.debug_check_plans = on;
SET vexec.debug_require_vector = off;
CREATE ROLE regress_vexec_settings;
SET ROLE regress_vexec_settings;
SET vexec.debug_check_plans = on;
SET vexec.mode = explain;
RESET ROLE;
DROP ROLE regress_vexec_settings;
RESET ALL;

-- EXPLAIN (SETTINGS) shows the planner's settings that differ from their
-- defaults, as it shows enable_*
CREATE FUNCTION explain_lines(query text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	line text;
BEGIN
	FOR line IN EXECUTE query LOOP
		RETURN NEXT line;
	END LOOP;
END
$$;
SET vexec.mode = explain;
SET vexec.batch_format = arrow;
SELECT l FROM explain_lines('EXPLAIN (SETTINGS, COSTS OFF) SELECT 1') l
WHERE l LIKE 'Settings:%';
RESET ALL;
DROP FUNCTION explain_lines(text);
