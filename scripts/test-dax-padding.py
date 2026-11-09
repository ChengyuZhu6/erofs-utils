#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
"""Check block sizes and 4 KiB images with 16 KiB DAX data alignment."""
import pathlib
import re
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


mkfs, dump, fsck = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="erofs-dax-padding-") as tmp:
    root = pathlib.Path(tmp)
    src = root / "src"
    src.mkdir()
    sizes = (0, 1, 4095, 4096, 4097, 16383, 16384, 16385, 65539)
    for seed, size in enumerate(sizes):
        (src / f"file-{size:05d}").write_bytes(
            bytes((i * 17 + (i >> 8) + seed) % 251 for i in range(size))
        )
    # Force inode metadata across block boundaries between data allocations.
    for seed in range(128):
        size = (1, 4096, 4097, 16385)[seed % 4]
        (src / f"extra-{seed:03d}").write_bytes(
            bytes((i * 17 + seed) % 251 for i in range(size))
        )
    invalid = root / "invalid.erofs"
    for blocksize in (511, 513, 49152, 131072):
        invalid.write_bytes(b"preserve existing output")
        result = subprocess.run(
            [mkfs, f"-b{blocksize}", str(invalid), str(src)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        assert result.returncode != 0, (blocksize, result.stdout)
        assert "invalid block size" in result.stdout, (blocksize, result.stdout)
        assert invalid.read_bytes() == b"preserve existing output", blocksize
    print("PASS invalid block sizes rejected without changing existing output")
    for workers in (1, 4):
        image = root / f"image-{workers}.erofs"
        image.write_bytes(b"stale-data-must-not-survive" * 65536)
        run(mkfs, "-b4096", "-E^inline_data", "--dsunit=4", "-x-1",
            f"--workers={workers}", str(image), str(src))
        data = image.read_bytes()
        assert data[1036] == 12, "filesystem blocks must remain 4 KiB"
        ranges = []
        for f in sorted(src.iterdir()):
            size = f.stat().st_size
            if not size:
                continue
            info = run(dump, "-e", f"--path=/{f.name}", str(image))
            match = re.search(r"^\s*0:\s+0\.\.\s*\d+\s*\|\s*\d+\s*:\s*(\d+)\.\.",
                              info, re.MULTILINE)
            assert match, info
            start = int(match[1])
            end = start + (size + 16383) // 16384 * 16384
            assert start % 16384 == 0, (f.name, start)
            assert end <= len(data), (f.name, "padding outside image")
            assert data[start:start + size] == f.read_bytes(), f.name
            assert not any(data[start + size:end]), (f.name, "nonzero DAX tail")
            ranges.append((start, end, f.name))
        ranges.sort()
        for left, right in zip(ranges, ranges[1:]):
            assert left[1] <= right[0], (left, right, "neighbor overlaps DAX tail")
        out = root / f"out-{workers}"
        run(fsck, f"--extract={out}", str(image))
        for f in src.iterdir():
            assert f.read_bytes() == (out / f.name).read_bytes(), f.name
        print(f"PASS workers={workers}: {len(ranges)} files, alignment, zero tails, neighbors, extraction")
