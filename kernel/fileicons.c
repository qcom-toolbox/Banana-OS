/*
 * File, folder and place icons (fileicons.h): little drawings on a 16 x 16
 * grid, each cell `u` pixels (1 at 16 px, 2 at 32, 3 at 48).
 */
#include "fileicons.h"
#include "gfx.h"
#include "kstring.h"

static int g_x, g_y, g_u;
static void R(int x, int y, int w, int h, uint32_t c) { gfx_fill_rect(g_x + x * g_u, g_y + y * g_u, w * g_u, h * g_u, c); }

/* ── kinds of files ─────────────────────────────────────────────── */

static const struct { const char* ext; fileicon_t kind; const char* type; } EXT[] = {
    { "txt", FI_TEXT, "Text Document" }, { "md", FI_TEXT, "Markdown text" }, { "log", FI_TEXT, "Log file" },
    { "conf", FI_TEXT, "Settings file" }, { "cfg", FI_TEXT, "Settings file" }, { "ini", FI_TEXT, "Settings file" },
    { "csv", FI_TEXT, "CSV table" }, { "json", FI_CODE, "JSON data" }, { "xml", FI_CODE, "XML document" },
    { "png", FI_IMAGE, "PNG image" }, { "jpg", FI_IMAGE, "JPEG image" }, { "jpeg", FI_IMAGE, "JPEG image" },
    { "gif", FI_IMAGE, "GIF image" }, { "bmp", FI_IMAGE, "Bitmap image" }, { "webp", FI_IMAGE, "WebP image" },
    { "svg", FI_IMAGE, "SVG drawing" }, { "ico", FI_IMAGE, "Icon" },
    { "mp3", FI_AUDIO, "MP3 audio" }, { "wav", FI_AUDIO, "WAV audio" }, { "flac", FI_AUDIO, "FLAC audio" },
    { "ogg", FI_AUDIO, "Ogg audio" }, { "oga", FI_AUDIO, "Ogg audio" }, { "opus", FI_AUDIO, "Opus audio" },
    { "m4a", FI_AUDIO, "MPEG-4 audio" }, { "aac", FI_AUDIO, "AAC audio" }, { "wma", FI_AUDIO, "Windows Media audio" },
    { "mp4", FI_VIDEO, "MP4 video" }, { "m4v", FI_VIDEO, "MP4 video" }, { "mkv", FI_VIDEO, "Matroska video" },
    { "webm", FI_VIDEO, "WebM video" }, { "avi", FI_VIDEO, "AVI video" }, { "mov", FI_VIDEO, "QuickTime video" },
    { "mpg", FI_VIDEO, "MPEG video" }, { "mpeg", FI_VIDEO, "MPEG video" }, { "wmv", FI_VIDEO, "Windows Media video" },
    { "flv", FI_VIDEO, "Flash video" }, { "ts", FI_VIDEO, "MPEG-TS video" }, { "3gp", FI_VIDEO, "3GP video" },
    { "bpk", FI_PACKAGE, "Banana OS app" },
    { "zip", FI_ARCHIVE, "ZIP archive" }, { "gz", FI_ARCHIVE, "Gzip archive" }, { "tgz", FI_ARCHIVE, "Gzip archive" },
    { "tar", FI_ARCHIVE, "Tar archive" }, { "7z", FI_ARCHIVE, "7-Zip archive" }, { "xz", FI_ARCHIVE, "XZ archive" },
    { "bz2", FI_ARCHIVE, "Bzip2 archive" }, { "rar", FI_ARCHIVE, "RAR archive" }, { "z", FI_ARCHIVE, "Compressed file" },
    { "iso", FI_DISK, "Disc image" }, { "img", FI_DISK, "Disk image" },
    { "exe", FI_PROGRAM, "Windows program" }, { "elf", FI_PROGRAM, "Program" }, { "sh", FI_PROGRAM, "Shell script" },
    { "bin", FI_PROGRAM, "Binary file" }, { "msi", FI_PROGRAM, "Windows installer" },
    { "html", FI_WEB, "Web page" }, { "htm", FI_WEB, "Web page" }, { "php", FI_WEB, "PHP page" }, { "css", FI_CODE, "Style sheet" },
    { "pdf", FI_PDF, "PDF document" },
    { "c", FI_CODE, "C source" }, { "h", FI_CODE, "C header" }, { "cpp", FI_CODE, "C++ source" }, { "py", FI_CODE, "Python script" },
    { "js", FI_CODE, "JavaScript" }, { "asm", FI_CODE, "Assembly source" }, { "rs", FI_CODE, "Rust source" },
};
#define NEXT (int)(sizeof(EXT) / sizeof(EXT[0]))

static int find_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot || dot == name) return -1;
    for (int i = 0; i < NEXT; i++) if (strcasecmp(dot + 1, EXT[i].ext) == 0) return i;
    return -1;
}

fileicon_t fileicon_for_name(const char* name) {
    int i = find_ext(name);
    return i >= 0 ? EXT[i].kind : FI_FILE;
}

const char* fileicon_type_name(const char* name, char* buf, int cap) {
    int i = find_ext(name);
    if (i >= 0) { kstrlcpy(buf, EXT[i].type, (size_t)cap); return buf; }
    const char* dot = strrchr(name, '.');
    if (dot && dot != name && dot[1] && strlen(dot + 1) <= 6) {
        char up[8];
        int n = 0;
        for (const char* p = dot + 1; *p && n < 7; p++) up[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        up[n] = 0;
        ksnprintf(buf, (size_t)cap, "%s File", up);
    } else {
        kstrlcpy(buf, "File", (size_t)cap);
    }
    return buf;
}

/* ── the drawings ───────────────────────────────────────────────── */

/* a sheet of paper with a folded top-right corner */
static void page(void) {
    R(3, 1, 8, 1, 0x008D96A3u);
    R(3, 1, 1, 15, 0x008D96A3u);
    R(3, 15, 11, 1, 0x008D96A3u);
    R(13, 4, 1, 12, 0x008D96A3u);
    R(4, 2, 7, 13, 0x00FFFFFFu);
    R(11, 4, 2, 11, 0x00FFFFFFu);
    /* the fold */
    R(10, 1, 1, 4, 0x008D96A3u);
    R(10, 4, 4, 1, 0x008D96A3u);
    R(11, 2, 1, 2, 0x00DCE3EAu);
    R(12, 3, 1, 1, 0x00DCE3EAu);
}

static void folder(int open) {
    R(1, 2, 6, 2, 0x00D9A63Bu);                          /* the tab */
    R(1, 3, 14, 11, 0x00E3B24Au);                        /* the back */
    if (open) {
        R(3, 5, 10, 6, 0x00FFFFFFu);                     /* a sheet sticking out */
        R(0, 7, 16, 8, 0x00F6D06Bu);
        R(1, 7, 14, 1, 0x00FBE3A2u);
    } else {
        R(1, 5, 14, 9, 0x00F6D06Bu);                     /* the front */
        R(2, 5, 12, 1, 0x00FBE3A2u);
        R(1, 13, 14, 1, 0x00D9A63Bu);
    }
}

static void note(uint32_t c, int x, int y) {
    R(x + 2, y, 1, 6, c);
    R(x + 2, y, 3, 1, c);
    R(x + 4, y + 1, 1, 1, c);
    R(x, y + 5, 3, 2, c);
}

static void picture(int x, int y, int w, int h) {
    R(x, y, w, h, 0x0079B8E6u);
    R(x, y + h - 2, w, 2, 0x004E9A45u);
    R(x + 1, y + h - 3, w / 2, 1, 0x004E9A45u);
    R(x + w - 3, y + 1, 2, 2, 0x00F7D44Cu);
}

static void film(int x, int y, int w, int h) {
    R(x, y, w, h, 0x00453266u);
    for (int i = 0; i + 1 < h; i += 2) { R(x, y + i, 1, 1, 0x00E8E0F4u); R(x + w - 1, y + i, 1, 1, 0x00E8E0F4u); }
    R(x + w / 2 - 1, y + h / 2 - 1, 1, 3, 0x00FFFFFFu);
    R(x + w / 2, y + h / 2, 1, 1, 0x00FFFFFFu);
}

static void star(int x, int y) {
    uint32_t c = 0x00F2C230u;
    R(x + 3, y, 2, 2, c);
    R(x, y + 2, 8, 2, c);
    R(x + 1, y + 4, 6, 1, c);
    R(x + 1, y + 5, 2, 2, c);
    R(x + 5, y + 5, 2, 2, c);
}

void fileicon_draw(fileicon_t k, int x, int y, int size) {
    g_x = x; g_y = y; g_u = size >= 48 ? 3 : size >= 32 ? 2 : 1;
    switch (k) {
    case FI_FOLDER: folder(0); break;
    case FI_FOLDER_OPEN: folder(1); break;
    case FI_FILE: page(); break;
    case FI_TEXT: page(); for (int i = 0; i < 5; i++) R(5, 5 + i * 2, i == 4 ? 4 : 6, 1, 0x009AA5B1u); break;
    case FI_IMAGE: page(); picture(5, 6, 6, 6); break;
    case FI_AUDIO: page(); note(0x00E07B20u, 5, 6); break;
    case FI_VIDEO: page(); film(5, 6, 7, 7); break;
    case FI_PDF: page(); R(4, 9, 9, 4, 0x00C8352Fu); R(5, 10, 2, 2, 0x00FFFFFFu); R(8, 10, 3, 1, 0x00FFFFFFu); break;
    case FI_CODE:
        page();
        R(5, 7, 1, 1, 0x002F6FBFu); R(4, 8, 1, 1, 0x002F6FBFu); R(5, 9, 1, 1, 0x002F6FBFu);
        R(10, 7, 1, 1, 0x002F6FBFu); R(11, 8, 1, 1, 0x002F6FBFu); R(10, 9, 1, 1, 0x002F6FBFu);
        R(8, 6, 1, 1, 0x009AA5B1u); R(7, 8, 1, 1, 0x009AA5B1u); R(7, 10, 1, 1, 0x009AA5B1u);
        R(5, 12, 6, 1, 0x009AA5B1u);
        break;
    case FI_WEB:
        page();
        R(6, 6, 4, 6, 0x003B8EDBu); R(5, 7, 6, 4, 0x003B8EDBu);
        R(7, 7, 2, 2, 0x005CBF62u); R(6, 9, 2, 1, 0x005CBF62u); R(9, 9, 1, 2, 0x005CBF62u);
        break;
    case FI_ARCHIVE:
        page();
        for (int i = 0; i < 10; i += 2) { R(7, 2 + i, 1, 1, 0x008A6D3Bu); R(8, 3 + i, 1, 1, 0x008A6D3Bu); }
        R(6, 12, 4, 3, 0x008A6D3Bu);
        R(7, 13, 2, 1, 0x00E8D7B0u);
        break;
    case FI_PACKAGE:
        R(2, 5, 12, 10, 0x00B88F1Fu);
        R(3, 6, 10, 8, 0x00F4D35Eu);
        R(1, 3, 14, 3, 0x00C9A227u);
        R(2, 4, 12, 1, 0x00F7E08Au);
        R(7, 3, 2, 12, 0x00B88F1Fu);
        break;
    case FI_PROGRAM:
        R(1, 3, 14, 11, 0x002D5E9Au);
        R(1, 3, 14, 2, 0x004C86CFu);
        R(2, 6, 12, 7, 0x00F4F7FBu);
        R(12, 3, 2, 1, 0x00E26D5Cu);
        R(3, 7, 4, 1, 0x009AA5B1u); R(3, 9, 6, 1, 0x009AA5B1u); R(3, 11, 3, 1, 0x009AA5B1u);
        break;
    case FI_COMPUTER:
        R(1, 2, 14, 10, 0x003D4654u);
        R(2, 3, 12, 8, 0x005DA9E9u);
        R(2, 3, 12, 2, 0x0089C2F0u);
        R(7, 12, 2, 2, 0x003D4654u);
        R(4, 14, 8, 1, 0x003D4654u);
        break;
    case FI_DISK:
        R(1, 5, 14, 8, 0x008D96A3u);
        R(1, 5, 14, 3, 0x00C5CCD5u);
        R(2, 9, 12, 3, 0x00A7AFBAu);
        R(12, 10, 2, 1, 0x0039C04Bu);
        break;
    case FI_USB:
        R(6, 1, 4, 4, 0x00C0C6CEu);
        R(7, 2, 1, 1, 0x003D4654u); R(8, 2, 1, 1, 0x003D4654u);
        R(4, 5, 8, 10, 0x003C6FB5u);
        R(5, 6, 6, 1, 0x0071A0DEu);
        R(7, 12, 2, 1, 0x0039C04Bu);
        break;
    case FI_HOME:
        R(7, 1, 2, 1, 0x00B5452Fu);
        R(5, 2, 6, 2, 0x00B5452Fu);
        R(3, 4, 10, 2, 0x00B5452Fu);
        R(1, 6, 14, 2, 0x00B5452Fu);
        R(3, 8, 10, 7, 0x00EDC37Eu);
        R(7, 10, 3, 5, 0x007A4B21u);
        R(4, 9, 2, 2, 0x0079B8E6u);
        break;
    case FI_DOCUMENTS: folder(0); R(4, 7, 8, 6, 0x00FFFFFFu); for (int i = 0; i < 3; i++) R(5, 8 + i * 2, 6, 1, 0x009AA5B1u); break;
    case FI_PICTURES: folder(0); picture(4, 7, 8, 6); break;
    case FI_MUSIC: folder(0); note(0x002F6FBFu, 5, 6); break;
    case FI_VIDEOS: folder(0); film(4, 7, 8, 6); break;
    case FI_DOWNLOADS:
        folder(0);
        R(7, 5, 2, 5, 0x002E9E3Eu);
        R(5, 9, 6, 1, 0x002E9E3Eu);
        R(6, 10, 4, 1, 0x002E9E3Eu);
        R(7, 11, 2, 1, 0x002E9E3Eu);
        break;
    case FI_FAVORITES: star(4, 4); break;
    case FI_LIBRARY:
        R(2, 3, 3, 12, 0x003C6FB5u); R(5, 2, 3, 13, 0x00C8352Fu); R(8, 4, 3, 11, 0x004E9A45u);
        R(11, 3, 3, 12, 0x00E3B24Au); R(1, 15, 14, 1, 0x006B4A2Au);
        break;
    case FI_SEARCH:
        R(3, 2, 6, 1, 0x005B6B7Fu); R(3, 9, 6, 1, 0x005B6B7Fu);
        R(2, 3, 1, 6, 0x005B6B7Fu); R(9, 3, 1, 6, 0x005B6B7Fu);
        R(3, 3, 6, 6, 0x00D8ECFAu);
        R(9, 9, 2, 2, 0x005B6B7Fu); R(11, 11, 2, 2, 0x005B6B7Fu); R(13, 13, 2, 2, 0x005B6B7Fu);
        break;
    default: page(); break;
    }
}
