/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The SDK's OpenGL windows (sdk/lib/gl3.c) on the Linux build of Banana OS's
 * libOSMesa: a window's frames read back a frame behind through pixel
 * buffers (BGL3_ASYNC=1 forces it on softpipe; on a GPU it is the default) -
 * each swap shows the frame before, right side up, with the app's GL state
 * (bound framebuffer, pixel buffer, pack settings) left as it was; frames
 * read back by bgl3_pixels are still the current ones.
 *   tools/tests/gl3_host/run.sh */
#include "GL/gl3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int g_updates;
static int fails;
#define CHECK(c, ...) do { printf((c) ? "ok   " : "FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static void frame(float r, float g, float b) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 48);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* a marker: the bottom-left 8x8 white */
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 8, 8);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

int main(void) {
    int pixels = !getenv("GL3_HOST_NO_PIXELS");      /* (a GPU model that does not draw: the path only) */
    static unsigned int px[64 * 48];
    bwin_t win = { 1, 64, 48, px };
    bgl3_ctx_t* gl = bgl3_create(&win);
    if (!gl) { printf("FAIL no context: %s\n", bgl3_error()); return 1; }
    printf("renderer: %s\n", (const char*)glGetString(GL_RENDERER));
    /* the app's own state, which a swap must leave alone */
    GLuint fbo, tex, pbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, 1024, NULL, GL_STREAM_READ);

    const float col[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 } };
    const unsigned want[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00 };
    for (int f = 0; f < 4; f++) {
        frame(col[f][0], col[f][1], col[f][2]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        memset(px, 0x55, sizeof(px));
        bgl3_swap(gl);
        GLint fb = 0, pb = 0, al = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pb);
        glGetIntegerv(GL_PACK_ALIGNMENT, &al);
        CHECK(fb == (GLint)fbo && pb == (GLint)pbo && al == 1 && glGetError() == GL_NO_ERROR,
              "swap %d: the app's framebuffer, pixel buffer and pack alignment kept, no GL error", f);
        if (!pixels) continue;
        if (f == 0) {
            CHECK(px[10 * 64 + 30] == 0, "swap 0: nothing drawn shown yet - black (its frame is copied during the next)");
        } else {
            unsigned mid = px[10 * 64 + 30], corner = px[47 * 64 + 2], top = px[2];
            CHECK(mid == want[f - 1], "swap %d: the frame before shown (%06x, want %06x)", f, mid, want[f - 1]);
            CHECK(corner == 0xFFFFFF && top == want[f - 1], "swap %d: right side up (the marker at the bottom left: %06x)", f, corner);
        }
    }
    CHECK(g_updates == 4, "every swap updated the window (%d)", g_updates);
    /* bgl3_pixels: the frame as it is now */
    frame(0, 1, 1);
    int w, h;
    const unsigned int* p = bgl3_pixels(gl, &w, &h);
    if (pixels) CHECK(p && w == 64 && h == 48 && (p[10 * 64 + 30] & 0xFFFFFF) == 0x00FFFF, "bgl3_pixels: the current frame (%06x)", p ? p[10 * 64 + 30] & 0xFFFFFF : 0);
    bgl3_destroy(gl);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
