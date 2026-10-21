/* SPDX-License-Identifier: BSD-2-Clause */
/* gl3test - checks OpenGL 3.3 (Mesa) on Banana OS.
 *
 *   gl3test           draws offscreen and checks the pixels (prints PASS / FAIL)
 *   gl3test window    a spinning shaded triangle in a window (Q or Escape quits)
 *   gl3test bench [softpipe | llvmpipe] [frames]
 *                     a block world like a game's (textured, depth-tested cubes)
 *                     drawn offscreen at 854x480: the renderer's frame rate
 *
 * Needs libOSMesa.so (ports/mesa/build.sh) in /lib, /usr/lib or /mnt/<disk>/lib. */
#include <GL/gl3.h>
#include <math.h>
#include <stdlib.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __x86_64__
static void fault(int sig, siginfo_t* info, void* context) {
    ucontext_t* u = context;
    unsigned long ip = u->uc_mcontext.gregs[REG_RIP];
    Dl_info d;
    if (dladdr((void*)ip, &d))
        printf("gl3test fault %d at %p: %s +0x%lx (%s)\n", sig, info->si_addr, d.dli_fname,
            ip - (unsigned long)d.dli_fbase, d.dli_sname ? d.dli_sname : "?");
    else printf("gl3test fault %d at %p, ip %lx\n", sig, info->si_addr, ip);
    exit(1);
}
#endif
static int g_fail;
static const char* required_renderer;

static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static const char* VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec3 col;\n"
    "uniform float angle;\n"
    "out vec3 vcol;\n"
    "void main() {\n"
    "    float c = cos(angle), s = sin(angle);\n"
    "    gl_Position = vec4(c * pos.x - s * pos.y, s * pos.x + c * pos.y, 0.0, 1.0);\n"
    "    vcol = col;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec3 vcol;\n"
    "uniform vec3 tint;\n"
    "out vec4 frag;\n"
    "void main() { frag = vec4(vcol * tint, 1.0); }\n";

static GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("shader: %s\n", log);
    }
    return s;
}

static GLuint program(void) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, VS));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    check(ok, "GLSL 3.30 program links");
    return p;
}

static GLuint triangle(void) {
    static const float v[] = {     /* x, y, r, g, b */
        -0.8f, -0.8f, 1, 0, 0,
         0.8f, -0.8f, 0, 1, 0,
         0.0f,  0.8f, 0, 0, 1,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    return vao;
}

static int offscreen(void) {
    const int W = 64, H = 64;
    bgl3_ctx_t* gl = bgl3_create_offscreen(W, H);
    if (!gl) { printf("gl3test: %s\nFAIL\n", bgl3_error()); return 1; }
    printf("GL_VERSION  %s\nGL_RENDERER %s\nGLSL        %s\n", (const char*)glGetString(GL_VERSION),
           (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    if (required_renderer) check(strstr((const char*)glGetString(GL_RENDERER), required_renderer) != NULL, "requested rendering backend is active");
    if (required_renderer && !strcmp(required_renderer,"llvmpipe")) {
        void* lib=dlopen("libLLVM.so",RTLD_NOW|RTLD_NOLOAD);
        int (*runtime)(void)=lib?(int(*)(void))dlsym(lib,"banana_llvm_runtime_check"):NULL;
        int rc=runtime?runtime():-1;
        if(rc)printf("LLVM C++ runtime check: %d\n",rc);
        check(rc==0,"C++ narrow/wide streams and fragmented UTF-8 state");
        if(lib)dlclose(lib);
    }
    GLint major = 0, minor = 0, prof = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &prof);
    check(major > 3 || (major == 3 && minor >= 3), "OpenGL 3.3 or later");
    check(prof & GL_CONTEXT_CORE_PROFILE_BIT, "core profile");

    GLuint p = program();
    GLuint vao = triangle();
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(p);
    glUniform1f(glGetUniformLocation(p, "angle"), 0.0f);
    glUniform3f(glGetUniformLocation(p, "tint"), 1.0f, 1.0f, 1.0f);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    int w, h;
    const unsigned int* px = bgl3_pixels(gl, &w, &h);
    unsigned int mid = px[(H / 2) * W + W / 2] & 0xFFFFFF, corner = px[0] & 0xFFFFFF;
    unsigned int low = px[(H - 8) * W + 10] & 0xFFFFFF;                /* near the red corner, bottom left */
    printf("  center %06x, bottom-left %06x, top-left %06x\n", mid, low, corner);
    check(corner == 0, "clear color outside the triangle");
    check(mid != 0 && (mid & 0xFF) > 0x20 && ((mid >> 8) & 0xFF) > 0x20 && ((mid >> 16) & 0xFF) > 0x20,
          "interpolated colors inside it");
    check(((low >> 16) & 0xFF) > 0xA0 && (low & 0xFF) < 0x60, "red at the red vertex (rows top first)");

    /* a framebuffer object with a texture, then the texture drawn back */
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer object complete");
    glViewport(0, 0, 256, 256);
    glClearColor(0.0f, 0.5f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char rgba[4] = { 0 };
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    check(rgba[0] == 0 && rgba[1] >= 0x7e && rgba[1] <= 0x81 && rgba[2] == 0xff, "glReadPixels from the FBO");
    /* Many tiles and repeated readbacks exercise the CPU worker pool and
     * the virtual GPU completion path, with new uniforms every frame. */
    unsigned begin=banana_ticks();int burst_ok=1;
    for(int frame=0;frame<24;frame++) {
        float tint=(frame&1)?1.0f:0.0f;
        glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(p);glUniform1f(glGetUniformLocation(p,"angle"),0);
        glUniform3f(glGetUniformLocation(p,"tint"),tint,tint,tint);
        glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,3);
        glReadPixels(128,128,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        if((frame&1)?(rgba[0]<0x20||rgba[1]<0x20||rgba[2]<0x20):(rgba[0]||rgba[1]||rgba[2]))burst_ok=0;
    }
    printf("  24 frames at 256x256 with completion/readback: %u ms\n",banana_ticks()-begin);
    check(burst_ok,"repeated tiled rendering sees every uniform update");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    check(glGetError() == GL_NO_ERROR, "no GL error");
    bgl3_destroy(gl);
    printf("gl3test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}

/* ── bench: a small block world, the way a voxel game draws it ───────── */
static const char* BVS =
    "#version 330 core\n"
    "layout(location = 0) in vec3 pos;\n"
    "layout(location = 1) in vec2 uv;\n"
    "layout(location = 2) in float shade;\n"
    "uniform mat4 mvp;\n"
    "out vec2 vuv; out float vshade; out float fog;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); vuv = uv; vshade = shade;\n"
    "    fog = clamp(gl_Position.z / 60.0, 0.0, 1.0); }\n";
static const char* BFS =
    "#version 330 core\n"
    "in vec2 vuv; in float vshade; in float fog;\n"
    "uniform sampler2D tex;\n"
    "out vec4 frag;\n"
    "void main() { vec4 c = texture(tex, vuv); if (c.a < 0.5) discard;\n"
    "    frag = vec4(mix(c.rgb * vshade, vec3(0.6, 0.75, 1.0), fog), 1.0); }\n";

#define BN 32                                /* the world: BN x BN columns, up to BH high */
#define BH 12
static unsigned char g_world[BN][BH][BN];
static int solid(int x, int y, int z) {
    return x >= 0 && z >= 0 && y >= 0 && x < BN && y < BH && z < BN && g_world[x][y][z];
}

static void mat_mul(float* r, const float* a, const float* b) {   /* column-major r = a * b */
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[k * 4 + i] * b[c * 4 + k];
            t[c * 4 + i] = v;
        }
    memcpy(r, t, sizeof(t));
}

static int bench(int frames) {
    const int W = 854, H = 480;
    bgl3_ctx_t* gl = bgl3_create_offscreen(W, H);
    if (!gl) { printf("gl3test: %s\n", bgl3_error()); return 1; }
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    printf("GL_RENDERER %s\n", renderer);
    if (required_renderer && !strstr(renderer, required_renderer)) {
        printf("gl3test: %s was asked for\n", required_renderer); bgl3_destroy(gl); return 1;
    }
    /* hills of grass over dirt and stone */
    for (int x = 0; x < BN; x++)
        for (int z = 0; z < BN; z++) {
            int h = 4 + (int)(3.0f * sinf(x * 0.35f) + 3.0f * cosf(z * 0.28f) + 1.5f * sinf((x + z) * 0.6f));
            if (h < 1) h = 1;
            if (h > BH) h = BH;
            for (int y = 0; y < h; y++) g_world[x][y][z] = y == h - 1 ? 1 : y > h - 4 ? 2 : 3;
        }
    /* the faces that can be seen: 6 vertices of x y z u v shade each */
    static const int N[6][3] = { {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0}, {0,0,1}, {0,0,-1} };
    static const float SH[6] = { 1.0f, 0.5f, 0.8f, 0.8f, 0.65f, 0.65f };
    size_t cap = (size_t)BN * BN * BH * 6 * 6 * 6;
    float* v = malloc(cap * sizeof(float));
    if (!v) { printf("gl3test: out of memory\n"); return 1; }
    size_t n = 0;
    int faces = 0;
    for (int x = 0; x < BN; x++)
        for (int y = 0; y < BH; y++)
            for (int z = 0; z < BN; z++) {
                int b = g_world[x][y][z];
                if (!b) continue;
                for (int f = 0; f < 6; f++) {
                    if (solid(x + N[f][0], y + N[f][1], z + N[f][2])) continue;
                    /* the face's four corners */
                    static const unsigned char CO[6][4][3] = {
                        { {0,1,0}, {1,1,0}, {1,1,1}, {0,1,1} }, { {0,0,0}, {1,0,0}, {1,0,1}, {0,0,1} },
                        { {1,0,0}, {1,0,1}, {1,1,1}, {1,1,0} }, { {0,0,1}, {0,0,0}, {0,1,0}, {0,1,1} },
                        { {1,0,1}, {0,0,1}, {0,1,1}, {1,1,1} }, { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0} } };
                    float c[4][3];
                    for (int k = 0; k < 4; k++) {
                        c[k][0] = (float)(x + CO[f][k][0]); c[k][1] = (float)(y + CO[f][k][1]); c[k][2] = (float)(z + CO[f][k][2]);
                    }
                    /* the atlas: 4 tiles across (grass top, grass side, dirt, stone) */
                    int tile = b == 1 ? (f == 0 ? 0 : f == 1 ? 2 : 1) : b == 2 ? 2 : 3;
                    float u0 = tile * 0.25f, u1 = u0 + 0.25f;
                    float uv[4][2] = { {u0, 1}, {u1, 1}, {u1, 0}, {u0, 0} };
                    static const int TRI[6] = { 0, 1, 2, 0, 2, 3 };
                    for (int t = 0; t < 6; t++) {
                        int k = TRI[t];
                        v[n++] = c[k][0]; v[n++] = c[k][1]; v[n++] = c[k][2];
                        v[n++] = uv[k][0]; v[n++] = uv[k][1]; v[n++] = SH[f];
                    }
                    faces++;
                }
            }
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(float)), v, GL_STATIC_DRAW);
    free(v);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    /* a 64x16 texture atlas, 16x16 tiles with noise, drawn without filtering */
    static unsigned int texels[16][64];
    static const unsigned int BASE[4] = { 0x3a9a30, 0x7a5a38, 0x7a5a38, 0x808080 };
    unsigned int seed = 12345;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 64; x++) {
            seed = seed * 1103515245u + 12345u;
            int tile = x / 16, j = (int)((seed >> 16) % 40) - 20;
            unsigned int c = BASE[tile];
            if (tile == 1 && y < 4) c = BASE[0];          /* the grass edge on the side */
            int r = (int)(c & 0xFF) + j, g = (int)((c >> 8) & 0xFF) + j, b = (int)((c >> 16) & 0xFF) + j;
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            texels[y][x] = 0xFF000000u | (unsigned)b << 16 | (unsigned)g << 8 | (unsigned)r;
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, BVS));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, BFS));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { printf("gl3test: the bench program does not link\n"); return 1; }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    GLint umvp = glGetUniformLocation(p, "mvp");
    glViewport(0, 0, W, H);
    glEnable(GL_DEPTH_TEST);
    printf("  %d faces (%d triangles) at %dx%d, %ld cores\n", faces, faces * 2, W, H, sysconf(_SC_NPROCESSORS_ONLN));

    float proj[16] = { 0 };
    float fov = 1.0f / tanf(70.0f * 3.14159265f / 360.0f), zn = 0.1f, zf = 100.0f;
    proj[0] = fov * H / W; proj[5] = fov; proj[10] = (zf + zn) / (zn - zf); proj[11] = -1; proj[14] = 2 * zf * zn / (zn - zf);
    unsigned begin = banana_ticks(), first = 0;
    unsigned long sum = 0;
    for (int frame = 0; frame <= frames; frame++) {
        if (frame == 1) first = banana_ticks() - begin, begin = banana_ticks();   /* (frame 0 compiles the shaders) */
        float a = frame * 0.05f;
        float ex = BN / 2 + 22 * cosf(a), ey = BH + 6, ez = BN / 2 + 22 * sinf(a);
        float fx = BN / 2 - ex, fy = 2 - ey, fz = BN / 2 - ez;
        float fl = sqrtf(fx * fx + fy * fy + fz * fz); fx /= fl; fy /= fl; fz /= fl;
        float sx = -fz, sz = fx, sl = sqrtf(sx * sx + sz * sz); sx /= sl; sz /= sl;          /* side = f x up */
        float ux = -sz * fy, uy = sz * fx - sx * fz, uz = sx * fy;                            /* up = s x f */
        float view[16] = { sx, ux, -fx, 0, 0, uy, -fy, 0, sz, uz, -fz, 0,
                           -(sx * ex + sz * ez), -(ux * ex + uy * ey + uz * ez), fx * ex + fy * ey + fz * ez, 1 };
        float mvp[16];
        mat_mul(mvp, proj, view);
        glUniformMatrix4fv(umvp, 1, GL_FALSE, mvp);
        glClearColor(0.6f, 0.75f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(n / 6));
        glFinish();
        int w, h;
        const unsigned int* px = bgl3_pixels(gl, &w, &h);       /* what a window would show */
        sum += px[(h / 2) * w + w / 2] & 0xFF;
    }
    unsigned ms = banana_ticks() - begin;
    if (!ms) ms = 1;
    printf("  first frame %u ms (shaders compiled)\n", first);
    printf("bench: %s: %d frames in %u ms = %u.%u fps (check %lu)\n", renderer, frames, ms,
           frames * 1000 / ms, frames * 10000 / ms % 10, sum);
    bgl3_destroy(gl);
    return glGetError() == GL_NO_ERROR ? 0 : 1;
}

static int window(void) {
    bwin_t win;
    if (bwin_open(&win, "OpenGL 3.3", 480, 360) < 0) { printf("gl3test: no desktop\n"); return 1; }
    bwin_resizable(&win, 160, 120);
    bgl3_ctx_t* gl = bgl3_create(&win);
    if (!gl) { printf("gl3test: %s\n", bgl3_error()); bwin_close(&win); return 1; }
    GLuint p = program();
    GLuint vao = triangle();
    GLint ua = glGetUniformLocation(p, "angle"), ut = glGetUniformLocation(p, "tint");
    float a = 0;
    unsigned frames = 0, t0 = banana_ticks();
    char title[64];
    for (;;) {
        banana_event_t ev;
        while (bwin_event(&win, &ev)) {
            if (ev.type == BANANA_EV_CLOSE) goto out;
            if (ev.type == BANANA_EV_KEY && (ev.key == 'q' || ev.key == 27)) goto out;
            if (ev.type == BANANA_EV_RESIZE) bgl3_resize(gl);
        }
        glViewport(0, 0, win.w, win.h);
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(p);
        glUniform1f(ua, a);
        glUniform3f(ut, 1.0f, 1.0f, 1.0f);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        bgl3_swap(gl);
        a += 0.03f;
        frames++;
        unsigned t = banana_ticks();
        if (t - t0 >= 1000) {
            snprintf(title, sizeof(title), "OpenGL 3.3 - %u fps", frames * 1000 / (t - t0));
            bwin_title(&win, title);
            frames = 0;
            t0 = t;
        }
    }
out:
    bgl3_destroy(gl);
    bwin_close(&win);
    return 0;
}

int main(int argc, char** argv) {
#ifdef __x86_64__
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask); sigaction(SIGSEGV, &sa, NULL);
#endif
    if (argc > 1 && (!strcmp(argv[1], "virgl") || !strcmp(argv[1], "llvmpipe"))) {
        required_renderer = argv[1];
        setenv("GALLIUM_DRIVER", argv[1], 1);
    }
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        int frames = 60;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "softpipe") || !strcmp(argv[i], "llvmpipe") || !strcmp(argv[i], "virgl")) {
                required_renderer = argv[i];
                setenv("GALLIUM_DRIVER", argv[i], 1);
            } else if (atoi(argv[i]) > 0) frames = atoi(argv[i]);
        }
        return bench(frames);
    }
    if (argc > 1 && !strcmp(argv[1], "window")) return window();
    return offscreen();
}
