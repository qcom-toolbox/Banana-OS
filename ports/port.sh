# SPDX-License-Identifier: GPL-3.0-or-later
# What the ports of Unix software built with the SDK's cross compiler
# (sdk/tools/banana-cc) share - sourced by ports/<name>/build.sh:
#
#     . "$(dirname "$0")/../port.sh"
#     port_fetch NAME VERSION SHA256 URL      the source, once, into ports/<name>/src
#     port_fetch_git NAME VERSION REPO TAG COMMIT   (or from a git tag)
#     port_configure [configure options]      ports/<name>/build/x86_64, configured
#     make ...                                (there)
#     port_package OUT.bpk name=... title=... (the manifest's lines) -- FILES
#
# Everything is 64-bit: these programs fork. A build is configured again
# from scratch when what goes into configure changes (its options, the
# PORT_ENV variables a port sets for configure, the toolchain, the SDK's
# headers): a fresh build directory, so nothing stale is left.

set -e
PORT_DIR=$(cd "$(dirname "$0")" && pwd)
SDK=$(cd "$PORT_DIR/../../sdk" && pwd)
B=$PORT_DIR/build/x86_64

mkdir -p "$PORT_DIR/build"
exec 9>"$PORT_DIR/build/.lock"
flock 9

# port_fetch name version sha256 url: SRC (the unpacked source)
port_fetch() {
    PORT_NAME=$1
    PORT_VER=$2
    local sha=$3 url=$4 file
    file=$PORT_DIR/src/$(basename "$url")
    SRC=$PORT_DIR/src/$PORT_NAME-$PORT_VER
    if [ ! -d "$SRC" ]; then
        mkdir -p "$PORT_DIR/src"
        # (retried: a site that does not answer once must not fail the build;
        #  a .part file, so a cut-off download is never taken for the archive)
        [ -f "$file" ] || { curl -fsSL --retry 4 --retry-delay 10 --retry-all-errors --connect-timeout 30 \
                                 -o "$file.part" "$url" && mv "$file.part" "$file"; }
        echo "$sha  $file" | sha256sum -c - >/dev/null
        tar -C "$PORT_DIR/src" -xf "$file"
    fi
    port_fixups
}

# port_fetch_git name version repo tag commit: the same from a git tag (a
# shallow clone; the tag must name that commit) - for sources only on GitHub
port_fetch_git() {
    PORT_NAME=$1
    PORT_VER=$2
    local repo=$3 tag=$4 commit=$5 got
    SRC=$PORT_DIR/src/$PORT_NAME-$PORT_VER
    if [ ! -d "$SRC" ]; then
        mkdir -p "$PORT_DIR/src"
        rm -rf "$SRC.git"
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$repo" "$SRC.git"
        got=$(git -C "$SRC.git" rev-parse HEAD)
        [ "$got" = "$commit" ] || { echo "port.sh: $repo $tag is $got, not $commit"; rm -rf "$SRC.git"; exit 1; }
        rm -rf "$SRC.git/.git"
        mv "$SRC.git" "$SRC"
    fi
    port_fixups
}

# what every port's source needs here, applied once (a mark in the file):
# gnulib's sigsegv module catches stack overflows on Linux by finding the
# stack in /proc/self/maps or with mincore - neither tells it here, and the
# search runs through the whole address space (grep, diff hung at start):
# off on Banana OS (c-stack then does without)
port_fixups() {
    local f
    for f in $(find "$SRC" -name sigsegv.in.h -path '*lib/*' 2>/dev/null); do
        grep -q __BANANA_OS__ "$f" && continue
        sed -i 's/^#if defined __linux__ || defined __ANDROID__ || defined __GNU__ \\$/#if !defined __BANANA_OS__ \&\& (defined __linux__ || defined __ANDROID__ || defined __GNU__) \&\& 1 \\/' "$f"
        sed -i '1i /* (Banana OS: no stack overflow or SIGSEGV recovery - ports/port.sh) */' "$f"
        grep -q '!defined __BANANA_OS__' "$f" || { echo "port.sh: could not patch $f"; exit 1; }
    done
}

# port_configure options...: configured in $B (cd there), when needed
port_configure() {
    local sum
    sum=$( (echo "${PORT_CONFV:-1} $PORT_VER $*"; for v in $PORT_ENV; do eval "echo $v=\${$v}"; done
            cat "$SDK/tools/banana-configure" "$SDK/tools/banana-config.site" "$SDK/tools/banana-cc"
            find "$SDK/include" -name '*.h' | LC_ALL=C sort | xargs cat) | sha256sum | cut -d" " -f1)
    if [ ! -f "$B/Makefile" ] || [ "$(cat "$B/.configured" 2>/dev/null)" != "$sum" ]; then
        rm -rf "$B"
        mkdir -p "$B"
        cd "$B"
        for v in $PORT_ENV; do export "$v"; done
        BANANA_ARCH=x86_64 CFLAGS="${PORT_CFLAGS:--O2 -g0}" "$SDK/tools/banana-configure" "$SRC/configure" "$@" \
            >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
        echo "$sum" > .configured
    fi
    cd "$B"
}

# the programs linked again when the SDK's C library changed (make sees no
# change of its own: the library is not one of its files)
port_relink() {
    local sum f
    sum=$(cat "$SDK"/lib/*.c "$SDK"/lib/*.h "$SDK/tools/banana-cc" 2>/dev/null | sha256sum | cut -d" " -f1)
    [ "$(cat "$B/.libc" 2>/dev/null)" = "$sum" ] && return
    find "$B" -type f ! -name '*.o' ! -name '*.a' ! -path '*/pkgdata/*' -size +1k | while read -r f; do
        [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ] || continue
        case $(od -An -tx1 -j16 -N1 "$f" | tr -d ' ') in 02|03) rm -f "$f" ;; esac
    done
    echo "$sum" > "$B/.libc"
}

# port_make targets...: make, its errors shown if it fails
port_make() {
    port_relink
    make -j"$(nproc)" "$@" >make.log 2>&1 || { grep -E "error:|Error [0-9]|undefined reference|cannot find" make.log | head -40; exit 1; }
}

# port_package out.bpk "key=value"... -- program [name=file...]: a package
# whose app-x86_64 is the program (stripped); the others are its data,
# stored as name - programs among them stripped too (bin=name then links
# /bin/name to that one: a package of several programs)
port_package() {
    local out=$1 prog f name src
    shift
    : > "$B/manifest"
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do printf '%s\n' "$1" >> "$B/manifest"; shift; done
    [ "$1" = "--" ] && shift
    prog=$1
    shift
    strip -o "$B/app-x86_64" "$prog"
    mkdir -p "$B/pkgdata"
    set -- "$@" END
    while [ "$1" != END ]; do
        f=$1
        shift
        case $f in
            *=*) name=${f%%=*}; src=${f#*=} ;;
            *) name=$(basename "$f"); src=$f ;;
        esac
        if [ "$(head -c 4 "$src" | od -An -c | tr -d ' ')" = "177ELF" ]; then
            strip -o "$B/pkgdata/$name" "$src"
            src=$B/pkgdata/$name
        fi
        set -- "$@" "$name=$src"
    done
    shift
    python3 "$SDK/tools/bpkg" pack -o "$out" "$B/manifest" "$B/app-x86_64" "$@" >/dev/null
    python3 "$SDK/tools/bpkg" check "$out"
}
