-- SPDX-License-Identifier: Apache-2.0
--
-- vexec_testpack's functions (vexec_testpack.c says what each does, and what
-- its pack declares of it), for vexec's own regression suite (VK).  Each is
-- a member of the extension, created from its library under the symbol its
-- declaration names -- but tp_alias, created under one of its own -- so
-- that a pack's declarations bind to them.

\echo Use "CREATE EXTENSION vexec_testpack" to load this file. \quit

CREATE FUNCTION tp_mix(int8) RETURNS int8
AS 'MODULE_PATHNAME', 'tp_mix' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- an overload, whose signature no declaration has
CREATE FUNCTION tp_mix(int4) RETURNS int8
AS 'MODULE_PATHNAME', 'tp_mix4' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- the declaration of this name names tp_mix's symbol
CREATE FUNCTION tp_alias(int8) RETURNS int8
AS 'MODULE_PATHNAME', 'tp_alias' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tp_div(int8, int8) RETURNS int8
AS 'MODULE_PATHNAME', 'tp_div' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tp_le(int8, int8) RETURNS bool
AS 'MODULE_PATHNAME', 'tp_le' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tp_bytes(bytea) RETURNS int4
AS 'MODULE_PATHNAME', 'tp_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- declared nowhere
CREATE FUNCTION tp_lazy(int8) RETURNS int8
AS 'MODULE_PATHNAME', 'tp_lazy' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- declared nowhere: raises where it is called in its own fn_mcxt
CREATE FUNCTION tp_context(int8) RETURNS int8
AS 'MODULE_PATHNAME', 'tp_context' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- operators over two of them, which compile to the same calls
CREATE OPERATOR #/# (LEFTARG = int8, RIGHTARG = int8, FUNCTION = tp_div);
CREATE OPERATOR #<=# (LEFTARG = int8, RIGHTARG = int8, FUNCTION = tp_le);
