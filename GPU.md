# Graphics work

Implementation order: diagnostics, damage tracking, asynchronous commands,
frame presentation, VirGL, LLVMpipe. Hardware 3D and LLVMpipe are separate
ports; the existing GL renderers run on the CPU.

## 1. Diagnostics

Run `selftest gpu` for offscreen pixel, clipping, stride, overflow and live
framebuffer checks. Run `gpubench` for CPU memory-copy bandwidth and pixel
verification. It does not measure GPU rendering speed or display latency.
`lsgpu` also reports presentation bytes, copy time, driver updates, command
counts and timeouts. Counters are cumulative since boot.

Hosted verification: `cc -O2 tools/tests/test_gpu_surface.c kernel/gpu_surface.c -o /tmp/gpu-surface && /tmp/gpu-surface`.
Both kernel architectures build. The seven in-OS checks passed with QEMU
standard VGA, virtio-vga and VMware SVGA. `gpubench` checked pixels and the
shell help registry reported no missing pages or duplicate commands.

## 2. Damage tracking

The desktop compares 32x32 tiles against its previous frame in RAM, joins
adjacent changed tiles and copies only changed runs. Comparisons are exact;
there is no hash-collision risk and no video-memory readback. The shadow
is invalidated when the framebuffer or backbuffer changes. Allocation
failure falls back to the full copy. Software cursor erasure is explicit.
Virtual GPUs coalesce up to 32 rectangles and flush them on demand; direct
console drawing and the splash mark their writes. The old unconditional
full-screen refresh every 100 ms is gone.

`selftest gpu` adds unchanged-frame, edge-tile and separated-change checks.
The hosted checks pass with address/undefined-behavior sanitizers. Both
kernels link. A virtio desktop passes all checks, changes resolution,
returns to the console and reports no GPU timeouts. In one run 5 MiB
changed out of 74 MiB considered; actual savings depend on the workload.

## 3. Asynchronous virtual GPU updates

Virtio screen updates and cursor movement use owned DMA command slots and
return after notifying the device. Completions are polled by the existing
GPU task. Control-queue replies validate descriptor IDs, response sizes
and fence IDs before reusing storage. Backpressure retains a full update
for the next tick. Mode/resource changes drain queued DMA first. A stalled
queue is reset with bounded waits; DMA memory is retained on a failed reset.
Boot, panic and resource-control operations still use bounded synchronous
commands. VMware frame updates also enqueue and kick without waiting for
host completion; its cursor-definition control path remains synchronous.

`selftest gpu` on virtio sends three full batches of 31 requests, checks
owned-buffer lifetimes, matching fences and descriptor reuse. These pass
in 64-bit and 32-bit QEMU guests, before and after starting the desktop.
The 64-bit test also changes resolution with pending screen work. VMware
passes its 64-update drain test. Live virtio counters showed 31 requests
in flight, zero protocol errors and zero timeouts. Actual device-hang/reset
recovery has not been fault-injected in QEMU.

Use `GPU=virtio tools/dev.sh run` or `GPU=vmware tools/dev.sh run` to select
the display adapter. Standard VGA remains the default.

## 4. Frame presentation

The standard VGA/DISPI driver presents desktop frames through two video
pages when the mode fits in VRAM. It draws damage into the invisible page,
waits at most 20 ms for retrace, switches the scanout offset, then updates
the old front page while it is invisible. This retains incremental copies
on both pages. Write-combining stores are fenced before switching. The
software pointer is drawn on the visible page after presentation.

Unsupported modes and insufficient VRAM use the normal copy path. Intel
and standard VGA retrace waits have a deadline; a missing retrace signal
cannot hang the desktop. These waits and DISPI offset changes do not
constitute a universal tear-free guarantee across hypervisors or hardware.
`lsgpu` reports flips, retrace waits and retrace timeouts.

At the console, `selftest gpu` flips the current image twice, verifies every
pixel and restores the original visible page. With the desktop running it
skips that intrusive live check while retaining the offscreen checks.
The live test, desktop and mode changes pass on 32-bit and 64-bit QEMU.
A 4 MiB VGA at 1024x768 correctly falls back to copying. BIOS/QEMU S3
sleep/resume also passes after enabling page flipping; resume invalidates
the damage shadow so both video pages are restored by the next full frame.

## 5. VirGL / Mesa virtual 3D

The optional virtio feature exposes owned application contexts and resources
through SDK API 22. The kernel validates ownership, request sizes, resource
boxes and backing offsets; handles are never guest-controlled host pointers.
Limits are 32 contexts, 256 resources, 64 MiB per resource, 128 MiB total
backing and 512 KiB per command submission. Application exit releases its
contexts and resources. `lsgpu` reports current usage and quarantine state.

Mesa's new Banana winsys submits real VirGL commands and uses fenced
upload/readback for OSMesa windows. Three-dimensional submissions currently
wait for completion; desktop transfers and cursor motion remain asynchronous.
This is an initial readback-based backend: there is no direct rendering to
compositor textures, dma-buf import, native fence FD or hardware video API.
The GL test validates basic shaders and pixels, not full OpenGL conformance
or compatibility with every game.

Build and run:

```sh
ports/mesa/build.sh
GPU=virgl DISK=ports/mesa/build/gl3disk.img MEM=2G KVM=1 tools/dev.sh run
```

Inside Banana OS run `selftest virgl`. It checks the kernel transport,
installs the bundled `gl3test` and requires the actual VirGL renderer before
checking GLSL 3.30 shaders, interpolated colors and framebuffer readback.
The host needs QEMU's VirGL display support and a working OpenGL backend.
`GL_DISPLAY` overrides the default `gtk,gl=on`; automated runs retain an
EGL headless backend rather than replacing GL with a non-GL display.

`GALLIUM_DRIVER=softpipe` selects CPU rendering. The library automatically
falls back to softpipe when VirGL is unavailable. Mesa's OSMesa frontend
now chooses a supported depth/stencil format and handles a missing screen
without dereferencing it. The SDK example is in `sdk/examples/gl3test`;
`gl3test window` displays its animated triangle.

Validation: both kernels build. Repeated 64-bit QEMU GL runs passed shader
and pixel checks on an AMD Radeon host, before and after starting the
desktop. All resources were returned after each test. Standard VGA passed
the same OpenGL tests using softpipe. A full target rebuild compiled all
819 objects, and every strong library import resolved against the SDK.

## 6. LLVMpipe CPU rendering

The optional Mesa build includes LLVMpipe and its LLVM 18.1.8 JIT runtime.
It renders on the processor and works without a 3D GPU. VirGL remains the
first automatic choice when available; this build uses LLVMpipe as the
software fallback. `GALLIUM_DRIVER=softpipe` still selects the simpler renderer.

```sh
JOBS=6 LLVMPIPE=1 ports/mesa/build.sh
DISK=ports/mesa/build-llvmpipe/gl3disk.img MEM=2G KVM=1 tools/dev.sh run
```

Inside the OS, run `selftest llvmpipe`. It explicitly requires LLVMpipe and
checks its C++ runtime, restartable UTF-8 state, shader compilation,
interpolated triangle pixels and framebuffer readback. The 256 MiB test disk
contains both shared libraries; they can also be installed together in `/lib`.
The default Mesa build stays available without the LLVM dependency.

See [the LLVM port notes](ports/llvm/README.md) for dependencies, reproducible
build steps and runtime limits. This is an initial OSMesa renderer, with the
same CPU readback presentation interface as the other Mesa backends. It does
not add native GPU drivers, Vulkan, or an OpenGL conformance certification.

Validation: the clean LLVM target build compiled all 1156 selected objects,
and the Mesa target compiled all 926 objects. Both shared libraries passed
strong-import audits. QEMU passed shader/pixel checks with LLVMpipe using
both a baseline CPU (128-bit vectors) and an AVX CPU (256-bit vectors),
including repeated 256x256 draws and readbacks. Repeated runs before and
after starting the desktop passed; the LLVM-enabled library also passed
VirGL rendering and returned all host resources. The complete system suite
passed 247 checks with zero failures. Hosted surface and exhaustive UTF-8
checks passed under address/undefined-behavior sanitizers.

## 7. Intel 3D (gen 5 - 7.5): crocus

`kernel/gpu_i915.c` drives the render engine of Intel HD Graphics of the 1st
to 4th generation Core (GTT, aliasing PPGTT, legacy ring, forcewake, Linux's
workarounds, hang reset); `kernel/gpu_gem.c` implements the part of Linux's
i915 interface Mesa's crocus uses. Apps get a supported GPU by default (`gpu3d off`
turns it off and is kept); `gpu3d test`
checks the engine on the real GPU. Mesa with crocus:

```sh
CROCUS=1 ports/mesa/build.sh                 # or LLVMPIPE=1 CROCUS=1
tools/tests/crocus_host/run.sh               # crocus on the GPU model
cc -O1 -no-pie -w -I kernel -I tools/tests tools/tests/test_i915.c kernel/gpu_i915.c kernel/gpu_gem.c -o /tmp/i915 && /tmp/i915
```

No hardware contexts: every batch re-emits the whole state (the Banana patch
in `crocus_batch.c`). No aperture (GTT) maps, no PRIME or flink, no BLT ring.

## 8. AMD Radeon HD 5000 - 7000 (TeraScale 2): the HD 7450

Experimental, off unless `gpu experimental on` (kept in the settings); written
without the card at hand. Everything is checked against a model first
(`tools/tests/radeon_kms.sh`) and touches the card only after the firmware's
own state matches what the driver expects.

**The display (done).** `kernel/gpu_radeon.c` takes the CRTC that scans out
the firmware's framebuffer and adds the hardware pointer and vsync. On
Evergreen and Northern Islands (DCE 4 / 5: Cedar ... Cayman, the HD 7450 is
a Caicos) it also sets **modes**:

- The video BIOS comes from the ACPI VFCT table (UEFI), the PCI expansion
  ROM (turned on for a moment if the firmware disabled it, as Linux's
  `ni_read_disabled_bios`), or the legacy copy at 0xC0000 (BIOS boot) - and
  must carry the AtomBIOS signature and this card's PCI ids.
- Its command tables run in Linux's AtomBIOS interpreter, vendored in
  `third_party/radeon` (MIT, notices in `/usr/share/licenses/radeon`), with
  every read bounded by the image's size. `kernel/radeon_atom.c` gives it
  the card's registers (MMIO, the I/O BAR, the memory controller's indirect
  ones) and reads what the BIOS describes: the display paths (connector,
  encoders, DDC line: `Object_Header`) and the pixel PLLs' limits
  (`FirmwareInfo`).
- The monitor's EDID is read through the BIOS's `ProcessI2cChannelTransaction`
  on each output's DDC line (`kernel/edid.c`: detailed, established and
  standard timings, the VESA DMT table); Settings > Screen offers its sizes
  that the output can clock (single-link DVI / HDMI 165 MHz, dual-link DVI
  330 MHz, VGA 400 MHz, and the BIOS's own maximum).
- `kernel/radeon_kms.c` sets one, as Linux's atombios_crtc.c / atombios_encoders.c:
  the output off; the CRTC locked, blanked and off; spread spectrum off; the
  pixel clock adjusted (`AdjustDisplayPll`), the PLL dividers computed
  (`radeon_compute_pll_avivo`'s search) and set (`SetPixelClock` 1.5 / 1.6:
  PPLL1 for even CRTCs, PPLL2 for odd); the timing (`SetCRTC_UsingDTDTiming`);
  the scanout surface (XRGB8888, linear, a framebuffer 64 MiB into video
  memory); the line buffer and DCE 5's DMIF buffers; then the encoder - a DIG
  engine and its UNIPHY transmitter for DVI / HDMI (single or dual link, DVI
  signalling), the DAC for VGA - and the CRTC back on.
- Not done: DisplayPort (link training - DP monitors keep the firmware's
  mode), HDMI audio and deep colour, spread spectrum, two monitors on one PLL.

`gpu radeon dump [folder]` saves the video BIOS, the display registers and
what the driver found (`/home/banana/radeon-report` by default): with them,
a problem on a real card can be replayed on the model.

**The 3D engine (started; Caicos, Turks, Barts, Cedar).** `gpu radeon 3d` brings up the 3D
engine of a Radeon HD 6450 / 7450 / 8450 (Caicos) - or of the HD 5450 /
6350 / 7350 (Cedar) that many cheap "HD 7450"s really are - as Linux's
`evergreen_startup` does, with Linux's own code for it
(`third_party/radeon/evergreen_gfx.c`, `r600_gfx.c`, `ni_gfx.c`, MIT) on
`kernel/radeon_gfx.c`:

- AMD's microcode, built in from linux-firmware (`firmware/amd`: CAICOS_pfp,
  CAICOS_me, BTC_rlc, CAICOS_mc; CEDAR_pfp, CEDAR_me, CEDAR_rlc; AMD's redistribution licence, shown in
  `/usr/share/licenses/radeon`). The memory controller's is loaded only on
  GDDR5 cards whose sequencer the video BIOS has not started.
- The GART: a 256 MiB window of system memory (the GTT) right after video
  memory in the memory controller's space, its page table 128 MiB into
  video memory (below that is the display's), every page the dummy one
  until bound (`rad_gart_bind`: valid, system, snooped, read / write).
- The engine's set-up (`evergreen_gpu_init`: pipes, SIMDs, backends, the
  tiling configuration), the run-list controller, the command processor's
  PFP and ME microcode, and a 64 KiB ring in video memory with a writeback
  page (read pointer, scratch registers, fences).
- Then it proves the engine runs: a scratch register set from the ring, a
  fence (`EVENT_WRITE_EOP`) written back, and an indirect buffer in system
  memory fetched through the GART that sets a scratch register. Any step
  failing stops the command processor, turns the GART off again and says
  which (lsgpu, `gpu radeon dump`).
- It measures the video memory: the engine writes 256 MiB, 512 MiB, 1 GiB...
  past a dword the CPU can see, and where the write shows up there the memory
  repeats. A BIOS that says 2 GiB on 1 GiB of chips (sold as 2 GB) is reported
  (lsgpu: "1024 MiB DDR3 (BIOS says 2048!)") and only the memory really there
  is used.
- `tools/tests/radeon_gfx.sh` runs all of it against a model command
  processor (ring, type 0 / 2 / 3 packets, writebacks, the page-table walk),
  checking the microcode uploaded word for word.

**OpenGL on it: Mesa's r600 (experimental, not yet run on the card).** With
`gpu experimental on` (the Radeon driving the screen), OpenGL apps - Java and
Minecraft too - draw on the Radeon:

- `kernel/radeon_drm.c` gives apps Linux's radeon interface on
  `/dev/dri/card0` (version 2.50): buffer objects in video memory (the
  part of it the CPU can see when the app maps the buffer, the rest
  otherwise) or in the GTT, mapped into the app; INFO about the chip; waits.
  TeraScale 2 has no per-process GPU address space, so every command stream
  goes through **Linux's own command checker** (`third_party/radeon/
  radeon_cs_check.c`, from evergreen_cs.c, MIT; the register whitelists
  generated from Linux's lists by `tools/radeon_regtable.py`): only the
  registers an app may set, every buffer address patched in from its
  relocation and checked against the buffer's size (surfaces, textures,
  vertex buffers, streamout). Then it runs as an indirect buffer on the
  3D ring, fenced per buffer. The 3D engine starts at the first app.
- Mesa's r600 driver and radeon winsys are in `libOSMesa.so` (`R600=1
  ports/mesa/build.sh`): `ports/mesa/banana/r600_screen.c` opens the
  device, libdrm's `radeon_surface.c` (MIT) lays out surfaces, and the
  winsys takes the address the kernel maps a buffer at; no DMA or video
  rings. r600's shader compiler is C++ with streams: libstdc++ is linked in
  as the LLVM port links it. OpenGL 4.5 core on Caicos / Cedar.
- The display's watermarks and priority marks (Linux's
  `evergreen_program_watermarks`, `third_party/radeon/evergreen_wm.c`) are
  programmed when the 3D engine starts and at each mode set, priority always
  on: without them the 3D engine's memory traffic starved the screen (it slid
  sideways and broke up into lines). The priority mark is computed in 64 bits
  (Linux's 20.12 product wraps at this card's 17 us latency).
- Order: r600 (only on a Radeon), then iris, crocus, VirGL, softpipe.
  `gpu3d off` sends apps back to the processor here too; `glinfo` shows the
  renderer ("AMD CAICOS").
- `tools/tests/radeon_host/run.sh` (in CI) runs the Linux build of r600 on
  the hosted kernel interface and the model card: an OpenGL 3.3 program
  (shaders, a texture, an FBO, 30 frames, glReadPixels) gets its context,
  and every one of its command streams is accepted by the checker and run,
  with no GPU access outside a buffer (the model does not draw, so the
  pixels are not checked). `tools/tests/radeon_gfx.sh drm` checks the
  interface itself, refusals included.

## Java and LWJGL

`java.bpk` carries the Mesa runtime's `libOSMesa.so` in `/usr/java/lib`
(CI copies it from the `mesa` job), so Java apps that draw through LWJGL,
such as Minecraft, use the same drivers as the Mesa ISO.
Mesa tries r600 (a Radeon HD 5450 - 7450 with `gpu experimental on`), then
Iris, then Crocus, then VirGL, then softpipe. On a 2nd
generation Core (Sandy Bridge, gen6) Crocus gives OpenGL 3.3 core and
compatibility, enough for Minecraft 1.7 to 1.20. If the Mesa runtime is also installed,
`/lib/libOSMesa.so` is found first; it is the same build.

On a GPU, a window's frames are read back by the GPU into two pixel buffers
in turn and shown a frame behind (`sdk/lib/gl3.c`), so the processor and
the GPU work at the same time; `BGL3_SYNC=1` copies each frame at the swap as
before (the processor waiting for the GPU).

Like every OpenGL app, Java uses a supported GPU by default (`gpu3d off`
switches it off). Start Java again after switching. Run `glinfo` to see the renderer
that apps will get. Minecraft also reports it in its log and F3 screen. Each
frame is drawn on the GPU and copied back into the window, so the copy costs
some speed at high resolutions.

## Mesa ISO in prereleases

Each prerelease ships two bootable BIOS/UEFI images: `Banana_OS.iso` (standard)
and `Banana_OS-mesa.iso` (the same OS plus the 64-bit Mesa runtime).
The Mesa image installs `/apps/mesa/libOSMesa.so` and the `glinfo` CLI command.
It adds no desktop or Start-menu launcher. OpenGL/EGL apps prefer this runtime
over Java's bundled copy (the same build). The package owns a /lib/libOSMesa.so
symlink, so existing applications work without recompilation; removal clears
that link only while it still points to this package. The runtime persists across installation
and updates like other built-in packages; it can be inspected in the package manager.

Both Intel Mesa drivers are included: Crocus for 1st–4th generation Core
(Ironlake through Haswell), Iris for 5th–10th generation (Broadwell through
Comet Lake and Ice Lake), including supported desktop and laptop PCI IDs.
Core processor generations differ from GPU generations (gen5 through gen11).
VirGL and softpipe remain available for virtual machines and software fallback.
The 32-bit kernel does not gain Mesa.
CI tests the Mesa profile with 1 GiB RAM (the standard ISO with 512 MiB).
At 512 MiB an installed Mesa system can exhaust memory when starting LibJS
after OpenGL use; allocate at least 1 GiB for this combined profile and more
for demanding pages.

Intel acceleration is experimental. Crocus has run on a real Sandy Bridge desktop
(HD Graphics 2000: Minecraft 1.20.4 through Java); the other generations are
tested against register/command-streamer models only. On hardware, inspect
`lsgpu` and run `gpu3d test`; apps use the GPU unless `gpu3d off` was given. Start a new GL app or run `glinfo` to see
the actual renderer. `gpu3d off` restores software rendering and is saved.
Unsupported hardware keeps the firmware display and software renderer;
this image does not add Vulkan or hardware video decoding.

Build the same variant locally:

```sh
CROCUS=1 IRIS=1 JOBS=2 sh ports/mesa/build.sh
make -C ports/mesa/runtime mesa-x86_64.bpk
make MESA_BPK="$PWD/ports/mesa/runtime/mesa-x86_64.bpk"
mv Banana_OS.iso Banana_OS-mesa.iso
```

Supply `LIBJS_WORKER_BPK=...` as usual when reusing a prebuilt LibJS renderer.
Omit `MESA_BPK` to return to the standard variant; the generated package list
is refreshed in both directions. CI compiles Mesa with Debian 13 and LLVM 17
(only the build-time Intel shader compiler uses LLVM). It checks both Intel
driver models, live ISO boot and software fallback, then BIOS/UEFI upgrades
from builds 212 and 218 and the installed runtime after reboot. Both ISOs
are published only after the boot/upgrade matrix passes.

## WebGL in the browser

A page's WebGL context runs in a GPU worker of its own (`apps/webgl`): the
page's isolated renderer records the calls (`web/webgl.c`, the stream in
`web/webgl_ops.h`) and the kernel's broker (`kernel/remote_page.c`, `WW_GL`)
hands them over; the worker plays them with OpenGL ES 2.0 through the SDK's
EGL (an off-screen pbuffer) and reads each shown frame back. The worker is a
web worker with a second restriction level (`kernel/app.c`,
`app_spawn_gpu_worker`): besides the web workers' calls it may open files
read-only, only those whose own path is in Mesa's folders or `/dev/dri/`,
and use the GPU calls (`gpu_call`, `drm_ioctl`, threads). The GPU objects it
makes are its own, as for any app. A page gets up to eight contexts; they
end with the page or when the worker fails (the context is then lost).

`getContext('webgl2')` asks the worker for OpenGL ES 3.0 (`WW_GL_INIT`'s
third argument; the worker answers the version it could make, and a page
gets no WebGL 2 context where Mesa has no ES 3.0). WebGL 2's calls are more
commands of the same stream: vertex arrays, 3D and array textures, immutable
storage, multiple draw buffers, `blitFramebuffer`, multisampled
renderbuffers, uniform buffers and blocks, unsigned and non-square
uniforms, integer attributes, `clearBuffer*`, samplers, queries, transform
feedback, buffer copies and `getBufferSubData`, instanced and range draws.
Sync objects are signalled at once (the stream runs in order).

A frame comes back through the kernel as RGBA: the answer buffer is sized
for the frame (`WW_GL_REPLY`), and a drawing buffer is at most `WW_GL_PIXELS`
(4 Mi pixels, 16 MiB a frame; a larger canvas gets a smaller buffer of the
same shape, as WebGL allows). The GPU worker looks for the next command every
millisecond for a second after each one (the kernel no longer sleeps 10 ms in
its wait), and the page's worker waits 1 ms between looks at a frame's
answer, so an animation is not held back by polling.

