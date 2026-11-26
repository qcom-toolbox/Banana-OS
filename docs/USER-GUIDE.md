# Using Banana OS

The desktop, its apps and the shell. Back to the [README](../README.md).

## GUI Overview (`startx`)

- 800x600 framebuffer desktop, Azure Flow wallpaper by default
- Taskbar with:
  - `Start` button
  - network status (`eth0 10.0.2.15`)
  - `Quit` button
  - live clock
- Start menu, Windows 7 style (the round orb on the taskbar, the Windows key or Ctrl+T): on the left the programs with big icons (Browser and Files pinned on top, then Terminal, Notepad, Task Manager and the installed apps), **All Programs** (every program A-Z, scrolls with the wheel) and the **search box** - just type: programs and files of the home folder show up as you type, Enter opens the first; on the right the user's picture (it shows the icon of the place under the pointer), *banana* (home), Documents, Pictures, Music, Downloads, Computer, Settings, Task Manager, Apps (and *Install Banana OS* on the live CD), and **Shut down** with its arrow (*Restart*, *Lock*, *Exit to the shell*)
- Desktop icons like on Windows: the icon with its name under it, straight on the wallpaper (no box behind the text, a shadow keeps it readable); every installed app gets its own icon too. Click selects, double-click opens, right-click an app's icon for *Open* / *Remove this app*
- Files - the file explorer (below)
- Browser - the web browser (below)
- Notepad - a text editor window: click to place the cursor, drag to select, double-click selects a word; Ctrl+A/C/X/V, Ctrl+S save, Ctrl+O open, Ctrl+N new, Ctrl+F find and Ctrl+G find next, right-click pastes; closing it with unsaved changes asks whether to save them
- Wallpaper app - pick one of 10 built-in presets, or one of your own pictures from `~/Pictures`
- Resizable Terminal windows with tabs, Bash by default and a shell selectable through chsh; all use the original Terminal icon
- Every window resizes from its bottom-right corner; double-click a title bar to maximize it (and again to restore)
- Copy and paste: drag over a terminal's text to select and copy it, right-click to paste it (into the shell, or whatever runs there); Notepad, the editor (Ctrl+C / Ctrl+V) and the browser share the same clipboard
- PS/2 mouse, Synaptics PS/2 and I2C HID touchpads (gestures, see above) all work for pointing
- Ctrl+Alt+Delete quits the GUI immediately, from anywhere

## The desktop: taskbar, Task Manager, right-click

- **Taskbar**: the **banana** button opens the Start menu; every open window has a button - click it to bring the window to the front, click again to minimize it (every window also has **_** minimize, maximize / restore and **x** close buttons); the right side shows the network status and the clock. Right-click the taskbar for **Task Manager**, **Show the desktop** and **Restore all windows**; right-click a window's button to restore, minimize or close it.
- **Task Manager**: *Apps & windows* (Switch to / End task, also on right-click), *Processes* (the kernel's tasks, their state, CPU use and priority - right-click one to change it), *Performance* (CPU and memory graphs, uptime, files, network, sound). Open it from its desktop icon, the Start menu, the taskbar's right-click menu, or `taskmgr`.
- **Right-click menus**: the desktop (open any app, wallpaper, exit), terminals (copy, paste, clear, new, minimize, close), Files (open, install app, play, set as wallpaper, edit, cut / copy / paste, rename, delete, properties, new folder / text file, terminal here, eject), the browser (open a link / in a new tab, save it, copy its address, back, forward, reload, copy, paste, save page, page source, Downloads folder), Notepad (cut, copy, paste, select all, find, open, save), Apps (open, details, uninstall).
- Files also takes the keyboard while it is in front: arrows (and left / right in the Icons view), Enter, Backspace (back), Delete, Ctrl+C / Ctrl+X / Ctrl+V, F2 / Ctrl+R (rename), Ctrl+N (new folder), Ctrl+F (search), Ctrl+L (address), a letter (jump to an item).

## File Explorer ("Files")

Open it from the desktop (`startx`): the **Files** icon or Start menu entry, or `files [folder]` in a terminal.

Laid out like Windows 7's Explorer:

- **Back / Forward** (round buttons; Backspace goes back), the **address bar** as a breadcrumb (*Computer > home > banana*: click a part to go there, click beside the parts - or Ctrl+L / F4 - to type a path), and the **search box** (click it, Ctrl+F or F3): the folder shows only what matches as you type, Esc clears it
- The command bar: **Organize** (cut, copy, paste, delete, rename, new text document, properties, refresh, close), **New folder** (named right away), **Open in terminal**, **Up**, and the views **Details** (Name, Type, Size - click a column heading to sort, again to reverse) or **Icons** (large icons)
- The **navigation pane**: Favorites (Downloads, the home folder), Libraries (Documents, Music, Pictures, Videos - made when first opened), Computer (the whole disk, a USB stick when one is in)
- The **details pane** at the bottom: the selected item's icon (a thumbnail for pictures), type, size, date modified and date created - or the folder's item count and total size; messages show there too
- **Dates**: every file and folder records when it was made and last changed (a folder: when something in it was added, removed or renamed; a copy keeps the original's date, like Windows). They are saved on the installed disk with the files, and USB sticks show the dates their FAT32 entries hold (and get today's date for what Banana OS writes). The Details view has a *Date modified* column (sortable)
- Icons by kind of file (pictures, songs, videos, apps, archives, programs, web pages, PDFs, source code...)
- Click selects, double-click (or Enter) opens: folders, apps (.bpk) install, songs and videos play in their app, web pages and SVG open in the Browser, pictures open in Photos, `.ttf` fonts install, text opens in Notepad; typing a letter jumps to the next item starting with it
- Right-click an item or the empty space for its menu; F2 renames, Delete deletes (twice to confirm; folders with their contents), Ctrl+C / Ctrl+X / Ctrl+V copy / move, Ctrl+N new folder, F5 refresh
- The window can be dragged by its title bar, resized from its bottom-right corner, and stacks with the terminal windows

## Web Browser ("Browser")

Open it from the desktop (`startx`): the **Browser** icon or Start menu entry, or `browser [address]` in a terminal (`browser localhost/demo.php`, `browser example.com`, `browser /home/banana`).

- Addresses: `http://` and `https://` (redirects followed, cookies kept for the session), `file:///path` (files and folder listings), `about:home` (the start page). A bare name gets `http://`, a path gets `file://`
- **<** / **>** history, **R** reload, **Hm** start page, the address bar (click it or Ctrl+L, Enter to go); scrollbar, arrow keys, space and PgUp/PgDn scroll; Backspace goes back; Ctrl+R reloads
- **Tabs**: **+** or Ctrl+N opens one, its **x** or Ctrl+W closes it, a right-click on a link (or `target=_blank`, `window.open`) opens the link in a new tab; up to 8, each with its own page and history
- **Copy and paste**: drag over the page's text to select it, Ctrl+C copies; Ctrl+V (or Paste in the right-click menu) pastes into the address bar or the focused text field
- **Right-click**: open a link (or in a new tab), save it, copy its address; back, forward, reload, copy, paste, save the page, view its source, open the Downloads folder
- **Downloads**: a link to something that is not a web page (a `.bpk` app, an archive, a program, `Content-Disposition: attachment`) is saved to `~/Downloads` - the progress and the result show in the status bar, and the page stays where it was. *Save link as* / *Save page as* always save (up to 32 MiB)
- The window resizes from its bottom-right corner (the page is laid out again for the new width); double-click the title to maximize
- HTML: a forgiving parser (missing end tags, entities, `<script>`/`<style>`), headings, paragraphs, lists, tables, links (`#anchors` too), `<pre>`, images (PNG/JPEG/GIF/BMP/static WebP), forms (text fields, textareas, checkboxes, radio buttons, selects, buttons, **file uploads** - submitted as GET, POST or `multipart/form-data`)
- CSS: `<style>`, `<link rel=stylesheet>` and `style=""`; selectors up to level 4 (`+` `~` `>` combinators, `:not()`, `:is()`, `:where()`, `:nth-child()` and friends, `:checked`, attribute operators), specificity and `!important`, custom properties (`var(--x)`), `calc()`/`min()`/`max()`/`clamp()`, `hsl()`/`rgba()`, `@media` width queries, `@supports`, `@layer`; colors, backgrounds, borders, margins/padding, widths, `margin: auto`, font size (built-in TrueType faces), bold/italic/underline, `text-align`, `display` (flex layout, explicit grid columns with fixed/percentage/fr tracks, `minmax`, numeric `repeat`, gaps and automatic rows), `position`, `opacity`, `visibility`, `white-space`
- JavaScript: Banana OS's own interpreter - `var/let/const` (with hoisting), functions, arrows, closures, classes (fields, `#private`, getters/setters, `static`, `extends`/`super`), destructuring, spread/rest, template strings (tagged too), labels, `async`/`await`, `for...of`, regular expressions (named groups, lookaround, flags `gimsuy`), Promise, Map/Set/WeakMap, Symbol, `Function()`/`eval`, and the usual String/Array/Object/Math/JSON/Date methods. **ES modules**: `<script type="module">`, `import`/`export` in all their forms, `import()`, `import.meta`, import maps; `nomodule` fallbacks are skipped
- The DOM: `document.getElementById/querySelector(All)/createElement/write`, `innerHTML`, `textContent`, `value`, `style`, `classList`, `attributes`, `appendChild`/`remove`/`insertAdjacent*`/..., events (`addEventListener`, `dispatchEvent`, `Event`/`CustomEvent`, `onclick=""` handlers: click, input, change, submit, keydown/keyup, load), `Element.prototype` and friends (polyfills work), `setTimeout/setInterval`, `requestAnimationFrame`, `fetch` and `XMLHttpRequest` (any method; bodies: text, `FormData` with files, `Blob`/`File`, `URLSearchParams`, bytes; `xhr.upload` progress), `File`/`FileList`/`FileReader`, `URL`/`URLSearchParams`, `AbortController`, `MessageChannel`, `history`, `matchMedia`, MutationObserver (asynchronous records, filters and old values; other observer types remain limited), `alert()`, `location`, `localStorage` (for the page's lifetime)
- **Uploads**: an `<input type="file">` (clicked, or `input.click()` from a script) opens *Choose a file to upload* over the page - arrows / Enter / Esc, Tab through the places; the page only ever gets to read the file you chose (its `value` is `C:\fakepath\name`, as in every browser)
- Real sites: Wikipedia, Hacker News, MDN, BBC News, Bing and catbox.moe (uploads) load with their scripts; big script-heavy apps (GitHub) partly work, within the page's memory
- Not supported: full CSS Grid (spans, named lines, explicit row tracks and auto-fit/auto-fill), web fonts, animated WebP, `<canvas>`, video, WebSockets, generators (`function*`). A script error is shown in the status bar (with the line and column) and the rest of the page still works
- Pages run in the browser's own task (an 8 MiB stack, with guard pages under it); a crash in it closes only that page (see *Latest additions*); a page gets a third of the free heap (16 to 384 MiB), and a script stops after 5 million steps

## Sound

| Driver | Devices |
|---|---|
| Intel HD Audio | ICH6 and newer chipsets, QEMU `-device intel-hda -device hda-output`, VirtualBox "Intel HD Audio"; any codec - Realtek ALC, IDT/SigmaTel, Conexant, VIA, Analog Devices |
| Intel AC'97 | ICH - ICH7, QEMU `-device AC97`, VirtualBox "ICH AC97" (its default) |
| USB audio | class-compliant (UAC1) headsets, USB speakers and sound cards, QEMU `-device usb-audio` - they take over while plugged in, the computer's own card plays again when they are unplugged |
| PC speaker | always: `beep` uses it when there is no sound card |

Sound is mixed into one 48 kHz 16-bit stereo stream; WAV files (PCM, 8/16-bit, mono/stereo, any rate) are converted on the fly. `play file.wav` plays in the background (`play -s` stops), `volume 60` sets the volume, `lsaudio` shows the card. Apps get sound through the SDK (`banana_play`, `banana_tone`). In Files, double-click a `.wav` to hear it.

QEMU: `-audiodev pa,id=snd0 -device intel-hda -device hda-output,audiodev=snd0` (or `-audiodev wav,id=snd0,path=out.wav` to record what Banana OS plays).


### Microphones and the Camera app

Recording comes from the computer's own input - an HD Audio mic or line-in jack, an AC'97 mic - or from a USB microphone while one is plugged in (a headset, a webcam's microphone: any rate it offers is made 48 kHz). `lsaudio` names it; `mic` listens for a second and says how loud it was, `mic level` shows a level meter, `mic record 5 memo.wav` records five seconds.

The **Camera** app shows a USB webcam live, takes photos (Space: `~/Pictures/Camera-<date>.bmp`) and records voice memos (R: `~/Recordings/Memo-<date>.wav`); M mirrors the picture. Either works without the other. `webcam` checks the camera from the shell, `webcam snap file.bmp` takes a picture, `lscam` lists its sizes. Apps use both through API version 34 (`mic`, `camera` in `banana_api.h`).

## Banana Code

`code` in a terminal, its desktop icon, or *Open in Banana Code* on a folder or a file in Files.

- **Projects**: a folder with a `banana.json` - `name`, `title`, `version`, `type` (`console` or `gui`), `sources`, optional `cflags` and `data` (files shipped with the app). *New App* (top bar, or the *Run and Build* view) makes one in `~/Projects` with a ready-to-run `main.c`; *Port a Program* makes one for the `.c` files of the open folder.
- **Building on Banana OS**: TinyCC is built into the editor, with the SDK's C library and headers (`/apps/code/tcc`). F7 compiles the sources and packs `build/<name>.bpk` (for the computer's own CPU; the SDK on Linux still builds packages for both); F5 also installs it and starts it - a console app in a terminal window of its own. Errors and warnings go to *Problems* (click one to jump to it) and are underlined in the editor.
- **Editing**: Ctrl+S save, Ctrl+P go to file, Ctrl+Shift+P all commands, Ctrl+F find (F3 next), Ctrl+H replace all, Ctrl+G go to line, Ctrl+/ comment, Ctrl+D duplicate a line, Ctrl+Z / Ctrl+Y undo / redo, Ctrl+B side bar, Ctrl+J panel, Shift + arrows / mouse drag / double-click to select, Tab / Shift+Tab indent, automatic indentation and closing brackets.
- **Extensions** (the squares icon): the installed ones, *Enable* / *Disable* / *Uninstall*, *Install from Folder...* (a folder with an `extension.json`, copied to `~/.bcode/extensions`). An extension's view opens on the right from its icon in the activity bar. How to write one: the comment at the top of `apps/code/ext.c` lists the messages; `apps/code/extensions/claude` is a complete example.
- **Claude**: the orange *C*. Paste an API key from console.anthropic.com once (it stays on this computer, in `~/.bcode/state/claude`, and is sent only to api.anthropic.com); choose Opus 5.5, Sonnet 5.5 or Haiku 5.5; ask with the open file included (or only the selection), or use *Explain*, *Fix build errors*, *Port to Banana OS*, *Review* - also in the command palette. Code in an answer has *Insert at cursor*, *Replace selection* and *New file* buttons.

## Wallpapers

Built-in presets, or any PNG, JPEG (baseline or progressive), BMP or GIF you put on the filesystem:

```bash
# download a picture and use it in one go (saved to ~/Pictures)
wallpaper url https://picsum.photos/id/1018/1920/1080

# or step by step
wget -O ~/Pictures/mountains.jpg https://example.com/mountains.jpg
wallpaper ~/Pictures/mountains.jpg          # fill the screen (crop to fit)
wallpaper ~/Pictures/mountains.jpg fit      # letterbox instead; also: stretch, center

wallpaper list                              # presets + your pictures
wallpaper preset "Ocean Wave"
wallpaper reset                             # back to Azure Flow
```

In the GUI, **Settings > Wallpaper** shows the presets and *Your pictures* (the last five you used, with thumbnails; the small x takes one off the list); *Browse...* picks any picture on the disk or a USB stick, and the position buttons switch between Fill, Fit, Stretch and Center. Right-click a picture in Files (*Set as desktop background*) or use *Set as wallpaper* in Photos - it shows up there too. The choice is stored in `/etc/wallpaper`, so on an installed disk it survives reboots (run `sync`, or just shut down).

Large photos are downscaled with area averaging (sharp, no aliasing) - a 3840x2160 JPEG decodes and applies in about a second in QEMU.

## Filesystem Layout

Banana OS seeds a small Unix-style root hierarchy at boot (in-memory, reset on reboot unless installed):

```
/
├── bin/
├── etc/            (motd, hostname, passwd, wallpaper; pkg/sources.list: the package repositories)
├── home/
│   └── banana/     ($HOME - shell starts here, readme.txt)
│       └── Pictures/  (your wallpapers)
├── root/
├── usr/
├── var/            (lib/pkg/lists: the repositories' package lists; cache/pkg: App Store pictures)
├── tmp/
├── dev/
├── proc/           (made when read, as on Linux: cpuinfo, meminfo, stat, uptime, loadavg, version, mounts, self/, <pid>/ ...)
└── sys/devices/system/cpu/  (each processor's topology/ and cpufreq/)
```

`/proc` is made when it is read and never saved: `cpuinfo` lists every processor thread (`processor`, with `siblings`, `core id` and `cpu cores` saying which ones share a core), `meminfo`, `stat` (each core's busy and idle time), `uptime`, `loadavg`, `version`, `mounts`, `cmdline`, `sys/kernel/hostname`, and a folder for each process (`stat`, `status`, `cmdline`, `comm`, `statm`, `exe`; `self` is your own). It is read-only. Task Manager's CPU page also shows the cores and the logical processors (threads), with a graph for each (*Show each core*).

Every path-taking command accepts absolute (`/etc/motd`), relative (`../etc`), `.`/`..`, and `~` (home) paths, just like a real Unix shell.

## Shells

Banana OS ships two shell **personas** built on one shared command engine (same builtins, same filesystem, same history) - only the prompt, banner, and a handful of bash-only builtins differ:

| | `sh` (stock, default) | `bash` (opt-in) |
|---|---|---|
| Prompt | `banana` (yellow) `@banana-os-0.5` (green) `:path$ ` | `banana@banana-os-0.5` (all green) `:path$ ` |
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


## Users and administrator rights

Banana OS has users the Unix way: `/etc/passwd`, `/etc/group`, `/etc/shadow` (root's only) and `/etc/doas.conf`. **banana** is the first user and an **administrator** - a member of the `wheel` group, whom `doas.conf` lets act as root (`permit persist :wheel`). **root** has no login and no password (it is locked); its rights come through `doas`.

- **More users**: Settings > Users, or `adduser [--admin] <name>`. Each gets a group of its own, `/home/<name>` (theirs only, 0700) and the `users` group. `deluser`, `usermod --admin / --no-admin` (or `-aG wheel`), `lsusers`; `passwd` sets your own password (the current one is asked), `passwd <user>` someone else's (an administrator's). The last administrator always stays one.
- **Files have owners**: a user writes their home, `/tmp` (sticky: only what is theirs goes), USB sticks; the system's folders (`/bin`, `/etc`, `/apps`, `/usr`) are root's. `id`, `groups` and `whoami` say who you are.
- **In a terminal**: `doas <command>` runs it as root - your password is asked, then remembered for a while in that terminal (`doas -L` forgets it); `doas -s` a root shell; `su <user>` becomes another user (their password). The system's commands that change the system - `pkg install`, `apt upgrade`, `update`, `ifconfig eth0 ...`, `hostname <name>`, `adduser`... - ask the same way by themselves. A standard user is told to ask an administrator.
- **On the desktop**: when an app (the App Store) or Settings needs an administrator, a small window comes up in the bottom-right corner, over the others - not over the whole screen, nothing else is stopped. An administrator answers **Yes / No** (Settings > Users > Administrator prompt: *ask for the password* instead); a standard user picks an administrator and types that one's password. On a Settings page that changes the system (Network, Date & time, Startup, Apps, System update, Users) the first change asks, then the page stays unlocked while it is open.
- **How long it is remembered**: Settings > Users > *Remember it for* - every time, 1, 5 (the default), 15 or 60 minutes.
- **No password set**: an administrator with no password is never asked - so a computer used as before users (the live CD, an install that never set one) works as it always did. Installing Banana OS from the live CD needs no permission; `update` on an installed system does.
- **Logging in**: with several users, or a password, the login screen shows the users to pick (a click, Tab or the arrows); Start > Shut down > *Switch user* ends your apps and shows it again. SSH logs in any user that has a password.
- **In Files**: Files and the desktop act with your rights. Creating, renaming, deleting, pasting or dropping something your rights refuse (a system folder, another user's file) brings up the administrator prompt; once allowed, only the refused items are done again - what already worked is not copied twice, a refused copy leaves nothing half-done, and a move that copied but could not delete the original only deletes it.
- **Older apps and `/home/banana`**: apps made before users (API 34 and older) often write to `/home/banana` by name. For another user they are sent to that user's own home instead (`/home/banana/notes.txt` becomes `/home/alice/notes.txt`). Settings > Users > *Older apps: /home/banana is your own home*, or `homecompat on|off`, turns it on or off; `homecompat skip <app>` leaves one app alone (`unskip` undoes it). Apps made with SDK 35 are not redirected (set `BANANA_LEGACY=1` to test an app as an older one).
- **SSH** starts once any user has a password (`passwd`) - not only banana - and logs in as that user.
- **What it does not cover yet**: on the 32-bit kernel apps run in ring 0, so the rights are advisory there.

## Installing to a Hard Disk (`install` / `sync`)

By default Banana OS boots from the CD/ISO every time and its filesystem is in-memory only, reset on every reboot. `install` does a real install onto a **second, dedicated IDE or SATA hard disk** attached to the VM (never the boot CD - optical drives are detected and skipped; `disks` shows what was found):

```bash
# QEMU: create a blank disk image (256 MB or more: the first 128 MB hold the
# boot image, the rest of the disk holds your files)
qemu-img create -f raw disk.img 1G
qemu-system-i386 -cdrom Banana_OS.iso -m 256 -nic user,model=e1000 -drive file=disk.img,format=raw,if=ide
```

In VirtualBox or VMware, attach a second blank virtual hard disk (IDE or SATA, 256 MB or more) to the same VM that boots `Banana_OS.iso`.

SATA (AHCI) works the same way - in QEMU with the q35 machine, whose disk controller is AHCI:

```bash
qemu-system-x86_64 -machine q35 -m 256 -cdrom Banana_OS.iso -drive file=disk.img,format=raw,if=none,id=d0 -device ide-hd,drive=d0,bus=ide.1
```

The easiest way is the **installer** on the live CD: start the desktop and open **Install Banana OS** (or type `installer`). It shows the disk it found, asks for the keyboard layout, a password and what starts at boot (desktop, login screen, web and SSH servers), then installs with a progress bar - or, when the disk already holds Banana OS, offers to update it and keep its files. It is only on the live CD.

Or, inside Banana OS:

```
install     # copies the boot image onto the disk and writes the current filesystem to it
sync        # saves right away (changes are also saved by themselves, ~2 s after each one)
```

`install` works because `Banana_OS.iso` is a "hybrid" image - a CD and a disk at once, like Linux live ISOs that can be `dd`'d straight onto a USB stick: its first sector is Banana Boot's MBR, with one partition, the EFI system partition. `install` raw-copies that already-bootable image from the CD (or the USB stick) onto the target disk, with the boot loaders' `set banana_medium=live-cd` line changed to `install` (so the kernel knows which copy started), then writes the filesystem after it.

Disk layout: the first **128 MB** are the boot area (room for the system to grow); the rest of the disk holds your files, each in a place of its own, with an index of them (names, folders, where each file is, a checksum) at the end of the disk. A save writes **only the files that changed** - each into a new place, never over the saved copy - and then the index, in the one of its two copies that does not hold the newest: a power cut during a save leaves the previous save intact. Renaming, moving or deleting writes nothing but the index. Saving happens in the background in 64 KiB pieces with the other tasks running in between, so the desktop never freezes while it saves. (Changing one line in a small file with 33 MB of other files on the disk: 5.9 s of frozen system before, a few ms now.) Disks installed by earlier versions are moved to this layout at their first boot, the old copy kept until the new one is complete. Settings > About, `sync` and `df` show how much of the disk your files use. A file is read from the disk the first time it is used and kept in RAM as a cache: when memory runs low, saved files not used for a while are dropped from RAM again, and big files are read in pieces (apps, `play`) - the RAM does not limit how much you can store or play. When the index outgrows its place at the end of the disk (thousands of files), it is written among the files, copy-on-write like them. Up to 65,536 files and 16,384 folders; disks up to 2 TB.

Once installed, the disk boots Banana OS **on its own** - drop `-cdrom Banana_OS.iso` entirely:

```bash
qemu-system-i386 -m 256 -nic user,model=e1000 -drive file=disk.img,format=raw,if=ide
```

Every boot after that loads the filesystem back from disk instead of reseeding the defaults, so your files persist too.

Started from the CD, Banana OS is always the **live** system - fresh, with nothing loaded from or saved onto an installed disk ("Running the LIVE CD" at boot and a LIVE CD badge on the taskbar).

**Updating an install:** boot the new CD with the installed disk attached and type `update`. It compares the disk's system with the CD's and rewrites only the system - your files, settings and password stay. Installs made with the earlier layout (a 32 MB boot area) are moved to the current one on the way. (`install` would erase the disk; it says so when the disk already holds Banana OS.)
