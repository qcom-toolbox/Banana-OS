#include "rtc.h"
#include "timer.h"
#include "kstring.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port));
    return v;
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int bcd_to_bin(int v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

static int rtc_read_raw(rtc_datetime_t* out, uint8_t* reg_b) {
    if (!out || !reg_b) return -1;

    out->second = cmos_read(0x00);
    out->minute = cmos_read(0x02);
    out->hour   = cmos_read(0x04);
    out->day    = cmos_read(0x07);
    out->month  = cmos_read(0x08);
    out->year   = cmos_read(0x09);
    *reg_b      = cmos_read(0x0B);
    return 0;
}

void rtc_init(void) {
    /* No setup needed for basic RTC reads. */
}

int rtc_read_datetime(rtc_datetime_t* out) {
    if (!out) return -1;

    rtc_datetime_t a, b;
    uint8_t reg_b = 0;
    int guard = 10000;

    while (guard-- > 0) {
        while (cmos_read(0x0A) & 0x80) { /* update-in-progress */ }
        rtc_read_raw(&a, &reg_b);
        while (cmos_read(0x0A) & 0x80) { }
        rtc_read_raw(&b, &reg_b);

        if (a.second == b.second &&
            a.minute == b.minute &&
            a.hour   == b.hour &&
            a.day    == b.day &&
            a.month  == b.month &&
            a.year   == b.year) {
            break;
        }
    }
    if (guard <= 0) return -1;

    /* Convert from BCD if needed (reg B bit 2 = 1 means binary mode). */
    if ((reg_b & 0x04) == 0) {
        b.second = (uint8_t)bcd_to_bin(b.second);
        b.minute = (uint8_t)bcd_to_bin(b.minute);
        b.hour   = (uint8_t)(bcd_to_bin(b.hour & 0x7F) | (b.hour & 0x80));
        b.day    = (uint8_t)bcd_to_bin(b.day);
        b.month  = (uint8_t)bcd_to_bin(b.month);
        b.year   = (uint16_t)bcd_to_bin((int)b.year);
    }

    /* Convert 12h mode to 24h if needed (reg B bit 1 = 1 means 24h mode). */
    if ((reg_b & 0x02) == 0) {
        int pm = (b.hour & 0x80) ? 1 : 0;
        int h = b.hour & 0x7F;
        if (pm && h < 12) h += 12;
        if (!pm && h == 12) h = 0;
        b.hour = (uint8_t)h;
    } else {
        b.hour &= 0x7F;
    }

    b.year = (uint16_t)(2000u + (b.year % 100u)); /* best effort */
    *out = b;
    return 0;
}


/* ── Unix time (days from the civil date: H. Hinnant's algorithms) ── */

static int32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint32_t rtc_make_time(int year, int month, int day, int hour, int minute, int second) {
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    return (uint32_t)days_from_civil(year, month, day) * 86400u + (uint32_t)(hour * 3600 + minute * 60 + second);
}

void rtc_split_time(uint32_t t, rtc_datetime_t* out) {
    int32_t z = (int32_t)(t / 86400u) + 719468;
    uint32_t secs = t % 86400u;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp + (mp < 10 ? 3 : -9);
    out->year = (uint16_t)(y + (m <= 2));
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->hour = (uint8_t)(secs / 3600u);
    out->minute = (uint8_t)((secs / 60u) % 60u);
    out->second = (uint8_t)(secs % 60u);
}

uint32_t rtc_now(void) {
    static uint32_t base, base_ms;
    static int have;
    uint32_t ms = timer_ms();
    if (!have || ms - base_ms > 600000u) {
        rtc_datetime_t dt;
        if (rtc_read_datetime(&dt) == 0 && dt.year >= 1970) {
            base = rtc_make_time(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
            base_ms = ms;
            have = 1;
        } else if (!have) {
            return 0;
        }
    }
    return base + (ms - base_ms) / 1000u;
}

void rtc_format(uint32_t t, char* out, int cap) {
    if (!t) { if (cap > 0) out[0] = 0; return; }
    rtc_datetime_t dt;
    rtc_split_time(t, &dt);
    ksnprintf(out, (size_t)cap, "%02u/%02u/%04u %02u:%02u", dt.day, dt.month, dt.year, dt.hour, dt.minute);
}
