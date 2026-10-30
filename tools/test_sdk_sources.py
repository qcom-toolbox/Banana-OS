#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Ensure SDK pattern rules never delete source inputs after make -o."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
sdk = repo/'sdk'
with tempfile.TemporaryDirectory(prefix='banana-sdk-inputs-') as name:
    root = Path(name)
    copied = root/'sdk'
    copied.mkdir()
    (copied/'lib').mkdir()
    shutil.copytree(sdk/'include', copied/'include')
    source = copied/'lib/malloc.c'
    shutil.copyfile(sdk/'lib/malloc.c', source)
    rules = (sdk/'banana.mk').read_text()
    (copied/'banana.mk').write_text(rules)
    project = root/'example'
    project.mkdir()
    (project/'Makefile').write_text(
        'APP = input-check\nSRCS = main.c\ninclude ../sdk/banana.mk\n')
    (project/'main.c').write_text('int main(void) { return 0; }\n')
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    targets = ['build/i686/sdk_malloc.o', 'build/x86_64/sdk_malloc.o']
    command = ['make', '--no-print-directory', '-o', '../sdk/lib/malloc.c', *targets]

    # Prove that the original rule set really schedules removal. All operations
    # occur in this isolated SDK copy, and this phase is a dry run.
    unguarded = '\n'.join(line for line in rules.splitlines()
                          if not line.startswith('.PRECIOUS:'))
    (copied/'banana.mk').write_text(unguarded)
    old = subprocess.run([*command, '-n'], cwd=project, check=True,
                         text=True, stdout=subprocess.PIPE).stdout
    assert 'rm ../sdk/lib/malloc.c' in old, old
    assert source.is_file()

    (copied/'banana.mk').write_text(rules)
    dry = subprocess.run([*command, '-n'], cwd=project, check=True,
                         text=True, stdout=subprocess.PIPE).stdout
    assert 'rm ../sdk/lib/malloc.c' not in dry, dry
    # Compile the actual allocator for both architectures, then repeat an
    # incremental build. Verify bytes, not just whether make returned success.
    for unused in range(2):
        subprocess.run(command, cwd=project, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert source.is_file()
        assert hashlib.sha256(source.read_bytes()).hexdigest() == before
    assert all((project/t).stat().st_size > 0 for t in targets)
print('SDK source preservation: reproduced old cleanup; 32/64-bit builds retain allocator input')
