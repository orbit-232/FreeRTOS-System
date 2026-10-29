# 微雪屏幕工程

ESP32-P4-WIFI6-Touch-LCD-10.1 的七页原生 LVGL 界面工程，强制 Rev1.3（pre-v3）构建。

## 当前开发入口

Android 三键/Overview 基线 **1.1.0-android** 已实机三轮 B1 并冻结，用户已确认三键/最近应用正常。

B2 raw 字体候选 **1.1.0-android-fonts** 实机在 control close 后卡死；PPA guard 三轮 cold 43 动作及三轮不重启 42 动作均通过，`guard_hits=50`。cache64 候选已由 Codex 冻结，独立 simulator 16 页面 PPM hash 与 cache0 全部一致。dual 三轮实测 P50/P95 均差于 cache64，最终选择 **cache64 单 worker**。

## 正式版本 1.1.0（已验收）

- 版本：`PANEL_FW_VERSION`/`PROJECT_VER` = `1.1.0`
- 配置：`CONFIG_LV_DRAW_SW_SHADOW_CACHE_SIZE=64`、`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`
- 字体：raw；PPA 窄条 guard + parallel font build guard
- 状态：Codex 独立全构建、烧录与 bootloader/partition/app 三段校验通过；最终 43 个导航动作通过。

原始 `1.1.0-android-fonts` 的窄条 PPA 失败不计入成绩。用户已确认正式版：“文字、方向和触摸正常，动画更顺滑”。三键导航和最近应用此前已确认正常。

## 阶段 A 应用恢复（开发中）

- 源码版本：`PANEL_FW_VERSION`/`PROJECT_VER` = `1.1.1-apps-a`，尚未发布或实机验收。
- 新增真实 Wi-Fi Hosted 服务、BLE Hosted/NimBLE 服务、网络设置页、密码键盘；Wi-Fi 凭据只写 NVS，不打印、不打包。
- 主屏移除相机/相册/家居并按 3×3 重排；控制中心显示真实 Wi-Fi/蓝牙状态。
- 新增中文字形按 raw 重新生成；正式 `1.1.0` 发布包未改动。

三轮实机测量中，活动 LVGL 更新间隔 P50 从 91.10 ms 降至 66.96 ms，P95 从 160.20 ms 降至 115.94 ms；这些指标不是屏幕扫描 FPS。完整证据见 [最终评审](01_项目文档/navigation_fps_final_review.md)。

说明见：

- [导航/动画阶段 A](01_项目文档/navigation_animation_phase_a.md)
- [BLE 生命周期最小方案 A 交付](01_项目文档/ble_lifecycle_plan_a_report.md)
- [Android 三键与 Overview](01_项目文档/android_three_button_navigation.md)
- [B1 基线协议](01_项目文档/navigation_performance_baseline_protocol.md)
- [B2 candidate 1 失败/修复](01_项目文档/navigation_fps_candidate1.md)
- [中文工程架构说明](01_项目文档/中文工程架构说明.md)

## 快速命令

```powershell
# 主机测试
& '.\工程入口.ps1' -操作 测试

# 构建正式 1.1.0
& '.\工程入口.ps1' -操作 构建

# 渲染
& '.\工程入口.ps1' -操作 渲染

# 烧录（仅授权执行者操作 COM3；脚本先验证 Rev1.3/32MB，不整片擦除）
& '.\04_开发工具\flash.ps1' -Port COM3

# 重新生成字体（默认 raw；显式指定避免静默回压缩）
node '.\04_Tools\字体工具\generate_fonts.js' --encoding raw

# 字体覆盖/编码检查
& '.\工程入口.ps1' -操作 字体检查
```

## 1.0.0 历史

1.0.0 首次烧录记录和现场截图见 `05_版本发布/v1.0.0-rev1_3/README.md` 及其 `evidence/screenshots`。该历史记录不代表当前候选已烧录。
