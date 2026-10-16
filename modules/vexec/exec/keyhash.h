/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * keyhash.h
 *	  How vexec's hash tables hash a key's bytes (pg_vector_executor.md
 *	  §3.19, C4).
 *
 * A key whose bytes decide its equality -- text of a deterministic
 * collation, bytea -- is hashed by CRC32C over its bytes, through
 * PostgreSQL's own implementation, chosen at run time for the CPU
 * (COMP_CRC32C, port/pg_crc32c.h): eight bytes an instruction where the CPU
 * has one.  PostgreSQL's hash_bytes() takes about twice as long on keys of
 * URLs' length (§3.19).  ClickHouse hashes its tables' strings so
 * (CH:base/base/StringViewHash.h).
 *
 * The hash is the node's own: never cdbhash's, nor a hash opclass's, so
 * nothing outside the node's tables reads it.  CRC32C is linear, so the
 * nodes mix the hashes of a row's keys through murmurhash32() before a
 * table takes buckets from them, as they did hash_bytes()'s.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_KEYHASH_H
#define VEXEC_KEYHASH_H

#include "port/pg_crc32c.h"

static inline uint32
vexec_hash_bytes(const char *bytes, int len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, bytes, len);
	FIN_CRC32C(crc);
	return (uint32) crc;
}

#endif							/* VEXEC_KEYHASH_H */
