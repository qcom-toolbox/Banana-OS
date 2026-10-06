#include "codec.h"

void codec_err(char* err, int ecap, const char* msg) {
    if (!err || ecap <= 0) return;
    int i = 0;
    for (; msg[i] && i < ecap - 1; i++) err[i] = msg[i];
    err[i] = 0;
}

/* an ID3v2 tag in front (MP3s, sometimes FLACs): its size */
static uint32_t id3_skip(const uint8_t* d, uint32_t len) {
    if (len < 10 || d[0] != 'I' || d[1] != 'D' || d[2] != '3') return 0;
    uint32_t n = (uint32_t)(d[6] & 0x7F) << 21 | (uint32_t)(d[7] & 0x7F) << 14 | (uint32_t)(d[8] & 0x7F) << 7 | (d[9] & 0x7F);
    n += 10;
    if (d[5] & 0x10) n += 10;                    /* a footer */
    return n < len ? n : len;
}

const char* codec_sniff(const uint8_t* d, uint32_t len) {
    if (len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0) return "WAV";
    if (len >= 4 && memcmp(d, "OggS", 4) == 0) return "OGG";
    uint32_t s = id3_skip(d, len);
    if (len - s >= 4 && memcmp(d + s, "fLaC", 4) == 0) return "FLAC";
    /* MPEG audio: a frame sync (11 bits) with a sane header */
    for (uint32_t i = s; i + 4 <= len && i < s + 4096; i++) {
        if (d[i] == 0xFF && (d[i + 1] & 0xE0) == 0xE0 && ((d[i + 1] >> 1) & 3) != 0 &&
            (d[i + 2] >> 4) != 15 && ((d[i + 2] >> 2) & 3) != 3) return "MP3";
    }
    return NULL;
}

codec_t* codec_open(const uint8_t* data, uint32_t len, char* err, int ecap) {
    const char* kind = codec_sniff(data, len);
    if (!kind) { codec_err(err, ecap, "not a sound file Banana OS knows (WAV, FLAC, MP3)"); return NULL; }
    if (strcmp(kind, "WAV") == 0) return wav_open(data, len, err, ecap);
    if (strcmp(kind, "FLAC") == 0) { uint32_t s = id3_skip(data, len); return flac_open(data + s, len - s, err, ecap); }
    if (strcmp(kind, "MP3") == 0) return mp3_open(data, len, err, ecap);
    codec_err(err, ecap, "Ogg Vorbis is not supported yet");
    return NULL;
}
