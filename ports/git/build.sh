#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Git 2.47.1 for Banana OS (/bin/git), an optional package (apt install git):
# HTTPS remotes through libcurl on mbedTLS (git-remote-https in its exec
# path, /apps/git), zlib, and Mozilla's CA certificates (/apps/git/cacert.pem);
# its system configuration is /apps/git/gitconfig (ports/git/gitconfig). The
# commands Git runs by their dashed names (git-upload-pack, ...) are links in
# /bin, as a Git installation's hard links are.
# Everything is linked statically; no Perl, Python, Tcl or gettext, so the
# commands written in those (git send-email, git svn, gitk, ...) are left out.
#     ports/git/build.sh <output.bpk>
[ -n "$1" ] || { echo "usage: $0 <output.bpk>"; exit 1; }
OUT=$(realpath -m "$1")
. "$(dirname "$0")/../port.sh"
CC=$SDK/tools/banana-cc

# fetch name version sha256 url: an archive of a library, into ports/git/src
fetch() {
    local file=$PORT_DIR/src/$(basename "$4")
    mkdir -p "$PORT_DIR/src"
    [ -f "$file" ] || { curl -fsSL --retry 4 --retry-delay 10 --retry-all-errors --connect-timeout 30 \
                             -o "$file.part" "$4" && mv "$file.part" "$file"; }
    echo "$3  $file" | sha256sum -c - >/dev/null
    [ -d "$PORT_DIR/src/$1-$2" ] || tar -C "$PORT_DIR/src" -xf "$file"
}
fetch zlib 1.3.1 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 \
    https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
fetch mbedtls 3.6.2 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca \
    https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
fetch curl 8.11.1 c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56 \
    https://curl.se/download/curl-8.11.1.tar.xz
CACERT=$PORT_DIR/src/cacert-2024-11-26.pem
[ -f "$CACERT" ] || curl -fsSL --retry 4 -o "$CACERT" https://curl.se/ca/cacert-2024-11-26.pem
echo "bb1782d281fe60d4a2dcf41bc229abe3e46c280212597d4abcc25bddf667739b  $CACERT" | sha256sum -c - >/dev/null
port_fetch git 2.47.1 f3d8f9bb23ae392374e91cd9d395970dabc5b9c5ee72f39884613cd84a6ed310 \
    https://www.kernel.org/pub/software/scm/git/git-2.47.1.tar.xz

# the libraries, built again when the toolchain or the SDK's headers change
D=$B/deps
SUM=$( (echo 1; cat "$0" "$CC"; find "$SDK/include" -name '*.h' | LC_ALL=C sort | xargs cat) | sha256sum | cut -d" " -f1)
if [ "$(cat "$B/.deps" 2>/dev/null)" != "$SUM" ]; then
    rm -rf "$B"
    mkdir -p "$B" "$D/lib" "$D/include"
    cp -a "$PORT_DIR/src/zlib-1.3.1" "$PORT_DIR/src/mbedtls-3.6.2" "$PORT_DIR/src/curl-8.11.1" "$B/"
    cd "$B/zlib-1.3.1"
    { CC=$CC CFLAGS="-O2 -g0" ./configure --static --prefix="$D" && make -j"$(nproc)" libz.a && make install; } \
        >build.log 2>&1 || { tail -30 build.log; exit 1; }
    # (mbedTLS's own sockets, net_sockets.c, are not used: curl has its own)
    cd "$B/mbedtls-3.6.2"
    { python3 scripts/config.py unset MBEDTLS_NET_C &&
      make -C library -j"$(nproc)" CC="$CC" CFLAGS="-O2 -g0" static; } >build.log 2>&1 || { tail -30 build.log; exit 1; }
    cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a "$D/lib/"
    cp -r include/mbedtls include/psa "$D/include/"
    # (curl.h wants fd_set without sys/select.h, which glibc's sys/time.h
    #  brings in; the path MTU constant for its UDP sockets is Linux's)
    cd "$B/curl-8.11.1"
    BANANA_ARCH=x86_64 CFLAGS="-O2 -g0" CPPFLAGS="-include sys/select.h -DIP_PMTUDISC_DO=2" "$SDK/tools/banana-configure" ./configure --disable-shared --enable-static \
        --prefix="$D" --with-mbedtls="$D" --with-zlib="$D" --without-libpsl --without-brotli --without-zstd \
        --without-nghttp2 --without-libidn2 --without-librtmp --without-libssh2 --disable-ldap --disable-ldaps \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp \
        --disable-gopher --disable-mqtt --disable-smb --disable-threaded-resolver --disable-docs --disable-manual \
        --with-ca-bundle=/apps/git/cacert.pem --without-ca-path >build.log 2>&1 || { tail -30 build.log; exit 1; }
    { make -j"$(nproc)" -C lib && make -C lib install && make -C include install; } >>build.log 2>&1 \
        || { grep -E "error|undefined" build.log | head -30; exit 1; }
    echo "$SUM" > "$B/.deps"
fi

# git itself: its Makefile, told what this system has (uname_S Banana: none
# of the Linux settings, which assume glibc)
G=$B/git-2.47.1
[ -d "$G" ] || cp -a "$SRC" "$G"
cat > "$G/config.mak" <<EOF
CC = $CC
AR = ar
CFLAGS = -O2 -g0 -include sys/select.h
prefix = /usr
bindir = /bin
gitexecdir = /apps/git
template_dir = /usr/share/git-core/templates
sysconfdir = /etc
ETC_GITCONFIG = /apps/git/gitconfig
NO_PERL = YesPlease
NO_PYTHON = YesPlease
NO_TCLTK = YesPlease
NO_GETTEXT = YesPlease
NO_EXPAT = YesPlease
NO_ICONV = YesPlease
NO_INSTALL_HARDLINKS = YesPlease
NO_REGEX = YesPlease
NO_GITWEB = YesPlease
NO_OPENSSL = YesPlease
NO_PTHREADS = YesPlease
NO_MEMMEM = YesPlease
NO_STRCASESTR = YesPlease
NO_STRLCPY = YesPlease
NO_POSIX_GOODIES = UnfortunatelyYes
HAVE_ALLOCA_H = YesPlease
HAVE_DEV_TTY = YesPlease
HAVE_CLOCK_GETTIME = YesPlease
HAVE_CLOCK_MONOTONIC = YesPlease
HAVE_GETDELIM = YesPlease
FREAD_READS_DIRECTORIES = UnfortunatelyYes
ZLIB_PATH = $D
CURLDIR = $D
NO_CURL_CONFIG = YesPlease
CURL_LDFLAGS = -L$D/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
EOF
cd "$G"
port_relink
make -j"$(nproc)" uname_S=Banana uname_M=x86_64 uname_R=1 uname_O=Banana uname_P=x86_64 uname_V=1 \
    git git-remote-http >make.log 2>&1 \
    || { grep -E "error:|Error [0-9]|undefined reference|cannot find" make.log | head -40; exit 1; }

# the templates of git init (hooks samples, info/exclude, description): its
# templates/ folder names them dir--file, dir-- an empty folder
rm -f "$B/templates.zip"
python3 - "$G/templates" "$B/templates.zip" <<'PY2'
import os, sys, zipfile
top = sys.argv[1]
z = zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED)
for f in sorted(os.listdir(top)):
    if "--" not in f:
        continue
    name = f.replace("--", "/")
    if name.endswith("/"):
        z.writestr(zipfile.ZipInfo(name), b"")
    else:
        data = open(os.path.join(top, f), "rb").read()
        if name.endswith(".sample"):
            data = data.replace(b"@PERL_PATH@", b"/usr/bin/perl").replace(b"@SHELL_PATH@", b"/bin/sh")
        z.writestr(name, data)
z.close()
PY2
B=$G                                               # (port_package works here)
port_package "$OUT" name=git "title=Git" version=$PORT_VER type=console \
    "description=Git: distributed version control - clone, commit, branch and push over HTTPS (/bin/git)" \
    "author=Linus Torvalds, Junio C Hamano and the Git developers" "category=Developer Tools" \
    bin=git,git-upload-pack,git-receive-pack,git-upload-archive,git-remote-https \
    unpack=templates.zip:/usr/share/git-core/templates \
    -- git git-remote-https=git-remote-http \
    "cacert.pem=$CACERT" "gitconfig=$PORT_DIR/gitconfig" "templates.zip=$B/../templates.zip"
