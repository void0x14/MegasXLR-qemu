#!/usr/bin/env python3
"""Check a serial log and QEMU HMP PPM capture from the standalone Doom proof."""

import argparse
from pathlib import Path
import re
import sys


MARKERS = (
    b"virtio_gpudrmfb",
    b"DOOMGENERIC_LINUXVT_INIT_START",
    b"DOOMGENERIC_LINUXVT_EXEC",
    b"Freedoom: Phase 2",
    b"I_InitGraphics: DOOM screen size: w x h: 320 x 200",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial_log", type=Path)
    parser.add_argument("screenshot_ppm", type=Path)
    args = parser.parse_args()

    log = args.serial_log.read_bytes()
    missing = [marker.decode() for marker in MARKERS if marker not in log]
    if missing:
        print("missing serial proof markers: " + ", ".join(missing), file=sys.stderr)
        return 1

    frame = args.screenshot_ppm.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", frame)
    if not header:
        print("screenshot is not a raw P6 PPM with 8-bit channels", file=sys.stderr)
        return 1
    width, height = map(int, header.groups())
    pixel_start = header.end()
    if (width, height) != (1280, 800) or len(frame) - pixel_start != width * height * 3:
        print("screenshot has unexpected dimensions or pixel payload", file=sys.stderr)
        return 1
    pixels = frame[pixel_start:]
    if len(set(pixels[::3])) < 16:
        print("screenshot has too few distinct pixel values", file=sys.stderr)
        return 1

    print(f"PASS: serial markers present; captured {width}x{height} QEMU frame")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
