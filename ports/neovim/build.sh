#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Neovim 0.12 for Banana OS (/bin/nvim), an optional package (`apt install
# neovim`): PUC Lua 5.1 (not LuaJIT), libuv, luv, lpeg, unibilium, utf8proc
# and tree-sitter, all built here with the SDK's compiler; the tree-sitter
# parsers Neovim comes with (C, Lua, Vim script, Vim help, queries, Markdown)
# are linked into nvim itself (banana-cc makes no shared libraries: the
# parser loader finds them by name - patched below). The runtime (Lua,
# syntax, filetype plugins, :help) is runtime.zip, unpacked into
# /usr/share/nvim/runtime ($VIMRUNTIME).
#
# How: Neovim makes much of its C code at build time with Lua scripts run
# by its nlua0 - so the dependencies and nlua0 are built for the build
# machine first (build/host), then everything for Banana OS (build/target)
# with that nlua0 (NLUA0_HOST_PRG).
# Needs: cmake (3.16+), git (the sources are tags on GitHub).
#     ports/neovim/build.sh <output.bpk>
[ -n "$1" ] || { echo "usage: $0 <output.bpk>"; exit 1; }
OUT=$(realpath -m "$1")
mkdir -p "$(dirname "$OUT")"
. "$(dirname "$0")/../port.sh"
port_fetch_git neovim 0.12.6 https://github.com/neovim/neovim.git v0.12.6 779c36969e31080503be2dfccbcc3f999e289606
JOBS=$(nproc)

# ── the dependencies' sources: tarballs the deps build takes as its URLs ──
DL=$PORT_DIR/src/deps
mkdir -p "$DL"
dep_git() {     # NAME REPO TAG COMMIT: $DL/NAME.tar.gz
    [ -f "$DL/$1.tar.gz" ] && return
    rm -rf "$DL/$1"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "https://github.com/$2.git" "$DL/$1"
    [ "$(git -C "$DL/$1" rev-parse HEAD)" = "$4" ] || { echo "neovim: $2 $3 is not $4"; exit 1; }
    rm -rf "$DL/$1/.git"
    tar -C "$DL" -czf "$DL/$1.tar.gz" "$1"
    rm -rf "$DL/$1"
}
dep_url() {     # NAME URL SHA256
    [ -f "$DL/$1.tar.gz" ] || { curl -fsSL --retry 4 --retry-delay 10 --retry-all-errors --connect-timeout 30 \
                                     -o "$DL/$1.tar.gz.part" "$2" && mv "$DL/$1.tar.gz.part" "$DL/$1.tar.gz"; }
    echo "$3  $DL/$1.tar.gz" | sha256sum -c - >/dev/null
}
dep_git libuv libuv/libuv v1.52.1 1cfa32ff59c076ffb6ed735bbc8c18361558661f
dep_git luv luvit/luv 1.52.1-0 f65fe9d7616f9fbcd20227fc7a73b38d1c5c180d
dep_git lua_compat53 lunarmodules/lua-compat-5.3 v0.13 7af7bf7e0db3c9d88881fdf10d880a6e2ab1d095
dep_git unibilium neovim/unibilium v2.1.2 bfcb0350129dd76893bc90399cf37c45812268a2
dep_git utf8proc juliastrings/utf8proc v2.11.3 e5e799221b45bbb90f5fdc5c69b6b8dfbf017e78
dep_git treesitter tree-sitter/tree-sitter v0.26.13 d97971e24500218865c05ed1febdee2acf41bae1
dep_git treesitter_c tree-sitter/tree-sitter-c v0.24.1 7fa1be1b694b6e763686793d97da01f36a0e5c12
dep_git treesitter_lua tree-sitter-grammars/tree-sitter-lua v0.5.0 10fe0054734eec83049514ea2e718b2a56acd0c9
dep_git treesitter_vim tree-sitter-grammars/tree-sitter-vim v0.8.1 3092fcd99eb87bbd0fc434aa03650ba58bd5b43b
dep_git treesitter_vimdoc neovim/tree-sitter-vimdoc v4.1.0 f061895a0eff1d5b90e4fb60d21d87be3267031a
dep_git treesitter_query tree-sitter-grammars/tree-sitter-query v0.8.0 a225e21d81201be77da58de614e2b7851735677a
dep_git treesitter_markdown tree-sitter-grammars/tree-sitter-markdown v0.5.3 f969cd3ae3f9fbd4e43205431d0ae286014c05b5
dep_url lua https://www.lua.org/ftp/lua-5.1.5.tar.gz 2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333
dep_url lpeg https://www.inf.puc-rio.br/~roberto/lpeg/lpeg-1.1.0.tar.gz 4b155d67d2246c1ffa7ad7bc466c1ea899bbc40fef0257cc9c03cecbaed4352a
URLS=""
for d in libuv luv lua_compat53 unibilium utf8proc treesitter treesitter_c treesitter_lua treesitter_vim \
         treesitter_vimdoc treesitter_query treesitter_markdown lua lpeg; do
    U=$(echo "$d" | tr a-z A-Z)
    URLS="$URLS -D${U}_URL=$DL/$d.tar.gz"
done
DEPS_OPTS="-DUSE_BUNDLED_LUAJIT=OFF -DUSE_BUNDLED_LUA=ON -DDEPS_IGNORE_SHA=ON -DCMAKE_BUILD_TYPE=Release $URLS"

# ── for the build machine: nlua0 (and a Lua to run it); nvim itself, which
# makes :help's tags ─────────────────────────────────────────────────────
H=$PORT_DIR/build/host
NLUA0=$H/nvim/lib/libnlua0.so
NVIM_HOST=$H/nvim/bin/nvim
if [ ! -f "$NLUA0" ] || [ ! -x "$NVIM_HOST" ]; then
    cmake -S "$SRC/cmake.deps" -B "$H/deps" -G "Unix Makefiles" $DEPS_OPTS -DUSE_BUNDLED_TS_PARSERS=OFF >"$PORT_DIR/build/host-deps.log" 2>&1 \
        && make -C "$H/deps" -j"$JOBS" >>"$PORT_DIR/build/host-deps.log" 2>&1 || { tail -30 "$PORT_DIR/build/host-deps.log"; exit 1; }
    cmake -S "$SRC" -B "$H/nvim" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DDEPS_PREFIX="$H/deps/usr" -DPREFER_LUA=ON \
        -DLUA_PRG="$H/deps/usr/bin/lua" -DENABLE_LIBINTL=OFF >"$PORT_DIR/build/host-nvim.log" 2>&1 \
        && make -C "$H/nvim" -j"$JOBS" nlua0 nvim_bin >>"$PORT_DIR/build/host-nvim.log" 2>&1 || { tail -30 "$PORT_DIR/build/host-nvim.log"; exit 1; }
fi
[ -f "$NLUA0" ] || { echo "neovim: no nlua0 for the build machine"; exit 1; }

# ── Neovim's source, for Banana OS (once: a mark in each file) ─────────
# the parsers linked in: found by name, not loaded from parser/<lang>.so
f=$SRC/src/nvim/lua/treesitter.c
if ! grep -q BANANA_PARSERS "$f"; then
    python3 - "$f" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """  uv_lib_t lib;
  if (uv_dlopen(path, &lib)) {"""
new = """#ifdef __BANANA_OS__  // BANANA_PARSERS: linked into nvim (ports/neovim/build.sh)
  {
    extern const TSLanguage *tree_sitter_c(void), *tree_sitter_lua(void), *tree_sitter_vim(void),
      *tree_sitter_vimdoc(void), *tree_sitter_query(void), *tree_sitter_markdown(void),
      *tree_sitter_markdown_inline(void);
    static const struct { const char *name; const TSLanguage *(*fn)(void); } builtin[] = {
      { "c", tree_sitter_c }, { "lua", tree_sitter_lua }, { "vim", tree_sitter_vim },
      { "vimdoc", tree_sitter_vimdoc }, { "query", tree_sitter_query },
      { "markdown", tree_sitter_markdown }, { "markdown_inline", tree_sitter_markdown_inline },
    };
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
      if (strcmp(builtin[i].name, symbol) == 0) {
        return builtin[i].fn();
      }
    }
    luaL_error(L, "Failed to load parser for language '%s': only the built-in parsers here", lang_name);
  }
#endif
  uv_lib_t lib;
  if (uv_dlopen(path, &lib)) {"""
assert old in s; s = s.replace(old, new, 1); open(p, "w").write(s)
PY
fi
f=$SRC/src/nvim/CMakeLists.txt
if ! grep -q BANANA_PARSERS "$f"; then
    printf '\n# Banana OS: the tree-sitter parsers, linked in (ports/neovim/build.sh); nlua0 is\n# the build machine'"'"'s (NLUA0_HOST_PRG), not made for Banana OS\nif(BANANA_PARSERS)\n  target_link_libraries(nvim_bin PRIVATE ${BANANA_PARSERS})\n  set_target_properties(nlua0 PROPERTIES EXCLUDE_FROM_ALL TRUE)\nendif()\n' >> "$f"
fi

# Patch libuv after CMake extracts it, including on a fresh build.
python3 - "$SRC/cmake.deps/cmake/BuildLibuv.cmake" "$PORT_DIR/patch_libuv.py" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
if "# Banana OS libuv compatibility" not in s:
    s = s.replace("ExternalProject_Add(libuv",
                  'ExternalProject_Add(libuv\n  # Banana OS libuv compatibility\n  PATCH_COMMAND python3 "' + sys.argv[2] + '" <SOURCE_DIR>', 1)
    p.write_text(s)
PY
# A previous build may already have extracted libuv before this patch existed.
if [ -f "$PORT_DIR/build/target/deps/build/src/libuv/src/unix/linux.c" ]; then
    python3 "$PORT_DIR/patch_libuv.py" "$PORT_DIR/build/target/deps/build/src/libuv"
fi

# ── for Banana OS ──────────────────────────────────────────────────────
T=$PORT_DIR/build/target
TC=$SDK/tools/banana-toolchain.cmake
if [ ! -f "$T/deps/usr/lib/libluv.a" ]; then
    cmake -S "$SRC/cmake.deps" -B "$T/deps" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$TC" $DEPS_OPTS \
        -DUSE_BUNDLED_TS_PARSERS=OFF "-DDEPS_CMAKE_CACHE_ARGS=-DCMAKE_TOOLCHAIN_FILE:FILEPATH=$TC;-DLUA_MATH_LIBRARY:STRING=m" >"$PORT_DIR/build/target-deps.log" 2>&1 \
        && make -C "$T/deps" -j"$JOBS" >>"$PORT_DIR/build/target-deps.log" 2>&1 \
        || { grep -E "error|Error" "$PORT_DIR/build/target-deps.log" | head -30; exit 1; }
fi
# the parsers: one library
P=$T/parsers
if [ ! -f "$P/libbanana_parsers.a" ]; then
    rm -rf "$P"; mkdir -p "$P"
    for l in c lua vim vimdoc query markdown; do
        tar -C "$P" -xzf "$DL/treesitter_$l.tar.gz"
        if [ $l = markdown ]; then dirs="$P/treesitter_markdown/tree-sitter-markdown $P/treesitter_markdown/tree-sitter-markdown-inline"
        else dirs=$P/treesitter_$l; fi
        for d in $dirs; do
            for c in "$d"/src/parser.c "$d"/src/scanner.c; do
                [ -f "$c" ] || continue
                o=$P/$(echo "$d" | tr / _)_$(basename "$c" .c).o
                "$SDK/tools/banana-cc" -O2 -g0 -std=c11 -I "$d/src" -c "$c" -o "$o" || exit 1
            done
        done
    done
    ar rcs "$P/libbanana_parsers.a" "$P"/*.o
fi
# (libm is the SDK's C library: an empty one where CMake wants a file)
[ -f "$T/libm.a" ] || ar rcs "$T/libm.a"
# Neovim, installed into a staging folder (the runtime with :help's tags)
N=$T/nvim
ST=$T/stage
if [ ! -x "$ST/usr/bin/nvim" ] || [ "$SRC/src/nvim/lua/treesitter.c" -nt "$ST/usr/bin/nvim" ]; then
    [ -f "$N/Makefile" ] || cmake -S "$SRC" -B "$N" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr -DDEPS_PREFIX="$T/deps/usr" -DPREFER_LUA=ON -DLUA_PRG="$H/deps/usr/bin/lua" \
        -DNLUA0_HOST_PRG="$NLUA0" -DNVIM_HOST_PRG="$NVIM_HOST" -DENABLE_LIBINTL=OFF -DLUA_MATH_LIBRARY="$T/libm.a" -DICONV_INCLUDE_DIR="$SDK/include" \
        -DBANANA_PARSERS="$P/libbanana_parsers.a" >"$PORT_DIR/build/target-nvim.log" 2>&1 \
        || { tail -30 "$PORT_DIR/build/target-nvim.log"; exit 1; }
    rm -rf "$ST"
    make -C "$N" -j"$JOBS" nvim_bin >>"$PORT_DIR/build/target-nvim.log" 2>&1 \
        && make -C "$N" install DESTDIR="$ST" >>"$PORT_DIR/build/target-nvim.log" 2>&1 \
        || { grep -E "error:|undefined reference|Error [0-9]" "$PORT_DIR/build/target-nvim.log" | head -30; exit 1; }
fi

# the runtime: a parser/<lang>.so for each linked-in parser (empty: what the
# Lua side looks for), without the GUI's pieces and the translations
RT=$ST/usr/share/nvim/runtime
mkdir -p "$RT/parser"
for l in c lua vim vimdoc query markdown markdown_inline; do : > "$RT/parser/$l.so"; done
cd "$T"
rm -f runtime.zip
python3 - "$RT" <<'PY'
import os, sys, zipfile
top = sys.argv[1]
z = zipfile.ZipFile("runtime.zip", "w", zipfile.ZIP_DEFLATED)
for root, dirs, files in os.walk(top):
    dirs.sort()
    rel = os.path.relpath(root, top)
    for f in sorted(files):
        if f.endswith((".png", ".ico", ".desktop", ".xml", ".rc", ".zig")):
            continue
        if rel.startswith("spell") and not f.startswith("en."):
            continue
        p = os.path.join(root, f)
        z.write(p, os.path.relpath(p, top))
z.close()
PY
B=$T                                               # (port_package works here)
port_package "$OUT" name=neovim "title=Neovim" version=$PORT_VER type=console \
    "description=Neovim: Vim, refactored - Lua plugins and configuration, tree-sitter, a built-in terminal (/bin/nvim)" \
    "author=the Neovim developers" "category=Developer Tools" bin=nvim \
    unpack=runtime.zip:/usr/share/nvim/runtime \
    -- "$ST/usr/bin/nvim" "runtime.zip=$T/runtime.zip"
