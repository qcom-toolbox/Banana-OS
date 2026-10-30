#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run full system and offline application regressions in a disposable live VM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--32', dest='bits32', action='store_true')
    parser.add_argument('--firmware', choices=['bios', 'uefi'], default='bios')
    parser.add_argument('--iso', type=Path, default=ROOT / 'Banana_OS.iso')
    parser.add_argument('--memory', default='1G')
    parser.add_argument('--mesa', action='store_true', help='also verify the optional Mesa runtime')
    args = parser.parse_args()
    if args.bits32 and (args.firmware == 'uefi' or args.mesa):
        parser.error('UEFI and Mesa require the 64-bit kernel')
    label = args.firmware + ('-32' if args.bits32 else '-64')
    out = ROOT / 'build/logs' / ('os-' + label)
    out.mkdir(parents=True, exist_ok=True)
    qemu = ['-accel', 'tcg', '-m', args.memory, '-smp', '2', '-nic', 'none',
            '-cdrom', str(args.iso.resolve()), '-boot', 'd']
    if args.bits32:
        qemu += ['-cpu', 'qemu32']
    if args.firmware == 'uefi':
        variables = out / 'OVMF_VARS.fd'
        variables.write_bytes(Path(os.environ.get('OVMF_VARS', '/usr/share/OVMF/OVMF_VARS_4M.fd')).read_bytes())
        qemu += ['-fw_cfg', 'name=opt/ovmf/X-PciMmio64Mb,string=0',
                 '-drive', 'if=pflash,format=raw,readonly=on,file=' +
                 os.environ.get('OVMF_CODE', '/usr/share/OVMF/OVMF_CODE_4M.fd'),
                 '-drive', 'if=pflash,format=raw,file=' + str(variables)]
    commands = ['apt install posixtest', 'posixtest sockets epoll ## 0 failed',
                'selftest ## 0 failed', 'posixtest ## 0 failed']
    if not args.bits32:
        commands += ['apt install shelltest',
                     'shelltest -s ## shelltest: all passed (0 failed)',
                     'shelltest -i ## shelltest: all passed (0 failed)',
                     'shelltest -e ## shelltest: all passed (0 failed)',
                     'selftest browser libjsdom ## 30 passed, 0 failed',
                     'selftest browser isolation ## 0 failed']
        if args.mesa:
            commands += ['glinfo ## glinfo: every check passed']
        commands += ["bash -c 'gpu3d on; gpu3d off; /bin/echo os-exec-OK' ## os-exec-OK"]
    commands += ['drop_caches ## drop_caches:', 'echo os-audit-OK ## os-audit-OK']
    command_file = out / 'commands.txt'
    command_file.write_text('\n'.join(commands) + '\n')
    with args.iso.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    sources = ['kernel/app.c', 'kernel/config.c', 'net/net.c', 'net/ip.c',
               'net/tcp.c', 'web/framework_image_tests.h']
    (out / 'provenance.json').write_text(json.dumps({
        'commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'sources_sha256': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in sources},
        'iso_sha256': digest, 'bits': 32 if args.bits32 else 64,
        'firmware': args.firmware, 'qemu_args': qemu,
    }, indent=2) + '\n')
    return subprocess.run([sys.executable, 'tools/serialtest.py',
                           '--timeout', '360', '--boot-timeout', '180',
                           '--log', str(out / 'serial.log'),
                           '--cmds-file', str(command_file), '--', *qemu], cwd=ROOT).returncode

if __name__ == '__main__':
    sys.exit(main())
