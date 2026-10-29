/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "fs.h"
#include "fsdisk.h"
#include "kstring.h"
#include "kheap.h"

/* the line holding `key` in text: its start and end, or NULL */
static const char* find_line(const char* text, const char* key, const char** end) {
    size_t kl = strlen(key);
    const char* p = text;
    while (*p) {
        const char* e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        const char* s = p;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        if ((size_t)(e - s) > kl && strncmp(s, key, kl) == 0 && s[kl] == '=') {
            *end = e;
            return p;
        }
        p = *e ? e + 1 : e;
    }
    return NULL;
}

int cfg_get(const char* path, const char* key, char* out, int cap) {
    int idx = fs_find_file(path);
    if (idx < 0 || cap <= 0) return 0;
    fs_pin(idx);
    fs_file_t* f = fs_get_file(idx);
    if (!f || !f->used || !f->loaded || !f->content) { fs_unpin(idx); return 0; }
    const char* end;
    const char* line = find_line(f->content, key, &end);
    if (!line) { fs_unpin(idx); return 0; }
    const char* v = strchr(line, '=') + 1;
    int n = 0;
    while (v < end && *v != '\r' && n < cap - 1) out[n++] = *v++;
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    out[n] = '\0';
    fs_unpin(idx);
    return 1;
}

int cfg_set(const char* path, const char* key, const char* value, const char* header) {
    const char* old = "";
    int idx = fs_find_file(path);
    if (idx >= 0) {
        /* Keep the old text resident while allocating the replacement. */
        fs_pin(idx);
        fs_file_t* f = fs_get_file(idx);
        if (!f || !f->used || !f->loaded || !f->content) { fs_unpin(idx); return -1; }
        old = f->content;
    } else if (!value) return 0;

    uint32_t old_len = (uint32_t)strlen(old);
    uint32_t cap = old_len + strlen(key) + (value ? strlen(value) : 0) +
                   (header ? strlen(header) : 0) + 8;
    char* buf = (char*)kmalloc(cap);
    if (!buf) { if (idx >= 0) fs_unpin(idx); return -1; }
    buf[0] = '\0';
    if (!old[0] && header) kstrlcat(buf, header, cap);

    const char* end;
    const char* line = find_line(old, key, &end);
    if (line) {
        size_t pre = (size_t)(line - old);
        size_t n = strlen(buf);
        memcpy(buf + n, old, pre);
        buf[n + pre] = '\0';
    } else {
        kstrlcat(buf, old, cap);
        size_t n = strlen(buf);
        if (n && buf[n - 1] != '\n') kstrlcat(buf, "\n", cap);
    }
    if (value) {
        kstrlcat(buf, key, cap);
        kstrlcat(buf, "=", cap);
        kstrlcat(buf, value, cap);
        kstrlcat(buf, "\n", cap);
    }
    if (line && *end) kstrlcat(buf, end + 1, cap);

    uint32_t len = (uint32_t)strlen(buf);
    /* Preserve formatting/unknown keys, but avoid writing identical bytes. */
    int r = old_len == len && !memcmp(old, buf, len) ? 0
          : fs_write_path(path, buf, len) >= 0 ? 0 : -1;
    kfree(buf);
    if (idx >= 0) fs_unpin(idx);
    return r;
}

int cfg_persist(void) {
    if (!fsdisk_is_installed()) return 0;
    return fsdisk_sync() == 0;
}
