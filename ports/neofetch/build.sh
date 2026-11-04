#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# neofetch 7.1.0 (Dylan Araps, MIT): the real bash script, with a Banana OS
# backend (banana.sh: its facts from the sysinfo command, the Banana OS logo)
# put in before it starts. A small launcher runs it with bash.
[ -n "$1" ] || { echo "usage: $0 <output.bpk>"; exit 1; }
OUT=$(realpath -m "$1")
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../port.sh"
port_fetch_git neofetch 7.1.0 https://github.com/dylanaraps/neofetch 7.1.0 60d07dee6b76769d8c487a40639fb7b5a1a7bc85
mkdir -p "$B"
# the script, with banana.sh just before its last line (main "$@")
tail -n 1 "$SRC/neofetch" | grep -qx 'main "$@"' || { echo "neofetch: its last line is not main \"\$@\""; exit 1; }
{ head -n -1 "$SRC/neofetch"; cat "$HERE/banana.sh"; echo 'main "$@"'; } > "$B/neofetch"
bash -n "$B/neofetch"
"$SDK/tools/banana-cc" -O2 -o "$B/neofetch-launcher" "$HERE/launcher.c"
port_package "$OUT" name=neofetch title=neofetch version=7.1.0 type=console \
    "description=System information with the Banana OS logo (the real neofetch, bash)" \
    "author=Dylan Araps" "category=System" bin=neofetch \
    -- "$B/neofetch-launcher" "neofetch.sh=$B/neofetch" "LICENSE=$SRC/LICENSE.md"
