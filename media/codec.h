#ifndef CODEC_H
#define CODEC_H

/*
 * Audio decoders: a whole file in memory in, 16-bit interleaved PCM out.
 * WAV, FLAC and MP3 (wav.c, flac.c, mp3.c); codec_open() sniffs the format.
 */

#ifdef MEDIA_HOST                   /* built for host tests */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define kmalloc(n)  malloc(n)
#define kfree(p)    free(p)
#define kzalloc(n)  calloc(1, (n))
#else
#include "types.h"
#include "kheap.h"
#include "kstring.h"
#endif

typedef struct codec codec_t;
struct codec {
    int      rate, channels;        /* what decode() gives (channels: 1 or 2) */
    uint64_t length;                /* in frames (samples per channel); 0 if unknown */
    const char* name;               /* "WAV", "FLAC", "MP3" */
    /* up to max frames of interleaved samples: how many, 0 at the end, -1 on an error */
    int  (*decode)(codec_t* c, int16_t* out, int max);
    int  (*seek)(codec_t* c, uint64_t frame);       /* 0, or -1 */
    void (*close)(codec_t* c);                      /* frees c */
};

codec_t* codec_open(const uint8_t* data, uint32_t len, char* err, int ecap);
const char* codec_sniff(const uint8_t* data, uint32_t len);   /* "WAV", "FLAC", "MP3", "OGG" or NULL */

codec_t* wav_open(const uint8_t* data, uint32_t len, char* err, int ecap);
codec_t* flac_open(const uint8_t* data, uint32_t len, char* err, int ecap);
codec_t* mp3_open(const uint8_t* data, uint32_t len, char* err, int ecap);

void codec_err(char* err, int ecap, const char* msg);

#endif
