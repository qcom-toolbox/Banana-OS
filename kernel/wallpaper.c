#include "wallpaper.h"
#include "wallpaper_data.h"
#include "fs.h"
#include "fb.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"

static const wallpaper_preset_t g_presets[] = {
    {"Midnight Blue", "assets/wallpapers/midnight-blue.png", 0x00151F30u, wallpaper_midnight_blue},
    {"Forest Night",  "assets/wallpapers/forest-night.png",  0x00162A1Eu, wallpaper_forest_night},
    {"Graphite",      "assets/wallpapers/graphite.png",      0x00252528u, wallpaper_graphite},
    {"Royal Purple",  "assets/wallpapers/royal-purple.png",  0x00261D3Au, wallpaper_royal_purple},
    {"Sunset Grid",   "assets/wallpapers/sunset-grid.png",   0x0030292Au, wallpaper_sunset_grid},
    {"Ocean Wave",    "assets/wallpapers/ocean-wave.png",    0x00131F2Du, wallpaper_ocean_wave},
    {"Cyber Mint",    "assets/wallpapers/cyber-mint.png",    0x00152A2Au, wallpaper_cyber_mint},
    {"Amber Mesh",    "assets/wallpapers/amber-mesh.png",    0x0033261Bu, wallpaper_amber_mesh},
    {"Azure Flow",    "assets/wallpapers/azure-flow.jpg",    0x00142E5Cu, wallpaper_azure_flow},
    {"Plain Black",   "assets/wallpapers/plain-black.png",   0x00121214u, wallpaper_plain_black},
};
#define PRESET_COUNT ((int)(sizeof(g_presets) / sizeof(g_presets[0])))

static int          g_preset = WALLPAPER_DEFAULT_PRESET;
static char         g_file[FS_PATH_LEN];
static image_mode_t g_mode = IMAGE_FILL;
static uint32_t*    g_custom;            /* rendered user image, screen sized */
static int          g_custom_w, g_custom_h;
static uint32_t     g_gen = 1;
static int          g_config_loaded;

int wallpaper_preset_count(void) { return PRESET_COUNT; }

const wallpaper_preset_t* wallpaper_preset(int i) {
    return (i >= 0 && i < PRESET_COUNT) ? &g_presets[i] : NULL;
}

int wallpaper_find_preset(const char* s) {
    uint32_t n;
    int used = k_parse_u32(s, &n);
    if (used && !s[used] && n >= 1 && n <= (uint32_t)PRESET_COUNT) return (int)n - 1;
    for (int i = 0; i < PRESET_COUNT; i++)
        if (strcasecmp(g_presets[i].name, s) == 0) return i;
    /* also accept the file-style name: "azure-flow" */
    for (int i = 0; i < PRESET_COUNT; i++) {
        const char* f = strrchr(g_presets[i].file, '/') + 1;
        size_t l = strlen(s);
        if (strncasecmp(f, s, l) == 0 && f[l] == '.') return i;
    }
    return -1;
}

int          wallpaper_current_preset(void) { return g_custom ? -1 : g_preset; }
const char*  wallpaper_current_file(void)   { return g_custom ? g_file : ""; }
image_mode_t wallpaper_current_mode(void)   { return g_mode; }
uint32_t     wallpaper_generation(void)     { return g_gen; }

int wallpaper_is_image_name(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".bmp") == 0 ||
           strcasecmp(dot, ".gif") == 0;
}

/* Bilinear upscale of a preset's 200x150 thumbnail-sized source to the
 * screen (integer 16.16 fixed point: the kernel never uses the FPU). */
static void render_preset(const uint8_t* pixels, uint32_t* dst, int w, int h, int stride) {
    image_t src = { (uint8_t*)pixels, WALLPAPER_IMG_W, WALLPAPER_IMG_H };
    if (stride == w) {
        image_render(&src, dst, w, h, IMAGE_STRETCH, 0);
        return;
    }
    uint32_t* tmp = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    if (!tmp) { for (int y = 0; y < h; y++) memset32(dst + y * stride, 0, (uint32_t)w); return; }
    image_render(&src, tmp, w, h, IMAGE_STRETCH, 0);
    for (int y = 0; y < h; y++) memcpy(dst + y * stride, tmp + y * w, (uint32_t)w * 4u);
    kfree(tmp);
}

static int set_file_at(const char* path, image_mode_t mode, int w, int h, char* err, uint32_t errlen, int persist);

static void save_config_now(void);
static int set_file(const char* path, image_mode_t mode, char* err, uint32_t errlen, int persist);

/* ── the pictures used lately ── */
static char      g_recent[WALLPAPER_RECENT_MAX][FS_PATH_LEN];
static int       g_nrecent;
static uint32_t* g_thumb[WALLPAPER_RECENT_MAX];
static int       g_thumb_w[WALLPAPER_RECENT_MAX], g_thumb_h[WALLPAPER_RECENT_MAX];
static int       g_thumb_bad[WALLPAPER_RECENT_MAX];

int         wallpaper_recent_count(void) { return g_nrecent; }
const char* wallpaper_recent(int i) { return i >= 0 && i < g_nrecent ? g_recent[i] : ""; }

static void recent_drop(int i) {
    kfree(g_thumb[i]);
    for (int k = i; k < g_nrecent - 1; k++) {
        memcpy(g_recent[k], g_recent[k + 1], FS_PATH_LEN);
        g_thumb[k] = g_thumb[k + 1];
        g_thumb_w[k] = g_thumb_w[k + 1]; g_thumb_h[k] = g_thumb_h[k + 1];
        g_thumb_bad[k] = g_thumb_bad[k + 1];
    }
    g_nrecent--;
    g_thumb[g_nrecent] = NULL;
    g_thumb_bad[g_nrecent] = 0;
}

/* to the front of the list (added if new) */
static void recent_add(const char* path) {
    for (int i = 0; i < g_nrecent; i++)
        if (!strcmp(g_recent[i], path)) {
            if (i == 0) return;
            char p[FS_PATH_LEN];
            uint32_t* t = g_thumb[i];
            int tw = g_thumb_w[i], th = g_thumb_h[i], bad = g_thumb_bad[i];
            kstrlcpy(p, path, sizeof(p));
            g_thumb[i] = NULL;
            recent_drop(i);
            for (int k = g_nrecent; k > 0; k--) {
                memcpy(g_recent[k], g_recent[k - 1], FS_PATH_LEN);
                g_thumb[k] = g_thumb[k - 1];
                g_thumb_w[k] = g_thumb_w[k - 1]; g_thumb_h[k] = g_thumb_h[k - 1];
                g_thumb_bad[k] = g_thumb_bad[k - 1];
            }
            kstrlcpy(g_recent[0], p, FS_PATH_LEN);
            g_thumb[0] = t; g_thumb_w[0] = tw; g_thumb_h[0] = th; g_thumb_bad[0] = bad;
            g_nrecent++;
            return;
        }
    if (g_nrecent == WALLPAPER_RECENT_MAX) recent_drop(WALLPAPER_RECENT_MAX - 1);
    for (int k = g_nrecent; k > 0; k--) {
        memcpy(g_recent[k], g_recent[k - 1], FS_PATH_LEN);
        g_thumb[k] = g_thumb[k - 1];
        g_thumb_w[k] = g_thumb_w[k - 1]; g_thumb_h[k] = g_thumb_h[k - 1];
        g_thumb_bad[k] = g_thumb_bad[k - 1];
    }
    kstrlcpy(g_recent[0], path, FS_PATH_LEN);
    g_thumb[0] = NULL;
    g_thumb_bad[0] = 0;
    g_nrecent++;
}

void wallpaper_recent_remove(int i) {
    if (i < 0 || i >= g_nrecent) return;
    recent_drop(i);
    g_gen++;
    save_config_now();
}

const uint32_t* wallpaper_recent_thumb(int i, int w, int h) {
    if (i < 0 || i >= g_nrecent || g_thumb_bad[i]) return NULL;
    if (g_thumb[i] && g_thumb_w[i] == w && g_thumb_h[i] == h) return g_thumb[i];
    kfree(g_thumb[i]);
    g_thumb[i] = NULL;
    int idx = fs_find_file(g_recent[i]);
    fs_file_t* f = idx >= 0 ? fs_get_file(idx) : NULL;
    image_t img;
    char err[64];
    if (!f) { g_thumb_bad[i] = 1; return NULL; }
    fs_pin(idx);
    int bad = image_decode((const uint8_t*)f->content, f->size, &img, err, sizeof(err)) != 0;
    fs_unpin(idx);
    if (bad) { g_thumb_bad[i] = 1; return NULL; }
    uint32_t* px = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    if (px) image_render(&img, px, w, h, IMAGE_FILL, 0x00101418u);
    image_free(&img);
    g_thumb[i] = px;
    g_thumb_w[i] = w;
    g_thumb_h[i] = h;
    return px;
}

void wallpaper_render(uint32_t* dst, int w, int h, int stride) {
    if (g_custom && (g_custom_w != w || g_custom_h != h)) {
        /* another size (a new resolution): the picture again, at this one */
        char path[FS_PATH_LEN], err[96];
        kstrlcpy(path, g_file, sizeof(path));
        if (set_file_at(path, g_mode, w, h, err, sizeof(err), 0) != 0)
            klog("wallpaper: %s at %dx%d: %s\n", path, w, h, err);
    }
    if (g_custom && g_custom_w == w && g_custom_h == h) {
        for (int y = 0; y < h; y++)
            memcpy(dst + y * stride, g_custom + y * w, (uint32_t)w * 4u);
        return;
    }
    render_preset(g_presets[g_preset].pixels, dst, w, h, stride);
}

static void save_config(void) {
    char buf[(FS_PATH_LEN + 16) * (WALLPAPER_RECENT_MAX + 1) + 96];
    if (g_custom)
        ksnprintf(buf, sizeof(buf), "# Banana OS wallpaper - set with `wallpaper` or Settings\n"
                                    "file %s %s\n", image_mode_name(g_mode), g_file);
    else
        ksnprintf(buf, sizeof(buf), "# Banana OS wallpaper - set with `wallpaper` or Settings\n"
                                    "preset %s\n", g_presets[g_preset].name);
    for (int i = 0; i < g_nrecent; i++) {
        uint32_t n = (uint32_t)strlen(buf);
        ksnprintf(buf + n, sizeof(buf) - n, "recent %s\n", g_recent[i]);
    }
    fs_write_path(WALLPAPER_CONFIG, buf, (uint32_t)strlen(buf));
}
static void save_config_now(void) { save_config(); }

static void drop_custom(void) {
    kfree(g_custom);
    g_custom = NULL;
    g_file[0] = '\0';
}

void wallpaper_set_preset(int i) {
    if (i < 0 || i >= PRESET_COUNT) return;
    drop_custom();
    g_preset = i;
    g_gen++;
    save_config();
}

/* the picture in a file, rendered at w x h for the given mode */
static int set_file_at(const char* path, image_mode_t mode, int w, int h, char* err, uint32_t errlen, int persist) {
    int idx = fs_find_file(path);
    if (idx < 0) { ksnprintf(err, errlen, "no such file: %s", path); return -1; }
    fs_file_t* f = fs_get_file(idx);

    image_t img;
    fs_pin(idx);                         /* (decoding a big photo takes a while) */
    int bad = image_decode((const uint8_t*)f->content, f->size, &img, err, errlen) != 0;
    fs_unpin(idx);
    if (bad) return -1;
    uint32_t* px = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    if (!px) {
        image_free(&img);
        ksnprintf(err, errlen, "out of memory");
        return -1;
    }
    image_render(&img, px, w, h, mode, 0x00101418u);
    klog("wallpaper: %s %dx%d -> %dx%d (%s)\n", path, img.w, img.h, w, h, image_mode_name(mode));
    image_free(&img);

    drop_custom();
    g_custom = px;
    g_custom_w = w;
    g_custom_h = h;
    g_mode = mode;
    fs_file_path(idx, g_file, sizeof(g_file));
    recent_add(g_file);
    g_gen++;
    if (persist) save_config();
    return 0;
}

int wallpaper_set_mode(image_mode_t mode, char* err, uint32_t errlen) {
    if (!g_custom) { ksnprintf(err, errlen, "the wallpaper is not a picture"); return -1; }
    char path[FS_PATH_LEN];
    kstrlcpy(path, g_file, sizeof(path));
    return set_file(path, mode, err, errlen, 1);
}

/* at the screen's size (the desktop's buffers follow it, up to 2560x1600) */
static int set_file(const char* path, image_mode_t mode, char* err, uint32_t errlen, int persist) {
    const fb_info_t* fi = fb_info();
    if (!fb_available() || !fi || fi->width == 0 || fi->height == 0) {
        ksnprintf(err, errlen, "no graphical framebuffer (wallpapers need the GUI)");
        return -1;
    }
    int w = (int)fi->width, h = (int)fi->height;
    if (w > 2560) w = 2560;
    if (h > 1600) h = 1600;
    return set_file_at(path, mode, w, h, err, errlen, persist);
}

int wallpaper_set_file(const char* path, image_mode_t mode, char* err, uint32_t errlen) {
    g_config_loaded = 1;     /* an explicit choice beats whatever is saved */
    return set_file(path, mode, err, errlen, 1);
}

void wallpaper_load_config(void) {
    if (g_config_loaded) return;
    g_config_loaded = 1;
    int idx = fs_find_file(WALLPAPER_CONFIG);
    if (idx < 0) return;
    const char* p = fs_get_file(idx)->content;
    while (*p) {
        const char* nl = strchr(p, '\n');
        uint32_t n = nl ? (uint32_t)(nl - p) : (uint32_t)strlen(p);
        char line[FS_PATH_LEN + 32];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl ? nl + 1 : p + n;

        if (strncmp(line, "recent ", 7) == 0) {
            if (g_nrecent < WALLPAPER_RECENT_MAX && fs_find_file(line + 7) >= 0) {
                int dup = 0;
                for (int i = 0; i < g_nrecent; i++) dup |= !strcmp(g_recent[i], line + 7);
                if (!dup) { kstrlcpy(g_recent[g_nrecent], line + 7, FS_PATH_LEN); g_nrecent++; }
            }
        } else if (strncmp(line, "preset ", 7) == 0) {
            int i = wallpaper_find_preset(line + 7);
            if (i >= 0) { drop_custom(); g_preset = i; g_gen++; }
        } else if (strncmp(line, "file ", 5) == 0) {
            char* mode_s = line + 5;
            char* path = strchr(mode_s, ' ');
            if (!path) continue;
            *path++ = '\0';
            image_mode_t mode = IMAGE_FILL;
            image_mode_parse(mode_s, &mode);
            char err[128];
            if (set_file(path, mode, err, sizeof(err), 0) != 0)
                klog("wallpaper: saved image %s unusable (%s), keeping the preset\n", path, err);
        }
    }
}
