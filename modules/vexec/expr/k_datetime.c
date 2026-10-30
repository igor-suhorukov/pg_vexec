/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_datetime.c
 *	  Kernels over date and timestamp (pg_vector_executor.md §3.7, H8 of
 *	  §3.14): the casts between them, and extract and date_part.
 *
 *	timestamp(date)		date2timestamp_safe(): the infinities kept, and a
 *						date past timestamp's range marked, where
 *						date_timestamp() raises "date out of range for
 *						timestamp" (PG19:src/backend/utils/adt/date.c)
 *	date(timestamp)		timestamp2date_safe(), likewise
 *	extract(f, date)	extract_date()'s fields (date.c), an integer each:
 *						the scaled layout at scale 0
 *	extract(f, timestamp)	timestamp_part_common()'s (timestamp.c): an
 *						integer, or for second, millisecond and epoch the
 *						microseconds at scale 6, 3 and 6, as
 *						int64_div_fast_to_numeric() makes them; hour to
 *						microsecond from the time of day alone, the
 *						calendar's fields by timestamp2tm()
 *	date_part(f, timestamp)	the same fields as float8, in the same
 *						expressions as timestamp_part_common()
 *	date_trunc(f, timestamp)	for a day and less: the timestamp less its
 *						remainder, as timestamp_trunc() zeroes the fields
 *	to_timestamp(float8)	float8_timestamptz() (timestamp.c): a NaN or
 *						seconds past the range marked, an infinity the
 *						timestamp's (W43, pg_vector_executor.md §3.35)
 *	extract(f, timestamptz), date_trunc(f, timestamptz)	under a session
 *						time zone of UTC's fixed offset alone -- the
 *						timestamp's own, its local time its UTC time --
 *						bound so as the call is (W43)
 *
 * The field is a constant, decoded when the call is bound with
 * DecodeUnits() and DecodeSpecial(), as those functions decode it; a field
 * they would refuse, or one these kernels do not compute, binds no kernel,
 * so its error stays PostgreSQL's.  An infinite date or timestamp gives
 * NULL for an oscillating field, and an infinity for a growing one, which
 * the scaled layout cannot hold: such a row is marked, and PostgreSQL's
 * evaluator computes it.  A column kept in Arrow's epoch is shifted back
 * first.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "parser/scansup.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/timestamp.h"
#include "pgtime.h"
#include "varatt.h"

#include "vexec.h"
#include "expr/kernel.h"

typedef enum DtKind
{
	DT_DATE_TO_TS,
	DT_TS_TO_DATE,
	DT_EXTRACT_DATE,
	DT_EXTRACT_TS,
	DT_DATE_PART_TS,
	DT_TRUNC_TS,
	DT_FLOAT8_TO_TS				/* W43 */
} DtKind;

typedef struct DtInfo
{
	uint8		kind;
	bool		utc;			/* W43: a timestamptz's, bound under UTC alone */
} DtInfo;

/*
 * W43 (pg_vector_executor.md §3.35): whether the session's time zone is
 * UTC's -- a fixed offset of none, no daylight saving -- under which a
 * timestamptz's fields and truncation are its UTC time's, a timestamp's
 */
bool
vexec_session_utc(void)
{
	long		gmtoff;

	return session_timezone != NULL && pg_get_timezone_offset(session_timezone, &gmtoff) && gmtoff == 0;
}

/* A call of extract or date_part: its field, decoded. */
typedef struct DtField
{
	uint8		kind;
	int			type;			/* UNITS or RESERV */
	int			val;			/* DTK_* */
	int			scale;			/* extract's: 0, 3 or 6 */
	int64		unit;			/* date_trunc's: the unit in microseconds */
} DtField;

static void trunc_ts(VexecKernelCall *kc);

/* A temporal argument in PostgreSQL's epoch (§3.4.2). */
static void
pg_epoch(VexecKernelCall *kc, VexecVec **arg)
{
	VexecShape	to;
	VexecVec   *c;

	if (!(*arg)->shape.arrow_values)
		return;
	to = (*arg)->shape;
	to.arrow_values = false;
	c = vexec_batch_alloc(kc->work, sizeof(VexecVec));
	*c = **arg;
	if (!vexec_vec_convert(kc->work, c, &to))
		elog(ERROR, "vexec: a temporal column could not return to PostgreSQL's epoch");
	*arg = c;
}

/* Mark row i of the result NULL. */
static void
set_null(VexecKernelCall *kc, int i)
{
	VexecVec   *r = kc->result;

	if (r->validity == NULL)
		r->validity = vexec_bitmap_alloc(kc->work, kc->nrows, true);
	vexec_bit_clear(r->validity, i);
}

/* ---- casts ---- */

static void
date_to_ts(VexecKernelCall *kc)
{
	const VexecVec *v = kc->args[0];
	Timestamp  *out = kc->result->values;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		ErrorSaveContext escontext = {T_ErrorSaveContext};
		DateADT		d = ((const DateADT *) v->values)[vexec_arg_row(v, i)];

		out[i] = date2timestamp_safe(d, (Node *) &escontext);
		if (escontext.error_occurred)
			vexec_fail(kc, i);
	}
}

/* W43: to_timestamp(float8) as float8_timestamptz() makes it, its errors' rows marked */
static void
float8_to_ts(VexecKernelCall *kc)
{
	const VexecVec *v = kc->args[0];
	TimestampTz *out = kc->result->values;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		float8		s = ((const float8 *) v->values)[vexec_arg_row(v, i)];

		if (isnan(s))
		{
			vexec_fail(kc, i);
			continue;
		}
		if (isinf(s))
		{
			if (s < 0)
				TIMESTAMP_NOBEGIN(out[i]);
			else
				TIMESTAMP_NOEND(out[i]);
			continue;
		}
		if (s < (float8) SECS_PER_DAY * (DATETIME_MIN_JULIAN - UNIX_EPOCH_JDATE) ||
			s >= (float8) SECS_PER_DAY * (TIMESTAMP_END_JULIAN - UNIX_EPOCH_JDATE))
		{
			vexec_fail(kc, i);
			continue;
		}
		s -= ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
		s = rint(s * USECS_PER_SEC);
		out[i] = (TimestampTz) s;
		if (!IS_VALID_TIMESTAMP(out[i]))
			vexec_fail(kc, i);
	}
}

static void
ts_to_date(VexecKernelCall *kc)
{
	const VexecVec *v = kc->args[0];
	DateADT    *out = kc->result->values;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		ErrorSaveContext escontext = {T_ErrorSaveContext};
		Timestamp	t = ((const Timestamp *) v->values)[vexec_arg_row(v, i)];

		out[i] = timestamp2date_safe(t, (Node *) &escontext);
		if (escontext.error_occurred)
			vexec_fail(kc, i);
	}
}

/* ---- fields ---- */

/* extract_date()'s and timestamp_part_common()'s year-based fields. */
static int64
year_field(int val, int year, int mon, int mday)
{
	int64		r;

	switch (val)
	{
		case DTK_YEAR:
			return year > 0 ? year : year - 1;	/* no year 0 */
		case DTK_DECADE:
			return year >= 0 ? year / 10 : -((8 - (year - 1)) / 10);
		case DTK_CENTURY:
			return year > 0 ? (year + 99) / 100 : -((99 - (year - 1)) / 100);
		case DTK_MILLENNIUM:
			return year > 0 ? (year + 999) / 1000 : -((999 - (year - 1)) / 1000);
		case DTK_ISOYEAR:
			r = date2isoyear(year, mon, mday);
			return r <= 0 ? r - 1 : r;
		case DTK_WEEK:
			return date2isoweek(year, mon, mday);
		case DTK_QUARTER:
			return (mon - 1) / 3 + 1;
		case DTK_MONTH:
			return mon;
		case DTK_DAY:
			return mday;
		case DTK_DOW:
		case DTK_ISODOW:
			r = j2day(date2j(year, mon, mday));
			return (val == DTK_ISODOW && r == 0) ? 7 : r;
		case DTK_DOY:
			return date2j(year, mon, mday) - date2j(year, 1, 1) + 1;
	}
	return 0;
}

/* A growing field: an infinite date or timestamp gives an infinity. */
static bool
field_grows(int val)
{
	switch (val)
	{
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
		case DTK_JULIAN:
		case DTK_ISOYEAR:
		case DTK_EPOCH:
			return true;
	}
	return false;
}

static void
extract_date(VexecKernelCall *kc)
{
	const DtField *f = kc->call->extra;
	const VexecVec *v = kc->args[1];
	int64	   *out = kc->result->values;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		DateADT		d = ((const DateADT *) v->values)[vexec_arg_row(v, i)];
		int			year,
					mon,
					mday;

		if (DATE_NOT_FINITE(d))
		{
			if (field_grows(f->val))
				vexec_fail(kc, i);	/* an infinity: PostgreSQL's evaluator */
			else
				set_null(kc, i);
			continue;
		}
		if (f->val == DTK_JULIAN)
		{
			out[i] = (int64) d + POSTGRES_EPOCH_JDATE;
			continue;
		}
		if (f->val == DTK_EPOCH)
		{
			out[i] = ((int64) d + POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
			continue;
		}
		j2date(d + POSTGRES_EPOCH_JDATE, &year, &mon, &mday);
		out[i] = year_field(f->val, year, mon, mday);
	}
}

static void
extract_ts(VexecKernelCall *kc)
{
	const DtField *f = kc->call->extra;
	const VexecVec *v = kc->args[1];
	bool		as_float = f->kind == DT_DATE_PART_TS;
	Timestamp	epoch = SetEpochTimestamp();

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		Timestamp	t = ((const Timestamp *) v->values)[vexec_arg_row(v, i)];
		struct pg_tm tt;
		fsec_t		fsec;
		int64		ir = 0;
		float8		fr = 0;
		bool		have_float = false;

		if (TIMESTAMP_NOT_FINITE(t))
		{
			if (!field_grows(f->val))
				set_null(kc, i);
			else if (as_float)
				((float8 *) kc->result->values)[i] =
					TIMESTAMP_IS_NOBEGIN(t) ? -get_float8_infinity() : get_float8_infinity();
			else
				vexec_fail(kc, i);	/* an infinity: PostgreSQL's evaluator */
			continue;
		}
		if (f->type == RESERV)
		{
			/* DTK_EPOCH: (timestamp - epoch) / 1000000 */
			if (t >= PG_INT64_MAX + epoch)
			{
				if (as_float)
				{
					fr = ((float8) t - epoch) / 1000000.0;
					have_float = true;
				}
				else
				{
					vexec_fail(kc, i);
					continue;
				}
			}
			else if (as_float)
			{
				fr = (t - epoch) / 1000000.0;
				have_float = true;
			}
			else
				ir = t - epoch;
		}
		else if (f->val == DTK_MICROSEC || f->val == DTK_MILLISEC || f->val == DTK_SECOND ||
				 f->val == DTK_MINUTE || f->val == DTK_HOUR)
		{
			/*
			 * F10 (§3.31): a field within the day from the time of day, as
			 * timestamp2tm() takes it -- the remainder modulo a day, taken
			 * upward -- and dt2time() splits it; a timestamp whose day it
			 * refuses, PostgreSQL's evaluator
			 */
			int64		tod = t % USECS_PER_DAY;
			int64		day = t / USECS_PER_DAY;
			int64		sec;
			int64		us;

			if (tod < 0)
			{
				tod += USECS_PER_DAY;
				day -= 1;
			}
			day += POSTGRES_EPOCH_JDATE;
			if (day < 0 || day > (int64) PG_INT32_MAX)
			{
				vexec_fail(kc, i);	/* "timestamp out of range" */
				continue;
			}
			sec = tod % USECS_PER_MINUTE / USECS_PER_SEC;
			us = tod % USECS_PER_SEC;
			switch (f->val)
			{
				case DTK_HOUR:
					ir = tod / USECS_PER_HOUR;
					break;
				case DTK_MINUTE:
					ir = tod % USECS_PER_HOUR / USECS_PER_MINUTE;
					break;
				case DTK_MILLISEC:
					if (as_float)
					{
						fr = sec * 1000.0 + us / 1000.0;
						have_float = true;
					}
					else
						ir = tod % USECS_PER_MINUTE;
					break;
				case DTK_SECOND:
					if (as_float)
					{
						fr = sec + us / 1000000.0;
						have_float = true;
					}
					else
						ir = tod % USECS_PER_MINUTE;
					break;
				default:		/* DTK_MICROSEC */
					ir = tod % USECS_PER_MINUTE;
					break;
			}
		}
		else
		{
			/* the calendar's fields */
			if (timestamp2tm(t, NULL, &tt, &fsec, NULL, NULL) != 0)
			{
				vexec_fail(kc, i);	/* "timestamp out of range" */
				continue;
			}
			ir = year_field(f->val, tt.tm_year, tt.tm_mon, tt.tm_mday);
		}
		if (as_float)
			((float8 *) kc->result->values)[i] = have_float ? fr : (float8) ir;
		else
			((int64 *) kc->result->values)[i] = ir;
	}
}

/*
 * The field of an extract or date_part, decoded as the function decodes it,
 * or false: not a constant, or a field these kernels do not compute.
 */
static bool
field_bind(VexecExpr *call, const void *info)
{
	const DtInfo *di = info;
	List	   *args = ((FuncExpr *) call->expr)->args;
	Const	   *c;
	text	   *units;
	char	   *lowunits;
	DtField    *f;
	int			type;
	int			val;

	if (call->kind != VE_CALL || !IsA(call->expr, FuncExpr) || list_length(args) != 2)
		return false;
	if (!IsA(linitial(args), Const) || ((Const *) linitial(args))->constisnull)
		return false;
	if (di->utc && !vexec_session_utc())
		return false;
	c = linitial_node(Const, args);
	units = DatumGetTextPP(c->constvalue);
	lowunits = downcase_truncate_identifier(VARDATA_ANY(units), VARSIZE_ANY_EXHDR(units), false);
	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	f = palloc0(sizeof(DtField));
	f->kind = di->kind;
	f->type = type;
	f->val = val;
	if (type == RESERV)
	{
		if (val != DTK_EPOCH)
			return false;
		f->scale = di->kind == DT_EXTRACT_TS ? 6 : 0;
	}
	else if (type == UNITS)
	{
		switch (val)
		{
			case DTK_DAY:
			case DTK_MONTH:
			case DTK_QUARTER:
			case DTK_WEEK:
			case DTK_YEAR:
			case DTK_DECADE:
			case DTK_CENTURY:
			case DTK_MILLENNIUM:
			case DTK_ISOYEAR:
			case DTK_DOW:
			case DTK_ISODOW:
			case DTK_DOY:
				break;
			case DTK_JULIAN:
				/* a date's is an integer; a timestamp's has a fraction */
				if (di->kind != DT_EXTRACT_DATE)
					return false;
				break;
			case DTK_MICROSEC:
			case DTK_MILLISEC:
			case DTK_SECOND:
			case DTK_MINUTE:
			case DTK_HOUR:
				if (di->kind == DT_EXTRACT_DATE)
					return false;	/* "not supported for type date" */
				f->scale = val == DTK_SECOND ? 6 : val == DTK_MILLISEC ? 3 : 0;
				break;
			default:
				return false;
		}
	}
	else
		return false;
	call->extra = f;
	return true;
}

static bool
dt_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	const DtInfo *info = kc->call->extra;
	int			k = kc->call->nargs == 2 ? ((const DtField *) kc->call->extra)->kind : info->kind;

	memset(&v->result, 0, sizeof(VexecShape));
	switch (k)
	{
		case DT_DATE_TO_TS:
			pg_epoch(kc, &args[0]);
			v->result.layout = VEXEC_FIXED;
			v->result.width = v->result.stride = 8;
			v->fn = date_to_ts;
			return true;
		case DT_TS_TO_DATE:
			pg_epoch(kc, &args[0]);
			v->result.layout = VEXEC_FIXED;
			v->result.width = v->result.stride = 4;
			v->fn = ts_to_date;
			return true;
		case DT_FLOAT8_TO_TS:
			v->result.layout = VEXEC_FIXED;
			v->result.width = v->result.stride = 8;
			v->fn = float8_to_ts;
			return true;
		case DT_EXTRACT_DATE:
		case DT_EXTRACT_TS:
			pg_epoch(kc, &args[1]);
			v->result.layout = VEXEC_SCALED;
			v->result.width = v->result.stride = 8;
			v->result.scale = ((const DtField *) kc->call->extra)->scale;
			v->fn = k == DT_EXTRACT_DATE ? extract_date : extract_ts;
			return true;
		case DT_TRUNC_TS:
			pg_epoch(kc, &args[1]);
			v->result.layout = VEXEC_FIXED;
			v->result.width = v->result.stride = 8;
			v->fn = trunc_ts;
			return true;
		default:				/* DT_DATE_PART_TS */
			pg_epoch(kc, &args[1]);
			v->result.layout = VEXEC_FIXED;
			v->result.width = v->result.stride = 8;
			v->fn = extract_ts;
			return true;
	}
}

/* ---- date_trunc ---- */

/*
 * date_trunc(unit, timestamp) for a unit of a day or less: the timestamp
 * less its remainder modulo the unit, the remainder taken upward as
 * timestamp2tm() takes a time of day, from 0 (timestamp.c:4690-4835, which
 * zeroes the fields below the unit); an infinity unchanged, as it returns
 * one.  Days are of the same length in a timestamp without time zone, and
 * both epochs begin one, so the remainder is the fields' sum.  The calendar's
 * units -- week, month and up -- keep PostgreSQL's function.
 */
static void
trunc_ts(VexecKernelCall *kc)
{
	const DtField *f = kc->call->extra;
	const VexecVec *v = kc->args[1];
	Timestamp  *out = kc->result->values;
	int64		u = f->unit;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		Timestamp	t = ((const Timestamp *) v->values)[vexec_arg_row(v, i)];
		int64		r;

		if (TIMESTAMP_NOT_FINITE(t))
		{
			out[i] = t;
			continue;
		}
		r = t % u;
		if (r < 0)
			r += u;
		out[i] = t - r;
	}
}

static bool
trunc_bind(VexecExpr *call, const void *info)
{
	List	   *args = ((FuncExpr *) call->expr)->args;
	Const	   *c;
	text	   *units;
	char	   *lowunits;
	DtField    *f;
	int			val;

	if (call->kind != VE_CALL || !IsA(call->expr, FuncExpr) || list_length(args) != 2)
		return false;
	if (!IsA(linitial(args), Const) || ((Const *) linitial(args))->constisnull)
		return false;
	if (((const DtInfo *) info)->utc && !vexec_session_utc())
		return false;
	c = linitial_node(Const, args);
	units = DatumGetTextPP(c->constvalue);
	lowunits = downcase_truncate_identifier(VARDATA_ANY(units), VARSIZE_ANY_EXHDR(units), false);
	if (DecodeUnits(0, lowunits, &val) != UNITS)
		return false;
	f = palloc0(sizeof(DtField));
	f->kind = ((const DtInfo *) info)->kind;
	f->type = UNITS;
	f->val = val;
	switch (val)
	{
		case DTK_DAY:
			f->unit = USECS_PER_DAY;
			break;
		case DTK_HOUR:
			f->unit = USECS_PER_HOUR;
			break;
		case DTK_MINUTE:
			f->unit = USECS_PER_MINUTE;
			break;
		case DTK_SECOND:
			f->unit = USECS_PER_SEC;
			break;
		case DTK_MILLISEC:
			f->unit = 1000;
			break;
		case DTK_MICROSEC:
			f->unit = 1;
			break;
		default:
			return false;
	}
	call->extra = f;
	return true;
}

static bool
cast_bind(VexecExpr *call, const void *info)
{
	call->extra = (void *) info;
	return call->kind == VE_CALL;
}

static const VexecKernelDef dt_cast_def = {"date cast", true, cast_bind, dt_variant};
static const VexecKernelDef dt_field_def = {"date field", true, field_bind, dt_variant};
static const VexecKernelDef dt_trunc_def = {"date_trunc", false, trunc_bind, dt_variant};

static const DtInfo info_date_to_ts = {DT_DATE_TO_TS};
static const DtInfo info_ts_to_date = {DT_TS_TO_DATE};
static const DtInfo info_extract_date = {DT_EXTRACT_DATE};
static const DtInfo info_extract_ts = {DT_EXTRACT_TS};
static const DtInfo info_date_part_ts = {DT_DATE_PART_TS};
static const DtInfo info_trunc_ts = {DT_TRUNC_TS};
static const DtInfo info_float8_to_ts = {DT_FLOAT8_TO_TS};
static const DtInfo info_extract_tstz = {DT_EXTRACT_TS, true};
static const DtInfo info_trunc_tstz = {DT_TRUNC_TS, true};

void
vexec_kernels_datetime(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	add(F_TIMESTAMP_DATE, &dt_cast_def, &info_date_to_ts);
	add(F_DATE_TIMESTAMP, &dt_cast_def, &info_ts_to_date);
	add(F_EXTRACT_TEXT_DATE, &dt_field_def, &info_extract_date);
	add(F_EXTRACT_TEXT_TIMESTAMP, &dt_field_def, &info_extract_ts);
	add(F_DATE_PART_TEXT_TIMESTAMP, &dt_field_def, &info_date_part_ts);
	add(F_DATE_TRUNC_TEXT_TIMESTAMP, &dt_trunc_def, &info_trunc_ts);
	/* W43 (§3.35) */
	add(F_TO_TIMESTAMP_FLOAT8, &dt_cast_def, &info_float8_to_ts);
	add(F_EXTRACT_TEXT_TIMESTAMPTZ, &dt_field_def, &info_extract_tstz);
	add(F_DATE_TRUNC_TEXT_TIMESTAMPTZ, &dt_trunc_def, &info_trunc_tstz);
}
