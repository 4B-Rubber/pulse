# GIF 快速预览底部信息闪烁修复

日期：2026-09-21。

## 现象与原因

GIF 预览底部的“尺寸 · 预览分辨率 · 文件大小 · GIF · 适应/缩放比例”在播放过程中短暂消失。

原因位于 `src/ui/quick_preview_window.cpp` 的 `QuickPreviewWindow::Render()`：

1. 动画计时器请求下一帧，异步解码尚未完成时，首次绘制返回 `PreviewDrawResult::Pending`。
2. 程序已经回退绘制上一张缓存帧，但没有接收这次绘制的返回值，局部变量 `result` 仍是 `Pending`。
3. 窗口每次渲染都会清屏，而底部 `DrawHud()` 只在 `result == Bitmap` 时调用。因此等待帧中有图像、没有底部文字；新帧就绪后文字又恢复，形成闪动。

## 最小修复

仅修改 `src/ui/quick_preview_window.cpp` 的动画缓存回退分支：

- 接收上一帧绘制返回值。
- 仅当上一帧确实绘制为 `Bitmap` 时，将本轮显示结果更新为 `Bitmap`，让其底部信息一并重绘。
- 缓存帧仍不可用或绘制失败时，不伪造成功结果，也不改变原来的等待语义。

回退发生在新帧提交/动画计时判断之后，所以不会提前提交下一帧、改变播放速度、重复计数循环或重新启动计时器。解码流程、帧缓存策略、尺寸元数据、缩放/平移逻辑以及文字样式均未修改。

生产代码差异为 3 行新增、1 行删除，其中包含一行说明注释。没有修改 CMake 配置、已有测试、预览解码进程或其它既有未提交改动。

## 实际验证

| 检查 | 结果 |
|---|---|
| 修复前定向渲染状态回归 | 26 项检查中 6 项失败，复现等待期间 HUD 被跳过 |
| 修复后同一回归 | 26 项检查全部通过 |
| `cmake --build build --target pulse` | 成功，退出码 0 |
| 当前源文件编辑器诊断 | 0 条 error/warning |
| 仅当前源文件的 `git diff --check` | 通过 |

状态回归覆盖：缓存旧帧、连续等待、多次绘制、下一帧提交、首次加载、静态图、缓存未就绪/失败、不同缓存分辨率、循环边界和帧延时上下限。没有运行完整 `pulse_preview_test`、全部历史测试、完整 `pulse --selftest` 或全量发布脚本。

### 验证边界

定向程序从当时的生产源码中逐字提取动画回退分支及其后的位图 HUD 调用判断，再编译执行；缓存绘制、HUD 绘制和计时器使用测试替身。它验证的是本次错误所在的状态衔接，不是完整 GIF 解码、GPU 绘制或实机视觉验收。

本轮没有对用户截图中的原始 GIF 做实机播放/逐帧录制，也没有生成真实窗口的修复前后截图，因此不声称已完成视觉签收。建议用下面的修复版打开原 GIF，确认底部信息在换帧、循环和缩放期间保持可见。

## 构建产物与运行状态

- 修复版主程序：`build/pulse.exe`，应从现有构建目录运行，不要只复制单个 EXE 而漏掉配套运行库。
- 未打包安装程序、未安装、未替换或重启用户正在运行的 Pulse。
- 未修改个人文件、偏好设置、PulseIndex 服务或其它项目。

## 复核材料

- `build/gif-footer-check/before.log`
- `build/gif-footer-check/after.log`
- `build/gif-footer-check/build.log`
- `bench_data/gif-footer-regression/probe-template.cpp`
- `bench_data/gif-footer-regression/generate-probe.ps1`
- `bench_data/gif-footer-regression/run.cmd`
- `bench_data/gif-footer-regression/transition-before.cpp`
- `bench_data/gif-footer-regression/transition-after.cpp`
- `bench_data/gif-footer-regression/context-before.json`
- `bench_data/gif-footer-regression/context-after.json`

修复前源码 SHA-256：`69e9a27dc3bb8b69ec2519c85634a0f13bed59ae2b0512ebbc9d429385ab73c8`。

修复后源码 SHA-256：`90934de4b1cd0c610ea900307589f345c75287e525c3ec340232318065b5b2f8`。
