#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""APT sources for CI: archive.ubuntu.com first (the Azure mirror GitHub's
Ubuntu runners pick is often slow), the Azure mirror as a fallback when the
archive does not answer (runs 357 and 358 failed on "Connection failed" from
archive.ubuntu.com alone), and retries for each download."""
import argparse
from pathlib import Path
import re

ARCHIVE = 'https://archive.ubuntu.com/ubuntu'
SECURITY = 'https://security.ubuntu.com/ubuntu'
AZURE = 'http://azure.archive.ubuntu.com/ubuntu'
MIRRORS = 'etc/apt/banana-mirrors.txt'
SECURITY_MIRRORS = 'etc/apt/banana-mirrors-security.txt'
RETRIES = 'etc/apt/apt.conf.d/80banana-retries'


def normalize(text):
    """Every Ubuntu archive source becomes the mirror list (tried in order)."""
    text = text.replace('mirror+file:/etc/apt/apt-mirrors-security.txt',
                        'mirror+file:/' + SECURITY_MIRRORS)
    text = text.replace('mirror+file:/etc/apt/apt-mirrors.txt',
                        'mirror+file:/' + MIRRORS)
    text = re.sub(r'https?://(azure\.)?archive\.ubuntu\.com/ubuntu/?(?=[\s"]|$)',
                  'mirror+file:/' + MIRRORS, text, flags=re.M)
    text = re.sub(r'https?://security\.ubuntu\.com/ubuntu/?(?=[\s"]|$)',
                  'mirror+file:/' + SECURITY_MIRRORS, text, flags=re.M)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/'))
    args = parser.parse_args()
    root = args.root
    apt = root / 'etc/apt'
    files = [apt / 'sources.list']
    files += sorted((apt / 'sources.list.d').glob('*.list'))
    files += sorted((apt / 'sources.list.d').glob('*.sources'))
    changed = 0
    for path in files:
        if not path.is_file():
            continue
        original = path.read_text()
        updated = normalize(original)
        if updated != original:
            path.write_text(updated)
            changed += 1
    (root / MIRRORS).parent.mkdir(parents=True, exist_ok=True)
    (root / MIRRORS).write_text(f'{ARCHIVE}\tpriority:1\n{AZURE}\tpriority:2\n')
    (root / SECURITY_MIRRORS).write_text(f'{SECURITY}\tpriority:1\n{AZURE}\tpriority:2\n')
    (root / RETRIES).parent.mkdir(parents=True, exist_ok=True)
    (root / RETRIES).write_text('Acquire::Retries "5";\n'
                                'Acquire::http::Timeout "30";\n'
                                'Acquire::https::Timeout "30";\n')
    print(f'APT source normalization: {changed} files updated '
          f'(archive.ubuntu.com, falling back to the Azure mirror; 5 retries)')


if __name__ == '__main__':
    main()
