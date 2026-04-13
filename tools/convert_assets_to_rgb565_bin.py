#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import sys

from PIL import Image


def rgba_to_rgb565le(
    img: Image.Image,
    invert: bool = False,
    transparent_black: bool = False,
) -> bytes:
    rgba = img.convert("RGBA")
    out = bytearray()
    pixels = rgba.load()
    for y in range(rgba.height):
        for x in range(rgba.width):
            r, g, b, a = pixels[x, y]
            if a == 0:
                if transparent_black:
                    r = g = b = 0
                else:
                    r = g = b = 255
            elif invert:
                r = 255 - r
                g = 255 - g
                b = 255 - b
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out.append(value & 0xFF)
            out.append((value >> 8) & 0xFF)
    return bytes(out)


def convert_one(
    src: Path,
    overwrite: bool,
    output_dir: Path | None,
    invert: bool,
    transparent_black: bool,
) -> tuple[bool, str]:
    try:
        with Image.open(src) as img:
            width, height = img.size
            if width != height:
                return False, f"skipped {src.name}: image must be square, got {width}x{height}"

            payload = rgba_to_rgb565le(
                img,
                invert=invert,
                transparent_black=transparent_black,
            )
    except Exception as exc:  # pragma: no cover - operational error path
        return False, f"failed {src.name}: {exc}"

    dst = (output_dir / f"{src.stem}.bin") if output_dir else src.with_suffix(".bin")
    if dst.exists() and not overwrite:
        return False, f"skipped {src.name}: {dst.name} already exists"

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(payload)
    return True, f"wrote {dst.name} ({len(payload)} bytes from {width}x{height})"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert every PNG in an assets folder to raw RGB565 little-endian .bin files."
    )
    parser.add_argument("assets_dir", type=Path, help="Folder containing PNG assets")
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Replace existing .bin files if they already exist",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Write converted .bin files into this folder instead of beside the source PNGs",
    )
    parser.add_argument(
        "--invert",
        action="store_true",
        help="Invert non-transparent PNG colors before writing the RGB565 .bin output",
    )
    parser.add_argument(
        "--transparent-black",
        action="store_true",
        help="Write transparent PNG pixels as black instead of white",
    )
    args = parser.parse_args()

    assets_dir = args.assets_dir.expanduser().resolve()
    if not assets_dir.is_dir():
        print(f"Assets folder not found: {assets_dir}", file=sys.stderr)
        return 1

    output_dir = args.output_dir.expanduser().resolve() if args.output_dir else None

    png_files = sorted(p for p in assets_dir.iterdir() if p.is_file() and p.suffix.lower() == ".png")
    if not png_files:
        print(f"No PNG files found in {assets_dir}")
        return 0

    converted = 0
    for png in png_files:
        ok, msg = convert_one(
            png,
            args.overwrite,
            output_dir,
            args.invert,
            args.transparent_black,
        )
        print(msg)
        if ok:
            converted += 1

    if output_dir:
        print(f"Converted {converted} of {len(png_files)} PNG files from {assets_dir} into {output_dir}")
    else:
        print(f"Converted {converted} of {len(png_files)} PNG files in {assets_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
