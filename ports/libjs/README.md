# Ladybird LibJS port prototype

This experiment evaluates replacing BananaScript's JavaScript implementation
inside the restricted web worker. LibJS is the default desktop browser engine on
64-bit systems; the legacy renderer remains available with browser --legacy. The 64-bit ISO includes the isolated LibJS DOM worker as an
internal OS component, automatically installed and updated at boot. The standalone
probe stays optional. Bundling the engine does not run it in kernel space.

## Reference milestone

The standalone reference runs 17 semantic probes: arithmetic, Map/Set,
WeakMap identity, closures/default arguments, classes/super, Proxy/Reflect,
BigInt, Unicode regular expressions, JSON, optional chaining, descriptors,
Intl number formatting, Promise jobs, async/await and allocation/collection.
These are focused port regressions, not an ECMAScript conformance score or
proof that Google/CAPTCHA works.

The source baseline is Ladybird commit
[e6669482e693bda4f75cd8a90b2abd292c0f3774](https://github.com/LadybirdBrowser/ladybird/tree/e6669482e693bda4f75cd8a90b2abd292c0f3774)
(20 February 2026), before the Rust migration. Current Ladybird master uses
a Rust runtime; this C++ baseline is an experimental bootstrap, not a claim
to ship current Ladybird or its latest fixes.

The sources.json manifest pins the source archives and public suffix data by
SHA-256. Preparation verifies downloads before extracting and publishes complete
trees atomically. Existing unverified trees are rejected. Upstream copyright
and license files remain in the source trees.

The native build also runs 36 DOM session regressions: persistent declarations,
realm separation, window identity, forged handles, promises, detached callbacks
VM teardown/recreation, callback arguments collected between conversions,
Base64/UTF-8 codecs and host exception diagnostics. These use the actual adapter
with a minimal host; HTML/layout/event integration is tested separately in QEMU.

The reference uses ICU 76.1 as supplied by Debian 13 and tests the APIs used
by this snapshot against it. Upstream specifies ICU 78. The probe includes
Intl formatting; broader locale and Temporal compatibility remains to test.

## Build the reference on Debian 13

Install the host prerequisites:

    sudo apt-get install g++ make binutils nasm cmake ninja-build python3 git ca-certificates \
        pkg-config libicu-dev libssl-dev libtommath-dev libfast-float-dev

Then:

    JOBS=2 bash ports/libjs/build.sh

The script builds the pinned simdutf and simdjson dependencies, a minimal
selection of Ladybird libraries, and executes:

    ports/libjs/build/native/libjs-probe
    ports/libjs/build/native/libjs-probe --eval 'new Set([2]).has(2)'

LADYBIRD_SOURCE, LIBJS_PREFIX and LIBJS_NATIVE_BUILD can reuse a reference
source/prefix/build directory. LIBJS_DOWNLOAD_CACHE reuses verified archives.
Build outputs and downloaded sources are ignored by Git.

The prototype uses GCC 14 with C++23, RTTI and no C++ exceptions. Two upstream
enum-switch diagnostics are warnings for this compiler. The SDK stack-bounds
adaptation preserves the actual bounds returned by pthread_getattr_np instead
of applying Linux/musl's stack-growth adjustment.

## Optional Banana OS package

On Debian 13 with GCC 14 and ICU 76.1 development headers:

    JOBS=2 bash ports/libjs/build.sh target

This first runs the native reference, then recompiles the selected libraries
against SDK C runtime headers and builds two packages: apps/libjsprobe/libjsprobe-x86_64.bpk
(the standalone probes) and apps/libjsworker/libjsworker-x86_64.bpk (the experimental
DOM renderer).
Both are internal 64-bit workers, hidden from the application catalog. Only
libjsworker is embedded in the ISO. No host shared library or ELF interpreter is allowed in its image.
The source tree, generated files, build output and package are ignored by Git.

The private runtime bridge keeps genuine GCC RTTI and executes the C++ static
initializers required by GC type allocators. GNU locale compatibility and the
large-file stream aliases stay inside this optional program; other SDK apps
keep their existing runtime. ICU data is embedded; ICU file I/O and dynamic
loading are disabled. Original third-party notices and GCC runtime licensing
are included in the package.

Build the normal ISO locally for validation, then run the disposable-VM checks
(requires QEMU, mtools and OVMF for UEFI):

    python3 ports/libjs/test_vm.py --memory 512M --live
    python3 ports/libjs/test_vm.py --memory 512M --uefi

The helper installs the BPK from a temporary FAT disk, runs 28 prototype checks,
the existing 26 browser isolation checks, offline browser tests and memory
tests. The live option also fetches real GitHub releases. It leaves the serial
log in ports/libjs/build/banana/vm.serial unless --log selects another path.
Inside an existing disposable VM, install the package and run:

    selftest browser libjs

The optional workflow_dispatch SDK input in the LibJS reference workflow also
builds and audits the target package; ordinary pushes run the native reference.

### Measured first target milestone

QEMU TCG, two virtual CPUs, 512 MiB RAM, BIOS and UEFI (live GitHub checked on BIOS):

- All 17 semantic probes execute in the restricted user process.
- Bounded evaluation IPC, Set.has, syntax errors and JS exceptions pass.
- Direct file/command/thread syscalls and ungranted broker file/network
  access are denied.
- Endless JavaScript reaches the execution quota while another renderer
  continues to evaluate correctly.
- 28 prototype, 26 isolation, 73 offline browser, six memory and six live
  GitHub checks pass.
- Sampled peak charged memory: 51,172 KiB, including loaded image and committed
  runtime pages. This is a probe workload, not a web-page memory benchmark.
- ELF: 44,683,184 bytes; BPK about 45 MB; compressed package about 17 MB.
  These are prototype measurements with full ICU data, not optimized shipping
  sizes. The normal ISO does not contain this package.

## DOM integration milestone

The integrated libjsworker uses persistent LibJS realms with the existing C DOM,
HTML/CSS layout and image decoder. Native getters, setters and functions cross
a bounded process-local adapter. DOM wrappers and JavaScript callbacks keep
their identity; VM roots retain callbacks across events and timers. Detached
realm callbacks use invalidated guards rather than freed session pointers.
The trusted web API prelude also runs in LibJS, avoiding its duplicate parse
in the legacy DOM adapter. File and network operations still go through the
same authenticated kernel IPC broker and quotas.

On 64-bit Banana OS, select the included renderer directly with:

    browser --libjs https://github.com/qcom-toolbox/Banana-OS/releases

The desktop must be running. LibJS is selected at boot for desktop browsing;
SDK webviews keep their original worker. Switch subsequent browser loads to
the legacy engine with `browser --legacy`, or restore LibJS with
`browser --default` (equivalent to `browser --libjs` on 64-bit systems).
`browser --engine` reports the current selection without opening the desktop.
The selection resets to the platform default at reboot.

Loads, decoding and layout updates now allow 120 seconds of CPU and 300 seconds
of elapsed time. Interactions/evaluation allow 10 seconds of CPU and 30 seconds
elapsed; initialization allows 30/90 seconds. Each worker can commit up to
512 MiB with a 1 GiB shared browser pool, further bounded by half the available
kernel heap. A machine with less RAM cannot consume those maxima. The 500 ms
per-second CPU throttle and IPC/origin restrictions remain active. CPU and
wall-time terminations show distinct errors; crashes are not labelled as proven
quota failures.

Every ISO workflow builds and audits LibJS before embedding the renderer in
both 64-bit ISO variants. No separate LibJS download or BPK installation is
needed, and the ISO prerelease no longer attaches standalone LibJS packages,
notices or checksum files.

Developer builds remain available in the run's `experimental-libjs-packages`
CI artifact:

- `libjsworker-x86_64.bpk`: the isolated browser renderer included in the ISO.
- `libjsprobe-x86_64.bpk`: optional standalone JavaScript/containment probes;
  this diagnostic application is not embedded in the ISO.
- `LIBJS-THIRD-PARTY-NOTICES.txt` and `LIBJS-SHA256SUMS`: licenses and checksums
  for the internal artifact.

To test a separate package build in a disposable VM, download the CI artifact,
unzip it and install its raw BPK explicitly:

    pkg install /home/banana/Downloads/libjsworker-x86_64.bpk
    browser --libjs https://github.com/qcom-toolbox/Banana-OS/releases

The optional probe installs with `pkg install` in the same way. A manual LibJS
workflow builds these artifacts by default; disable its `sdk` input for native
probes only.

The root Makefile builds LibJS locally using the Debian prerequisites above.
CI passes the same commit's audited artifact through `LIBJS_WORKER_BPK=/path/to/libjsworker-x86_64.bpk`.
Only the worker is embedded, compressed; its third-party notices remain inside its BPK.

Test the integrated renderer without installing a package:

    python3 ports/libjs/test_vm.py --bundled --memory 512M --uefi --live

Target checks:

    python3 ports/libjs/test_vm.py --dom --memory 512M --live
    python3 ports/libjs/test_vm.py --dom --memory 512M --uefi --live

The 30 DOM checks exercise DOM updates, node/Window identity, event callbacks and removal,
element creation, dynamically inserted iframe execution/rendering, promise jobs,
timers, attributes, native construction and MutationObserver delivery/disconnection,
binary-safe Base64 and UTF-8 text codecs, denied file fetches, forged
bridge indices, exceptions, pixel IPC and CPU containment alongside an
independent renderer. Classic nomodule fallback scripts run because the module
loader is not yet integrated. The existing 26 isolation, 76 offline framework
and six memory checks pass. Real GitHub releases pass in both the original
worker and the integrated LibJS worker.

The DOM fixture's charged peak is approximately 59 MiB, including the image.
Its static PIE is approximately 56 MB and the compressed BPK approximately
23 MB, with full ICU data and embedded fonts. These are prototype measurements,
not web-page benchmarks or optimized shipping sizes.
An additional native differential run checked 2,048 UTF-8 decoding/streaming/fatal
vectors and 512 encoding/encodeInto vectors against Python UTF-8/UTF-16 codecs.
The comparison uses complete output because malformed sequences can be emitted
on different incremental chunks by the two decoders.

TextEncoder and TextDecoder now operate on real LibJS BufferSource values.
UTF-8 decoding supports streaming, fatal errors and BOM handling; encodeInto
writes complete characters and reports consumed UTF-16 units. Other decoder
labels currently throw RangeError. Base64 uses Latin-1 binary strings, including
NUL, instead of converting byte data through C UTF-8 strings.

## Remaining compatibility work

This adapter is not Ladybird LibWeb or a complete WebIDL implementation. DOM
reflection/prototype details, typed host values, Dedicated Workers, ES module
loading, cross-frame
messaging and transferable ports require further work. Existing multimedia
restrictions and TLS verification limitations are unchanged. The older engine
snapshot also needs updates and a security audit before a production switch.

Google reaches consent and a verification page. Native diagnostics now get past
native constructor calls and binary/UTF-8 decoding, then fail on the missing
Dedicated Worker API used by reCAPTCHA. The QEMU live Google check still fails
and may reach the bounded document CPU quota. Search results and manual CAPTCHA
completion have not passed. Reproduce the diagnostic without counting it as a passing regression:

    python3 ports/libjs/test_vm.py --dom --memory 1G --google

The ordinary standalone probe intentionally creates a fresh realm for each eval;
the DOM worker retains its realm and is the path for page integration.
