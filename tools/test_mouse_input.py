#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run actual pointer arbitration with only privileged I/O/IRQ instructions shimmed."""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--32', dest='bits32', action='store_true')
p.add_argument('--sanitize', action='store_true')
p.add_argument('--source', type=Path, default=root / 'kernel/usb.c',
               help='source revision to exercise (for baseline reproductions)')
a = p.parse_args()
out = root / 'build/test/mouse-input'
out.mkdir(parents=True, exist_ok=True)
source = a.source.read_text()
start = source.index('static inline uint8_t  inb ')
end = source.index('static char usb_status_buf', start)
source = source[:start] + """
static inline uint8_t inb(uint16_t p){(void)p;return 0;}
static inline uint16_t inw(uint16_t p){(void)p;return 0;}
static inline uint32_t inl(uint16_t p){(void)p;return 0;}
static inline void outb(uint16_t p,uint8_t v){(void)p;(void)v;}
static inline void outl(uint16_t p,uint32_t v){(void)p;(void)v;}
""" + source[end:]
irq = '__asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");'
sti = '__asm__ volatile("sti" ::: "memory");'
assert source.count(irq) >= 6 and source.count(sti) >= 6
source = source.replace(irq, 'f = 0;').replace(sti, '(void)f;')
(out / 'input-under-test.c').write_text(source)
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-ffunction-sections', '-fdata-sections', '-Ikernel', '-Iusb',
         '-I' + str(out), '-include', 'web/test/shim/types.h']
if a.bits32:
    flags += ['-m32']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-fno-pie', '-no-pie']
exe = out / 'test'
subprocess.run(['cc', *flags, 'tools/tests/mouse_input.c', '-Wl,--gc-sections',
                '-o', str(exe)], cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)
