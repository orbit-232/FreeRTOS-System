"""Syntax-check changed firmware sources with the ESP-IDF RISC-V compiler.

CMake/Ninja configuration can stall in this sandbox at compiler ABI detection.
The project already has a generated compile_commands.json from IDF, so we reuse
its exact defines/include paths and run only -fsyntax-only.  No object files,
link step, flash or hardware access.
"""
from __future__ import annotations

import json
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(r"C:\Users\30782\Desktop\微雪10.1寸项目\微雪屏幕工程\03_固件工程")
COMPILE_DB = ROOT / "build-rev1_3" / "compile_commands.json"
TARGETS = {
    "panel_ui.c",
    "ui_widgets.c",
    "ui_nav.c",
    "ui_perf.c",
    "ui_diag.c",
    "page_home.c",
    "page_control.c",
    "page_settings.c",
    "page_weather.c",
    "page_monitor.c",
    "page_calculator.c",
    "panel_settings.c",
    "panel_services.c",
    "page_lock.c",
}


def clean_args(command: str) -> list[str]:
    # posix=True eats the backslashes in the Windows GCC path; posix=False
    # preserves them but leaves shell-escaped quotes around macro values.
    args = shlex.split(command, posix=False)
    args = [arg.replace('\\"', '"') for arg in args]
    args = [arg[1:-1] if arg.startswith('"') and arg.endswith('"') else arg for arg in args]
    args = [("@" + arg[2:-1]) if arg.startswith('@"') and arg.endswith('"') else arg
            for arg in args]
    out: list[str] = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-o", "-MT", "-MF"):
            skip_next = True
            continue
        if arg in ("-MD", "-MMD", "-MP"):
            continue
        out.append(arg)
    # Keep the executable, drop any existing syntax-only/dependency flags, then
    # request syntax-only.  The source path and -c remain intact.
    out.append("-fsyntax-only")
    return out


def main() -> int:
    db = json.loads(COMPILE_DB.read_text(encoding="utf-8"))
    entries = {}
    for entry in db:
        name = pathlib.Path(entry["file"]).name
        if name in TARGETS:
            entries[name] = entry

    missing = sorted(TARGETS - set(entries))
    if missing:
        print(f"FAIL: compile_commands missing targets: {missing}", file=sys.stderr)
        return 1

    failures = 0
    for name in sorted(TARGETS):
        entry = entries[name]
        args = clean_args(entry["command"])
        args[1:1] = [
            "-IC:/wsp/WavesharePanel/03_Firmware/components/panel_network/include",
            "-IC:/wsp/WavesharePanel/03_Firmware/components/panel_online/include",
        ]
        print(f"SYNTAX {name}: {entry['file']}")
        sys.stdout.flush()
        proc = subprocess.run(args, cwd=entry["directory"])
        if proc.returncode != 0:
            print(f"FAIL {name}: exit={proc.returncode}", file=sys.stderr)
            failures += 1
        else:
            print(f"PASS {name}")
    if failures:
        print(f"FAIL: IDF syntax failures={failures}", file=sys.stderr)
        return 1
    print(f"PASS: IDF -fsyntax-only for {len(TARGETS)} changed/related sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
