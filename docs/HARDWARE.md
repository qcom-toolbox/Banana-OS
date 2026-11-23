# Hardware: USB, storage, graphics and drivers

What Banana OS drives, and the Driver Kit for what it does not yet. Back to the [README](../README.md). The per-device status is in [driver-status.md](driver-status.md).

## USB

Banana OS 0.6 has its own USB stack: **xHCI** (USB 3.x) and **EHCI** (USB 2.0) host controllers, device enumeration and hot-plug (`usb rescan`), and these drivers:

| Driver | Devices |
|---|---|
| `r8152` | **Realtek RTL8152 / RTL8152B** USB 2.0 Fast Ethernet (`0bda:8152`): **Lanberg NC-0100-01**, TP-Link UE200 and most "USB 2.0 to RJ45" dongles |
| `cdc_ecm` | Class-compliant USB Ethernet (CDC-ECM): QEMU `usb-net`, many adapters, phone USB tethering |
| `usbhid` | USB keyboards and mice (boot protocol), tablets and touchscreens, HID gamepads and joysticks |
| `xpad` | Xbox 360 controllers (and the pads that copy them), Xbox One / Series controllers |
| `adb` | Android phones and tablets: their USB debugging (adb) and bootloader (fastboot) interfaces, any maker |
| `uaudio` | USB Audio Class 1 headsets, speakers and DACs (48 kHz 16-bit stereo, on xHCI): sound goes to them while they are plugged in |
| `usb-storage` | USB sticks and disks (Bulk-Only, SCSI): FAT32 volumes are mounted at `/mnt/usb` (whole-disk, MBR or GPT partitioned) |

A USB network adapter shows up as **`usb0`** next to the PCI card (`eth0`). When one is plugged in it becomes the active interface and gets an address by DHCP; `ifconfig eth0 up` / `ifconfig usb0 up` switch between them (Banana OS uses one interface at a time).

```
lsusb                 # controllers and devices, with the bound driver
usb rescan            # pick up a device plugged in (or removed) after boot
ifconfig              # every interface; the active one is marked UP
ifconfig usb0 up      # use the USB adapter
```

### Lanberg NC-0100-01 (RTL8152B)

- **Real PC:** plug it into any USB port driven by an xHCI or EHCI controller (all PCs of the last ~15 years). If the firmware has "legacy USB" / "xHCI hand-off" options, either setting works.
- **QEMU:** pass the adapter through from the host:
  ```bash
  qemu-system-i386 -cdrom Banana_OS.iso -m 256 -device qemu-xhci -device usb-host,vendorid=0x0bda,productid=0x8152
  ```
  (QEMU needs permission to the device node: run it as root or add a udev rule for `0bda:8152`.)
- **VirtualBox:** install the Extension Pack, **Settings → USB → USB 2.0 (EHCI) or USB 3.0 (xHCI) Controller**, then add a USB device filter for *Realtek USB 10/100 LAN*.

> The project owner reports RTL8152 working and tested on physical hardware. Its existing driver remains unchanged; register/USB models also protect it against regressions.

Limitations: no USB hubs (plug devices directly into a root port), no UHCI/OHCI controllers - on EHCI-only machines full/low-speed devices (most keyboards and mice) are handed to the companion controller and stay on PS/2 emulation; only FAT32 sticks with 512-byte sectors are mounted (not FAT12/16, exFAT or NTFS).

### Webcams and USB audio

- **Webcams**: USB Video Class 1.0-1.5 (nearly every USB webcam and the cameras built into laptops), on xHCI ports, USB 2 speeds. YUY2 and MJPEG pictures up to 640x480; the smallest streaming setting with the bandwidth the camera asks for is chosen (high-bandwidth endpoints: up to 3 x 1024 bytes a microframe). MJPEG pictures are handed to apps as JPEG files (the kernel has no image codecs). Cameras that stream by bulk transfers are not supported yet. Tested against a model camera: `tools/test_uvc.py`.
- **USB audio** (USB Audio Class 1): speakers and DACs play 48 kHz stereo; microphones record 16-bit mono or stereo at 48 kHz or the nearest rate they have (linear resampling to 48 kHz). A webcam's microphone works with its camera. Tested against model devices: `tools/test_uaudio_in.py`.

## USB sticks and NVMe drives

A FAT32 USB stick is mounted at **`/mnt/usb`** a moment after it is plugged in (a second one at `/mnt/usb2`); FAT32 partitions on an NVMe SSD appear at **`/mnt/nvme`**. They are ordinary folders: `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, redirections (`> /mnt/usb/log.txt`), Files (its **USB** button, *Eject* in the right-click menu), the browser (`file:///mnt/usb`), `pkg install /mnt/usb/app.bpk`... Every change is written to the stick right away - long file names included - so after `umount` (or *Eject*) it can be pulled out.

```
mount                 # what is mounted
umount                # unmount (or: umount /mnt/usb)
mount -a              # mount unmounted sticks again
```

NVMe drives are disks like the IDE and SATA ones: `disks` lists them and `install` can put Banana OS on one (it boots from it on UEFI).

QEMU:

```bash
# a FAT32 stick image (mtools)
truncate -s 64M stick.img && mformat -F -i stick.img ::
qemu-system-x86_64 -cdrom Banana_OS.iso -m 256 -boot order=d -device qemu-xhci \
    -drive if=none,id=st,file=stick.img,format=raw -device usb-storage,drive=st
# an NVMe drive
qemu-system-x86_64 -cdrom Banana_OS.iso -m 256 -drive if=none,id=nv,file=nvme.img,format=raw \
    -device nvme,serial=banana1,drive=nv
```

(`-boot order=d`: a FAT32 stick has a boot sector, and the BIOS would otherwise try to boot from it.)

## Graphics drivers

At boot every display controller gets the driver that knows it (`lsgpu` shows them); without one the framebuffer the firmware set up is used as before. AMD Radeon, NVIDIA GeForce and Intel Arc / Xe cards (no driver for them here: their display engines change with every generation, and recent NVIDIA ones need signed firmware) are named by family and their framebuffer is mapped write-combining (the page attribute table, on every core) - the biggest speed-up a driverless screen can get; their screen size comes from the firmware through Banana Boot (`resolution`, Settings > Screen). Tested on QEMU's ATI Radeon (`-device ati-vga`); the family table follows the vendors' PCI device id ranges.

| Driver | Cards | What it does |
|---|---|---|
| `vmsvga` | VMware SVGA II: VMware Workstation / Player / ESXi, VirtualBox VMSVGA and VBoxSVGA, QEMU `-vga vmware` | Mode setting, the screen updated through the command FIFO, the pointer drawn by the host (alpha or AND/XOR cursor) |
| `virtio` | virtio-gpu: QEMU / KVM `-vga virtio`, `-device virtio-gpu-pci`, crosvm | A screen of its own (also when the firmware has none: `-vga none -device virtio-gpu-pci`), any resolution, the host window's size, a hardware pointer on the cursor queue |
| `bga` | Bochs VBE DISPI: QEMU `-vga std` and `bochs-display`, Bochs, VirtualBox VBoxVGA | Mode setting (ports or memory-mapped registers); sets a mode itself when the firmware gave none |
| `intel` | Intel GMA 950 (945G/GM/GME) and the graphics of the 1st to 10th generation Core: HD Graphics (Ironlake, Sandy Bridge, Ivy Bridge, Haswell, Broadwell, Skylake), HD / UHD 6xx and Iris (Kaby Lake, Coffee Lake, Comet Lake), Iris Plus / UHD (Ice Lake) | Takes the display engine over from the firmware: reads the pipe, plane, mode and stolen memory; the cursor plane for the pointer (64x64 ARGB); waits for the vertical blank; the panel's backlight. The mode stays the panel's native one (the clocks and timings the firmware set are kept) |

`resolution` lists the modes and switches (`resolution 1280x720`; also Settings > Screen). Without a driver it lists the sizes the firmware offers and sets the one used from the next start (`resolution auto`: the loader's choice) - see the boot menu's *Screen:* line; `brightness 60` sets a laptop panel's backlight. Tested in QEMU (`std`, `vmware`, `virtio`, `virtio-gpu-pci` without VGA, `bochs-display`); the Intel driver follows Intel's documentation and could not be tried on that hardware here.

## Driver Kit

Hardware makers (or anyone) can write drivers for Banana OS without changing it: [driver-sdk/banana_driver.h](../driver-sdk/banana_driver.h) is the whole interface. A driver is a position-independent program of the kernel's own (ring 0, no floating point) with one entry point, `int banana_driver_main(const banana_driver_api_t* api)`; the table gives it the kernel log, PCI configuration space and BARs (mapped), I/O ports, device memory, ordinary and DMA memory, timing, shared interrupt lines, tasks, and two ways to plug in: `register_pci_driver()` (the devices of its ID list, probed right away) and `register_display()` (it becomes the screen - a display module takes over from the built-in driver of its card).

```make
DRIVER      = acme-gpu
TITLE       = ACME Graphics 9000
VERSION     = 1.0
SRCS        = acme.c
include /path/to/banana-driver-sdk/driver.mk
```

`make` builds it for both kernels into `acme-gpu.bpk` (type `driver`); on Banana OS `pkg install acme-gpu.bpk` installs it and loads it, and it is loaded again at every boot. `drivers` lists the drivers, what each module drives and its state; `driver load <file>` loads a driver file for testing. Examples in [driver-sdk/examples](../driver-sdk/examples): `edu` (QEMU's `-device edu` teaching device: registers, an interrupt, a task) and `bochsfb` (a display driver for the Bochs / QEMU VGA). `make driver-examples` builds them. The separate `make driver-sdk` target exports `banana-driver-sdk.tar.gz`.
The application `make sdk` target exports `banana-sdk.tar.gz` without driver headers.
The [Driver SDK guide](../driver-sdk/README.md) documents versioned Ethernet, raw Wi-Fi,
USB, firmware and laptop-provider services, callback contexts and lifetime rules.
