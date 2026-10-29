# Noto Sans SC 字体来源与授权

本目录的 `music_font_*.c` 是 Noto Sans SC 2.002 的 4bpp 原始位图子集，仅包含
音乐播放器界面实际使用的 ASCII 和中文/标点字形。

## 原始字体

| 用途 | 字体 | 文件名 | SHA-256 |
| --- | --- | --- | --- |
| 常规 12/13/14/16 | Noto Sans SC Regular 2.002 | `NotoSansSC-Regular.otf` | `A2B93E6C2DB05D6BBBF6F27D413EC73269735B7B679019C8A5AA9670FF0FFBF2` |
| 粗体 15/32 | Noto Sans SC Bold 2.002 | `Noto Sans SC Bold (TrueType).otf` | `D1961BE1161EA1BE08496C920862D06EA5C23A757628F4FD69368DE1D9F51BED` |

原始 OTF `name` 表实测值（Regular 与 Bold 相同）：

- nameID 0（Copyright）：`© 2014-2020 Adobe (http://www.adobe.com/).`
- nameID 1 / 4（Family）：`Noto Sans SC`
- nameID 2（Subfamily）：`Regular` / `Bold`
- nameID 5（Version）：`Version 2.002;hotconv 1.0.116;makeotfexe 2.5.65601`
- nameID 8（Manufacturer）：`Adobe`
- nameID 11（Vendor URL）：`http://www.google.com/get/noto/`
- nameID 13（License）：`SIL Open Font License, Version 1.1`
- nameID 14（License URL）：`http://scripts.sil.org/OFL`

## 许可

本字体及本目录位图子集按 **SIL Open Font License 1.1** 使用和再分发。完整许可证
文本见同目录 `OFL.txt`；版权声明已按上述 nameID 0 原始元数据更正，不再使用旧模板
中与 Noto Sans SC 不符的 `Source` 保留字体名声明。

Noto 是 Google Inc. 的商标；本工程不宣称对字体名称或商标的所有权。

## 可复现生成

1. 常规字体来自工程内：
   `03_Firmware/components/panel_ui/assets/font/NotoSansSC-Regular.otf`。
2. 粗体使用同名 Noto Sans SC Bold 2.002 OTF。公开发布环境应通过
   `NOTO_SANS_SC_BOLD` 环境变量指向本机合法安装的该字体文件，不提交系统字体文件。
3. 在仓库根运行：

   ```powershell
   $env:NOTO_SANS_SC_BOLD = 'C:\path\to\Noto Sans SC Bold (TrueType).otf'
   & '.\微雪屏幕工程\04_开发工具\字体工具\generate_music_fonts.js'
   ```

该脚本生成六款 `music_font_*.c` 和 `music_font_manifest.json`；manifest 使用相对
输出路径和描述性来源，不记录本机用户名或绝对路径。
