/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "help.h"
#include "../kernel/terminal.h"
#include "../kernel/keyboard.h"
#include "../kernel/kstring.h"
#include "../kernel/kheap.h"
#include "../kernel/sysinfo.h"
#include "../kernel/task.h"
#include "../kernel/tty.h"
#include "../kernel/gui.h"

/*
 * Every command's page. A page is: the topic it belongs to, the synopsis,
 * a one-line summary (what `help <topic>` and `help -a` show), a longer
 * description (paragraphs, wrapped to the terminal), the options (one
 * "option|meaning" per line), examples (one per line, "command  # note")
 * and related commands. Keeping them in one table is what lets `help -k`
 * search them and `help --check` find a command without a page.
 */

typedef struct {
    const char* key;
    const char* title;
} topic_t;

static const topic_t TOPICS[] = {
    { "files",    "Files and folders" },
    { "text",     "Reading and editing text" },
    { "disks",    "Disks and filesystems" },
    { "system",   "System, processes and power" },
    { "network",  "Network" },
    { "servers",  "Servers and remote access" },
    { "apps",     "Apps and packages" },
    { "desktop",  "The desktop" },
    { "hardware", "Hardware and devices" },
    { "sound",    "Sound" },
    { "shell",    "The shell itself" },
    { "vm",       "Virtual machines and tests" },
};
#define NTOPICS ((int)(sizeof(TOPICS) / sizeof(TOPICS[0])))

typedef struct {
    const char* name;
    const char* topic;
    const char* synopsis;
    const char* brief;
    const char* desc;
    const char* opts;
    const char* examples;
    const char* see;
} page_t;

static const page_t PAGES[] = {
    /* ── files ── */
    { "ls", "files", "ls [-l] [path]", "list a folder",
      "Lists the folders (in blue, with a slash) and files in path, or in the current folder. "
      "With -l, one line each: the mode, owner, group, size, the date modified (the time this year, "
      "the year for older files) and the name; a symbolic link shows where it points.",
      "-l|long format: mode, owner, group, size, date, link targets",
      "ls\nls -l /etc\nls ~/Downloads", "cd, find, stat, chmod" },
    { "cd", "files", "cd [path]", "change the current folder",
      "Changes the current folder. With no argument, or ~, goes home (/home/banana); .. goes up one; "
      "paths may be absolute (/etc), relative (../x) or start with ~. Symbolic links to folders are followed.",
      NULL, "cd /etc\ncd ..\ncd ~/Pictures", "pwd, ls" },
    { "pwd", "files", "pwd", "print the current folder",
      "Prints the full path of the current folder.", NULL, NULL, "cd" },
    { "mkdir", "files", "mkdir [-p] <folder>", "make a folder",
      "Makes a folder. With -p, the folders above it are made too where they are missing, and an "
      "existing folder is not an error. Needs write permission on the folder it is made in.",
      "-p|make the parents too", "mkdir notes\nmkdir -p ~/projects/banana/src", "rm, ls" },
    { "rm", "files", "rm [-r] <path>", "delete a file or a folder",
      "Deletes a file, or with -r a folder and everything in it. A symbolic link is deleted itself, "
      "not what it points at. Files are gone for good (the desktop's Delete uses the Recycle Bin instead).",
      "-r|a folder and all it holds", "rm old.txt\nrm -r ~/tmp/build", "mkdir, mv" },
    { "touch", "files", "touch <file>", "make an empty file, or update its date",
      "Makes the file if it is not there; otherwise sets its date modified to now.", NULL,
      "touch notes.txt", "stat, ls" },
    { "cp", "files", "cp <source> <destination>", "copy a file",
      "Copies a file. When destination is a folder, the copy gets the same name inside it. A symbolic "
      "link is followed: what it points at is copied. Copies keep the original's date modified.",
      NULL, "cp notes.txt notes.bak\ncp /etc/motd ~/", "mv, rm" },
    { "mv", "files", "mv <source> <destination>", "move or rename a file or folder",
      "Moves or renames a file or a folder. Onto another volume (a USB stick) a file is copied, then "
      "deleted. A folder stays on its own volume.", NULL,
      "mv draft.txt final.txt\nmv photo.jpg ~/Pictures/", "cp, rm" },
    { "find", "files", "find [path] [-name <part>]", "list everything under a folder",
      "Lists every folder and file under path (the current folder by default), one path a line; "
      "with -name, only the ones whose name contains part.", "-name part|only names containing part",
      "find /etc\nfind ~ -name .txt", "ls, grep" },
    { "chmod", "files", "chmod <mode> <file>...", "change permissions",
      "Sets the permissions of files and folders: an octal mode (755, 0640) or a symbolic one "
      "(u+x, go-w, a=r, u=rwx,g=rx,o=). Banana OS checks the owner's bits: a file without w cannot "
      "be changed, a folder without w cannot have things made, deleted or renamed in it. "
      "FAT32 and exFAT volumes keep no permissions.", NULL,
      "chmod 600 ~/secret.txt\nchmod u+x ~/bin/hello.sh\nchmod a-w ~/keep", "chown, stat, ls" },
    { "chown", "files", "chown <owner>[:<group>] <file>...", "change the owner (and group)",
      "Sets the owner, and with :group the group, of files and folders: root, banana or a number. "
      "chown :group changes only the group.", NULL, "chown root:root /etc/motd\nchown banana ~/x", "chgrp, chmod, stat" },
    { "chgrp", "files", "chgrp <group> <file>...", "change the group",
      "Sets the group of files and folders: root, banana or a number.", NULL, "chgrp root /etc/hosts", "chown" },
    { "ln", "files", "ln -s [-f] <target> <link>", "make a symbolic link",
      "Makes link, a symbolic link to target (stored as written: relative targets are relative to the "
      "link's folder). Opening, cd, cat and cp follow it; rm and mv act on the link. When link is a "
      "folder, the link goes inside it with the target's name. Hard links are not supported.",
      "-s|symbolic (required)\n-f|replace an existing file", "ln -s /etc etclink\nln -s notes.txt ~/latest", "readlink, stat" },
    { "readlink", "files", "readlink <link>", "print where a symbolic link points", NULL, NULL,
      "readlink ~/latest", "ln" },
    { "stat", "files", "stat [-L] <path>...", "everything about a file or folder",
      "Shows a file's or folder's type, size, blocks, index, permissions (octal and rwx), owner, group "
      "and its three dates: access, modify and birth. A link is described itself unless -L is given.",
      "-L|follow symbolic links", "stat /etc/motd\nstat -L ~/latest", "ls, chmod" },
    { "zip", "files", "zip [-r] [-q] [-0] <archive>[.zip] <file|folder>...", "pack files and folders into a ZIP archive",
      "Makes a new ZIP archive (an existing one is replaced) of the files and folders given, folders with "
      "everything in them, each under its own name. Files are compressed (deflate) unless that would not "
      "make them smaller. Archives open on Windows, macOS and Linux; up to 4 GB and 65535 entries.",
      "-r|folders with all they hold (always done)\n-q|quiet: only the summary\n-0|store, no compression",
      "zip photos.zip ~/Pictures\nzip -0 backup.zip notes.txt todo.txt", "unzip" },
    { "unzip", "files", "unzip [-l] [-o] [-q] <archive>.zip [-d <folder>]", "extract a ZIP archive",
      "Extracts every file and folder of a ZIP archive into the current folder, or the one -d gives (made "
      "if needed). Files already there are kept unless -o is given. Names that would land outside the "
      "folder (\"../\", absolute) and encrypted or ZIP64 entries are skipped; every file's CRC is checked. "
      "In Files, double-click an archive (or right-click > Extract) to extract it.",
      "-l|list the entries\n-o|replace files that are there\n-q|quiet: only the summary\n-d|the folder to extract to",
      "unzip photos.zip\nunzip -l photos.zip\nunzip /mnt/usb/game.zip -d ~/game", "zip" },
    { "screenshot", "files", "screenshot [directory]", "save the screen as PNG",
      "Capture the current graphical screen. Win+Shift+S captures the completed desktop frame. "
      "Names are numbered without replacing existing captures; the default folder is /home/banana/Pictures.",
      "directory|absolute output directory", "screenshot\nscreenshot /home/banana/Pictures", "startx" },
    { "clip", "files", "clip <text> | -p | -l | -c", "the clipboard: copy, print, its history",
      "clip <text> copies the text (quotes around all of it are dropped), as Ctrl+C would; -p prints the clipboard; "
      "-l lists the clipboard history (Win+V on the desktop: the last 25 texts copied, * pinned); -c clears it, "
      "the pinned entries stay.", "-p|print the clipboard\n-l|the history, the newest first\n-c|clear the history (not the pinned ones)",
      "clip hello world\nclip -l", "startx" },
    /* ── text ── */
    { "cat", "text", "cat <file>", "print a file",
      "Prints a text file. Binary files are refused (they would fill the screen with noise).", NULL,
      "cat /etc/motd", "head, tail, edit" },
    { "edit", "text", "edit <file>", "the text editor",
      "A nano-like editor in the terminal: the arrows move, Ctrl+O or Ctrl+S saves, Ctrl+X quits, "
      "Ctrl+K cuts a line and Ctrl+U pastes it. On the desktop, Notepad does the same with the mouse.",
      NULL, "edit ~/notes.txt", "notepad, cat, nano" },
    { "nano", "text", "nano [file]", "GNU nano, the friendly editor",
      "GNU nano 8.7, the friendly terminal editor, with syntax colouring for many languages: Ctrl+O saves, Ctrl+X "
      "quits, Ctrl+W searches, Ctrl+K / Ctrl+U cut and paste. Comes with the 64-bit system. Settings: ~/.nanorc.",
      NULL, "nano ~/notes.txt", "vim, vi, nvim, edit" },
    { "vim", "text", "vim [file]", "Vim, the programmer's editor",
      "Vim 9.2 with its whole runtime: syntax colouring, file type plugins and indenting, colour schemes, :help. "
      "Also vimdiff and view. Comes with the 64-bit system. Settings: ~/.vimrc.",
      NULL, "vim ~/main.c", "vi, nvim, nano" },
    { "vi", "text", "vi [file]", "vi, the classic Unix editor",
      "Independent UTF-8 vi/ex editor (Neatvi), separate from Vim. Comes "
      "with the 64-bit system.", NULL, "vi /etc/motd", "vim, nvim, nano" },
    { "nvim", "text", "nvim [file]", "Neovim (optional: apt install neovim)",
      "Neovim 0.12: Lua configuration and plugins (~/.config/nvim/init.lua), tree-sitter (its parsers for C, Lua, "
      "Vim script, help, queries and Markdown built in), :help, a terminal in a window (:terminal). Installed when "
      "asked for: apt install neovim.", NULL, "apt install neovim\nnvim ~/init.lua", "vim, vi, nano" },
    { "grep", "text", "grep [-n] <text> <file>", "print the lines holding a text",
      "Prints the lines of file that contain text (a plain substring). -n puts the line number in front.",
      "-n|line numbers", "grep -n banana /etc/passwd", "find, wc" },
    { "wc", "text", "wc <file>", "count lines, words and bytes", NULL, NULL, "wc ~/notes.txt", "grep" },
    { "head", "text", "head [-n N] <file>", "the first lines of a file",
      "Prints the first N lines (10 by default).", "-n N|how many lines", "head -n 3 /etc/passwd", "tail, cat" },
    { "tail", "text", "tail [-n N] <file>", "the last lines of a file",
      "Prints the last N lines (10 by default).", "-n N|how many lines", "tail -n 20 /var/log/x", "head, cat" },
    { "echo", "text", "echo <text>", "print text",
      "Prints its arguments. $NAME is replaced by the variable's value; with > or >> the text goes into a file.",
      NULL, "echo hello\necho $HOME\necho line >> notes.txt", "export" },
    /* ── disks ── */
    { "df", "disks", "df [-h|-k|-m] [-i]", "disk space",
      "The size, used and free space of / (the installed disk) and of every mounted volume. -i shows "
      "files and folders instead of bytes.", "-h|sizes like 1.2G\n-k|kilobytes\n-m|megabytes\n-i|files and folders",
      "df -h\ndf -i", "mount, disks, free" },
    { "disks", "disks", "disks", "the drives",
      "Lists the drives: IDE, SATA (AHCI), NVMe and virtio disks and CD/DVD drives, with their size and model.",
      NULL, NULL, "install, mount, df" },
    { "lsblk", "disks", "lsblk", "the drives, their partitions and where they are mounted",
      "Lists every drive - the installed disk, CD drives, extra IDE / SATA / NVMe / virtio / SCSI disks, eMMC, USB "
      "sticks, SD cards - and under each its partitions (MBR primary and logical, or GPT) with their "
      "size, filesystem, mount point and type. Names like sata1p2 are what mount and umount take.",
      NULL, "lsblk", "mount, umount, df, disks" },
    { "mount", "disks", "mount [-a] | mount <drive>[p<N>] [folder]", "mounted volumes, or mount one",
      "Without arguments, lists the mounted volumes (FAT32, exFAT) with their device, size and mount point "
      "(/mnt/usb, /mnt/sata, /mnt/nvme2...) and the drives not mounted. Every volume of every extra drive "
      "is mounted by itself - each partition gets its own folder. mount <drive> mounts a drive's volumes "
      "again after umount; mount <drive>p<N> [folder] mounts one partition, where you say.",
      "-a|retry every unmounted drive", "mount\nmount sata1\nmount sata1p2 /mnt/data", "lsblk, umount, df" },
    { "umount", "disks", "umount [path|drive]", "unmount a volume or a whole drive",
      "Unmounts the volume mounted at path (the only one when nothing is given), or with a drive's name "
      "(usb0, sata1) all of its volumes, so it can be pulled out. eject does the same.",
      NULL, "umount /mnt/usb\numount sata1", "mount, lsblk" },
    { "eject", "disks", "eject [path]", "unmount a volume (same as umount)", NULL, NULL, NULL, "umount" },
    { "fsck", "disks", "fsck [-f] [-y]", "check the filesystem",
      "Checks the filesystem in two passes: the tree in memory (every folder reachable from /, every "
      "file in a folder, names found again, the folder lists whole) and the installed disk (both "
      "metadata copies, the extents inside the files' area and not over each other, every saved record "
      "belonging to a file). Unsaved changes are written first. e2fsck is the same command.",
      "-f|also read every file back against its CRC-32C\n-y|repair the tree (lost entries go to /lost+found)\n-n|check only (the default)",
      "fsck\nfsck -f", "sync, df" },
    { "e2fsck", "disks", "e2fsck [-f] [-y]", "the same as fsck", NULL, NULL, NULL, "fsck" },
    { "sync", "disks", "sync", "write to the installed disk now",
      "Writes every changed block to the installed disk (ext4) at once, as a journal transaction. Changes "
      "go to the disk by themselves within half a second; sync is for right now.", NULL, NULL, "fsck, install, drop_caches" },
    { "drop_caches", "disks", "drop_caches", "give back the RAM holding the disk's data",
      "As Linux's /proc/sys/vm/drop_caches: the file data and disk blocks kept in RAM, and the entries of "
      "folders nobody is in (no file open there, not the current folder) go - all of it is on the disk "
      "and read again when used. It happens by itself too, when RAM runs low.", NULL, NULL, "free, sync" },
    { "fstest", "disks", "fstest [-k] <mount point>", "test a FAT32 or exFAT drive",
      "Checks a mounted FAT32 or exFAT drive end to end, in a folder of its own (fstest/): a folder, a small "
      "file and an append, 300 KiB plus 100 KiB appended, a rename (to a Unicode name on exFAT), a sub-folder "
      "and a move, 300 files in one folder and 150 of them deleted; then it unmounts the drive, mounts it again "
      "and reads everything back from the drive itself, every byte. Works on real sticks and SD cards. One "
      "ok / FAIL line a check, then the number failed. All the tests: docs/TESTING.md.",
      "-k|keep the test folder afterwards", "fstest /mnt/usb\nfstest -k /mnt/virtio", "mount, lsblk, selftest" },
    { "install", "disks", "install [number] [--full|--standard|--minimal]", "install Banana OS on a disk (live CD)",
      "Copies Banana OS from the CD or USB stick onto a hard disk and writes the current files to it: the "
      "disk then boots by itself, with files saved. Everything on that disk is erased; the other disks are "
      "left alone. In three steps: the disks are listed with a number and, with several, it asks which one "
      "(or give it: install 2); then the warning (with the apps it puts on), answered y or n; then the "
      "install. The apps left out by --standard or --minimal stay installable from the App Store (and apt). "
      "Only on the live system. On the desktop, Install Banana OS is the graphical way.",
      "--full|every app and the examples (the default)\n"
      "--standard|App Store, Media Player, Music, Photos\n"
      "--minimal|the App Store only",
      "install\ninstall --full      # the same as install alone\ninstall 2 --standard\ninstall --minimal",
      "installer, update, lsblk, apt" },
    { "installer", "disks", "installer", "the graphical installer (live CD)", NULL, NULL, NULL, "install" },
    { "update", "disks", "update [number]", "update an installed system from a newer CD",
      "Writes this CD's system over the installed one; your files, settings and password are kept. With "
      "several disks holding Banana OS it asks which one (or give its number: update 2), then asks to "
      "confirm, then updates. Only on the live system.",
      NULL, NULL, "install" },
    /* ── system ── */
    { "uname", "system", "uname", "the system's name", NULL, NULL, NULL, "neofetch" },
    { "whoami", "system", "whoami", "the user", NULL, NULL, NULL, "passwd" },
    { "hostname", "system", "hostname [NAME]", "show or change the computer's name",
      "Sets the PC name immediately and saves it in /etc/hostname. Use 1-63 letters, digits or hyphens, "
      "starting and ending with a letter or digit. Names are lowercased. New terminals use the new name; "
      "DHCP sends it on its next request. Live-session changes last until reboot unless installed.",
      NULL, "hostname\nhostname banana-laptop", "settings" },
    { "date", "system", "date [-u | -a] | date set [YYYY-MM-DD] [HH:MM[:SS]] | date utc on|off",
      "the date and time; set the clock by hand",
      "Shows the local time - the time zone's (see timezone) - with the zone and whether it is summer time. "
      "date set sets the clock: a date (2026-10-17, or 17/10/2026), a time (18:30 or 18:30:15), or both; "
      "the part not given stays as it is. The computer's clock chip keeps the local time (as Windows does: a "
      "PC that also starts Windows shows the same time in both) or UTC (as Linux does): date utc on|off "
      "changes which, keeping the time. Settings > Date & time does the same with the mouse.",
      "-u|the time in UTC\n-a|local time, UTC, and what the clock chip keeps",
      "date set 2026-10-17 18:30\ndate set 9:05\ndate utc on", "timezone" },
    { "language", "desktop", "language [en | fr | de | it | rm]", "the desktop's language",
      "English (the default), French (fr), German (de), Italian (it) or Romansh (rm): menus, windows, Settings, the installer. A text not "
      "translated yet shows in English; commands stay in English. Settings > Language does the same; apps built "
      "with the SDK read it with banana_language().",
      NULL, "language fr\nlanguage en", "keyboardctl" },
    { "theme", "desktop", "theme [list | set NAME | show [NAME] | install FILE | new NAME [FILE] | check FILE | taskmgr STYLE]",
      "the desktop's colours",
      "A theme colours windows, the taskbar, the Start menu, menus and the built-in apps, and may bring a wallpaper. "
      "Built in: Banana Dark, Light, Midnight, Ocean, Forest, Banana, Sakura, High Contrast. theme new writes a copy "
      "of the theme in use to change in Notepad; theme check finds mistakes and hard-to-read colours; theme install "
      "takes a .theme or a .btheme (a ZIP with its picture). Settings > Themes does the same with the mouse. On Linux, "
      "sdk/tools/bananatheme makes, checks, previews and packs themes (sdk/themes/README.md). A theme also picks Task Manager's look - Banana OS, Windows classic, Windows 10 or Windows 11; theme taskmgr STYLE (or Settings > Themes) chooses another, theme taskmgr theme goes back to the theme's.",
      NULL, "theme set Ocean\ntheme new Mine\ntheme install Mine.theme\ntheme taskmgr classic", "settings" },
    { "cal", "system", "cal [month [year]] | cal year", "a month as a calendar",
      "A month as a calendar, weeks from Monday, today marked: this month with nothing, another with its "
      "number (and year), a whole year with only the year. On the desktop, a click on the taskbar's clock "
      "opens the same calendar in a panel.",
      NULL, "cal\ncal 2 2028\ncal 2027", "date" },
    { "timezone", "system", "timezone [list [words] | <zone or place>]", "the time zone",
      "With nothing, the zone in use and its offset from UTC. timezone list shows every zone (words: only "
      "those naming them); timezone Europe/Paris, or a place - timezone tokyo - sets it: the time does not "
      "change, the local time does. Summer time comes and goes by itself, by the zone's rule (the EU's, the "
      "USA's, Australia's...). Apps get it too: localtime(), strftime %z / %Z; Java reads /etc/timezone.",
      NULL, "timezone list europe\ntimezone new york\ntimezone UTC", "date" },
    { "uptime", "system", "uptime", "how long it has been running, the load average", NULL, NULL, NULL, "top" },
    { "boottime", "system", "boottime", "where the boot's time went, step by step",
      "Each step of the boot - the kernel's (heap, graphics, timer, cores, network, disks...), the shell's (the "
      "installed disk, apps, servers), the login and the desktop's first frame - with when it started and how "
      "long it took; the three longest are highlighted. Before the kernel, the firmware, the boot menu and "
      "the loading took about as long as the first line says (from the processor's time stamp counter).",
      NULL, NULL, "uptime" },
    { "neofetch", "system", "neofetch", "a summary of the system, with the Banana OS logo",
      "On the 64-bit system this is the real neofetch (7.1.0, a bash script) with a Banana OS backend: it reads "
      "sysinfo, as Banana OS has no /proc. Its options work as on Linux (neofetch --help).", NULL,
      "neofetch\nneofetch --off", "sysinfo, hw_info" },
    { "sysinfo", "system", "sysinfo", "the system's facts as 'key: value' lines, for scripts",
      "os, version, arch, boot, cpu, threads, memory_used_kb, memory_total_kb, gpu, resolution, uptime_s, packages, desktop.",
      NULL, "sysinfo\nsysinfo | grep cpu", "neofetch, uname" },
    { "sleep", "system", "sleep [status | SECONDS]", "ACPI S3 suspend to RAM",
      "Sleep retains the session in RAM. The first supported profile is QEMU PC/PIIX4 with standard VGA, e1000 and optional virtio-blk. Unsupported platforms/devices are refused before suspend. Files are synced first. Use the VM wake/power control to resume, or specify 1..3600 seconds for an RTC wake. This is not hibernation.",
      "status|show whether the current platform can suspend\nSECONDS|arm an RTC wake after this many seconds",
      "sleep status\nsleep 5\nselftest sleep\nselftest sleep cycle", "selftest, shutdown, reboot" },
    { "top", "system", "top", "a live view of the processor, memory and tasks", "q quits.", NULL, NULL, "htop" },
    { "htop", "system", "htop", "the colour monitor",
      "A meter per processor core, memory and disk, then the tasks. P sorts by processor use, N by number, "
      "T by time; q quits.", NULL, NULL, "top, sched" },
    { "free", "system", "free [-h|-k|-m|-g]", "memory: total, used, free",
      "The RAM: total, used, free, and the file data kept in RAM that could be given back.", NULL, "free -h", "ram_info, df, swap" },
    { "swap", "system", "swap [on | off | now | disk]", "swap: compressed memory and the swap area on the disk",
      "When RAM runs low, the pages of apps' memory that were not used lately (their mmap memory: a Java "
      "heap...) are compressed in RAM and their memory given back to the system; a page is decompressed "
      "the moment it is touched again. Pages of zeros take no room. On an installed system, when that is "
      "not enough (or much is compressed), the oldest compressed pages go to a swap area on the disk - "
      "taken at boot from the top of its free space, as big as the RAM (4 GB at most) - and are read back "
      "when touched. swap shows what is compressed and on the disk; on / off turns it on or off (on by "
      "default); now compresses every page not in use right now; disk writes the compressed pages out.",
      "on|swap when RAM runs low (the default)\noff|nothing more compressed or written\nnow|compress what can be, now\n"
      "disk|write the compressed pages to the swap area now", "swap\nswap now", "free" },
    { "latency", "system", "latency [on|off|status]", "measure boot-core input and IRQ latency",
      "Opt-in diagnostics written to the serial log. Measures IRQ-disabled sections, handler time, "
      "timer intervals, keyboard queue delay and drops. Uses the shared hardware clock; timing samples "
      "are unavailable with the IRQ-only fallback. p95 values are histogram upper bounds.",
      "on|start a fresh session\noff|stop and report\nstatus|report current sample", "latency on\nlatency status\nlatency off", "sched" },
    { "sched", "system", "sched [calls [reset]]", "the scheduler's view of the tasks",
      "App threads run on the other cores; most system calls are done on the boot core (the thread goes there and "
      "back). sched calls counts, per call, those done on the thread's own core and those sent to the boot core - "
      "what keeps the boot core busy (reset: start counting again).", NULL, "sched\nsched calls reset\nsched calls",
      "nice, renice, top" },
    { "nice", "system", "nice [-n N] <command>", "run a command at another priority",
      "Runs command with its nice value raised by N (10 by default; negative gets more processor time). "
      "nice alone shows the current value.", "-n N|the change", "nice -n 5 fstest /mnt/usb", "renice" },
    { "renice", "system", "renice [-n] N <pid>...", "change a running task's priority",
      "Sets the nice value (-20 most processor time .. 19 least) of running tasks.", NULL, "renice 10 7", "nice, sched" },
    { "time", "system", "time <command>", "how long a command takes", NULL, NULL, "time fsck -f", NULL },
    { "shutdown", "system", "shutdown [now|-c]", "power off",
      "Saves the files and turns the computer off (ACPI) in 60 seconds, now, or cancels a pending shutdown with -c.",
      NULL, "shutdown now", "reboot" },
    { "reboot", "system", "reboot", "restart now (files saved first)", NULL, NULL, NULL, "shutdown" },
    { "halt", "system", "halt", "stop the processor (no power-off)", NULL, NULL, NULL, "shutdown" },
    { "passwd", "system", "passwd [-d]", "set the password",
      "Sets the password of banana, asked at boot (unless autologin is on), on the lock screen and for "
      "SSH and Remote Desktop. -d removes it.", "-d|remove the password", NULL, "autologin, sshd" },
    { "autologin", "system", "autologin [on|off]", "log in at boot without the password", NULL, NULL, NULL, "passwd" },
    { "proc_info", "system", "proc_info [-c -t -n -v -f -m -s -ht]", "the processor", NULL, NULL, NULL, "lscpu" },
    { "ram_info", "system", "ram_info [-t -u -h -f -p -m]", "memory details (-h: the kernel heap)", NULL, NULL, NULL, "free" },
    { "hw_info", "system", "hw_info [-c -m -g -u -k -r]", "a hardware summary", NULL, NULL, NULL, "lscpu, lsgpu" },
    { "lscpu", "system", "lscpu", "processors: cores, threads, packages, each one's load", NULL, NULL, NULL, "proc_info" },
    { "crashtest", "system", "crashtest [preview|report|panic|ud2|pagefault]", "the crash screen",
      "Tests the kernel crash screen (stop code, registers, faulting address, stack words; the full report "
      "also goes to COM1). preview (the default) shows it safely - Enter or Esc returns; report prints a "
      "sample report here. panic, ud2 (an invalid opcode) and pagefault (a read of page zero, 64-bit kernel) "
      "really stop the system - save your work first.",
      "preview|the screen, safely\nreport|a sample report in the terminal\npanic|a kernel panic (halts)\n"
      "ud2|an invalid-opcode exception (halts)\npagefault|a page fault (halts; 64-bit)",
      "crashtest\ncrashtest report", "selftest" },
    { "kptest", "system", "kptest <picture> <drawing.svg> [seconds]", "check kernel preemption", NULL, NULL, NULL, "selftest" },
    /* ── network ── */
    { "ntp", "network", "ntp [--set] [server]", "query network UTC, optionally set the clock",
      "Query SNTP using the native UDP stack (default pool.ntp.org). Without --set the clock is unchanged. "
      "--set changes UTC explicitly; this is a step, not gradual discipline. Ordinary NTP is unauthenticated; "
      "the local whole-second clock makes the offset approximate.", "--set|set UTC after a validated reply",
      "ntp\nntp --set pool.ntp.org", "date" },
    { "ifconfig", "network", "ifconfig [priority [auto|ethernet|wifi] | <if> <ip> [netmask m] [gw g] [dns d] | <if> up | reset]", "network settings",
      "Shows the interfaces, or sets one up with a fixed address (saved in /etc/network.conf). "
      "ifconfig eth1 up pins a card; priority auto/ethernet/wifi selects connected links with DHCP. Auto prefers Ethernet. Changing priority removes a pin/static address. A preferred link must stay ready for 3 seconds; existing connections may need to reconnect. ifconfig reset restores defaults.", NULL,
      "ifconfig\nifconfig eth0 192.168.1.50 netmask 255.255.255.0 gw 192.168.1.1 dns 1.1.1.1", "dhcp, ping" },
    { "wifi", "network", "wifi [scan [IF] | networks [IF] | connect IF SSID --password-file FILE | connect IF SSID --open | disconnect IF]", "Wi-Fi discovery and WPA2 connection",
      "Initial experimental hardware driver: MediaTek MT7601U, 2.4 GHz channels 1-11, legacy b/g. Run wifi scan, then connect using a password file or Settings > Wi-Fi. Passwords are not placed on the command line. No WEP/TKIP/WPA3/PMF-required support yet. Network priority remains in ifconfig.",
      NULL, "wifi scan\nwifi connect wlan0 TestNet --password-file /home/banana/wifi.secret", "ifconfig" },
    { "dhcp", "network", "dhcp", "ask for a new address (DHCP)", NULL, NULL, NULL, "ifconfig" },
    { "ping", "network", "ping [-c N] [-s size] [-i sec] <host>", "is that host there?",
      "Sends ICMP echo requests (4 by default) and prints the replies and times. Ctrl+C stops.",
      "-c N|how many\n-s size|bytes of data\n-i sec|between them", "ping example.com", "ping6, nslookup" },
    { "ping6", "network", "ping6 <host>", "ping over IPv6", NULL, NULL, NULL, "ipv6, ping" },
    { "nslookup", "network", "nslookup [-6] <host>", "look a name up in DNS (host is the same)", NULL, NULL, "nslookup example.com", "ping" },
    { "host", "network", "host <host>", "look a name up in DNS", NULL, NULL, NULL, "nslookup" },
    { "curl", "network", "curl [-L] [-o file|-O] [-I] [-i] [-v] [-s] [-A agent] <url>", "an HTTP and HTTPS client",
      NULL, "-L|follow redirects\n-o file|save to file\n-O|save under the URL's name\n-I|headers only\n-i|headers too\n-v|show the exchange\n-s|silent\n-6|over IPv6",
      "curl https://example.com\ncurl -O https://example.com/file.zip", "wget" },
    { "wget", "network", "wget [-q] [-O file] <url>", "download a file", NULL, "-O file|where to save\n-q|quiet",
      "wget https://example.com/pic.jpg", "curl" },
    { "netstat", "network", "netstat", "TCP connections", NULL, NULL, NULL, "arp" },
    { "arp", "network", "arp", "the ARP cache", NULL, NULL, NULL, "netstat" },
    { "ipv6", "network", "ipv6 [on|off]", "IPv6: addresses and router, or switch it", NULL, NULL, NULL, "toggle6, ping6" },
    { "toggle6", "network", "toggle6 [on|off]", "switch IPv6 (off by default, saved)", NULL, NULL, NULL, "ipv6" },
    { "firewall", "network", "firewall [on|off|allow|deny|ping|log]", "the firewall",
      "Off by default. On, connections from outside are refused except the ports allowed; what you start works as before.",
      NULL, "firewall on\nfirewall allow tcp 22\nfirewall log", "sshd, httpd" },
    { "cryptotest", "network", "cryptotest", "the TLS crypto self-tests", NULL, NULL, NULL, "selftest" },
    /* ── servers ── */
    { "sshd", "servers", "sshd [status|start [port]|stop|enable [port]|disable]", "the SSH server",
      "Lets others log in with ssh banana@<this machine> (port 22). enable also starts it at every boot.",
      NULL, "sshd start\nsshd enable", "passwd, firewall" },
    { "rdpd", "servers", "rdpd [status|start [port]|stop|enable [port]|disable]", "the Remote Desktop server",
      "Windows' Remote Desktop Connection, the Microsoft apps and FreeRDP connect to this desktop (port 3389).",
      NULL, "rdpd start", "rdp, passwd" },
    { "rdp", "servers", "rdp [-u user] [-d domain] <host>[:port]", "connect to a Remote Desktop server", NULL, NULL,
      "rdp -u admin 192.168.1.20", "rdpd" },
    { "httpd", "servers", "httpd [status|start [port]|stop|enable|disable|expose|unexpose]", "the web server",
      "Serves /var/www (port 80); .php pages run. expose forwards a port on your router (UPnP) to it.",
      NULL, "httpd start\nhttpd expose 8080", "firewall" },
    /* ── apps ── */
    { "pkg", "apps", "pkg install <file.bpk>... | list | info <app> | verify [app...] | remove <app> | run <app> | ps", "install and run apps",
      "Installs apps built with the SDK (.bpk files), lists, describes, removes and runs them. Installed "
      "apps also run by their name, and appear in Apps on the desktop. Each install records its files' sizes and "
      "checksums (/apps/<app>/.files); pkg verify checks every file against them and puts a damaged built-in app "
      "back (now on the live system, at the next start on an installed one). At every start the built-in apps' "
      "files are checked by size and put back if one is missing or cut short.", NULL,
      "pkg install ~/Examples/gears.bpk\npkg list\npkg verify\npkg verify mesa java", "apt, apps, update" },
    { "apt", "apps", "apt update | search <words> | show <app> | install <app>... | remove <app>... | upgrade | list | sources",
      "packages from repositories",
      "Like Debian's apt: package lists from signed repositories, search, install with what an app needs, "
      "upgrade, remove. apt add-repo <url> key=<hex> adds a repository. apt-get is the same.", NULL,
      "apt update\napt search game\napt install gears", "pkg, store" },
    { "apt-get", "apps", "apt-get ...", "the same as apt", NULL, NULL, NULL, "apt" },
    { "apps", "apps", "apps", "open Apps on the desktop", NULL, NULL, NULL, "pkg" },
    { "store", "apps", "store", "the App Store", NULL, NULL, NULL, "apt" },
    { "code", "apps", "code [folder|file]", "Banana Code, the code editor with a C compiler", NULL, NULL, NULL, NULL },
    { "mediaplayer", "apps", "mediaplayer [file]", "the Media Player (video and music)", NULL, NULL, NULL, "music" },
    { "music", "apps", "music [file...]", "the Music app", NULL, NULL, NULL, "amethyst" },
    { "amethyst", "apps", "amethyst [server]", "Amethyst Music: stream from a server", NULL, NULL, NULL, "music" },
    { "drivers", "apps", "drivers", "loaded drivers and driver modules", NULL, NULL, NULL, "driver" },
    { "driver", "apps", "driver load <file>", "load a driver module", NULL, NULL, NULL, "drivers" },
    /* ── desktop ── */
    { "startx", "desktop", "startx", "start the desktop",
      "Starts the desktop on this screen (not over SSH, nor from a terminal window of the desktop). start is the same.\n"
      "Windows: drag a title bar to a screen edge to snap it to a half (a corner: a quarter, the top: full screen); "
      "Win+Left / Right / Up / Down snap, maximize, restore; right-click a title bar for the window menu (always on "
      "top, move to another desktop); Alt+drag moves a window from anywhere. Four virtual desktops: the switcher on "
      "the taskbar, Win+Ctrl+Left / Right, Win+Ctrl+Shift+Left / Right takes the front window along. Alt+Tab, Win+Tab "
      "(task view), Win+D (the desktop), Win+V (the clipboard history: the last 25 texts copied - Enter pastes one, "
      "Delete removes it, P pins it to keep it), Win+. or Win+; (emoji: type to search, Tab changes the group, "
      "Enter or a click types the emoji into the window in front), Alt+F4.",
      NULL, NULL, "stopx" },
    { "stopx", "desktop", "stopx", "leave the desktop (stop is the same)", NULL, NULL, NULL, "startx" },
    { "start", "desktop", "start", "the same as startx", NULL, NULL, NULL, "startx" },
    { "stop", "desktop", "stop", "the same as stopx", NULL, NULL, NULL, "stopx" },
    { "files", "desktop", "files [folder]", "open Files, the file explorer", NULL, NULL, NULL, "ls" },
    { "browser", "desktop", "browser [--libjs|--default|--legacy] [address] | browser --engine", "open the web browser", NULL, NULL, "browser example.com", NULL },
    { "notepad", "desktop", "notepad [file]", "open Notepad", NULL, NULL, NULL, "edit" },
    { "notify", "system", "notify TITLE [BODY]", "post a desktop notification",
      "Show a native toast; Win+N opens the last 16 notifications. Use quoted text for multi-word titles and bodies. "
      "The shell is limited to three alerts per two seconds.", "", "notify Test \"Hello world\"", "startx" },
    { "taskmgr", "desktop", "taskmgr", "open the Task Manager", NULL, NULL, NULL, "htop" },
    { "settings", "desktop", "settings [apps [name] | time | keyboard | about]", "open Settings (or its Apps, Date & time or Keyboard page)",
      "Opens Settings. `settings apps` opens its Apps page: every installed app with its version, size and "
      "whether it is running or has an update; select one to open it, stop it, update it or uninstall it "
      "(Uninstall asks for a second click). The Apps launcher's Details opens the same page. Settings > About changes the PC name.",
      NULL, "settings\nsettings apps\nsettings apps music", "apps, apt" },
    { "wallpaper", "desktop", "wallpaper [list|reset|preset <n>|url <url>|<file> [fill|fit|stretch|center]]", "the wallpaper",
      NULL, NULL, "wallpaper list\nwallpaper ~/Pictures/beach.jpg fill", NULL },
    { "resolution", "desktop", "resolution [WxH|auto]", "the screen's modes, or switch",
      "Without a graphics driver for the card, the size changes from the next start.", NULL, "resolution 1920x1080", "lsgpu" },
    { "exit", "desktop", "exit", "close this terminal window (or end an SSH session)", NULL, NULL, NULL, NULL },
    /* ── hardware ── */
    { "diskmgr", "disks", "diskmgr [list | show DISK | init DISK mbr|gpt | create DISK [SIZE] [fat32|exfat] [LABEL] | format DISK[pN] fat32|exfat [LABEL] | delete DISKpN | mount DISKpN | unmount DISKpN | gui]",
      "Disk Management: partitions, formatting",
      "Shows the disks - their size, partition table (MBR or GPT), partitions, free space, and what each volume holds "
      "(FAT32, exFAT, NTFS, ext4...), its name and where it is mounted - and changes them: init writes a new, empty "
      "partition table; create makes a volume in the biggest free space (or freeN, as show numbers them) of SIZE (500M, 2G, "
      "all: the default) and formats it; format formats a partition (or a whole disk without a table) as FAT32 (every "
      "computer reads it, files up to 4 GB; 33 MB at least) or exFAT (big files); delete removes a partition. The changes "
      "erase what is there, so they ask for -y. The disk Banana OS runs from and CD drives are never changed. gui (or "
      "diskmgmt.msc) opens the Disk Management window; it is also in the Start menu (All Programs) and the desktop's menu.",
      "-y|go ahead with a change that erases data",
      "diskmgr list\ndiskmgr init usb0 gpt -y\ndiskmgr create usb0 2G exfat PHOTOS -y\ndiskmgr format usb0p1 fat32 STICK -y\ndiskmgr delete sata1p2 -y",
      "lsblk, mount, umount, mkfs.fat" },
    { "diskpart", "disks", "diskpart", "the same as diskmgr", NULL, NULL, NULL, "diskmgr" },
    { "diskmgmt.msc", "disks", "diskmgmt.msc", "the Disk Management window", NULL, NULL, NULL, "diskmgr" },
    { "mkfs.fat", "disks", "mkfs.fat [-n LABEL] DISK[pN]", "format as FAT32 (mkfs.vfat too)",
      "Formats a partition (usb0p1) or a whole disk without a partition table (usb0) as FAT32, as Linux's mkfs.fat does. "
      "Never the disk Banana OS runs from.", "-n LABEL|the volume's name (11 letters)", "mkfs.fat -n STICK usb0p1", "mkfs.exfat, diskmgr" },
    { "mkfs.vfat", "disks", "mkfs.vfat [-n LABEL] DISK[pN]", "the same as mkfs.fat", NULL, NULL, NULL, "mkfs.fat" },
    { "mkfs.exfat", "disks", "mkfs.exfat [-n LABEL] DISK[pN]", "format as exFAT",
      "Formats a partition or a whole disk without a partition table as exFAT (files bigger than 4 GB; read by Windows, "
      "macOS and Linux).", "-n LABEL|the volume's name (11 letters)", "mkfs.exfat -n BIG usb0p1", "mkfs.fat, diskmgr" },
    { "devmgr", "hardware", "devmgr [-c | show N|NAME | problems | rescan | gui]", "Device Manager: every device and its driver",
      "Lists the computer's devices by type, as Windows' Device Manager does - disk drives, display adapters, keyboards, "
      "network adapters, processors, USB controllers... - with the driver that runs each one; ! marks a device with no driver "
      "(under Other devices). -c shows them by connection: each under the bridge, controller or hub it is plugged into. "
      "show prints one device's properties: maker, location, status, driver, hardware ID (PCI\\VEN_8086&DEV_100E...) and "
      "resources (IRQ, memory and I/O ranges). gui (or devmgmt.msc) opens the Device Manager window; it is also in the Start "
      "menu (All Programs) and the desktop's right-click menu.",
      "-c|by connection\nshow N|one device's properties (its number, part of its name, or its driver)\nproblems|the devices without a driver\nrescan|look for USB devices plugged in or out first\ngui|the window",
      "devmgr\ndevmgr -c\ndevmgr show e1000\ndevmgr gui", "lspci, lsusb, lsblk, drivers" },
    { "lsdev", "hardware", "lsdev", "the same as devmgr", NULL, NULL, NULL, "devmgr" },
    { "devmgmt.msc", "hardware", "devmgmt.msc", "the Device Manager window", NULL, NULL, NULL, "devmgr" },
    { "gpu", "hardware", "gpu experimental [on | off]", "experimental graphics drivers",
      "Turns on (or off) the graphics drivers written without the hardware to try them on: for AMD Radeon HD 5000 / 6000 / "
      "7000 TeraScale 2 cards (the HD 7450...) and RX 400 / 500 Polaris cards (the RX 580...), a hardware mouse pointer and "
      "vsync over the firmware's screen. The driver first checks that the card scans out exactly the screen the firmware "
      "handed over, else it leaves the card alone. The choice is kept; off takes effect at the next start. With no word: "
      "whether it is on and what the driver found. Intel HD / UHD / Iris Graphics (1st to 10th generation Core) need no switch: their "
      "driver is always on. NVIDIA cards keep the firmware's screen.",
      NULL, "gpu experimental on\ngpu experimental", "lsgpu, devmgr" },
    { "gpu3d", "hardware", "gpu3d [status | on | off | test]", "3D on Intel HD / UHD / Iris Graphics (1st - 10th generation Core)",
      "Banana OS drives the render engine of Intel graphics of the 1st to 10th generation Core processors itself, for OpenGL "
      "through Mesa: crocus on Ironlake, Sandy Bridge, Ivy Bridge and Haswell, iris (experimental: written without the "
      "hardware) on Broadwell, Skylake, Kaby / Coffee / Comet Lake and Ice Lake, where every app gets its own GPU address "
      "space. It starts the first time an app uses "
      "it. With no word: whether it runs, its ring, the GPU memory in use. test starts it if it is not running and checks "
      "it on the real GPU: a batch that writes memory, the 3D pipeline's write, 4000 submissions, the GPU's clock. A GPU "
      "that hangs is reset after 2 seconds. Apps (OpenGL through Mesa) use a supported GPU by default; gpu3d off sends "
      "them back to drawing on the processor (the choice is kept), gpu3d on lets them use it again.",
      "status|whether it is on and runs (the default)\non|OpenGL apps started from now on use the GPU (the default)\noff|they draw on the processor\ntest|check it on this GPU",
      "gpu3d\ngpu3d test\ngpu3d off", "lsgpu, gpu" },
    { "lspci", "hardware", "lspci [-v] [-n]", "the PCI devices, as Linux's lspci",
      "One line per PCI function: its address (bus:device.function), class, maker, name and IDs. -v adds its interrupt, "
      "its memory and I/O regions and the Banana OS driver in use; -n prints the numbers only.",
      "-v|interrupt, regions, driver\n-n|numeric IDs", "lspci\nlspci -v", "devmgr, lsusb" },
    { "lsusb", "hardware", "lsusb", "USB controllers and devices", NULL, NULL, NULL, "usb" },
    { "usb", "hardware", "usb rescan", "look for USB devices plugged in or out", NULL, NULL, NULL, "lsusb" },
    { "usbctl", "hardware", "usbctl", "USB legacy handoff state", NULL, NULL, NULL, NULL },
    { "lsgpu", "hardware", "lsgpu", "graphics cards: driver, mode, video memory", NULL, NULL, NULL, "resolution" },
    { "gpubench", "hardware", "gpubench", "offscreen framebuffer bandwidth and GPU counters",
      "Measures CPU copies in RAM, checks pixels and reports presentation counters. This is not GPU 3D performance.", NULL, NULL, "lsgpu, selftest" },
    { "gpu_info", "hardware", "gpu_info [-n -r -b -p -m]", "the framebuffer", NULL, NULL, NULL, "lsgpu" },
    { "laptop", "hardware", "laptop", "ACPI battery, adapter, lid and panel information", NULL, NULL, NULL, NULL },
    { "usbc", "hardware", "usbc [role PORT source|sink]", "USB-C Power Delivery contracts and firmware power roles", NULL, NULL, NULL, NULL },
    { "brightness", "hardware", "brightness [0-100]", "a laptop panel's backlight (Intel graphics)", NULL, NULL, NULL, NULL },
    { "keyboardctl", "hardware", "keyboardctl [layout]", "the keyboard layout",
      "Shows or sets it: EN (Default) - the US keyboard -, UK, US-Intl (US international: dead accents), FR (AZERTY), BE (Belgian AZERTY), fr_CH, de_CH (Swiss), DE (QWERTZ), ES, IT, PT, BEPO, fr_CA (Canadian French), LatAm, BR (ABNT2), NL, SE, FI, NO, DK, PL (programmer), CZ, HU, HR (Croatian, Slovene), RO, TR (Q), GR (Greek), RU (Russian), UA (Ukrainian), Dvorak, Colemak. On the desktop Win+Space switches to EN and back (for Russian or Greek, to type commands). Every key is read by its position, with AltGr (the right Alt key, or Ctrl+Alt) and the dead accents (^ then e: ê). Settings > Keyboard does the same; the layout is kept for the next start. loadctl is the same.", NULL, "keyboardctl FR\nkeyboardctl UK", NULL },
    { "loadctl", "hardware", "loadctl [layout]", "the same as keyboardctl", NULL, NULL, NULL, "keyboardctl" },
    { "sensors", "hardware", "sensors", "the processor's temperature (real hardware)", NULL, NULL, NULL, NULL },
    { "gamepad", "hardware", "gamepad", "game controllers: sticks, triggers, buttons", NULL, NULL, NULL, NULL },
    { "adb", "hardware", "adb devices|shell|push|pull|install|logcat|reboot ...", "Android phones over USB",
      "With USB debugging on: the phone asks to allow this computer the first time (the key is in ~/.android).", NULL,
      "adb devices\nadb push notes.txt /sdcard/", "fastboot" },
    { "fastboot", "hardware", "fastboot devices|getvar|flash|erase|boot|reboot ...", "a phone in its bootloader", NULL, NULL, NULL, "adb" },
    /* ── sound ── */
    { "play", "sound", "play <file.wav> | play -s", "play a sound (-s stops)", NULL, NULL, NULL, "volume" },
    { "beep", "sound", "beep [hz] [ms]", "a tone", NULL, NULL, "beep 880 200", NULL },
    { "volume", "sound", "volume [0-100]", "the sound volume", NULL, NULL, NULL, NULL },
    { "lsaudio", "sound", "lsaudio", "the sound card in use", NULL, NULL, NULL, NULL },
    /* ── shell ── */
    { "help", "shell", "help [command|topic] | help -k <word> | help -a", "this help",
      "help alone lists the commands by topic. help <command> shows a command's page; help <topic> lists "
      "a topic's commands (files, text, disks, system, network, servers, apps, desktop, hardware, sound, "
      "shell, vm). <command> --help and man <command> show the page too.",
      "-k word|search names and descriptions (apropos)\n-a|every command, a line each\n--check|commands without a page",
      "help ls\nhelp network\nhelp -k disk\nls --help", "man, apropos" },
    { "man", "shell", "man <command>", "a command's page (the same as help <command>)", NULL, NULL, "man chmod", "help" },
    { "apropos", "shell", "apropos <word>", "search the commands (the same as help -k)", NULL, NULL, "apropos network", "help" },
    { "clear", "shell", "clear", "clear the screen", NULL, NULL, NULL, NULL },
    { "history", "shell", "history", "the commands typed before (!! repeats the last one)", NULL, NULL, NULL, NULL },
    { "which", "shell", "which <command>", "where a command comes from (built in, $PATH)", NULL, NULL, "which gears", "type" },
    { "type", "shell", "type <command>", "like which, aliases too", NULL, NULL, NULL, "which, alias" },
    { "alias", "shell", "alias [name[=value]]", "list or define a command alias", NULL, NULL, "alias ll='ls -l'", "unalias" },
    { "unalias", "shell", "unalias <name>", "remove an alias", NULL, NULL, NULL, "alias" },
    { "export", "shell", "export [NAME=value]", "list or set an environment variable",
      "$NAME in a command is replaced by its value. PATH says where commands are looked for (/bin:/apps:/home/banana/bin).",
      NULL, "export PATH=$PATH:/home/banana/tools", "unset, env" },
    { "unset", "shell", "unset <name>", "remove an environment variable", NULL, NULL, NULL, "export" },
    { "env", "shell", "env", "the environment variables", NULL, NULL, NULL, "export" },
    { "chsh", "shell", "chsh [sh|bash|zsh]", "the shell for new Terminal windows and tabs", "Saves the choice for Terminal; existing shells keep running.", NULL, "chsh zsh\nchsh bash", "settings" },
    { "run", "shell", "run <file.sh>", "run a script, line by line",
      "Scripts also run by their name when they are in a $PATH folder.", NULL, NULL, "export" },
    /* ── vm and tests ── */
    { "vmtools", "vm", "vmtools [on|off|time]", "guest tools for virtual machines",
      "Shows the hypervisor and the guest tools working: the QEMU guest agent (Proxmox: addresses, "
      "clean shutdown, clock, backups), VMMouse (the pointer follows the host's), VMware's tools channel "
      "and VirtualBox's VMMDev. off / on take effect from the next start; time compares the host's clock with this one.",
      NULL, "vmtools\nvmtools time", "selftest" },
    { "selftest", "vm", "selftest [llvmpipe|virgl|gpu|ring3|memory|ram|sleep [cycle]|limits|cores|clock|malloc|avx|syscall|disk|heap|opencl|browser [swm|isolation|github|google]|mouse [demo]|boot|posix [part]|ipv6 [host [port]]]", "check the system",
      "Runs the built-in checks - one ok / FAIL / skip line each, then the number failed. Alone it runs "
      "ring3, memory, limits, cores, malloc, AVX, syscall, disk, heap, OpenCL, offline browser and POSIX checks; a part name runs only that part. The app side is "
      "the systest example, installed from the kernel's own copy. All the tests: docs/TESTING.md.",
      "ring3|an app runs in user mode (64-bit kernel)\n"
      "memory|RAM above 4 GiB: 64 MiB for an app, given back after\n"
      "limits|60 threads, 60 open files, 24 apps at once\n"
      "cores|malloc / free / a lock from threads on every core\n"
      "clock|shared HPET / ACPI clock, rollovers, real elapsed time and migrating user threads\n"
      "malloc|the apps' own heap: random blocks checked, and how much faster than the kernel's allocator\n"
      "avx|AVX registers kept across task switches, none left for a new thread (CPU=max in QEMU)\n"
      "syscall|the syscall / sysret entry: registers kept, stack arguments right, timed against int 0x80\n"
      "disk|32 MiB read twice from a SATA / virtio / NVMe disk while a 1 ms task keeps running (read only)\n"
      "heap|the kernel's heap checked block by block, and timed among thousands of live blocks\n"
      "opencl|the cltest example: barriers, local memory, vectors...\n"
      "browser|offline Tailwind, React, Vue, Alpine, Astro, grid and WebP checks\n"
      "boot|shared BIOS/UEFI graphical menu rendering, clipping and fallback checks\n"
      "ram|firmware RAM totals, above-4-GiB maps and live accounting\n"
      "sleep [cycle]|ACPI S3 checks; cycle sleeps for five seconds and wakes by RTC\n"
      "mouse [demo]|relative mouse capture tests; demo opens a look-around window\n"
      "browser swm|live checks against https://swm.mxtpasontop.fr/\n"
      "posix [PART]|mmap, threads, sockets, signals and dlopen (the posixtest example)\n"
      "ipv6|IPv6 UDP / TCP on ::1, parsing, URLs; IPv6 is put back as it was\n"
      "ipv6 HOST [PORT]|also an AAAA lookup and a TCP connection (port 80)",
      "selftest\nselftest browser\nselftest browser swm\nselftest posix\nselftest ipv6 example.com", "fstest, crashtest, fsck, vmtools" },
};
#define NPAGES ((int)(sizeof(PAGES) / sizeof(PAGES[0])))

static const char* const* g_known;

void help_set_known(const char* const* names) { g_known = names; }

/* ── output ───────────────────────────────────────────────────────────── */

static int cols(void) {
    const uint32_t* ch;
    const uint8_t* co;
    int w = 80, h = 25, stride = 80;
    terminal_vt_get_buffer(terminal_vt_get_active(), &ch, &co, &w, &h, &stride);
    if (w < 40) w = 40;
    if (w > 132) w = 132;
    return w;
}

static void color_line(const char* s, uint8_t fg) {
    terminal_write_color(s, fg, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

/* text wrapped to the screen, every line indented by indent spaces */
static void wrapped(const char* text, int indent, uint8_t fg) {
    int width = cols() - 1 - indent;
    if (width < 20) width = 20;
    char line[160];
    const char* p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0, last_space = -1;
        const char* q = p;
        while (*q && *q != '\n' && n < width) {
            if (*q == ' ') last_space = n;
            n++;
            q++;
        }
        int take = n;
        if (*q && *q != '\n' && last_space > 0) take = last_space;
        int k = 0;
        for (; k < indent && k < (int)sizeof(line) - 1; k++) line[k] = ' ';
        for (int i = 0; i < take && k < (int)sizeof(line) - 1; i++) line[k++] = p[i];
        line[k] = 0;
        color_line(line, fg);
        p += take;
        if (*p == '\n') p++;
    }
}

static const page_t* find_page(const char* name) {
    for (int i = 0; i < NPAGES; i++) if (strcmp(PAGES[i].name, name) == 0) return &PAGES[i];
    return NULL;
}

static const topic_t* find_topic(const char* key) {
    for (int i = 0; i < NTOPICS; i++)
        if (strcmp(TOPICS[i].key, key) == 0) return &TOPICS[i];
    return NULL;
}

/* commands of the live CD only are not shown on an installed system */
static int shown(const page_t* p) {
    if (sysinfo_live_boot()) return 1;
    return strcmp(p->name, "install") && strcmp(p->name, "installer") && strcmp(p->name, "update");
}

/* "  name           brief" */
static void brief_line(const page_t* p) {
    char b[160];
    ksnprintf(b, sizeof(b), "  %-13s ", p->name);
    terminal_write_color(b, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    int room = cols() - 17;
    char d[140];
    kstrlcpy(d, p->brief, sizeof(d));
    if (room > 10 && (int)strlen(d) > room) { d[room - 3] = '.'; d[room - 2] = '.'; d[room - 1] = 0; }
    terminal_writeln(d);
}

static void heading(const char* s) { color_line(s, VGA_COLOR_YELLOW); }

static void show_page(const page_t* p) {
    heading("NAME");
    char b[200];
    ksnprintf(b, sizeof(b), "    %s - %s", p->name, p->brief);
    terminal_writeln(b);
    heading("SYNOPSIS");
    wrapped(p->synopsis, 4, VGA_COLOR_LIGHT_GREEN);
    if (p->desc) { heading("DESCRIPTION"); wrapped(p->desc, 4, VGA_COLOR_LIGHT_GREY); }
    if (p->opts) {
        heading("OPTIONS");
        const char* s = p->opts;
        while (*s) {
            char opt[48], what[140];
            int n = 0;
            while (*s && *s != '|' && *s != '\n' && n < (int)sizeof(opt) - 1) opt[n++] = *s++;
            opt[n] = 0;
            n = 0;
            if (*s == '|') s++;
            while (*s && *s != '\n' && n < (int)sizeof(what) - 1) what[n++] = *s++;
            what[n] = 0;
            if (*s == '\n') s++;
            ksnprintf(b, sizeof(b), "    %-14s ", opt);
            terminal_write_color(b, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            terminal_writeln(what);
        }
    }
    if (p->examples) {
        heading("EXAMPLES");
        const char* s = p->examples;
        while (*s) {
            char ex[160];
            int n = 0;
            while (*s && *s != '\n' && n < (int)sizeof(ex) - 1) ex[n++] = *s++;
            ex[n] = 0;
            if (*s == '\n') s++;
            terminal_write("    ");
            terminal_write_color("$ ", VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            color_line(ex, VGA_COLOR_WHITE);
        }
    }
    const topic_t* t = find_topic(p->topic);
    if (p->see || t) {
        heading("SEE ALSO");
        ksnprintf(b, sizeof(b), "    %s%shelp %s", p->see ? p->see : "", p->see ? "; " : "", t ? t->key : "");
        terminal_writeln(b);
    }
}

int help_show_page(const char* cmd) {
    const page_t* p = find_page(cmd);
    if (!p) return 0;
    show_page(p);
    return 1;
}

static void overview(void) {
    color_line("Banana OS 0.6 - the commands, by topic", VGA_COLOR_YELLOW);
    int width = cols();
    for (int t = 0; t < NTOPICS; t++) {
        char b[48];
        ksnprintf(b, sizeof(b), "%-9s", TOPICS[t].key);
        terminal_write_color(b, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        int x = 9;
        for (int i = 0; i < NPAGES; i++) {
            const page_t* p = &PAGES[i];
            if (strcmp(p->topic, TOPICS[t].key) || !shown(p)) continue;
            int l = (int)strlen(p->name) + 1;
            if (x + l >= width - 1) { terminal_putchar('\n'); terminal_write("         "); x = 9; }
            terminal_write(" ");
            terminal_write(p->name);
            x += l;
        }
        terminal_putchar('\n');
    }
    terminal_putchar('\n');
    color_line("  help <command>   a command's page (also: man <command>, <command> --help)", VGA_COLOR_DARK_GREY);
    color_line("  help <topic>     a topic's commands with a line each (help files, help network...)", VGA_COLOR_DARK_GREY);
    color_line("  help -k <word>   search the commands;  help -a: all of them, a line each", VGA_COLOR_DARK_GREY);
    color_line("  Shell: cmd > file, cmd >> file, cmd1; cmd2, cmd1 && cmd2, $VAR, !!", VGA_COLOR_DARK_GREY);
}

static void topic_list(const topic_t* t) {
    heading(t->title);
    for (int i = 0; i < NPAGES; i++)
        if (strcmp(PAGES[i].topic, t->key) == 0 && shown(&PAGES[i])) brief_line(&PAGES[i]);
}

/* case-insensitive substring */
static int has_word(const char* hay, const char* w) {
    if (!hay) return 0;
    int wl = (int)strlen(w);
    for (const char* h = hay; *h; h++) {
        int k = 0;
        while (k < wl && h[k]) {
            char a = h[k], b = w[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (k == wl) return 1;
    }
    return 0;
}

void help_apropos(const char* w) {
    while (*w == ' ') w++;
    if (!*w) { terminal_writeln("usage: apropos <word>   (or help -k <word>)"); return; }
    int n = 0;
    for (int i = 0; i < NPAGES; i++) {
        const page_t* p = &PAGES[i];
        if (!shown(p)) continue;
        if (has_word(p->name, w) || has_word(p->brief, w) || has_word(p->desc, w) || has_word(p->synopsis, w)) {
            brief_line(p);
            n++;
        }
    }
    if (!n) {
        terminal_write("apropos: nothing about ");
        terminal_writeln(w);
    }
}

/* ── help -a: every command, with a pager on the local screen ─────────── */

static int key_wait(void) {
    int my_vt = terminal_vt_get_active();
    for (;;) {
        gui_poll();
        terminal_vt_set_active(my_vt);
        if (tty_current() < 0 && gui_focused_vt() != my_vt) { task_sleep_ms(10); continue; }
        char c = keyboard_try_getchar();
        if (c) return c;
        task_sleep_ms(10);
    }
}

static void all_commands(void) {
    const uint32_t* ch;
    const uint8_t* co;
    int w = 80, h = 25, stride = 80;
    terminal_vt_get_buffer(terminal_vt_get_active(), &ch, &co, &w, &h, &stride);
    int page_rows = h - 2, row = 0;
    int pager = !terminal_is_capturing();
    for (int t = 0; t < NTOPICS; t++) {
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) {
                if (pager && row && row + 2 > page_rows) {
                    terminal_write_color("-- more: any key, q to stop --", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
                    int c = key_wait();
                    terminal_putchar('\n');
                    if (c == 'q' || c == 'Q' || c == 3) return;
                    row = 0;
                }
                heading(TOPICS[t].title);
                row++;
                continue;
            }
            for (int i = 0; i < NPAGES; i++) {
                const page_t* p = &PAGES[i];
                if (strcmp(p->topic, TOPICS[t].key) || !shown(p)) continue;
                if (pager && row >= page_rows) {
                    terminal_write_color("-- more: any key, q to stop --", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
                    int c = key_wait();
                    terminal_putchar('\n');
                    if (c == 'q' || c == 'Q' || c == 3) return;
                    row = 0;
                }
                brief_line(p);
                row++;
            }
        }
    }
}

static void check(void) {
    int missing = 0;
    for (int i = 0; g_known && g_known[i]; i++) {
        if (find_page(g_known[i])) continue;
        terminal_write("help: no page for ");
        terminal_writeln(g_known[i]);
        missing++;
    }
    int dup = 0;
    for (int i = 0; i < NPAGES; i++)
        for (int j = i + 1; j < NPAGES; j++)
            if (strcmp(PAGES[i].name, PAGES[j].name) == 0) { terminal_write("help: two pages for "); terminal_writeln(PAGES[i].name); dup++; }
    char b[96];
    ksnprintf(b, sizeof(b), "help: %d pages, %d commands without one, %d duplicates", NPAGES, missing, dup);
    terminal_writeln(b);
}

void help_man(const char* args) {
    while (*args == ' ') args++;
    if (!*args) { terminal_writeln("What manual page do you want? (man <command>; help lists them)"); return; }
    if (!help_show_page(args)) {
        terminal_write("No manual entry for ");
        terminal_writeln(args);
        help_suggest(args);
    }
}

void help_command(const char* args) {
    while (*args == ' ') args++;
    char a[48];
    kstrlcpy(a, args, sizeof(a));
    for (int i = (int)strlen(a) - 1; i >= 0 && a[i] == ' '; i--) a[i] = 0;
    if (!a[0]) { overview(); return; }
    if (strcmp(a, "-a") == 0 || strcmp(a, "all") == 0) { all_commands(); return; }
    if (strcmp(a, "--check") == 0) { check(); return; }
    if (strncmp(a, "-k ", 3) == 0) { help_apropos(a + 3); return; }
    const topic_t* t = find_topic(a);
    if (t) { topic_list(t); return; }
    if (help_show_page(a)) return;
    terminal_write("help: no command or topic called ");
    terminal_writeln(a);
    help_suggest(a);
}

/* ── did you mean ...? ───────────────────────────────────────────────── */

/* Damerau-Levenshtein distance (adjacent swaps count 1), short names only */
static int distance(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 24 || lb > 24) return 99;
    int d[26][26];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < v) v = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < v) v = d[i - 1][j - 1] + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && d[i - 2][j - 2] + 1 < v) v = d[i - 2][j - 2] + 1;
            d[i][j] = v;
        }
    return d[la][lb];
}

void help_suggest(const char* word) {
    char w[32];
    int n = 0;
    while (word[n] && word[n] != ' ' && n < (int)sizeof(w) - 1) { w[n] = word[n]; n++; }
    w[n] = 0;
    if (!w[0]) return;
    const char* best[3] = { 0, 0, 0 };
    int bestd[3] = { 99, 99, 99 };
    int limit = n <= 3 ? 1 : 2;
    for (int i = 0; i < NPAGES; i++) {
        const char* c = PAGES[i].name;
        if (!shown(&PAGES[i])) continue;
        int dd = distance(w, c);
        if (dd > limit || dd == 0) continue;
        for (int k = 0; k < 3; k++) {
            if (dd < bestd[k]) {
                for (int m = 2; m > k; m--) { best[m] = best[m - 1]; bestd[m] = bestd[m - 1]; }
                best[k] = c;
                bestd[k] = dd;
                break;
            }
        }
    }
    if (!best[0]) return;
    terminal_write_color("Did you mean: ", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    for (int k = 0; k < 3 && best[k]; k++) {
        if (k) terminal_write(", ");
        terminal_write_color(best[k], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }
    terminal_putchar('\n');
}
