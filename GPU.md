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

## Java and LWJGL

`java.bpk` carries the Intel Mesa runtime's `libOSMesa.so` in `/usr/java/lib`
(CI copies it from the `mesa` job), so Java apps that draw through LWJGL,
such as Minecraft, use the same drivers as the Mesa ISO.
Mesa tries Iris, then Crocus, then VirGL, then softpipe. On a 2nd
generation Core (Sandy Bridge, gen6) Crocus gives OpenGL 3.3 core and
compatibility, enough for Minecraft 1.7 to 1.20. If the Mesa runtime is also installed,
`/lib/libOSMesa.so` is found first; it is the same build.

Like every OpenGL app, Java uses a supported Intel GPU by default (`gpu3d off`
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
