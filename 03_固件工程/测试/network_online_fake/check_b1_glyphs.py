"""Check all B1 UI/online static strings against the real raw font cmaps."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT.parent
BASE_DIRS = [
    ROOT / "components" / "panel_ui" / "src",
    ROOT / "components" / "panel_online" / "src",
]


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
    for directory in BASE_DIRS:
        for path in directory.glob("*.c"):
            for ch in path.read_text(encoding="utf-8"):
                if ord(ch) >= 0x80:
                    expected.add(ord(ch))

    failures = 0
    for path in sorted(fc.FONT_DIR.glob("ui_font_*.c")):
        if path.name.startswith("ui_font_num_"):
            continue
        covered = fc.parse_cmap(path)
        missing = sorted(expected - covered)
        print(f"{path.name}: missing={len(missing)}")
        for cp in missing[:40]:
            print(f"  missing U+{cp:04X} {chr(cp)!r}")
        failures += len(missing)
    if failures:
        print(f"FAIL: B1 glyph coverage failures={failures}")
        return 1
    print("PASS: B1 UI/online static strings covered by existing raw fonts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
