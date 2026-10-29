# panel_music_ui（纯音乐启动界面）

本组件是 `music-player` 分支的唯一启动界面，直接依赖 `panel_music`、`panel_bsp`
和 `lvgl`。它不依赖 `panel_ui`，因此旧多功能 UI 不会参与启动路径。

## 编译开关

```c
#define ENABLE_DISC_SPIN 1          // 封面旋转，默认开启
#define MUSIC_UI_DISC_PERIOD_MS 50U // 20 fps 上限，20 秒/圈
```

可在编译参数中设置 `-DENABLE_DISC_SPIN=0` 关闭旋转。

## 代码职责

| 文件 | 职责 |
| --- | --- |
| `src/panel_music_ui.c` | 单屏构建、1 s 状态刷新、拖动 seek、300 ms 切歌动画、不可用提示、A8 图标 recolor |
| `src/panel_music_ui_diag.c` | 只读状态/play/pause/seek/volume/随机/循环串口命令 |
| `assets/` | 由 `04_开发工具/播放器素材生成/generate_assets.py` 生成的 const 图片 |
| `fonts/` | 由 `04_开发工具/字体工具/generate_music_fonts.js` 生成的 4bpp raw 字体 |

## 约束

- 屏幕创建/加载只在 `panel_music_ui_start()` 中持有一次 `panel_bsp_lock()`；
  页面回调不获取锁，不创建第二个 LVGL 任务。
- 时间显示只读真实后端状态，1 s 定时器不推进播放位置。
- 拖动进度环仅预览；松手调用 `panel_music_seek()`。
- 预烘焙图不在运行时缩放、模糊或加实时阴影；只有 180 px 圆封面使用图片旋转。
- 列表/队列/下载/更多/最小化未实现，必须显示明确提示。

## 字体与许可

- 六款字体是 Noto Sans SC 2.002 的 4bpp raw 子集：常规 12/13/14/16，粗体 15/32。
- 版权：`© 2014-2020 Adobe (http://www.adobe.com/).`；协议：SIL OFL 1.1。
- 完整许可证：`fonts/OFL.txt`；来源与 name 表核对结果：`fonts/FONT_NOTICE.md`。
- 可复现：

  ```powershell
  $env:NOTO_SANS_SC_BOLD = 'C:\path\to\Noto Sans SC Bold (TrueType).otf'
  & '.\微雪屏幕工程\04_开发工具\字体工具\generate_music_fonts.js'
  ```

  常规字体取自工程内 `components/panel_ui/assets/font/NotoSansSC-Regular.otf`；
  粗体不提交系统字体文件，使用 `NOTO_SANS_SC_BOLD` 指向合法本地副本。
  `fonts/music_font_manifest.json` 只记录相对路径与描述性来源，不含本机用户路径。

## 主机测试

```powershell
& '.\微雪屏幕工程\04_开发工具\音乐界面模拟器\run_music_ui_tests.ps1'
```

生成图位于 `04_开发工具/音乐界面模拟器/screenshots/`；主机测试不访问 COM/TF。
