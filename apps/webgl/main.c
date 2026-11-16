/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The WebGL worker: one per web page WebGL context, started by
 * the kernel (kernel/remote_page.c) under the web workers restrictions,
 * plus reading Mesa and the GPU (kernel/app.c: app_spawn_gpu_worker). It speaks the worker
 * protocol (sdk/include/banana_web_worker.h): the drawing buffer size, then
 * command streams to play with OpenGL ES 2.0 (gl.c). Kept apart from the page
 * renderer: only this process can reach the GPU. */
#include <banana.h>
#include <banana_web_worker.h>
#include <stdlib.h>
#include <string.h>

int ww_gl_execute(unsigned int op, const int* a, const void* data, unsigned int n,
                     unsigned char** out, unsigned int* out_len);

static ww_command_t cmd;
static unsigned char packet[WW_CHUNK + sizeof(ww_io_t)];

static int ipc(int op, const void* in, unsigned int n, void* out, unsigned int cap) {
    return __banana->web_ipc(op, in, n, out, cap);
}

static void output(const void* data, unsigned int n) {
    unsigned int off = 0;
    while (off < n) {
        unsigned int part = n - off < WW_CHUNK ? n - off : WW_CHUNK;
        ww_io_t h = { off, WW_RESULT };
        memcpy(packet, &h, sizeof(h));
        memcpy(packet + sizeof(h), (const unsigned char*)data + off, part);
        if (ipc(WW_OUTPUT, packet, sizeof(h) + part, NULL, 0) < 0) __banana->exit(121);
        off += part;
    }
}

static void* input(unsigned int n) {
    if (n > WW_GL_COMMANDS) __banana->exit(122);
    unsigned char* b = malloc((size_t)n + 1);
    if (!b) __banana->exit(120);
    unsigned int off = 0;
    while (off < n) {
        ww_io_t h = { off, 0 };
        unsigned int part = n - off < WW_CHUNK ? n - off : WW_CHUNK;
        if (ipc(WW_INPUT, &h, sizeof(h), b + off, part) != (int)part) __banana->exit(121);
        off += part;
    }
    return b;
}

static void execute(void) {
    unsigned char* data = cmd.length ? input(cmd.length) : NULL;
    unsigned char* out = NULL;
    unsigned int out_len = 0;
    int result = -1;
    if (cmd.op == WW_GL_INIT || cmd.op == WW_GL_EXEC)
        result = ww_gl_execute(cmd.op, cmd.a, data, cmd.length, &out, &out_len);
    else if (cmd.op == WW_INIT) result = 0;
    free(data);
    if (out_len) output(out, out_len);
    free(out);
    ww_state_t s;
    memset(&s, 0, sizeof(s));
    s.version = WW_VERSION;
    s.result = result;
    ipc(WW_FINISH, &s, sizeof(s), NULL, 0);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!BANANA_HAS(__banana, web_ipc)) return 125;
    unsigned int last = 0;
    for (;;) {
        int n = ipc(WW_WAIT, NULL, 0, &cmd, sizeof(cmd));
        if (n < 0) return 121;
        /* (a page drawing frames sends the next soon: looked for every
         *  millisecond for a second after each, else every 10) */
        if (!n) { __banana->sleep_ms(banana_ticks() - last < 1000u ? 1 : 10); continue; }
        if (n != (int)sizeof(cmd) || cmd.version != WW_VERSION) return 121;
        execute();
        last = banana_ticks();
    }
}
