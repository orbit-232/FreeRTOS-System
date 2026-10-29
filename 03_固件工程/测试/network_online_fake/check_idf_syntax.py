"""Syntax-check panel_online sources with the official ESP-IDF headers."""
from __future__ import annotations

import json
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
IDF = pathlib.Path(r"C:\esp\v6.0.2\esp-idf")
COMPILE_DB = ROOT / "build-apps-a2-rev1_3" / "compile_commands.json"
SOURCES = [
    ROOT / "components" / "panel_online" / "src" / "panel_online.c",
    ROOT / "components" / "panel_online" / "src" / "panel_online_http.c",
    ROOT / "components" / "panel_online" / "src" / "panel_online_model.c",
]
EXTRA_INCLUDES = [
    ROOT / "components" / "panel_online" / "include",
    ROOT / "components" / "panel_online" / "src",
    ROOT / "test" / "network_online_fake" / "third_party" / "cjson",
    IDF / "components" / "esp_http_client" / "include",
    IDF / "components" / "mbedtls" / "esp_crt_bundle" / "include",
    IDF / "components" / "mbedtls" / "mbedtls" / "include",
    IDF / "components" / "mbedtls" / "mbedtls" / "tf-psa-crypto" / "include",
    IDF / "components" / "mbedtls" / "mbedtls" / "tf-psa-crypto" / "drivers" / "builtin" / "include",
    IDF / "components" / "mbedtls" / "port" / "include",
    IDF / "components" / "esp-tls" / "esp-tls",
    IDF / "components" / "esp-tls",
]


def rewrite(value: str) -> str:
    value = value.replace("C:/wsp/WavesharePanel/03_Firmware", ROOT.as_posix())
    value = value.replace(r"C:\wsp\WavesharePanel\03_Firmware", str(ROOT))
    return value


def main() -> int:
    entries = json.loads(COMPILE_DB.read_text(encoding="utf-8"))
    base = next((entry for entry in entries if entry["file"].endswith("panel_network.c")), None)
    if base is None:
        print("FAIL: no panel_network.c compile entry", file=sys.stderr)
        return 1
    args = shlex.split(base["command"], posix=False)
    args = [arg.replace('\\"', '"') for arg in args]
    args = [arg[1:-1] if arg.startswith('"') and arg.endswith('"') else arg for arg in args]
    args = [("@" + arg[2:-1]) if arg.startswith('@"') and arg.endswith('"') else arg
            for arg in args]
    cleaned: list[str] = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-o", "-MT", "-MF"):
            skip_next = True
            continue
        if arg in ("-MD", "-MMD", "-MP", "-c"):
            continue
        if arg.lower().endswith("panel_network.c"):
            continue
        cleaned.append(rewrite(arg))
    for include in EXTRA_INCLUDES:
        cleaned.insert(1, "-I" + str(include))

    failures = 0
    for source in SOURCES:
        command = cleaned + ["-fsyntax-only", str(source)]
        print(f"SYNTAX {source.name}")
        proc = subprocess.run(command, cwd=base["directory"], text=True)
        if proc.returncode != 0:
            failures += 1
    if failures:
        print(f"FAIL: panel_online IDF syntax failures={failures}", file=sys.stderr)
        return 1
    print("PASS: panel_online sources with official ESP-IDF 6.0.2 headers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
