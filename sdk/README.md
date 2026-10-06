# Banana OS SDK

Build apps for Banana OS on Linux, in C. One `make` compiles an app for both
CPUs Banana OS runs on (i686 and x86_64) and packs them into a single
`.bpk` package that installs on either kernel.

```
sdk/
├── include/        banana.h (windows, drawing, events, sound, network, ...),
│                   banana_api.h (the system call table / ABI), and a small
│                   C library: stdio.h stdlib.h string.h math.h ctype.h time.h unistd.h ...
├── lib/            crt0.c, libc.c, stdio.c, banana.c (compiled into every app)
├── banana.mk       build rules: include it from your app's Makefile
├── tools/bpkg      the packager (pack, list, extract, check)
└── examples/       hello, guess (console), paint, clock, mandel (desktop), tones (sound)
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
- Apps still run in ring 0 with the kernel and can reach all memory: a stray
  write through a wild (non-NULL) pointer can damage the system.
- **Resizable windows**: call `bwin_resizable(&win, min_w, min_h)` and handle
  `BANANA_EV_RESIZE` - `bwin_event()` has already updated `win.w`, `win.h`
  and `win.px` then; redraw everything (see `clock.c`, `mandel.c`). Users resize
  with the grip in the corner and maximize by double-clicking the title.

### Packaging details

`make` runs `tools/bpkg pack` with:

- `manifest` - key=value lines: `name`, `title`, `version`, `type`,
  `description`, `author` (generated from your Makefile)
- `app-i686`, `app-x86_64` - the program, as a static position-independent ELF
  (only relative relocations: `bpkg check` verifies it)
- your `DATA` files, installed next to the program in `/apps/<name>/`

```bash
python3 tools/bpkg list myapp.bpk
python3 tools/bpkg check myapp.bpk
```

The format: `"BPK1"`, a little-endian `uint32` file count, then per file
`{ char name[56]; uint32 offset; uint32 size; }`, then the data.

### The ABI

Banana OS loads the ELF, applies its relocations, and calls
`_banana_start(api, argc, argv)` (in `lib/crt0.c`) on a 1 MiB stack; `api`
points to the `banana_api_t` table in `include/banana_api.h`. The table only
ever grows at the end, and `api->size` tells an app how much of it the running
system has - so apps built today keep working on later Banana OS versions.
