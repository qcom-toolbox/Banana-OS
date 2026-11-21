#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# kernel/ext4.c built for Linux (stub/: the kernel's helpers as libc) and
# checked against Linux's own e2fsprogs: a filesystem made and used, then
# e2fsck -fn on it; and power cuts at various transaction stages, EIO injection,
# and ENOSPC handling.
# Needs gcc and e2fsprogs (e2fsck, dumpe2fs). Usage: sh run.sh [MB]
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT HUP INT TERM
cp ../../kernel/ext4.c ../../kernel/ext4.h stub/*.h "$T"/
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie -Wall -Wno-unused -I "$T" -o "$T/host" host.c "$T/ext4.c"
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie -Wall -Wno-unused -I "$T" -o "$T/crash" crash.c "$T/ext4.c"
E2FSCK=$(command -v e2fsck || echo /sbin/e2fsck)

echo "== made, used, mounted again (${1:-300} MB)"
"$T/host" "$T/fs.img" "${1:-300}"
"$E2FSCK" -fn "$T/fs.img"

echo "== a cut after the commit block (stage 4): replayed by the mount"
"$T/crash" "$T/c.img" 0 4
"$T/crash" "$T/c.img" 1 4
"$E2FSCK" -fn "$T/c.img"

echo "== the same, replayed by e2fsck"
"$T/crash" "$T/c.img" 0 4
rc=0
"$E2FSCK" -fy "$T/c.img" >"$T/replay.log" 2>&1 || rc=$?
cat "$T/replay.log"
test "$rc" -le 1
"$E2FSCK" -fn "$T/c.img"
"$T/crash" "$T/c.img" 1 4

echo "== a cut before descriptor (stage 1): as if it never began"
"$T/crash" "$T/c.img" 0 1
"$T/crash" "$T/c.img" 1 1
"$E2FSCK" -fn "$T/c.img"

echo "== a cut before the commit block (stage 3): uncommitted transaction dropped"
"$T/crash" "$T/c.img" 0 3
"$T/crash" "$T/c.img" 1 3
"$E2FSCK" -fn "$T/c.img"

echo "== I/O error injection: device read, write, and flush errors"
"$T/crash" "$T/io.img" 3
"$E2FSCK" -fn "$T/io.img"

echo "== ENOSPC condition: fill disk, verify anchor intact, free space, verify write succeeds"
"$T/crash" "$T/spc.img" 2
"$E2FSCK" -fn "$T/spc.img"

echo "== an installed disk after many updates: packages replaced, small files churned, remounts"
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie -Wall -Wno-unused -I "$T" -o "$T/aging" aging.c "$T/ext4.c"
echo "== a file's blocks marked free in the bitmap: found and marked used (ext4_scrub)"
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie -Wall -Wno-unused -I "$T" -o "$T/scrub" scrub.c "$T/ext4.c"
"$T/scrub" "$T/scrub.img" 64
"$E2FSCK" -fn "$T/scrub.img"
for seed in 1 2; do
    "$T/aging" "$T/age.img" 160 200 "$seed"
    "$E2FSCK" -fn "$T/age.img"
done

rm -rf "$T"
echo "ext4test: all passed"
