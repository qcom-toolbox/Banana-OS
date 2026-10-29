# Experimental SDL2-style software-window subset

This is an opt-in, source-level compatibility experiment, **not a port or
redistribution of the SDL2 project**. Its small local header is not SDL2 ABI
compatible. Do not install it over official SDL headers or use it to claim
that an arbitrary SDL game is supported.

The adapter uses the existing Banana SDK. No kernel, SDK ABI, system library,
default Makefile, boot configuration or ISO contents are changed by the adapter.
The separate review-ISO workflow now consumes the audited LibJS worker from
its existing Debian build job, matching the release pipeline. Building it
creates static libraries; only an application explicitly linking one uses it.
The code is adapted from the Yhello-Banana SDL experiment and restricted to
the independently tested subset below. The original broader implementation's
audio, threading, OpenGL, controller and pretend-success paths are not imported.

## Implemented subset

- Video/timer/event initialization; unsupported subsystem flags fail explicitly.
- One software window using `bwin`, title changes and user-driven resizing.
- Opaque 32-bit XRGB surfaces, fill, clipped copy and overlapping self-blits.
  Destination clip rectangles are honored and retained until the window is resized.
  Surface allocations are limited to 64 MiB; unsupported formats/alpha fail.
- Close, resize, focus, ASCII key-press, mouse button/motion and wheel events.
  Right/middle button numbers and wheel direction are translated, not copied blindly.
  Wheel events are explicitly enabled on the native window.
- Non-consuming `SDL_PollEvent(NULL)`, bounded event waits and millisecond
  timers extending the SDK's 32-bit monotonic counter. At least one timer query
  per 49.7-day wrap is required; no extension can infer completely missed wraps.
  The accumulated 64-bit result is tested across an observed wrap.

## Deliberate limitations

No multi-window API, programmatic resize, fullscreen, hide/show, key-release or
physical scancode support, Unicode text input, relative capture, audio, threads,
controllers, renderer/texture API, alpha blending, image codecs or OpenGL.
The API is single-threaded. `SDL_MouseMotionEvent` relative/button-state fields
are not tracked and remain zero. Public structures expose only this subset;
linking binaries built against the upstream SDL headers is unsupported.
Chocolate Doom and other third-party applications are **not qualified** by this
contribution. A native SDL backend and real application qualification are future work.

## GPU compatibility

This subset always draws in software. It neither opens a DRM device nor loads
Mesa. Passing `SDL_WINDOW_OPENGL` to `SDL_CreateWindow` fails before a native
window is created; SDL OpenGL contexts and hardware rendering are unsupported.
Other applications keep their existing Mesa/i915 paths. A successful software
demo does not qualify physical Intel acceleration or fix the outstanding
installed-machine GPU/command problem.

## Build only when requested

From the repository root:

```sh
make -C ports/sdl2
make -C ports/sdl2/test
```

The outputs are `build/{i686,x86_64}/libSDL2.a` beneath this directory and
`ports/sdl2/test/sdl2_demo.bpk`. The demo is not automatically installed into
the live image or the application's system package repository.

Copy the package to a disk visible in Banana OS, then:

```sh
startx
pkg install /mnt/virtio/sdl2_demo.bpk
sdl2_demo --selftest
sdl2_demo
```

`--selftest` creates, draws and destroys 100 windows, reporting a failure if
creation/drawing fails. It does not measure all memory/resource leaks or test
physical input. The interactive mode redraws on user resize, prints ASCII key
presses and mouse clicks, and closes with Escape or the window close button.

## Regression checks

```sh
make -C ports/sdl2 test
```

This builds and runs the actual adapter against host libc plus deterministic
Banana window/time stubs: x86_64 with ASan/UBSan and i686 without sanitizers.
Checks cover rejected dimensions/formats/subsystems, extreme rectangles, clipping,
overlapping copies, borrowed window pixels, failed-window cleanup, event peeking,
button translation, resize updates, timeout bounds, the 32-bit tick boundary,
and 100 create/destroy cycles. These are not guest hardware tests.

The demo must additionally be installed and run under an official Banana ISO;
keep the ISO version, source commit, firmware and serial result with the PR.
No successful host compilation alone establishes game compatibility.

## Reference contracts

- [SDL2 surface creation](https://wiki.libsdl.org/SDL2/SDL_CreateRGBSurface)
- [SDL2 clipped blit and destination-rectangle behavior](https://wiki.libsdl.org/SDL2/SDL_BlitSurface)
- [SDL2 64-bit millisecond counter](https://wiki.libsdl.org/SDL2/SDL_GetTicks64)
