#!/usr/bin/env python3
"""生成音乐播放器预烘焙素材与 A8 图标。

设计约束：
- 背景 1280x800 RGB565，生成时做 8x8 Bayer 抖动，固件作为 const 放 Flash；
- 唱片 344x344、圆封面 180x180、曲目卡 160x160 全部预烘焙，运行时不缩放/模糊；
- 图标源文件为自制 SVG，本脚本以 4x 超采样转换为 A8 alpha 遮罩，LVGL 运行时只做 recolor；
- 输出 C 数组，不依赖运行时的 PNG 解码器。
"""
from __future__ import annotations

import math
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "03_Firmware" / "components" / "panel_music_ui"
ASSET_SRC = Path(__file__).resolve().parent
ICON_DIR = ASSET_SRC / "icons"
PREVIEW_DIR = ASSET_SRC / "preview"
ASSET_OUT = COMPONENT / "assets"
HEADER = COMPONENT / "include" / "music_assets.h"
W, H = 1280, 800
BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.float32) / 64.0 - 0.5


def ensure_dirs() -> None:
    for path in (ICON_DIR, PREVIEW_DIR, ASSET_OUT, HEADER.parent):
        path.mkdir(parents=True, exist_ok=True)


def smoothstep(edge0: float, edge1: float, x: np.ndarray | float):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def dither_image(rgb: np.ndarray, bits=(5, 6, 5)) -> Image.Image:
    """8x8 Bayer 抖动后量化到 RGB565，减少暗部渐变的色带。"""
    h, w, _ = rgb.shape
    tile = np.tile(BAYER8, (h // 8 + 1, w // 8 + 1))[:h, :w]
    out = rgb.astype(np.float32).copy()
    steps = [256.0 / (1 << b) for b in bits]
    for c, step in enumerate(steps):
        out[:, :, c] += tile * step
    out = np.clip(out, 0, 255).astype(np.uint8)
    r = (out[:, :, 0] >> 3).astype(np.uint16)
    g = (out[:, :, 1] >> 2).astype(np.uint16)
    b = (out[:, :, 2] >> 3).astype(np.uint16)
    return ((r << 11) | (g << 5) | b).astype("<u2")


def rgb565_bytes(rgb: np.ndarray) -> bytes:
    return dither_image(rgb).tobytes()


def rgb565a8_bytes(rgb: np.ndarray, alpha: np.ndarray) -> bytes:
    return dither_image(rgb).tobytes() + np.clip(alpha, 0, 255).astype(np.uint8).tobytes()


def write_bytes(path: Path, data: bytes) -> None:
    path.write_bytes(data)


def build_background() -> Image.Image:
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    base = np.zeros((H, W, 3), np.float32)
    base[:] = (16, 16, 18)  # #101012
    # 左上紫雾与右下暖光，暗部仍保持 #101012 的基调。
    d_purple = ((x - 560.0) ** 2 + (y - 210.0) ** 2) / (2.0 * 360.0 ** 2)
    d_orange = ((x - 1040.0) ** 2 + (y - 690.0) ** 2) / (2.0 * 340.0 ** 2)
    purple = np.exp(-d_purple)[..., None] * np.array([54, 36, 86], np.float32)
    orange = np.exp(-d_orange)[..., None] * np.array([62, 35, 18], np.float32)
    base += purple * 0.52 + orange * 0.34
    # 中心区域压暗，保证唱片与文字有足够对比度。
    ring_shadow = np.exp(-(((x - 640.0) / 430.0) ** 2 + ((y - 266.0) / 300.0) ** 2))
    base -= ring_shadow[..., None] * np.array([5, 5, 7], np.float32)
    # 边角暗角。
    nx = (x - W / 2) / (W / 2)
    ny = (y - H / 2) / (H / 2)
    vignette = np.clip(1.0 - 0.17 * (nx * nx + ny * ny), 0.72, 1.0)
    base *= vignette[..., None]
    # 极轻的胶片颗粒，避免大面积纯色在 16bit 面板上出现台阶。
    rng = np.random.default_rng(20261029)
    base += rng.normal(0.0, 0.75, (H, W, 1))
    base = np.clip(base, 0, 255).astype(np.uint8)
    img = Image.fromarray(base, "RGB")
    img.save(PREVIEW_DIR / "background_preview.png")
    write_bytes(ASSET_OUT / "music_bg.bin", rgb565_bytes(base))
    return img


def build_vinyl() -> None:
    size = 344
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    cx = cy = (size - 1) / 2.0
    dx, dy = x - cx, y - cy
    r = np.sqrt(dx * dx + dy * dy)
    edge = 171.2
    alpha = np.clip((edge - r) * 2.4 + 0.5, 0, 1) * 255.0
    rgb = np.zeros((size, size, 3), np.float32)
    rgb[:] = (10, 10, 13)

    groove_mask = smoothstep(84.0, 88.0, r) * (1.0 - smoothstep(166.0, 169.0, r))
    rings = 0.5 + 0.5 * np.cos((r - 88.0) * (2.0 * math.pi / 2.05))
    fine = 0.5 + 0.5 * np.cos((r - 88.0) * (2.0 * math.pi / 7.7))
    mod = (rings * 7.5 + fine * 3.5) * groove_mask
    rgb += mod[..., None] * np.array([1.0, 0.98, 1.05], np.float32)
    # 外圈与内圈边缘。
    rim = (1.0 - smoothstep(167.0, 171.0, r)) * smoothstep(164.5, 168.0, r)
    rgb += rim[..., None] * np.array([23, 22, 27], np.float32)
    inner = (1.0 - smoothstep(83.0, 88.0, r))
    rgb = rgb * (1.0 - inner[..., None] * 0.55)
    # 斜向高光，仅预烘焙，运行时不加阴影或滤镜。
    ang = np.arctan2(dy, dx)
    highlight = np.exp(-((ang + 2.15) ** 2) / 0.035) * smoothstep(84, 100, r) * \
                (1.0 - smoothstep(164, 171, r))
    rgb += highlight[..., None] * np.array([27, 23, 34], np.float32)
    rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    img = Image.fromarray(np.dstack([rgb, alpha.astype(np.uint8)]), "RGBA")
    img.save(PREVIEW_DIR / "vinyl_preview.png")
    write_bytes(ASSET_OUT / "music_vinyl.bin", rgb565a8_bytes(rgb, alpha))


def gradient(size: int, top: tuple[int, int, int], bottom: tuple[int, int, int]) -> Image.Image:
    y = np.linspace(0, 1, size, dtype=np.float32)[:, None, None]
    a = np.array(top, np.float32)[None, None, :]
    b = np.array(bottom, np.float32)[None, None, :]
    arr = np.repeat(a * (1 - y) + b * y, size, axis=1)
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def build_cover(track: int) -> Image.Image:
    size = 720
    palettes = [
        ((28, 26, 53), (42, 31, 64), [(112, 99, 196), (145, 79, 122), (61, 113, 157)]),
        ((22, 20, 40), (44, 24, 58), [(109, 63, 177), (37, 113, 166), (213, 101, 68)]),
        ((34, 25, 39), (73, 36, 40), [(234, 145, 81), (145, 77, 120), (57, 94, 151)]),
        ((28, 13, 22), (54, 18, 29), [(201, 53, 58), (52, 68, 137), (137, 50, 105)]),
    ]
    top, bottom, accent = palettes[track]
    img = gradient(size, top, bottom)
    draw = ImageDraw.Draw(img, "RGBA")
    rng = np.random.default_rng(9000 + track)
    for i in range(18):
        radius = int(rng.integers(size // 16, size // 3))
        cx = int(rng.integers(-radius, size + radius))
        cy = int(rng.integers(-radius, size + radius))
        color = accent[i % len(accent)]
        alpha = int(rng.integers(34, 95))
        draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius),
                     fill=(*color, alpha))
    if track == 0:
        for i in range(38):
            cx = int(rng.integers(100, size - 100))
            cy = int(rng.integers(110, size - 90))
            rr = int(rng.integers(3, 16))
            draw.ellipse((cx - rr, cy - rr, cx + rr, cy + rr),
                         fill=(220, 173, 114, int(rng.integers(40, 120))))
    elif track == 1:
        draw.rectangle((size * 0.30, 0, size * 0.36, size), fill=(217, 210, 255, 32))
        draw.rectangle((0, size * 0.64, size, size * 0.69), fill=(88, 188, 226, 30))
    elif track == 2:
        draw.ellipse((size * 0.18, size * 0.12, size * 0.82, size * 0.76),
                     outline=(255, 207, 151, 72), width=9)
        draw.ellipse((size * 0.32, size * 0.25, size * 0.68, size * 0.61),
                     fill=(255, 176, 112, 36))
    else:
        for i in range(5):
            x0 = int(size * (0.08 + i * 0.17))
            draw.line((x0, 0, x0 + 150, size), fill=(235, 85, 89, 36), width=6)
        draw.line((0, size * 0.78, size, size * 0.35), fill=(64, 100, 190, 42), width=13)
    img = img.filter(ImageFilter.GaussianBlur(14))
    # 轻颗粒，避免纯渐变。
    arr = np.asarray(img).astype(np.float32)
    arr += rng.normal(0.0, 2.0, (size, size, 1))
    img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")
    draw = ImageDraw.Draw(img, "RGBA")
    draw.ellipse((size * 0.10, size * 0.10, size * 0.90, size * 0.90),
                 outline=(255, 255, 255, 28), width=3)
    img.save(PREVIEW_DIR / f"cover_{track}_preview.png")

    center = img.resize((180, 180), Image.Resampling.LANCZOS)
    write_bytes(ASSET_OUT / f"music_cover_{track}.bin", circular_rgb565a8(center, 90.0))

    card = img.resize((160, 160), Image.Resampling.LANCZOS)
    write_bytes(ASSET_OUT / f"music_card_{track}.bin", rounded_rgb565a8(card, 24.0))
    return img


def alpha_mask(size: int, radius: float, rounded: bool) -> np.ndarray:
    scale = 4
    m = Image.new("L", (size * scale, size * scale), 0)
    d = ImageDraw.Draw(m)
    if rounded:
        d.rounded_rectangle((0, 0, size * scale - 1, size * scale - 1),
                            radius=int(radius * scale), fill=255)
    else:
        c = size * scale / 2.0
        d.ellipse((c - radius * scale, c - radius * scale,
                   c + radius * scale, c + radius * scale), fill=255)
    return np.asarray(m.resize((size, size), Image.Resampling.LANCZOS), dtype=np.uint8)


def circular_rgb565a8(img: Image.Image, radius: float) -> bytes:
    rgb = np.asarray(img.convert("RGB"))
    return rgb565a8_bytes(rgb, alpha_mask(img.width, radius, False))


def rounded_rgb565a8(img: Image.Image, radius: float) -> bytes:
    rgb = np.asarray(img.convert("RGB"))
    return rgb565a8_bytes(rgb, alpha_mask(img.width, radius, True))


ICONS = {
    "chevron_down": """<path d="M10 16 L24 30 L38 16" fill="none" stroke="#fff" stroke-width="3.4" stroke-linecap="round" stroke-linejoin="round"/>""",
    "more": """<circle cx="13" cy="24" r="3" fill="#fff"/><circle cx="24" cy="24" r="3" fill="#fff"/><circle cx="35" cy="24" r="3" fill="#fff"/>""",
    "play": """<path d="M18 12 L38 24 L18 36 Z" fill="#fff"/>""",
    "pause": """<rect x="15" y="12" width="6" height="24" rx="2" fill="#fff"/><rect x="27" y="12" width="6" height="24" rx="2" fill="#fff"/>""",
    "prev": """<path d="M9 12 H14 V36 H9 Z" fill="#fff"/><path d="M39 12 L16 24 L39 36 Z" fill="#fff"/>""",
    "next": """<path d="M39 12 H34 V36 H39 Z" fill="#fff"/><path d="M9 12 L32 24 L9 36 Z" fill="#fff"/>""",
    "shuffle": """<path d="M6 14 H13 C26 14 25 34 40 34" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round"/><path d="M33 28 L40 34 L33 40" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/><path d="M6 34 H13 C26 34 25 14 40 14" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round"/><path d="M33 8 L40 14 L33 20" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>""",
    "repeat": """<path d="M14 12 H32 C38 12 42 16 42 22 V25" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round"/><path d="M36 8 L42 14 L36 20" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/><path d="M34 36 H16 C10 36 6 32 6 26 V23" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round"/><path d="M12 40 L6 34 L12 28" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>""",
    "heart": """<path d="M24 40 C10 30 5 22 8 15 C11 9 19 9 24 16 C29 9 37 9 40 15 C43 22 38 30 24 40 Z" fill="none" stroke="#fff" stroke-width="2.8" stroke-linejoin="round"/>""",
    "heart_filled": """<path d="M24 41 C9 31 4 22 8 14 C11 8 19 8 24 16 C29 8 37 8 40 14 C44 22 39 31 24 41 Z" fill="#fff"/>""",
    "list": """<path d="M12 13 H40 M12 24 H40 M12 35 H40" fill="none" stroke="#fff" stroke-width="2.8" stroke-linecap="round"/><circle cx="6" cy="13" r="2" fill="#fff"/><circle cx="6" cy="24" r="2" fill="#fff"/><circle cx="6" cy="35" r="2" fill="#fff"/>""",
    "volume": """<path d="M7 19 H15 L25 11 V37 L15 29 H7 Z" fill="#fff"/><path d="M30 17 C35 20 35 28 30 31" fill="none" stroke="#fff" stroke-width="2.8" stroke-linecap="round"/><path d="M36 12 C43 18 43 30 36 36" fill="none" stroke="#fff" stroke-width="2.8" stroke-linecap="round"/>""",
    "volume_mute": """<path d="M7 19 H15 L25 11 V37 L15 29 H7 Z" fill="#fff"/><path d="M31 19 L41 29 M41 19 L31 29" fill="none" stroke="#fff" stroke-width="2.8" stroke-linecap="round"/>""",
    "queue": """<path d="M9 11 H37 M9 21 H29 M9 31 H25" fill="none" stroke="#fff" stroke-width="2.8" stroke-linecap="round"/><path d="M31 28 L42 34 L31 40 Z" fill="#fff"/>""",
    "download": """<path d="M24 7 V31 M14 22 L24 32 L34 22" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/><path d="M9 39 H39" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round"/>""",
}


def write_svgs() -> None:
    for name, body in ICONS.items():
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" '
               f'viewBox="0 0 48 48">\n{body}\n</svg>\n')
        (ICON_DIR / f"{name}.svg").write_text(svg, encoding="utf-8")


def parse_path(d: str) -> list[list[tuple[float, float]]]:
    tokens = re.findall(r"[MLHVCQZmlhvcqz]|[-+]?(?:\d*\.\d+|\d+)", d)
    subs: list[list[tuple[float, float]]] = []
    cur: list[tuple[float, float]] = []
    x = y = 0.0
    cmd = "M"
    i = 0
    rel = False
    while i < len(tokens):
        t = tokens[i]
        if re.fullmatch(r"[A-Za-z]", t):
            cmd = t
            rel = cmd.islower()
            i += 1
            if cmd.upper() == "Z":
                if cur and cur[0] != cur[-1]:
                    cur.append(cur[0])
                if cur:
                    subs.append(cur)
                cur = []
                continue
        def num() -> float:
            nonlocal i
            v = float(tokens[i])
            i += 1
            return v
        if cmd.upper() == "M":
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            if cur:
                subs.append(cur)
            cur = [(nx, ny)]
            x, y = nx, ny
            cmd = "L" if rel else "L"
        elif cmd.upper() == "L":
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            cur.append((nx, ny))
            x, y = nx, ny
        elif cmd.upper() == "H":
            nx = num()
            if rel:
                nx += x
            cur.append((nx, y))
            x = nx
        elif cmd.upper() == "V":
            ny = num()
            if rel:
                ny += y
            cur.append((x, ny))
            y = ny
        elif cmd.upper() == "C":
            x1, y1, x2, y2, nx, ny = (num() for _ in range(6))
            if rel:
                x1, y1, x2, y2, nx, ny = x + x1, y + y1, x + x2, y + y2, x + nx, y + ny
            p0 = (x, y)
            for k in range(1, 13):
                tt = k / 12.0
                px = (1 - tt) ** 3 * p0[0] + 3 * (1 - tt) ** 2 * tt * x1 + \
                     3 * (1 - tt) * tt * tt * x2 + tt ** 3 * nx
                py = (1 - tt) ** 3 * p0[1] + 3 * (1 - tt) ** 2 * tt * y1 + \
                     3 * (1 - tt) * tt * tt * y2 + tt ** 3 * ny
                cur.append((px, py))
            x, y = nx, ny
        elif cmd.upper() == "Q":
            x1, y1, nx, ny = (num() for _ in range(4))
            if rel:
                x1, y1, nx, ny = x + x1, y + y1, x + nx, y + ny
            p0 = (x, y)
            for k in range(1, 13):
                tt = k / 12.0
                px = (1 - tt) ** 2 * p0[0] + 2 * (1 - tt) * tt * x1 + tt * tt * nx
                py = (1 - tt) ** 2 * p0[1] + 2 * (1 - tt) * tt * y1 + tt * tt * ny
                cur.append((px, py))
            x, y = nx, ny
        else:
            i += 1
    if cur:
        subs.append(cur)
    return subs


def stroke_points(draw: ImageDraw.ImageDraw, pts, scale: float, width: float) -> None:
    if len(pts) < 2:
        return
    sp = [(x * scale, y * scale) for x, y in pts]
    sw = max(1, int(round(width * scale)))
    draw.line(sp, fill=255, width=sw, joint="curve")
    rad = sw / 2.0
    for point in (sp[0], sp[-1]):
        draw.ellipse((point[0] - rad, point[1] - rad, point[0] + rad, point[1] + rad),
                     fill=255)


def render_svg(svg_path: Path, size: int) -> bytes:
    root = ET.parse(svg_path).getroot()
    scale = 4
    hi = size * scale
    mask = Image.new("L", (hi, hi), 0)
    draw = ImageDraw.Draw(mask)
    unit = hi / 48.0
    for el in root.iter():
        tag = el.tag.split("}")[-1]
        attr = el.attrib
        fill = attr.get("fill", "none")
        stroke = attr.get("stroke", "none")
        sw = float(attr.get("stroke-width", "1"))
        if tag == "path":
            for pts in parse_path(attr.get("d", "")):
                if fill != "none":
                    draw.polygon([(x * unit, y * unit) for x, y in pts], fill=255)
                if stroke != "none":
                    stroke_points(draw, pts, unit, sw)
        elif tag == "circle":
            cx = float(attr["cx"]) * unit
            cy = float(attr["cy"]) * unit
            r = float(attr["r"]) * unit
            draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
        elif tag == "rect":
            x0 = float(attr["x"]) * unit
            y0 = float(attr["y"]) * unit
            x1 = x0 + float(attr["width"]) * unit
            y1 = y0 + float(attr["height"]) * unit
            r = float(attr.get("rx", "0")) * unit
            draw.rounded_rectangle((x0, y0, x1, y1), radius=r, fill=255)
        elif tag == "line":
            pts = [(float(attr["x1"]), float(attr["y1"])),
                   (float(attr["x2"]), float(attr["y2"]))]
            stroke_points(draw, pts, unit, sw)
    out = mask.resize((size, size), Image.Resampling.LANCZOS)
    return np.asarray(out, dtype=np.uint8).tobytes()


def emit_c_array(name: str, data: bytes, per_line: int = 16) -> str:
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def descriptor(name: str, cf: str, w: int, h: int, stride: int, data: bytes) -> str:
    return f"""const lv_image_dsc_t {name} = {{
    .header = {{
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = {cf},
        .w = {w},
        .h = {h},
        .stride = {stride},
    }},
    .data_size = {len(data)}U,
    .data = {name}_map,
}};
"""


def write_image_source(filename: str, symbol: str, cf: str, w: int, h: int,
                       stride: int, data: bytes) -> None:
    src = ("/* Generated by 04_开发工具/播放器素材生成/generate_assets.py. Do not edit. */\n"
           '#include "lvgl.h"\n#include "music_assets.h"\n\n')
    src += emit_c_array(f"{symbol}_map", data)
    src += "\n" + descriptor(symbol, cf, w, h, stride, data)
    (ASSET_OUT / filename).write_text(src, encoding="utf-8")


def write_icon_source() -> None:
    names = list(ICONS)
    src = ("/* Generated by 04_开发工具/播放器素材生成/generate_assets.py. Do not edit. */\n"
           '#include "lvgl.h"\n#include "music_assets.h"\n\n')
    for size in (24, 32):
        for name in names:
            data = render_svg(ICON_DIR / f"{name}.svg", size)
            src += emit_c_array(f"icon_{name}_{size}_map", data) + "\n"
            src += descriptor(f"icon_{name}_{size}", "LV_COLOR_FORMAT_A8",
                              size, size, size, data) + "\n"
    src += "const lv_image_dsc_t *music_asset_icon(music_icon_t icon, bool large)\n{\n"
    src += "    static const lv_image_dsc_t *const small[] = {\n"
    for name in names:
        src += f"        &icon_{name}_24,\n"
    src += "    };\n    static const lv_image_dsc_t *const big[] = {\n"
    for name in names:
        src += f"        &icon_{name}_32,\n"
    src += "    };\n"
    src += "    if (icon < 0 || icon >= MUSIC_ICON_COUNT) return NULL;\n"
    src += "    return large ? big[icon] : small[icon];\n}\n"
    (ASSET_OUT / "music_icons.c").write_text(src, encoding="utf-8")


def write_header() -> None:
    names = list(ICONS)
    lines = [
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "/* 预烘焙音乐播放器素材；全部数据为 const，驻留 Flash。 */",
        "extern const lv_image_dsc_t music_bg;",
        "extern const lv_image_dsc_t music_vinyl;",
    ]
    for i in range(4):
        lines.append(f"extern const lv_image_dsc_t music_cover_{i};")
        lines.append(f"extern const lv_image_dsc_t music_card_{i};")
    lines += [
        "",
        "typedef enum {",
    ]
    for name in names:
        lines.append(f"    MUSIC_ICON_{name.upper()},")
    lines += [
        "    MUSIC_ICON_COUNT,",
        "} music_icon_t;",
        "",
        "const lv_image_dsc_t *music_asset_icon(music_icon_t icon, bool large);",
        "",
    ]
    header = "\n".join(lines)
    HEADER.write_text(header, encoding="utf-8")


def main() -> None:
    ensure_dirs()
    # 删除历史生成物，避免尺寸变化后残留被 CMake glob 误编译。
    for old in ASSET_OUT.glob("*.c"):
        old.unlink()
    for old in ASSET_OUT.glob("*.bin"):
        old.unlink()

    build_background()
    write_image_source("music_bg.c", "music_bg", "LV_COLOR_FORMAT_RGB565",
                       W, H, W * 2, (ASSET_OUT / "music_bg.bin").read_bytes())
    build_vinyl()
    write_image_source("music_vinyl.c", "music_vinyl", "LV_COLOR_FORMAT_RGB565A8",
                       344, 344, 344 * 2, (ASSET_OUT / "music_vinyl.bin").read_bytes())
    for track in range(4):
        build_cover(track)
        write_image_source(f"music_cover_{track}.c", f"music_cover_{track}",
                           "LV_COLOR_FORMAT_RGB565A8", 180, 180, 180 * 2,
                           (ASSET_OUT / f"music_cover_{track}.bin").read_bytes())
        write_image_source(f"music_card_{track}.c", f"music_card_{track}",
                           "LV_COLOR_FORMAT_RGB565A8", 160, 160, 160 * 2,
                           (ASSET_OUT / f"music_card_{track}.bin").read_bytes())
    write_svgs()
    write_icon_source()
    write_header()
    total = sum(p.stat().st_size for p in ASSET_OUT.glob("*.bin"))
    print(f"generated assets: {total} bytes raw; previews in {PREVIEW_DIR}")


if __name__ == "__main__":
    main()
