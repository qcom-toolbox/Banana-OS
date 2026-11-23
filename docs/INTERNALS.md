# How Banana OS works

The source tree, the boot, 64-bit and Secure Boot, and technical notes. Back to the [README](../README.md).

## Project Layout

```
Banana-OS/
├── boot/
│   ├── boot.asm        # Multiboot2 entry point, 32-bit kernel
│   ├── boot64.asm      # Multiboot2 entry of the 64-bit kernel: page tables, long mode
│   └── linker.ld, linker64.ld  # Linker scripts (export _kernel_end for the heap)
├── kernel/
│   ├── kernel.c        # kernel_main()
│   ├── idt.c, isr.asm  # CPU exceptions (panic screen) + PIC / hardware IRQs (isr64.asm: 64-bit)
│   ├── timer.c         # PIT at 1 kHz on IRQ0, hlt-based idle
│   ├── task.c/h        # Kernel threads, fair scheduling (weighted run time, nice values, wakeup preemption); app code on other cores
│   ├── smp.c, smp_tramp.asm # Starts the other CPU cores (ACPI MADT, INIT/STARTUP IPIs, real -> long mode)
│   ├── splash.c        # The boot screen: a banana drawn in code, three dots taking turns under it
│   ├── kheap.c         # Kernel heap (first-fit, coalescing) over the Multiboot2 memory map
│   ├── kstring.c       # memcpy/memset/..., ksnprintf
│   ├── serial.c        # COM1 console: mirrored output (async), keyboard input
│   ├── pci.c           # PCI configuration space + bus scan
│   ├── random.c        # CSPRNG (ChaCha20 keyed from a SHA-256 entropy pool)
│   ├── terminal.c/h    # Terminal (VGA + framebuffer + virtual terminals), deferred painting
│   ├── fb.c, gfx.c     # Framebuffer + 2D drawing
│   ├── gui.c/h         # Desktop GUI (taskbar/start menu/windows/wallpaper app)
│   ├── explorer.c      # "Files" window (file explorer, Windows 7 layout)
│   ├── startmenu.c     # the Start menu (Windows 7 layout)
│   ├── fileicons.c     # icons of folders, kinds of files, disks and libraries
│   ├── browser.c       # "Browser" window with tabs (runs web/ in its own task)
│   ├── notepad.c       # "Notepad" window (GUI text editor)
│   ├── winframe.c      # Window moving / resizing / maximizing, shared by the windows
│   ├── clipboard.c     # The desktop's clipboard
│   ├── textbuf.c       # Text buffer with selection (editor, Notepad)
│   ├── ahci.c, disk.c  # SATA (AHCI) driver; disk.c puts IDE and SATA drives behind one API
│   ├── tty.c           # Remote terminals (SSH sessions)
│   ├── config.c        # key=value settings in /etc (network, services)
│   ├── passwd.c        # Password hashes (PBKDF2) in /etc/shadow
│   ├── wallpaper.c     # Wallpaper presets, user images, /etc/wallpaper
│   ├── image.c         # Image decoding (via stb_image) + resampling
│   ├── fs.c, fsdisk.c  # In-memory Unix-like FS (with mounts) + on-disk persistence (only changed files are written)
│   ├── fat32.c         # FAT32 volumes (USB sticks, NVMe partitions), long file names
│   ├── blockdev.c      # Removable drives + the automount task
│   ├── nvme.c          # NVMe SSD driver
│   ├── virtio_blk.c    # virtio-blk disks (QEMU/KVM, clouds)
│   ├── virtio_scsi.c   # virtio-scsi disks (QEMU/KVM, Proxmox)
│   ├── virtio_pci.c    # the virtio PCI transport (legacy and modern)
│   ├── sdhci.c         # SD card readers and eMMC
│   ├── sensors.c       # CPU temperature (Intel DTS, AMD Tctl)
│   ├── applesmc.c      # the Apple SMC of Intel Macs (temperatures, fans)
│   ├── gamepad.c       # game controllers (the common layout)
│   ├── paging.c        # Maps device memory above 4 GiB (64-bit, UEFI)
│   ├── audio.c         # Intel HD Audio, AC'97, PC speaker, WAV playback (USB audio: usb/uaudio.c)
│   ├── app.c, appcall*.asm # App loader (PIE ELF) + the app API table
│   ├── appwin.c        # Windows of apps
│   ├── rdpclient.c rdpclient_rle.c # Remote Desktop client (RDP) + bitmap decoders
│   ├── pkg.c           # .bpk packages (pkg install / remove / run)
│   ├── repo.c          # package repositories: sources, signed indexes, dependencies (apt, the App Store)
│   ├── launcher.c      # "Apps" window
│   ├── taskmgr.c       # "Task Manager" window
│   ├── settings.c      # "Settings" window
│   ├── ctxmenu.c       # Right-click menus
│   ├── examples.c, exbin.asm # The SDK examples built into the kernel (/usr/share/extras)
│   └── keyboard.c ...  # PS/2 keyboard/mouse, ATA/ATAPI, RTC, USB handoff
├── net/
│   ├── e1000.c         # Intel 8254x NIC driver
│   ├── rtl8139.c       # Realtek RTL8139 NIC driver
│   ├── r8169.c         # Realtek RTL8111/8168, RTL8169, RTL8101/8102 NIC driver
│   ├── pcnet.c         # AMD PCnet-PCI II / PCnet-FAST III NIC driver
│   ├── virtio_net.c    # virtio-net NIC driver (legacy interface)
│   ├── usbnet.c        # Shared plumbing for USB network adapters
│   ├── r8152.c         # Realtek RTL8152/8152B USB Ethernet (Lanberg NC-0100-01, ...)
│   ├── cdc_ecm.c       # USB CDC-ECM class Ethernet (QEMU usb-net, phones, ...)
│   ├── net.c           # Interfaces (eth0, usb0), Ethernet, netd task
│   ├── arp.c ip.c icmp.c udp.c dhcp.c dns.c tcp.c
│   ├── netconf.c       # Saved network configuration (/etc/network.conf)
│   ├── tls.c           # TLS 1.3 client
│   ├── tls12.c         # TLS 1.2 client + server (Remote Desktop)
│   ├── http.c          # HTTP/1.1 client (curl, wget)
│   ├── httpd.c         # Web server
│   ├── httpd_php.c     # .php pages for httpd
│   ├── upnp.c          # UPnP IGD client (httpd expose: port forwarding on the router)
│   ├── sshd.c          # SSH server
│   ├── rdpd.c rdp.h    # Remote Desktop server (RDP)
│   └── ntlm.c credssp.c # NTLMv2 + CredSSP (the RDP client's NLA)
├── web/                # Browser engine: html.c (parser + DOM), css.c, layout.c, render.c,
│                       #   script.c + script_lib.c + script_es.c (JavaScript and PHP interpreter),
│                       #   regex.c (regular expressions), jsdom.c (the DOM, ES modules), page.c,
│                       #   prelude.js (web APIs written in JavaScript; prelude.h is generated from it)
├── crypto/             # SHA-256/512, HMAC/HKDF/PBKDF2, ChaCha20, Poly1305, AES-128-GCM, X25519, Ed25519, self-tests
├── shell/
│   ├── shell.c/h       # Banana shell
│   ├── netcmds.c       # ifconfig, ping, curl, wget, ...
│   ├── srvcmds.c       # sshd, rdpd, rdp, httpd, passwd, files, browser, notepad
│   ├── wpcmd.c         # wallpaper command
│   ├── aptcmd.c        # apt / apt-get (repositories, kernel/repo.c)
│   └── editor.c/h      # Nano-like text editor
├── sdk/                # The Linux SDK for apps: include/, lib/, banana.mk, tools/bpkg, examples/
├── third_party/        # stb_image (runtime image decoding), lodepng
├── loader/             # Banana Boot, the boot loader
│   ├── mbr.asm         # the MBR of USB sticks and disks (loads bios.bin)
│   ├── bios.asm/.c     # BIOS loader: CD entry, real-mode BIOS calls, menu, ISO9660, ELF, VBE, E820
│   ├── efi.c, efi.h    # UEFI loader (BOOTX64.EFI): menu, FAT file, GOP, memory map, ExitBootServices
│   ├── tramp.S, tramp32.S # UEFI: to 32-bit protected mode (from 64-bit long mode / 32-bit firmware)
│   ├── sha256.h        # UEFI: the kernels checked under Secure Boot
│   ├── keys/           # Secure Boot: Banana OS's certificate (the private key is not here)
│   └── common.h        # the menu, the command line, ELF and Multiboot2 for both
├── tools/mkimage.py    # makes the ISO a hybrid CD/disk image (MBR, EFI partition)
├── tools/repo/         # banana-repo (make, sign and serve package repositories), install-server.sh
├── Makefile
├── build.sh
└── README.md
```

## Banana Boot and 64-bit: how it works

- **One image, two firmwares, three media**: `Banana_OS.iso` is an ISO9660 CD with two El Torito entries - the BIOS loader (`/boot/bios.bin`) and the EFI system partition (`/efi.img`: `EFI/BOOT/BOOTX64.EFI` and the two kernels) - and, for USB sticks and disks, an MBR (`tools/mkimage.py`): its code loads `/boot/bios.bin`, its one partition is `/efi.img`. Written to a disk by `install` (or with `dd` to a USB stick) it boots both ways.
- **BIOS**: the MBR (or, from a CD, the first 2 KiB of `bios.bin`, which El Torito loads) brings the loader to 0x8000. It enables A20, switches to 32-bit protected mode and runs C; every BIOS service (disk reads, keyboard, VBE, E820) is a trip back to real mode through `bios_int`. It finds `/boot/kernel64.bin` in the ISO9660 directory, copies the ELF segments to 1 MiB, picks the VBE mode closest to what the kernel's Multiboot2 header asks for (800x600x32) and starts it.
- **UEFI**: `BOOTX64.EFI` is position-independent C with its own UEFI definitions, linked to an ELF image that `objcopy` turns into a PE program with no relocations. It reads the kernel from its own partition, keeps the GOP mode the firmware set, converts and merges the memory map, calls ExitBootServices, and jumps through `tramp.S`: a 32-bit code segment, paging off, long mode off - the state a BIOS boot gives - so the same kernel entry serves both.
- **The hand-over**: Multiboot2, as GRUB did it - EAX = 0x36D76289, EBX = the information: the command line (`medium=live-cd` or `install`, `nosmp`, `verbose`), the memory map, the framebuffer, the BIOS boot drive or the EFI system table, the ACPI RSDP.
- **Long mode**: both kernels start in 32-bit protected mode on both firmwares. `boot/boot64.asm` checks the CPU, builds page tables mapping the first 4 GiB 1:1 with 2 MiB pages (RAM, PCI device memory and the framebuffer all live there), enables PAE, long mode and paging, loads a 64-bit GDT and jumps to `kernel_main`.
- **Same C code**: both kernels are built from the same sources (`*.o` for i386, `*.o64` for x86_64: `-mcmodel=small -mno-red-zone`, no SSE). Pointer-sized types come from the compiler, the interrupt stubs (`isr64.asm`), the task switch (`task_switch64.asm`), the IDT gate format and the new-task stack frame have 64-bit versions; the heap stays below 3.5 GiB, so DMA addresses still fit the 32-bit registers of the older devices.
- **UEFI quirk**: UEFI firmware leaves the CPU's local APIC enabled with the legacy PIC input masked (a BIOS sets it to pass-through); Banana OS drives interrupts with the 8259 PIC, so it switches the local APIC off at boot - without that, no timer or device interrupt would arrive.
- **Several CPU cores and Hyper-Threading**: the 64-bit kernel starts every logical processor the ACPI tables list (up to 64; xAPIC and x2APIC entries), in x2APIC mode when the firmware uses it or locks it (newer Intel platforms) or ids need it. It knows which logical processors are hardware threads of the same physical core (CPUID 0x1F / 0xB, or 1 / 4 / AMD leaves on older processors) and which cores are performance or efficient ones on hybrid Intel processors, and puts app work on a whole idle core first, then on a sibling thread; `lscpu` shows it all. The kernel itself runs on the first core; apps run on all of them - an app's threads compute in parallel (`threads`, `mandel` in /usr/share/extras), and every system call an app makes on another core is carried out on the first one. Task Manager and `top` show which core each app runs on. The 32-bit kernel uses one core.
- **32-bit UEFI**: `BOOTIA32.EFI` is the same C built for IA32 firmware (cdecl, `-malign-double` for the specification's structure layout); there the firmware already runs in 32-bit protected mode, so `tramp32.S` only loads a flat GDT and turns paging off.
- **Memory above 4 GiB** (64-bit): the kernel heap stays below 3.5 GiB (its buffers are DMA buffers for 32-bit devices); the RAM the firmware lists above 4 GiB is mapped at boot and goes to apps' memory, which takes it first (`kernel/hmem.c`) - up to 512 GiB. `free` and Task Manager count it.
- **Not (yet)**: Secure Boot with the keys PCs ship with (Microsoft's) - Banana OS's key must be enrolled, see below; kernel code on more than one core.

## Secure Boot

Banana Boot's UEFI loaders (`BOOTX64.EFI`, `BOOTIA32.EFI`) are signed with Banana OS's own key - certificate `loader/keys/banana-sb.crt` ("Banana OS Secure Boot 2026", SHA-256 fingerprint `88:91:57:37:6D:10:07:65:90:14:69:1E:C5:B6:83:41:FB:04:FA:A7:76:7D:16:0D:DF:DD:F0:0C:68:A5:14:22`). PCs come trusting Microsoft's keys only, so the firmware refuses Banana OS until the key is enrolled - or turn Secure Boot off, as before. Two ways to enroll it:

- **From the boot menu**: clear the Secure Boot keys in the firmware's settings (this puts it in *setup mode*), start Banana OS from the CD or stick, choose **Enroll Banana OS's Secure Boot keys** and press Y. The certificate becomes the platform key (PK) and is added to KEK and db (what db already holds stays). The signed key files are `EFI/BananaOS/PK.auth`, `KEK.auth`, `db.auth` on the EFI partition.
- **In the firmware's settings**: many firmwares can add a certificate to db from a file (*Secure Boot > Key management > db > Append / Enroll from file*) and keep Microsoft's keys: pick `EFI/BananaOS/BananaOS.cer` on the stick or CD.

With Secure Boot on, the firmware checks the loader's signature, and the loader checks the kernel: it holds the SHA-256 of both kernels it was built with and starts no other ("Secure Boot: the kernel is not the one this loader was built with"). The menu shows *Secure Boot: on / off / setup*. `install` keeps the signature valid: the line it rewrites (`medium=live-cd` -> `install`) is in `EFI/BananaOS/medium.cfg`, not in the signed loaders.

Building: the Makefile signs with `sbsign` when the private key is at `~/.banana-secureboot/banana-sb.key` (the GitHub build gets it from the repository secret `BANANA_SB_KEY`); without it the loaders are built unsigned (a note says so) and boot with Secure Boot off. The private key is never in the repository.

## Technical Notes

- **Bootloader**: Banana Boot, our own (Multiboot2), BIOS and UEFI
- **Architectures**: x86_64 (long mode) and i686 (protected mode), same sources
- **Language**: freestanding C + NASM, no floating point (integer-only graphics and crypto)
- **Graphics**: 32-bit framebuffer + 8x8 bitmap font; the console paints lazily (dirty rows, coalesced scrolling)
- **Interrupts**: PIC remapped to vectors 32-47; IRQ0 (1 kHz timer) and the NIC's IRQ are used, keyboard/mouse stay polled
- **Scheduling**: kernel threads switch where they choose to (app code is also time-sliced by the timer); the next to run is the ready one that has had the least CPU time (counted with the TSC, weighted by its **nice value**, -20 to 19, with Linux's CFS weights: the desktop's task -3, daemons like autosave 6). Busy work gives way after a time slice that shrinks as more tasks compete (16 ms shared, 2-8 ms each) and, after at least 1 ms, as soon as a sleeping task wakes (a key, a timer, a packet): a 5 ms sleep next to busy threads now wakes ~0.6 ms late instead of ~4 ms. The other cores take app code by the least weighted processor time (on any core), for a turn scaled by weight, and the first core takes turns with them. With nothing to run the CPU halts until the next interrupt. **Kernel preemption**: the kernel code that computes for long - picture and zlib decoding (stb_image: PNG, JPEG, GIF, BMP, compressed data) and SVG drawing - is preemptible: a timer interrupt in it switches tasks like one in app code (a Wikipedia page used to hold the processor 734 ms while drawing its SVG logo; now a time slice at most). That code keeps no mutable state of its own, and `kptest <picture> <svg>` checks it: tasks decode at once, preempted thousands of times, every result compared. The rest of the kernel switches where it chooses, as before (its shared structures are never seen half-updated); the desktop's task is never preempted in kernel code; the kernel log names any task that keeps the processor over 50 ms. `nice`, `renice`, Task Manager's *Processes* tab (right-click: High ... Low) change priorities; `sched` shows each task's switches, wait for the processor and longest run, `uptime` and `htop` the load average
- **Multicore** (64-bit): the other cores are started with INIT/STARTUP IPIs through a real-mode trampoline at 0x8000 and run app code taken from a queue; the boot core's local APIC is on in "virtual wire" mode (the 8259 still delivers the device interrupts), IPIs move tasks between cores
- **Memory**: physical = virtual (32-bit: paging off; 64-bit: the first 4 GiB identity-mapped), kernel heap from the Multiboot2 memory map, so heap buffers double as DMA buffers. 64-bit apps run in ring 3 on page tables of their own: a copy of that map in which only the app's 2 MiB blocks of memory are open to user mode (`kernel/uspace.c`, `kernel/umem.c`)
- **Input**:
  - PS/2 keyboard (`0x60` / `0x64`)
  - PS/2 mouse AUX packets
  - COM1 serial
- **Kernel load address**: `0x100000` (1 MiB)

## 100% Custom Kernel

This project uses no existing OS kernel:

- custom assembly entry point
- custom framebuffer + terminal stack
- custom keyboard/mouse handling
- custom shell and in-memory filesystem
- custom NIC drivers, TCP/IP stack and TLS
- custom linker script and build chain

The kernel, boot loader, drivers, network stack, shell and desktop are Banana OS's own. Third-party code is limited to the image decoders (stb_image, lodepng, libwebp) and the ported programs - TinyCC in Banana Code, FFmpeg in Media Player, Mesa, OpenJDK, LWJGL and LLVM - see `THIRD-PARTY-NOTICES`.
