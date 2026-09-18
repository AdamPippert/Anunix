/*
 * anx/civil.h — Calendar date and time from a UNIX timestamp.
 */

#ifndef ANX_CIVIL_H
#define ANX_CIVIL_H

#include <anx/types.h>

struct anx_civil {
	int32_t  year;
	uint8_t  month;		/* 1..12 */
	uint8_t  day;		/* 1..31 */
	uint8_t  hour;
	uint8_t  min;
	uint8_t  sec;
	uint8_t  wday;		/* 0 = Sunday */
};

/* Break a UNIX timestamp, shifted by offset_hours, into calendar fields. */
void anx_civil_from_unix(int64_t unix_ts, int32_t offset_hours,
			 struct anx_civil *out);

/* Three-letter English day name for wday 0..6 ("???" otherwise). */
const char *anx_civil_day_name(uint8_t wday);

#endif /* ANX_CIVIL_H */
