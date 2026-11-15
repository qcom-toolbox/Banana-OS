# Banana OS SDK

Build apps for Banana OS on Linux, in C. One `make` compiles an app for both
CPUs Banana OS runs on (i686 and x86_64) and packs them into a single
`.bpk` package that installs on either kernel.

The SDK is under the BSD 2-Clause License (`sdk/LICENSE`), not the GPL that covers the rest of Banana OS: apps you build with it - which link its libraries in - may use any licence you like, open or closed.

```
sdk/
├── include/        banana.h (windows, drawing, events, sound, network, ...),
│                   banana_api.h (the system call table / ABI), and a small
│                   C library: stdio.h stdlib.h string.h math.h ctype.h time.h unistd.h ...
├── lib/            crt0.c, libc.c, stdio.c, banana.c (compiled into every app)
├── banana.mk       build rules: include it from your app's Makefile
├── tools/bpkg      the packager (pack, list, extract, check)
├── tools/banana-cc a gcc for Unix software's own build systems (banana-configure: autoconf)
├── tools/bananatheme  themes for the desktop: new, check, preview (PNG), pack
├── themes/         the theme format (README.md) and examples
└── examples/       hello, guess (console), paint, clock, mandel (desktop), tones (sound),
                    gl3test, glinfo, gles2 (OpenGL, EGL, OpenGL ES through Mesa)
```

## Requirements

Any x86_64 Linux with:

```bash
sudo apt install gcc gcc-multilib binutils make python3
```

(The same tools that build Banana OS itself.) Get the SDK from the Banana OS
source tree (`sdk/`), or as a tarball: `make sdk` at the top of the tree
writes `banana-sdk.tar.gz`.

## Your first app

```bash
mkdir myapp && cd myapp
```

`Makefile`:

```make
APP         = myapp            # a-z 0-9 - _ , 24 characters at most
TITLE       = My App           # shown in Apps on the desktop
VERSION     = 1.0
TYPE        = console          # console: runs in a terminal; gui: opens windows
DESCRIPTION = My first Banana OS app
SRCS        = main.c

include /path/to/banana-sdk/banana.mk
```

`main.c`:

```c
#include <stdio.h>
#include <banana.h>

int main(int argc, char** argv) {
    printf("Hello from %s on the %s kernel!\n", argv[0], banana_api()->arch);
    return 0;
}
```

```bash
make            # -> myapp.bpk
```

## Getting it onto Banana OS

Any of:

- **USB stick** (FAT32): copy `myapp.bpk` to it, plug it in - it shows up in
  `/mnt/usb`.
- **Download**: serve it from your Linux machine (`python3 -m http.server`) and
  open `http://<your-ip>:8000/myapp.bpk` in Banana OS's browser (files that are
  not web pages are saved to `~/Downloads`), or `wget` it.
- **NVMe / installed disk**: FAT32 partitions on an NVMe drive appear in
  `/mnt/nvme`.

Then install it:

- in a terminal: `pkg install /mnt/usb/myapp.bpk`, or
- in **Files**: double-click the `.bpk` (or right-click > *Install app*).

Or publish it in a **repository** - then everyone who has added it installs
it with `apt install myapp` or the **App Store**, and gets your new versions
with `apt upgrade` / *Update*:

```bash
../tools/repo/banana-repo init ~/myrepo          # once (from sdk/: the repo tools are in tools/repo)
../tools/repo/banana-repo add ~/myrepo myapp.bpk
../tools/repo/banana-repo serve ~/myrepo         # or a real server: tools/repo/install-server.sh
../tools/repo/banana-repo key ~/myrepo http://<your-ip>:8080   # what to type on Banana OS
```

(see *Packages: apt and the App Store* in the main README).

Run it by typing its name (`myapp`), with `pkg run myapp`, or from **Apps** on
the desktop. `pkg list`, `pkg info myapp` and `pkg remove myapp` do what they
say. Installed apps live in `/apps/<name>/` (kept on an installed disk).

## Writing apps

### What you have

- **C library**: `printf`/`snprintf`/`scanf`/`sscanf`, `FILE*` (`fopen`, `fgets`,
  `fread`, `fwrite`, `fseek`...), `malloc`/`free`/`realloc`/`calloc`, the
  `string.h` functions, `strtol` & co, `qsort`, `bsearch`, `rand`, `time`,
  `localtime`, `strftime`, `sleep`/`usleep`, `getcwd`/`chdir`.
- **Files**: paths work like in the shell (`/home/banana/...`, `~/notes.txt`,
  relative to the current folder). USB sticks and NVMe drives are regular
  folders (`/mnt/usb`, `/mnt/nvme`).
- **Terminal**: `banana_color()`, `banana_clear_screen()`, `banana_cursor()`,
  `banana_key()` (arrow keys come as `BANANA_KEY_UP` & co).
- **Windows** (`TYPE = gui`): `bwin_open()` gives you a pixel buffer
  (`0x00RRGGBB`), draw with `bwin_fill_rect`, `bwin_line`, `bwin_circle`,
  `bwin_text` (8x8 font), `bwin_text_scaled`, `bwin_blit`..., show it with
  `bwin_update()`, read the mouse / keyboard with `bwin_event()` or
  `bwin_wait_event()`. Handle `BANANA_EV_CLOSE`: the window's close button.
- **Sound**: `banana_play()` queues PCM (8/16-bit, mono/stereo, any rate);
  `banana_tone()` and `banana_beep()` for quick tones.
- **Network**: `banana_http_get()` downloads `http://` and `https://` URLs.
- **Clipboard**: `banana_copy()`, `banana_paste()`.
- **Threads** (Banana OS API 3+): `banana_thread(fn, arg)`, `banana_join(id)`, `banana_thread_id()`, `banana_thread_exit()`, `banana_cpus()`; mutexes (`banana_lock`/`banana_trylock`/`banana_unlock`), and with API 4 condition variables (`banana_cond_wait`/`timedwait`/`signal`/`broadcast`), semaphores (`banana_sem_*`) and `banana_wait_value()`/`banana_wake()`. The timer shares the CPU between an app's threads, so a busy thread never freezes the desktop; `exit()` or a crash in any thread ends the whole app. See the `threads` and `mandel` examples.
- **Time**: `banana_ticks()`, `banana_sleep()`, `banana_time()`, `banana_random()`.
- **Math**: `math.h` - see below.

See `include/banana.h` for the full list and `examples/` for working code:

| Example | Type | Shows |
|---|---|---|
| `hello` | console | printf, arguments, files, malloc |
| `guess` | console | scanf, colors, random numbers |
| `paint` | gui | a window, mouse drawing, buttons, saving a .bmp |
| `clock` | gui | math.h trigonometry, a resizable window that scales its drawing |
| `tones` | console | synthesizing sound and playing it |
| `mandel` | gui | double-precision math, a resizable window, mouse zoom |

Build them all with `make examples` at the top of the Banana OS tree; they are
also built into Banana OS itself, in `~/Examples` (`pkg install ~/Examples/paint.bpk`).

### Web views

Since API version 5 an app can embed the system browser's engine: a web
view loads a page (HTML, CSS and JavaScript, from `https://`, `file://` or a
path, or HTML you pass it) in the background and keeps a picture of it that
you draw wherever you want in your window.

```c
int v = bweb_open(win.w, win.h - 30);
bweb_load(v, "https://example.com");          /* or bweb_html(v, "<h1>Hi</h1>", NULL) */
for (;;) {
    banana_event_t ev;
    while (bwin_event(&win, &ev)) {
        if (ev.type == BANANA_EV_CLOSE) return 0;
        ev.y -= 30;                            /* view coordinates */
        bweb_event(v, &ev);                    /* clicks, typing, arrows / wheel scroll */
    }
    if (bweb_poll(v) & BANANA_WEB_DIRTY) { bweb_draw(v, &win, 0, 30); bwin_update(&win); }
    banana_sleep(10);
}
```

The page and the app can talk: the page calls `banana.postMessage("text")`
(objects are sent as JSON) and the app reads it with `bweb_message()`; the
app's `bweb_post(v, "text")` reaches the page as a `message` event
(`window.addEventListener("message", e => ... e.data ...)`). `bweb_eval()`
runs a script in the page and returns its value as text. `bweb_back`,
`bweb_forward`, `bweb_reload` and `bweb_info` (title, address, loading)
cover the rest. `examples/webview` is a small browser built this way, with a
start page that talks to its app. Check `bweb_available()` if your app
should also run on older systems.

### Fonts

Since API version 6 apps can write with the system's TrueType fonts
(DejaVu Sans and Sans Mono, regular and bold): anti-aliased, any size, UTF-8
text (accents, Greek, Cyrillic, symbols).

```c
bwin_font(&win, 20, 20, BANANA_FONT_SANS_BOLD, 24, "Héllo, wörld!", 0x202020);
int w = banana_font_width(BANANA_FONT_SANS, 14, "Centered");
bwin_font(&win, (win.w - w) / 2, 60, BANANA_FONT_SANS, 14, "Centered", 0x404040);
int ascent, descent, line_h;
banana_font_metrics(BANANA_FONT_MONO, 13, &ascent, &descent, &line_h);
```

`(x, y)` is the top-left of the line; `bwin_font` returns the x after the
text. On older systems these fall back to the 8x8 font.

### Players, keys, POSIX and ported libraries

Since API version 7 an app can drive a player: `banana_audio_queued_ms()`
(how much queued sound has not been heard - the clock of a video player),
`banana_audio_stop()` (drops it: pause, seek), `banana_volume()` (the
system volume) and `bwin_media_keys(&win)` (play / pause, next, previous
and stop come to this window even when another one has the focus). Keys
include `BANANA_KEY_F1`...`BANANA_KEY_F12` and the media keys
(`BANANA_KEY_PLAY`, `_STOP`, `_NEXT`, `_PREV`).

The C library also has the POSIX pieces ported code expects: `open`,
`lseek`, `fstat`, `stat`, `isatty`, `fdopen`, `posix_memalign` /
`aligned_alloc`, `<inttypes.h>`, `<fcntl.h>`, `<sys/stat.h>`, more of
`<math.h>` (`lrint`, `rint`, `copysign`, `erf`, ...) and the usual
`errno` codes. That is enough for FFmpeg: `ports/ffmpeg/build.sh` builds
it (libavformat, libavcodec, libswscale, libswresample) as static
libraries for both CPUs, and `apps/mediaplayer` and `apps/music` show how
an app links them (`LIBS_i686` / `LIBS_x86_64` in its Makefile). On the
32-bit kernel apps run in ring 0, where interrupts use the app's stack:
code must not keep data below the stack pointer (`-mno-red-zone` - the
build script patches the two FFmpeg assembly files that did).

Since API version 8 any HTTP request can be made: `banana_http_post(url,
form, ...)` (with `banana_url_encode()` for the form's values) and
`banana_http_request(method, url, body, len, content_type, ...)` - what
`apps/amethyst` uses to talk to a web API.

### Porting Unix software: banana-cc (autoconf, plain make)

Programs with their own build system build with `tools/banana-cc`, a C
compiler that takes gcc's options: it is gcc with this SDK's flags and
headers, and its link makes a program Banana OS runs (crt0, your objects,
the SDK's C library and libgcc, as a static PIE). It never looks at the
build machine's headers or libraries: `-lc`, `-lm`, `-lpthread`, `-ldl`,
`-lrt`, `-lutil`, `-lcrypt`, `-lresolv`, `-liconv` and `-lintl` are all
the SDK's C library, and a function your program defines wins over the
library's. The library is compiled once per CPU into `~/.cache/banana-cc`
(`BANANA_CC_CACHE`).

```bash
banana-cc -O2 -c foo.c && banana-cc -o foo foo.o -lm      # 64-bit
BANANA_ARCH=i686 banana-cc -o foo foo.c                   # or i686-banana-cc
banana-configure ./configure --prefix=/usr && make        # autoconf
```

`tools/banana-configure` runs a configure script cross-compiling for Banana
OS (CC=banana-cc, `--host` / `--build`), with `tools/banana-config.site`
answering what configure cannot test without running a program; give a
port's own answers in the environment (`ac_cv_...=yes`). No shared
libraries (`-shared` fails, so configure falls back to static ones).
`fork` and `exec` exist on the 64-bit kernel only (the 32-bit one runs
apps without address spaces): Unix programs are usually 64-bit.
`ports/port.sh` has what the ports share (download + sha256, configure
again from scratch when its options or the SDK change, `port_package`);
`ports/bash/build.sh` and `ports/coreutils/build.sh` are examples, packaged
with `bin=` (below).

What Unix programs find in the C library (API version 28, "Unix files"):
`stat` / `lstat` / `fstat` with modes, owners and times; `chmod`, `chown`,
`symlink`, `readlink`, `utimes` / `utimensat` (the modification time),
`unlink`, `rename`; directory descriptors - `open(dir, O_DIRECTORY)`,
`openat` and the other `*at` calls, `fchdir`, `fdopendir`, `dirfd`;
`realpath` through symlinks; `scandir`. Since API version 29: hard links
(`link`, `linkat`: not on FAT / exFAT sticks - `EPERM`), and `utimensat` /
`futimens` set the access and modification times apart (`UTIME_OMIT`). stdio is buffered (full for files and pipes, by line for
a terminal) with `fseeko`, `tmpfile`, `dprintf`, the `*_unlocked` calls and
`<stdio_ext.h>` (`__fpending`, `__fpurge`... - what gnulib uses). Also GNU
`getopt_long` (`<getopt.h>`), `<search.h>` (`tsearch`, `hsearch`,
`lsearch`), glibc's `strerror` texts and `program_invocation_name`.
API version 30: POSIX regular expressions (`<regex.h>`), timers
(`alarm`, `ualarm`, `setitimer`, `timer_create`: a signal from the
system), an atomic `sigsuspend` and `pause`, and the process list
(`banana_api_t`'s `proc_list`: what `ps` shows).

### Rust: banana-cargo

Rust programs build with `tools/banana-cargo`, cargo with Banana OS's
link: Rust's standard library for `x86_64-unknown-linux-musl`, linked by
`banana-cc` against this SDK's C library instead of musl, into one static
program (64-bit). `std` works as it does on Linux - files and folders,
threads and their locks and channels, time, environment variables,
`std::process`, `std::net` (TCP, UDP, name lookup). C code built by crates
(the `cc` crate) goes through `banana-cc` too.

    rustup target add x86_64-unknown-linux-musl     # once
    cd examples/rust-hello
    ../../tools/banana-cargo build --release        # target/x86_64-unknown-linux-musl/release/
    ../../tools/banana-cargo package --release      # rust-hello.bpk

On Banana OS, `pkg install rust-hello.bpk` and then `rust-hello` runs it
(the package's command has the binary's name). Any cargo command passes
through (`check`, `clippy`, `test --no-run`...). Programs abort on panic:
the message is printed and the program ends (`panic=abort`; unwinding
and backtraces are not available). Window, sound and the other Banana
APIs are C functions (`banana.h`): call them through `extern "C"`.

### Drivers

Hardware drivers are not apps: they run inside the kernel. The Driver Kit
in the separate [Banana OS Driver SDK](https://github.com/qcom-toolbox/Banana-OS/tree/main/driver-sdk)
builds them into `.bpk` packages of type `driver`. Get `banana-driver-sdk.tar.gz`
or run `make driver-sdk` in the source repository. Driver headers, examples and
build rules are not part of the application SDK archive.

### Rules of the road

- **Floating point works**: `float`, `double`, `math.h` (`sqrt`, `sin`, `cos`,
  `atan2`, `exp`, `log`, `pow`, `floor`, `fmod`...), `printf("%f %e %g")`,
  `scanf("%f")`, `strtod`/`atof`. Banana OS keeps every task's FPU/SSE
  registers apart (x86_64 apps use SSE, i686 apps the x87).
- **Be cooperative.** Banana OS multitasks cooperatively: a long computation
  should call `banana_yield()` (or sleep, or read events) now and then. Most
  library calls - printing, file I/O, window updates - already do.
- **Ctrl+C** ends a console app, unless it calls `banana_interrupted()` to
  handle it itself.
- **Everything is released** when the app exits: memory, open files, windows.
- **A crash ends only the app**: a bad pointer, a division by zero or an invalid
  instruction prints what happened (`segmentation fault at address ...`, with
  the offset in the app) and the system goes on. On the 64-bit kernel a NULL
  pointer access always faults (page 0 is unmapped).
- **The stack is 1 MiB.** On the 64-bit kernel unmapped guard pages sit under it
  and apps are built with `-fstack-clash-protection`, so an overflow is caught
  and reported ("stack overflow"); the 32-bit kernel has no paging and only
  catches small overflows. Use `malloc` for big buffers.
- On the 64-bit kernel apps run in ring 3, in an address space of their own:
  only the app's own memory is open to it, a wild pointer stops just the app,
  and every pointer handed to a `__banana` call is checked to be its memory
  (passing anything else stops the app). Already-built apps work unchanged.
  On the 32-bit kernel (no paging) apps still run in ring 0 and a stray write
  can damage the system.
- **Resizable windows**: call `bwin_resizable(&win, min_w, min_h)` and handle
  `BANANA_EV_RESIZE` - `bwin_event()` has already updated `win.w`, `win.h`
  and `win.px` then; redraw everything (see `clock.c`, `mandel.c`). Users resize
  with the grip in the corner and maximize by double-clicking the title.

### Packaging details

`make` runs `tools/bpkg pack` with:

- `manifest` - key=value lines: `name`, `title`, `version`, `type`,
  `description`, `author`, `category`, `depends` (generated from your Makefile)
- `app-i686`, `app-x86_64` - the program, as a static position-independent ELF
  (only relative relocations: `bpkg check` verifies it)
- your `DATA` files, installed next to the program in `/apps/<name>/`
- `bin=name,...` in the manifest (optional): commands in `/bin` - each
  `/bin/<name>` is a link to `/apps/<pkg>/<name>` when the package has a
  file of that name (a package of several programs: coreutils, diffutils),
  otherwise to the program; made when the package is installed and removed
  with it (bash: `bin=bash,sh`)
- `icon.png` - from `ICON = picture.png` in the Makefile: the app's picture
  on the desktop, the taskbar, the Start menu and in the App Store (square;
  its four corners' colour, when they agree, is see-through)

For repositories, three optional Makefile lines:

```make
CATEGORY = Games                  # the App Store's shelf
DEPENDS  = libfoo (>= 1.0), bar   # packages apt installs first (| for "one of")
ICON     = icon.png
```

Give every new release a higher `VERSION` - Banana OS compares versions the
way Debian does (`1.10` is newer than `1.9`, `2.0~beta` older than `2.0`).

```bash
python3 tools/bpkg list myapp.bpk
python3 tools/bpkg check myapp.bpk
```

The format: `"BPK1"`, a little-endian `uint32` file count, then per file
`{ char name[56]; uint32 offset; uint32 size; }`, then the data.

### Tools: any HTTP request, modifier keys, the wheel, installing apps

Since API version 10: `banana_http_fetch(&req, &resp)` makes any request
(`req.method`, `req.url`, `req.headers` - lines of `"Name: value\r\n"` -,
`req.body`, `req.content_type`, `req.timeout_ms`) and returns the answer
whatever its status (`resp.status`, `resp.data` - free() it). `banana_key_mods()`
tells which of Shift, Ctrl and Alt are held; `bwin_wheel(&win)` makes the
mouse wheel come as `BANANA_EV_WHEEL` (`ev.y` notches) instead of Up / Down
keys; `banana_pkg_install(path, msg, cap)` installs a `.bpk` and
`banana_app_run(name, argc, argv, err, cap)` starts an installed app. Banana
Code (`apps/code`) is built on them.

### OpenGL

Set `USE_GL = 1` in the Makefile and `#include <GL/bgl.h>`: the OpenGL 1.x
API (`<GL/gl.h>`, `<GL/glu.h>`) drawing into a window - `bgl_create(&win)`,
draw, `bgl_swap(ctx)`; after `BANANA_EV_RESIZE`, `bgl_resize(ctx)` and
`glViewport`. It renders in software on every processor core.
`examples/gears` is a complete app.

### OpenCL

Set `USE_CL = 1` and `#include <CL/cl.h>`: the OpenCL 1.2 host API, the
kernels compiled when the program is built (TinyCC inside the app - such
apps build in the Banana OS source tree, which has `ports/tcc`) and run on
every core. `examples/cltest` is a complete program. Run `selftest opencl`
inside Banana OS to install and run the matching built-in tests; the default
`selftest` includes them too. Linux checks: `tools/test_opencl.sh` and
`tools/test_opencl.sh i686` (the latter needs 32-bit development libraries).

`barrier(flags)` supports `CLK_LOCAL_MEM_FENCE`, `CLK_GLOBAL_MEM_FENCE` or
both. Each work-item preserves its private variables while paused; a group
can use repeated barriers and call barriers from helper functions. Different
groups run on different CPU workers. Common divergent barriers return
`CL_INVALID_OPERATION`; every work-item must reach the same barrier on each
iteration. This cannot diagnose arbitrary infinite loops or every divergent
helper call.

Local pointer arguments and block-scope `__local T name[N]` arrays are
shared by the group. Multidimensional arrays work. Use one array per
declaration, a literal `local`/`__local` qualifier and no initializer. Arrays
are lowered to pointers, so `sizeof(array)` is not supported for these local
arrays; use `sizeof(T) * N` instead. Each group has 32 KiB total local memory
and a program can contain at most 32 local array declarations. Collective
kernels use a 16 KiB private stack per work-item (including helper calls and
compiler temporaries). Allocation failures and local-memory exhaustion are
reported by the enqueue call.

Vectors: `floatN`, `intN`, `uintN` for N = 2, 3, 4, 8, 16, with matching host
`cl_floatN`, `cl_intN`, `cl_uintN` unions. Three-lane types have four-lane size;
`vload3`/`vstore3` still read/write packed triples. Constructors such as
`(float4)(1,2,3,4)` and `(float4)(2)`, arithmetic, scalar splats, comparisons
(all bits set for true), integer bitwise operations, single-component stores,
read swizzles (`.wzyx`, `.s0123`, `.lo`, `.hi`, `.even`, `.odd`) and vector
loads/stores work. Built-ins include vector `sqrt`, `sin`, `cos`, `fabs`,
`min`, `max`, `clamp`, `mad`, `mix`, `dot`, `length` and `normalize`.
Other built-ins retain their scalar behavior; multi-component swizzle stores,
other vector element types, images and samplers are not implemented. This
is an OpenCL C subset on the CPU, with vector operations lowered to scalar
lanes rather than GPU or SIMD execution.

### Packages (API version 11): what the App Store uses

`banana_repo_list(pkgs, max)` fills `banana_pkg_t` entries (name, title,
version, installed version, type, category, description, author, depends,
size, `flags`: `BANANA_PKG_INSTALLED`, `_UPGRADABLE`, `_AVAILABLE`, `_AUTO`) -
every package of the repositories and every installed one.
`banana_repo_update`, `banana_repo_install(name, ...)` (with its dependencies),
`banana_repo_remove`, `banana_repo_upgrade` do what `apt update / install /
remove / upgrade` do; they take a while, so call them from a thread
(`banana_thread`) and show `banana_repo_status(msg, cap)` - the percent, -1
once nothing runs. `banana_repo_icon(name, path, cap)` gives a picture file
for a package. `apps/store` (the App Store) is a complete example.

### Game controllers (API version 12)

`banana_gamepad(index, &pad)` gives controller `index` (0-3, in the order
they were plugged in): 1 and a `banana_gamepad_t` - the sticks (`lx`, `ly`,
`rx`, `ry`: -32767..32767, + is right / down), the triggers (`lt`, `rt`:
0..32767) and `buttons` (`BANANA_PAD_A`, `_B`, `_X`, `_Y`, `_LB`, `_RB`,
`_BACK`, `_START`, `_LS`, `_RS`, `_UP`, `_DOWN`, `_LEFT`, `_RIGHT`, `_GUIDE`,
`_LT`, `_RT`) - or 0 when there is none. Every controller comes in that
Xbox layout: USB HID gamepads and joysticks (PlayStation, Logitech, most PC
pads), Xbox 360 and Xbox One / Series controllers. Read it once a frame.

### Drag and drop (API version 13)

Files dragged from Files or the desktop and let go over an app's window
come as `BANANA_EV_DROP`: `ev.key` paths, `ev.x` / `ev.y` where (client
coordinates). `bwin_dropped(&win, i, path, cap)` gives path `i`.

### Keys held down (API version 14)

Key events are typing: a held key repeats, and only the last one pressed
does. Games ask instead, once a frame, which keys are down -
`banana_key_down(key)` is 1 while `key` is held, for as many keys at once
as the keyboard reports (moving with W and D while holding Space):

```c
if (banana_key_down('a') || banana_key_down(BANANA_KEY_LEFT))  x -= speed;
if (banana_key_down('d') || banana_key_down(BANANA_KEY_RIGHT)) x += speed;
if (banana_key_down(' ')) jump();
```

Characters go by their place on a US keyboard, so WASD stays WASD on
AZERTY and QWERTZ keyboards; also `'\n'`, `'\t'`, `'\b'`, 27 (Esc),
`BANANA_KEY_UP` ... `BANANA_KEY_DELETE`, `BANANA_KEY_F1` ... `F12`, and
`BANANA_KEY_SHIFT`, `_CTRL`, `_ALT` (either side). It is 0 while the app's
window (or terminal) is not the one in front. Keep reading the window's
events too. Web pages get the same as `keydown` / `keyup` with `event.code`.

The C library also has `setjmp` / `longjmp`, `gettimeofday` (`<sys/time.h>`),
`realpath`, `strtold` and `ldexpl`.

### mmap (API version 16)

`<sys/mman.h>` has `mmap`, `munmap`, `mprotect` (and `madvise`, `msync`,
`mlock` as no-ops), with Linux's flag values. On the 64-bit system an app
has 2 TiB of addresses of its own for mappings: `mmap` only reserves them,
and each 4 KiB page gets memory - zeroed - the first time it is touched, so
reserving a big range (a garbage-collected heap) costs only what is used.
`PROT_NONE` and `PROT_READ` are enforced: touching a page against its
protection is a bad access. `MAP_FIXED` replaces what was mapped there with
fresh pages. A file mapping is a private copy of the file (`MAP_SHARED`
writes are not written back). System calls take mapped buffers like any
other memory. The 32-bit system has no paging: `mmap` hands out zeroed
memory right away and protection is not enforced. `selftest posix` checks it.

### Threads: <pthread.h> (API version 17)

`<pthread.h>`, `<semaphore.h>` and `<sched.h>` are the POSIX ones: threads
with the stack size you ask for (mapped, a guard page under it), joinable or
detached (more than the 64 that can run at once over an app's life: a
detached thread's slot comes back), `pthread_getattr_np` (its stack),
mutexes (normal, recursive, error-checking, timed), condition variables on
`CLOCK_REALTIME` or `CLOCK_MONOTONIC`, read-write locks, once, keys with
destructors, spinlocks, barriers, unnamed semaphores. `__thread` variables
work on the 64-bit system (the kernel keeps each thread's FS base) in every
thread started by `pthread_create` or `banana_thread`, and in the main one;
the 32-bit system has no thread pointer - use keys there. `errno` is per
thread. Also new: `clock_gettime` / `nanosleep` / `clock_nanosleep`
(millisecond resolution), `sysconf` (`_SC_NPROCESSORS_ONLN`, `_SC_PHYS_PAGES`,
`_SC_PAGESIZE`...), `getpid`, `gettid`, `sched_getaffinity`, `gmtime_r`.
`selftest posix threads` checks them.

### Sockets: <sys/socket.h> (API version 18)

BSD sockets for TCP and UDP over IPv4, the loopback (127.0.0.1) included:
`socket`, `bind`, `listen`, `accept` / `accept4`, `connect`, `send` / `recv`
/ `sendto` / `recvfrom` / `sendmsg` / `recvmsg` (`MSG_PEEK`, `MSG_DONTWAIT`,
`MSG_WAITALL`), `shutdown`, `getsockopt` / `setsockopt` (`SO_RCVTIMEO`,
`SO_SNDTIMEO`, `SO_ERROR`, `SO_REUSEADDR`, `TCP_NODELAY`...), `getsockname` /
`getpeername`. A socket is a file descriptor: `read`, `write`, `close`,
`poll`, `select`, `ioctl(FIONREAD / FIONBIO)` and `fcntl(O_NONBLOCK)` work on
it. `<netdb.h>` resolves names through the system's DNS (`getaddrinfo`,
`gethostbyname`, `getnameinfo` with numbers), `<arpa/inet.h>` converts
addresses (IPv4 and IPv6 text). Not there yet: IPv6 sockets (`AF_INET6`
gives `EAFNOSUPPORT`, so Java uses IPv4), Unix-domain sockets, `socketpair`,
half-closing (`shutdown(SHUT_WR)` sends no FIN until `close`). The system
handles 64 TCP connections and 16 listening ports at once.
`selftest posix sockets` checks them.

### Signals: <signal.h> (API version 19)

`sigaction` / `signal`, `sigprocmask` / `pthread_sigmask`, `sigpending`,
`sigsuspend`, `kill` / `raise` / `pthread_kill`, `sigaltstack`, `sigsetjmp` /
`siglongjmp`, with Linux's numbers and flags (`SA_SIGINFO`, `SA_ONSTACK`,
`SA_NODEFER`, `SA_RESETHAND`). On the 64-bit system a CPU fault in an app's
code becomes a signal for its handler: a bad access `SIGSEGV` (`si_addr`,
`SEGV_MAPERR` / `SEGV_ACCERR`), a division by zero `SIGFPE`, an invalid
instruction `SIGILL`, also `SIGBUS` and `SIGTRAP`. The handler gets a
`ucontext_t` (`<ucontext.h>`, Linux's `REG_RIP`... order) whose registers it
may change: returning resumes from them. Signals sent to a thread arrive when
it next leaves a system call or the timer interrupts it, and they wake it from
a wait or a sleep. Ctrl+C is `SIGINT` once the app handles (or ignores) it.
The 32-bit system runs handlers for `raise` only. Not there: `sigwait`,
`SIGPIPE` (a send to a closed socket fails with `EPIPE`), `alarm`.
`selftest posix signals` checks it.

### Shared libraries: <dlfcn.h> (all apps)

`dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr` and `dl_iterate_phdr`
(`<link.h>`). Build a library as on Linux, `-fPIC` and `-shared`, for the
app's CPU (`sdk/examples/posixtest/Makefile` shows how). `dlopen` maps it,
loads what it needs (`DT_NEEDED`: next to it, in the current folder, `/lib`,
`/usr/lib`), binds it at once and runs its constructors. A library finds
symbols in itself, in the libraries opened `RTLD_GLOBAL`, and in the app:
every app built with the SDK carries a table of its own symbols and the C
library's (`sdk/tools/mkexports` makes it), so libraries call `malloc`,
`printf`, `pthread_create`... and the app's own functions. Not there:
`__thread` variables inside a library, symbol versions, lazy binding.
`selftest posix dlopen` checks it.

### Building on Banana OS: TinyCC

Banana Code has TinyCC inside (`ports/tcc/build.sh` fetches a fixed commit and
builds it): apps can be written, built and run on Banana OS itself. The same
headers are used there (`/apps/code/tcc/include`); they also work with TinyCC on
Linux (`ports/tcc/src/host/x86_64-tcc -nostdinc -I sdk/include ...`). TinyCC's
programs are linked with `-shared -Wl,-e,_banana_start` against
`libbanana.a` (this C library, built by TinyCC) and `libtcc1.a`; Banana OS
resolves their GOT / PLT relocations against their own symbols when it loads
them.

### The ABI

API 32 appends the internal web-worker IPC entry. SDK web views use isolated
64-bit renderers with origin/file permissions and quotas; browser compatibility
and multimedia limits are described in [browser isolation](../docs/browser-isolation.md).
Ordinary applications cannot attach to another process's worker channel.

Banana OS loads the ELF, applies its relocations, and calls
`_banana_start(api, argc, argv)` (in `lib/crt0.c`) on a 1 MiB stack; `api`
points to the `banana_api_t` table in `include/banana_api.h`. The table only
ever grows at the end, and `api->size` tells an app how much of it the running
system has - so apps built today keep working on later Banana OS versions.

## Relative mouse capture for games (API 20)

Call `bwin_capture_mouse(&win, 1)` after a click in the foreground window.
It returns 0 on success, -1 if denied or the OS is too old. While captured,
the desktop pointer is hidden and `BANANA_EV_MOUSE_REL` carries raw `ev.x` /
`ev.y` deltas (positive right/down). Consecutive motion events add their deltas.
Button events carry button 1/2/3 and a held mask (bits 0/1/2); wheel events
arrive even without opting into `bwin_wheel()`.

Handle `BANANA_EV_CAPTURE`: `ev.x` is 1 on acquisition, 0 on release.
Clear held game buttons and pause mouse-look when it becomes 0. The OS
releases capture on physical Escape (independent of an app polling keys),
focus loss, minimize, desktop switching, close and process exit. Reacquire
only on a new user click. Use `bwin_mouse_captured()` to resynchronize, or
`bwin_capture_mouse(&win, 0)` to release explicitly. These wrappers check
the table size before accessing API-20 fields, preserving older-system support.

VMMouse switches to relative reports during capture; VirtualBox guest absolute
integration is suspended. Physical relative mice work beyond desktop edges.
Absolute-only USB tablets still have the host's coordinate bounds: use a
relative mouse for unrestricted camera movement. A VM may require the host's
normal mouse grab before sending unbounded relative motion.

The embedded `mousetest` example shows mouse-look and held button/wheel state.
Run `selftest mouse` for automatic input-state checks and
`selftest mouse demo` from a desktop terminal for the interactive demo.
