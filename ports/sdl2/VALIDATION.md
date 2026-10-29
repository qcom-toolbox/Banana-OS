# Validation record (2026-10-28)

SDK/source baseline: upstream `808c17e01f9d1d2166d159cc4c1031bf47a5480e`.
The adapter additions are under `ports/sdl2/` plus the path-filtered optional
host-check workflow. Review also repairs the existing review-ISO workflow to
consume the audited LibJS worker instead of building it with missing dependencies. Existing kernel, SDK, root Makefile, loader and default
image inputs are unchanged.

## Host and cross-build

- `make -C ports/sdl2 test`: PASS, actual adapter with host backend stubs;
  x86_64 ASan/UBSan and native i686 executions.
- `make -C ports/sdl2`: PASS, freestanding i686 and x86_64 static archives,
  with warnings treated as errors and explicit baseline ISA flags.
- `make -C ports/sdl2/test -j2`: PASS, linked dual-architecture `sdl2_demo.bpk`.

Host checks cover invalid dimensions and formats, 64 MiB allocation bounds,
extreme rectangles, clipped and overlapping copies, borrowed framebuffer
ownership, failed resize setup, event peeking/button mapping/resize refresh,
bounded event waits, an observed 32-bit timer wrap and 100 window lifecycles.
They do not simulate every allocation-failure path or physical input device.

## Guest smoke tests

Unmodified official release `build-212`, published 2026-10-23, target commit
`5bf92680a78367ed06fcc0d1f6d08e430c7c478c`:

- ISO SHA-256: `a71ee27998d0af3b9ca2d60d271cc8bc7f50dc8054a4e0f0c600df7b30333a23`.
- QEMU TCG, 512 MiB, two virtual cores, no networking or host-device passthrough.
- Disposable 64 MiB FAT disk containing only the test package, attached as VirtIO.
- BIOS/i686 (`qemu-system-i386 -cpu qemu32`): PASS; start desktop, install
  package, execute 100 create/draw/destroy cycles and return to shell. The
  stock `selftest boot` (37 checks) and `selftest ram` (17 checks) also passed.
- BIOS/x86_64: PASS; start desktop, install package, execute 100 cycles,
  report `sdl2-compat: PASS`, then execute `echo SDL-post-app-OK` successfully.
- UEFI/x86_64 with OVMF: PASS, same commands/results as BIOS/x86_64.

Guest commands:

```sh
startx
pkg install /mnt/virtio/sdl2_demo.bpk
sdl2_demo --selftest
echo SDL-post-app-OK
```

The serial driver waited for each expected result **and a subsequent shell
prompt**, rather than interpreting a partially printed prompt as completion.
Earlier attempts without `startx` correctly reported window creation failure.

## Limitations and baseline issue

The current SDK was used to build the app, but the tested guest kernel was the
published ISO above, not a custom kernel rebuilt from the SDK/source baseline.
No physical hardware, interactive resize/input session, complete SDL game,
audio, OpenGL or complete upstream SDL ABI qualification is claimed.

On the same unmodified ISO's x86_64 Bash console, `selftest boot` output is
truncated before its final result. This also reproduces in a control VM that
never installed or ran this adapter. Full x86_64 boot/RAM command qualification
is therefore **not claimed**; the successful SDL smoke and post-app shell tests
are distinct from that existing baseline issue. Original stock sources and
serial-test tools are not changed by this contribution.


## Maintainer review (2026-10-29)

- Fixed clipping to the public destination clip rectangle, including preservation
  across repeated window-surface queries.
- Enabled native wheel events and translated their direction to SDL's convention.
- Added explicit OpenGL-window refusal before native window creation.
- New regressions fail against the original adapter and pass on x86_64
  ASan/UBSan and native i686; both static archives and the demo package rebuild.
- Existing Crocus (Gen5/6/7/7.5) and Iris (Gen8/9/9.5/11) hosted Intel models
  render 30 frames per profile with no stray GPU writes or GPU resets.
  These are model tests, not physical Intel hardware qualification.
