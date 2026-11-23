# Testing Banana OS

Banana OS checks itself in two places: **commands inside Banana OS** that test a
feature where it runs (a real PC, a VM), and **tools on the Linux build host**
that build the ISO, boot it in QEMU and drive those commands over the serial
console. Most of them came with pull request #30 (builds 141-150); `fsck` and
`vmtools` were added later (builds 151-152).

Every test prints one line per check - `ok`, `FAIL` (with the reason) or
`skip` (with why it cannot run here) - and a summary with the number failed.

## Inside Banana OS

### `selftest [part]`

Checks the system's foundations, from user mode to every processor core. What
has to run as an app is done by `systest` (`sdk/examples/systest`), installed
from the copy built into the kernel; each of its modes' exit code is the result.

```
selftest                 ring3, memory, limits, cores, malloc, AVX, syscall, disk, heap, OpenCL, offline browser,
                         POSIX and IPv6 (if enabled) checks
selftest ring3           an app runs in user mode (64-bit kernel; skipped on 32-bit)
selftest memory          RAM above 4 GiB: an app gets 64 MiB from there, every byte
                         holds, and it is given back when the app ends
                         (skipped without RAM above 4 GiB: MEM=8G tools/dev.sh run)
selftest limits          an app with 60 threads, an app with 60 files open,
                         24 apps at once
selftest clock           shared HPET / ACPI / IRQ clock, counter rollovers, elapsed
                         time against physical CMOS, and reads across migrating threads
selftest cores           threads calling malloc / free / a lock on every core, and
                         whether they spread over the cores (SMP=4 in QEMU)
selftest malloc          the apps' own heap (sdk/lib/malloc.c): random blocks with
                         every byte checked, realloc, aligned blocks; then 200000
                         malloc + free on one thread and on every core, timed against
                         the kernel's allocator (64-bit: must be at least 2x as fast)
selftest avx             2 threads per core fill YMM0-15 and spin until switched out
                         and moved between cores many times: every byte must be
                         back; then new threads must find their YMM upper halves
                         clean (skipped without AVX: CPU=max or KVM=1 in QEMU)
selftest syscall         the syscall / sysret entry: rbx and r12-r15 kept through a
                         call, a 9-argument call (4th argument in r10, 3 on the
                         stack) drawing a glyph checked pixel by pixel; then 200000
                         calls timed against int 0x80 (must be at least 1.1x as fast)
selftest disk            reads 32 MiB twice from the first SATA / virtio / NVMe disk
                         (read only): the same data both times; a task waking every
                         1 ms must keep running meanwhile (30%); how many waits the
                         disk's interrupt ended (QEMU: DISK=disk.img or -machine q35)
selftest heap            the kernel's heap checked through (every block, the list both
                         ways, every free block in its bin), then 100000 kmalloc +
                         kfree among 2000 live blocks with holes (under 2 us each)
selftest tlb             global kernel pages and PCIDs: a private address space whose
                         page is changed under it must never be seen as it was; switches
                         timed against forgetting everything; threads on every core
                         mapping / write-protecting / unmapping pages (fresh pages read 0)
selftest net             TCP over loopback: window scaling and SACK agreed (window over
                         64 KiB), 8 MiB checked byte for byte, then 2 MiB with 1 data
                         segment in 25 dropped (holes resent; still every byte right)
selftest utf8            a terminal's cells hold any character (é € Ω Ж ─ and one past
                         U+FFFF, a stray byte as U+FFFD); DejaVu Sans Mono draws what
                         the 8x8 font has not; VGA text mode's code page 437; UTF-8 to
                         UTF-16 and back (FAT32 / exFAT names, surrogate pairs)
selftest opencl          installs and runs the cltest example: build errors in the
                         log, vadd, a 2D Mandelbrot range, atomics, math built-ins,
                         __local arguments, barrier(), local arrays, float4 / int /
                         float8-16 vectors, refused group sizes
selftest boot            render the shared BIOS/UEFI menu into a guarded buffer;
                         highlight, countdown, clipping, color, stride and fallback checks;
                         incremental updates preserve unchanged options and header pixels
selftest ram             firmware RAM reporting: complete maps before/after legacy counts,
                         8/24/64 GiB totals, reserved holes, malformed input and live totals
selftest sleep           static ACPI sleep-package and wake-page checks; capability status;
                         does not suspend (also included in the default selftest)
selftest sleep cycle     actually enter S3, wake by RTC, check live user threads, FPU/stack
                         retention, the resumed timer and the wake trampoline's return
selftest mouse           deterministic relative-movement, ownership and release-state checks
selftest mouse demo      also launch the interactive mouse demo (desktop must be running);
                         click to capture, look around, Escape releases, Q exits
selftest browser         offline framework, CSS/layout, DOM observer and WebP tests
selftest browser swm     live SuperWebManager HTTPS, layout, hydration and image tests
selftest posix [part]    installs and runs the posixtest example: the POSIX layer for
                         ported programs - mmap: a 1 GiB reservation, mprotect, holes
                         mapped again, file mappings, system calls on mapped buffers,
                         threads faulting pages in on every core (posix mmap); pthreads:
                         mutexes, condition variables, __thread (64-bit), keys, once,
                         150 detached threads, a 4 MiB stack, semaphores, barriers,
                         read-write locks, errno per thread, clocks (posix threads)
                         sockets: TCP echo of 100 KB and UDP on the loopback, poll,
                         select, FIONREAD, SO_RCVTIMEO, ECONNREFUSED, getaddrinfo,
                         inet_pton/ntop, and an HTTP request to example.com if the
                         network reaches it (posix sockets); epoll and eventfd over
                         TCP sockets (one-shot, a wakeup from another thread), pipes,
                         socket pairs, dup2 over a socket (posix epoll)
                         signals (64-bit): SIGSEGV / SIGFPE / SIGILL from faults with
                         siginfo, a handler changing RIP/RAX, PROT_READ enforced, masks,
                         pthread_kill to a busy and a sleeping thread, sigaltstack, a
                         stack overflow caught (posix signals)
                         dlopen: a library needing another (DT_NEEDED), dlsym,
                         constructors, calls into the C library and back into the app,
                         dladdr, dl_iterate_phdr, dlerror, dlclose (posix dlopen)
selftest ipv6 [host [port]]
                         IPv6 off blocks TCP, UDP and resolution; address parsing and
                         formatting; localhost; bracketed HTTP URLs; UDP loopback
                         (odd lengths, bad checksums and truncation dropped, extension
                         headers, fragments); TCP connect / listen / accept, 8193
                         bytes both ways, orderly close; IPv4 unaffected. With a host
                         (and port, 80 by default): an AAAA lookup and a TCP
                         connection over the real network. IPv6 is switched on for
                         the test and put back as it was; saved settings are untouched.
```

Expected: `selftest: N passed, 0 failed`.

### `fstest [-k] <mount point>`

Checks a mounted FAT32 or exFAT drive end to end, in a folder of its own on it
(`fstest/`): make a folder, write a small file and append to it, write 300 KiB
and append 100 KiB, rename (to a Unicode name on exFAT), make a sub-folder and
move a file into it, 300 files so the folder outgrows its first cluster, delete
half of them, then **unmount and mount again** and read everything back from the
drive itself - the renamed file, every byte of the 400 KiB file, the 150 files
left and none of the deleted ones. Then the test folder is removed; `-k` keeps it to look at.

```
fstest /mnt/usb          a USB stick
fstest -k /mnt/virtio    a virtio disk, keeping /mnt/virtio/fstest
```

Works on real SD cards and sticks as well as in QEMU. `lsblk` and `mount` list
the mount points.

### `crashtest [mode]`

The kernel crash screen (stop code, registers, faulting address, stack words).

```
crashtest                (= preview) shows the screen safely; Enter or Esc returns
crashtest report         prints a sample report in the terminal
crashtest panic          a real kernel panic          - HALTS the system
crashtest ud2            an invalid-opcode exception  - HALTS the system
crashtest pagefault      a read of page zero (64-bit) - HALTS the system
```

Save your work before the last three; the full report also goes to COM1.

### `toggle6` / `ipv6`

Not tests themselves, but `selftest ipv6` uses them: `toggle6 [on|off]` (or
`ipv6 on|off`, Settings > Network) switches IPv6; `ipv6` shows the addresses.

### Later additions

- `fsck [-f] [-y]` (build 151): the filesystem tree, both metadata copies on the
  installed disk and the extents; `-f` reads every file back against its CRC-32C.
- `vmtools` (build 152): which hypervisor, and which guest tools answer.
- `help --check` (build 153): every shell command has a help page.

## On the build host (Linux)

Run these from the repository root. They need `nasm`, `gcc-multilib`,
`xorriso`, `mtools`, `python3` and `qemu-system-x86` (Debian:
`sudo apt install nasm gcc-multilib binutils xorriso mtools python3 qemu-system-x86 ovmf`).

### `tools/dev.sh`

```
tools/dev.sh build                    build the ISO; log in build/logs/build.log
tools/dev.sh run  [bios|uefi|32]      build, then boot in a QEMU window; the serial
                                      console in the terminal and build/logs/serial-<fw>.log
tools/dev.sh test [bios|uefi|32] [s]  build, boot headless for s seconds (60), fail on a PANIC
tools/dev.sh apps [bios|uefi|32]      build, boot headless and run selftest and the
                                      threads / cltest examples over the serial console
tools/dev.sh clean                    make clean
```

Environment: `MEM` (QEMU memory, default `512M`), `SMP` (cores, default `4`),
`NIC` (default `e1000`), `OVMF` (UEFI firmware file), `DISK` (an image attached
as a virtio disk, mounted at `/mnt/virtio`). For example `MEM=8G tools/dev.sh apps`
exercises the RAM above 4 GiB.

### `tools/serialtest.py`

Boots Banana OS in QEMU without a window and types commands at its serial
console, each once the prompt is back - on the 64-bit system that is the
login bash on ttyS0 (build 245: the kernel's commands are `/bin` programs
there), on the 32-bit one the built-in shell. A command fails if its output shows a
panic or a crashed app, or - written `command ## text` - lacks `text`.

```
tools/serialtest.py [--qemu BIN] [--log FILE] [--timeout S] [--boot-timeout S]
                    [--cmd "..."]... [--cmds-file FILE] [--window] [--keep] -- <qemu args>
```

Without `--cmd` / `--cmds-file` it runs the default app tests: `selftest`,
installs and runs `threads 4 200000` and `cltest`, then `free`. In a commands
file: one command a line, `#wait N` waits N seconds, other `#` lines are
comments. `--window` shows QEMU's screen, `--keep` leaves it running afterwards.
Exit code 0 when every command passed.

### `tools/test-exfat.sh [bios|uefi]`

The exFAT driver end to end (needs `exfatprogs`): makes an empty 64 MiB exFAT
image, boots Banana OS with it as a virtio disk and runs `mount`, `df` and
`fstest -k /mnt/virtio`; then, on Linux, `fsck.exfat` must find the volume clean
and `tools/exfat_check.py` checks what Banana OS left there - the tree, 150 files
in `fstest/many`, the Unicode-named file, and the 400 KiB file byte for byte.
`HEADLESS=1` runs without a window, `KEEP=1` keeps QEMU open first.

### `tools/exfat_check.py`

An exFAT reader of its own (not Banana OS's driver), used by test-exfat.sh:

```
tools/exfat_check.py IMAGE list              every file and folder, with sizes
tools/exfat_check.py IMAGE cat PATH          a file's contents
tools/exfat_check.py IMAGE cmp PATH LOCAL    exit 0 if the file equals LOCAL
```

### `tools/test_opencl.sh [x86_64|i686]`

The OpenCL runtime and its `cltest` checks, built and run on the host with the
same TinyCC vector patch and `setjmp` as Banana OS, once with 1 and once with 4
worker threads. Run `ports/tcc/build.sh` first (it prepares the TinyCC sources).

### `tools/test_crashscreen.py`

The crash screen's real formatter and renderer (`kernel/crash_draw.c`), compiled
for the host with `tools/tests/crashscreen.c`: 32- and 64-bit reports, fault
flags, long messages, clipped and padded framebuffers (nothing drawn outside
them). It writes preview PNGs and reports to `build/test/crashscreen/` without
booting anything; prints `PASS` when every check holds.

### `tools/test_malloc.py`

The apps' heap (`sdk/lib/malloc.c`), built with the SDK's own flags and run on
Linux under a stand-in for the system's call table (`vm_map` is `mmap`), by
`tools/tests/malloc.c`: 1.2 million random `malloc` / `calloc` / `realloc` /
`free` with every byte checked (without and with the thread cache), aligned
blocks from 16 B to 64 KiB alignment, kernel memory passed back to the kernel,
8 threads freeing each other's blocks, and nothing left in use at the end.

### `tools/test_kheap.py [BASE]`

The kernel's heap (`kernel/kheap.c`) on Linux, with stand-in headers
(`tools/tests/kheap_stub`) and its memory mapped below 4 GiB: 300000 random
`kmalloc` / `kfree` / `krealloc` / `kmalloc_aligned` (64 B - 2 MiB) with every
byte checked and `kheap_check()` every 5000; everything freed must merge back
into one block. Then the same fragmented workload on it and on the `kheap.c`
of `BASE` (default: build 193's, before the bins), timed.

### `tools/test_tcp.py [BASE]`

TCP (`net/tcp.c`) on Linux with stand-in headers (`tools/tests/tcp_stub`):
both ends of a connection in one process over a simulated link
(`tools/tests/tcp_sim.c`: a one-way delay, a bandwidth, a bottleneck queue,
random loss, a virtual clock). Downloads from 2 to 300 ms round trips, with
0 to 2% loss, checked byte for byte; window scaling and SACK must be agreed,
and at a 100 ms round trip it must be at least 4x the `tcp.c` of `BASE`
(default: build 195's, before window scaling and SACK).

## A full check before a release

```
tools/dev.sh test bios && tools/dev.sh test uefi && tools/dev.sh test 32
tools/dev.sh apps
MEM=8G tools/dev.sh apps
tools/test-exfat.sh
tools/test_opencl.sh x86_64 && tools/test_opencl.sh i686
python3 tools/test_crashscreen.py
python3 tools/test_malloc.py
python3 tools/test_kheap.py
python3 tools/test_tcp.py
```

Then inside Banana OS: `selftest`, `selftest ipv6 example.com`, `fsck -f` on an
installed disk, `fstest` on a real stick, and `crashtest` for the screen.

### Browser host checks

`python3 tools/test_browser.py` runs the offline suite; add `--32` for i686,
`--sanitize` for ASan/UBSan, or `--swm` for the live website checks and a PNG
preview. The live host checks need libcurl development files and pkg-config.
The OS commands use Banana OS networking; the host transport uses libcurl.
See [the fixture documentation](../web/fixtures/README.md) for coverage and limits.

### Mouse capture host checks

`python3 tools/test_mouse_capture.py` tests the real app-window event and capture
implementation, the SDK wrappers on older API tables, and shared input math.
Use `--32` for the i686 ABI or `--sanitize` for ASan/UBSan. This uses host
framebuffer/heap stubs; it does not validate physical devices or hypervisor I/O.

### Boot menu host checks

`python3 tools/test_bootmenu.py` runs the same renderer checks as `selftest boot`
and exports 640x480, 1024x768 and 1920x1080 PNG previews under `build/test/boot/`.
Use `--32` or `--sanitize` for the i686 ABI or ASan/UBSan. These renderer tests
need neither QEMU nor Pillow. `tools/build_boot_font.py` optionally regenerates
the committed atlas from `fonts/DejaVuSans.ttf` (Pillow required; license in
`fonts/LICENSE`). Normal OS builds consume the atlas directly.

Firmware smoke coverage for build 168: an isolated ISO booted under SeaBIOS and
64-bit OVMF, detected the graphical menu, paused the timer, moved selection with
the arrow keys and started the kernel with Enter. The 32-bit UEFI loader was
compiled, linked and checked for relocations and valid PE conversion; it was not
run under 32-bit UEFI firmware. Rebuild with `tools/dev.sh run` to see the menu;
`tools/dev.sh run uefi` selects the UEFI path.

Build 170 adds 22 incremental redraw checks (37 total). Selection, display
and countdown changes repaint only affected bands, composed in a 3 KiB
scanline buffer before copying to the framebuffer. Repeated unchanged input
writes no pixels. Tests compare incremental output with a full render at
640x480 and 1024x768 and check cache invalidation when the framebuffer or
menu layout changes. The BIOS linker still enforces the loader's 64 KiB
address-space limit.

The updated renderer passed ASan/UBSan and i686 host checks, both kernel
links, BIOS/UEFI32/UEFI64 loader builds, BIOS/UEFI64 QEMU menu navigation
and kernel handoff, and all 37 `selftest boot` checks in a 64-bit BIOS VM.

### RAM reporting

`python3 tools/test_bootmem.py --sanitize` (or `--32`) runs the same 16
memory-map regression checks as `selftest ram`. The in-OS command also checks
that the detected total covers the RAM managed by the allocators. Rebuild and
boot with `MEM=8G tools/dev.sh run`, then run `selftest ram` and `neofetch`;
Settings > About reads the same total. These are usable firmware RAM totals:
reserved memory and the PCI hole are excluded. A 32-bit kernel can report RAM
above 4 GiB even though it cannot allocate from it.

Build 169 validation: all 17 in-OS checks passed with 8 GiB assigned under
64-bit BIOS, 64-bit UEFI and 32-bit BIOS. Neofetch reported 8191 MB under
BIOS and 8185 MB under UEFI. The host checks also cover synthetic 24/64 GiB
maps; those larger guest sizes were not run on this host.

### ACPI S3 sleep (QEMU first)

The initial profile is QEMU's PC/PIIX4 machine, standard VGA, e1000 (or no
NIC), and optional virtio-blk storage. `sleep status` checks the firmware's
S3 package, FACS, usable waking-vector RAM, display and device support.
Unsupported configurations return a reason before entering sleep. This
does not provide generic physical-PC ACPI power management or an AML
interpreter; Q35/AHCI, other GPUs/NICs, USB controllers and audio-controller
resume need additional driver support.

Rebuild with `tools/dev.sh run`, then use `selftest sleep cycle`. The safe
`selftest sleep` runs 24 parser/memory-map checks plus the platform check
(or a skip when unavailable). The cycle adds live user threads that retain
their stack and floating-point values across S3, a stack marker in the
caller, resumed interrupts/monotonic time, and the actual firmware return:
30 checks on a supported guest. It uses a five-second RTC alarm, with the
clock's one-second granularity. Repeating the cycle checks reuse of CPU
stacks and device buffers. A default `selftest` never suspends the machine.

`sleep 5` is a simple timed sleep. `sleep` without a delay, Start's power
menu > Sleep, and Settings > About > Sleep wait for a host wake request.
In QEMU, Ctrl+Alt+2 opens the monitor: enter `system_wakeup`, then use
Ctrl+Alt+1 to return to the display. A host test can also issue QMP
`system_wakeup`. Installed filesystem data is synced before suspend;
monotonic timers pause while asleep and the wall clock is reread on wake.
This keeps the session in RAM, rather than saving a hibernation image.

`python3 tools/test_sleep.py --sanitize` or `--32` runs the same 24 static
checks on the host without suspending any machine. Firmware/device
validation requires a VM cycle. The wake protocol follows the
[ACPI sleeping/waking specification](https://uefi.org/specs/ACPI/6.6/16_Waking_and_Sleeping.html);
QEMU's wake path can reset devices, so driver restoration is required
([QEMU reset documentation](https://www.qemu.org/docs/master/devel/reset.html)).


## Shared monotonic clock (build 199)

Run `selftest clock` inside Banana OS. It prints the selected source and checks
12 conversion/rollover cases, a 1200 ms sleep (with 1 ms rounding tolerance),
elapsed time against the actual CMOS clock, and at least 80000 serialized user
clock reads on a four-core guest. Workers start together and remain runnable
for at least 250 ms, beginning with 50 ms of app computation sampled between
batches so timer interrupts can offload them. The previous 1 ms sleep every
256 reads could keep every burst on the boot core, making the multicore
coverage check fail on a healthy clock. One-core guests use two threads and
skip the multiple-core requirement. This command also runs as part of `selftest`.

HPET is preferred, then the ACPI PM timer. Both are shared hardware counters;
CPU TSC values and their advertised invariant/hypervisor flags are not used
for elapsed time. The 32-bit kernel reads a 64-bit HPET with a high/low/high
retry. Narrow HPET/PM counters are extended by the timer interrupt, with a
sequence counter protecting readers on other cores. They must be sampled at
least once per wrap (a 24-bit PM counter wraps in about 4.7 s). The last fallback
counts timer interrupts and is less precise if the machine delays delivery.
S3 rebases the selected source to its saved nanosecond epoch, excluding sleep.
The CMOS value, rather than the timezone-adjusted display clock, sets RTC alarms.

The host log showed Linux rejecting TSC after a 2470544928-cycle warp, and QEMU
TCG reproduced large jumps even though the guest cores agreed at startup.
The initial boot-time TSC synchronization patch did not fix the allocator's
reported 4166170 ms runtime. With the shared clock, the same four-thread kernel
allocator benchmark reported about 1252 ms; the app allocator reported 94 ms.
These figures describe this host and are not general performance guarantees.

The syscall entry benchmark now uses `cpu_count`, a cheap query, so hardware
clock MMIO does not hide entry overhead. The VirtualBox workaround still checks
real elapsed time to exit to the hypervisor periodically; raw TSC only limits
how often that check reads hardware, with an additional call-count bound.
The serial test runner rejects failing checks and failing final summaries even
when a nested suite prints `0 failed`. Numeric expectations have digit boundaries,
so `100% packet loss` cannot satisfy a `0% packet loss` assertion.

A resume regression from the upstream merge was also found: e1000 probing
cleared `present` and `ifname`, which made the network poller skip the NIC.
Resume now retains its registration metadata. `selftest sleep cycle` checks
that registration and reports 31 checks with a NIC present (30 without one).

Host checks:

```
python3 tools/test_clock.py --sanitize
python3 tools/test_clock.py --32
python3 tools/tests/test_serialtest.py
```

Reference layouts: [Intel HPET specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/software-developers-hpet-spec-1-0a.pdf)
and [ACPI hardware specification](https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html).


Validation on this host (scratch kernels and ISO):

| Guest/check | Result |
|---|---|
| Full 64-bit TCG selftest, 4 cores | 230 passed, 0 failed |
| Full 64-bit KVM selftest, 4 cores, host CPU/AVX | 232 passed, 0 failed |
| Full 32-bit selftest | 217 passed, 0 failed |
| UEFI KVM clock and core checks | 17 + 2 passed, 0 failed |
| HPET, HPET disabled (ACPI PM), ACPI/HPET disabled (IRQ fallback) | clock checks passed |
| Clock arithmetic, ASan/UBSan and i686 | 12 checks each, 0 failed |
| Serial runner regression tests | 8 passed |
| Clock multicore coverage regression, 512 MiB/4 cores | Old test failed 3/3 TCG runs; corrected test passed 5/5 TCG and 3/3 KVM runs, all on 4 cores; i686 passed |
| BIOS 64-bit/ACPI PM resume | Three real suspend/wake cycles; clock checks before/after, FAT I/O, ping and POSIX passed |
| BIOS 32-bit/HPET resume | Three real suspend/wake cycles; 31 cycle checks, FAT I/O, ping and POSIX passed |


## Networking, desktop and descriptor-sync regressions

Run these from the repository root on the Linux build host. Each runner in
the table accepts `--sanitize` for ASan/UBSan and `--32` for a 32-bit host
executable (requires gcc-multilib).

| Runner | Coverage |
|---|---|
| `python3 tools/test_dhcp_state.py` | Acquisition/retry, renewal, rebinding, expiry, NAK, hostname and clock wrap |
| `python3 tools/test_dns_tcp.py` | Truncated UDP replies and native DNS-over-TCP fallback |
| `python3 tools/test_fd_sync.py` | RAM-root sync, installed-volume flushing, descriptor lifetime, unsupported mounts, I/O failures and backup freeze |
| `python3 tools/test_screenshot.py` | PNG capture, allocation failures, paths and unique filenames |
| `python3 tools/test_notification.py` | Notification history, expiry, rate limiting, actions and drawing |
| `python3 tools/test_ntp.py` | Reply validation, request correlation, deadlines and NTP era rollover |
| `python3 tools/test_kcmd_output.py` | Complete output-ring draining, partial writes, EAGAIN, cancellation and broken pipes |
| `python3 tools/test_latency.py` | Boot-core filtering, IRQ nesting, histograms, timer wrapping and disabled collection |

After the screenshot runner, `python3 tools/check_screenshot_png.py`
independently decodes its output and checks chunk CRCs, dimensions and pixels.
`sh tools/ext4test/run.sh` exercises ENOSPC/reclaim, device errors and journal
cut/replay cases, then checks disposable filesystems with `e2fsck`; any failed
subtest stops the run.

### Live editor saves and exact-ISO boot checks

Inside a 64-bit live session, run:

```sh
pkg install /usr/share/extras/shelltest.bpk
apt install neovim
shelltest -e
```

This checks typing, saving and quitting Nano, Vim, vi and Neovim, plus Vim
syntax/help and Neovim Lua, tree-sitter, help and terminal support. It catches
the regression where live-root `fsync` returned ENOSYS and editors reported
save failures. A successful RAM-root sync does not add persistence after
shutdown; installed-root sync still flushes the backing volume.

`bash tools/ci-boot.sh bios` and `bash tools/ci-boot.sh uefi` boot the
already-built ISO without networking, run boot/RAM selftests and the editor
checks above, and retain serial logs and ISO provenance under `build/logs`.
The ordinary ISO workflow runs both against its exact build artifact.
`BASE_ISO=/path/to/previous.iso bash tools/ci-install-test.sh bios` (or
`uefi`) additionally checks installation, standalone disk boot, upgrade to
the current ISO and user-file persistence on disposable disk images.

Host shims and QEMU checks do not establish physical-controller power-loss,
GPU, network-adapter or hardware RTC behavior.

### Independent vi/ex regression

The vi package now contains Neatvi, not Vim's tiny build. Vim remains its own
package with its existing runtime. After installing vi, vim, nano and neovim,
run `shelltest -e`. In addition to Vim help/syntax and the other editors, it
checks vi line deletion/truncation, UTF-8 insertion/save, the bundled upstream
ISC notice, and ex line-mode save/exit. Removing vi must not remove vim.

The LibJS long-interaction regression measures elapsed guest time with
`performance.now()` rather than the emulated RTC-backed `Date.now()`. QEMU's
RTC and timer can drift separately under TCG; this must not turn a finite test
into an apparent hostile loop. The following infinite-loop test still requires
a CPU quota stop and a surviving independent renderer.

### AQC107 native Ethernet model

Run `python3 tools/test_aqc107.py --sanitize` and `--32`. Both compile the
actual frontend and shared hardware initialization, with modeled card-resident
firmware, registers and DMA completion. Tests cover B0/B1 firmware 2.x/3.x,
factory MAC, link changes, two simultaneous independent cards, ring wrap/full
queues, malformed RX/packet chains, firmware/reset/mailbox/semaphore failures,
unknown PCI/revision/firmware rejection and PCI power/MSI capability handling.
A failed card loses PCI bus mastering without stopping the other card.
No controller firmware is flashed. These are protocol/model tests, not physical
AQC107 verification or throughput measurements.

### BCM5720 native Ethernet model

See [BCM5720 scope and tests](bcm5720.md). Run
`python3 tools/test_bcm5720.py --sanitize` and `--32` for the actual reset,
PHY and DMA frontend with error injection, mutated RX completions and two
independent functions. Physical copper/firmware interoperability is pending.

### AX88179A vendor framing and USB startup

`python3 tools/test_network_drivers.py --sanitize` and `--32` now include
AX88179A/B native vendor mode, all TX frame lengths, link modes, three USB
speeds, aggregate fuzzing, exact silicon matching and failed RX submissions.
Failed initialization retains embedded URBs for all affected existing USB
Ethernet frontends. See [ASIX scope and source provenance](../third_party/asix/README.md).

### AR9280 native radio and station

`python3 tools/test_ar9280.py --sanitize` and `--32` exercise the actual
EEPROM/radio/calibration HAL, private PCI DMA frontend and WPA2/CCMP station.
See [AR9280 scope, source hashes and model coverage](../third_party/athn/README.md).
Physical RF and throughput remain unverified.
