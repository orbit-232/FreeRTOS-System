# WavesharePanel 固件

## 分层

```text
app_main
  └─ panel_ui      七页原生 LVGL、导航、主题、字体、手势
       ├─ panel_services   NVS 设置、时钟种子、运行时快照
       │    ├─ panel_model 计算器、设置模型（无 LVGL）
       │    └─ panel_bsp   唯一显示旋转/触摸变换/背光入口
       └─ LVGL 9.5.0 + esp_lvgl_adapter 0.6.4
            └─ waveshare/esp32_p4_wifi6_touch_lcd_x 2.0.3
                 └─ ESP-IDF 6.0.2 / JD9365 / GT9271
```

## 配置

- `sdkconfig.defaults`：目标、32MB Flash、PSRAM、分区表、LVGL、字体。
- `config/sdkconfig/lcd-10-1.defaults`：10.1 寸 800×1280 BSP。
- `config/sdkconfig/rev1_3.defaults`：pre-v3、最低 Rev1.0；必须最后加载。
- `config/sdkconfig/rev3_x.defaults`：仅作文档对照，未用于构建。

构建目录：`build-rev1_3/`。服务使用官方 `esp_lv_adapter` 的 LVGL 任务/锁，不另起 LVGL 调度。

## 输出

- `build-rev1_3/waveshare_panel.bin`：factory app。
- `build-rev1_3/bootloader/bootloader.bin`：0x2000。
- `build-rev1_3/partition_table/partition-table.bin`：0x8000。
- `build-rev1_3/waveshare_panel.elf` / `.map`。

## 已实现的七页

1. 锁屏：本地时钟、未校时标记、示例通知、上滑或点击解锁。
2. 主屏：两页（应用、系统概览）、9 个应用入口、实时堆/PSRAM、未实现应用提示。
3. 控制中心：顶部下拉开、底部上滑/关闭；真实背光；无线/音量/媒体明确演示。
4. 设置：网络、显示、声音、通知、系统、关于六节；亮度/主题/动画/休眠生效，自动亮度禁用并说明，触摸校准实机待验收。
5. 天气：当前/逐小时/未来几天，全部明确标注演示天气，不联网。
6. 监视器：运行时间、堆、PSRAM、任务数实测；60 秒堆历史；CPU/FPS/温度无可信接口则不显示。
7. 计算器：四则运算、连续运算、小数、正负号、百分号、清空、退格、历史、除零/溢出/超长错误。

## 实机状态

- 2026-10-20 已通过 COM3 烧录到 ESP32-P4 rev1.3（32MB Flash），写入与独立校验成功；启动日志 5,217 字节，显示/触摸/LVGL 初始化完成。
- 屏幕旋转后的四角/中心触摸、上滑/下滑/左右滑手势、背光实际亮度与休眠唤醒：仍需现场逐项验收。
- `04_开发工具/界面模拟器/screenshots/` 是主机 LVGL 渲染，不是面板照片。
