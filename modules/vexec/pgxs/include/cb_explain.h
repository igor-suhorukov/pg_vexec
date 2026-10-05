/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * cb_explain.h
 *	  EXPLAIN's names for the port's custom scan nodes.
 *
 * PostgreSQL 19's EXPLAIN prints a custom scan node as "Custom Scan (name)".
 * Cloudberry's users know the port's nodes by the names its EXPLAIN gives
 * them -- "Gather Motion 3:1  (slice1; segments: 3)", "Assert", "Dynamic Seq
 * Scan on t" -- and its expected test outputs are written in those names.
 *
 * In text format ExplainNode() ends a node's first line before it calls the
 * node's ExplainCustomScan, and before it prints any of its children, so the
 * callback can rename the node there: CbExplainRelabel() replaces the last
 * "Custom Scan (name)" of es->str -- the node's own, since what follows it
 * is the node's own detail -- with the label, and puts the suffix after the
 * scan's target, before its costs, its actual numbers or "never executed".
 * Other formats keep "Custom Scan" and the provider's name, as they did.
 *
 * Header only, so that gp_core and gp_orca each have it.  A core patch, O4,
 * gave ExplainNode() a hook for this until 2026-09-28.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_EXPLAIN_H
#define CB_EXPLAIN_H

#include "commands/explain_state.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"

static inline void
CbExplainRelabel(CustomScanState *node, ExplainState *es,
				 const char *label, const char *suffix)
{
	static const char *const ends[] = {"  (cost=", " (actual ", " (never executed)"};
	StringInfo	str = es->str;
	StringInfoData buf;
	char	   *custom;
	char	   *line = NULL;
	char	   *eol;
	size_t		start;
	size_t		after;
	size_t		at;

	if (es->format != EXPLAIN_FORMAT_TEXT || label == NULL)
		return;

	custom = psprintf("Custom Scan (%s)", node->methods->CustomName);
	for (char *p = str->data; (p = strstr(p, custom)) != NULL; p++)
		line = p;
	if (line == NULL)
	{
		pfree(custom);
		return;
	}
	start = line - str->data;
	after = start + strlen(custom);
	pfree(custom);

	/* the suffix goes where the first line's costs, or the line, begin */
	eol = strchr(str->data + after, '\n');
	at = eol != NULL ? (size_t) (eol - str->data) : (size_t) str->len;
	for (int i = 0; i < lengthof(ends); i++)
	{
		char	   *p = strstr(str->data + after, ends[i]);

		if (p != NULL && (size_t) (p - str->data) < at)
			at = p - str->data;
	}

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, str->data, start);
	appendStringInfoString(&buf, label);
	appendBinaryStringInfo(&buf, str->data + after, at - after);
	if (suffix != NULL)
		appendStringInfoString(&buf, suffix);
	appendBinaryStringInfo(&buf, str->data + at, str->len - at);
	resetStringInfo(str);
	appendBinaryStringInfo(str, buf.data, buf.len);
	pfree(buf.data);
}

/*
 * The same, with the label and suffix a function of the port's gives the
 * node, where it gives one: gp_orca's nodes name themselves so.
 */
typedef bool (*CbExplainLabelFunc) (PlanState *planstate, ExplainState *es,
									const char **pname, const char **suffix);

static inline void
CbExplainRelabelBy(CustomScanState *node, ExplainState *es,
				   CbExplainLabelFunc label)
{
	const char *pname = NULL;
	const char *suffix = NULL;

	if (label(&node->ss.ps, es, &pname, &suffix) && pname != NULL)
		CbExplainRelabel(node, es, pname, suffix);
}

#endif							/* CB_EXPLAIN_H */
