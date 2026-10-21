# GPU work in progress (branch `gpu-experimental`)

This branch holds GPU and 3D work that is not finished. It is not merged into
`main`, and CI does not build it (the workflow runs only for `main`). Each part
below says what works, what does not, how to test it, and what to do next.

## LLVMpipe for Java / Minecraft (java.bpk)

**Goal:** Minecraft (Banana-MC-Launcher, which loads
`/usr/java/lib/libOSMesa.so` through `BGL3_LIBRARY`) renders with Mesa's
LLVMpipe on every core instead of softpipe, with no change to the launcher.

### What is on this branch

- **`ports/openjdk/package.sh`** puts LLVMpipe's `libOSMesa.so` and
  `libLLVM.so` into `/usr/java/lib`. The SDK loader finds `libLLVM.so` next to
  `libOSMesa.so` (DT_NEEDED, the library's own folder first).
  - It prefers Mesa's `build-llvmpipe-crocus/` (LLVMpipe and crocus for Intel
    GPUs), then `build-llvmpipe/`, then the softpipe `build/`.
  - Both libraries are stripped with `--strip-unneeded`: libLLVM goes from
    59 MB to 49 MB, and java.bpk from 32 MB to about 50 MB.
- **`.github/workflows/build-iso.yml`** (only takes effect once merged to main):
  - the java job builds `LLVMPIPE=1 CROCUS=1 ports/mesa/build.sh`, with cmake
    and libdrm-dev installed;
  - it caches LLVM's reference build and points LWJGL's build at that Mesa
    (`MESA_OUT`).
- **`gl3test bench [softpipe | llvmpipe] [frames]`** (`sdk/examples/gl3test`):
  a block world like a voxel game's, drawn offscreen at 854x480 with depth
  testing (3692 faces, 7384 triangles). It prints the renderer and the frame
  rate.
- **Kernel fix in `kernel/uspace.c`.** This was the cause of the Minecraft
  crashes; see below. It is a general bug and should go to main on its own.
  - `posixtest mmap` gains a check for it ("MAP_FIXED while another thread
    maps: the range is never given away").
  - `sdk/lib/malloc.c` now checks the result of the FIXED remap it uses to
    give an empty span's pages back.

### Results (QEMU, TCG with no KVM, `-cpu max`, 2 GB, 2 to 4 CPUs)

- **gl3test bench:**
  - softpipe (the old java.bpk library, or `bench softpipe`): **0.2 fps**;
  - llvmpipe (256-bit AVX2, every core): **2.1 to 2.3 fps**;
  - so about **11x faster**. On real hardware both are many times faster than
    under TCG.
- **llvmpipe picks up every core:** the SDK's `sched_getaffinity` and
  `sysconf` report `cpu_count()`, and app threads run on the APs.
  `selftest llvmpipe` and `gl3test llvmpipe` pass.
- **Minecraft 1.20.1 on LLVMpipe:**
  - **Before the kernel fix:** it crashed in 6 out of 6 runs, at a different
    place each time:
    - C2 compiler threads (`memset` into a heap block whose page was
      unmapped);
    - `lp_setup_bin_triangle` (a command block's count overwritten by another
      thread between a store and the next load);
    - `lp_rast_triangle_32_3` and `lp_rast_shade_quads_mask_sample` (a NULL
      shader variant pointer read from memory).
    - It also crashed with `LP_NUM_THREADS=0` and with
      `LP_NATIVE_VECTOR_WIDTH=128`, so the cause was neither rasterizer
      threads nor AVX.
    - softpipe ran 55 minutes with no crash.
  - **After the kernel fix:** it reached the title screen (the panorama
    rendered) with no crash. "Create New World" was still generating under TCG
    when the work was stopped, so **a world has not been entered on LLVMpipe
    yet**.
- Minecraft 1.8.9 (LWJGL 2) was downloaded but not run on LLVMpipe.

### The kernel bug that LLVMpipe exposed

`uvm_map(..., UVM_FIXED)` (mmap with MAP_FIXED) did three steps:

1. took the range out of the address space (`vma_cut`);
2. **dropped `vm_lock`** while it released the old pages;
3. added the range back.

During step 2 the range was a gap, and another thread's `mmap(NULL or hint)`
could be given it (`vm_find` returns the first free gap). The two mappings then
shared pages, and when one was unmapped or protected, the other's memory went
too. `uvm_unmap` had the same window: a mapping made meanwhile could lose the
pages it had just touched to the ongoing release.

The heap gives empty spans back with exactly such a FIXED map. LLVM's JIT makes
many small mappings. Together, Minecraft's memory was corrupted within minutes.
The old kernel fails the new posixtest check badly: 20278 of 35248 hinted
mappings were given addresses inside the range being remapped. With the fix:
0 of 18672.

**The fix:** `vma_replace()` first makes the range a no-access placeholder
VMA (prot 0: faults are not served), in one step under the lock. Then the
pages are released. Then the final protection is set, or for munmap the
placeholder is removed. `uvm_protect` and `uvm_unmap_frames` use the same
helper, and room is reserved first so a cut can never be left without its add.

### Known problems and notes

- **`export NAME=value` in the shell does not reach apps.** `GALLIUM_DRIVER`,
  `BGL3_LIBRARY` and `LP_NUM_THREADS` set that way are ignored by programs
  started from the shell. Pass them on the command line instead:
  - `gl3test bench softpipe` or `gl3test bench llvmpipe`;
  - `mcplay --env NAME=VALUE`, which comes from
    `tools/tests/minecraft/mcplay-env.patch` and must be applied to the
    launcher repository's `test/banana/mcplay.c`.
- **Shader variant failures in llvmpipe are silent**, and a NULL variant gets
  bound (`llvmpipe_update_fs` passes it to `lp_setup_set_fs_variant`). If a
  NULL-variant crash comes back with the fixed kernel, instrument
  `generate_variant` and `lp_setup.c` (stored->variant). The Mesa source the
  reference build compiles is the one its `compile_commands.json` names
  (`~/mesa/mesa-24.3.4` on the dev VM), not necessarily
  `ports/mesa/src/mesa-24.3.4`.
- **`rebuild.py`'s KEEP=1 can reuse an object after its source changed.**
  Delete `banana/objs/*<file>.c.o*` to force it to recompile.
- **`mcplay` replays the previous game log when it starts.** Search only after
  this run's "Minecraft is running" line.
- The launcher's README still says Java is the Zero interpreter. It has been
  HotSpot server (C1 + C2) since build 175.

### How to test (dev VM)

The scripts are in `tools/tests/minecraft/`. They expect `vmtest.py` and a
test disk in `~/bos-tests`. Keep guests at `-m 2G -smp 2` and wrap every heavy
job in `flock ~/heavy.lock`.

1. **Build Mesa with LLVMpipe and crocus:**

   ```
   PATH=$PATH:/usr/sbin JOBS=2 LLVMPIPE=1 CROCUS=1 ports/mesa/build.sh
   ```

2. **Make java.bpk** from a JDK and LWJGL. The CI java.bpk from a release can
   be unpacked with `unzip` into `x/`:

   ```
   JDK_OUT=x LWJGL_OUT=x MESA_OUT=ports/mesa/build-llvmpipe-crocus ports/openjdk/package.sh out
   ```

3. **Get the game files on Linux** with the launcher's own code:

   ```
   JAVA_HOME=<jdk17> OUT=~/mcdl sh test/run-host.sh 1.20.1
   ```

   For 1.8.9, also set `LWJGL_HOME=<dir with lwjgl2.jar>`.

4. **Make a FAT32 test disk** (2 GB) with:
   - `jdk/`, `lib/` (libOSMesa.so and libLLVM.so) and `lwjgl/` from
     `out/usr-java`;
   - `minecraft/` (the `.minecraft` from step 3);
   - `mcplay.bpk` (built from `test/banana` in the launcher repository);
   - `gl3test.bpk` and `posixtest.bpk`.

   The launcher finds the JDK at `/mnt/virtio/jdk` and the library at
   `lib/libOSMesa.so` next to it.

5. **Run the tests:**
   - `lprun.sh glbench.txt LOG` runs gl3test bench with each renderer;
   - `lprun.sh mmap-race.txt LOG` runs posixtest mmap (set `ISO=...` to test
     another build);
   - `lprun.sh mc-1.20.1.txt LOG -qmp unix:/tmp/lpq2.sock,server=on,wait=off -usb -device usb-tablet -machine pc,vmport=off`
     starts the game. Drive it with
     `qcl.py /tmp/lpq2.sock c:X,Y w:SECONDS shot:NAME`, for clicks, waits and
     screenshots (`/tmp/NAME.png`). The game's output is in the raw serial log
     (LOG), not in vmtest's text output.
   - At 800x600, the buttons are:
     - Continue (first start): 398,506;
     - Singleplayer: 398,298;
     - Create New World: 238,506.

### Next steps

1. **Merge the kernel fix to main by itself** (`kernel/uspace.c`,
   `sdk/lib/malloc.c`, the posixtest check), after the usual regression. It
   fixes memory corruption for any multithreaded app that maps memory.
2. **Rerun Minecraft 1.20.1 on LLVMpipe** with the fixed kernel through world
   creation, and walk around. Record F3's fps. Then run 1.8.9.
3. **If it holds, merge the java.bpk and CI parts**, and check that the first
   CI run builds LLVM within the job's time limit (LLVM's reference build is
   about an hour on a GitHub runner; it is cached afterwards).
4. **Update the launcher README:** LLVMpipe, and the JIT.
5. **Give shell `export` to apps**, and add `mcplay --env` to the launcher
   repository.

## Intel HD Graphics 3D (gen 5 - 7.5: 1st - 4th generation Core)

Goal: OpenGL on the GPU (Mesa's crocus) for Intel HD Graphics of the 1st to
4th generation Core - Ironlake, Sandy Bridge, Ivy Bridge, Haswell - so apps
such as Minecraft draw on the GPU instead of the processor. The user's test
machine is a 1st - 3rd generation Core.

### What there is, and its state

| Part | Where | State |
|---|---|---|
| Kernel render-engine driver | `kernel/gpu_i915.c`, `kernel/gpu_i915.h` | Done; tested on a GPU model only. GTT plus an aliasing PPGTT (gen 6+), a 128 KiB legacy render ring, seqnos in the status page, forcewake, Linux i915's workarounds and clock gating for gen 5 - 7.5, bit-6 swizzling as the firmware set it, a 2 s hang check and GT reset. Started on first use only. |
| i915 interface for apps | `kernel/gpu_gem.c`, `kernel/i915_uapi.h` | Done; model-tested. Linux's request numbers and structures: GEM create / mmap / close / wait / busy / tiling / set-domain / pread / pwrite, execbuffer2 (relocations, HANDLE_LUT, NO_RELOC, BATCH_FIRST, fence array), contexts and reset stats, syncobjs, REG_READ (timestamp), GETPARAM. Apps may use it only after `gpu3d on` (a setting, off by default). |
| Device pages in apps | `kernel/uspace.c`, `kernel/uvm.h` (`uvm_map_frames`, `uvm_unmap_frames`, P_DEV) | Done; `selftest memory` checks it in QEMU. |
| SDK | `sdk/lib/ufd.c`, `sdk/lib/posix.c`, `sdk/include/banana_api.h` (API v24 `drm_ioctl`) | Done: `open("/dev/dri/card0")` gives a descriptor whose ioctl() goes to the kernel. |
| Shell | `shell/devcmd.c`: `gpu3d [status | on | off | test]` | Done. `gpu3d test` runs 4 checks on the real GPU. |
| Mesa crocus | `ports/mesa/build.sh` (CROCUS=1), `ports/mesa/patch.py`, `ports/mesa/rebuild.py`, `ports/mesa/banana/crocus_screen.c`, `ports/mesa/banana/drm_shim.c`, `ports/mesa/include/` (libdrm's xf86drm.h, Linux type headers) | Compiles for Banana OS, alone and with LLVMpipe (`LLVMPIPE=1 CROCUS=1`), all imports resolve. Two Mesa files needed patching for the SDK's C library (in patch.py): `src/intel/common/intel_measure.c` (no mkfifoat/openat: its debug fifo is off) and `src/intel/compiler/elk/elk_eu_validate.c` (memmem). crocus re-emits the whole GPU state at every batch (Banana has no hardware contexts: patch in `crocus_batch.c`). Order of back ends: crocus (if `gpu3d on`), VirGL, then the processor. |
| Tests | `tools/tests/test_i915.c` + `tools/tests/i915/model.h` (the GPU model), `tools/tests/crocus_host/` (Linux crocus on the model) | Kernel test: 47 - 48 checks pass for gen 5, 6, 7, 7.5 (including a hung batch and its reset). crocus on the model: all four generations get a context (GL 2.1 / 3.3 / 4.2 / 4.6), run 10 frames of an OpenGL program, no GL error, no stray GPU write. The model does not execute 3D commands: pixels are not checked. |

**Not yet run on a real Intel GPU.** Nothing here has touched real hardware.

On main: builds 235 (`502b60e`, kernel/SDK/shell/tests) and 236 (`6f7df33`,
Mesa crocus). This branch carries the same work plus this document.

### Build and test (dev VM `banana@192.168.1.25`)

Rules: one heavy job at a time on the VM, through the shared lock, 2 cores,
QEMU at most `-m 2G -smp 2` (the VM ran out of memory once with parallel jobs
and froze the host PC).

```sh
cd ~/bos-i915            # (the worktree used for this work; any checkout works)
# the kernel driver against the GPU model (seconds, light)
cc -O1 -no-pie -w -I kernel -I tools/tests tools/tests/test_i915.c kernel/gpu_i915.c kernel/gpu_gem.c -o /tmp/i915 && /tmp/i915
# the OS
flock ~/heavy.lock nice make -j2 Banana_OS.iso
# Mesa with crocus (needs libdrm-dev on the VM; sfdisk is in /usr/sbin)
flock ~/heavy.lock nice env PATH=$PATH:/usr/sbin CROCUS=1 JOBS=2 ports/mesa/build.sh
# ... with LLVMpipe too (reuses an existing LLVM build)
flock ~/heavy.lock nice env PATH=$PATH:/usr/sbin LLVM_BUILD=$HOME/Banana-OS/ports/llvm/build LLVMPIPE=1 CROCUS=1 JOBS=2 ports/mesa/build.sh
# Linux crocus on the kernel interface and the GPU model (needs the CROCUS=1 reference build)
flock ~/heavy.lock nice tools/tests/crocus_host/run.sh 10
# java.bpk with that Mesa (/usr/java/lib/libOSMesa.so + libLLVM.so)
MESA_OUT=$HOME/bos-i915/ports/mesa/build-llvmpipe-crocus ports/openjdk/package.sh /some/out
# QEMU: commands and selftests (no Intel GPU there: gpu3d reports it, apps fall back)
cd ~/bos-tests && flock ~/heavy.lock python3 vmtest.py --log /tmp/g3d.log --script g3d.txt --timeout 600 -- \
    -cdrom ~/bos-i915/Banana_OS.iso -m 2G -smp 2 -boot d
```

On the real machine: `gpu3d test` (4 checks: a batch writing memory, the 3D
pipeline's post-sync write, 4000 submissions, the GPU clock), then
`gpu3d on` and an OpenGL app (`gl3test`, `glinfo`, Minecraft).

### Known problems and limits

- Never run on real hardware: the ring start-up, forcewake, the PPGTT and the
  workarounds follow Linux's i915 but are unverified. A wrong step may hang
  the GPU (the driver resets it after 2 s) or freeze the screen.
- No hardware contexts (MI_SET_CONTEXT): crocus re-emits all state every
  batch - correct but some CPU cost.
- No GTT (aperture) maps, no PRIME / flink, no BLT or video rings, no
  interrupts (waits poll the status page every 1 ms), no GPU frequency
  control (the GPU may stay at its lowest clock: slow).
- Ironlake (1st gen) gets OpenGL 2.1 only: Minecraft 1.16 or older.
- The LLVMpipe fall-back used to crash Minecraft: an mmap race in kernel/uspace.c,
  fixed on this branch (see the LLVMpipe section); uvm_unmap_frames had the
  same gap and was fixed the same way in that commit.
- Environment variables set with `export` do not reach apps on Banana OS:
  GALLIUM_DRIVER cannot pick the driver; `gpu3d on|off` does.

### Next steps, in order

1. On the user's machine: `gpu3d test`; fix what fails (log the register
   dump `i915:` lines print with `dmesg`/the serial log).
2. `gpu3d on`, then `glinfo` and `gl3test`: the first real frames; compare
   pixels with LLVMpipe.
3. GPU frequency (RPS): ask for the maximum clock on gen 6+ (GEN6_RPNSWREQ,
   RP0 from GEN6_RP_STATE_CAP) - likely the biggest speed-up.
4. Interrupts for waits (MI_USER_INTERRUPT) instead of 1 ms polling.
5. Minecraft on the GPU; then hardware contexts if state re-emission costs
   too much.
6. Later: gen 8 - 9 (Broadwell to Coffee Lake: Mesa's iris needs execlists,
   softpin and full PPGTT); AMD (amdgpu + radeonsi) is far bigger.
