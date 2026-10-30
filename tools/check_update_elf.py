#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check exact executable bytes across install/update and repeated disk boots."""
import argparse
from pathlib import Path
import re

def hashes(path):
    text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', path.read_text(errors='replace'))
    found = {}
    for digest, name in re.findall(r'(?m)^\s*([0-9a-f]{64})\s+(/[^\r\n]+)', text):
        name = name.strip()
        if name in found and found[name] != digest:
            raise RuntimeError(f'{path}: inconsistent hashes for {name}')
        found[name] = digest
    return found

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path)
    args = parser.parse_args()
    before = hashes(args.logs / 'serial-diskboot.log')
    candidate = hashes(args.logs / 'serial-update.log')
    after = hashes(args.logs / 'serial-after-update.log')
    reboot = hashes(args.logs / 'serial-preferences.log')
    frozen = '/home/banana/ci-old-bash'
    if frozen not in before:
        raise RuntimeError('missing pre-update frozen ELF hash')
    for label, result in [('updated disk', after), ('cold reboot', reboot)]:
        if result.get(frozen) != before[frozen]:
            raise RuntimeError(f'{label}: preserved old ELF bytes changed')
        for name in ['/bin/bash', '/bin/echo', '/apps/terminal/app-x86_64']:
            if name not in candidate or result.get(name) != candidate[name]:
                raise RuntimeError(f'{label}: {name} differs from the candidate ISO')
    print('ELF update: frozen executable preserved; installed binaries match ISO after update and cold boot')

if __name__ == '__main__':
    main()
