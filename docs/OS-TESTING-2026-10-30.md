# OS regression audit — 29–30 October 2026

The audit started from main `b62f4e21bb89c4b6a5818a5346b010b2506e049c`,
after PR #47. The official build 275 Mesa ISO used for baseline comparisons had
SHA-256 `381a57d974a3999605349db7105232e0eb4924400ec8f1cc11566544943bb975`.
Local validation builds include the fixes described here. GitHub Actions
publishes the distributable ISO after source commits; test images are not releases.

## Reproduced bugs and changes

### Settings cache lifetime and unnecessary writes

The settings writer retained a pointer to cached file contents while allocating
its replacement. Allocation can evict a cache-eligible file (4 KiB or larger).
An 8 KiB settings file and eviction injection reproduced a use-after-free in
the actual writer under AddressSanitizer before the fix.

The getter and writer now pin the file while they reference its contents.
Read and allocation failures preserve the existing file and release pins.
Unknown settings and the existing text formatting are preserved. If the final
replacement bytes are identical, the writer skips the file rewrite.
A sequence of 100 identical updates made 100 writes before and zero after;
explicit persistence still flushes other pending filesystem data.

Regression: `tools/test_config.py`, 675 checks each on native 32-bit and
64-bit ASan/UBSan. Source milestone: `2a8c859`.

### ELF executable validation

A wrapped program-header offset reproduced a heap-buffer-overflow in the
kernel loader's sizing pass. Other unchecked additions, dynamic-table bounds,
symbol-name scans and relocation sizes could similarly escape the file or
allocated image when loading malformed input.

Sizing and loading now share checked layout validation. Header accesses use
copies, avoiding alignment assumptions. Segment/file ranges, executable entry,
dynamic termination, symbol strides and relocation destinations are checked
with subtraction-based bounds. Symbol diagnostics have a bounded string copy.
PC-relative 32-bit relocations can legitimately occupy the last four image bytes.
The loader preserves the existing SDK/TinyCC relocation types and 64 MiB image
limit; invalid inputs return ENOEXEC, allocation failures ENOMEM.
Source milestone: `294c22b`.

Regression: `tools/test_elf_loader.py` uses the actual kernel implementation,
including both allocation paths, staged relocation addresses, malformed inputs,
allocation failures and 20,000 deterministic mutations per architecture.
ASan/UBSan passed 64,405 checks on x86_64 and 66,554 on i686. In the built tree,
42 x86_64 and 24 i686 SDK/application/port executable fixtures also loaded.

### Loopback without a network card

An official-build VM with `-nic none` could bind/listen but could not connect
to its own `127.0.0.1` server. Physical-interface guards stopped local packet
delivery and TCP timers. The full POSIX suite consequently failed or stalled.

Local packet draining and TCP timers now run without a NIC. Local IPv4 connect
and send bypass the physical device/DHCP checks, while external destinations
retain them. Physical receive drops packets claiming `127/8` or `::1` as source
or destination, keeping those addresses private to the software path.

Regression: `tools/test_loopback.py`, 586 checks on 64-bit ASan/UBSan and
native 32-bit: byte-accurate 100 KB TCP transfer, replies, timeouts, refused ports,
UDP, interface-switch preservation and physical loopback-spoof rejection.
In the fixed no-NIC guest, the socket/epoll subset passed 25 checks and the
full POSIX suite passed 123; the full system selftest passed 316.

### Stale test fixtures

The first host campaign ran 50 groups: 47 passed and three fixtures failed
before they could verify the current code. These were repaired rather than
counted as OS failures or silently skipped:

- File-descriptor test: accept newer API versions while retaining frozen ABI
  field-offset assertions (59 checks on both architectures).
- Mouse capture: provide the theme-color fixture dependency (37 checks each).
- Legacy TCP simulation: provide the current IPv6 loopback classifier stub;
  loss, window-scaling, SACK and byte-accurate download cases pass again.

The native 32-bit browser selftest also attempted eight image checks despite
its intentionally unavailable 64-bit isolated decoder. Seven failed; truncated
input happened to pass by refusal. It now explicitly checks that decoding is
refused with no published pixels. Hosted 32-bit decoder checks still run in
full via their decoder shim. GNU Bash/Zsh are 64-bit ports, so their guest
shell suites are only applicable to the 64-bit kernel.

The executable, settings, descriptor, mouse and offline-network regressions
are included in reusable GitHub Actions gates.

## Coverage and reproduction

Run from the repository root on a host with GCC multilib and Python:

```sh
python3 tools/test_config.py --sanitize
python3 tools/test_config.py --32
python3 tools/test_elf_loader.py --sanitize
python3 tools/test_elf_loader.py --32 --sanitize
python3 tools/test_program_load.py --sanitize
python3 tools/test_loopback.py --sanitize
python3 tools/test_loopback.py --32
python3 tools/test_fd_sync.py --sanitize
python3 tools/test_mouse_capture.py --sanitize
```

Real executable fixtures are loaded when their SDK/application build outputs
exist; generated malformed-input tests run even on a clean CI checkout.

The reusable command/network host gates cover these fixes. A separate
publication gate runs `tools/test_os_vm.py` in BIOS 32-bit, BIOS 64-bit
and UEFI 64-bit VMs with no physical or emulated NIC. It runs the full system
selftest and POSIX suite, plus Bash/Zsh/editor, LibJS DOM and isolation checks
on the supported 64-bit kernel. Logs include the ISO and source hashes.

The broader host campaign also exercised DHCP, DNS/TCP fallback, network
selection, USB Ethernet including the existing RTL8152 path, experimental
Wi-Fi/Ethernet models, WPA2 cryptography, driver SDK exports, ACPI/laptop/PD
models, ext4 journal/error/space handling, allocator stress, browser policy and
worker IPC, LibJS/offline page fixtures, SDL and nine Intel i915 model profiles.
The heap test ran 300,000 randomized operations with content checks and
recovered to one free block. Existing allocator improvements needed no changes.

## Guest and storage results

- BIOS x86_64, 4 GiB, no NIC: 316 system checks, 123 POSIX checks,
  Bash/Zsh scripts and interactive terminals, independent vi/Vim editors,
  LibJS DOM (30) and renderer isolation (26) passed. Mesa OpenGL/ES frames
  rendered through softpipe, as expected without an Intel/virtio GPU.
- BIOS i686, 512 MiB, no NIC: 289 system checks and 88 POSIX checks passed
  after correcting the unsupported-decoder test selection.
- UEFI x86_64, 1 GiB, no NIC: the same full system/POSIX, Bash/Zsh/editor,
  LibJS/isolation and Mesa checks passed through the permanent VM runner.
- Dual-NIC e1000/RTL8139 VM: DHCP, ping, priority selection, link loss/recovery,
  IPv6 selftests and two separate 30 MiB downloads passed. Each download was
  verified against SHA-256
  `8460a3ef74fc87a3ad129af4d6807471e84306b2e558eddbc3d3ec5426dc3143`.
- Driver SDK BIOS 32/64-bit: frozen ABI v1 and current external modules loaded;
  USB serial control and data were checked in both directions. Simulated AML
  reported battery 50%, brightness 60% and a 20 V / 3 A PD contract.
- BIOS and UEFI: install official build 275 to a disposable disk, standalone
  disk boot, update from the candidate ISO, boot again, save terminal/network
  preferences and cold boot. All five phases passed on each firmware.
  User data, Mesa/LibJS integration, terminal font/theme/shell and network
  priority survived; commands still ran after dropping disk caches.
- AHCI SATA installation: an expanded settings file (180 unknown padding keys),
  cache eviction, 25 GPU permission on/off cycles and external commands survived
  a cold boot. SHA-256 of Bash, glinfo and Terminal was identical before the
  cycles, after them and after reboot; the extra settings were preserved.
  This exercises disk/configuration behavior without a physical Intel GPU.
- Separate disposable virtio FAT32 and exFAT disks: each passed all 14 `fstest`
  checks (append, Unicode rename, 400 KiB byte comparison, 300 directory
  entries, deletion, unmount/remount and reread), followed by host `fsck -n`.
  FAT32 reported its intentionally unknown free-cluster summary without an
  error exit; exFAT reported clean.

The first general guest script called `fstest` without a mount point and
therefore printed usage. This was a harness mistake, not a filesystem result.
The separate mounted-volume checks above replaced it.

```sh
python3 tools/test_os_vm.py --32 --memory 512M
python3 tools/test_os_vm.py --mesa
python3 tools/test_os_vm.py --firmware uefi --mesa
python3 tools/test_network_vm.py --model multi
python3 tools/test_driver_sdk_vm.py --firmware bios
python3 tools/test_driver_sdk_vm.py --32
BASE_ISO=/path/to/build-275/Banana_OS-mesa.iso MESA_ISO=1 bash tools/ci-install-test.sh bios
BASE_ISO=/path/to/build-275/Banana_OS-mesa.iso MESA_ISO=1 bash tools/ci-install-test.sh uefi
```

The `--mesa` flag asserts that the tested ISO includes the optional runtime.
CI runs the same offline OS script on the standard ISO without that flag.
Installation and filesystem tests use disposable image files exclusively.

## Desktop and file operations follow-up

The desktop right-click menu no longer contains **Restore removed icons**.
Its action and unused translations were removed. Existing icon layout and
hidden-icon records are retained.

Three additional regressions were reproduced against the previous source:

- **Saved desktop coordinates:** `4294967295` became signed `-1` and indexed
  the desktop occupancy array before its start. UBSan stopped in the actual
  placement function. The parser now bounds decimal accumulation, rejects
  malformed/out-of-range coordinates and overlong records, and keeps CRLF
  compatibility. Placement independently rejects negative indices.
- **Incomplete recursive moves:** a folder with 33 children was reported moved
  successfully, but only 32 children were copied before the source was removed.
  The shared implementation now sizes the listing buffer to actual counts,
  covers all children within the listing limit and rejects failed/oversized
  or growing listings. Any copy failure prevents source deletion. Files also
  uses this shared implementation instead of its duplicate truncated copier.
- **Exhausted destination names:** when all 999 candidate names were taken,
  the previous helper returned the already-existing `note (999).txt`.
  It now returns failure with an empty output path on exhaustion or truncation;
  transfer, shortcut creation, trash restore and desktop item creation all
  handle that failure. Recursive copies also reject truncated child paths.

`tools/test_desktop_layout.py` passes 19,993 checks on both architectures
under ASan/UBSan, including invalid coordinates, duplicate cells, wide/tiny
screens, CRLF records and an invalid in-memory entry. `tools/test_fileops.py`
passes 11,591 checks on each architecture, including 0–64 child directories,
payload preservation, listing/copy/allocation failures and exhausted names.
Both run in the publication-gating host workflow.

```sh
python3 tools/test_desktop_layout.py --sanitize
python3 tools/test_desktop_layout.py --32 --sanitize
python3 tools/test_fileops.py --sanitize
python3 tools/test_fileops.py --32 --sanitize
```

In a live graphical VM, QMP keyboard input cut/pasted a folder containing
40 subdirectories from the OS filesystem to a separate FAT32 volume.
Every payload matched and the source directory was gone. After QEMU exited,
host `mtype` independently reread all 40 payloads and `fsck.fat -n` returned
success. A screenshot of the actual desktop menu confirmed that the removed
restore entry is absent; Arrange icons, Refresh and shortcut creation remain.

Copy failures can leave partial destination contents for inspection; this is
not an atomic filesystem transaction. The source is retained when recursive
copy reports failure.

## ELF checks after upgrading an installed system

The strengthened five-phase install/update test keeps a copy of the old
release's Bash executable in the user's home directory. It executes that
unchanged ELF before the update, after the update and after a further cold
boot, each following `drop_caches`. The preserved executable launches the
current `/bin/echo`, exercising old-app compatibility and new-program execution.

The test also captures SHA-256 of the candidate ISO's `/bin/bash`, `/bin/echo`
and `/apps/terminal/app-x86_64`. Both updated-disk boots must match these
exact hashes, while the preserved old executable must retain its original hash.
Missing or conflicting hash records fail the check; 26 host verifier checks
cover missing entries, changed payloads and conflicting duplicate records.

The local source candidate `816eaf2` passed upgrade from official build 212
in BIOS and UEFI with a Mesa/LibJS target; the same five-phase test also
passed with an emulated SATA/AHCI system disk. The older ELF's SHA-256 was
`3f6f8c10ac9f5bd499556ae903ebb5cdc47aa2c7562b95ec2ad2ee52c7b706e9`;
the target Bash differed, proving the compatibility probe used old code.
The usual browser, Mesa, terminal preference and user-data checks also passed.
No ELF breakage was reproduced in these upgrade paths.

These assertions are now part of `tools/ci-install-test.sh`, so the existing
standard/Mesa, BIOS/UEFI, build-212/build-218 publication matrix runs them.
`tools/check_update_elf.py` verifies the captured bytes rather than inferring
executable integrity merely from reaching a shell prompt.

## Notification placement

After completing the ELF upgrade checks, existing notification toasts were
moved from the top-right to the bottom-right. Drawing and hit testing share
the same rectangle, with a 12-pixel margin above the shared 28-pixel taskbar.
The desktop now uses the same taskbar-height constant as notification layout.
Frames too small to hold the card suppress the toast instead of covering the bar.

Hosted 32/64-bit notification regressions cover expiry, history, rate limits,
actions, the relocated close/body clicks, old-position and taskbar clicks,
resolution changes up to 3840×2160 and undersized frames. These checks now
also run in the main publication gate.

In an 800×600 graphical VM the card occupied (528,496) to (788,560), while
the taskbar began at y=572. A screenshot confirmed the 12-pixel gap; QMP
clicked the relocated close button and verified the card disappeared.
The following boot selftest passed all 37 checks.

## CC0 landscapes, wallpaper faults and Task Manager presentation

Three new presets are genuine 3200×1800 photographs: Alpine Peaks (Harald
Hetzner), Seeley Lake (Extemporalist), and Grand Tetons (Scott Horvath / USGS).
Each selected source explicitly states CC0 1.0. The asset README and manifest
record authors, original source/license URLs, original and packaged dimensions,
transformations, checksums and verification date. No picture was upscaled.

The three JPEGs total 2,890,520 bytes. Builds embed these compressed bytes
directly, and the existing isolated image worker decodes the selected preset
on x86_64. Only one decoded photograph is cached. Drawing re-entry during
worker waits uses the fallback thumbnail; a stale selection is not published.
The 32-bit kernel embeds only thumbnails. No decoder quota was increased and
no image decoder was moved into the kernel.

Two pre-existing faults were reproduced with the actual wallpaper implementation:

- A failed image read dereferenced a null file pointer.
- Decoding while reading an 8 KiB saved configuration could reclaim that
  configuration, producing an ASan heap-use-after-free in `wallpaper_load_config`.

The file is now pinned before read/decode and checked for valid loaded contents.
Configuration remains pinned while it is parsed. Invalid output dimensions
are rejected before allocation. Failure preserves the previous custom image.
Hosted ASan/UBSan cases pass on both architectures: I/O failures, configuration
cache eviction, output bounds, thumbnail fallback, full-resolution cache reuse
and recursive drawing during decode. Offline asset tests verify SHA-256, JPEG
dimensions, CC0 provenance fields, assembly blob sizes and the 3 MiB JPEG budget.

Windows 10/11 Task Manager layouts now use coherent light/charcoal palettes,
proportional anti-aliased text, aligned numeric columns and flat title controls.
The classic/Banana fixed-cell text paths remain in place. Memory-cell heat
uses the process's share of usable RAM rather than raw megabytes. Native QMP
tests exercised both styles' Processes and Performance pages and maximize,
restore and close controls; cropped screenshots are committed in
`assets/screenshots/task-manager-windows10.png` and `task-manager-windows11.png`.

Live VM serial traces confirmed all three photos decoded to **3200×1800**
through the user-space worker. Each background and both Task Manager styles
were inspected visually. Full 32/64-bit OS/POSIX, shell/editor, LibJS/isolation
and Mesa regressions passed for the supported architectures. The updated image
also passed the complete build-212 BIOS install/update/reboot path and exact
ELF hash assertions. A separate AHCI disk overlay was upgraded, configured
with Seeley Lake, synced and cold-booted; the saved preset was restored and
again decoded to 3200×1800, and external commands worked after cache eviction.
The local Mesa ISO measured 125,413,376 bytes (119.6 MiB).

```sh
python3 tools/test_landscapes.py
python3 tools/test_wallpaper.py --sanitize
python3 tools/test_wallpaper.py --32 --sanitize
# Only needed when changing the packaged photos; requires Pillow:
python3 tools/gen_landscape_data.py
```

## Filesystem mutations under memory pressure

The actual filesystem implementation is exercised with allocation-triggered
cache eviction and mocked volume I/O. Running the same harness against the
pre-fix source reproduced:

- A heap-use-after-free while copying an 8 KiB cached file.
- An invalid null-buffer read during a cross-volume move after cache eviction.
- A double-free while appending from a file's own cached contents.
- An invalid null-buffer write when volume I/O reclaimed the destination cache.
- A 4,294,967,295-byte read after buffered append length arithmetic wrapped.

Copy and cross-volume move now pin their source until the destination write
finishes. Write, offset-write, truncate and buffered append pin the destination
across allocation and volume callbacks, releasing pins on failures as well.
Inputs that alias the destination cache are tracked by offset across reallocations,
and overlapping writes use memmove. Append checks the maximum file size before
adding lengths. The intentional streaming transition for large downloads is
retained, including appends through fs_write_at; it does not discard an aliased
input buffer.

The host regression runs ASan/UBSan on both x86_64 and i686, using real fs.c,
not a reimplementation. Cases cover existing/new copy destinations, a
cross-volume move, overflow rejection, zero-filled gaps, overlapping and
relocated inputs, whole-file and offset-volume APIs, hard links, truncation, large-file
streaming, read/write errors, and allocation failures. These tests are now a
required host gate of the ISO workflow.

The final host run passed 20,870 checks per architecture (41,740 total).
Related configuration, file-operation and wallpaper regressions also passed,
as did the ext4 fault/recovery suite with e2fsck. Both kernels built successfully.
The full live-OS campaign passed on BIOS x86_64 (1 GiB) and i686 (512 MiB);
64-bit coverage includes Bash/Zsh, editors, browser/LibJS isolation, Mesa and
external execution after GPU setting changes. These GPU checks used QEMU's
software renderer and do not establish physical Intel acceleration.

The five-phase BIOS install/update/cold-boot test from the official build-212
ISO also passed. A frozen pre-update Bash executable still ran after cache
eviction and cold boot; installed Bash, echo and terminal SHA-256 values matched
the new ISO. User data, terminal preferences and network priority survived.
The reproductions and native logs are retained under
/home/banana/bos-tests/fs-audit-20261030 on the build host.

```sh
python3 tools/test_fs_mutation.py --sanitize
python3 tools/test_fs_mutation.py --32 --sanitize
```

## Clipboard history and persistence

The actual clipboard implementation reproduced two memory faults under
ASan/UBSan: a history entry growing from one byte to 512 bytes between the
length query and the snapshot caused an out-of-bounds read; cache eviction
during restoration of a 6,000-byte pinned entry caused a use-after-free.
The race test injects a history change at allocation time, exercising the
size-change interleaving deterministically rather than depending on timing.

Selection now verifies that the second snapshot fits its allocated buffer
before passing its length to clipboard_set. If the entry grew, selection
returns an error and preserves the current clipboard. Loading pins the
saved file through parsing/allocation and rejects failed disk reads.

Malformed pinned-history files previously accepted wrapped decimal lengths
(e.g. 4294967299 becoming 3), empty length fields, and records missing their
final newline. The parser now bounds each decimal step before multiplication
and requires complete record delimiters. The 64 KiB maximum remains supported.

A further reproduced behavior violated the pinned-history guarantee: copying
a new entry with all 25 slots pinned discarded the oldest pinned entry. It
now updates the current clipboard while preserving every pinned history item.
Duplicate selection and ordinary eviction of unpinned entries remain supported.

The new host suite passes 367 checks on each architecture (734 total) with
ASan/UBSan. It covers normal persistence, full pinned history, the size-change
race, cache eviction, missing files, null/error reads, allocation failure,
maximum-length records, overflow and truncated/malformed input. Spinlock IRQ
instructions are shimmed for the host; the parser/history code is the actual
kernel implementation. Both variants are required in the ISO host gate.

Both kernels built. Native x86_64 and i686 clipboard selftests passed all six
checks. A separate x86_64 GUI run wrote a two-record saved history, verified
its bytes, opened Win+V through QMP keyboard input and confirmed both entries
were restored as pinned and survived clip -c. Logs and baseline reproductions
are retained at /home/banana/bos-tests/clipboard-20261030 on the build host.

```sh
python3 tools/test_clipboard.py --sanitize
python3 tools/test_clipboard.py --32 --sanitize
```

## Scope of the evidence

Hosted driver models and QEMU test emulated behavior. They do not qualify
physical Intel GPU acceleration, Wi-Fi radio operation or USB-C power delivery.
No physical hardware was available for this audit. In particular, the owner's
persistent installed-disk terminal corruption after `gpu3d on` has not been
reproduced on the affected Intel PC; these fixes must not be presented as proof
that its cause is resolved.

The safe optimization in this audit is avoiding byte-identical settings writes.
No graphics quality, networking timeout, isolation quota or persistence guarantee
was reduced to improve benchmark numbers. Passing this campaign establishes
the listed coverage, not the absence of all OS bugs.
