# 播放器素材生成

本目录保存可复现的素材生成脚本、SVG 图标源和预览图。生成物写入
`03_固件工程/components/panel_music_ui/assets/` 与 `include/music_assets.h`。

## 生成

在仓库根目录运行（`$env:PYTHON` 可选；未设置时使用 PATH 中的 `python`）：

```powershell
$py = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
& $py '.\微雪屏幕工程\04_开发工具\播放器素材生成\generate_assets.py'
```

需要 Python 3.12+、Pillow、numpy。脚本自动按自身位置解析工程根目录，不依赖
用户名或本机绝对路径。

## 设计

- 背景 1280×800：基色 `#101012`，紫/橙暗光，8×8 Bayer 抖动到 RGB565，
  作为 `const` 数据存 Flash。
- 唱片 344×344：预烘焙沟槽、边缘、斜向高光；运行时不加阴影。
- 四首封面各 180×180；曲目卡各 160×160；均为预烘焙 RGB565A8。
- 图标：自制 SVG，脚本用 4× 超采样转 A8，LVGL 运行时仅 recolor。
- 所有 `.c` 文件顶部标注自动生成，不手改；尺寸/颜色变化请改脚本后重生成。

## 产物

```text
assets/music_bg.c            背景
assets/music_vinyl.c         唱片
assets/music_cover_0..3.c    圆封面
assets/music_card_0..3.c     曲目卡
assets/music_icons.c         15 个图标 × 24/32 px A8
include/music_assets.h       外部描述符与图标枚举
preview/                     生成预览图，不是面板照片
```

原始二进制 `.bin` 与预览图仅作生成中间物，固件使用生成的 `.c`。
