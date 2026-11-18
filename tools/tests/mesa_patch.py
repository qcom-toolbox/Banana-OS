#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Ensure a pristine pinned Mesa tree links the Intel drivers and r600, idempotently."""
from pathlib import Path
import hashlib
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
archive = ROOT / "ports/mesa/src/mesa-24.3.4.tar.xz"
assert hashlib.sha256(archive.read_bytes()).hexdigest() == "e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025"
with tempfile.TemporaryDirectory(prefix="banana-mesa-patch-") as tmp:
    with tarfile.open(archive) as source:
        source.extractall(tmp, filter="data")
    tree = Path(tmp) / "mesa-24.3.4"
    subprocess.run(["python3", str(ROOT / "ports/mesa/patch.py"), str(tree)], check=True)
    target = tree / "src/gallium/targets/osmesa/meson.build"
    def snapshot():
        return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).digest()
                for p in tree.rglob("*") if p.is_file()}
    before = snapshot()
    dependencies = target.read_text().split("dependencies : [", 1)[1].split("]", 1)[0]
    for driver in ("driver_crocus", "driver_iris", "driver_r600", "driver_virgl"):
        assert driver in dependencies, f"{driver} missing from fresh-tree OSMesa dependencies"
    subprocess.run(["python3", str(ROOT / "ports/mesa/patch.py"), str(tree)], check=True)
    assert snapshot() == before, "Mesa sources changed on the second patch"
print("Mesa fresh-tree Intel and r600 linkage and patch idempotence: passed")
