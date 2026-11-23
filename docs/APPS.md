# Apps, packages and the SDK

Installing apps (apt, the App Store), writing them (OpenGL, OpenCL...), hosting a repository. Back to the [README](../README.md); the SDK itself: [sdk/README.md](../sdk/README.md).

## Apps

Apps are made with the **Banana OS SDK** on Linux - see [sdk/README.md](../sdk/README.md). A `.bpk` package holds the app for both kernels; installing it unpacks it into `/apps/<name>/`.

- **Install**: `pkg install app.bpk`, or double-click the `.bpk` in Files (from a USB stick, `~/Downloads`, `/usr/share/extras`...)
- **Run**: type its name in a terminal (a console app runs there; a desktop app opens its window), or click it in **Apps** (Start menu or desktop)
- **Manage**: `pkg list`, `pkg info <name>`, `pkg remove <name>` (or right-click in Apps), `pkg ps`; End task in the Task Manager

The examples are built into the system: `pkg install /usr/share/extras/paint.bpk /usr/share/extras/clock.bpk` (or `apt install paint clock`, or the App Store) and open them from Apps.

### Packages: apt and the App Store

`pkg` installs a `.bpk` file you have; **`apt`** gets packages from **repositories** - folders of packages on a web server (or a USB stick) with a signed index - and the **App Store** is the same thing with the mouse.

| | |
|---|---|
| `apt update` | fetch every repository's list of packages (the App Store does it when it opens, and with *Refresh*) |
| `apt search <words>` / `apt show <name>` | find packages / one package's details |
| `apt install <name>...` | download and install - first what it depends on, marked *automatic* (also: `apt install file.bpk`) |
| `apt upgrade` / `apt list --upgradable` | every installed package that has a newer version |
| `apt remove <name>` / `apt autoremove` | uninstall / remove what was only installed for apps that are gone |
| `apt list [--installed]`, `apt reinstall <name>` | |
| `apt sources`, `apt add-repo <url> key=<key>`, `apt remove-repo <url or number>` | the repositories (`/etc/pkg/sources.list`) |

`apt-get` is the same command, and `pkg update / upgrade / search / show` go to it too.

**The App Store** (its desktop icon, Apps > *Get apps...*, or `store` in a terminal): *Discover* (everything, and the categories), *Updates* (with *Update all*), *Installed*; type to search; click an app for its page - *Install* (with what it needs), *Open* / *Run*, *Update*, *Remove*. Esc goes back, Up / Down / Enter pick an app, F5 refreshes.

**Sources** - `/etc/pkg/sources.list`, one repository per line:

```
repo https://packages.example.org/banana key=<the repository's public key, 64 hex digits>
repo file:///mnt/usb/repo trusted
repo file:///usr/share/extras trusted      # (there from the start)
```

A repository on the network must be signed: `apt update` checks the index's Ed25519 signature against the `key=` and ignores the repository if it does not match; each package downloaded must then have the size and SHA-256 the signed index says, or it is not installed. `trusted` (no signature) is for local folders - a folder of `.bpk` files without an index works too: its packages are read from the files.

**The index** (`Packages`) has one paragraph per package - `Package`, `Version`, `Title`, `Type`, `Category`, `Description`, `Author`, `Depends` (`libfoo (>= 1.2), bar | baz`), `Arch` (`i686 x86_64`), `Filename`, `Size`, `SHA256`, `Icon` - and versions compare the way Debian's do (`1.10` > `1.9`, `1.0~rc1` < `1.0`). An app says its category, what it needs and its picture in its Makefile:

```make
CATEGORY = Games
DEPENDS  = libfoo (>= 1.0)
ICON     = icon.png          # a square PNG, 64x64 is plenty
```

### OpenGL

```c
#include <GL/bgl.h>                 /* <GL/gl.h> + <GL/glu.h> + contexts on windows */

bwin_open(&win, "Spin", 640, 480);
bgl_ctx_t* gl = bgl_create(&win);   /* now current */
for (;;) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ... draw ...
    bgl_swap(gl);                   /* the frame shows in the window */
    /* BANANA_EV_RESIZE: bgl_resize(gl) and glViewport */
}
```

with `USE_GL = 1` in the app's Makefile. `bgl_create_offscreen(w, h)` + `bgl_pixels()` draw without a window. What is there: OpenGL 1.3's fixed-function pipeline - `glBegin`/`glEnd`, vertex arrays (`glDrawArrays`, `glDrawElements`), display lists, the matrix stacks, `glOrtho`/`glFrustum`/`gluPerspective`/`gluLookAt`, lighting and materials, textures (`glTexImage2D`, `glTexSubImage2D`, filters, wrap modes, `GL_MODULATE`/`REPLACE`/`DECAL`/`ADD`), depth test, blending, alpha test, fog, scissor, color mask, culling, polygon modes, lines and points, `glReadPixels`, `glGetError`. Not there (yet): shaders (OpenGL 2+), stencil, accumulation buffer, mipmap levels (level 0 is used), 1D/3D textures. Drawing is queued and done at `bgl_swap` / `glFinish` by every core; `bgl_threads()` says how many. `sdk/lib/gltest/` tests it on Linux (`-DBGL_HOST`).

### OpenGL 3.3

```c
#include <GL/gl3.h>                 /* every GL 1.0 - 3.3 core function + contexts on windows */

bwin_open(&win, "Triangle", 640, 480);
bgl3_ctx_t* gl = bgl3_create(&win); /* GL 3.3 core, now current; NULL: bgl3_error() says why */
... glCreateShader, glGenVertexArrays, glBufferData ...
for (;;) {
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    bgl3_swap(gl);                  /* the frame shows in the window */
    /* BANANA_EV_RESIZE: bgl3_resize(gl) and glViewport */
}
```

with `USE_GL3 = 1` in the app's Makefile (64-bit; an app uses `USE_GL` or `USE_GL3`, not both). The renderer is Mesa's softpipe in `libOSMesa.so`, which `bgl3_create` loads from `BGL3_LIBRARY`, `/lib`, `/usr/lib`, or `/mnt/usb/lib`, `/mnt/virtio/lib`, `/mnt/sata/lib`: build it with `ports/mesa/build.sh` (it also makes `gl3test.bpk` and a test disk with both). `bgl3_create_offscreen(w, h)` + `bgl3_pixels()` draw without a window; `bgl3_proc("glSomethingARB")` gives the extensions Mesa has. Mesa draws on one processor core, slower than a GPU: fine for tools, simple games and tests.

### EGL and OpenGL ES

```c
#include <EGL/egl.h>          /* USE_EGL = 1 in the Makefile (64-bit) */
#include <GLES2/gl2.h>        /* or <GLES/gl.h> (1.1), <GLES3/gl3.h> (3.0) */

EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
eglInitialize(d, NULL, NULL);
EGLConfig cfg; EGLint n;
eglChooseConfig(d, (EGLint[]){ EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE }, &cfg, 1, &n);
EGLSurface s = eglCreateWindowSurface(d, cfg, (EGLNativeWindowType)&win, NULL);
EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, (EGLint[]){ EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE });
eglMakeCurrent(d, s, s, c);
/* ... draw ... */
eglSwapBuffers(d, s);
```

The same Mesa as `USE_GL3`; `eglBindAPI(EGL_OPENGL_API)` gives desktop OpenGL contexts instead (any version and profile Mesa has). `sdk/examples/glinfo` and `sdk/examples/gles2` show both.

### OpenCL

```c
#include <CL/cl.h>              /* USE_CL = 1 in the Makefile */

const char* src = "__kernel void scale(__global float* v, float k) { v[get_global_id(0)] *= k; }";
clGetPlatformIDs(1, &platform, NULL);
clGetDeviceIDs(platform, CL_DEVICE_TYPE_DEFAULT, 1, &device, NULL);
ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
q = clCreateCommandQueue(ctx, device, 0, &err);
prog = clCreateProgramWithSource(ctx, 1, &src, NULL, &err);
clBuildProgram(prog, 1, &device, NULL, NULL, NULL);   /* errors: CL_PROGRAM_BUILD_LOG */
k = clCreateKernel(prog, "scale", &err);
buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, n * 4, data, &err);
clSetKernelArg(k, 0, sizeof(cl_mem), &buf); clSetKernelArg(k, 1, sizeof(float), &two);
clEnqueueNDRangeKernel(q, k, 1, NULL, &n, NULL, 0, NULL, NULL);
clEnqueueReadBuffer(q, buf, CL_TRUE, 0, n * 4, data, 0, NULL, NULL);
```

The one device is the processor (`CL_DEVICE_TYPE_CPU` / `DEFAULT`; asking for a GPU gives `CL_DEVICE_NOT_FOUND`), *Banana OS CPU (N cores)*. Kernels are compiled when the program is built by the TinyCC built into the app (from `ports/tcc`: USE_CL apps are built in the Banana OS source tree), and work-groups run on every core; commands finish before their call returns (in-order queues, events complete at once, with profiling times). OpenCL C here: `__kernel`, `__global`/`__constant`/`__local`/`__private`, `get_global_id` and the other work-item functions, scalar types (`uchar`, `uint`, `size_t`, `half` as storage...), the math functions, `rsqrt`, `native_*`, `mad`, `fma`, `min`, `max`, `clamp`, `mix`, `step`, `smoothstep`, `sign`, `select`, `atomic_*`/`atom_*` on 32-bit ints, `printf`, `-D` options. `barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE)` pauses work-items until the group arrives; `__local` pointer arguments and block-scope arrays are shared within the group. `float`, `int` and `uint` vectors support 2, 3, 4, 8 and 16 lanes, constructors, arithmetic, comparison masks, component access, read swizzles and `vloadN`/`vstoreN`. These operations use scalar CPU code. Images and samplers remain unsupported. See [the SDK notes](../sdk/README.md#opencl) for the supported subset and limits. Run `selftest opencl` inside Banana OS; `tools/test_opencl.sh` runs the same kernel tests on Linux with the patched compiler.

### Crash-screen diagnostics

After building with `tools/dev.sh run`, run this inside Banana OS:

```sh
crashtest                 # safe, simulated full-screen preview; Enter/Esc returns
crashtest report          # simulated text report, also usable over SSH
```

For the real fatal paths, save your work first. Each command below stops
Banana OS and requires restarting the VM or computer:

```sh
crashtest panic           # explicit kernel check failure
crashtest ud2             # actual invalid-instruction CPU exception
crashtest pagefault       # actual unmapped-page access (64-bit kernel only)
```

The screen stays visible until you restart it; there is no automatic reboot
or disk dump. The full report is sent to serial COM1 (`tools/dev.sh run`
keeps it in `build/logs/serial-bios.log`, or the corresponding firmware log).
Keep the stop code, IP, fault address and the matching kernel binary. The
raw stack words are bounded to the known task stack, and are **not a call
trace**. `addr2line -e kernel64.bin -f -C <IP>` can identify symbols in that
binary; source lines require a build with debug information. Use
`kernel.bin` for the 32-bit kernel.

The preview lets other tasks run and restores the display when closed.
Actual faults bypass the desktop and allocator, notify other cores to stop,
and use bounded emergency serial output. Built-in Virtio and VMware graphics
also attempt a bounded display update without the normal graphics worker.
If a Virtio command was already in flight, serial remains the fallback. A nested fault falls back to a
minimal serial/VGA message. App faults and recoverable browser faults keep
their existing recovery paths. To check the merged browser recovery, open
`about:crash` in the browser: the page should report a crash while the OS
continues.

`python3 tools/test_crashscreen.py` exercises the same formatter and bitmap
renderer on the host, checks full-width addresses and framebuffer boundaries,
and writes PNG previews to `build/test/crashscreen/`.

### Hosting a repository (Linux)

`tools/repo/banana-repo` (Python 3 only) makes and serves repositories:

```sh
tools/repo/banana-repo init ~/myrepo                 # a repository and its signing key (~/.config/banana-repo/myrepo.key)
tools/repo/banana-repo add ~/myrepo game.bpk         # publish (a new version: add it again - Banana OS upgrades)
tools/repo/banana-repo list ~/myrepo
tools/repo/banana-repo key ~/myrepo https://pkgs.example.org     # the line for Banana OS (apt add-repo ...)
tools/repo/banana-repo serve ~/myrepo --port 8080    # a quick server (tests, a LAN)
tools/repo/banana-repo verify ~/myrepo               # signature and every file
```

**The package server** - `banana-pkgd` serves a repository and takes uploads:

```sh
tools/repo/banana-pkgd init ~/myrepo                 # the repository, its key, and an upload token (shown once)
tools/repo/banana-pkgd serve ~/myrepo --bind 0.0.0.0 --port 8080
tools/repo/banana-repo push http://server:8080 game.bpk --token bpk_...   # publish from any machine
tools/repo/banana-pkgd token add ~/myrepo alice      # more tokens (token list / token remove)
```

Its pages (`/`, `/package/<name>`, `/upload`) show the packages and the `apt add-repo` line; `GET /api/packages` is the index as JSON, `POST /api/upload` (a `.bpk`, `Authorization: Bearer <token>`) publishes, `DELETE /api/packages/<name>[/<version>]` takes one out. Uploads are checked (format, name, programs), a version already published with other contents is refused (give it a new version), only the tokens' SHA-256 is stored.

For a real server, `tools/repo/install-server.sh` (as root, on Debian / Ubuntu / Fedora / Arch) installs `banana-pkgd` as a systemd service and nginx in front of it, makes the repository in `/srv/banana-repo` owned by the user who publishes, prints the first upload token, and with `--domain` + `--email` gets an HTTPS certificate:

```sh
sudo tools/repo/install-server.sh --owner alice --domain pkgs.example.org --email alice@example.org
# or without nginx: banana-pkgd alone on a port
sudo tools/repo/install-server.sh --owner alice --no-nginx --port 8080
# --dry-run shows what it would do
```

The signing key never goes in the served folder - keep a copy somewhere safe: whoever has it can publish packages your users will trust. In QEMU's user network the host is `10.0.2.2` (`apt add-repo http://10.0.2.2:8080 key=...`).
