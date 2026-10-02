#include "clipboard.h"
#include "kheap.h"
#include "kstring.h"

static char*    g_text;
static uint32_t g_len;
static uint32_t g_gen;

void clipboard_set(const char* text, uint32_t len) {
    if (len > CLIPBOARD_MAX) len = CLIPBOARD_MAX;
    char* t = (char*)kmalloc(len + 1);
    if (!t) return;
    memcpy(t, text, len);
    t[len] = 0;
    if (g_text) kfree(g_text);
    g_text = t;
    g_len = len;
    g_gen++;
}

const char* clipboard_get(uint32_t* len) {
    if (len) *len = g_text ? g_len : 0;
    return g_text ? g_text : "";
}

uint32_t clipboard_generation(void) { return g_gen; }
