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
 *						int64_div_fast_to_numeric() makes them
 *	date_part(f, timestamp)	the same fields as float8, in the same
 *						expressions as timestamp_part_common()
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
#include "varatt.h"

#include "vexec.h"
#include "expr/kernel.h"

typedef enum DtKind
{
	DT_DATE_TO_TS,
	DT_TS_TO_DATE,
	DT_EXTRACT_DATE,
	DT_EXTRACT_TS,
	DT_DATE_PART_TS
} DtKind;

typedef struct DtInfo
{
	uint8		kind;
} DtInfo;

/* A call of extract or date_part: its field, decoded. */
typedef struct DtField
{
	uint8		kind;
	int			type;			/* UNITS or RESERV */
	int			val;			/* DTK_* */
	int			scale;			/* extract's: 0, 3 or 6 */
} DtField;

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
		else
		{
			if (timestamp2tm(t, NULL, &tt, &fsec, NULL, NULL) != 0)
			{
				vexec_fail(kc, i);	/* "timestamp out of range" */
				continue;
			}
			switch (f->val)
			{
				case DTK_MICROSEC:
					ir = tt.tm_sec * INT64CONST(1000000) + fsec;
					break;
				case DTK_MILLISEC:
					if (as_float)
					{
						fr = tt.tm_sec * 1000.0 + fsec / 1000.0;
						have_float = true;
					}
					else
						ir = tt.tm_sec * INT64CONST(1000000) + fsec;
					break;
				case DTK_SECOND:
					if (as_float)
					{
						fr = tt.tm_sec + fsec / 1000000.0;
						have_float = true;
					}
					else
						ir = tt.tm_sec * INT64CONST(1000000) + fsec;
					break;
				case DTK_MINUTE:
					ir = tt.tm_min;
					break;
				case DTK_HOUR:
					ir = tt.tm_hour;
					break;
				default:
					ir = year_field(f->val, tt.tm_year, tt.tm_mon, tt.tm_mday);
					break;
			}
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
		case DT_EXTRACT_DATE:
		case DT_EXTRACT_TS:
			pg_epoch(kc, &args[1]);
			v->result.layout = VEXEC_SCALED;
			v->result.width = v->result.stride = 8;
			v->result.scale = ((const DtField *) kc->call->extra)->scale;
			v->fn = k == DT_EXTRACT_DATE ? extract_date : extract_ts;
			return true;
		default:				/* DT_DATE_PART_TS */
			pg_epoch(kc, &args[1]);
			v->result.layout = VEXEC_FIXED;
			v->result.width = v->result.stride = 8;
			v->fn = extract_ts;
			return true;
	}
}

static bool
cast_bind(VexecExpr *call, const void *info)
{
	call->extra = (void *) info;
	return call->kind == VE_CALL;
}

static const VexecKernelDef dt_cast_def = {"date cast", true, cast_bind, dt_variant};
static const VexecKernelDef dt_field_def = {"date field", true, field_bind, dt_variant};

static const DtInfo info_date_to_ts = {DT_DATE_TO_TS};
static const DtInfo info_ts_to_date = {DT_TS_TO_DATE};
static const DtInfo info_extract_date = {DT_EXTRACT_DATE};
static const DtInfo info_extract_ts = {DT_EXTRACT_TS};
static const DtInfo info_date_part_ts = {DT_DATE_PART_TS};

void
vexec_kernels_datetime(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	add(F_TIMESTAMP_DATE, &dt_cast_def, &info_date_to_ts);
	add(F_DATE_TIMESTAMP, &dt_cast_def, &info_ts_to_date);
	add(F_EXTRACT_TEXT_DATE, &dt_field_def, &info_extract_date);
	add(F_EXTRACT_TEXT_TIMESTAMP, &dt_field_def, &info_extract_ts);
	add(F_DATE_PART_TEXT_TIMESTAMP, &dt_field_def, &info_date_part_ts);
}
