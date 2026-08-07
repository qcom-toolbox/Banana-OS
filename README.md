# 🍌 Banana OS 0.3

Banana OS 0.3 is a minimal x86 operating system written from scratch (no Linux kernel, no external OS kernel), bootable in VirtualBox/QEMU via GRUB + Multiboot2.

```
  ____                               ____  ____
 | __ )  __ _ _ __   __ _ _ __   __|  _ \/ ___|
 |  _ \ / _` | '_ \ / _` | '_ \ / _` | | \___  \
 | |_) | (_| | | | | (_| | | | | (_| | |___)  |
 |____/ \__,_|_| |_|\__,_|_| |_|\__,_|___/____/
```

## Project Layout

```
bananOS/
├── boot/
│   ├── boot.asm        # Multiboot2 entry point (assembly)
│   └── linker.ld       # Linker script
├── kernel/
│   ├── kernel.c        # kernel_main()
│   ├── terminal.c/h    # Terminal (VGA + framebuffer + virtual terminals)
│   ├── fb.c/h          # Multiboot2 framebuffer driver
│   ├── gfx.c/h         # 2D drawing primitives + bitmap font rendering
│   ├── gui.c/h         # Basic desktop GUI (taskbar/start menu/windows)
│   ├── task.c/h        # Real cooperative kernel threads (own stack + switch) + `top`'s process list
│   ├── task_switch.asm # Context switch (save/restore callee-saved regs, swap esp)
│   ├── daemon.c/h      # Background daemon loop
│   └── keyboard.c/h    # PS/2 keyboard driver
├── shell/
│   ├── shell.c/h       # Banana shell
│   └── editor.c/h      # Nano-like text editor
├── iso/
│   └── boot/grub/
│       └── grub.cfg    # GRUB config
├── Makefile
├── build.sh
└── README.md
```

## Features

- Bare-metal x86 kernel (freestanding C + NASM)
- Multiboot2 boot flow with framebuffer mode
- Dual terminal backend:
  - VGA text mode
  - Framebuffer-rendered console
- GUI desktop (started on demand with `startx`)
- PS/2 keyboard + PS/2 mouse support
- Multiple draggable terminal windows in GUI mode
- Fluxbox-inspired dark desktop theme
- Wallpaper app with PNG-backed graphical presets from `assets/wallpapers/`
- Desktop and menu icons for built-in apps
- In-memory filesystem + shell utilities
- Built-in editor and live system monitor

# Minimum Requirements

- CPU : Yes
- RAM : 8 MB
- GPU : any sort of graphics accelerator should do it
- Keyboard / Mouse : PS/2
- Not hating AI slop

## GUI Overview (`startx`)

- 800x600 framebuffer desktop
- Taskbar with:
  - `Start` button
  - `Quit` button
  - live clock
- Start menu entries:
  - About app
  - Terminal
  - Wallpaper
  - Quit GUI
- Desktop shortcuts:
  - About
  - Terminal
  - Wallpaper
  - Quit GUI
- Up to 4 terminal windows (draggable, closable, focusable)

## Available Commands

| Command | Description |
|---|---|
| `help` | Show command list |
| `neofetch` | Show system information |
| `echo <text>` | Print text |
| `clear` | Clear screen |
| `uname` | Show OS/kernel string |
| `ls` | List current directory |
| `pwd` | Print current directory |
| `cd <dir>` | Change directory (`cd ..` to go up) |
| `mkdir <dir>` | Create directory |
| `rm <name>` | Remove file or directory |
| `edit <file>` | Open built-in nano-like editor |
| `cat <file>` | Print file contents |
| `run <file.sh>` | Execute script line by line |
| `uptime` | Show uptime |
| `top` | Live CPU/RAM/process monitor (`q` to quit) |
| `startx` | Start GUI desktop |
| `stopx` | Quit GUI desktop |
| `start` | Alias of `startx` |
| `stop` | Alias of `stopx` |
| `keyboardctl [layout]` | Show/set keyboard layout (`EN (Default)`, `fr_CH`, `FR`, `DE`, `de_CH`, `BEPO`) |
| `loadctl [layout]` | Alias of `keyboardctl` |
| `usbctl` | Show USB legacy handoff state |
| `shutdown [now|-c]` | Schedule shutdown (60s), immediate shutdown, or cancel |
| `reboot` | Immediate reboot |
| `halt` | Hard CPU halt |

## Build (Ubuntu/Debian)

### Quick build

```bash
chmod +x build.sh
./build.sh
```

### Manual build

```bash
sudo apt-get install nasm gcc-multilib grub-pc-bin grub-common xorriso
make
# Output: Banana_OS.iso
```

## Run in VirtualBox

1. Create a new VM:
   - Name: `Banana OS 0.3`
   - Type: `Other`
   - Version: `Other/Unknown (32-bit)`
2. Assign at least **8 MB RAM** (more recommended for GUI/testing)
3. No virtual disk required
4. Attach `Banana_OS.iso` as optical media
5. Boot

## Run in QEMU

```bash
qemu-system-i386 -cdrom Banana_OS.iso
```

## Technical Notes

- **Bootloader**: GRUB 2 (Multiboot2)
- **Language**: freestanding C + NASM
- **Graphics**: 32-bit framebuffer + 8x8 bitmap font
- **Input**:
  - PS/2 keyboard (`0x60` / `0x64`)
  - PS/2 mouse AUX packets
- **Kernel load address**: `0x100000` (1 MiB)

## 100% Custom Kernel

This project uses no existing OS kernel:

- custom assembly entry point
- custom framebuffer + terminal stack
- custom keyboard/mouse handling
- custom shell and in-memory filesystem
- custom linker script and build chain
