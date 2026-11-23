/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UAUDIO_H
#define UAUDIO_H

#include "usbcore.h"

/* The USB Audio Class 1 parts of device d (its configuration already set):
 * speakers play, a microphone records - 0 if it had either. A driver that
 * claimed a composite device (usb/uvc.c: a webcam) binds its sound this
 * way; unbind when it is unplugged. */
int  uaudio_bind(usb_device_t* d);
void uaudio_unbind(usb_device_t* d);

#endif
