#!/usr/bin/env python3
"""Record bundled LVGL provenance and generate native-size Unifont data.

Glyphs stay 8x16/16x16 in ROM; player_font.c expands one requested glyph to
2x in a 128-byte scratch buffer. No downloaded or pre-expanded font is used.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
LVGL = ROOT / "libbare-cpu/projects/x16xx-halley6/Demos/LVGL_GUIDemo/components/lvgl"
FONT = ROOT / "third_party/ReGBA/source/tools/unifont.hex.gz"
FONT_SHA = "3d334e48426681bffe5005b4b399b704e8612fc162098d86383fe5463ae95c0e"


def prepare(output):
    header = (LVGL / "lvgl.h").read_text(encoding="utf-8")
    for name, value in (("MAJOR", 8), ("MINOR", 3), ("PATCH", 10)):
        if not re.search(rf"#define LVGL_VERSION_{name}\s+{value}\b", header):
            raise ValueError("bundled LVGL must be version 8.3.10")
    font_bytes = FONT.read_bytes()
    if hashlib.sha256(font_bytes).hexdigest() != FONT_SHA:
        raise ValueError("Unifont differs from the pinned ReGBA source")
    glyphs = []
    with gzip.open(FONT, "rt", encoding="ascii") as source:
        for line in source:
            code, bits = line.strip().split(":")
            code = int(code, 16)
            if code > 0xffff:
                continue
            if len(bits) not in (32, 64):
                raise ValueError(f"unsupported glyph U+{code:04X}")
            width = len(bits) // 4
            rows = [int(bits[i:i + width // 4], 16) << (16 - width)
                    for i in range(0, len(bits), width // 4)]
            glyphs.append((code, width, rows))
    glyphs.sort()
    available = {item[0] for item in glyphs}
    required = set(range(32, 127))
    for path in (ROOT / "projects/halley7m-player/gui").glob("*.c"):
        required.update(ord(c) for c in path.read_text(encoding="utf-8") if ord(c) >= 128)
    missing = sorted(required - available)
    if missing:
        raise ValueError("missing GUI glyphs: " + ", ".join(f"U+{c:04X}" for c in missing))
    lines = ["/* Generated native-size GNU Unifont; GPL-2.0-or-later.",
             " * GNU Unifont (C) 2000 Roman Czyborra.",
             " * ReGBA commit 7f379052dc14366e622a96c7cf7fb6b5a7e918a3.",
             " * See gui/UNIFONT-LICENSE.txt and SOURCE.json. */",
             "#include <stdint.h>",
             "typedef struct { uint16_t code; uint8_t width; uint16_t rows[16]; } player_font_glyph_t;",
             "static const player_font_glyph_t player_font_glyphs[] = {"]
    for code, width, rows in glyphs:
        lines.append("{0x%04x,%d,{%s}}," % (code, width, ",".join(f"0x{r:04x}" for r in rows)))
    lines.extend(["};", "#define PLAYER_FONT_COUNT (sizeof(player_font_glyphs)/sizeof(player_font_glyphs[0]))", ""])
    output.mkdir(parents=True, exist_ok=True)
    generated = "\n".join(lines)
    destination = output / "player_font_data.h"
    if not destination.exists() or destination.read_text(encoding="ascii") != generated:
        destination.write_text(generated, encoding="ascii")
    sources = sorted((LVGL / "src").rglob("*.c"))
    if len(sources) < 100:
        raise ValueError("LVGL core source tree is incomplete")
    manifest = dict(schema=1, lvgl_version="8.3.10", lvgl_root=str(LVGL),
                    sdk_commit="9500d2ec917a44d0d9657940e865e70f1a885849",
                    lvgl_license=str(LVGL / "LICENCE.txt"),
                    lvgl_sources={str(p.relative_to(LVGL)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                    unifont_source=str(FONT), unifont_sha256=FONT_SHA,
                    regba_commit="7f379052dc14366e622a96c7cf7fb6b5a7e918a3",
                    glyph_count=len(glyphs), stored_size="native 8x16 or 16x16, expanded on demand")
    (output / "SOURCE.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared LVGL 8.3.10 ({len(sources)} sources), {len(glyphs)} native Unifont glyphs")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.output)
