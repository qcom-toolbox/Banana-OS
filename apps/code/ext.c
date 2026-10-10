/* Banana Code: extensions.
 *
 * An extension is a folder with an extension.json:
 *
 *   { "id": "claude", "name": "Claude", "version": "1.0", "publisher": "...",
 *     "description": "...", "icon": "C", "color": "#D97757",
 *     "view": "panel.html",
 *     "commands": [ { "command": "claude.explain", "title": "Claude: Explain the selection" } ] }
 *
 * Built-in ones are in /apps/code/extensions, the user's in
 * ~/.bcode/extensions. Its view is a web page (HTML, CSS, JavaScript - the
 * system browser's engine) shown on the editor's right. The page and the
 * editor exchange JSON messages: the page calls banana.postMessage(json)
 * and hears the editor in window "message" events (event.data).
 *
 * From the page:                       the editor answers
 *   {type:"ready"}                       (queued messages are delivered)
 *   {type:"getContext"}                  {type:"context", file, language, text,
 *                                         selection, line, project, problems, output}
 *   {type:"insertText", text}            inserted at the cursor
 *   {type:"replaceSelection", text}
 *   {type:"newFile", name, text}         a new file in the folder, opened
 *   {type:"openFile", path, line}
 *   {type:"runCommand", command}         "build", "run", "save"...
 *   {type:"status", text}                in the status bar
 *   {type:"getState", key}               {type:"state", key, value}
 *   {type:"setState", key, value}        kept in ~/.bcode/state/<id>/<key>
 *   {type:"http", id, method, url, headers:{...}, body}
 *                                        {type:"httpResponse", id, status, body, error}
 *                                        (done in the background: the editor
 *                                        keeps working; any header, any status)
 * From the editor:
 *   {type:"command", command}            one of its commands was chosen
 */
#include "code.h"

ext_t g_ext[MAX_EXT];
int   g_next;
int   g_ext_open = -1;

#define USER_EXT_DIR "/home/banana/.bcode/extensions"
#define STATE_DIR    "/home/banana/.bcode/state"
#define BUILTIN_DIR  "/apps/code/extensions"
#define DISABLED     "/home/banana/.bcode/disabled"

static unsigned parse_color(const char* s) {
    if (!s || s[0] != '#') return 0x3A7BD5u;
    return (unsigned)strtoul(s + 1, NULL, 16) & 0xFFFFFFu;
}

static int disabled(const char* id) {
    char* t = read_file(DISABLED, NULL);
    if (!t) return 0;
    int hit = 0;
    for (char* p = t; *p; ) {
        char* e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        if ((int)strlen(id) == n && !strncmp(p, id, (size_t)n)) hit = 1;
        if (!e) break;
        p = e + 1;
    }
    free(t);
    return hit;
}

static void load_one(const char* dir, int builtin) {
    if (g_next >= MAX_EXT) return;
    char p[PATH_MAX_];
    join_path(p, sizeof(p), dir, "extension.json");
    char* t = read_file(p, NULL);
    if (!t) return;
    json_t* j = json_parse(t);
    free(t);
    if (!j || j->type != J_OBJ || !json_str(j, "id", "")[0]) { json_free(j); return; }
    for (int i = 0; i < g_next; i++) if (!strcmp(g_ext[i].id, json_str(j, "id", ""))) { json_free(j); return; }
    ext_t* e = &g_ext[g_next];
    memset(e, 0, sizeof(*e));
    snprintf(e->id, sizeof(e->id), "%s", json_str(j, "id", ""));
    snprintf(e->name, sizeof(e->name), "%s", json_str(j, "name", e->id));
    snprintf(e->version, sizeof(e->version), "%s", json_str(j, "version", ""));
    snprintf(e->publisher, sizeof(e->publisher), "%s", json_str(j, "publisher", ""));
    snprintf(e->description, sizeof(e->description), "%s", json_str(j, "description", ""));
    snprintf(e->view, sizeof(e->view), "%s", json_str(j, "view", ""));
    snprintf(e->icon, sizeof(e->icon), "%s", json_str(j, "icon", "?"));
    snprintf(e->dir, sizeof(e->dir), "%s", dir);
    e->color = parse_color(json_str(j, "color", ""));
    e->builtin = builtin;
    e->enabled = !disabled(e->id);
    e->webview = -1;
    json_t* cmds = json_get(j, "commands");
    for (json_t* c = cmds && cmds->type == J_ARR ? cmds->child : NULL; c && e->ncmds < MAX_EXT_CMDS; c = c->next) {
        const char* id = json_str(c, "command", "");
        if (!id[0]) continue;
        snprintf(e->cmd_id[e->ncmds], 64, "%s", id);
        snprintf(e->cmd_title[e->ncmds], 80, "%s", json_str(c, "title", id));
        e->ncmds++;
    }
    json_free(j);
    g_next++;
}

static void load_dir(const char* root, int builtin) {
    banana_dirent_t de;
    for (int i = 0; __banana->readdir(root, i, &de) == 0; i++) {
        if (!de.is_dir) continue;
        char d[PATH_MAX_];
        join_path(d, sizeof(d), root, de.name);
        load_one(d, builtin);
    }
}

void ext_load_all(void) {
    for (int i = 0; i < g_next; i++) if (g_ext[i].webview >= 0) bweb_close(g_ext[i].webview);
    g_next = 0;
    g_ext_open = -1;
    load_dir(BUILTIN_DIR, 1);
    load_dir(USER_EXT_DIR, 0);
}

void ext_set_enabled(int i, int on) {
    if (i < 0 || i >= g_next) return;
    g_ext[i].enabled = on;
    if (!on && g_ext[i].webview >= 0) { bweb_close(g_ext[i].webview); g_ext[i].webview = -1; if (g_ext_open == i) g_ext_open = -1; }
    sbuf_t b;
    sb_init(&b);
    for (int k = 0; k < g_next; k++) if (!g_ext[k].enabled) { sb_add(&b, g_ext[k].id); sb_add(&b, "\n"); }
    mkdir_p("/home/banana/.bcode");
    write_file(DISABLED, b.s ? b.s : "", b.len);
    sb_free(&b);
}

static int remove_tree(const char* dir, int depth) {
    if (depth > 8) return -1;
    banana_dirent_t de;
    char names[64][64];
    int dirs[64], n = 0;
    for (int i = 0; n < 64 && __banana->readdir(dir, i, &de) == 0; i++) { snprintf(names[n], 64, "%s", de.name); dirs[n] = de.is_dir; n++; }
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX_];
        join_path(p, sizeof(p), dir, names[i]);
        if (dirs[i]) remove_tree(p, depth + 1);
        else __banana->remove(p);
    }
    return __banana->remove(dir);
}

int ext_uninstall(int i, char* msg, int cap) {
    if (i < 0 || i >= g_next) return -1;
    if (g_ext[i].builtin) { snprintf(msg, (size_t)cap, "%s comes with Banana Code: disable it instead", g_ext[i].name); return -1; }
    char name[64];
    snprintf(name, sizeof(name), "%s", g_ext[i].name);
    remove_tree(g_ext[i].dir, 0);
    ext_load_all();
    snprintf(msg, (size_t)cap, "Uninstalled %s", name);
    return 0;
}

static int copy_tree(const char* from, const char* to, int depth) {
    if (depth > 8 || mkdir_p(to) != 0) return -1;
    banana_dirent_t de;
    for (int i = 0; __banana->readdir(from, i, &de) == 0; i++) {
        char a[PATH_MAX_], b[PATH_MAX_];
        join_path(a, sizeof(a), from, de.name);
        join_path(b, sizeof(b), to, de.name);
        if (de.is_dir) { if (copy_tree(a, b, depth + 1)) return -1; continue; }
        int l = 0;
        char* t = read_file(a, &l);
        if (!t || write_file(b, t, l)) { free(t); return -1; }
        free(t);
    }
    return 0;
}

int ext_install_folder(const char* dir, char* msg, int cap) {
    char p[PATH_MAX_];
    join_path(p, sizeof(p), dir, "extension.json");
    char* t = read_file(p, NULL);
    json_t* j = t ? json_parse(t) : NULL;
    free(t);
    const char* id = j ? json_str(j, "id", "") : "";
    if (!id[0]) { snprintf(msg, (size_t)cap, "%s has no extension.json with an \"id\"", dir); json_free(j); return -1; }
    char dst[PATH_MAX_];
    join_path(dst, sizeof(dst), USER_EXT_DIR, id);
    int rc = copy_tree(dir, dst, 0);
    snprintf(msg, (size_t)cap, rc ? "Could not copy %s" : "Installed %s", json_str(j, "name", id));
    json_free(j);
    ext_load_all();
    return rc;
}

/* ── the view and its messages ── */
#define QMAX 16
static char* g_queue[QMAX];          /* posts waiting for the page's "ready" */
static int   g_queue_ext[QMAX];
static int   g_nqueue;
static int   g_ready[MAX_EXT];

static void post(int i, const char* json) {
    if (i < 0 || i >= g_next || g_ext[i].webview < 0) return;
    if (!g_ready[i]) {
        if (g_nqueue < QMAX) { g_queue_ext[g_nqueue] = i; g_queue[g_nqueue++] = strdup(json); }
        return;
    }
    bweb_post(g_ext[i].webview, json);
}

/* the page is ready: what waited for it */
static void flush_queue(int i) {
    int k = 0;
    for (int q = 0; q < g_nqueue; q++) {
        if (g_queue_ext[q] == i) { bweb_post(g_ext[i].webview, g_queue[q]); free(g_queue[q]); }
        else { g_queue_ext[k] = g_queue_ext[q]; g_queue[k++] = g_queue[q]; }
    }
    g_nqueue = k;
}

void ext_open_view(int i, int w, int h) {
    if (i < 0 || i >= g_next || !g_ext[i].enabled || !g_ext[i].view[0]) return;
    ext_t* e = &g_ext[i];
    if (e->webview < 0) {
        e->webview = bweb_open(w, h);
        if (e->webview < 0) { set_status("%s: could not open its view", e->name); return; }
        char p[PATH_MAX_];
        join_path(p, sizeof(p), e->dir, e->view);
        g_ready[i] = 0;
        bweb_load(e->webview, p);
    } else {
        bweb_resize(e->webview, w, h);
    }
    g_ext_open = i;
}

void ext_resize(int w, int h) { if (g_ext_open >= 0 && g_ext[g_ext_open].webview >= 0) bweb_resize(g_ext[g_ext_open].webview, w, h); }
void ext_close_view(void) { g_ext_open = -1; }

void ext_command(const char* cmd) {
    for (int i = 0; i < g_next; i++)
        for (int c = 0; c < g_ext[i].ncmds; c++)
            if (!strcmp(g_ext[i].cmd_id[c], cmd) && g_ext[i].enabled) {
                open_ext_panel(i);
                sbuf_t b;
                sb_init(&b);
                sb_add(&b, "{\"type\":\"command\",\"command\":");
                sb_json_str(&b, cmd, -1);
                sb_add(&b, "}");
                post(i, b.s);
                sb_free(&b);
                return;
            }
}

/* the editor's state for the page */
static void send_context(int i) {
    sbuf_t b;
    sb_init(&b);
    doc_t* d = cur_doc();
    sb_add(&b, "{\"type\":\"context\",\"file\":");
    sb_json_str(&b, d ? d->path : "", -1);
    sb_add(&b, ",\"language\":");
    sb_json_str(&b, d ? doc_lang_name(d->lang) : "", -1);
    sb_printf(&b, ",\"line\":%d,\"text\":", d ? d->cy + 1 : 0);
    int len = 0;
    char* t = d ? doc_text(d, &len) : NULL;
    if (len > 120000) len = 120000;                      /* (the page gets the first 120 KB) */
    sb_json_str(&b, t ? t : "", len);
    free(t);
    sb_add(&b, ",\"selection\":");
    char* s = d ? doc_sel_text(d) : NULL;
    sb_json_str(&b, s ? s : "", -1);
    free(s);
    sb_add(&b, ",\"project\":");
    sb_json_str(&b, g_folder, -1);
    sb_add(&b, ",\"problems\":");
    sbuf_t pb;
    sb_init(&pb);
    for (int k = 0; k < g_nproblems; k++)
        sb_printf(&pb, "%s:%d: %s: %s\n", g_problems[k].file, g_problems[k].line, g_problems[k].is_error ? "error" : "warning", g_problems[k].msg);
    sb_json_str(&b, pb.s ? pb.s : "", pb.len);
    sb_free(&pb);
    sb_add(&b, ",\"output\":");
    int ol = g_output.len > 20000 ? 20000 : g_output.len;
    sb_json_str(&b, g_output.s ? g_output.s + g_output.len - ol : "", ol);
    sb_add(&b, ",\"arch\":\"");
#ifdef __x86_64__
    sb_add(&b, "x86_64\"}");
#else
    sb_add(&b, "i686\"}");
#endif
    post(i, b.s);
    sb_free(&b);
}

/* HTTP in a thread of its own: the editor keeps drawing */
typedef struct {
    int  ext;
    char id[48];
    banana_http_req_t req;
    char* url; char* method; char* headers; char* body;
    banana_http_resp_t resp;
    int  rc;
    volatile int done;
} hjob_t;
#define HJOBS 4
static hjob_t* g_jobs[HJOBS];

static int http_thread(void* arg) {
    hjob_t* h = arg;
    h->rc = banana_http_fetch(&h->req, &h->resp);
    h->done = 1;
    return 0;
}

static void start_http(int i, const json_t* m) {
    int slot = -1;
    for (int k = 0; k < HJOBS; k++) if (!g_jobs[k]) { slot = k; break; }
    const char* id = json_str(m, "id", "");
    if (slot < 0) {
        sbuf_t b;
        sb_init(&b);
        sb_add(&b, "{\"type\":\"httpResponse\",\"id\":");
        sb_json_str(&b, id, -1);
        sb_add(&b, ",\"status\":0,\"body\":\"\",\"error\":\"too many requests at once\"}");
        post(i, b.s);
        sb_free(&b);
        return;
    }
    hjob_t* h = calloc(1, sizeof(hjob_t));
    if (!h) return;
    h->ext = i;
    snprintf(h->id, sizeof(h->id), "%s", id);
    h->url = strdup(json_str(m, "url", ""));
    h->method = strdup(json_str(m, "method", "GET"));
    h->body = strdup(json_str(m, "body", ""));
    sbuf_t hb;
    sb_init(&hb);
    json_t* hd = json_get(m, "headers");
    for (json_t* c = hd && hd->type == J_OBJ ? hd->child : NULL; c; c = c->next) {
        if (c->type != J_STR || !c->key || strpbrk(c->key, "\r\n:") || strpbrk(c->str, "\r\n")) continue;
        if (!strcasecmp(c->key, "content-type")) continue;           /* (given as the body's type) */
        sb_printf(&hb, "%s: ", c->key);
        sb_add(&hb, c->str);
        sb_add(&hb, "\r\n");
    }
    h->headers = hb.s;
    const char* ct = "application/json";
    for (json_t* c = hd && hd->type == J_OBJ ? hd->child : NULL; c; c = c->next)
        if (c->type == J_STR && c->key && !strcasecmp(c->key, "content-type")) ct = c->str;
    h->req.method = h->method;
    h->req.url = h->url;
    h->req.headers = h->headers;
    h->req.body = h->body[0] ? h->body : NULL;
    h->req.body_len = strlen(h->body);
    h->req.content_type = strdup(ct);
    h->req.timeout_ms = 120000;                      /* a model can think for a while */
    g_jobs[slot] = h;
    if (banana_thread(http_thread, h) < 0) http_thread(h);
}

static void finish_http(void) {
    for (int k = 0; k < HJOBS; k++) {
        hjob_t* h = g_jobs[k];
        if (!h || !h->done) continue;
        sbuf_t b;
        sb_init(&b);
        sb_add(&b, "{\"type\":\"httpResponse\",\"id\":");
        sb_json_str(&b, h->id, -1);
        sb_printf(&b, ",\"status\":%d,\"body\":", h->rc == 0 ? h->resp.status : 0);
        sb_json_str(&b, h->resp.data ? h->resp.data : "", h->resp.data ? (int)h->resp.len : 0);
        sb_add(&b, ",\"error\":");
        sb_json_str(&b, h->rc == 0 ? "" : h->resp.err, -1);
        sb_add(&b, "}");
        post(h->ext, b.s);
        sb_free(&b);
        free(h->resp.data);
        free(h->url); free(h->method); free(h->headers); free(h->body); free((void*)h->req.content_type);
        free(h);
        g_jobs[k] = NULL;
        g_ui_dirty = 1;
    }
}

static void state_path(int i, const char* key, char* out, int cap) {
    char k[64];
    int n = 0;
    for (const char* c = key; *c && n < 60; c++) k[n++] = (char)((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' ? *c : '_');
    k[n] = 0;
    char d[PATH_MAX_];
    join_path(d, sizeof(d), STATE_DIR, g_ext[i].id);
    join_path(out, cap, d, k);
}

static void handle(int i, const char* msg) {
    json_t* m = json_parse(msg);
    if (!m || m->type != J_OBJ) { json_free(m); return; }
    const char* type = json_str(m, "type", "");
    doc_t* d = cur_doc();
    if (!strcmp(type, "ready")) {
        g_ready[i] = 1;
        flush_queue(i);
    } else if (!strcmp(type, "getContext")) {
        send_context(i);
    } else if (!strcmp(type, "insertText") || !strcmp(type, "replaceSelection")) {
        const char* t = json_str(m, "text", "");
        if (!d) { d = open_file(""); }
        if (d) {
            doc_snapshot(d, OP_OTHER);
            if (!strcmp(type, "insertText")) doc_clear_sel(d);
            doc_insert(d, t, (int)strlen(t));
            set_status("%s: inserted %d characters", g_ext[i].name, (int)strlen(t));
        }
    } else if (!strcmp(type, "newFile")) {
        const char* name = json_str(m, "name", "untitled.c");
        const char* t = json_str(m, "text", "");
        char p[PATH_MAX_];
        if (strstr(name, "..")) name = "untitled.c";
        if (name[0] == '/') snprintf(p, sizeof(p), "%s", name);
        else join_path(p, sizeof(p), g_folder[0] ? g_folder : "/home/banana", name);
        write_file(p, t, (int)strlen(t));
        open_file(p);
        set_status("%s: made %s", g_ext[i].name, name);
    } else if (!strcmp(type, "openFile")) {
        doc_t* o = open_file(json_str(m, "path", ""));
        int line = (int)json_num(m, "line", 0);
        if (o && line > 0) doc_goto(o, line - 1, 0, 0);
    } else if (!strcmp(type, "runCommand")) {
        run_builtin_command(json_str(m, "command", ""));
    } else if (!strcmp(type, "status")) {
        set_status("%s", json_str(m, "text", ""));
    } else if (!strcmp(type, "getState")) {
        char p[PATH_MAX_];
        const char* key = json_str(m, "key", "");
        state_path(i, key, p, sizeof(p));
        char* v = read_file(p, NULL);
        sbuf_t b;
        sb_init(&b);
        sb_add(&b, "{\"type\":\"state\",\"key\":");
        sb_json_str(&b, key, -1);
        sb_add(&b, ",\"value\":");
        sb_json_str(&b, v ? v : "", -1);
        sb_add(&b, "}");
        post(i, b.s);
        sb_free(&b);
        free(v);
    } else if (!strcmp(type, "setState")) {
        char p[PATH_MAX_], d2[PATH_MAX_];
        state_path(i, json_str(m, "key", ""), p, sizeof(p));
        join_path(d2, sizeof(d2), STATE_DIR, g_ext[i].id);
        mkdir_p(d2);
        const char* v = json_str(m, "value", "");
        if (v[0]) write_file(p, v, (int)strlen(v));
        else __banana->remove(p);
    } else if (!strcmp(type, "http")) {
        start_http(i, m);
    }
    json_free(m);
    g_ui_dirty = 1;
}

static char g_msg[1 << 20];

int ext_poll(void) {
    int changed = 0;
    finish_http();
    for (int i = 0; i < g_next; i++) {
        if (g_ext[i].webview < 0) continue;
        int f = bweb_poll(g_ext[i].webview);
        if ((f & BANANA_WEB_DIRTY) && i == g_ext_open) changed = 1;
        if (f & BANANA_WEB_MESSAGE) {
            int n;
            while ((n = bweb_message(g_ext[i].webview, g_msg, sizeof(g_msg))) >= 0) {
                if (n >= (int)sizeof(g_msg) - 1) continue;          /* too long: dropped */
                handle(i, g_msg);
                changed = 1;
            }
        }
    }
    return changed;
}
