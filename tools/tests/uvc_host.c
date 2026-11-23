/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tools/tests/uvc.c's host side: files and a JPEG decoder (stb_image, the
 * full one), in a file of its own - the driver's file sees only the
 * kernel's headers. */
#include <stdio.h>
#include <stdlib.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_SIMD
#include "../../third_party/stb/stb_image.h"

int host_read_file(const char* path, unsigned char** data, unsigned int* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = malloc((size_t)n);
    if (!*data || fread(*data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    *len = (unsigned int)n;
    return 0;
}

unsigned char* host_jpeg_decode(const unsigned char* data, int len, int* w, int* h) {
    int comp = 0;
    return stbi_load_from_memory(data, len, w, h, &comp, 3);
}

void host_free(void* p) { free(p); }
