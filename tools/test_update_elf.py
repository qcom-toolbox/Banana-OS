#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ensure update verification rejects changed, missing and inconsistent ELF hashes."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
names = ['/bin/bash', '/bin/echo', '/apps/terminal/app-x86_64']
frozen = '/home/banana/ci-old-bash'
with tempfile.TemporaryDirectory() as temp:
    logs = Path(temp)
    data = {
        'serial-diskboot.log': {frozen: 'a' * 64},
        'serial-update.log': {name: str(i + 1) * 64 for i, name in enumerate(names)},
        'serial-after-update.log': {frozen: 'a' * 64, **{name: str(i + 1) * 64 for i, name in enumerate(names)}},
        'serial-preferences.log': {frozen: 'a' * 64, **{name: str(i + 1) * 64 for i, name in enumerate(names)}},
    }
    def run(changed_file=None, changed_name=None, replacement=None, extra=''):
        for filename, values in data.items():
            rows = dict(values)
            if filename == changed_file:
                if replacement is None:
                    rows.pop(changed_name, None)
                else:
                    rows[changed_name] = replacement
            text = ''.join(digest + '  ' + name + '\n' for name, digest in rows.items())
            if filename == changed_file:
                text += extra
            (logs / filename).write_text(text)
        return subprocess.run([sys.executable, str(root / 'tools/check_update_elf.py'), str(logs)],
                              capture_output=True).returncode
    assert run() == 0
    cases = 1
    for filename, values in data.items():
        for name in values:
            assert run(filename, name) != 0
            assert run(filename, name, 'f' * 64) != 0
            cases += 2
    assert run('serial-after-update.log', frozen, 'a' * 64,
               'f' * 64 + '  ' + frozen + '\n') != 0
    cases += 1
print(f'Update ELF verifier: {cases} checks passed')
