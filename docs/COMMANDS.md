# Commands

Every command of the shell. Inside Banana OS, `help <command>` shows the same with examples. Back to the [README](../README.md).

## Available Commands

| Command | Description |
|---|---|
| `help [command\|topic]` / `man <command>` / `<command> --help` | A command's page (synopsis, description, options, examples), a topic's commands, or every command by topic; `help -k <word>` / `apropos <word>` search, `help -a` lists all |
| `neofetch` | Show system information |
| `echo <text>` | Print text |
| `clear` | Clear screen |
| `uname` | Show OS/kernel string |
| `whoami` | Print current user |
| `hostname [NAME]` | Display or change the PC name, also editable in Settings > About; saved on installed systems |
| `date` | Print current date/time (from RTC) |
| `ntp [--set] [server]` | Query network UTC; explicitly set the RTC with `--set` |
| `screenshot [directory]` | Save a PNG capture; Win+Shift+S captures the desktop |
| `notify TITLE [BODY]` | Post a desktop notification; Win+N opens its history |
| `latency [on\|off\|status]` | Opt-in boot-core diagnostics written to serial |
| `ls [-l] [path]` | List a directory (long format with `-l`: mode, owner, group, size and date modified - the time this year, the year for older files; links as `name -> target`) |
| `chmod MODE <file>...` | Change permissions: octal (`755`, `0640`) or symbolic (`u+x`, `go-w`, `a=r`, `u=rwx,g=rx,o=`) |
| `chown OWNER[:GROUP] <file>...` / `chgrp GROUP <file>...` | Change the owner / group (`root`, `banana` or a number) |
| `ln -s [-f] <target> <link>` | Make a symbolic link (`-f` replaces an existing file) |
| `readlink <link>` | Print where a symbolic link points |
| `stat [-L] <path>...` | Everything about a file or folder: type, size, mode, owner, the three dates (`-L` follows links) |
| `fsck [-f] [-y]` | Check the filesystem: the tree in memory, both metadata copies and the extents on the installed disk (`-f`: read every file back against its CRC-32C; `-y`: repair the tree, lost entries go to `/lost+found`) |
| `vmtools [on\|off\|time]` | Guest tools: the hypervisor, VMMouse, the QEMU guest agent (Proxmox), VMware's tools channel, VirtualBox's VMMDev and their state; `off` / `on` from the next start; `time` compares the host's clock with this one |
| `pwd` | Print current directory (full absolute path) |
| `cd [path]` | Change directory (no arg or `~` goes home, `..` up) |
| `mkdir [-p] <dir>` | Create directory (`-p` creates parents too) |
| `rm [-r] <name>` | Remove a file, or a directory with `-r` |
| `touch <file>` | Create an empty file, or set an existing file's date modified to now |
| `cp <src> <dst>` | Copy a file |
| `mv <src> <dst>` | Move/rename a file or directory |
| `edit <file>` | Open built-in nano-like editor |
| `cat <file>` | Print file contents (binary files are refused) |
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
| `time <command>` | Run a command and print its wall-clock time |
| `uptime` | Show uptime and the load average (1, 5, 15 minutes) |
| `nice [-n N] command` | Run a command with its nice value raised by N (default 10; negative = more processor time); `nice` alone shows it |
| `renice [-n] N pid...` | Set the nice value (-20 .. 19) of running tasks |
| `kptest <picture> <svg> [s]` | Checks kernel preemption: tasks decode the picture and draw the SVG at once, preempted inside that code; every result must match |
| `lscpu` | Processors: physical cores, hardware threads (Hyper-Threading), packages, performance / efficient cores, each one's load |
| `adb devices / shell / push / pull / install / logcat / reboot` | Android phones over USB (USB debugging on) |
| `fastboot devices / getvar / flash / erase / boot / reboot` | a phone in its bootloader |
| `firewall [on/off] / allow / deny / ping / log` | the firewall (off by default) |
| `toggle6 [on\|off]` / `ipv6 [on\|off]` | IPv6 switch (off by default; saved), or address/router information |
| `ping6 HOST` / `nslookup -6 HOST` | IPv6 echo / AAAA DNS lookup |
| `crashtest [preview\|report\|panic\|ud2\|pagefault]` | Safe crash-screen preview/report; the last three modes intentionally halt the OS |
| `selftest opencl` | Compile and check OpenCL barriers, shared local memory and vectors on the CPU |
| `selftest browser [swm]` | Offline framework checks; `swm` tests the live https://swm.mxtpasontop.fr/ site with the browser's network and image decoder |
| `selftest ipv6 [HOST [PORT]]` | Local IPv6 TCP/UDP checks; optional network lookup and TCP connection (default port 80) |
| `sensors` | The processor's temperature (Intel and AMD, real hardware) |
| `gamepad` | The game controllers plugged in: sticks, triggers, buttons held |
| `sched` | The scheduler's view: load average, and per task its nice value, weight, switches, wait for the processor, longest run |
| `top` | Live CPU/RAM/process monitor (`q` to quit) |
| `htop` | Colour monitor: a meter per CPU core, memory and disk, the tasks (`P`/`N`/`T` sort by CPU, PID, time; `q` quits) |
| `free [-h\|-k\|-m\|-g]` | RAM: total, used, free, file data in RAM |
| `df [-h\|-k\|-m] [-i]` | Disk space of `/` and the USB sticks: size, used, free (`-i`: files and folders) |
| `ram_info [-t -u -h -f -p -m]` | Memory usage, including the kernel heap (`-h`) |
| `startx` | Start GUI desktop (on the machine's own screen: not over SSH, nor from a terminal window of the desktop) |
| `stopx` | Quit GUI desktop (not over SSH, nor from a terminal window inside the desktop: use Start > Shut down > Exit to the shell) |
| `start` | Alias of `startx` |
| `stop` | Alias of `stopx` |
| `keyboardctl [layout]` | Show/set keyboard layout: `EN (Default)`, `UK`, `US-Intl`, `FR`, `BE`, `fr_CH`, `de_CH`, `DE`, `ES`, `IT`, `PT`, `BEPO` |
| `loadctl [layout]` | Alias of `keyboardctl` |
| `usbctl` | Show USB legacy handoff state |
| `shutdown [now\|-c]` | Schedule shutdown (60s), immediate shutdown, or cancel - powers off the ACPI way (the firmware's FADT and `\_S5_`), which VMware, VirtualBox, QEMU and real PCs all accept |
| `lsgpu` | Graphics cards: driver, mode, video memory, hardware pointer, backlight |
| `code [folder\|file]` | Banana Code, the code editor (also its desktop icon) |
| `resolution [WxH\|auto]` | The screen's modes, or switch to one (without a graphics driver: the size from the next start) |
| `brightness [0-100]` | A laptop panel's backlight (Intel graphics) |
| `drivers` / `driver load <file>` | Drivers and driver modules (Driver Kit) / load one |
| `mediaplayer [file]` / `music [file...]` | The Media Player and Music apps |
| `amethyst [server]` | Amethyst Music: stream from an Amethyst server (asked the first time) |
| `autologin [on\|off]` | Log in without the password at boot (also in Settings > Startup) |
| `reboot` | Immediate reboot |
| `halt` | Hard CPU halt |
| `install [number] [--full\|--standard\|--minimal]` | Install Banana OS onto the IDE, SATA, NVMe or virtio disk you choose: it lists the disks and, with several, asks *Which disk?* (or `install 2`), then asks to confirm (`y`), then installs - the others are left alone. `--standard` puts on only the everyday apps, `--minimal` only the App Store (the rest installs from it). `update [number]` works the same way. Bootable, with a persistent filesystem (live CD/USB only: `install` and `update` do not exist on an installed system) |
| `disks` | List the IDE, SATA (AHCI) and NVMe disks and CD/DVD drives |
| `sync` | Re-write the filesystem to the installed disk on demand |
| **Storage** | |
| `mount [-a]` / `mount <drive>[p<N>] [folder]` | Mounted volumes (USB sticks in `/mnt/usb`, extra disks in `/mnt/sata`, `/mnt/ide`, `/mnt/nvme`, `/mnt/virtio`, a folder per partition); `-a` retries unmounted ones; with a drive or partition name (`sata1`, `sata1p2`) mounts it |
| `lsblk` | The drives as a tree: partitions (MBR primary and logical, GPT), sizes, filesystems, mount points, types |
| `umount [path\|drive]` / `eject` | Unmount a volume, or all of a drive's (`umount sata1`), so it can be pulled out |
| **Apps** | |
| `pkg install <file.bpk>...` | Install (or upgrade) apps built with the SDK |
| `pkg list` / `pkg info <name\|file>` / `pkg remove <name>` | Installed apps, details, uninstall |
| `<app> [args]` / `pkg run <app>` | Run an installed app |
| `pkg ps` | Running apps |
| `apt update` / `apt search <words>` / `apt show <name>` | Package lists from the repositories, find, details |
| `apt install <name>...` / `apt remove <name>...` / `apt autoremove` | Install (with what it needs) / uninstall from repositories |
| `apt upgrade` / `apt list [--installed\|--upgradable]` | Upgrade everything / list packages |
| `apt sources` / `apt add-repo <url> key=<hex>` / `apt remove-repo <url>` | The repositories (`/etc/pkg/sources.list`) |
| `store` | The App Store (desktop) |
| `apps` / `taskmgr` | Open Apps / the Task Manager (desktop running) |
| **Sound** | |
| `play <file.wav>` / `play -s` | Play a WAV file in the background / stop |
| `beep [hz] [ms]` | A tone (sound card, or the PC speaker) |
| `volume [0-100]` | Show / set the volume |
| `lsaudio` | The sound card in use |
| **Networking** | |
| `ifconfig [<if> <ip> [netmask m] [gw g] [dns d]]` | Show the interfaces, or configure one statically |
| `ifconfig <if> up` | Make `eth0` or `usb0` the active interface |
| `lsusb` | List USB controllers and devices |
| `usb rescan` | Detect USB devices plugged in or removed since boot |
| `dhcp` | Request a new DHCP lease |
| `ping [-c N] [-s size] [-i sec] <host>` | ICMP echo (default 4 packets; Ctrl+C stops) |
| `nslookup <host>` / `host <host>` | Resolve a name with DNS |
| `curl [-L] [-o file\|-O] [-I] [-i] [-v] [-s] [-A ua] [--tls13-ciphers ..] <url>` | HTTP/HTTPS client |
| `wget [-q] [-O file] <url>` | Download a file (follows redirects) |
| `netstat` | TCP connections |
| `arp` | ARP cache |
| `cryptotest` | Run the crypto known-answer self-tests |
| `ifconfig reset` | Forget the saved network settings (back to DHCP) |
| **Servers** | |
| `passwd [-d]` | Set (or remove) the password of `banana`, used for SSH logins |
| `sshd [status\|start [port]\|stop\|enable [port]\|disable]` | SSH server (port 22); `enable` also starts it at boot |
| `rdpd [status\|start [port]\|stop\|enable [port]\|disable]` | Remote Desktop server (port 3389), off by default |
| `rdp [-u user] [-d domain] host[:port]` | Remote Desktop client - a window onto another RDP server |
| `httpd [status\|start [port]\|stop\|enable [port]\|disable]` | Web server for `/var/www` (port 80, `.php` pages run); `enable` also starts it at boot |
| `httpd expose [port]` / `httpd unexpose` | Forward a public port on your router (UPnP) to the web server, and back |
| **Wallpaper / desktop** | |
| `wallpaper [list\|reset\|preset <n>\|url <url>\|<file> [fill\|fit\|stretch\|center]]` | Show or change the desktop wallpaper |
| `files [folder]` | Open the file explorer (desktop running) |
| `browser [address]` | Open the web browser, optionally at an address (desktop running) |
| `notepad [file]` | Open Notepad, optionally on a file (desktop running) |
| **Shell syntax** | |
| `cmd > file`, `cmd >> file` | Write / append a command's output to a file |
| `cmd1; cmd2`, `cmd1 && cmd2` | Run commands one after the other |
