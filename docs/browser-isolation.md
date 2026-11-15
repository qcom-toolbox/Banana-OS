# Browser isolation: first milestone

The 64-bit browser and SDK web views now create restricted user processes for
untrusted HTML, CSS, JavaScript, SVG, fonts and raster image decoding. The kernel
retains the window, validated pixel composition, input forwarding and a resource
broker. Engine pointers never cross the IPC boundary. Kernel codec entry points
also delegate to an isolated image worker; the kernel keeps zlib for archives.

Workers cannot open files or sockets, execute commands, create processes or
threads, or access other applications through normal or raw syscalls. The allowed
syscalls cover private memory, read-only UTC/local time and timezone queries,
randomness, scheduling and the authenticated web IPC channel. IPC associates each channel with both a process ID and its
generation, so reusing an application slot does not transfer a channel.

## Broker permissions and quotas

- At most four renderer/decoder channels. Each worker has a 512 MiB physical
  committed-memory limit (loader allocations and touched anonymous mmap pages
  share the same budget), 1 GiB virtual address limit and 512 virtual ranges.
  Their combined physical pool is capped at 1 GiB or half the kernel heap
  available when the first worker starts, whichever is smaller. Compressed or
  swapped mmap pages keep their charge; unmapping or ending the process returns
  it. Quota termination also works when a worker faults on another CPU core.
- CPU use is limited to 500 ms per one-second window. Page loads, image decoding and layout updates have a
  120-second CPU / 300-second wall limit; initialization has a 30-second CPU /
  90-second wall limit; other commands have a 10-second CPU / 30-second wall limit.
  Idle workers wait without using their command budget. A fault or exhausted budget terminates that worker.
- Documents and resource responses are limited to 8 MiB. Each channel has
  512 resource requests and 64 MiB total fetched data for its lifetime.
  Images are limited to eight million pixels with checked output dimensions
  and exact byte counts.
- Resource access defaults to denied until the host supplies a document principal.
  Fetches allow HTTP(S) only. Script requests and cookies are scoped to the
  current origin; cross-origin public assets receive no origin cookies.
  Redirect targets and resolved addresses are checked. Public sites cannot
  use DNS rebinding to reach private or loopback addresses.
- File access requires an exact local document or a file explicitly selected
  through the host's picker. A remote page cannot navigate into local files.
- IPC uses bounded chunks and fixed-width fields. Kernel copies validate
  lengths, offsets, strings and writable user mappings.

These controls reduce the damage from a compromised engine. They do not prove
the kernel or its networking implementation free of vulnerabilities. Existing
TLS certificate verification limitations are unchanged.

## Site compatibility and remaining work

GitHub release pages have a user-process reader that presents release notes and
fetches the release asset fragments, including download and source archive links.
It does not implement the entire GitHub application.

Google search and manual CAPTCHA compatibility remain unfinished. Live testing
reaches reCAPTCHA JavaScript and still fails inside it. Same-window postMessage
now delivers asynchronous MessageEvents with origin filtering and cloned
primitives, plain objects and arrays (including cycles). It is bounded to 64
queued messages, 64 KiB and 128 objects per message. Unsupported clone types
raise DataCloneError. Cross-frame messaging and transferable ports remain
unfinished. No CAPTCHA solver or challenge bypass is included.
The existing default search provider is retained while this work continues.

Page audio and video are decoded in a restricted process: each `<audio>` /
`<video>` stream gets a restricted worker of its own (`apps/webmedia`, FFmpeg,
kept apart from the page renderer) that receives the file and returns sound and
pictures (`WW_MEDIA_*`). The page's worker sees only its own streams' status
and pictures through the broker (`WW_MEDIA`); the kernel (`kernel/media.c`)
keeps the clock, plays the sound and never parses the media itself. Files are
limited to 96 MB, pictures to 1280x720. The 32-bit fallback refuses isolated
browsing rather than reverting to a kernel renderer.

## Verification

Run the sanitized host regressions:

    bash tools/tests/web_worker_ipc.sh
    bash tools/tests/web_policy.sh
    bash tools/tests/web_memory.sh
    bash tools/tests/web_hit.sh
    bash tools/tests/github_reader.sh
    python3 tools/test_serialtest.py

In a disposable VM:

    selftest browser isolation
    selftest browser
    selftest browser github

The isolation test deliberately faults a worker, runs an endless loop and exhausts
its memory, checking that another page still evaluates JavaScript and renders.
It also checks denied direct syscalls, file reads, local navigation, POST form
activation, asynchronous messages, anonymous mmap quota reclamation, starting a
replacement after termination, stale process proxies, image rejection and
validated pixel transfers. Only the deliberate
address-one worker fault is allowed by the serial test harness, and only after
the isolation test reports its containment and overall success.

The existing offline engine framework tests use trusted bundled fixtures.
Production browser pages and SDK web views use the restricted worker path.


## Optional Ladybird LibJS experiment

The standalone LibJS prototype in [ports/libjs](../ports/libjs/README.md) now
runs semantic and containment probes inside the same restricted user-process
boundary. It has no file or network grants and is not embedded in the ISO.
Loaded program pages are charged when the quota is attached, together with
subsequent heap/mmap pages, so the measured footprint includes the engine
image. The optional DOM worker now connects persistent LibJS realms to the C DOM,
events, promise checkpoints, timers and the existing layout/pixel IPC. It is
included as an internal component of the 64-bit OS and selected explicitly with
browser --libjs; no manual package installation is needed. It is installed and
updated at boot, while its execution remains isolated in user space.
Ordinary desktop browser launches use LibJS by default on 64-bit systems;
SDK webviews retain the legacy worker. Use browser --legacy for the older desktop
engine and browser --default to restore LibJS. browser --engine reports the selection. Google
and manual CAPTCHA compatibility remain unfinished. The module loader is not
integrated, so the experimental worker honors classic nomodule fallbacks.


Input hit testing follows rounded outlines, transparent corners and clipping.
A rounded container border no longer covers the controls painted inside it.
Frame clicks also use the topmost visible display item instead of activating a
clipped or covered iframe. The host regression compares 640,008 pixel/hit and
clipping/stacking cases. A physical click on Google's consent rejection button
was verified in the experimental browser.

Layout updates can load newly inserted iframe documents and decode resources.
They use the same bounded 120-second CPU / 300-second wall budget as document
loading; eval, timers, input and rendering use the 10-second CPU / 30-second
wall budget. The per-second CPU throttle and memory limits apply to both.
