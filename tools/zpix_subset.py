#!/usr/bin/env python3
"""
Create a minimal Zpix BDF subset for embedded usage.

Default charset:
- ASCII printable: U+0020..U+007E
- GB2312 symbol area: A1-A9 (common CJK punctuation/symbols)
- GB2312 level-1 Hanzi: B0-F7 (common simplified Chinese)
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List, Set, Tuple


def gb2312_codepoints(zone_start: int, zone_end: int) -> Set[int]:
    cps: Set[int] = set()
    for high in range(zone_start, zone_end + 1):
        for low in range(0xA1, 0xFE + 1):
            pair = bytes([high, low])
            try:
                ch = pair.decode("gb2312")
            except UnicodeDecodeError:
                continue
            if ch:
                cps.add(ord(ch))
    return cps


def build_default_charset() -> Set[int]:
    cps: Set[int] = set(range(0x20, 0x7F))
    cps |= gb2312_codepoints(0xA1, 0xA9)
    cps |= gb2312_codepoints(0xB0, 0xF7)
    return cps


def parse_encoding(line: str) -> int | None:
    # ENCODING can be "ENCODING n" or "ENCODING -1 n"
    parts = line.strip().split()
    if len(parts) < 2 or parts[0] != "ENCODING":
        return None

    try:
        first = int(parts[1])
    except ValueError:
        return None

    if first >= 0:
        return first

    if len(parts) >= 3:
        try:
            second = int(parts[2])
        except ValueError:
            return None
        return second if second >= 0 else None

    return None


def parse_bdf_blocks(lines: List[str]) -> Tuple[List[str], List[str], Dict[int, List[str]]]:
    header: List[str] = []
    footer: List[str] = []
    glyphs: Dict[int, List[str]] = {}

    i = 0
    n = len(lines)

    while i < n:
        line = lines[i]
        if line.startswith("CHARS "):
            break
        header.append(line)
        i += 1

    if i >= n or not lines[i].startswith("CHARS "):
        raise ValueError("Invalid BDF: missing CHARS line")

    # Skip original CHARS line
    i += 1

    while i < n:
        line = lines[i]

        if line.startswith("STARTCHAR "):
            block: List[str] = [line]
            i += 1

            while i < n:
                block.append(lines[i])
                if lines[i].strip() == "ENDCHAR":
                    i += 1
                    break
                i += 1

            encoding = None
            for bline in block:
                if bline.startswith("ENCODING"):
                    encoding = parse_encoding(bline)
                    break

            if encoding is not None and encoding not in glyphs:
                glyphs[encoding] = block
            continue

        footer = lines[i:]
        break

    return header, footer, glyphs


def load_extra_codepoints(extra_file: Path) -> Set[int]:
    cps: Set[int] = set()
    for raw in extra_file.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue

        if line.startswith("U+"):
            cps.add(int(line[2:], 16))
            continue

        for ch in line:
            cps.add(ord(ch))
    return cps


def write_subset_bdf(
    output_path: Path,
    header: List[str],
    footer: List[str],
    glyphs: Dict[int, List[str]],
    wanted: Set[int],
) -> Tuple[int, int]:
    selected = sorted(cp for cp in wanted if cp in glyphs)
    missing = sorted(cp for cp in wanted if cp not in glyphs)

    out_lines: List[str] = []
    out_lines.extend(header)
    out_lines.append(f"CHARS {len(selected)}\n")
    for cp in selected:
        out_lines.extend(glyphs[cp])
    out_lines.extend(footer)

    output_path.write_text("".join(out_lines), encoding="utf-8")
    return len(selected), len(missing)


def write_manifest(manifest_path: Path, selected: Set[int]) -> None:
    lines = [
        "# Zpix minimal charset codepoints\n",
        "# Format: U+XXXX\n",
    ]
    for cp in sorted(selected):
        lines.append(f"U+{cp:04X}\n")
    manifest_path.write_text("".join(lines), encoding="utf-8")


def write_missing(missing_path: Path, missing: List[int]) -> None:
    lines = [
        "# Requested but missing in source BDF\n",
        "# Format: U+XXXX\n",
    ]
    for cp in missing:
        lines.append(f"U+{cp:04X}\n")
    missing_path.write_text("".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Subset zpix.bdf for Zephyr minimal Chinese set")
    parser.add_argument("--input", default="fonts/zpix.bdf", help="Input BDF file")
    parser.add_argument("--output", default="fonts/zpix_min_zh.bdf", help="Output subset BDF")
    parser.add_argument(
        "--manifest",
        default="fonts/zpix_min_zh_codepoints.txt",
        help="Output codepoint manifest",
    )
    parser.add_argument(
        "--missing",
        default="fonts/zpix_min_zh_missing.txt",
        help="Output missing codepoint list",
    )
    parser.add_argument(
        "--extra",
        default="",
        help="Optional text file with extra characters or U+XXXX lines to include",
    )
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    manifest_path = Path(args.manifest)
    missing_path = Path(args.missing)

    raw = input_path.read_text(encoding="utf-8", errors="strict").splitlines(keepends=True)
    header, footer, glyphs = parse_bdf_blocks(raw)

    wanted = build_default_charset()
    if args.extra:
        wanted |= load_extra_codepoints(Path(args.extra))

    selected_count, missing_count = write_subset_bdf(output_path, header, footer, glyphs, wanted)

    selected_set = {cp for cp in wanted if cp in glyphs}
    missing_list = sorted(cp for cp in wanted if cp not in glyphs)
    write_manifest(manifest_path, selected_set)
    write_missing(missing_path, missing_list)

    print(f"Input glyphs: {len(glyphs)}")
    print(f"Requested codepoints: {len(wanted)}")
    print(f"Selected glyphs: {selected_count}")
    print(f"Missing codepoints: {missing_count}")
    print(f"Subset BDF: {output_path}")
    print(f"Manifest: {manifest_path}")
    print(f"Missing list: {missing_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
