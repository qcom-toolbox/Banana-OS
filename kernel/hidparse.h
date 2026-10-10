#ifndef HIDPARSE_H
#define HIDPARSE_H

#include "types.h"

/*
 * HID report descriptors: every value of every report, where it sits and
 * what it means (usage), so a driver can read a touchpad's fingers or a
 * mouse's motion from any device's own layout.
 */

#define HID_INPUT    0
#define HID_OUTPUT   1
#define HID_FEATURE  2

#define HID_U(page, id)  ((uint32_t)(page) << 16 | (uint32_t)(id))

typedef struct {
    uint8_t  rid;          /* report ID (0: the device uses none) */
    uint8_t  type;         /* HID_INPUT / HID_FEATURE / HID_OUTPUT */
    uint8_t  flags;        /* the main item's bits: 1 constant, 2 variable, 4 relative */
    int8_t   finger;       /* which Finger collection of its application (0, 1, ...), -1: none */
    uint32_t app;          /* usage of its application collection */
    uint32_t usage;        /* page << 16 | id */
    uint16_t bit, size;    /* where it is in the report (after the report ID byte), in bits */
    int32_t  lmin, lmax;   /* logical range */
    int32_t  mm10;         /* physical extent in 1/10 mm (lengths), 0 if not given */
} hid_field_t;

/* fills up to max fields; returns how many (or -1: not a valid descriptor) */
int hid_parse(const uint8_t* desc, int len, hid_field_t* out, int max);

/* the value of a field in a report (the bytes after the report ID); signed
 * when its logical minimum is negative */
int32_t hid_value(const uint8_t* report, int report_len, const hid_field_t* f);
/* stores a value (feature reports the host sends) */
void hid_set_value(uint8_t* report, int report_len, const hid_field_t* f, uint32_t v);

/* the size in bytes of report `rid` of `type` (without the report ID byte) */
int hid_report_len(const hid_field_t* f, int n, uint8_t rid, uint8_t type);

#endif
