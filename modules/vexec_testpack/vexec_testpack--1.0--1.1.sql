-- SPDX-License-Identifier: Apache-2.0
--
-- 1.1 changes nothing but the extension's version, which its pack does not
-- name: ALTER EXTENSION vexec_testpack UPDATE TO '1.1' unbinds every
-- declaration, in every session that had them (vexec's suite, vexec_packs).

\echo Use "ALTER EXTENSION vexec_testpack UPDATE TO '1.1'" to load this file. \quit

COMMENT ON EXTENSION vexec_testpack IS 'A kernel pack of vexec''s tests, at a version its pack does not name';
