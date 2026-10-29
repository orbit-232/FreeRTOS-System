# WavesharePanel 固件

当前分支 `music-player` 的启动界面已切换为**纯音乐播放器**；旧七页多功能 UI 只保留在
`main` 分支与已归档固件中回退，不进入当前启动路径。

## 分层

```text
app_main
  └─ panel_music_ui    单屏播放器、真实状态刷新、seek 交互、串口诊断
       ├─ panel_music      真实 MP3 worker、状态机、连续播放、收藏/随机/循环
       │    ├─ panel_music_index   帧扫描、VBR 时长、稀疏字节索引
       │    ├─ minimp3 / panel_audio / panel_files
       │    └─ PSRAM scratch + index
       ├─ panel_bsp        唯一显示旋转/触摸映射/背光/LVGL 锁
       └─ LVGL 9.5.0 + esp_lvgl_adapter 0.6.4
            └─ ESP-IDF 6.0.2 / JD9365 / GT9271
```

## 配置与构建

- `sdkconfig.rev1_3`：ESP32-P4 Rev1.3、32MB Flash、PSRAM、LVGL 9.5。
- 构建入口：`04_开发工具/build.ps1 -BuildName build-music-player`
- 产物：`build-music-player/waveshare_panel.bin` 等。
- 不改变 BSP 引脚、屏幕方向、触摸映射、TF 共享 SDMMC 和既有内存配置。

## 当前界面

- 仅音乐页：1280×800、中心 x=640、深色背景、380 px 进度环、344 px 唱片、
  180 px 封面、左右曲目卡、真实歌名/歌手/时间与播放状态。
- 1 秒定时器只刷新真实状态；播放、暂停、seek、连续播放均由后端决定。
- 列表/队列/下载/更多/最小化未实现，按钮显示明确的不可用提示。

## 输入与资源

| 输入 | 来源 | 说明 |
| --- | --- | --- |
| 音乐 | `/sdcard/panel/music/*.mp3` | 四首真实曲目，TF 共享 SDMMC |
| 触摸 | GT9271 | 沿用 `panel_bsp` 的精确映射 |
| 显示 | JD9365 1280×800 | BSP rotation270，RGB565 |

素材预烘焙在 Flash；Noto Sans SC 子集为 4bpp raw；图标为自制 SVG→A8。
详细设计与证据见：

- `01_项目文档/音乐播放器架构说明.md`
- `01_项目文档/音乐播放器验证记录.md`

## 状态

- 主机编译、真实 MP3 索引/seek 解码测试、主机界面点击与渲染已通过。
- 未提交、未烧录、未操作 COM/TF；实板触摸、seek 听感、连续播放和 20 秒封面旋转
  待 Codex 独立验证。
- `04_开发工具/音乐界面模拟器/screenshots/` 是主机渲染，不是面板照片。
- 稳定回退：`05_版本发布/v1.3.0-fps-memory-kept`（未修改）。
