#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""collect_sources.py OUTDIR PORT[:archives]... - the corresponding source of the
third-party programs built into Banana OS: what each ports/<PORT>/build.sh
fetched (its src/ folder) - the upstream archives exactly as downloaded,
checked against the SHA-256 in the script, and the trees it took from git
(as .tar.gz) - copied into OUTDIR/<PORT>/, with SOURCES-<job>.txt listing
each file, its size and SHA-256.

Together with the repository at the release's tag (git archive: the build
scripts, the patches and Banana OS itself) this is the complete source of
every program in the images and java.bpk: CI attaches it to each release as
Banana_OS-sources.tar (the GPL's and LGPL's "same place" as the binaries).

Build tools fetched only to build (the boot JDK) are left out: they are not
distributed."""
import hashlib, os, shutil, sys, tarfile

ARCHIVES = ('.tar.gz', '.tgz', '.tar.xz', '.tar.bz2', '.tar.zst', '.zip', '.pem')   # (.pem: Mozilla's CA list, its own source)
SKIP = ('OpenJDK17U-jdk_', 'temurin-', 'jdk-17')   # (the boot JDK: a build tool, not shipped)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def is_binary(path):
    """an object or a program built here (not source)"""
    if path.endswith(('.o', '.a', '.so', '.lo', '.obj')):
        return True
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return True


def add_tree(path, name, out):
    """a source tree taken from git, as name/..., without what was built in it"""
    with tarfile.open(out, 'w:gz') as tf:
        for top, dirs, files in os.walk(path):
            dirs[:] = sorted(d for d in dirs if d not in ('build', '.git'))
            rel = os.path.relpath(top, path)
            tf.add(top, arcname=os.path.normpath(os.path.join(name, rel)), recursive=False)
            for f in sorted(files):
                fp = os.path.join(top, f)
                if os.path.islink(fp) or not is_binary(fp):
                    tf.add(fp, arcname=os.path.normpath(os.path.join(name, rel, f)), recursive=False)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, ports = sys.argv[1], sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    job = os.environ.get('SOURCES_JOB', 'build')
    lines = []
    for spec in ports:
        port, _, mode = spec.partition(':')      # (PORT:archives - the archives only, not the trees unpacked from them)
        src = os.path.join('ports', port, 'src')
        if not os.path.isdir(src):
            sys.exit('collect_sources: %s has no fetched sources (ports/%s/build.sh first)' % (src, port))
        dst = os.path.join(out, port)
        os.makedirs(dst, exist_ok=True)
        names = sorted(os.listdir(src))
        archives = [n for n in names if n.endswith(ARCHIVES) and not n.startswith(SKIP)]
        for n in archives:
            shutil.copy2(os.path.join(src, n), os.path.join(dst, n))
        # trees taken from git (no archive of the same name next to them)
        for n in (names if mode != 'archives' else []):
            p = os.path.join(src, n)
            if not os.path.isdir(p) or n.endswith('.git') or n in ('build',) or n.startswith(SKIP):
                continue
            if any(a.startswith(n) for a in archives) or any(n.startswith(a.split('.tar')[0]) for a in archives):
                continue
            add_tree(p, n, os.path.join(dst, n + '.tar.gz'))
        got = sorted(os.listdir(dst))
        if not got:
            sys.exit('collect_sources: nothing fetched for %s' % port)
        for n in got:
            p = os.path.join(dst, n)
            lines.append('%s  %10d  %s/%s' % (sha256(p), os.path.getsize(p), port, n))
    with open(os.path.join(out, 'SOURCES-%s.txt' % job), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('collect_sources: %d files for %s' % (len(lines), ', '.join(ports)))


if __name__ == '__main__':
    main()
