# Intel graphics audit — 2026-11-01

## Corrections

- **Retired GEM handles and leaked pages.** When all buffer slots were occupied,
  allocation retired completed GPU work but kept its old slot cursor. Retirement
  can shrink the table or free an earlier slot. The next handle could lie beyond
  the recorded table, making it inaccessible even to owner cleanup. Allocation
  now searches again after retirement. Three submit/close/retire/allocate cycles
  verify handle usability, slot reuse and recovery of every backing page.
- **Overflowing relocation offsets.** Adding the patch width to a near-maximum
  64-bit offset could wrap, pass the bounds check and index outside the physical
  page array. The check now subtracts from the known buffer size. The old driver
  crashes with SIGSEGV in the hosted Sandy Bridge reproduction; the corrected
  driver returns EINVAL. A valid submission still works after rejection.
- **Context extension limits.** A chain longer than sixteen entries, including
  cycles, previously returned success with a partially configured context.
  Unfinished chains now return EINVAL and release the new context. A valid chain
  of sixteen entries remains supported.
- **Empty ioctl arguments.** Argument-free commands no longer pass a null
  pointer to a zero-length memcpy. The hosted compiler's UBSan diagnosed this;
  no native crash from that zero-length copy was observed.
- **Relocation overhead.** Once a buffer has been waited on, its remaining
  relocations reuse that idle state within the same submission. Already-correct
  presumed addresses still need no wait. Instrumenting i915_wait around the
  first three-relocation Sandy Bridge submission measured three calls before
  and one after, with both submissions succeeding. This is a call-count
  reduction, not a measured game frame-rate improvement.

## Validation

- Hosted kernel driver model: all nine device profiles pass, both normally and
  with UBSan configured to stop on the first error. Profiles cover Ironlake,
  Sandy Bridge, Ivy Bridge, Haswell, Broadwell, Skylake, Coffee Lake, Comet Lake
  and Ice Lake. Existing reset, context switching, softpin, isolation and ring
  wrap tests remain enabled.
- The pre-fix implementation fails the handle-retirement and context-chain
  regressions. The overflowing relocation independently crashes the old model
  process. No real GPU or user installation was used for these destructive inputs.
- Real Mesa Crocus and Iris libraries each submitted sixty frames on each of
  their four hosted device models: no OpenGL errors, no stray model writes,
  no resets, and no remaining GEM buffers at process teardown. The model does
  not rasterize full scenes, so this does not certify physical rendering.
- The display-driver regression previously failed to link (missing PCI revision
  and render-attachment stubs), and its equality macro evaluated hardware
  operations twice. The fixture now captures the Intel render handoff, evaluates
  each operation once, and runs in CI. All nine Intel and three Radeon display
  cases pass in both 32 and 64 bits, including cursor/backlight paths.
- The 64-bit BIOS live VM passes 314 system selftests, POSIX checks, noninteractive
  and interactive shell suites, LibJS DOM/isolation, Mesa glinfo and commands
  after toggling gpu3d. Its editor suite fails two assertions in one Vim session
  (write/quit and saved text); the rest completes. This reproduces the existing
  intermittent editor failure and is not recorded as a clean full-suite pass.
- The ISO builds both kernels with the existing Mesa and LibJS worker packages.
- The complete 32-bit live VM regression passes, including 289 system selftests.
- All five BIOS installation/update phases from build 212 pass on the final
  rebuilt ISO: initial install, disk boot, update, updated disk boot and another
  cold boot. Mesa checks pass, the old saved Bash ELF remains executable with
  the same hash, and installed executable hashes match the ISO. Terminal,
  network-priority and wallpaper preferences persist. The editor check also
  passes in this run; the separate live-VM failure above remains open.

## Compatibility and remaining limits

The 32-bit GPU fallback, GPU enable preference, driver selection and supported
device IDs are unchanged. This patch improves the existing Intel path; it does
not add a new GPU family or demonstrate a Minecraft speedup on physical Intel
hardware. Hardware validation is still required.

The preceding GitHub run (41247102427) failed on the known intermittent Vim
":qa!" exit in the standard BIOS upgrade check. These graphics corrections do
not claim to repair that separate editor issue.
