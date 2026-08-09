# 🍌 Banana OS 0.3

Banana OS 0.3 is a minimal x86 operating system written from scratch (no Linux kernel, no external OS kernel), bootable in VirtualBox/QEMU via GRUB + Multiboot2.

```
  ____                               ____  ____
 | __ )  __ _ _ __   __ _ _ __   __|  _ \/ ___|
 |  _ \ / _` | '_ \ / _` | '_ \ / _` | | \___  \
 | |_) | (_| | | | | (_| | | | | (_| | |___)  |
 |____/ \__,_|_| |_|\__,_|_| |_|\__,_|___/____/
```

![Banana OS desktop](assets/screenshots/gui-desktop.png)

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
- IDT-based exception handling with a panic screen instead of silent triple-faults
- Dual terminal backend:
  - VGA text mode
  - Framebuffer-rendered console
- GUI desktop (started on demand with `startx`)
- PS/2 keyboard + PS/2 mouse support, plus real Synaptics PS/2 touchpad support (absolute mode + tap-to-click)
- Ctrl+Alt+Delete closes the GUI, from anywhere
- Up to 4 draggable, closable, focusable terminal windows in GUI mode, each with its own independent shell task and scrollback
- Fluxbox-inspired dark desktop theme
- Wallpaper app with 10 PNG/JPG-backed graphical presets from `assets/wallpapers/` (bilinear-upscaled), Azure Flow as the default
- Desktop and menu icons for built-in apps
- In-memory Unix-style filesystem (`/bin`, `/etc`, `/home/banana`, `/usr`, `/var`, `/tmp`, `/dev`, `/root`) with absolute/relative path resolution, `.`/`..`/`~`
- POSIX-flavored shell utilities (`ls -l`, `mkdir -p`, `rm -r`, `cp`, `mv`, `touch`, `whoami`, `hostname`, `date`, ...)
- Two shell personas sharing one command engine - stock `sh` (default) and a bash-compatible `bash` (aliases, `export`/`$VAR`, `!!`) - selectable per-session with `chsh`
- Built-in editor and live system monitor
- Real bootable disk install (`install`/`sync`) - installs onto a dedicated ATA hard disk so Banana OS boots on its own, with a persistent filesystem, no CD required

# Minimum Requirements

- CPU : Yes
- RAM : 8 MB
- GPU : any sort of graphics accelerator should do it
- Keyboard / Mouse : PS/2
- Not hating AI slop

## GUI Overview (`startx`)

- 800x600 framebuffer desktop, Azure Flow wallpaper by default
- Taskbar with:
  - `Start` button
  - `Quit` button
  - live clock
- Start menu and desktop shortcuts, both with the same entries:
  - About app
  - Terminal
  - Wallpaper
  - Quit GUI
- Wallpaper app - pick from 10 built-in presets (bilinear-upscaled to the framebuffer)
- Up to 4 terminal windows (draggable, closable, focusable, scrollable), each running its own independent shell task
- PS/2 mouse and Synaptics touchpad (absolute mode + tap-to-click) both work for pointing
- Ctrl+Alt+Delete quits the GUI immediately, from anywhere

## Filesystem Layout

Banana OS seeds a small Unix-style root hierarchy at boot (in-memory, reset on reboot):

```
/
├── bin/
├── etc/            (motd, hostname, passwd)
├── home/
│   └── banana/     ($HOME - shell starts here, readme.txt)
├── root/
├── usr/
├── var/
├── tmp/
└── dev/
```

Every path-taking command accepts absolute (`/etc/motd`), relative (`../etc`), `.`/`..`, and `~` (home) paths, just like a real Unix shell.

## Shells

Banana OS ships two shell **personas** built on one shared command engine (same builtins, same filesystem, same history) - only the prompt, banner, and a handful of bash-only builtins differ:

| | `sh` (stock, default) | `bash` (opt-in) |
|---|---|---|
| Prompt | `banana` (yellow) `@banana-os-0.3` (green) `:path$ ` | `banana@banana-os-0.3` (all green) `:path$ ` |
| `uname` / `neofetch` | reports `sh` | reports `bash` |
| Aliases, `export`/`$VAR`, `!!` | available (shared engine) | available |

Switch which persona **new** shells boot into with `chsh` - like real Unix `chsh(1)`, it only affects future sessions (new terminal windows, or the next reboot), never the one you ran it from:

```
chsh          # show the current default and this session's persona
chsh bash     # make new shells start as the bash persona
chsh sh       # switch back to the stock shell
```

Bash-flavored extras (available in both personas, since they share one engine):

- `alias [name[=value]]`, `unalias <name>` - e.g. `alias ll='ls -l'`
- `export [NAME=value]`, `unset <name>`, `env` - environment variables; `$NAME` expands inline (`echo $HOME`, `echo $SHELL`)
- `!!` - re-runs (and re-records) the previous command
- `type <cmd>` - like `which`, but alias-aware

## Available Commands

| Command | Description |
|---|---|
| `help` | Show command list |
| `neofetch` | Show system information |
| `echo <text>` | Print text |
| `clear` | Clear screen |
| `uname` | Show OS/kernel string |
| `whoami` | Print current user |
| `hostname` | Print system hostname |
| `date` | Print current date/time (from RTC) |
| `ls [-l] [path]` | List a directory (long format with `-l`) |
| `pwd` | Print current directory (full absolute path) |
| `cd [path]` | Change directory (no arg or `~` goes home, `..` up) |
| `mkdir [-p] <dir>` | Create directory (`-p` creates parents too) |
| `rm [-r] <name>` | Remove a file, or a directory with `-r` |
| `touch <file>` | Create an empty file (or no-op if it exists) |
| `cp <src> <dst>` | Copy a file |
| `mv <src> <dst>` | Move/rename a file or directory |
| `edit <file>` | Open built-in nano-like editor |
| `cat <file>` | Print file contents |
| `grep [-n] <pattern> <file>` | Print lines matching a substring (`-n` numbers them) |
| `wc <file>` | Count lines/words/bytes in a file |
| `head [-n N] <file>` | Print first N lines (default 10) |
| `tail [-n N] <file>` | Print last N lines (default 10) |
| `find [path] [-name <sub>]` | Recursively list files/dirs under path |
| `history` | Show command history |
| `which <cmd>` | Show whether a command is a shell builtin |
| `type <cmd>` | Like `which`, but alias-aware (bash-flavored) |
| `alias [name[=value]]` | List/define a command alias (bash-flavored) |
| `unalias <name>` | Remove an alias |
| `export [NAME=value]` | List/set an environment variable |
| `unset <name>` | Remove an environment variable |
| `env` | List environment variables |
| `chsh [sh\|bash]` | Show/set the default shell persona for new sessions |
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
| `install` | Install Banana OS onto a dedicated ATA hard disk - bootable, with a persistent filesystem |
| `sync` | Re-write the filesystem to the installed disk on demand |

## Installing to a Hard Disk (`install` / `sync`)

By default Banana OS boots from the GRUB CD/ISO every time and its filesystem is in-memory only, reset on every reboot. `install` does a real install onto a **second, dedicated IDE/ATA hard disk** attached to the VM (never the GRUB boot CD - the driver detects and skips ATAPI/optical drives):

```bash
# QEMU: create a blank disk image (64 MB+ recommended - a ~32 MB region is
# reserved for the boot image, plus a bit more for the filesystem), then
# attach it alongside the ISO
qemu-img create -f raw disk.img 64M
qemu-system-i386 -cdrom Banana_OS.iso -drive file=disk.img,format=raw,if=ide
```

In VirtualBox, attach a second blank virtual hard disk (IDE, 64 MB+) to the same VM that boots `Banana_OS.iso`.

Then, inside Banana OS:

```
install     # copies the boot image onto the disk and writes the current filesystem to it
sync        # re-writes the filesystem on demand (also happens automatically on shutdown/reboot/halt)
```

`install` works because `grub-mkrescue` already builds `Banana_OS.iso` as a GRUB "hybrid" image - the same trick that lets Linux live ISOs be `dd`'d straight onto a USB stick or disk and boot with no CD. `install` raw-copies that already-bootable image from the CD onto the target disk via a small ATAPI driver, then writes the filesystem into a reserved region right after it - no custom bootloader needed.

Once installed, the disk boots Banana OS **on its own** - drop `-cdrom Banana_OS.iso` entirely:

```bash
qemu-system-i386 -drive file=disk.img,format=raw,if=ide
```

Every boot after that loads the filesystem back from disk instead of reseeding the defaults, so your files persist too.

## Build (Ubuntu/Debian)

### Quick build

```bash
chmod +x build.sh
./build.sh
```

### Manual build

```bash
sudo apt-get install nasm gcc-multilib grub-pc-bin grub-common xorriso mtools
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
