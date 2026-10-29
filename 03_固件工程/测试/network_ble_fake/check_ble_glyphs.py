"""Check panel_ble.c display strings against the real raw font cmaps.

The BLE fault/status reason strings are rendered by the UI even though they
live in the network component.  This reuses the project's cmap parser and adds
the production panel_ble.c source to the expected codepoint set.  No font is
regenerated or modified.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT.parent
BLE_SOURCE = ROOT / "components" / "panel_network" / "src" / "panel_ble.c"


def main() -> int:
    tool = PROJECT / "04_开发工具" / "字体工具" / "check_font_coverage.py"
    spec = importlib.util.spec_from_file_location("panel_font_coverage", tool)
    if spec is None or spec.loader is None:
        print(f"FAIL: cannot load {tool}", file=sys.stderr)
        return 1
    fc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fc)

    fc.FONT_DIR = ROOT / "components" / "panel_ui" / "fonts"
    fc.UI_SRC = ROOT / "components" / "panel_ui" / "src"
    fc.GLYPH_FILE = PROJECT / "04_开发工具" / "字体工具" / "ui_glyphs.txt"
    fc.SDKCONFIG = ROOT / "sdkconfig.rev1_3"

    expected = fc.ui_codepoints()
    ble_text = BLE_SOURCE.read_text(encoding="utf-8")
    ble_char_set = {ch for ch in ble_text if ord(ch) >= 0x80}
    ble_chars = {ord(ch) for ch in ble_char_set}
    expected |= ble_chars
    compact = set((PROJECT / "04_开发工具" / "字体工具" / "ui_glyphs_compact.txt")
                  .read_text(encoding="utf-8"))
    missing_glyph_list = sorted(ble_char_set - compact)
    print(f"panel_ble non-ASCII codepoints: {len(ble_chars)}")
    print(f"ui_glyphs_compact.txt entries: {len(compact)}")
    for ch in missing_glyph_list[:20]:
        print(f"  missing from ui_glyphs_compact.txt U+{ord(ch):04X} {ch!r}")

    failures = len(missing_glyph_list)
    for path in sorted(fc.FONT_DIR.glob("ui_font_*.c")):
        if path.name.startswith("ui_font_num_"):
            continue
        covered = fc.parse_cmap(path)
        missing = sorted(expected - covered)
        print(f"{path.name}: missing={len(missing)}")
        for cp in missing[:20]:
            print(f"  missing from cmap U+{cp:04X} {chr(cp)!r}")
        failures += len(missing)

    if failures:
        print(f"FAIL: glyph coverage failures={failures}")
        return 1
    print("PASS: panel_ble status strings are covered by the existing raw fonts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
