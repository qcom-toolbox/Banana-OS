#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"

/*
 * Sound. Drivers: Intel HD Audio (HDA: ICH6+ chipsets, QEMU
 * `-device intel-hda -device hda-output`, VirtualBox "Intel HD Audio"),
 * Intel AC'97 (ICH/ICH2-ICH7, QEMU `-device AC97`, VirtualBox's default
 * "ICH AC97"), and the PC speaker for beeps.
 *
 * Whatever the source format, sound is converted to the card's 16-bit
 * stereo stream (48 kHz) and queued in a ring the card plays from by DMA;
 * audio_play() returns as soon as the data is queued.
 */

void        audio_init(void);                  /* PCI probe (kernel_main) */
void        audio_tick(void);                  /* the timer interrupt: keeps the card fed */
int         audio_available(void);             /* a sound card is driven */
const char* audio_device_name(void);           /* "Intel HD Audio (QEMU)", "none" */

/* queues PCM: 8-bit unsigned or 16-bit signed little endian, 1 or 2
 * channels, any rate (resampled); waits while the queue is full. 0, or -1
 * without a card. */
int  audio_play(const void* pcm, uint32_t bytes, int rate, int channels, int bits);
int  audio_busy(void);                        /* 1 while queued sound plays */
uint32_t audio_queued_ms(void);               /* queued sound not yet played */
void audio_stop(void);                        /* drop what is queued */
void audio_wait(void);                        /* until the queue has played */

void audio_set_volume(int percent);           /* 0..100 */
int  audio_get_volume(void);

/* a tone: PC speaker (a square wave from the card if it is driven) */
void audio_beep(int hz, int ms);

/* plays a .wav file (PCM) from the filesystem; 0, or -1 with err */
int  audio_play_wav(const char* path, char* err, int ecap);

/* lsaudio */
void audio_list(void);

#endif
