#!/usr/bin/env python3
"""Generate the embedded emoji catalogue from Unicode emoji-test.txt."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


LINE_RE = re.compile(
    r"^(?P<codepoints>[0-9A-F ]+)\s*;\s*(?P<status>[a-z-]+)\s*#\s*"
    r"(?P<emoji>\S+)\s+E(?P<version>[0-9.]+)\s+(?P<name>.+)$"
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    group = ""
    subgroup = ""
    entries: list[dict[str, str]] = []
    seen: set[str] = set()

    for raw_line in args.source.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line.startswith("# group:"):
            group = line.split(":", 1)[1].strip()
            continue
        if line.startswith("# subgroup:"):
            subgroup = line.split(":", 1)[1].strip()
            continue

        match = LINE_RE.match(line)
        if not match or match.group("status") != "fully-qualified":
            continue

        emoji = match.group("emoji")
        if emoji in seen:
            continue
        seen.add(emoji)
        entries.append(
            {
                "e": emoji,
                "n": match.group("name"),
                "g": group,
                "s": subgroup,
                "v": match.group("version"),
            }
        )

    payload = {
        "unicodeVersion": "17.0",
        "source": "https://www.unicode.org/Public/17.0.0/emoji/emoji-test.txt",
        "entries": entries,
    }
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_text(
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
        encoding="utf-8",
    )
    print(f"generated {len(entries)} fully-qualified emoji entries")


if __name__ == "__main__":
    main()
