/* the kernel font, built for the host tests */
#include <stdint.h>
#define FONT8X8_H
#include "../../kernel/font8x8.c"

/* the system fonts for the host tests: fonts/ of the source tree (or $FONTDIR).
 * Build a test with ../kernel/font.c ../kernel/ttf.c -DFONT_HOST -I../kernel */
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/font.h"

void host_fonts(void) {
    static const char* names[FONT_FACES] = { "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf" };
    const char* dir = getenv("FONTDIR") ? getenv("FONTDIR") : "../fonts";
    for (int i = 0; i < FONT_FACES; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE* f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "font_host: no %s (set FONTDIR)\n", path); continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char* b = malloc((size_t)n);
        if (b && fread(b, 1, (size_t)n, f) == (size_t)n) font_register(i, b, (uint32_t)n);
        fclose(f);
    }
}
