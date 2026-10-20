/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_regex.c
 *	  A kernel for regexp_replace(text, text, text) with a constant pattern
 *	  and replacement, for the patterns whose answer under PostgreSQL's
 *	  engine a greedy matcher gives (pg_vector_executor.md §3.14, H10).
 *
 * PostgreSQL's regular expressions are Henry Spencer's ARE engine
 * (PG19:src/backend/regex/), whose answers are not Perl's: the whole match
 * is the longest, or the shortest, the RE's first quantifier says, and only
 * then is each subexpression's part of it chosen, the earlier ones first,
 * each as its own quantifier is greedy or not.  A greedy matcher that
 * backtracks finds the same match where nothing can make it differ:
 *
 *	the whole match is fixed	the pattern is anchored at both ends, ^
 *								first and $ last, so that a match is the
 *								whole value or none, as both engines see it
 *								($ ends the value only, the matching being
 *								newline-insensitive, as regexp_replace()'s
 *								default is);
 *	no alternation				no |, so that each part's choice is one of
 *								lengths, not of branches;
 *	every quantifier greedy		?, * and + -- none of *?, +?, {m,n} --
 *								and the earlier atom takes the longest part
 *								that leaves the rest a match, as Spencer's
 *								rule for subexpressions takes it.
 *
 * The items: literal characters, a backslash before any character but a
 * letter or a digit making that character a literal; a literal character
 * or a non-capturing group of literals, (?:...), made optional by ?; a
 * class -- ., or a bracket of literal ASCII characters, ^ first negating
 * it -- once, or under ?, * or +; and capturing groups of those, not
 * nested, nine at most.  Ranges, named classes, escapes like \d, embedded
 * options and back-references bind no kernel; neither does a
 * nondeterministic collation, which regexp_replace() refuses, nor an
 * encoding other than UTF8 or a single-byte one.  No item depends on the
 * collation: none folds case or names a class.
 *
 * The matcher reads the value's bytes, a character at a time: a literal
 * matches its bytes at a character's start, which in UTF8 is the same
 * character; a class's members are ASCII, so that a character of more
 * bytes is in a negated class and in no other, and . is any character,
 * newlines too.  Its backtracking is bounded: a row past
 * REGEX_MAX_STEPS steps is the function's to answer (`failed`), so that no
 * value's time grows past a bound.  So is a value the encoding does not
 * take whole (pg_encoding_verifymbstr()): PostgreSQL's engine matches a
 * value's wide characters, which pg_mb2wchar_with_len() ends at a NUL byte
 * and makes without a truncated last character, where the matcher reads
 * the bytes -- 'caf' and a lone 0xc3, or 'café', a NUL and more, would be
 * answered otherwise (PostgreSQL's encoding test).  Where a batch's values
 * lie one after another in one buffer -- offsets, a source's text and
 * Arrow's -- their bytes are checked at once, with every offset a
 * character's start: whole, and cut only between characters, each value is
 * whole.  Any other batch's values are checked one by one.  The replacement is regexp_replace()'s
 * (appendStringInfoRegexpSubstr(), PG19:src/backend/utils/adt/
 * varlena.c:3247-3331): \1 to \9 a group's text, \& the match's, \\ a
 * backslash, and any other backslash itself; a value the pattern does not
 * match is returned as it is.
 *
 * Its correctness is checked by differential fuzzing against PostgreSQL's
 * engine (vexec_regex.sql): random patterns of these items and values
 * made from their pieces, each value's answer the function's.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/ascii.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "varatt.h"

#include "vexec.h"
#include "expr/expr.h"
#include "expr/kernel.h"

/* A row's matching gives up past this many steps (above). */
#define REGEX_MAX_STEPS		20000
#define REGEX_MAX_ITEMS		64
#define REGEX_MAX_GROUPS	9

typedef enum RegexItemKind
{
	RI_LIT,						/* bytes, as they are */
	RI_OPT,						/* bytes, or none: greedy */
	RI_CLASS,					/* min to max characters of a class: greedy */
	RI_OPEN,					/* a group's start */
	RI_CLOSE					/* and its end */
} RegexItemKind;

typedef struct RegexItem
{
	uint8		kind;
	bool		any;			/* RI_CLASS: . */
	bool		negate;			/* RI_CLASS: [^...] */
	int			min;			/* RI_CLASS: 0 or 1 */
	int			max;			/* RI_CLASS: 1, or -1 for no bound */
	uint64		member[2];		/* RI_CLASS: its ASCII members, a bit each */
	int			excl;			/* RI_CLASS: a negated class's one member, or
								 * -1 */
	int			group;			/* RI_OPEN, RI_CLOSE: from 1 */
	int			off;			/* RI_LIT, RI_OPT: its bytes in the pattern's */
	int			len;
} RegexItem;

/* The replacement: literal bytes, and references to the match's parts. */
typedef struct RegexPart
{
	int			ref;			/* -1: bytes; 0: the match; 1 to 9: a group */
	int			off;			/* the bytes', in the replacement's */
	int			len;
} RegexPart;

typedef struct RegexCall
{
	int			nitems;
	RegexItem	items[REGEX_MAX_ITEMS];
	int			ngroups;
	char	   *lits;			/* the items' literal bytes */
	int			nparts;
	RegexPart  *parts;
	char	   *repl;			/* the replacement's bytes */
	bool		utf8;
	int			encoding;		/* the database's: each value checked in it */
} RegexCall;

/* ---- the pattern ---- */

static inline bool
regex_alnum(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* A character's bytes at p, whose lead byte is p[0]: UTF8's, or one. */
static inline int
regex_charlen(bool utf8, const char *p, int left)
{
	int			l;

	if (!utf8 || (unsigned char) p[0] < 0x80)
		return 1;
	l = pg_utf_mblen((const unsigned char *) p);
	return Min(Max(l, 1), left);
}

/*
 * Whether a value's bytes are its encoding's whole.  Its ASCII prefix without
 * a NUL -- which every server encoding takes as it is, and which ends where a
 * character begins -- is found a chunk at a time (is_valid_ascii()); the
 * rest, from the first chunk that is not, is checked by
 * pg_encoding_verifymbstr().  So a URL pays the cheap pass alone, and a value
 * of other text the verifier's from its first such byte's chunk.
 */
static inline bool
regex_value_whole(int encoding, const char *s, int len)
{
	int			i = 0;

	while (i + (int) sizeof(Vector8) <= len &&
		   is_valid_ascii((const unsigned char *) s + i, (int) sizeof(Vector8)))
		i += (int) sizeof(Vector8);
	/* the tail's bytes, where the chunks reached it: none 0, none past 0x7F */
	if (i + (int) sizeof(Vector8) > len)
	{
		int			j = i;

		while (j < len && (unsigned char) (s[j] - 1) < 0x7F)
			j++;
		if (j == len)
			return true;
	}
	return pg_encoding_verifymbstr(encoding, s + i, len - i) == len - i;
}

/*
 * Whether every value of an OFFSETS column's nrows is whole at once: their
 * bytes, from the first's start to the last's end, the encoding's whole,
 * and in UTF8 no offset within them at a continuation byte, so that no
 * value begins or ends inside a character.  The rows not active are
 * checked too; one not whole sends the batch to the check of each value.
 */
static bool
regex_batch_whole(const RegexCall *rc, const VexecVec *s, int nrows)
{
	const int32 *off = (const int32 *) s->values;
	const char *b = s->buffers[0];
	int32		lo = off[0];
	int32		hi = off[nrows];

	if (!regex_value_whole(rc->encoding, b + lo, hi - lo))
		return false;
	if (rc->utf8)
		for (int i = 1; i < nrows; i++)
			if (off[i] < hi && ((unsigned char) b[off[i]] & 0xC0) == 0x80)
				return false;
	return true;
}

/* The quantifier after an atom: ?, * or +, or none; *? and the like refused. */
static bool
regex_quantifier(const char *p, int len, int *i, char *q)
{
	*q = 0;
	if (*i < len && (p[*i] == '?' || p[*i] == '*' || p[*i] == '+'))
	{
		*q = p[(*i)++];
		/* a non-greedy quantifier, or a quantifier repeated */
		if (*i < len && (p[*i] == '?' || p[*i] == '*' || p[*i] == '+' || p[*i] == '{'))
			return false;
	}
	else if (*i < len && p[*i] == '{')
		return false;
	return true;
}

/* A literal character at p[*i] -- one escaped, or plain -- its bytes appended. */
static bool
regex_literal(RegexCall *rc, const char *p, int len, int *i, StringInfo lits, int *clen)
{
	unsigned char c = (unsigned char) p[*i];

	if (c == '\\')
	{
		if (*i + 1 >= len || regex_alnum((unsigned char) p[*i + 1]) ||
			(unsigned char) p[*i + 1] >= 0x80)
			return false;
		appendStringInfoChar(lits, p[*i + 1]);
		*clen = 1;
		*i += 2;
		return true;
	}
	if (strchr("^$.[]()|*+?{}", c) != NULL && c != 0)
		return false;
	*clen = regex_charlen(rc->utf8, p + *i, len - *i);
	appendBinaryStringInfo(lits, p + *i, *clen);
	*i += *clen;
	return true;
}

static bool
regex_add(RegexCall *rc, RegexItem *it)
{
	if (rc->nitems >= REGEX_MAX_ITEMS)
		return false;
	rc->items[rc->nitems++] = *it;
	return true;
}

/* A bracket's members, from p[*i] past its [ to past its ]. */
static bool
regex_bracket(const char *p, int len, int *i, RegexItem *it)
{
	bool		first = true;

	if (*i < len && p[*i] == '^')
	{
		it->negate = true;
		(*i)++;
	}
	for (;;)
	{
		unsigned char c;

		if (*i >= len)
			return false;
		c = (unsigned char) p[*i];
		if (c == ']' && !first)
		{
			(*i)++;
			break;
		}
		/* named classes, collating elements, equivalence classes, ranges */
		if (c == '[' || c == '\\' || c >= 0x80 ||
			(c == '-' && !first && *i + 1 < len && p[*i + 1] != ']'))
			return false;
		it->member[c >> 6] |= UINT64CONST(1) << (c & 63);
		(*i)++;
		first = false;
	}
	return true;
}

/*
 * The pattern's items, where it is one of those above; false for any
 * other, which keeps PostgreSQL's function.
 */
static bool
regex_parse(RegexCall *rc, const char *p, int len)
{
	StringInfoData lits;
	int			i = 1;
	int			open = 0;		/* the group open, or 0 */
	int			end;

	if (len < 2 || p[0] != '^' || p[len - 1] != '$')
		return false;
	/* the $ escaped by an odd run of backslashes before it is a literal */
	{
		int			b = 0;

		for (int j = len - 2; j >= 1 && p[j] == '\\'; j--)
			b++;
		if (b % 2 == 1)
			return false;
	}
	end = len - 1;
	/* ARE's director, ***=, and embedded options, (?...) at its start */
	if (len >= 3 && p[1] == '*')
		return false;
	initStringInfo(&lits);
	while (i < end)
	{
		RegexItem	it;
		char		q;

		memset(&it, 0, sizeof(it));
		if (p[i] == '(')
		{
			if (i + 1 < end && p[i + 1] == '?')
			{
				/* (?:literals)? -- an optional run of literals */
				int			clen;

				if (i + 2 >= end || p[i + 2] != ':')
					return false;
				i += 3;
				it.kind = RI_OPT;
				it.off = lits.len;
				while (i < end && p[i] != ')')
					if (!regex_literal(rc, p, end, &i, &lits, &clen))
						return false;
				if (i >= end || lits.len == it.off)
					return false;
				i++;
				if (i >= end || p[i] != '?')
					return false;
				i++;
				if (i < end && (p[i] == '?' || p[i] == '*' || p[i] == '+' || p[i] == '{'))
					return false;
				it.len = lits.len - it.off;
				if (!regex_add(rc, &it))
					return false;
				continue;
			}
			if (open > 0 || rc->ngroups >= REGEX_MAX_GROUPS)
				return false;
			it.kind = RI_OPEN;
			it.group = open = ++rc->ngroups;
			i++;
			if (!regex_add(rc, &it))
				return false;
			continue;
		}
		if (p[i] == ')')
		{
			if (open == 0)
				return false;
			it.kind = RI_CLOSE;
			it.group = open;
			open = 0;
			i++;
			/* a quantified group */
			if (i < end && (p[i] == '?' || p[i] == '*' || p[i] == '+' || p[i] == '{'))
				return false;
			if (!regex_add(rc, &it))
				return false;
			continue;
		}
		if (p[i] == '.' || p[i] == '[')
		{
			it.kind = RI_CLASS;
			if (p[i] == '.')
			{
				it.any = true;
				i++;
			}
			else
			{
				i++;
				if (!regex_bracket(p, end, &i, &it))
					return false;
			}
			if (!regex_quantifier(p, end, &i, &q))
				return false;
			it.min = q == '?' || q == '*' ? 0 : 1;
			it.max = q == '*' || q == '+' ? -1 : 1;
			it.excl = -1;
			if (it.negate && pg_popcount64(it.member[0]) + pg_popcount64(it.member[1]) == 1)
				it.excl = it.member[0] != 0 ? pg_rightmost_one_pos64(it.member[0]) :
					64 + pg_rightmost_one_pos64(it.member[1]);
			if (!regex_add(rc, &it))
				return false;
			continue;
		}
		{
			/* a literal character, maybe quantified */
			int			off = lits.len;
			int			clen;

			if (!regex_literal(rc, p, end, &i, &lits, &clen))
				return false;
			if (!regex_quantifier(p, end, &i, &q))
				return false;
			if (q == 0)
			{
				/* runs of literals as one item */
				if (rc->nitems > 0 && rc->items[rc->nitems - 1].kind == RI_LIT &&
					rc->items[rc->nitems - 1].off + rc->items[rc->nitems - 1].len == off)
					rc->items[rc->nitems - 1].len += clen;
				else
				{
					it.kind = RI_LIT;
					it.off = off;
					it.len = clen;
					if (!regex_add(rc, &it))
						return false;
				}
			}
			else if (q == '?')
			{
				it.kind = RI_OPT;
				it.off = off;
				it.len = clen;
				if (!regex_add(rc, &it))
					return false;
			}
			else
			{
				/* x* or x+: a class of one ASCII member */
				unsigned char c = (unsigned char) lits.data[off];

				if (clen != 1 || c >= 0x80)
					return false;
				lits.len = off;
				it.kind = RI_CLASS;
				it.member[c >> 6] |= UINT64CONST(1) << (c & 63);
				it.min = q == '*' ? 0 : 1;
				it.max = -1;
				it.excl = -1;
				if (!regex_add(rc, &it))
					return false;
			}
		}
	}
	if (open > 0)
		return false;
	rc->lits = lits.data;
	return true;
}

/* The replacement's parts, as appendStringInfoRegexpSubstr() reads them. */
static void
regex_parse_replacement(RegexCall *rc, const char *r, int len)
{
	int			i = 0;
	int			start = 0;
	int			n = 0;

	rc->repl = palloc(len + 1);
	memcpy(rc->repl, r, len);
	rc->parts = palloc(sizeof(RegexPart) * (len + 1));

#define FLUSH(upto) \
	do { \
		if ((upto) > start) \
		{ \
			rc->parts[n].ref = -1; \
			rc->parts[n].off = start; \
			rc->parts[n].len = (upto) - start; \
			n++; \
		} \
	} while (0)

	while (i < len)
	{
		if (r[i] != '\\')
		{
			i++;
			continue;
		}
		FLUSH(i);
		i++;
		if (i >= len)
		{
			/* a backslash at the very end: itself */
			start = i - 1;
			break;
		}
		if (r[i] >= '1' && r[i] <= '9')
		{
			rc->parts[n].ref = r[i] - '0';
			rc->parts[n].off = rc->parts[n].len = 0;
			n++;
			start = ++i;
		}
		else if (r[i] == '&')
		{
			rc->parts[n].ref = 0;
			rc->parts[n].off = rc->parts[n].len = 0;
			n++;
			start = ++i;
		}
		else if (r[i] == '\\')
		{
			/* one backslash, the second's */
			start = i++;
		}
		else
		{
			/* the backslash itself, and the character read as any other */
			start = i - 1;
		}
	}
	FLUSH(len);
#undef FLUSH
	rc->nparts = n;
}

/* ---- the matcher ---- */

typedef struct RegexMatch
{
	const RegexCall *rc;
	const char *s;
	int			len;
	int			steps;
	int			caps[2 * (REGEX_MAX_GROUPS + 1)];
} RegexMatch;

/* Whether the character at pos is in the class; its bytes in *clen. */
static inline bool
regex_in_class(const RegexMatch *m, const RegexItem *it, int pos, int *clen)
{
	unsigned char c = (unsigned char) m->s[pos];

	*clen = regex_charlen(m->rc->utf8, m->s + pos, m->len - pos);
	if (it->any)
		return true;
	if (c >= 0x80)
		return it->negate;		/* no member is past ASCII */
	return (((it->member[c >> 6] >> (c & 63)) & 1) != 0) != it->negate;
}

/* The start of the character before pos, which is a character's start. */
static inline int
regex_back(const RegexMatch *m, int pos)
{
	pos--;
	if (m->rc->utf8)
		while (pos > 0 && ((unsigned char) m->s[pos] & 0xC0) == 0x80)
			pos--;
	return pos;
}

/*
 * Items from `item` on matched from pos to the value's end: 1, 0 for no
 * match, -1 past REGEX_MAX_STEPS.  Greedy: an optional item tried
 * present first, a class's run longest first.
 */
static int
regex_match(RegexMatch *m, int item, int pos)
{
	const RegexItem *it;

	for (;;)
	{
		if (++m->steps > REGEX_MAX_STEPS)
			return -1;
		if (item == m->rc->nitems)
			return pos == m->len ? 1 : 0;
		it = &m->rc->items[item];
		switch (it->kind)
		{
			case RI_LIT:
				if (m->len - pos < it->len ||
					memcmp(m->s + pos, m->rc->lits + it->off, it->len) != 0)
					return 0;
				pos += it->len;
				item++;
				continue;
			case RI_OPT:
				if (m->len - pos >= it->len &&
					memcmp(m->s + pos, m->rc->lits + it->off, it->len) == 0)
				{
					int			r = regex_match(m, item + 1, pos + it->len);

					if (r != 0)
						return r;
				}
				item++;
				continue;
			case RI_OPEN:
			case RI_CLOSE:
				{
					int		   *cap = &m->caps[2 * it->group + (it->kind == RI_CLOSE)];
					int			old = *cap;
					int			r;

					*cap = pos;
					r = regex_match(m, item + 1, pos);
					if (r != 1)
						*cap = old;
					return r;
				}
			case RI_CLASS:
				{
					int			end;
					int			least;
					int			clen;

					if (it->max == 1)
					{
						/* once, or none under ? */
						if (pos < m->len && regex_in_class(m, it, pos, &clen))
						{
							if (it->min == 0)
							{
								int			r = regex_match(m, item + 1, pos + clen);

								if (r != 0)
									return r;
								item++;
								continue;
							}
							pos += clen;
							item++;
							continue;
						}
						if (it->min == 1)
							return 0;
						item++;
						continue;
					}

					/*
					 * A run: its end, the longest -- every character of a
					 * value for ., the next excluded byte for a negated class
					 * of one member, else bytes while they are members: a
					 * byte past ASCII is a member of a negated class and of
					 * no other, so that the run ends at a character's start
					 */
					if (it->any)
						end = m->len;
					else if (it->excl >= 0)
					{
						const char *x = memchr(m->s + pos, it->excl, m->len - pos);

						end = x != NULL ? (int) (x - m->s) : m->len;
					}
					else
					{
						end = pos;
						while (end < m->len)
						{
							unsigned char c = (unsigned char) m->s[end];

							if (c >= 0x80 ? !it->negate :
								(((it->member[c >> 6] >> (c & 63)) & 1) != 0) == it->negate)
								break;
							end++;
						}
					}
					if (end == pos && it->min == 1)
						return 0;
					/* the last item: the run to the end, or nothing */
					if (item + 1 == m->rc->nitems)
						return end == m->len ? 1 : 0;
					least = it->min == 0 ? pos : pos + regex_charlen(m->rc->utf8, m->s + pos, m->len - pos);
					for (;;)
					{
						int			r = regex_match(m, item + 1, end);

						if (r != 0)
							return r;
						if (end <= least)
							return 0;
						if (++m->steps > REGEX_MAX_STEPS)
							return -1;
						end = regex_back(m, end);
					}
				}
		}
	}
}

/* ---- the kernel ---- */

static bool
regex_bind(VexecExpr *call, const void *info)
{
	List	   *args = ((FuncExpr *) call->expr)->args;
	Const	   *pat;
	Const	   *rep;
	text	   *t;
	RegexCall  *rc;

	(void) info;
	if (call->kind != VE_CALL || !IsA(call->expr, FuncExpr) || list_length(args) != 3 ||
		!IsA(lsecond(args), Const) || !IsA(lthird(args), Const))
		return false;
	pat = lsecond_node(Const, args);
	rep = lthird_node(Const, args);
	if (pat->constisnull || rep->constisnull)
		return false;
	if (vexec_collation_class(call->collation) == InvalidOid)
		return false;
	if (GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() != 1)
		return false;
	rc = palloc0(sizeof(RegexCall));
	rc->utf8 = GetDatabaseEncoding() == PG_UTF8;
	rc->encoding = GetDatabaseEncoding();
	t = DatumGetTextPP(pat->constvalue);
	if (!regex_parse(rc, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t)))
		return false;
	t = DatumGetTextPP(rep->constvalue);
	regex_parse_replacement(rc, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	call->extra = rc;
	return true;
}

/*
 * Each active row matched, its groups kept; then the result's bytes, a
 * row's replacement where it matched, its value where not.
 */
static void
regex_kernel(VexecKernelCall *kc)
{
	const RegexCall *rc = kc->call->extra;
	const VexecVec *s = kc->args[0];
	VexecVec   *out = kc->result;
	int			ncaps = 2 * (rc->ngroups + 1);
	int		   *caps = vexec_batch_alloc(kc->work, sizeof(int) * ncaps * kc->nrows);
	int32	   *offsets = (int32 *) out->values;
	RegexMatch	m;
	int64		size = 0;
	int64		used = 0;
	char	   *data;
	bool		whole;

	m.rc = rc;
	whole = s->shape.layout == VEXEC_OFFSETS && s->encoding == VEXEC_FLAT &&
		regex_batch_whole(rc, s, kc->nrows);
	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		const char *a;
		Size		alen;
		int		   *c = caps + i * ncaps;
		int			r;

		vexec_vec_value_bytes(s, vexec_arg_row(s, i), &a, &alen);
		if (!whole && !regex_value_whole(rc->encoding, a, (int) alen))
		{
			/* a NUL, or a character the encoding refuses: PostgreSQL's answer */
			vexec_fail(kc, i);
			c[0] = -2;
			continue;
		}
		m.s = a;
		m.len = (int) alen;
		m.steps = 0;
		for (int k = 0; k < ncaps; k++)
			m.caps[k] = -1;
		m.caps[0] = 0;
		m.caps[1] = (int) alen;
		r = regex_match(&m, 0, 0);
		if (r < 0)
		{
			/* past the bound: PostgreSQL's function answers it */
			vexec_fail(kc, i);
			c[0] = -2;
			continue;
		}
		if (r == 0)
		{
			c[0] = -1;			/* no match: the value as it is */
			size += alen;
			continue;
		}
		memcpy(c, m.caps, sizeof(int) * ncaps);
		for (int k = 0; k < rc->nparts; k++)
		{
			const RegexPart *pt = &rc->parts[k];

			if (pt->ref < 0)
				size += pt->len;
			else if (pt->ref <= rc->ngroups && c[2 * pt->ref] >= 0 && c[2 * pt->ref + 1] >= 0)
				size += c[2 * pt->ref + 1] - c[2 * pt->ref];
		}
	}
	if (size > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a batch's offsets column cannot hold more than %d bytes",
						PG_INT32_MAX)));
	data = vexec_batch_alloc(kc->work, Max(size, 1));
	out->nbuffers = 1;
	out->buffers = vexec_batch_alloc0(kc->work, sizeof(char *));
	out->buffer_sizes = vexec_batch_alloc0(kc->work, sizeof(int64));

	offsets[0] = 0;
	for (int i = 0; i < kc->nrows; i++)
	{
		const char *a;
		Size		alen;
		const int  *c = caps + i * ncaps;

		offsets[i + 1] = (int32) used;
		if (!vexec_bit(kc->active, i) || c[0] == -2)
			continue;
		vexec_vec_value_bytes(s, vexec_arg_row(s, i), &a, &alen);
		if (c[0] == -1)
		{
			memcpy(data + used, a, alen);
			used += alen;
		}
		else
			for (int k = 0; k < rc->nparts; k++)
			{
				const RegexPart *pt = &rc->parts[k];

				if (pt->ref < 0)
				{
					memcpy(data + used, rc->repl + pt->off, pt->len);
					used += pt->len;
				}
				else if (pt->ref <= rc->ngroups && c[2 * pt->ref] >= 0 && c[2 * pt->ref + 1] >= 0)
				{
					int			n = c[2 * pt->ref + 1] - c[2 * pt->ref];

					memcpy(data + used, a + c[2 * pt->ref], n);
					used += n;
				}
			}
		offsets[i + 1] = (int32) used;
	}
	Assert(used <= size);
	out->buffers[0] = data;
	out->buffer_sizes[0] = Max(size, 1);
}

static bool
regex_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	args[0] = vexec_text_plain(kc, args[0]);
	memset(&v->result, 0, sizeof(VexecShape));
	v->result.layout = VEXEC_OFFSETS;
	v->fn = regex_kernel;
	return true;
}

static const VexecKernelDef regex_replace_def = {"regexp_replace", true, regex_bind, regex_variant};

void
vexec_kernels_regex(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	add(F_REGEXP_REPLACE_TEXT_TEXT_TEXT, &regex_replace_def, NULL);
}
