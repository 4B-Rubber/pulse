# 缩略图缓存与用户数据目录

缩略图缓存现在有界面可查可清，并且和应用的其它数据放在同一个用户目录里。

## 行为

- **设置页**：设置 → 通用 → 缩略图缓存 显示当前占用（磁盘缓存目录的大小）并可一键清除；清除是幂等的，之后打开文件会重新生成。
- **位置**：全部位于 `%LOCALAPPDATA%\Pulse`（`pulse::PulseDataDir()`）之下。自测通过 `PULSE_TEST_DATA_DIR` 重定向到临时目录，绝不触碰真实目录。
- **失败不影响功能**：缓存目录不可写时只退化成“每次都重新生成”，不阻塞缩略图显示。

## 实现入口

- 内存 LRU 与磁盘缓存：`src/ui/thumbnail_cache.{h,cpp}`（`DiskCacheDir()` 走 `pulse::PulseDataDir()`）。
- 目录的唯一来源：`src/common/pulse_data_dir.h`（app 与预览进程共用，含自测重定向）。
- 设置页：`src/app/settings_controller.cpp`、`src/ui/ui_settings_core.cpp`、`src/ui/ui_settings_view.cpp`。
- 用例：`pulse_thumbnail_cache_test`（含缓存占用与清除）、`pulse_preview_test`。
