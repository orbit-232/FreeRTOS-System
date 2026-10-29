# Lock-screen wallpaper asset

Source: `C:/Users/30782/Downloads/windows-11-stock-official-blue-background-apps-2021-3840x2160-5781.jpg`

Prepared at 1280x800 (16:10, centered 3456x2160 crop, Lanczos), raw RGB565 little-endian, 2,048,000 payload bytes. Header (24 bytes, little-endian) is:

`PWL1` | u16 width=1280 | u16 height=800 | u16 format=1 | u16 reserved=0 | u32 data_bytes=2048000 | u32 crc32 | u32 reserved=0

Deployment (Codex, no reformat/overwrite of existing files):

1. Copy `lock_wallpaper.rgb565` to `/sdcard/panel/assets/lock/lock_wallpaper.rgb565` (create the `lock` directory if needed).
2. Keep existing `panel/assets/manifest.json` and `fonts/file_cjk_20.pfnt` unchanged.
3. Missing, truncated, bad-header or bad-CRC file leaves the lock screen on its existing gradient; no internal-RAM allocation is used.

SHA-256: `9984c2a1ebff3f1ec8b94e47c783e080ab995a4c4ba70163774d120b7b13eb28`
