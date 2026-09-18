/*
 * civil.c — Calendar date and time from a UNIX timestamp.
 *
 * The CMOS clock cannot be trusted for the date: Fedora on the same
 * machine keeps it in UTC and never maintains the weekday register, so
 * `date` showed Wednesday on a Thursday. Once NTP has answered, the date
 * comes from the timestamp instead (days_from_civil inverse, H. Hinnant).
 */

#include <anx/types.h>
#include <anx/civil.h>

void anx_civil_from_unix(int64_t unix_ts, int32_t offset_hours,
			 struct anx_civil *out)
{
	int64_t t = unix_ts + (int64_t)offset_hours * 3600;
	int64_t days = t / 86400, secs = t % 86400;
	int64_t z, era, doe, yoe, doy, mp;

	if (secs < 0) {
		secs += 86400;
		days--;
	}
	out->hour = (uint8_t)(secs / 3600);
	out->min = (uint8_t)((secs / 60) % 60);
	out->sec = (uint8_t)(secs % 60);
	out->wday = (uint8_t)(((days % 7) + 11) % 7);	/* 1970-01-01: Thu */

	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	out->day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
	out->month = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
	out->year = (int32_t)(yoe + era * 400 + (out->month <= 2));
}

const char *anx_civil_day_name(uint8_t wday)
{
	static const char *const names[] = {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};

	return wday < 7 ? names[wday] : "???";
}
