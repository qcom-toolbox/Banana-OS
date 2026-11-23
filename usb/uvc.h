/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UVC_H
#define UVC_H

#include "types.h"

/*
 * Webcams: USB Video Class 1.x cameras (usb/uvc.c) - nearly every USB
 * webcam and the cameras built into laptops. Pictures come as YUY2
 * (uncompressed) or MJPEG at up to 640x480; a reader gets the newest one
 * as 0x00RRGGBB pixels or a JPEG file. It streams only while it is open.
 */

int         camera_available(void);
const char* camera_name(void);                 /* "none" without one */

/* 0, or -1 (none, or it does not start). The size it streams at. */
int  camera_open(int* w, int* h);
void camera_close(void);

/* A picture: 0x00RRGGBB pixels (YUY2 cameras), or a JPEG file to decode
 * (MJPEG cameras) */
#define CAMERA_XRGB 1
#define CAMERA_JPEG 2
typedef struct {
    int      width, height, format;
    uint32_t seq;                              /* its number: 1, 2, ... */
    uint32_t bytes;                            /* what was copied */
} camera_frame_t;

/* The newest picture, if it is newer than *seq, into out (cap bytes);
 * *seq becomes its number. 1 if one was copied, 0 if none came since
 * *seq, -1 if the camera is not streaming or out is too small. */
int      camera_frame(void* out, uint32_t cap, camera_frame_t* info, uint32_t* seq);
/* the most a picture can take (the buffer camera_frame needs) */
uint32_t camera_frame_bytes(void);

/* the timer interrupt (kernel/timer.c): packets in, pictures put together */
void uvc_tick(void);

/* lscam */
void camera_list(void);

/* (tests) the YUY2 -> XRGB conversion; MJPEG with its Huffman tables added (kmalloc'd) */
void     uvc_yuy2_to_xrgb(const uint8_t* src, uint32_t* dst, int w, int h);
uint8_t* uvc_mjpeg_fix(const uint8_t* src, uint32_t len, uint32_t* out_len);

#endif
