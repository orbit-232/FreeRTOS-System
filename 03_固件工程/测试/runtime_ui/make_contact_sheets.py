from pathlib import Path
import hashlib
import json

from PIL import Image, ImageChops, ImageDraw, ImageStat

root = Path(r"C:\wsp\WavesharePanel\04_Tools\simulator\screenshots\transitions")
groups = [
    ("00_unlock", "Unlock: lock exits upward (0 / 120 / 260 ms)",
     ["start 0 ms", "mid 120 ms", "end 260 ms"]),
    ("01_enter", "Application enter: page slides from right (0 / 100 / 220 ms)",
     ["start 0 ms", "mid 100 ms", "end 220 ms"]),
    ("02_back", "Application back: page slides from left (0 / 100 / 200 ms)",
     ["start 0 ms", "mid 100 ms", "end 200 ms"]),
    ("03_control_open", "Control centre down (0 / 100 / 220 ms)",
     ["start 0 ms", "mid 100 ms", "end 220 ms"]),
    ("04_control_close", "Control centre up (0 / 100 / 220 ms)",
     ["start 0 ms", "mid 100 ms", "end 220 ms"]),
    ("05_home_page", "Home paging left (0 / 100 / 200 ms)",
     ["start 0 ms", "mid 100 ms", "end 200 ms"]),
    ("06_modal", "Notice fade (0 / 70 / 140 ms)",
     ["start 0 ms", "mid 70 ms", "end 140 ms"]),
    ("07_overview_light", "Overview light, fixed system bar (0 / 100 / 200 ms)",
     ["start 0 ms", "mid 100 ms", "end 200 ms"]),
    ("08_overview_dark", "Overview dark, fixed system bar (0 / 100 / 200 ms)",
     ["start 0 ms", "mid 100 ms", "end 200 ms"]),
]

manifest = {"source": "real panel_ui.c + real LVGL pages", "resolution": [1280, 800],
            "note": "Host software render, not a panel photograph.", "groups": []}

for prefix, title, labels in groups:
    paths = [root / f"{prefix}_{suffix}.ppm" for suffix in ("start", "mid", "end")]
    missing = [str(p) for p in paths if not p.exists()]
    if missing:
        raise SystemExit(f"missing frames: {missing}")
    images = [Image.open(p).convert("RGB") for p in paths]
    pngs = []
    for image, suffix in zip(images, ("start", "mid", "end")):
        png = root / f"{prefix}_{suffix}.png"
        image.save(png, "PNG", optimize=True)
        pngs.append(png)

    thumb_w, thumb_h, label_h = 426, 266, 28
    sheet = Image.new("RGB", (thumb_w * 3, thumb_h + label_h), "white")
    draw = ImageDraw.Draw(sheet)
    for i, (image, label) in enumerate(zip(images, labels)):
        thumb = image.resize((thumb_w, thumb_h), Image.Resampling.LANCZOS)
        sheet.paste(thumb, (i * thumb_w, label_h))
        draw.text((i * thumb_w + 8, 7), label, fill="black")
    contact = root / f"contact_{prefix}.png"
    sheet.save(contact, "PNG", optimize=True)

    diffs = []
    for a, b, label in ((images[0], images[1], "start_to_mid"),
                        (images[1], images[2], "mid_to_end")):
        diff = ImageChops.difference(a, b)
        stat = ImageStat.Stat(diff)
        bbox = diff.getbbox()
        diffs.append({
            "pair": label,
            "mean_abs_channel_diff": round(sum(stat.mean) / 3.0, 3),
            "bbox": list(bbox) if bbox else None,
        })

    entry = {
        "prefix": prefix,
        "title": title,
        "frames": [],
        "contact_sheet": str(contact.relative_to(root)).replace("\\", "/"),
        "differences": diffs,
    }
    for image, png, label in zip(images, pngs, labels):
        entry["frames"].append({
            "label": label,
            "file": str(png.relative_to(root)).replace("\\", "/"),
            "sha256": hashlib.sha256(png.read_bytes()).hexdigest(),
        })
    manifest["groups"].append(entry)
    print(f"{prefix}: contact={contact.name} diffs={diffs}")

(root / "frames_manifest.json").write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

# PNG is lossless; remove the 3 MB raw PPM copies after conversion.
for path in root.glob("*.ppm"):
    path.unlink()
print("PASS: converted continuous frames and removed raw PPM copies")
