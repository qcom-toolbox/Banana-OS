/* Amethyst Music - the Banana OS app of Amethyst Music
 * (https://music.office-works.ch/, github.com/qcom-toolbox/Amethyst-Music):
 * the server's catalogue, albums, artists and playlists, recommendations
 * and listening history for an account, streamed songs (cached in
 * ~/.cache/amethyst, the next one fetched ahead: no gap between them),
 * the covers (WebP, decoded with FFmpeg), an equalizer and a visualizer.
 *
 * Everything goes through the server's api.php: catalogue, covers and
 * streams are public; an account's history, recommendations, play counts
 * and playlists take the user name and password with each request (that
 * is how the API works), so they are kept in ~/.config/amethyst/account.
 *
 *     amethyst [server url]
 */
#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define WIN_W     960
#define WIN_H     600
#define SIDE_W    210
#define TOP_H     52
#define BAR_H     84
#define ROW_H     30
#define RATE      48000
#define DEFAULT_SERVER "https://music.office-works.ch/"
#define CONF_DIR  "/home/banana/.config/amethyst"
#define CACHE_DIR "/home/banana/.cache/amethyst"
#define CACHE_SONGS 40                    /* songs kept on the disk */

#define C_BG      0x14161Cu
#define C_SIDE    0x0F1116u
#define C_BAR     0x1B1E26u
#define C_ROW     0x1A1D24u
#define C_SEL     0x2E2A40u
#define C_HOVER   0x22252Eu
#define C_TEXT    0xECEEF4u
#define C_DIM     0x8C93A4u
#define C_FAINT   0x4A5060u
#define C_ACCENT  0x9B6CF0u               /* amethyst */
#define C_ACCENT2 0xC9A8FFu
#define C_ERR     0xF0577Au
#define C_PANEL   0x1E2129u

#define F_SANS    BANANA_FONT_SANS
#define F_BOLD    BANANA_FONT_SANS_BOLD

static bwin_t win;
static char g_server[200] = DEFAULT_SERVER;

/* ── the catalogue ────────────────────────────────────────────────── */
typedef struct {
    int  id;
    char title[96];
    char artist[64];
    char album[64];
    char genre[32];
    int  album_id;
    int  dur;                              /* seconds */
    int  plays;
} track_t;

typedef struct { int id; char name[96]; int count; } album_t;
typedef struct {
    int  id;
    char name[96];
    char creator[48];
    int  creator_id;
    int  is_public;
    int* songs;                            /* track indices */
    int  nsongs;
} playlist_t;

#define MAX_TRACKS 8192
static track_t*    g_tracks;               /* (allocated once: never moves) */
static volatile int g_ntracks;
static album_t*    g_albums;
static int         g_nalbums;
static playlist_t* g_pls;
static int         g_npls;
static int         g_rec[50], g_nrec;      /* recommended (track indices) */
static int         g_hist[300], g_nhist;   /* recently played */
static banana_mutex_t g_data_lock = BANANA_MUTEX_INIT;
static volatile int g_data_gen;
static volatile int g_loading = 1;
static char        g_status[160];          /* a message under the views (errors...) */

/* the account */
static char g_user[64], g_pass[128];
static int  g_user_id = -1, g_admin;

static int track_index(int id) {
    for (int i = 0; i < g_ntracks; i++) if (g_tracks[i].id == id) return i;
    return -1;
}

/* ── JSON (what api.php sends: arrays of flat objects) ────────────── */
static void js_ws(const char** p) { while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') (*p)++; }

static void put_utf8(char** o, char* end, unsigned c) {
    char b[4];
    int n;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); n = 4; }
    if (*o + n >= end) return;
    for (int i = 0; i < n; i++) *(*o)++ = b[i];
}

static unsigned hex4(const char* s) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v = v * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

/* a string value into out (cap bytes); the HTML entities the server keeps
 * (&amp; &#039;...) are decoded too */
static void js_string(const char** p, char* out, int cap) {
    char* o = out;
    char* end = out + cap - 1;
    if (**p != '"') { *out = 0; return; }
    (*p)++;
    while (**p && **p != '"') {
        char c = *(*p)++;
        if (c == '\\' && **p) {
            char e = *(*p)++;
            if (e == 'u' && strlen(*p) >= 4) {
                unsigned u = hex4(*p);
                *p += 4;
                if (u >= 0xD800 && u < 0xDC00 && (*p)[0] == '\\' && (*p)[1] == 'u') {   /* a surrogate pair */
                    unsigned lo = hex4(*p + 2);
                    *p += 6;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(&o, end, u);
                continue;
            }
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
        }
        if (o < end) *o++ = c;
    }
    if (**p == '"') (*p)++;
    *o = 0;
    /* HTML entities */
    char* r = out;
    char* w = out;
    while (*r) {
        if (*r == '&') {
            static const struct { const char* e; char c; } ENT[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#039;", '\'' }, { "&#39;", '\'' }, { "&apos;", '\'' } };
            int done = 0;
            for (unsigned k = 0; k < sizeof(ENT) / sizeof(ENT[0]); k++) {
                size_t n = strlen(ENT[k].e);
                if (!strncmp(r, ENT[k].e, n)) { *w++ = ENT[k].c; r += n; done = 1; break; }
            }
            if (done) continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

static void js_skip(const char** p) {
    js_ws(p);
    if (**p == '"') { char tmp[2]; const char* q = *p; (void)q; js_string(p, tmp, 1); return; }
    if (**p == '{' || **p == '[') {
        int depth = 0;
        do {
            if (**p == '"') { char tmp[2]; js_string(p, tmp, 1); continue; }
            if (**p == '{' || **p == '[') depth++;
            else if (**p == '}' || **p == ']') depth--;
            (*p)++;
        } while (**p && depth > 0);
        return;
    }
    while (**p && **p != ',' && **p != '}' && **p != ']') (*p)++;
}

/* one object's fields, as text (numbers too; null: "") */
#define JS_FIELDS 24
typedef struct { char key[24]; char val[200]; } js_field_t;
typedef struct { js_field_t f[JS_FIELDS]; int n; } js_obj_t;

static int js_object(const char** p, js_obj_t* o) {
    o->n = 0;
    js_ws(p);
    if (**p != '{') return -1;
    (*p)++;
    for (;;) {
        js_ws(p);
        if (**p == '}') { (*p)++; return 0; }
        if (**p != '"') return -1;
        char key[24];
        js_string(p, key, sizeof(key));
        js_ws(p);
        if (**p != ':') return -1;
        (*p)++;
        js_ws(p);
        js_field_t* f = o->n < JS_FIELDS ? &o->f[o->n] : NULL;
        if (**p == '"') {
            if (f) js_string(p, f->val, sizeof(f->val)); else js_skip(p);
        } else if (**p == '{' || **p == '[') {
            js_skip(p);
            if (f) f->val[0] = 0;
        } else {
            const char* s = *p;
            js_skip(p);
            if (f) {
                int n = (int)(*p - s);
                if (n >= (int)sizeof(f->val)) n = (int)sizeof(f->val) - 1;
                memcpy(f->val, s, (size_t)n);
                f->val[n] = 0;
                while (n > 0 && (f->val[n - 1] == ' ' || f->val[n - 1] == '\n')) f->val[--n] = 0;
                if (!strcmp(f->val, "null")) f->val[0] = 0;
            }
        }
        if (f) { snprintf(f->key, sizeof(f->key), "%s", key); o->n++; }
        js_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; return 0; }
        return -1;
    }
}

static const char* js_get(const js_obj_t* o, const char* key) {
    for (int i = 0; i < o->n; i++) if (!strcmp(o->f[i].key, key)) return o->f[i].val;
    return "";
}
static int js_int(const js_obj_t* o, const char* key) { return atoi(js_get(o, key)); }

/* the objects of an array, one by one: 1 while there is one */
static int js_array_next(const char** p, js_obj_t* o) {
    js_ws(p);
    if (**p == '[') (*p)++;
    js_ws(p);
    if (**p == ',') { (*p)++; js_ws(p); }
    if (**p != '{') return 0;
    return js_object(p, o) == 0;
}

/* ── the API ──────────────────────────────────────────────────────── */
static void api_url(char* out, int cap, const char* action, const char* extra) {
    snprintf(out, cap, "%sapi.php?action=%s%s", g_server, action, extra ? extra : "");
}

static char* api_get(const char* action, const char* extra, char* err, int ecap) {
    char url[400];
    api_url(url, sizeof(url), action, extra);
    char* data = NULL;
    unsigned long len = 0;
    if (banana_http_get(url, &data, &len, err, ecap) < 0) return NULL;
    return data;
}

/* a POST with the account's name and password (when logged in) and more fields */
static char* api_post(const char* action, const char* fields, int with_account, char* err, int ecap) {
    char url[400], body[900], u[200], pw[400];
    api_url(url, sizeof(url), action, NULL);
    body[0] = 0;
    if (with_account && g_user[0]) {
        banana_url_encode(g_user, u, sizeof(u));
        banana_url_encode(g_pass, pw, sizeof(pw));
        snprintf(body, sizeof(body), "username=%s&password=%s", u, pw);
    }
    if (fields && fields[0]) {
        size_t n = strlen(body);
        snprintf(body + n, sizeof(body) - n, "%s%s", n ? "&" : "", fields);
    }
    char* data = NULL;
    unsigned long len = 0;
    if (banana_http_post(url, body, &data, &len, err, ecap) < 0) return NULL;
    return data;
}

/* "status":"success" in an answer (or its "message") */
static int api_ok(const char* json, char* msg, int cap) {
    const char* p = json;
    js_obj_t o;
    if (!json || js_object(&p, &o) < 0) { if (msg) snprintf(msg, cap, "no answer from the server"); return 0; }
    if (!strcmp(js_get(&o, "status"), "success")) return 1;
    if (msg) snprintf(msg, cap, "%s", js_get(&o, "message")[0] ? js_get(&o, "message") : "refused by the server");
    return 0;
}

static void parse_track(const js_obj_t* o, track_t* t) {
    memset(t, 0, sizeof(*t));
    t->id = js_int(o, "id");
    snprintf(t->title, sizeof(t->title), "%s", js_get(o, "title"));
    snprintf(t->artist, sizeof(t->artist), "%s", js_get(o, "artist")[0] ? js_get(o, "artist") : "Unknown artist");
    snprintf(t->album, sizeof(t->album), "%s", js_get(o, "album"));
    snprintf(t->genre, sizeof(t->genre), "%s", js_get(o, "genre"));
    t->album_id = js_int(o, "album_id");
    t->dur = js_int(o, "duration");
    t->plays = js_int(o, "play_count");
}

/* a list of tracks (recommend, history) as catalogue indices */
static int parse_track_list(const char* json, int* out, int max) {
    const char* p = json;
    js_obj_t o;
    int n = 0;
    while (json && n < max && js_array_next(&p, &o)) {
        int k = track_index(js_int(&o, "id"));
        if (k >= 0) out[n++] = k;
    }
    return n;
}

static void load_catalogue(void) {
    char err[120];
    char* j = api_get("list", NULL, err, sizeof(err));
    if (!j) { snprintf(g_status, sizeof(g_status), "Cannot reach %s: %s", g_server, err); return; }
    if (!g_tracks) g_tracks = calloc(MAX_TRACKS, sizeof(track_t));
    const char* p = j;
    js_obj_t o;
    int n = 0;
    banana_lock(&g_data_lock);
    while (g_tracks && n < MAX_TRACKS && js_array_next(&p, &o)) parse_track(&o, &g_tracks[n++]);
    g_ntracks = n;
    banana_unlock(&g_data_lock);
    free(j);
    j = api_get("albums", NULL, err, sizeof(err));
    if (j) {
        album_t* a = calloc(1024, sizeof(album_t));
        int na = 0;
        p = j;
        while (a && na < 1024 && js_array_next(&p, &o)) {
            a[na].id = js_int(&o, "id");
            snprintf(a[na].name, sizeof(a[na].name), "%s", js_get(&o, "name"));
            a[na].count = js_int(&o, "track_count");
            na++;
        }
        banana_lock(&g_data_lock);
        free(g_albums);
        g_albums = a;
        g_nalbums = na;
        /* the album names of the tracks (album_tracks has none) */
        for (int i = 0; i < g_ntracks; i++)
            if (!g_tracks[i].album[0] && g_tracks[i].album_id)
                for (int k = 0; k < na; k++) if (a[k].id == g_tracks[i].album_id) snprintf(g_tracks[i].album, sizeof(g_tracks[i].album), "%s", a[k].name);
        banana_unlock(&g_data_lock);
        free(j);
    }
    g_data_gen++;
}

static void load_playlists(void) {
    char err[120];
    char* j = api_post("playlists", NULL, 1, err, sizeof(err));
    if (!j) return;
    playlist_t* pl = calloc(256, sizeof(playlist_t));
    int n = 0;
    const char* p = j;
    js_obj_t o;
    while (pl && n < 256 && js_array_next(&p, &o)) {
        playlist_t* x = &pl[n];
        x->id = js_int(&o, "id");
        snprintf(x->name, sizeof(x->name), "%s", js_get(&o, "name"));
        snprintf(x->creator, sizeof(x->creator), "%s", js_get(&o, "creator"));
        x->creator_id = js_int(&o, "creator_id");
        x->is_public = js_get(&o, "is_public")[0] ? js_int(&o, "is_public") : 1;
        const char* ids = js_get(&o, "song_ids");
        int cnt = 1;
        for (const char* c = ids; *c; c++) if (*c == ',') cnt++;
        x->songs = calloc((size_t)cnt + 1, sizeof(int));
        for (const char* c = ids; x->songs && *c;) {
            int id = atoi(c);
            int k = id > 0 ? track_index(id) : -1;
            if (k >= 0) x->songs[x->nsongs++] = k;
            const char* q = strchr(c, ',');
            if (!q) break;
            c = q + 1;
        }
        n++;
    }
    free(j);
    banana_lock(&g_data_lock);
    for (int i = 0; i < g_npls; i++) free(g_pls[i].songs);
    free(g_pls);
    g_pls = pl;
    g_npls = n;
    banana_unlock(&g_data_lock);
    g_data_gen++;
}

static void load_personal(void) {
    char err[120];
    char* j = api_post("recommend", "limit=30", 1, err, sizeof(err));
    if (j) {
        int tmp[50];
        int n = parse_track_list(j, tmp, 30);
        banana_lock(&g_data_lock);
        memcpy(g_rec, tmp, sizeof(int) * (size_t)n);
        g_nrec = n;
        banana_unlock(&g_data_lock);
        free(j);
    }
    g_nhist = 0;
    if (g_user[0]) {
        j = api_post("history", "limit=60", 1, err, sizeof(err));
        if (j) {
            int tmp[300];
            int n = parse_track_list(j, tmp, 300);
            banana_lock(&g_data_lock);
            memcpy(g_hist, tmp, sizeof(int) * (size_t)n);
            g_nhist = n;
            banana_unlock(&g_data_lock);
            free(j);
        }
    }
    g_data_gen++;
}

/* ── the account ──────────────────────────────────────────────────── */
static void conf_dirs(void) {
    mkdir("/home/banana/.config", 0755);
    mkdir(CONF_DIR, 0755);
    mkdir("/home/banana/.cache", 0755);
    mkdir(CACHE_DIR, 0755);
}

static void account_save(void) {
    conf_dirs();
    FILE* f = fopen(CONF_DIR "/account", "w");
    if (!f) return;
    if (g_user[0]) fprintf(f, "server=%s\nuser=%s\npassword=%s\n", g_server, g_user, g_pass);
    else fprintf(f, "server=%s\n", g_server);
    fclose(f);
}

static void account_load(void) {
    FILE* f = fopen(CONF_DIR "/account", "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "server=", 7) && line[7]) snprintf(g_server, sizeof(g_server), "%s", line + 7);
        else if (!strncmp(line, "user=", 5)) snprintf(g_user, sizeof(g_user), "%s", line + 5);
        else if (!strncmp(line, "password=", 9)) snprintf(g_pass, sizeof(g_pass), "%s", line + 9);
    }
    fclose(f);
}

/* 0, or -1 with msg */
static int account_login(const char* user, const char* pass, int create, char* msg, int cap) {
    char u[200], p[400], body[700], err[120];
    banana_url_encode(user, u, sizeof(u));
    banana_url_encode(pass, p, sizeof(p));
    snprintf(body, sizeof(body), "username=%s&password=%s", u, p);
    if (create) {
        char* j = api_post("register", body, 0, err, sizeof(err));
        if (!j) { snprintf(msg, cap, "%s", err); return -1; }
        int ok = api_ok(j, msg, cap);
        free(j);
        if (!ok) return -1;
    }
    char* j = api_post("login", body, 0, err, sizeof(err));
    if (!j) { snprintf(msg, cap, "%s", err); return -1; }
    int ok = api_ok(j, msg, cap);
    if (ok) {
        const char* q = j;
        js_obj_t o;
        js_object(&q, &o);
        g_user_id = js_int(&o, "user_id");
        g_admin = !strcmp(js_get(&o, "is_admin"), "true");
        snprintf(g_user, sizeof(g_user), "%s", js_get(&o, "username")[0] ? js_get(&o, "username") : user);
        snprintf(g_pass, sizeof(g_pass), "%s", pass);
    }
    free(j);
    return ok ? 0 : -1;
}

/* ── the queue ────────────────────────────────────────────────────── */
enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE };
static int  g_queue[4096];               /* track indices, in play order */
static int  g_qlen, g_qpos = -1;
static int  g_shuffle, g_repeat;
static banana_mutex_t g_q_lock = BANANA_MUTEX_INIT;

static int queue_step(int step, int manual) {
    if (g_qlen == 0) return -1;
    if (g_repeat == REPEAT_ONE && !manual) return g_qpos;
    int p = g_qpos + step;
    if (p >= g_qlen) return g_repeat == REPEAT_ALL || manual ? 0 : -1;
    if (p < 0) return g_repeat == REPEAT_ALL ? g_qlen - 1 : 0;
    return p;
}

static void shuffle_queue(void) {
    if (g_qlen < 2) return;
    int cur = g_qpos >= 0 ? g_queue[g_qpos] : -1;
    for (int i = g_qlen - 1; i > 0; i--) {
        int j = (int)(banana_random() % (unsigned)(i + 1));
        int t = g_queue[i]; g_queue[i] = g_queue[j]; g_queue[j] = t;
    }
    if (cur >= 0)
        for (int i = 0; i < g_qlen; i++) if (g_queue[i] == cur) { g_queue[i] = g_queue[0]; g_queue[0] = cur; g_qpos = 0; break; }
}

/* ── the song cache: downloads, the next song ahead ───────────────── */
static banana_mutex_t g_dl_lock = BANANA_MUTEX_INIT;
static banana_cond_t  g_dl_cond = BANANA_COND_INIT;
static int  g_dl_want[8], g_dl_nwant;    /* track ids to fetch, first first */
static volatile int g_dl_busy = -1;      /* the id being fetched */
static int  g_cached[CACHE_SONGS], g_ncached;   /* ids on the disk, oldest first */
static volatile int g_dl_failed = -1;    /* the last id that could not be fetched */
static char g_dl_err[120];

static void cache_path(int id, char* out, int cap) { snprintf(out, cap, CACHE_DIR "/%d.audio", id); }

static int is_cached(int id) {
    banana_lock(&g_dl_lock);
    int r = 0;
    for (int i = 0; i < g_ncached; i++) if (g_cached[i] == id) r = 1;
    banana_unlock(&g_dl_lock);
    return r;
}

static void cache_scan(void) {
    banana_dirent_t e;
    for (int i = 0; __banana->readdir(CACHE_DIR, i, &e) == 0 && g_ncached < CACHE_SONGS; i++) {
        int id = atoi(e.name);
        if (id > 0 && strstr(e.name, ".audio") && e.size > 0) g_cached[g_ncached++] = id;
    }
}

/* asks for a song (first = 1: before the others) */
static void want_song(int id, int first) {
    banana_lock(&g_dl_lock);
    int dup = 0;
    for (int i = 0; i < g_dl_nwant; i++) if (g_dl_want[i] == id) dup = 1;
    for (int i = 0; i < g_ncached; i++) if (g_cached[i] == id) dup = 1;
    if (!dup && g_dl_busy != id) {
        if (g_dl_nwant == 8) g_dl_nwant--;
        if (first) { memmove(g_dl_want + 1, g_dl_want, sizeof(int) * (size_t)g_dl_nwant); g_dl_want[0] = id; }
        else g_dl_want[g_dl_nwant] = id;
        g_dl_nwant++;
    }
    banana_cond_broadcast(&g_dl_cond);
    banana_unlock(&g_dl_lock);
}

static volatile int g_quit;

static int download_thread(void* arg) {
    (void)arg;
    while (!g_quit) {
        banana_lock(&g_dl_lock);
        while (!g_dl_nwant && !g_quit) banana_cond_timedwait(&g_dl_cond, &g_dl_lock, 500);
        if (g_quit) { banana_unlock(&g_dl_lock); break; }
        int id = g_dl_want[0];
        memmove(g_dl_want, g_dl_want + 1, sizeof(int) * (size_t)--g_dl_nwant);
        g_dl_busy = id;
        banana_unlock(&g_dl_lock);

        char url[400], q[32], path[96], err[120];
        snprintf(q, sizeof(q), "&q=%d", id);
        api_url(url, sizeof(url), "stream", q);
        char* data = NULL;
        unsigned long len = 0;
        int ok = banana_http_get(url, &data, &len, err, sizeof(err)) == 0 && len > 0;
        if (ok) {
            cache_path(id, path, sizeof(path));
            FILE* f = fopen(path, "w");
            ok = f && fwrite(data, 1, len, f) == len;
            if (f) fclose(f);
            if (!ok) snprintf(err, sizeof(err), "the disk is full");
        }
        free(data);
        banana_lock(&g_dl_lock);
        if (ok) {
            if (g_ncached == CACHE_SONGS) {          /* the oldest one goes */
                char old[96];
                cache_path(g_cached[0], old, sizeof(old));
                remove(old);
                memmove(g_cached, g_cached + 1, sizeof(int) * (CACHE_SONGS - 1));
                g_ncached--;
            }
            g_cached[g_ncached++] = id;
        } else {
            g_dl_failed = id;
            snprintf(g_dl_err, sizeof(g_dl_err), "%s", err);
        }
        g_dl_busy = -1;
        banana_cond_broadcast(&g_dl_cond);
        banana_unlock(&g_dl_lock);
    }
    return 0;
}

/* ── the equalizer ────────────────────────────────────────────────── */
#define EQ_BANDS 6
static const double EQ_FREQ[EQ_BANDS] = { 60, 170, 600, 3000, 6000, 14000 };
static const char* const EQ_LABEL[EQ_BANDS] = { "60", "170", "600", "3k", "6k", "14k" };
static volatile int g_eq_db[EQ_BANDS];
static volatile int g_eq_gen;
typedef struct { double b0, b1, b2, a1, a2; double z1[2], z2[2]; } biquad_t;
static biquad_t g_bq[EQ_BANDS];
static int g_bq_gen = -1;

static void eq_design(void) {
    for (int b = 0; b < EQ_BANDS; b++) {
        double A = pow(10.0, g_eq_db[b] / 40.0), w0 = 2 * M_PI * EQ_FREQ[b] / RATE;
        double alpha = sin(w0) / 2, c = cos(w0);
        double a0 = 1 + alpha / A;
        biquad_t* q = &g_bq[b];
        q->b0 = (1 + alpha * A) / a0;
        q->b1 = (-2 * c) / a0;
        q->b2 = (1 - alpha * A) / a0;
        q->a1 = (-2 * c) / a0;
        q->a2 = (1 - alpha / A) / a0;
    }
    g_bq_gen = g_eq_gen;
}

static void eq_process(float* x, int frames) {
    if (g_bq_gen != g_eq_gen) eq_design();
    for (int b = 0; b < EQ_BANDS; b++) {
        if (!g_eq_db[b]) continue;
        biquad_t* q = &g_bq[b];
        for (int ch = 0; ch < 2; ch++) {
            double z1 = q->z1[ch], z2 = q->z2[ch];
            for (int i = 0; i < frames; i++) {
                double in = x[i * 2 + ch];
                double out = q->b0 * in + z1;
                z1 = q->b1 * in - q->a1 * out + z2;
                z2 = q->b2 * in - q->a2 * out;
                x[i * 2 + ch] = (float)out;
            }
            q->z1[ch] = z1;
            q->z2[ch] = z2;
        }
    }
}

/* ── the player engine (a thread) ─────────────────────────────────── */
#define VIZ_RING 131072
static float g_viz[VIZ_RING];
static volatile unsigned long long g_sent;

typedef struct { unsigned long long at; int song; double offset; } mark_t;
static mark_t g_marks[8];
static int    g_nmarks;

static banana_mutex_t g_e_lock = BANANA_MUTEX_INIT;
static banana_cond_t  g_e_cond = BANANA_COND_INIT;
enum { E_IDLE = 0, E_PLAY };
static volatile int g_cmd;
static volatile int g_cmd_song = -1;
static volatile double g_cmd_pos;
static volatile int g_playing;
static volatile int g_buffering = -1;     /* a song being fetched before it plays */
static volatile int g_paused;
static double g_paused_pos;
static int    g_paused_song = -1;
static volatile int g_no_card;
static volatile int g_counted = -1;       /* the last song whose play was counted */

static unsigned long long played_samples(void) {
    unsigned long long q = (unsigned long long)banana_audio_queued_ms() * RATE / 1000;
    return g_sent > q ? g_sent - q : 0;
}

static int now_playing(double* pos) {
    if (g_paused) { if (pos) *pos = g_paused_pos; return g_paused_song; }
    banana_lock(&g_e_lock);
    unsigned long long p = played_samples();
    int song = -1;
    double at = 0;
    for (int i = 0; i < g_nmarks; i++)
        if (g_marks[i].at <= p) { song = g_marks[i].song; at = g_marks[i].offset + (double)(p - g_marks[i].at) / RATE; }
    banana_unlock(&g_e_lock);
    if (pos) *pos = at;
    return song;
}

static void add_mark(int song, double offset) {
    banana_lock(&g_e_lock);
    unsigned long long p = played_samples();
    int keep = 0;
    for (int i = 0; i < g_nmarks; i++) {
        int later_passed = 0;
        for (int j = 0; j < g_nmarks; j++) if (g_marks[j].at > g_marks[i].at && g_marks[j].at <= p) later_passed = 1;
        if (!later_passed) g_marks[keep++] = g_marks[i];
    }
    g_nmarks = keep;
    if (g_nmarks == 8) { memmove(g_marks, g_marks + 1, 7 * sizeof(mark_t)); g_nmarks = 7; }
    g_marks[g_nmarks++] = (mark_t){ g_sent, song, offset };
    banana_unlock(&g_e_lock);
}

typedef struct {
    AVFormatContext* fmt;
    AVCodecContext*  dec;
    SwrContext*      swr;
    int              idx;
} dec_t;

static void dec_close(dec_t* t) {
    if (t->swr) swr_free(&t->swr);
    if (t->dec) avcodec_free_context(&t->dec);
    if (t->fmt) avformat_close_input(&t->fmt);
    memset(t, 0, sizeof(*t));
}

static int dec_open(dec_t* t, const char* path, double pos) {
    memset(t, 0, sizeof(*t));
    if (avformat_open_input(&t->fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(t->fmt, NULL) < 0) { dec_close(t); return -1; }
    t->idx = av_find_best_stream(t->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (t->idx < 0) { dec_close(t); return -1; }
    AVStream* st = t->fmt->streams[t->idx];
    const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c || !(t->dec = avcodec_alloc_context3(c)) || avcodec_parameters_to_context(t->dec, st->codecpar) < 0 ||
        avcodec_open2(t->dec, c, NULL) < 0) { dec_close(t); return -1; }
    t->dec->pkt_timebase = st->time_base;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&t->swr, &stereo, AV_SAMPLE_FMT_S16, RATE, &t->dec->ch_layout, t->dec->sample_fmt,
                            t->dec->sample_rate, 0, NULL) < 0 || swr_init(t->swr) < 0) { dec_close(t); return -1; }
    if (pos > 0.5) {
        int64_t ts = (int64_t)(pos * AV_TIME_BASE);
        avformat_seek_file(t->fmt, -1, INT64_MIN, ts, ts, 0);
        avcodec_flush_buffers(t->dec);
    }
    return 0;
}

/* the song's file, fetched first if it is not there (waits; -1 if it
 * failed or another command came meanwhile) */
static int song_file(int song, char* path, int cap) {
    if (song < 0 || song >= g_ntracks) return -1;
    int id = g_tracks[song].id;
    cache_path(id, path, cap);
    if (is_cached(id)) return 0;
    want_song(id, 1);
    g_buffering = song;
    banana_lock(&g_dl_lock);
    for (;;) {
        int cached = 0;
        for (int i = 0; i < g_ncached; i++) if (g_cached[i] == id) cached = 1;
        if (cached) break;
        if (g_dl_failed == id || g_quit || g_cmd == E_PLAY) { banana_unlock(&g_dl_lock); g_buffering = -1; return -1; }
        banana_cond_timedwait(&g_dl_cond, &g_dl_lock, 200);
    }
    banana_unlock(&g_dl_lock);
    g_buffering = -1;
    return 0;
}

static float*   g_fbuf;
static int16_t* g_sbuf;
static int      g_bufcap;

static int send_frame(dec_t* t, AVFrame* f, double* skip) {
    int out = swr_get_out_samples(t->swr, f->nb_samples);
    if (out > g_bufcap) {
        float* a = realloc(g_fbuf, (size_t)out * 8);
        int16_t* b = realloc(g_sbuf, (size_t)out * 4);
        if (!a || !b) return -1;
        g_fbuf = a; g_sbuf = b; g_bufcap = out;
    }
    uint8_t* dst[1] = { (uint8_t*)g_sbuf };
    int n = swr_convert(t->swr, dst, out, (const uint8_t**)f->extended_data, f->nb_samples);
    if (n <= 0) return 0;
    for (int i = 0; i < n * 2; i++) g_fbuf[i] = g_sbuf[i] * (1.0f / 32768.0f);
    int from = 0;
    double pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp * av_q2d(t->fmt->streams[t->idx]->time_base) : -1;
    if (*skip > 0 && pts >= 0) {
        if (pts + (double)n / RATE <= *skip) return 0;
        if (pts < *skip) from = (int)((*skip - pts) * RATE);
        if (from > n) from = n;
        *skip = 0;
    }
    float* x = g_fbuf + from * 2;
    int frames = n - from;
    eq_process(x, frames);
    for (int i = 0; i < frames; i++) {
        float l = x[i * 2], r = x[i * 2 + 1];
        g_viz[(g_sent + (unsigned long long)i) % VIZ_RING] = (l + r) * 0.5f;
        int a = (int)(l * 32767.0f), b = (int)(r * 32767.0f);
        g_sbuf[i * 2] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
        g_sbuf[i * 2 + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
    }
    if (g_no_card) banana_sleep((unsigned)(frames * 1000 / RATE));
    else if (banana_play(g_sbuf, (unsigned long)frames * 4, RATE, 2, 16) < 0) g_no_card = 1;
    g_sent += (unsigned long long)frames;
    return 0;
}

/* the song after the current one, fetched ahead */
static void prefetch_next(void) {
    banana_lock(&g_q_lock);
    int np = queue_step(1, 0);
    int next = np >= 0 ? g_queue[np] : -1;
    banana_unlock(&g_q_lock);
    if (next >= 0 && next < g_ntracks) want_song(g_tracks[next].id, 0);
}

static int engine_thread(void* arg) {
    (void)arg;
    dec_t t;
    memset(&t, 0, sizeof(t));
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    double skip = 0;
    int cur_song = -1;
    char path[96];
    while (!g_quit) {
        if (g_cmd == E_PLAY) {
            banana_lock(&g_e_lock);
            int song = g_cmd_song;
            double pos = g_cmd_pos;
            g_cmd = E_IDLE;
            banana_unlock(&g_e_lock);
            banana_audio_stop();
            dec_close(&t);
            banana_lock(&g_e_lock);
            g_nmarks = 0;
            g_sent = 0;
            banana_unlock(&g_e_lock);
            memset(g_bq, 0, sizeof(g_bq));
            g_bq_gen = -1;
            cur_song = -1;
            g_playing = 0;
            if (song >= 0 && song_file(song, path, sizeof(path)) == 0 && dec_open(&t, path, pos) == 0) {
                cur_song = song;
                skip = pos;
                add_mark(song, pos);
                g_playing = 1;
                prefetch_next();
            }
            continue;
        }
        if (!g_playing || cur_song < 0) {
            banana_lock(&g_e_lock);
            if (g_cmd != E_PLAY && !g_quit) banana_cond_timedwait(&g_e_cond, &g_e_lock, 200);
            banana_unlock(&g_e_lock);
            continue;
        }
        int r = av_read_frame(t.fmt, pkt);
        if (r >= 0) {
            if (pkt->stream_index == t.idx && avcodec_send_packet(t.dec, pkt) == 0)
                while (avcodec_receive_frame(t.dec, fr) == 0) {
                    send_frame(&t, fr, &skip);
                    av_frame_unref(fr);
                    if (g_cmd == E_PLAY || g_quit) break;
                }
            av_packet_unref(pkt);
            continue;
        }
        avcodec_send_packet(t.dec, NULL);
        while (avcodec_receive_frame(t.dec, fr) == 0) { send_frame(&t, fr, &skip); av_frame_unref(fr); }
        dec_close(&t);
        banana_lock(&g_q_lock);
        int np = queue_step(1, 0);
        int next = np >= 0 ? g_queue[np] : -1;
        if (np >= 0) g_qpos = np;
        banana_unlock(&g_q_lock);
        if (next < 0 || song_file(next, path, sizeof(path)) < 0 || dec_open(&t, path, 0) < 0) {
            g_playing = 0; cur_song = -1; add_mark(-1, 0); continue;
        }
        cur_song = next;
        skip = 0;
        add_mark(next, 0);
        prefetch_next();
    }
    dec_close(&t);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    return 0;
}

static void engine_play(int song, double pos) {
    banana_lock(&g_e_lock);
    g_cmd_song = song;
    g_cmd_pos = pos < 0 ? 0 : pos;
    g_cmd = E_PLAY;
    g_paused = 0;
    banana_cond_broadcast(&g_e_cond);
    banana_unlock(&g_e_lock);
    banana_lock(&g_dl_lock);                     /* (a song_file() waiting gives up) */
    banana_cond_broadcast(&g_dl_cond);
    banana_unlock(&g_dl_lock);
}

static void engine_pause(void) {
    double pos;
    int s = now_playing(&pos);
    if (s < 0) return;
    g_paused_song = s;
    g_paused_pos = pos;
    g_paused = 1;
    engine_play(-1, 0);
    g_paused = 1;
}

/* ── the network jobs: covers, play counts, playlists (a thread) ──── */
enum { J_NONE = 0, J_COVER, J_ALBUM_COVER, J_COUNT_PLAY, J_PLAYLISTS, J_PERSONAL, J_PL_ADD, J_PL_REMOVE, J_PL_CREATE, J_PL_DELETE };
typedef struct { int kind, a, b; char text[100]; } job_t;
static job_t g_jobs[64];
static int   g_njobs;
static banana_mutex_t g_j_lock = BANANA_MUTEX_INIT;
static banana_cond_t  g_j_cond = BANANA_COND_INIT;

static void job(int kind, int a, int b, const char* text) {
    banana_lock(&g_j_lock);
    int dup = 0;
    for (int i = 0; i < g_njobs; i++) if (g_jobs[i].kind == kind && g_jobs[i].a == a && g_jobs[i].b == b && kind != J_PL_CREATE) dup = 1;
    if (!dup && g_njobs < 64) {
        job_t* j = &g_jobs[g_njobs++];
        j->kind = kind; j->a = a; j->b = b;
        snprintf(j->text, sizeof(j->text), "%s", text ? text : "");
    }
    banana_cond_broadcast(&g_j_cond);
    banana_unlock(&g_j_lock);
}

/* covers: decoded, cut square and scaled, kept in memory */
typedef struct { int kind, id, size; unsigned int* px; int state; unsigned int used; } cover_t;   /* state 1 asked, 2 ready, 3 none */
#define NCOVERS 96
static cover_t g_covers[NCOVERS];
static banana_mutex_t g_c_lock = BANANA_MUTEX_INIT;
static volatile int g_cover_gen;

static unsigned int* decode_picture(const uint8_t* data, int len, int size) {
    enum AVCodecID cid = AV_CODEC_ID_NONE;
    if (len > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) cid = AV_CODEC_ID_WEBP;
    else if (len > 3 && data[0] == 0xFF && data[1] == 0xD8) cid = AV_CODEC_ID_MJPEG;
    if (cid == AV_CODEC_ID_NONE) return NULL;
    const AVCodec* c = avcodec_find_decoder(cid);
    AVCodecContext* d = c ? avcodec_alloc_context3(c) : NULL;
    unsigned int* out = NULL;
    if (d && avcodec_open2(d, c, NULL) >= 0) {
        AVPacket* pkt = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        uint8_t* buf = av_malloc((size_t)len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (pkt && fr && buf) {
            memcpy(buf, data, (size_t)len);
            memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            av_packet_from_data(pkt, buf, len);
            buf = NULL;
            if (avcodec_send_packet(d, pkt) >= 0 && avcodec_receive_frame(d, fr) >= 0) {
                int s = fr->width < fr->height ? fr->width : fr->height;
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fr->format);
                int ox = (fr->width - s) / 2 & ~1, oy = (fr->height - s) / 2 & ~1;
                const uint8_t* src[4] = { 0 };
                for (int p = 0; p < 4 && desc; p++) {
                    if (!fr->data[p]) continue;
                    int chroma = (p == 1 || p == 2) && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
                    int sx = chroma ? ox >> desc->log2_chroma_w : ox, sy = chroma ? oy >> desc->log2_chroma_h : oy;
                    int bpp = (desc->flags & AV_PIX_FMT_FLAG_PLANAR) ? 1 : av_get_bits_per_pixel(desc) / 8;
                    src[p] = fr->data[p] + sy * fr->linesize[p] + sx * (bpp > 0 ? bpp : 1);
                }
                struct SwsContext* sw = sws_getContext(s, s, fr->format, size, size, AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
                out = malloc((size_t)size * size * 4);
                if (sw && out) {
                    uint8_t* dst[4] = { (uint8_t*)out, 0, 0, 0 };
                    int dls[4] = { size * 4, 0, 0, 0 };
                    sws_scale(sw, src, fr->linesize, 0, s, dst, dls);
                } else { free(out); out = NULL; }
                if (sw) sws_freeContext(sw);
            }
        }
        av_free(buf);
        av_packet_free(&pkt);
        av_frame_free(&fr);
    }
    if (d) avcodec_free_context(&d);
    return out;
}

/* the picture, or NULL (asked for: it comes later, g_cover_gen bumps) */
static unsigned int* cover_get(int kind, int id, int size) {
    banana_lock(&g_c_lock);
    int lru = 0;
    for (int i = 0; i < NCOVERS; i++) {
        cover_t* c = &g_covers[i];
        if (c->state && c->kind == kind && c->id == id && c->size == size) {
            c->used = banana_ticks();
            unsigned int* px = c->state == 2 ? c->px : NULL;
            banana_unlock(&g_c_lock);
            return px;
        }
        if (g_covers[i].state != 1 && g_covers[i].used < g_covers[lru].used) lru = i;
    }
    cover_t* c = &g_covers[lru];
    if (c->state == 1) { banana_unlock(&g_c_lock); return NULL; }   /* (all busy) */
    free(c->px);
    c->px = NULL;
    c->kind = kind; c->id = id; c->size = size; c->state = 1; c->used = banana_ticks();
    banana_unlock(&g_c_lock);
    job(kind, id, size, NULL);
    return NULL;
}

static void cover_job(int kind, int id, int size) {
    char q[32], url[400], err[100];
    snprintf(q, sizeof(q), "&q=%d", id);
    api_url(url, sizeof(url), kind == J_COVER ? "cover" : "album_cover", q);
    char* data = NULL;
    unsigned long len = 0;
    unsigned int* px = NULL;
    if (banana_http_get(url, &data, &len, err, sizeof(err)) == 0) px = decode_picture((const uint8_t*)data, (int)len, size);
    free(data);
    banana_lock(&g_c_lock);
    for (int i = 0; i < NCOVERS; i++) {
        cover_t* c = &g_covers[i];
        if (c->state == 1 && c->kind == kind && c->id == id && c->size == size) {
            c->px = px;
            c->state = px ? 2 : 3;
            px = NULL;
            break;
        }
    }
    banana_unlock(&g_c_lock);
    free(px);
    g_cover_gen++;
}

static int net_thread(void* arg) {
    (void)arg;
    while (!g_quit) {
        banana_lock(&g_j_lock);
        while (!g_njobs && !g_quit) banana_cond_timedwait(&g_j_cond, &g_j_lock, 500);
        if (g_quit) { banana_unlock(&g_j_lock); break; }
        /* what the user did first (in order), then the covers asked last:
         * they are the ones on the screen now */
        int pick = g_njobs - 1;
        for (int i = 0; i < g_njobs; i++) if (g_jobs[i].kind != J_COVER && g_jobs[i].kind != J_ALBUM_COVER) { pick = i; break; }
        job_t j = g_jobs[pick];
        memmove(g_jobs + pick, g_jobs + pick + 1, sizeof(job_t) * (size_t)(g_njobs - pick - 1));
        g_njobs--;
        banana_unlock(&g_j_lock);
        char err[120], f[200];
        char* r = NULL;
        switch (j.kind) {
        case J_COVER: case J_ALBUM_COVER: cover_job(j.kind, j.a, j.b); break;
        case J_COUNT_PLAY:
            snprintf(f, sizeof(f), "track_id=%d", j.a);
            free(api_post("increment_play", f, 1, err, sizeof(err)));
            break;
        case J_PLAYLISTS: load_playlists(); break;
        case J_PERSONAL: load_personal(); break;
        case J_PL_ADD: case J_PL_REMOVE: case J_PL_DELETE:
            if (j.kind == J_PL_DELETE) snprintf(f, sizeof(f), "playlist_id=%d&mode=delete", j.a);
            else snprintf(f, sizeof(f), "playlist_id=%d&mode=%s&track_id=%d", j.a, j.kind == J_PL_ADD ? "add" : "remove", j.b);
            r = api_post("playlist_mod", f, 1, err, sizeof(err));
            if (!api_ok(r, err, sizeof(err))) snprintf(g_status, sizeof(g_status), "Playlist: %s", err);
            else snprintf(g_status, sizeof(g_status), "%s", j.kind == J_PL_ADD ? "Added to the playlist" : j.kind == J_PL_REMOVE ? "Removed from the playlist" : "Playlist deleted");
            free(r);
            load_playlists();
            break;
        case J_PL_CREATE: {
            char n[300];
            banana_url_encode(j.text, n, sizeof(n));
            snprintf(f, sizeof(f), "name=%s&is_public=%d", n, j.a);
            r = api_post("playlist_create", f, 1, err, sizeof(err));
            if (!api_ok(r, err, sizeof(err))) snprintf(g_status, sizeof(g_status), "Playlist: %s", err);
            else snprintf(g_status, sizeof(g_status), "Playlist \"%s\" created", j.text);
            free(r);
            load_playlists();
            break;
        }
        }
    }
    return 0;
}

static int loader_thread(void* arg) {
    (void)arg;
    g_loading = 1;
    snprintf(g_status, sizeof(g_status), "Loading the catalogue from %s...", g_server);
    load_catalogue();
    if (g_ntracks) {
        g_status[0] = 0;
        g_loading = 0;
        load_personal();
        load_playlists();
    }
    g_loading = 0;
    g_data_gen++;
    return 0;
}

/* ── text and small drawing helpers ───────────────────────────────── */
static void text_fit(int x, int y, int font, int size, const char* s, int maxw, unsigned int color) {
    if (maxw <= 8 || !s) return;
    if (banana_font_width(font, size, s) <= maxw) { bwin_font(&win, x, y, font, size, s, color); return; }
    char b[200];
    int n = (int)strlen(s);
    if (n > 190) n = 190;
    while (n > 0) {
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        memcpy(b, s, (size_t)n);
        strcpy(b + n, "...");
        if (banana_font_width(font, size, b) <= maxw) break;
        n--;
    }
    bwin_font(&win, x, y, font, size, b, color);
}

static void fmt_time(char* b, int cap, double t) {
    if (t < 0) t = 0;
    int s = (int)t;
    if (s >= 3600) snprintf(b, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, cap, "%d:%02d", s / 60, s % 60);
}

static void tri(int x, int y, int w, int h, int right, unsigned int c) {
    for (int i = 0; i < w; i++) {
        int hh = right ? h * (w - i) / w : h * i / w;
        bwin_fill_rect(&win, x + i, y + (h - hh) / 2, 1, hh, c);
    }
}

static void draw_placeholder(int x, int y, int size, const char* name) {
    unsigned h = 5381;
    for (const char* s = name; *s; s++) h = h * 33 + (unsigned char)*s;
    bwin_fill_rect(&win, x, y, size, size, BANANA_RGB(50 + h % 70, 40 + (h >> 8) % 50, 90 + (h >> 16) % 90));
    char ini[8] = { name[0] ? name[0] : '?', 0 };
    int fs = size / 2 > 10 ? size / 2 : 10;
    int w = banana_font_width(F_BOLD, fs, ini);
    bwin_font(&win, x + (size - w) / 2, y + size / 2 - fs * 2 / 3, F_BOLD, fs, ini, 0xFFFFFFu);
}

static void draw_track_cover(int t, int x, int y, int size) {
    if (t < 0 || t >= g_ntracks) { bwin_fill_rect(&win, x, y, size, size, C_PANEL); return; }
    unsigned int* px = cover_get(J_COVER, g_tracks[t].id, size);
    if (px) bwin_blit(&win, x, y, px, size, size);
    else draw_placeholder(x, y, size, g_tracks[t].album[0] ? g_tracks[t].album : g_tracks[t].title);
}

static void draw_album_cover(int a, int x, int y, int size) {
    unsigned int* px = cover_get(J_ALBUM_COVER, g_albums[a].id, size);
    if (px) bwin_blit(&win, x, y, px, size, size);
    else draw_placeholder(x, y, size, g_albums[a].name);
}

/* ── the views ────────────────────────────────────────────────────── */
enum { V_HOME = 0, V_SONGS, V_ALBUMS, V_ARTISTS, V_PLAYLISTS, V_QUEUE, V_NOW, V_EQ, V_COUNT };
static const char* const VIEW_NAME[V_COUNT] = { "Home", "Songs", "Albums", "Artists", "Playlists", "Queue", "Now playing", "Equalizer" };
static int  g_view = V_HOME;
static int  g_open_album = -1;            /* an album / artist / playlist opened from its view */
static char g_open_artist[64];
static int  g_open_pl = -1;               /* playlist id */
static char g_search[64];
static int  g_scroll, g_sel = -1, g_hover = -1;
static int  g_sort;                       /* 0 popular, 1 title, 2 artist, 3 album, 4 time */
static unsigned int g_last_click;
static int  g_last_click_id = -1;

/* the rows shown: track indices (or album / artist heads) */
static int* g_list;
static int  g_nlist, g_list_cap;

static int contains_ci(const char* hay, const char* needle) {
    if (!needle[0]) return 1;
    size_t n = strlen(needle);
    for (; *hay; hay++) if (strncasecmp(hay, needle, n) == 0) return 1;
    return 0;
}

static int cmp_tracks(const void* a, const void* b) {
    const track_t* x = &g_tracks[*(const int*)a];
    const track_t* y = &g_tracks[*(const int*)b];
    int r = 0;
    switch (g_sort) {
    case 0: r = y->plays - x->plays; break;
    case 2: r = strcasecmp(x->artist, y->artist); break;
    case 3: r = strcasecmp(x->album, y->album); break;
    case 4: r = x->dur - y->dur; break;
    default: break;
    }
    if (!r) r = strcasecmp(x->title, y->title);
    return r;
}
static int cmp_artist(const void* a, const void* b) { return strcasecmp(g_tracks[*(const int*)a].artist, g_tracks[*(const int*)b].artist); }
static int cmp_album_name(const void* a, const void* b) { return strcasecmp(g_albums[*(const int*)a].name, g_albums[*(const int*)b].name); }

static void list_push(int v) {
    if (g_nlist == g_list_cap) {
        int nc = g_list_cap ? g_list_cap * 2 : 512;
        int* n = realloc(g_list, (size_t)nc * sizeof(int));
        if (!n) return;
        g_list = n;
        g_list_cap = nc;
    }
    g_list[g_nlist++] = v;
}

static playlist_t* open_playlist(void) {
    for (int i = 0; i < g_npls; i++) if (g_pls[i].id == g_open_pl) return &g_pls[i];
    return NULL;
}

/* what kind of rows: 0 tracks, 1 albums (grid), 2 artists, 3 playlists, 4 home */
static int list_kind(void) {
    if (g_search[0] && g_view != V_EQ && g_view != V_NOW) return 0;
    if (g_view == V_HOME) return 4;
    if (g_view == V_ALBUMS && g_open_album < 0) return 1;
    if (g_view == V_ARTISTS && !g_open_artist[0]) return 2;
    if (g_view == V_PLAYLISTS && g_open_pl < 0) return 3;
    return 0;
}

static void build_list(void) {
    g_nlist = 0;
    int kind = list_kind();
    if (kind == 4) return;
    if (kind == 1) {
        for (int i = 0; i < g_nalbums; i++) list_push(i);
        qsort(g_list, (size_t)g_nlist, sizeof(int), cmp_album_name);
        return;
    }
    if (kind == 3) { for (int i = 0; i < g_npls; i++) list_push(i); return; }
    if (kind == 2) {
        int* tmp = malloc((size_t)(g_ntracks ? g_ntracks : 1) * sizeof(int));
        if (!tmp) return;
        for (int i = 0; i < g_ntracks; i++) tmp[i] = i;
        qsort(tmp, (size_t)g_ntracks, sizeof(int), cmp_artist);
        for (int i = 0; i < g_ntracks; i++)
            if (!g_nlist || strcasecmp(g_tracks[tmp[i]].artist, g_tracks[g_list[g_nlist - 1]].artist)) list_push(tmp[i]);
        free(tmp);
        return;
    }
    /* tracks */
    if (g_search[0]) {
        for (int i = 0; i < g_ntracks; i++) {
            track_t* t = &g_tracks[i];
            if (contains_ci(t->title, g_search) || contains_ci(t->artist, g_search) || contains_ci(t->album, g_search) || contains_ci(t->genre, g_search)) list_push(i);
        }
    } else if (g_view == V_QUEUE) {
        banana_lock(&g_q_lock);
        for (int i = 0; i < g_qlen; i++) list_push(g_queue[i]);
        banana_unlock(&g_q_lock);
        return;
    } else if (g_view == V_PLAYLISTS) {
        playlist_t* p = open_playlist();
        if (p) for (int i = 0; i < p->nsongs; i++) list_push(p->songs[i]);
        return;                                     /* (in the playlist's order) */
    } else {
        for (int i = 0; i < g_ntracks; i++) {
            track_t* t = &g_tracks[i];
            if (g_view == V_ALBUMS && t->album_id != g_albums[g_open_album].id) continue;
            if (g_view == V_ARTISTS && strcasecmp(t->artist, g_open_artist)) continue;
            list_push(i);
        }
    }
    qsort(g_list, (size_t)g_nlist, sizeof(int), cmp_tracks);
}

/* plays tracks[] from i on: the queue becomes that list */
static void play_list(const int* tracks, int n, int i) {
    if (i < 0 || i >= n) return;
    banana_lock(&g_q_lock);
    g_qlen = 0;
    for (int k = 0; k < n && g_qlen < 4096; k++) g_queue[g_qlen++] = tracks[k];
    g_qpos = i;
    if (g_shuffle) shuffle_queue();
    int song = g_queue[g_qpos];
    banana_unlock(&g_q_lock);
    engine_play(song, 0);
}

static void play_pause(void) {
    double pos;
    int s = now_playing(&pos);
    if (g_paused) { engine_play(g_paused_song, g_paused_pos); return; }
    if (s >= 0 && g_playing) { engine_pause(); return; }
    if (g_qlen && g_qpos >= 0) { engine_play(g_queue[g_qpos], 0); return; }
    if (g_nrec) play_list(g_rec, g_nrec, 0);
    else if (g_nlist && list_kind() == 0) play_list(g_list, g_nlist, 0);
}

static void skip_track(int step) {
    double pos;
    now_playing(&pos);
    banana_lock(&g_q_lock);
    if (step < 0 && pos > 3) {
        int s = g_qpos >= 0 ? g_queue[g_qpos] : -1;
        banana_unlock(&g_q_lock);
        if (s >= 0) engine_play(s, 0);
        return;
    }
    int np = queue_step(step, 1);
    if (np >= 0) g_qpos = np;
    int s = np >= 0 ? g_queue[np] : -1;
    banana_unlock(&g_q_lock);
    if (s >= 0) engine_play(s, 0);
}

static void queue_insert(int t, int next) {
    banana_lock(&g_q_lock);
    if (g_qlen < 4096) {
        int at = next && g_qpos >= 0 ? g_qpos + 1 : g_qlen;
        memmove(g_queue + at + 1, g_queue + at, sizeof(int) * (size_t)(g_qlen - at));
        g_queue[at] = t;
        g_qlen++;
        if (g_qpos < 0) g_qpos = 0;
    }
    banana_unlock(&g_q_lock);
    snprintf(g_status, sizeof(g_status), next ? "Plays next: %s" : "Added to the queue: %s", g_tracks[t].title);
}

/* ── settings ─────────────────────────────────────────────────────── */
static void settings_save(void) {
    conf_dirs();
    FILE* f = fopen(CONF_DIR "/settings", "w");
    if (!f) return;
    fprintf(f, "shuffle=%d\nrepeat=%d\nsort=%d\neq=", g_shuffle, g_repeat, g_sort);
    for (int b = 0; b < EQ_BANDS; b++) fprintf(f, "%d%s", g_eq_db[b], b < EQ_BANDS - 1 ? "," : "\n");
    fclose(f);
}

static void settings_load(void) {
    FILE* f = fopen(CONF_DIR "/settings", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "shuffle=", 8)) g_shuffle = atoi(line + 8);
        else if (!strncmp(line, "repeat=", 7)) g_repeat = atoi(line + 7) % 3;
        else if (!strncmp(line, "sort=", 5)) g_sort = atoi(line + 5) % 5;
        else if (!strncmp(line, "eq=", 3)) {
            char* p = line + 3;
            for (int b = 0; b < EQ_BANDS && p; b++) {
                int v = atoi(p);
                g_eq_db[b] = v < -12 ? -12 : v > 12 ? 12 : v;
                p = strchr(p, ',');
                if (p) p++;
            }
            g_eq_gen++;
        }
    }
    fclose(f);
}

/* ── the visualizer ───────────────────────────────────────────────── */
#define FFT_N  1024
#define NBARS  48
static float g_bars[NBARS], g_peaks[NBARS];

static void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static void update_spectrum(void) {
    static float re[FFT_N], im[FFT_N], wf[FFT_N];
    static int init;
    if (!init) { for (int i = 0; i < FFT_N; i++) wf[i] = 0.5f - 0.5f * (float)cos(2 * M_PI * i / (FFT_N - 1)); init = 1; }
    int heard = !g_paused && g_playing;
    unsigned long long p = played_samples();
    for (int i = 0; i < FFT_N; i++) {
        re[i] = heard && p > FFT_N ? g_viz[(p + (unsigned long long)i - FFT_N) % VIZ_RING] * wf[i] : 0;
        im[i] = 0;
    }
    fft(re, im, FFT_N);
    for (int b = 0; b < NBARS; b++) {
        double f0 = 40 * pow(400.0, (double)b / NBARS), f1 = 40 * pow(400.0, (double)(b + 1) / NBARS);
        int i0 = (int)(f0 * FFT_N / RATE), i1 = (int)(f1 * FFT_N / RATE);
        if (i1 <= i0) i1 = i0 + 1;
        float m = 0;
        for (int i = i0; i < i1 && i < FFT_N / 2; i++) { float v = re[i] * re[i] + im[i] * im[i]; if (v > m) m = v; }
        float db = m > 1e-9f ? 10.0f * (float)log10(m) : -90;
        float v = (db + 30) / 60.0f;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        g_bars[b] = v > g_bars[b] ? v : g_bars[b] * 0.82f + v * 0.18f;
        g_peaks[b] = g_bars[b] > g_peaks[b] ? g_bars[b] : g_peaks[b] - 0.012f;
        if (g_peaks[b] < 0) g_peaks[b] = 0;
    }
}

static void draw_spectrum(int x, int y, int w, int h) {
    int bw = w / NBARS;
    if (bw < 3 || h < 8) return;
    for (int b = 0; b < NBARS; b++) {
        int bh = (int)(g_bars[b] * h);
        if (bh < 2) bh = 2;
        bwin_fill_rect(&win, x + b * bw + 1, y + h - bh, bw - 2, bh, C_ACCENT);
        if (bh > h / 2) bwin_fill_rect(&win, x + b * bw + 1, y + h - bh, bw - 2, bh - h / 2, C_ACCENT2);
        int pk = (int)(g_peaks[b] * h);
        bwin_fill_rect(&win, x + b * bw + 1, y + h - pk - 3, bw - 2, 2, C_TEXT);
    }
}

/* ── hit areas of the last frame ──────────────────────────────────── */
typedef struct { int x, y, w, h, id; } hit_t;
static hit_t g_hits[256];
static int   g_nhits;
enum {
    H_NAV = 1,
    H_PLAY = 20, H_PREV, H_NEXT, H_SHUFFLE, H_REPEAT, H_SEEK, H_VOLUME, H_BACK, H_COVER, H_ACCOUNT, H_RELOAD,
    H_NEWPL, H_DELPL, H_PLAYALL,
    H_SORT = 40,
    H_EQ = 50,
    H_PRESET = 70,
    H_DLG = 90,               /* + field / button of a dialog */
    H_MENU = 200,             /* + item of the right-click menu */
    H_ROW = 1000,             /* + row */
    H_CARD = 20000,           /* + section * 1000 + card (home) */
};
static void hit(int x, int y, int w, int h, int id) { if (g_nhits < 256) g_hits[g_nhits++] = (hit_t){ x, y, w, h, id }; }
static int hit_at(int mx, int my) {
    for (int i = g_nhits - 1; i >= 0; i--)
        if (mx >= g_hits[i].x && mx < g_hits[i].x + g_hits[i].w && my >= g_hits[i].y && my < g_hits[i].y + g_hits[i].h) return g_hits[i].id;
    return 0;
}

/* ── the dialog (log in, new playlist) and the right-click menu ───── */
enum { D_NONE = 0, D_LOGIN, D_NEWPL };
static int  g_dlg;
static char g_f[2][128];                   /* its text fields */
static int  g_focus;
static int  g_dlg_public = 1;
static char g_dlg_msg[160];
static volatile int g_dlg_busy;

typedef struct { char label[100]; int act, a, b; int disabled; } mitem_t;
enum { M_PLAY = 1, M_NEXT, M_QUEUE, M_ADDPL, M_REMOVEPL, M_ALBUM, M_ARTIST, M_SEP };
static mitem_t g_menu[40];
static int g_nmenu, g_menu_x, g_menu_y, g_menu_open, g_menu_hover = -1;

static void open_menu(int x, int y, int t) {
    g_nmenu = 0;
    mitem_t* m = g_menu;
    snprintf(m[g_nmenu].label, 100, "Play"); m[g_nmenu].act = M_PLAY; m[g_nmenu++].a = t;
    snprintf(m[g_nmenu].label, 100, "Play next"); m[g_nmenu].act = M_NEXT; m[g_nmenu++].a = t;
    snprintf(m[g_nmenu].label, 100, "Add to the queue"); m[g_nmenu].act = M_QUEUE; m[g_nmenu++].a = t;
    if (g_tracks[t].album_id) { snprintf(m[g_nmenu].label, 100, "Go to the album"); m[g_nmenu].act = M_ALBUM; m[g_nmenu++].a = t; }
    snprintf(m[g_nmenu].label, 100, "Go to the artist"); m[g_nmenu].act = M_ARTIST; m[g_nmenu++].a = t;
    playlist_t* cur = g_view == V_PLAYLISTS ? open_playlist() : NULL;
    if (cur && g_user[0] && (cur->creator_id == g_user_id || g_admin)) {
        m[g_nmenu].act = M_SEP; g_nmenu++;
        snprintf(m[g_nmenu].label, 100, "Remove from \"%.60s\"", cur->name); m[g_nmenu].act = M_REMOVEPL; m[g_nmenu].a = cur->id; m[g_nmenu++].b = t;
    }
    if (g_user[0]) {
        int any = 0;
        for (int i = 0; i < g_npls && g_nmenu < 38; i++) {
            if (g_pls[i].creator_id != g_user_id && !g_admin) continue;
            if (!any) { m[g_nmenu].act = M_SEP; g_nmenu++; any = 1; }
            snprintf(m[g_nmenu].label, 100, "Add to \"%.60s\"", g_pls[i].name);
            m[g_nmenu].act = M_ADDPL; m[g_nmenu].a = g_pls[i].id; m[g_nmenu++].b = t;
        }
    }
    g_menu_x = x;
    g_menu_y = y;
    g_menu_open = 1;
    g_menu_hover = -1;
}

static void menu_do(int i) {
    g_menu_open = 0;
    if (i < 0 || i >= g_nmenu) return;
    mitem_t* m = &g_menu[i];
    switch (m->act) {
    case M_PLAY: { int one[1] = { m->a }; play_list(one, 1, 0); break; }
    case M_NEXT: queue_insert(m->a, 1); break;
    case M_QUEUE: queue_insert(m->a, 0); break;
    case M_ADDPL: job(J_PL_ADD, m->a, g_tracks[m->b].id, NULL); break;
    case M_REMOVEPL: job(J_PL_REMOVE, m->a, g_tracks[m->b].id, NULL); break;
    case M_ALBUM:
        for (int k = 0; k < g_nalbums; k++) if (g_albums[k].id == g_tracks[m->a].album_id) { g_view = V_ALBUMS; g_open_album = k; }
        g_search[0] = 0; g_scroll = 0; g_sel = -1;
        break;
    case M_ARTIST:
        g_view = V_ARTISTS;
        snprintf(g_open_artist, sizeof(g_open_artist), "%s", g_tracks[m->a].artist);
        g_search[0] = 0; g_scroll = 0; g_sel = -1;
        break;
    }
}

static void draw_menu(void) {
    if (!g_menu_open) return;
    int w = 220;
    for (int i = 0; i < g_nmenu; i++) { int tw = banana_font_width(F_SANS, 13, g_menu[i].label) + 28; if (tw > w) w = tw; }
    if (w > 420) w = 420;
    int h = 8;
    for (int i = 0; i < g_nmenu; i++) h += g_menu[i].act == M_SEP ? 9 : 26;
    int x = g_menu_x, y = g_menu_y;
    if (x + w > win.w - 4) x = win.w - 4 - w;
    if (y + h > win.h - 4) y = win.h - 4 - h;
    if (y < 4) y = 4;
    bwin_fill_rect(&win, x + 3, y + 3, w, h, 0x08090Cu);
    bwin_fill_rect(&win, x, y, w, h, C_PANEL);
    bwin_rect(&win, x, y, w, h, 0x353A48u);
    int cy = y + 4;
    for (int i = 0; i < g_nmenu; i++) {
        if (g_menu[i].act == M_SEP) { bwin_fill_rect(&win, x + 10, cy + 4, w - 20, 1, 0x353A48u); cy += 9; continue; }
        if (i == g_menu_hover) bwin_fill_rect(&win, x + 4, cy, w - 8, 26, C_SEL);
        text_fit(x + 14, cy + 5, F_SANS, 13, g_menu[i].label, w - 24, C_TEXT);
        hit(x + 4, cy, w - 8, 26, H_MENU + i);
        cy += 26;
    }
}

static void open_dialog(int kind) {
    g_dlg = kind;
    g_focus = 0;
    g_dlg_msg[0] = 0;
    memset(g_f, 0, sizeof(g_f));
    if (kind == D_LOGIN && g_user[0]) snprintf(g_f[0], sizeof(g_f[0]), "%s", g_user);
}

static void draw_field(int x, int y, int w, const char* label, const char* text, int focus, int secret, int id) {
    bwin_font(&win, x, y, F_SANS, 12, label, C_DIM);
    bwin_fill_rect(&win, x, y + 18, w, 30, C_BG);
    bwin_rect(&win, x, y + 18, w, 30, focus ? C_ACCENT : 0x353A48u);
    char shown[140];
    if (secret) { int n = (int)strlen(text); if (n > 60) n = 60; memset(shown, '*', (size_t)n); shown[n] = 0; }
    else snprintf(shown, sizeof(shown), "%s", text);
    if (focus) strcat(shown, "_");
    text_fit(x + 8, y + 25, F_SANS, 14, shown, w - 16, C_TEXT);
    hit(x, y + 18, w, 30, id);
}

static void button(int x, int y, int w, const char* label, int primary, int id) {
    bwin_fill_rect(&win, x, y, w, 30, primary ? C_ACCENT : C_ROW);
    int tw = banana_font_width(F_BOLD, 13, label);
    bwin_font(&win, x + (w - tw) / 2, y + 7, F_BOLD, 13, label, C_TEXT);
    hit(x, y, w, 30, id);
}

static void draw_dialog(void) {
    if (!g_dlg) return;
    for (int y = 0; y < win.h; y += 2) bwin_fill_rect(&win, 0, y, win.w, 1, 0x000000u);   /* (dims the window) */
    int w = 400, h = g_dlg == D_LOGIN ? 300 : 230, x = (win.w - w) / 2, y = (win.h - h) / 2;
    bwin_fill_rect(&win, x, y, w, h, C_PANEL);
    bwin_rect(&win, x, y, w, h, C_ACCENT);
    if (g_dlg == D_LOGIN) {
        bwin_font(&win, x + 24, y + 18, F_BOLD, 18, g_user[0] ? "Your account" : "Log in to Amethyst", C_TEXT);
        text_fit(x + 24, y + 44, F_SANS, 12, g_server, w - 48, C_DIM);
        draw_field(x + 24, y + 66, w - 48, "User name", g_f[0], g_focus == 0, 0, H_DLG + 0);
        draw_field(x + 24, y + 124, w - 48, "Password", g_f[1], g_focus == 1, 1, H_DLG + 1);
        if (g_dlg_msg[0]) text_fit(x + 24, y + 186, F_SANS, 12, g_dlg_msg, w - 48, C_ERR);
        if (g_dlg_busy) bwin_font(&win, x + 24, y + 186, F_SANS, 12, "Talking to the server...", C_ACCENT2);
        button(x + 24, y + 214, 108, "Log in", 1, H_DLG + 2);
        button(x + 140, y + 214, 132, "Create account", 0, H_DLG + 3);
        button(x + 280, y + 214, 96, "Cancel", 0, H_DLG + 4);
        if (g_user[0]) button(x + 24, y + 254, w - 48, "Log out", 0, H_DLG + 5);
    } else {
        bwin_font(&win, x + 24, y + 18, F_BOLD, 18, "New playlist", C_TEXT);
        draw_field(x + 24, y + 56, w - 48, "Name", g_f[0], 1, 0, H_DLG + 0);
        bwin_rect(&win, x + 24, y + 124, 16, 16, C_DIM);
        if (g_dlg_public) bwin_fill_rect(&win, x + 28, y + 128, 8, 8, C_ACCENT2);
        bwin_font(&win, x + 48, y + 124, F_SANS, 13, "Public (everyone can see it)", C_TEXT);
        hit(x + 20, y + 120, 260, 24, H_DLG + 6);
        button(x + 24, y + 176, 160, "Create", 1, H_DLG + 2);
        button(x + 216, y + 176, 160, "Cancel", 0, H_DLG + 4);
    }
}

typedef struct { int create; } login_arg_t;
static login_arg_t g_login_arg;

static int login_thread(void* arg) {
    login_arg_t* a = arg;
    char msg[160];
    if (account_login(g_f[0], g_f[1], a->create, msg, sizeof(msg)) == 0) {
        account_save();
        g_dlg = D_NONE;
        snprintf(g_status, sizeof(g_status), "Logged in as %s", g_user);
        job(J_PERSONAL, 0, 0, NULL);
        job(J_PLAYLISTS, 0, 0, NULL);
    } else snprintf(g_dlg_msg, sizeof(g_dlg_msg), "%s", msg);
    g_dlg_busy = 0;
    return 0;
}

static void dialog_submit(int create) {
    if (g_dlg == D_LOGIN) {
        if (!g_f[0][0] || !g_f[1][0]) { snprintf(g_dlg_msg, sizeof(g_dlg_msg), "Type your user name and password"); return; }
        if (g_dlg_busy) return;
        g_dlg_busy = 1;
        g_dlg_msg[0] = 0;
        g_login_arg.create = create;
        if (banana_thread(login_thread, &g_login_arg) < 0) { g_dlg_busy = 0; snprintf(g_dlg_msg, sizeof(g_dlg_msg), "no thread"); }
    } else if (g_dlg == D_NEWPL) {
        if (!g_f[0][0]) return;
        job(J_PL_CREATE, g_dlg_public, 0, g_f[0]);
        g_dlg = D_NONE;
    }
}

static void logout(void) {
    g_user[0] = g_pass[0] = 0;
    g_user_id = -1;
    g_admin = 0;
    account_save();
    g_dlg = D_NONE;
    snprintf(g_status, sizeof(g_status), "Logged out");
    job(J_PERSONAL, 0, 0, NULL);
    job(J_PLAYLISTS, 0, 0, NULL);
}

/* ── drawing ──────────────────────────────────────────────────────── */
static int g_song_now = -1;
static double g_pos_now;

static void icon_nav(int v, int x, int y, unsigned int c) {
    switch (v) {
    case V_HOME:    for (int i = 0; i < 7; i++) bwin_fill_rect(&win, x + 7 - i, y + i, 1 + 2 * i, 1, c); bwin_fill_rect(&win, x + 2, y + 7, 11, 7, c); break;
    case V_SONGS:   bwin_fill_rect(&win, x + 6, y, 2, 11, c); bwin_fill_circle(&win, x + 4, y + 11, 3, c); bwin_fill_rect(&win, x + 6, y, 6, 2, c); break;
    case V_ALBUMS:  bwin_circle(&win, x + 7, y + 7, 7, c); bwin_fill_circle(&win, x + 7, y + 7, 2, c); break;
    case V_ARTISTS: bwin_fill_circle(&win, x + 7, y + 4, 4, c); bwin_fill_rect(&win, x + 1, y + 10, 12, 4, c); break;
    case V_PLAYLISTS: for (int i = 0; i < 3; i++) { bwin_fill_rect(&win, x, y + 2 + i * 5, 10, 2, c); } bwin_fill_rect(&win, x + 12, y + 8, 2, 7, c); bwin_fill_rect(&win, x + 9, y + 11, 8, 2, c); break;
    case V_QUEUE:   for (int i = 0; i < 3; i++) { bwin_fill_rect(&win, x, y + 2 + i * 5, 14, 2, c); } break;
    case V_NOW:     for (int i = 0; i < 4; i++) { bwin_fill_rect(&win, x + i * 4, y + 12 - (i * 7 % 11), 3, 2 + (i * 7 % 11), c); } break;
    case V_EQ:      for (int i = 0; i < 3; i++) { bwin_fill_rect(&win, x + 2 + i * 5, y, 1, 14, c); bwin_fill_rect(&win, x + i * 5, y + 3 + i * 4, 5, 3, c); } break;
    }
}

static void draw_sidebar(void) {
    bwin_fill_rect(&win, 0, 0, SIDE_W, win.h - BAR_H, C_SIDE);
    bwin_fill_circle(&win, 26, 28, 10, C_ACCENT);
    bwin_fill_circle(&win, 26, 28, 4, C_SIDE);
    bwin_font(&win, 44, 17, F_BOLD, 19, "Amethyst", C_TEXT);
    int y = 62;
    for (int v = 0; v < V_COUNT; v++) {
        int on = g_view == v && !g_search[0];
        if (on) { bwin_fill_rect(&win, 8, y, SIDE_W - 16, 30, C_SEL); bwin_fill_rect(&win, 8, y, 3, 30, C_ACCENT); }
        icon_nav(v, 22, y + 8, on ? C_ACCENT2 : C_DIM);
        bwin_font(&win, 46, y + 7, on ? F_BOLD : F_SANS, 14, VIEW_NAME[v], on ? C_TEXT : C_DIM);
        hit(8, y, SIDE_W - 16, 30, H_NAV + v);
        y += 33;
        if (v == V_QUEUE) { bwin_fill_rect(&win, 20, y + 1, SIDE_W - 40, 1, 0x262A34u); y += 6; }
    }
    /* the account, at the bottom */
    int ay = win.h - BAR_H - 58;
    bwin_fill_rect(&win, 8, ay, SIDE_W - 16, 48, C_PANEL);
    bwin_fill_circle(&win, 30, ay + 24, 14, g_user[0] ? C_ACCENT : C_FAINT);
    char ini[4] = { g_user[0] ? g_user[0] : '?', 0 };
    if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32;
    int iw = banana_font_width(F_BOLD, 14, ini);
    bwin_font(&win, 30 - iw / 2, ay + 15, F_BOLD, 14, ini, 0xFFFFFFu);
    text_fit(52, ay + 8, F_BOLD, 13, g_user[0] ? g_user : "Not logged in", SIDE_W - 70, C_TEXT);
    bwin_font(&win, 52, ay + 26, F_SANS, 11, g_user[0] ? "Account, log out" : "Log in / create one", C_ACCENT2);
    hit(8, ay, SIDE_W - 16, 48, H_ACCOUNT);
}

static void draw_top(void) {
    int x = SIDE_W, w = win.w - SIDE_W;
    bwin_fill_rect(&win, x, 0, w, TOP_H, C_BG);
    char title[160];
    int back = 0;
    playlist_t* pl = open_playlist();
    if (g_search[0]) snprintf(title, sizeof(title), "Search");
    else if (g_view == V_ALBUMS && g_open_album >= 0) { snprintf(title, sizeof(title), "%s", g_albums[g_open_album].name); back = 1; }
    else if (g_view == V_ARTISTS && g_open_artist[0]) { snprintf(title, sizeof(title), "%s", g_open_artist); back = 1; }
    else if (g_view == V_PLAYLISTS && pl) { snprintf(title, sizeof(title), "%s", pl->name); back = 1; }
    else snprintf(title, sizeof(title), "%s", VIEW_NAME[g_view]);
    int tx = x + 24;
    if (back) { bwin_font(&win, tx, 15, F_BOLD, 18, "<", C_ACCENT2); hit(tx - 6, 8, 26, 36, H_BACK); tx += 24; }
    text_fit(tx, 14, F_BOLD, 20, title, w - 330, C_TEXT);
    int sx = win.w - 260, sw = 236;
    bwin_fill_rect(&win, sx, 12, sw, 28, C_ROW);
    bwin_rect(&win, sx, 12, sw, 28, g_search[0] ? C_ACCENT : 0x2A2E38u);
    bwin_circle(&win, sx + 14, 24, 5, C_DIM);
    bwin_line(&win, sx + 18, 28, sx + 21, 31, C_DIM);
    if (g_search[0]) { char b[80]; snprintf(b, sizeof(b), "%s_", g_search); text_fit(sx + 28, 18, F_SANS, 13, b, sw - 36, C_TEXT); }
    else bwin_font(&win, sx + 28, 18, F_SANS, 13, "Search songs, artists, albums", C_FAINT);
}

static void draw_rows(int x, int y, int w, int h) {
    int cw_t = w * 40 / 100, cw_a = w * 24 / 100, cw_al = w * 22 / 100;
    int cx_t = x + 82, cx_a = cx_t + cw_t - 44, cx_al = cx_a + cw_a, cx_time = cx_al + cw_al;
    static const char* const HEAD[5] = { "TITLE", "ARTIST", "ALBUM", "TIME", "" };
    int hx[4] = { cx_t, cx_a, cx_al, cx_time };
    static const int SORT_OF[4] = { 1, 2, 3, 4 };
    int sortable = g_view != V_QUEUE && !(g_view == V_PLAYLISTS && open_playlist());
    int top = y;
    playlist_t* pl = g_view == V_PLAYLISTS ? open_playlist() : NULL;
    if (pl && !g_search[0]) {                        /* the playlist's header */
        char b[160];
        snprintf(b, sizeof(b), "%d song%s - by %s - %s", pl->nsongs, pl->nsongs == 1 ? "" : "s", pl->creator, pl->is_public ? "public" : "private");
        bwin_font(&win, x + 24, y + 4, F_SANS, 13, b, C_DIM);
        button(win.w - 300, y - 2, 120, "Play all", 1, H_PLAYALL);
        if (g_user[0] && (pl->creator_id == g_user_id || g_admin)) button(win.w - 170, y - 2, 140, "Delete playlist", 0, H_DELPL);
        y += 36;
        h -= 36;
    }
    for (int c = 0; c < 4; c++) {
        bwin_font(&win, hx[c], y + 4, F_BOLD, 11, HEAD[c], sortable && g_sort == SORT_OF[c] ? C_ACCENT2 : C_FAINT);
        if (sortable) hit(hx[c], y, 90, 20, H_SORT + SORT_OF[c]);
    }
    if (sortable) {
        bwin_font(&win, x + 24, y + 4, F_BOLD, 11, "#", g_sort == 0 ? C_ACCENT2 : C_FAINT);
        hit(x + 16, y, 40, 20, H_SORT + 0);
    }
    bwin_fill_rect(&win, x + 16, y + 22, w - 32, 1, 0x262A34u);
    y += 26;
    h -= 26;
    int rows = h / ROW_H;
    if (g_scroll > g_nlist - rows) g_scroll = g_nlist - rows;
    if (g_scroll < 0) g_scroll = 0;
    for (int r = 0; r < rows && g_scroll + r < g_nlist; r++) {
        int i = g_scroll + r, t = g_list[i];
        track_t* so = &g_tracks[t];
        int ry = y + r * ROW_H;
        int cur = g_view == V_QUEUE ? i == g_qpos : t == g_song_now;
        unsigned int bg = i == g_sel ? C_SEL : i == g_hover ? C_HOVER : C_BG;
        bwin_fill_rect(&win, x + 8, ry, w - 16, ROW_H - 2, bg);
        if (cur && g_playing && !g_paused) {
            for (int k = 0; k < 3; k++) { int bh = 4 + (int)(g_bars[4 + k * 6] * 12); bwin_fill_rect(&win, x + 18 + k * 5, ry + 22 - bh, 3, bh, C_ACCENT2); }
        } else {
            char num[12];
            snprintf(num, sizeof(num), "%d", i + 1);
            bwin_font(&win, x + 16, ry + 7, F_SANS, 12, num, cur ? C_ACCENT2 : C_FAINT);
        }
        draw_track_cover(t, x + 48, ry + 2, 24);
        unsigned int tc = cur ? C_ACCENT2 : C_TEXT;
        text_fit(cx_t, ry + 6, cur ? F_BOLD : F_SANS, 14, so->title, cx_a - cx_t - 12, tc);
        text_fit(cx_a, ry + 7, F_SANS, 13, so->artist, cw_a - 12, C_DIM);
        text_fit(cx_al, ry + 7, F_SANS, 13, so->album[0] ? so->album : "-", cw_al - 12, C_DIM);
        char tm[16];
        if (so->dur) fmt_time(tm, sizeof(tm), so->dur); else snprintf(tm, sizeof(tm), "-");
        bwin_font(&win, cx_time, ry + 7, F_SANS, 13, tm, C_DIM);
        hit(x + 8, ry, w - 16, ROW_H - 2, H_ROW + i);
    }
    if (!g_nlist) {
        const char* m = g_loading ? "Loading..." : g_search[0] ? "Nothing matches the search" :
                        g_view == V_QUEUE ? "Nothing is queued - double-click a song" :
                        pl ? "This playlist is empty: right-click a song > Add to it" : "No songs";
        int mw = banana_font_width(F_SANS, 14, m);
        bwin_font(&win, x + (w - mw) / 2, y + 60, F_SANS, 14, m, C_DIM);
    }
    (void)top;
}

static void draw_albums(int x, int y, int w, int h) {
    int tile = 150, gap = 22, cols = (w - 32 + gap) / (tile + gap);
    if (cols < 1) cols = 1;
    int th = tile + 46, rows = h / th + 1;
    int first = g_scroll * cols;
    if (first >= g_nlist) { g_scroll = 0; first = 0; }
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int i = first + r * cols + c;
            if (i >= g_nlist) break;
            int a = g_list[i];
            int tx = x + 24 + c * (tile + gap), ty = y + 8 + r * th;
            if (ty > y + h) break;
            if (i == g_hover) bwin_fill_rect(&win, tx - 6, ty - 6, tile + 12, th - 4, C_HOVER);
            draw_album_cover(a, tx, ty, tile);
            text_fit(tx, ty + tile + 6, F_BOLD, 13, g_albums[a].name, tile, C_TEXT);
            char n[32];
            snprintf(n, sizeof(n), "%d song%s", g_albums[a].count, g_albums[a].count == 1 ? "" : "s");
            bwin_font(&win, tx, ty + tile + 24, F_SANS, 12, n, C_DIM);
            hit(tx - 6, ty - 6, tile + 12, th - 4, H_ROW + i);
        }
}

static void draw_heads(int x, int y, int w, int h, int playlists) {
    if (playlists) {
        button(x + 24, y + 4, 150, "+ New playlist", 1, H_NEWPL);
        if (!g_user[0]) bwin_font(&win, x + 190, y + 11, F_SANS, 12, "(log in to make playlists)", C_DIM);
        y += 44;
        h -= 44;
    }
    int rows = h / 46;
    if (g_scroll > g_nlist - rows) g_scroll = g_nlist - rows;
    if (g_scroll < 0) g_scroll = 0;
    for (int r = 0; r < rows && g_scroll + r < g_nlist; r++) {
        int i = g_scroll + r, k = g_list[i];
        int ry = y + r * 46;
        if (i == g_hover) bwin_fill_rect(&win, x + 8, ry, w - 16, 44, C_HOVER);
        if (playlists) {
            playlist_t* p = &g_pls[k];
            if (p->nsongs) draw_track_cover(p->songs[0], x + 20, ry + 4, 36);
            else draw_placeholder(x + 20, ry + 4, 36, p->name);
            text_fit(x + 68, ry + 6, F_BOLD, 14, p->name, w - 200, C_TEXT);
            char b[120];
            snprintf(b, sizeof(b), "%d song%s - %s%s", p->nsongs, p->nsongs == 1 ? "" : "s", p->creator, p->is_public ? "" : " - private");
            text_fit(x + 68, ry + 25, F_SANS, 12, b, w - 200, C_DIM);
        } else {
            const char* name = g_tracks[k].artist;
            bwin_fill_circle(&win, x + 38, ry + 22, 17, 0x2A2E38u);
            char ini[4] = { name[0], 0 };
            bwin_font(&win, x + 33, ry + 13, F_BOLD, 15, ini, C_ACCENT2);
            int n = 0;
            for (int t = 0; t < g_ntracks; t++) if (!strcasecmp(g_tracks[t].artist, name)) n++;
            char c[32];
            snprintf(c, sizeof(c), "%d song%s", n, n == 1 ? "" : "s");
            text_fit(x + 68, ry + 6, F_BOLD, 14, name, w - 200, C_TEXT);
            bwin_font(&win, x + 68, ry + 25, F_SANS, 12, c, C_DIM);
        }
        hit(x + 8, ry, w - 16, 44, H_ROW + i);
    }
    if (!g_nlist && playlists) bwin_font(&win, x + 24, y + 20, F_SANS, 14, g_loading ? "Loading..." : "No playlists yet", C_DIM);
}

/* home: rows of cards - recommended, recently played, most played */
static int g_top[30], g_ntop;
static int g_home_scroll;

static void draw_home(int x, int y, int w, int h) {
    /* the most played, from the catalogue */
    g_ntop = 0;
    for (int i = 0; i < g_ntracks; i++) {
        int at = g_ntop < 18 ? g_ntop++ : 17;
        if (at == 17 && g_ntop == 18 && g_tracks[i].plays <= g_tracks[g_top[17]].plays) continue;
        g_top[at] = i;
        while (at > 0 && g_tracks[g_top[at]].plays > g_tracks[g_top[at - 1]].plays) { int t = g_top[at]; g_top[at] = g_top[at - 1]; g_top[at - 1] = t; at--; }
    }
    struct { const char* title; const int* list; int n; } S[3] = {
        { g_user[0] ? "Made for you" : "Discover", g_rec, g_nrec },
        { "Recently played", g_hist, g_nhist },
        { "Most played", g_top, g_ntop },
    };
    int card = 132, gap = 16;
    int cy = y + 12 - g_home_scroll;
    for (int s = 0; s < 3; s++) {
        if (!S[s].n) continue;
        if (cy + 30 > y && cy < y + h) bwin_font(&win, x + 24, cy, F_BOLD, 17, S[s].title, C_TEXT);
        cy += 32;
        int per = (w - 48 + gap) / (card + gap);
        for (int k = 0; k < S[s].n && k < per; k++) {
            int t = S[s].list[k];
            int cx = x + 24 + k * (card + gap);
            if (cy + card + 44 > y && cy < y + h) {
                int id = H_CARD + s * 1000 + k;
                if (g_hover == id) bwin_fill_rect(&win, cx - 6, cy - 6, card + 12, card + 52, C_HOVER);
                draw_track_cover(t, cx, cy, card);
                text_fit(cx, cy + card + 6, F_BOLD, 13, g_tracks[t].title, card, t == g_song_now ? C_ACCENT2 : C_TEXT);
                text_fit(cx, cy + card + 24, F_SANS, 12, g_tracks[t].artist, card, C_DIM);
                if (cy >= y) hit(cx - 6, cy - 6, card + 12, card + 52, id);
            }
        }
        cy += card + 62;
    }
    if (g_loading && !g_ntracks) bwin_font(&win, x + 24, y + 30, F_SANS, 14, "Loading the catalogue...", C_DIM);
    else if (!g_ntracks) bwin_font(&win, x + 24, y + 30, F_SANS, 14, "The server sent no songs", C_DIM);
    if (g_home_scroll > 0 && cy + g_home_scroll < y + h) g_home_scroll = 0;
}

static void draw_now(int x, int y, int w, int h) {
    int song = g_song_now;
    if (g_buffering >= 0) song = g_buffering;
    if (song < 0) {
        const char* m = "Nothing is playing";
        int mw = banana_font_width(F_SANS, 15, m);
        bwin_font(&win, x + (w - mw) / 2, y + h / 2 - 10, F_SANS, 15, m, C_DIM);
        return;
    }
    track_t* s = &g_tracks[song];
    int cs = h - 150 < w / 2 - 40 ? h - 150 : w / 2 - 40;
    if (cs > 300) cs = 300;
    if (cs < 80) cs = 80;
    int cx = x + 32, cy = y + 16;
    draw_track_cover(song, cx, cy, cs);
    int tx = cx + cs + 32, tw = x + w - tx - 24;
    text_fit(tx, cy + 10, F_BOLD, 26, s->title, tw, C_TEXT);
    text_fit(tx, cy + 50, F_SANS, 17, s->artist, tw, C_ACCENT2);
    char b[160];
    snprintf(b, sizeof(b), "%s%s%s", s->album, s->album[0] && s->genre[0] ? "  -  " : "", s->genre);
    text_fit(tx, cy + 76, F_SANS, 15, b, tw, C_DIM);
    snprintf(b, sizeof(b), "%d plays", s->plays);
    bwin_font(&win, tx, cy + 100, F_SANS, 12, b, C_FAINT);
    bwin_font(&win, tx, cy + 136, F_BOLD, 12, "UP NEXT", C_FAINT);
    banana_lock(&g_q_lock);
    for (int k = 1; k <= 4; k++) {
        int p = g_qpos + k;
        if (p >= g_qlen) { if (g_repeat == REPEAT_ALL && g_qlen) p %= g_qlen; else break; }
        track_t* n = &g_tracks[g_queue[p]];
        snprintf(b, sizeof(b), "%s  -  %s", n->title, n->artist);
        text_fit(tx, cy + 136 + k * 22, F_SANS, 13, b, tw, C_DIM);
    }
    banana_unlock(&g_q_lock);
    draw_spectrum(x + 32, cy + cs + 20, w - 64, h - cs - 52);
}

static const char* const PRESET_NAME[] = { "Flat", "Bass boost", "Treble boost", "Vocal", "Loudness", "Rock", "Classical" };
static const signed char PRESET[][EQ_BANDS] = {
    { 0, 0, 0, 0, 0, 0 }, { 8, 6, 2, 0, 0, 0 }, { 0, 0, 0, 2, 6, 8 }, { -2, -1, 4, 5, 2, 0 },
    { 6, 3, 0, 0, 3, 6 }, { 5, 3, -2, 2, 4, 5 }, { 3, 2, 0, 0, 2, 3 },
};
#define NPRESETS 7

static void draw_eq(int x, int y, int w, int h) {
    bwin_font(&win, x + 32, y + 8, F_SANS, 13, "Shape the sound: drag the sliders, or pick a preset.", C_DIM);
    int sh = h - 140 > 260 ? 260 : h - 140, top = y + 50;
    int bw = (w - 64) / EQ_BANDS;
    for (int b = 0; b < EQ_BANDS; b++) {
        int bx = x + 32 + b * bw + bw / 2;
        bwin_fill_rect(&win, bx - 2, top, 4, sh, 0x262A34u);
        bwin_fill_rect(&win, bx - 10, top + sh / 2, 20, 1, C_FAINT);
        int ky = top + sh / 2 - g_eq_db[b] * (sh / 2) / 12;
        int y0 = ky < top + sh / 2 ? ky : top + sh / 2, y1 = ky < top + sh / 2 ? top + sh / 2 : ky;
        bwin_fill_rect(&win, bx - 2, y0, 4, y1 - y0, C_ACCENT);
        bwin_fill_circle(&win, bx, ky, 9, C_ACCENT2);
        char v[16];
        snprintf(v, sizeof(v), "%+d dB", g_eq_db[b]);
        int vw = banana_font_width(F_SANS, 12, v);
        bwin_font(&win, bx - vw / 2, top + sh + 10, F_SANS, 12, v, C_TEXT);
        char hz[16];
        snprintf(hz, sizeof(hz), "%s Hz", EQ_LABEL[b]);
        int hw = banana_font_width(F_BOLD, 12, hz);
        bwin_font(&win, bx - hw / 2, top + sh + 28, F_BOLD, 12, hz, C_DIM);
        hit(bx - bw / 2, top - 10, bw, sh + 20, H_EQ + b);
    }
    int py = top + sh + 60, px = x + 32;
    for (int p = 0; p < NPRESETS; p++) {
        int pw = banana_font_width(F_SANS, 13, PRESET_NAME[p]) + 24;
        if (px + pw > x + w - 16) { px = x + 32; py += 34; }
        int on = 1;
        for (int b = 0; b < EQ_BANDS; b++) if (PRESET[p][b] != g_eq_db[b]) on = 0;
        bwin_fill_rect(&win, px, py, pw, 26, on ? C_ACCENT : C_ROW);
        bwin_font(&win, px + 12, py + 5, on ? F_BOLD : F_SANS, 13, PRESET_NAME[p], C_TEXT);
        hit(px, py, pw, 26, H_PRESET + p);
        px += pw + 10;
    }
}

static void draw_bar(void) {
    int song = g_song_now, y0 = win.h - BAR_H;
    bwin_fill_rect(&win, 0, y0, win.w, BAR_H, C_BAR);
    bwin_fill_rect(&win, 0, y0, win.w, 1, 0x2A2E38u);
    int infow = win.w / 4;
    int show = g_buffering >= 0 ? g_buffering : song;
    if (show >= 0) {
        draw_track_cover(show, 14, y0 + 12, 60);
        hit(14, y0 + 12, 60, 60, H_COVER);
        text_fit(86, y0 + 20, F_BOLD, 14, g_tracks[show].title, infow - 90, C_TEXT);
        text_fit(86, y0 + 42, F_SANS, 12, g_buffering >= 0 ? "Loading..." : g_tracks[show].artist, infow - 90, g_buffering >= 0 ? C_ACCENT2 : C_DIM);
    } else {
        text_fit(20, y0 + 32, F_SANS, 13, g_status[0] ? g_status : "Pick a song", infow, C_DIM);
    }
    int cx = win.w / 2, by = y0 + 14;
    int playing = g_playing && !g_paused;
    bwin_fill_circle(&win, cx, by + 16, 17, C_TEXT);
    if (playing) { bwin_fill_rect(&win, cx - 6, by + 8, 4, 16, C_BAR); bwin_fill_rect(&win, cx + 2, by + 8, 4, 16, C_BAR); }
    else tri(cx - 4, by + 7, 13, 18, 1, C_BAR);
    hit(cx - 18, by - 2, 36, 36, H_PLAY);
    tri(cx - 58, by + 9, 10, 14, 0, C_TEXT); bwin_fill_rect(&win, cx - 62, by + 9, 3, 14, C_TEXT);
    hit(cx - 68, by, 28, 32, H_PREV);
    tri(cx + 48, by + 9, 10, 14, 1, C_TEXT); bwin_fill_rect(&win, cx + 59, by + 9, 3, 14, C_TEXT);
    hit(cx + 42, by, 28, 32, H_NEXT);
    unsigned int shc = g_shuffle ? C_ACCENT2 : C_DIM;
    bwin_line(&win, cx - 104, by + 10, cx - 88, by + 22, shc); bwin_line(&win, cx - 104, by + 22, cx - 88, by + 10, shc);
    bwin_fill_rect(&win, cx - 90, by + 9, 3, 3, shc); bwin_fill_rect(&win, cx - 90, by + 21, 3, 3, shc);
    if (g_shuffle) bwin_fill_circle(&win, cx - 96, by + 30, 2, C_ACCENT2);
    hit(cx - 110, by, 28, 34, H_SHUFFLE);
    unsigned int rc = g_repeat ? C_ACCENT2 : C_DIM;
    bwin_rect(&win, cx + 86, by + 9, 18, 13, rc);
    tri(cx + 100, by + 6, 6, 7, 1, rc);
    if (g_repeat == REPEAT_ONE) bwin_font(&win, cx + 92, by + 9, F_BOLD, 10, "1", C_ACCENT2);
    if (g_repeat) bwin_fill_circle(&win, cx + 95, by + 30, 2, C_ACCENT2);
    hit(cx + 80, by, 30, 34, H_REPEAT);
    int sw = win.w / 2 - 40, sx = cx - sw / 2, sy = y0 + 62;
    double dur = song >= 0 ? g_tracks[song].dur : 0;
    char a[16], b[16];
    fmt_time(a, sizeof(a), g_pos_now);
    fmt_time(b, sizeof(b), dur);
    if (song >= 0) bwin_font(&win, sx - 40, sy - 6, F_SANS, 11, a, C_DIM);
    if (song >= 0 && dur) bwin_font(&win, sx + sw + 8, sy - 6, F_SANS, 11, b, C_DIM);
    bwin_fill_rect(&win, sx, sy, sw, 4, 0x343846u);
    if (dur > 0 && song >= 0) {
        int f = (int)(sw * g_pos_now / dur);
        if (f > sw) f = sw;
        bwin_fill_rect(&win, sx, sy, f, 4, C_ACCENT);
        bwin_fill_circle(&win, sx + f, sy + 2, 6, C_TEXT);
    }
    hit(sx - 6, sy - 8, sw + 12, 20, H_SEEK);
    int vx = win.w - 170, vw = 110, vy = y0 + 40;
    int vol = banana_volume(-1);
    if (vol >= 0) {
        bwin_fill_rect(&win, vx - 22, vy - 3, 5, 8, C_DIM);
        tri(vx - 18, vy - 7, 7, 16, 1, C_DIM);
        bwin_fill_rect(&win, vx, vy, vw, 4, 0x343846u);
        bwin_fill_rect(&win, vx, vy, vw * vol / 100, 4, C_ACCENT);
        bwin_fill_circle(&win, vx + vw * vol / 100, vy + 2, 5, C_TEXT);
        hit(vx - 6, vy - 10, vw + 12, 24, H_VOLUME);
    }
    /* a message (errors, "added to the playlist") above the volume */
    if (g_status[0] && show >= 0) text_fit(win.w - 300, y0 + 10, F_SANS, 11, g_status, 290, C_ACCENT2);
    if (g_dl_failed >= 0 && g_dl_err[0] && show < 0) text_fit(win.w - 300, y0 + 10, F_SANS, 11, g_dl_err, 290, C_ERR);
}

static void draw_all(void) {
    g_nhits = 0;
    g_song_now = now_playing(&g_pos_now);
    banana_lock(&g_data_lock);
    build_list();
    bwin_fill_rect(&win, SIDE_W, TOP_H, win.w - SIDE_W, win.h - TOP_H - BAR_H, C_BG);
    draw_sidebar();
    draw_top();
    int x = SIDE_W, y = TOP_H, w = win.w - SIDE_W, h = win.h - TOP_H - BAR_H;
    int kind = list_kind();
    if (kind == 4) draw_home(x, y, w, h);
    else if (g_view == V_NOW && !g_search[0]) draw_now(x, y, w, h);
    else if (g_view == V_EQ && !g_search[0]) draw_eq(x, y, w, h);
    else if (kind == 1) draw_albums(x, y, w, h);
    else if (kind == 2 || kind == 3) draw_heads(x, y, w, h, kind == 3);
    else draw_rows(x, y, w, h);
    draw_bar();
    draw_menu();
    draw_dialog();
    banana_unlock(&g_data_lock);
}

/* ── input ────────────────────────────────────────────────────────── */
static int g_drag;

static void drag_to(int id, int mx, int my) {
    for (int i = 0; i < g_nhits; i++) {
        if (g_hits[i].id != id) continue;
        hit_t* h = &g_hits[i];
        if (id == H_SEEK && g_song_now >= 0 && g_tracks[g_song_now].dur > 0) {
            double f = (mx - h->x - 6) / (double)(h->w - 12);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            if (g_paused) g_paused_pos = f * g_tracks[g_song_now].dur;
            else engine_play(g_song_now, f * g_tracks[g_song_now].dur);
        } else if (id == H_VOLUME) {
            int v = (mx - h->x - 6) * 100 / (h->w - 12);
            banana_volume(v < 0 ? 0 : v > 100 ? 100 : v);
        } else if (id >= H_EQ && id < H_EQ + EQ_BANDS) {
            int mid = h->y + h->h / 2, half = (h->h - 20) / 2;
            int db = (mid - my) * 12 / (half ? half : 1);
            g_eq_db[id - H_EQ] = db < -12 ? -12 : db > 12 ? 12 : db;
            g_eq_gen++;
        }
        return;
    }
}

static void set_view(int v) {
    g_view = v;
    g_scroll = 0;
    g_home_scroll = 0;
    g_sel = -1;
    g_open_album = -1;
    g_open_artist[0] = 0;
    g_open_pl = -1;
    g_search[0] = 0;
}

static void click(int mx, int my, int dbl, int right) {
    int id = hit_at(mx, my);
    if (g_dlg) {
        if (id == H_DLG + 0 || id == H_DLG + 1) g_focus = id - H_DLG;
        else if (id == H_DLG + 2) dialog_submit(0);
        else if (id == H_DLG + 3) dialog_submit(1);
        else if (id == H_DLG + 4) g_dlg = D_NONE;
        else if (id == H_DLG + 5) logout();
        else if (id == H_DLG + 6) g_dlg_public = !g_dlg_public;
        return;
    }
    if (g_menu_open) {
        if (id >= H_MENU && id < H_MENU + 40) menu_do(id - H_MENU);
        else g_menu_open = 0;
        return;
    }
    banana_lock(&g_data_lock);
    if (id >= H_CARD) {
        int s = (id - H_CARD) / 1000, k = (id - H_CARD) % 1000;
        const int* l = s == 0 ? g_rec : s == 1 ? g_hist : g_top;
        int n = s == 0 ? g_nrec : s == 1 ? g_nhist : g_ntop;
        if (k < n) {
            if (right) open_menu(mx, my, l[k]);
            else play_list(l, n, k);
        }
        banana_unlock(&g_data_lock);
        return;
    }
    if (id >= H_ROW) {
        int row = id - H_ROW;
        int kind = list_kind();
        if (row < g_nlist) {
            if (kind == 1) { g_open_album = g_list[row]; g_scroll = 0; g_sel = -1; }
            else if (kind == 2) { snprintf(g_open_artist, sizeof(g_open_artist), "%s", g_tracks[g_list[row]].artist); g_scroll = 0; g_sel = -1; }
            else if (kind == 3) { g_open_pl = g_pls[g_list[row]].id; g_scroll = 0; g_sel = -1; }
            else {
                g_sel = row;
                if (right) open_menu(mx, my, g_list[row]);
                else if (dbl) {
                    if (g_view == V_QUEUE && !g_search[0]) { banana_lock(&g_q_lock); g_qpos = row; banana_unlock(&g_q_lock); engine_play(g_list[row], 0); }
                    else play_list(g_list, g_nlist, row);
                }
            }
        }
        banana_unlock(&g_data_lock);
        return;
    }
    banana_unlock(&g_data_lock);
    if (right) return;
    if (id >= H_PRESET && id < H_PRESET + NPRESETS) {
        for (int b = 0; b < EQ_BANDS; b++) g_eq_db[b] = PRESET[id - H_PRESET][b];
        g_eq_gen++;
        settings_save();
        return;
    }
    if (id >= H_EQ && id < H_EQ + EQ_BANDS) { g_drag = id; drag_to(id, mx, my); return; }
    if (id >= H_SORT && id < H_SORT + 5) { g_sort = id - H_SORT; settings_save(); return; }
    if (id >= H_NAV && id < H_NAV + V_COUNT) { set_view(id - H_NAV); return; }
    switch (id) {
    case H_PLAY: play_pause(); break;
    case H_PREV: skip_track(-1); break;
    case H_NEXT: skip_track(1); break;
    case H_SHUFFLE:
        g_shuffle = !g_shuffle;
        banana_lock(&g_q_lock);
        if (g_shuffle) shuffle_queue();
        banana_unlock(&g_q_lock);
        settings_save();
        break;
    case H_REPEAT: g_repeat = (g_repeat + 1) % 3; settings_save(); break;
    case H_SEEK: g_drag = id; break;
    case H_VOLUME: g_drag = id; drag_to(id, mx, my); break;
    case H_BACK: g_open_album = -1; g_open_artist[0] = 0; g_open_pl = -1; g_scroll = 0; break;
    case H_COVER: set_view(V_NOW); break;
    case H_ACCOUNT: open_dialog(D_LOGIN); break;
    case H_NEWPL:
        if (!g_user[0]) open_dialog(D_LOGIN);
        else open_dialog(D_NEWPL);
        break;
    case H_DELPL: { playlist_t* p = open_playlist(); if (p) { job(J_PL_DELETE, p->id, 0, NULL); g_open_pl = -1; } break; }
    case H_PLAYALL: if (g_nlist) play_list(g_list, g_nlist, 0); break;
    }
}

static void dialog_key(int k) {
    char* f = g_f[g_dlg == D_LOGIN ? g_focus : 0];
    if (k == 27) { g_dlg = D_NONE; return; }
    if (k == '\t' || k == BANANA_KEY_DOWN || k == BANANA_KEY_UP) { if (g_dlg == D_LOGIN) g_focus ^= 1; return; }
    if (k == '\n' || k == '\r') {
        if (g_dlg == D_LOGIN && g_focus == 0) { g_focus = 1; return; }
        dialog_submit(0);
        return;
    }
    if (k == '\b' || k == 127) { size_t n = strlen(f); if (n) f[n - 1] = 0; return; }
    if (k >= 32 && k < 256) {
        size_t n = strlen(f);
        if (n < sizeof(g_f[0]) - 1) { f[n] = (char)k; f[n + 1] = 0; }
    }
}

static void key(int k) {
    if (g_dlg) { dialog_key(k); return; }
    if (g_menu_open) { if (k == 27) g_menu_open = 0; return; }
    int rows = (win.h - TOP_H - BAR_H - 26) / ROW_H;
    switch (k) {
    case BANANA_KEY_PLAY: play_pause(); return;
    case BANANA_KEY_NEXT: skip_track(1); return;
    case BANANA_KEY_PREV: skip_track(-1); return;
    case BANANA_KEY_STOP: if (g_playing && !g_paused) engine_pause(); return;
    case BANANA_KEY_UP:
        if (list_kind() == 4) { g_home_scroll -= 60; if (g_home_scroll < 0) g_home_scroll = 0; return; }
        if (g_sel > 0) g_sel--; else g_scroll -= 1;
        if (g_sel >= 0 && g_sel < g_scroll) g_scroll = g_sel;
        return;
    case BANANA_KEY_DOWN:
        if (list_kind() == 4) { g_home_scroll += 60; return; }
        if (g_sel < g_nlist - 1 && g_sel >= 0) g_sel++; else g_scroll += 1;
        if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
        return;
    case BANANA_KEY_PGUP: g_scroll -= rows; g_home_scroll -= 300; if (g_home_scroll < 0) g_home_scroll = 0; return;
    case BANANA_KEY_PGDN: g_scroll += rows; g_home_scroll += 300; return;
    case BANANA_KEY_HOME: g_scroll = 0; g_home_scroll = 0; g_sel = g_nlist ? 0 : -1; return;
    case BANANA_KEY_END:  g_scroll = g_nlist; g_sel = g_nlist - 1; return;
    case BANANA_KEY_LEFT:  if (g_song_now >= 0 && !g_paused) engine_play(g_song_now, g_pos_now - 10); return;
    case BANANA_KEY_RIGHT: if (g_song_now >= 0 && !g_paused) engine_play(g_song_now, g_pos_now + 10); return;
    case BANANA_KEY_F1 + 4: job(J_PLAYLISTS, 0, 0, NULL); job(J_PERSONAL, 0, 0, NULL); return;   /* F5: refresh */
    case '\n': case '\r':
        banana_lock(&g_data_lock);
        if (g_sel >= 0 && list_kind() == 0) play_list(g_list, g_nlist, g_sel);
        banana_unlock(&g_data_lock);
        return;
    case 27:
        if (g_search[0]) g_search[0] = 0;
        else { g_open_album = -1; g_open_artist[0] = 0; g_open_pl = -1; }
        g_scroll = 0;
        return;
    case '\b': case 127: { size_t n = strlen(g_search); if (n) g_search[n - 1] = 0; g_scroll = 0; return; }
    case ' ': if (!g_search[0]) { play_pause(); return; } break;
    default: break;
    }
    if (k >= 32 && k < 256 && g_view != V_EQ) {
        size_t n = strlen(g_search);
        if (n < sizeof(g_search) - 1) { g_search[n] = (char)k; g_search[n + 1] = 0; }
        g_scroll = 0;
        g_sel = -1;
    }
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_QUIET);
    if (bwin_open(&win, "Amethyst Music", WIN_W, WIN_H) < 0) {
        printf("amethyst: the desktop is not running (startx)\n");
        return 1;
    }
    bwin_resizable(&win, 760, 480);
    bwin_media_keys(&win);
    bwin_fill_rect(&win, 0, 0, win.w, win.h, C_BG);
    bwin_update(&win);

    conf_dirs();
    account_load();
    if (argc > 1) {
        snprintf(g_server, sizeof(g_server), "%s", argv[1]);
        size_t n = strlen(g_server);
        if (n && g_server[n - 1] != '/' && n < sizeof(g_server) - 1) strcat(g_server, "/");
    }
    settings_load();
    cache_scan();

    int eng = banana_thread(engine_thread, NULL);
    banana_thread(download_thread, NULL);
    banana_thread(net_thread, NULL);
    banana_thread(loader_thread, NULL);
    if (eng < 0) { printf("amethyst: no thread\n"); return 1; }

    unsigned int last = 0;
    int dirty = 1, seen_data = -1, seen_cover = -1, counted_for = -1;
    for (;;) {
        banana_event_t ev;
        int timeout = 30;
        while (bwin_wait_event(&win, &ev, timeout)) {
            timeout = 0;
            if (ev.type == BANANA_EV_CLOSE) goto out;
            if (ev.type == BANANA_EV_RESIZE) { dirty = 1; continue; }
            if (ev.type == BANANA_EV_MOUSE_DOWN && (ev.button == 1 || ev.button == 2)) {
                unsigned int now = banana_ticks();
                int id = hit_at(ev.x, ev.y);
                int dbl = ev.button == 1 && id == g_last_click_id && now - g_last_click < 450;
                g_last_click = now;
                g_last_click_id = id;
                click(ev.x, ev.y, dbl, ev.button == 2);
                dirty = 1;
            } else if (ev.type == BANANA_EV_MOUSE_MOVE) {
                if (g_drag && g_drag != H_SEEK && (ev.buttons & 1)) { drag_to(g_drag, ev.x, ev.y); dirty = 1; }
                int id = hit_at(ev.x, ev.y);
                if (g_menu_open) {
                    int mh = id >= H_MENU && id < H_MENU + 40 ? id - H_MENU : -1;
                    if (mh != g_menu_hover) { g_menu_hover = mh; dirty = 1; }
                } else {
                    int hv = id >= H_CARD ? id : id >= H_ROW ? id - H_ROW : -1;
                    if (hv != g_hover) { g_hover = hv; dirty = 1; }
                }
            } else if (ev.type == BANANA_EV_MOUSE_UP) {
                if (g_drag >= H_EQ && g_drag < H_EQ + EQ_BANDS) settings_save();
                if (g_drag == H_SEEK) drag_to(H_SEEK, ev.x, ev.y);
                g_drag = 0;
            } else if (ev.type == BANANA_EV_KEY) {
                key(ev.key);
                dirty = 1;
            }
        }
        unsigned int now = banana_ticks();
        if (seen_data != g_data_gen) { seen_data = g_data_gen; dirty = 1; }
        if (seen_cover != g_cover_gen) { seen_cover = g_cover_gen; dirty = 1; }
        /* a song heard: its play counts (and goes into the history) */
        if (g_song_now >= 0 && g_song_now != counted_for && g_pos_now > 5 && g_user[0] && g_song_now < g_ntracks) {
            counted_for = g_song_now;
            job(J_COUNT_PLAY, g_tracks[g_song_now].id, 0, NULL);
        }
        int animate = (g_playing && !g_paused) || g_buffering >= 0;
        if (g_playing && !g_paused) update_spectrum();
        if (dirty || now - last >= (animate ? 33u : 500u)) {
            draw_all();
            bwin_update(&win);
            last = now;
            dirty = 0;
        }
    }
out:
    settings_save();
    g_quit = 1;
    banana_lock(&g_e_lock);
    banana_cond_broadcast(&g_e_cond);
    banana_unlock(&g_e_lock);
    banana_lock(&g_dl_lock);
    banana_cond_broadcast(&g_dl_cond);
    banana_unlock(&g_dl_lock);
    banana_lock(&g_j_lock);
    banana_cond_broadcast(&g_j_cond);
    banana_unlock(&g_j_lock);
    banana_audio_stop();
    banana_join(eng);
    bwin_close(&win);
    return 0;
}
