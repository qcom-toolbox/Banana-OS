/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AUDIOIN_H
#define AUDIOIN_H

#include "types.h"

/*
 * Recording: microphones and line inputs. Sources: Intel HD Audio's input
 * streams (a mic or line-in pin; QEMU `-device intel-hda -device
 * hda-micro`), AC'97's PCM in (its mic input; QEMU `-device AC97`), and
 * USB microphones (usb/uaudio.c: headsets, webcams' microphones, USB mics
 * - they take over while plugged in).
 *
 * Every source writes 48 kHz 16-bit stereo into one ring (by DMA, or the
 * USB driver from the timer interrupt). The source runs only while some
 * reader has it open; each reader has a cursor of its own into the ring.
 */

#define AUDIO_IN_RATE 48000

/* A source: it writes the ring (audio_in_ring(), audio_in_ring_bytes()
 * long) round and round; position() is the byte it writes next. tick()
 * runs from the timer interrupt every 2 ms (it must not sleep). */
typedef struct {
    const char* name;
    int      (*start)(void);              /* the ring is ready: record */
    uint32_t (*position)(void);           /* 0..audio_in_ring_bytes()-1 */
    void     (*tick)(void);               /* or NULL */
    void     (*stop)(void);               /* or NULL */
} audio_in_t;

uint8_t* audio_in_ring(void);
uint32_t audio_in_ring_bytes(void);

/* the computer's own input (audio.c, at boot) */
void audio_in_register(const audio_in_t* src, const char* name);
/* a USB microphone: recording comes from it while it is plugged in */
void audio_in_plug(const audio_in_t* src, const char* name);
void audio_in_unplug(const audio_in_t* src);

int         audio_in_available(void);
const char* audio_in_name(void);         /* "none" without one */

/* A reader: open starts the source (the first reader) and puts *cursor at
 * what it records now; read copies what was recorded since - 16-bit
 * signed samples, 1 (both channels mixed) or 2 channels at 48 kHz - at
 * most `frames`, without waiting; how many frames (0: nothing new, -1: no
 * source). A reader that fell more than the ring behind skips ahead. */
int  audio_in_open(uint64_t* cursor);
int  audio_in_read(uint64_t* cursor, int16_t* out, uint32_t frames, int channels);
void audio_in_close(void);
/* the loudest sample in the last ~100 ms (0..32767) */
int  audio_in_level(void);
/* the timer interrupt (audio.c's audio_tick) */
void audio_in_tick(void);

/* lsaudio / lsmic */
void audio_in_list(void);

/* (host tests: what the ring and the counts hold) */
uint64_t audio_in_recorded(void);

#endif
