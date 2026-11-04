#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Vim 9.2, with ncurses and its full runtime. vi/ex is the independent
# Neatvi package built by ports/vi/build.sh.
#     ports/vim/build.sh <output.bpk>
[ -n "$1" ] || { echo "usage: $0 <output.bpk>"; exit 1; }
OUT=$(realpath -m "$1")
mkdir -p "$(dirname "$OUT")"
. "$(dirname "$0")/../port.sh"
port_fetch_git vim 9.2.1225 https://github.com/vim/vim.git v9.2.1225 f9b3d0d8dd1a4345375fb401861d5f60ff7c7df1
NC=$PORT_DIR/../ncurses/build/x86_64
[ -f "$NC/lib/libncursesw.a" ] || { echo "vim: build ports/ncurses first"; exit 1; }

# Vim builds in its source tree: a copy for each package
FEAT=huge RC=/apps/vim/vimrc DATA=/usr/share
T=$PORT_DIR/build/tree-$FEAT
CPPFLAGS="-I$NC/include"
LDFLAGS="-L$NC/lib"
# what configure cannot find out by running a program here
vim_cv_toupper_broken=no
vim_cv_terminfo=yes
vim_cv_tgetent=zero
vim_cv_getcwd_broken=no
vim_cv_stat_ignores_slash=no
vim_cv_memmove_handles_overlap=yes
vim_cv_bcopy_handles_overlap=yes
vim_cv_memcpy_handles_overlap=no
vim_cv_timer_create=no
vim_cv_timer_create_works=no
vim_cv_uname_output=Linux
vim_cv_uname_r_output=6.0
vim_cv_uname_m_output=x86_64
vim_cv_tty_group=world
vim_cv_tty_mode=0620
ac_cv_small_wchar_t=no
PKG_CONFIG=false                                   # (not the build machine's libraries)
OPTS="--with-features=$FEAT --with-tlib=ncursesw --disable-gui --without-x --disable-nls --disable-netbeans
      --disable-canberra --disable-libsodium --disable-gpm --disable-sysmouse --disable-selinux --disable-smack
      --disable-xattr --disable-acl --enable-multibyte --with-compiledby=Banana-OS
      --prefix=/usr --bindir=/bin --datarootdir=$DATA --with-vim-name=vim"
VARS="CPPFLAGS LDFLAGS $(set | sed -n 's/^\(vim_cv_[a-z_0-9]*\)=.*/\1/p' | tr '\n' ' ') ac_cv_small_wchar_t PKG_CONFIG"
sum=$( (echo "4 $PORT_VER $FEAT $OPTS $RC"; for v in $VARS; do eval "echo $v=\${$v}"; done
        cat "$SDK/tools/banana-configure" "$SDK/tools/banana-config.site" "$SDK/tools/banana-cc"
        find "$SDK/include" -name '*.h' | LC_ALL=C sort | xargs cat) | sha256sum | cut -d" " -f1)
if [ ! -f "$T/src/auto/config.mk" ] || [ "$(cat "$T/.configured" 2>/dev/null)" != "$sum" ]; then
    rm -rf "$T"
    cp -a "$SRC" "$T"
    cd "$T/src"
    for v in $VARS; do export "$v"; done
    # Keep Vim's system configuration inside its package.
    sed -i "s|^#define SYS_VIMRC_FILE .*|#define SYS_VIMRC_FILE \"$RC\"|" feature.h
    grep -q "$RC" feature.h || echo "#define SYS_VIMRC_FILE \"$RC\"" >> feature.h
    BANANA_ARCH=x86_64 CFLAGS="-O2 -g0" "$SDK/tools/banana-configure" ./configure $OPTS >configure.log 2>&1 \
        || { tail -30 configure.log; exit 1; }
    echo "$sum" > "$T/.configured"
fi
cd "$T/src"
# A timed wait must not become "wait for ever". inchar_loop() computes
# wait_time = wtime - elapsed and on its first pass always calls the wait
# function, also with a negative time, which means "block": with wtime 0
# (a peek: settmode -> vpeekc_nomap while writing a file, :wq, :qa!) and
# a clock tick between ELAPSED_INIT and the first ELAPSED_FUNC, Vim waited
# for a key for ever. Banana OS's clock counts whole milliseconds, so this
# happened about once in 15 runs of shelltest -e; Linux's microseconds make
# it rare there.
python3 - ui.c <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
mark = "/* Banana OS: never negative */"
if mark not in s:
    old = "\t    if (wtime >= 0)\n\t\twait_time = wtime - elapsed_time;\n"
    assert s.count(old) == 1, "vim: inchar_loop changed - see ports/vim/build.sh"
    s = s.replace(old, "\t    if (wtime >= 0)\n\t\twait_time = wtime - elapsed_time < 0 ? 0 : wtime - elapsed_time;  " + mark + "\n")
    open(p, "w").write(s)
PY
B=$T/src                                           # (port_make, port_package work here)
port_make vim

# the runtime, without the translations, the GUI's pieces and the word lists
# but English's
rm -f runtime.zip
python3 - "$T/runtime" <<'PY'
import os, sys, zipfile
top = sys.argv[1]
skip_dirs = {"lang", "icons", "print"}
z = zipfile.ZipFile("runtime.zip", "w", zipfile.ZIP_DEFLATED)
for root, dirs, files in os.walk(top):
    rel = os.path.relpath(root, top)
    if rel == ".":
        dirs[:] = [d for d in dirs if d not in skip_dirs]
    dirs.sort()
    for f in sorted(files):
        if f.endswith((".png", ".xpm", ".ico", ".desktop", ".svg", ".bmp", ".ps")) or f in ("Makefile", "makemenu.vim"):
            continue
        if rel.startswith("spell") and (rel != "spell" or not f.startswith(("en.utf-8.", "README"))):
            continue                               # (the English word list only: `apt` has no others)
        if rel.startswith("tutor") and not (f.startswith("tutor") and ("." not in f or f.endswith((".tutor", ".utf-8", ".vim", ".json")))) and not f.endswith(".vim"):
            continue
        p = os.path.join(root, f)
        z.write(p, os.path.relpath(p, top))
z.close()
PY
port_package "$OUT" name=vim "title=Vim" version=$PORT_VER type=console \
    "description=Vim: the programmer's text editor - syntax colouring for 600 languages, :help, plugins (/bin/vim, vimdiff, view)" \
    "author=Bram Moolenaar and the Vim developers" "category=Developer Tools" bin=vim,vimdiff,view \
    unpack=runtime.zip:/usr/share/vim/vim92 \
    -- vim "vimrc=$PORT_DIR/vimrc" "runtime.zip=$T/src/runtime.zip"
