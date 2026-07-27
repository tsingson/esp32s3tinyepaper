#!/usr/bin/env python3
"""
Convert Zpix BDF glyphs to compact 12x12 C arrays for Zephyr firmware.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


@dataclass
class Glyph:
    codepoint: int
    width: int
    height: int
    xoff: int
    yoff: int
    bitmap_rows: List[int]
    bitmap_bits: int


def parse_encoding(parts: List[str]) -> int | None:
    if len(parts) < 2:
        return None

    first = int(parts[1])
    if first >= 0:
        return first

    if len(parts) >= 3:
        second = int(parts[2])
        return second if second >= 0 else None

    return None


def parse_bdf(path: Path) -> Tuple[int, Dict[int, Glyph]]:
    lines = path.read_text(encoding="utf-8", errors="strict").splitlines()
    ascent = 10
    glyphs: Dict[int, Glyph] = {}

    i = 0
    n = len(lines)

    while i < n:
        line = lines[i].strip()

        if line.startswith("FONT_ASCENT "):
            ascent = int(line.split()[1])
            i += 1
            continue

        if not line.startswith("STARTCHAR "):
            i += 1
            continue

        i += 1
        encoding = None
        bbx = (0, 0, 0, 0)
        rows: List[int] = []
        in_bitmap = False

        while i < n:
            cur = lines[i].strip()

            if cur.startswith("ENCODING"):
                encoding = parse_encoding(cur.split())
            elif cur.startswith("BBX "):
                p = cur.split()
                bbx = (int(p[1]), int(p[2]), int(p[3]), int(p[4]))
            elif cur == "BITMAP":
                in_bitmap = True
            elif cur == "ENDCHAR":
                if encoding is not None and encoding >= 0:
                    width, height, xoff, yoff = bbx
                    bits = max(8, ((width + 7) // 8) * 8)
                    glyphs[encoding] = Glyph(
                        codepoint=encoding,
                        width=width,
                        height=height,
                        xoff=xoff,
                        yoff=yoff,
                        bitmap_rows=rows,
                        bitmap_bits=bits,
                    )
                i += 1
                break
            elif in_bitmap and cur:
                rows.append(int(cur, 16))

            i += 1

    return ascent, glyphs


def rasterize_to_12x12(ascent: int, g: Glyph) -> List[int]:
    cell_w = 12
    cell_h = 12
    out = [0] * cell_h

    if g.width <= 0 or g.height <= 0:
        return out

    top = ascent - (g.height + g.yoff)

    for src_row in range(g.height):
        dst_row = top + src_row
        if dst_row < 0 or dst_row >= cell_h:
            continue

        row_val = g.bitmap_rows[src_row] if src_row < len(g.bitmap_rows) else 0
        mask = out[dst_row]

        for src_col in range(g.width):
            src_bit = g.bitmap_bits - 1 - src_col
            if src_bit < 0:
                continue
            if ((row_val >> src_bit) & 0x1) == 0:
                continue

            dst_col = g.xoff + src_col
            if dst_col < 0 or dst_col >= cell_w:
                continue

            mask |= 1 << (11 - dst_col)

        out[dst_row] = mask

    return out


def render_header(guard: str, h_name: str, c_name: str) -> str:
    return (
        f"#ifndef {guard}\n"
        f"#define {guard}\n\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n\n"
        "typedef struct {\n"
        "\tuint32_t codepoint;\n"
        "\tuint16_t rows[12];\n"
        "} zpix12_glyph_t;\n\n"
        f"extern const zpix12_glyph_t {c_name}[];\n"
        f"extern const size_t {c_name}_count;\n\n"
        f"#endif /* {guard} */\n"
    )


def render_source(h_name: str, c_name: str, table: List[Tuple[int, List[int]]]) -> str:
    lines: List[str] = []
    lines.append(f"#include \"{h_name}\"\n\n")
    lines.append(f"const zpix12_glyph_t {c_name}[] = {{\n")

    for cp, rows in table:
        row_txt = ", ".join(f"0x{v:03X}" for v in rows)
        lines.append(f"\t{{0x{cp:04X}, {{{row_txt}}}}},\n")

    lines.append("};\n\n")
    lines.append(f"const size_t {c_name}_count = sizeof({c_name}) / sizeof({c_name}[0]);\n")
    return "".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert Zpix BDF to 12x12 C arrays")
    parser.add_argument("--input", default="fonts/zpix_min_zh.bdf", help="Input BDF file")
    parser.add_argument("--out-c", default="src/zpix12_font_data.c", help="Output C source")
    parser.add_argument("--out-h", default="src/zpix12_font_data.h", help="Output C header")
    parser.add_argument("--symbol", default="zpix12_font_glyphs", help="C symbol prefix")
    args = parser.parse_args()

    ascent, glyphs = parse_bdf(Path(args.input))
    entries = sorted((cp, rasterize_to_12x12(ascent, g)) for cp, g in glyphs.items())

    guard = "ZPIX12_FONT_DATA_H"
    h_name = Path(args.out_h).name
    c_name = args.symbol

    Path(args.out_h).write_text(render_header(guard, h_name, c_name), encoding="utf-8")
    Path(args.out_c).write_text(render_source(h_name, c_name, entries), encoding="utf-8")

    print(f"Input glyphs: {len(glyphs)}")
    print(f"Output glyphs: {len(entries)}")
    print(f"Header: {args.out_h}")
    print(f"Source: {args.out_c}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
