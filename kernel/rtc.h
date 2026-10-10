#ifndef RTC_H
#define RTC_H

#include "types.h"

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_datetime_t;

void rtc_init(void);
int rtc_read_datetime(rtc_datetime_t* out);

/* Seconds since 1970-01-01 00:00 of the clock as it reads (the PC's clock
 * keeps the local time; so do the dates of files, kept in these). The
 * clock is read once, then counted with the timer (read again every 10
 * minutes); 0 if there is no clock. */
uint32_t rtc_now(void);
uint32_t rtc_make_time(int year, int month, int day, int hour, int minute, int second);
void     rtc_split_time(uint32_t t, rtc_datetime_t* out);
/* "10/10/2026 16:45" (day/month/year), "" for 0 */
void     rtc_format(uint32_t t, char* out, int cap);

#endif
