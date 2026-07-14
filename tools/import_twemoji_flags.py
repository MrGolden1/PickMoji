#!/usr/bin/env python3
"""Copy only Twemoji flag PNGs used by the embedded Unicode catalogue."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


def codepoints(value: str) -> list[int]:
    return [ord(character) for character in value]


def twemoji_name(value: str) -> str:
    points = codepoints(value)
    # Twemoji removes VS16 only from non-ZWJ asset names.
    if 0x200D not in points:
        points = [point for point in points if point != 0xFE0F]
    return "-".join(f"{point:x}" for point in points)


def embedded_name(value: str) -> str:
    return "-".join(f"{point:x}" for point in codepoints(value))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("catalogue", type=Path)
    parser.add_argument("twemoji_png_directory", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    catalogue = json.loads(args.catalogue.read_text(encoding="utf-8"))
    flags = [entry for entry in catalogue["entries"] if entry["g"] == "Flags"]
    args.destination.mkdir(parents=True, exist_ok=True)

    copied = 0
    missing: list[str] = []
    for entry in flags:
        emoji = entry["e"]
        candidates = [
            args.twemoji_png_directory / f"{twemoji_name(emoji)}.png",
            args.twemoji_png_directory / f"{embedded_name(emoji)}.png",
        ]
        source = next((candidate for candidate in candidates if candidate.exists()), None)
        if source is None:
            missing.append(f"{entry['n']} ({emoji})")
            continue
        shutil.copyfile(source, args.destination / f"{embedded_name(emoji)}.png")
        copied += 1

    print(f"copied {copied}/{len(flags)} Twemoji flag assets")
    if missing:
        print("missing:")
        print("\n".join(missing))
        raise SystemExit(1)


if __name__ == "__main__":
    main()
