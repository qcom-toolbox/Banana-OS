#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline checks of packaged CC0 provenance, checksums and high-resolution JPEG bounds."""
from pathlib import Path
import hashlib,json,re,struct
root=Path(__file__).resolve().parents[1]
assets=root/'assets/wallpapers/landscapes'
rows=json.loads((assets/'manifest.json').read_text())
assert len(rows)==3
total=0
header=(root/'kernel/landscape_data.h').read_text()
assembly=(root/'kernel/landscapebin.asm').read_text()
for row in rows:
    assert row['license']=='CC0-1.0' and row['license_url']=='https://creativecommons.org/publicdomain/zero/1.0/'
    assert row['author'] and row['source_page'].startswith('https://') and len(row['source_sha256'])==64
    raw=(assets/row['file']).read_bytes()
    assert hashlib.sha256(raw).hexdigest()==row['sha256'] and len(raw)==row['bytes']
    assert raw[:2]==b'\xff\xd8'
    p=2;size=None
    while p<len(raw):
        assert raw[p]==255
        while raw[p]==255:p+=1
        marker=raw[p];p+=1
        if marker in [0xd8,0xd9]:continue
        n=struct.unpack_from('>H',raw,p)[0]
        assert n>=2 and p+n<=len(raw)
        if marker in [0xc0,0xc1,0xc2]:
            h,w=struct.unpack_from('>HH',raw,p+3);size=[w,h];break
        p+=n
    assert size==row['dimensions']==[3200,1800] and size[0]*size[1]<=8000000
    assert row['source_dimensions'][0]>=3200 and row['source_dimensions'][1]>=1800
    ident='landscape_'+row['id'].replace('-','_')
    assert f'{ident}_jpeg[{len(raw)}]' in header
    assert ident+'_jpeg: incbin "assets/wallpapers/landscapes/'+row['file']+'"' in assembly
    total+=len(raw)
assert total<3*1024*1024
print(f'CC0 landscape assets: 3 verified 3200x1800 JPEGs; {total} bytes, offline provenance/hash/bounds passed')
