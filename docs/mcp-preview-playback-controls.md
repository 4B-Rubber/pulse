# 快速预览：GIF / 视频播放控制

日期：2026-09-21。

## 本次范围

为 `QuickPreviewWindow` 补齐播放控制，不改变主列表、索引、安装器或其它预览功能。保留工作区原有未提交修改，包括 GIF 换帧时保留底部信息的修复。

此前 GIF 只有自动换帧；视频交由通用系统预览处理器，应用没有可靠的播放控制接口。

## 交互

| 功能 | GIF / 多帧图像 | 视频 |
| --- | --- | --- |
| 播放 / 暂停 | 支持 | 支持 |
| 逐帧 | 上一帧、下一帧；到边界不循环 | 下一帧，调用 MFPlay 的真实 FrameStep |
| 进度条 | 按帧定位 | 按媒体时间定位，要求媒体可 seek |
| 信息 | 已显示帧 / 总帧数、播放帧延时、播放状态 | 当前时间 / 时长、分辨率、成功逐帧次数、播放状态 |

- 拖动时暂停，释放后恢复拖动前的播放状态；原本暂停的内容保持暂停。
- 鼠标捕获丢失时结束拖动并保持暂停，关闭或切换文件时清理控制状态。
- GIF 等待解码时继续显示已提交帧的信息，不把尚未显示的请求帧当作当前帧。
- 原有尺寸、文件大小、格式、缩放 / 适应信息继续保留在 GIF 的 HUD 中；控制条单独预留空间。
- 视频定位请求合并到最新目标，逐帧操作串行执行。拖动定位前先完成暂停，避免连续 seek 使音频无法暂停。
- 视频播放结束后再点击播放，从开头重播。

快捷键：

- `Space`：有播放控制时播放 / 暂停；普通静态预览仍保持原来的关闭行为。
- `,`：GIF 上一帧。
- `.`：下一帧。
- `Home` / `End`：暂停并定位到开头 / 末尾。
- `Esc`：关闭预览；原有方向键切换文件的行为不变。

## 实现

- `src/ui/quick_preview_playback.cpp`：播放条绘制、帧 / 时间进度交互、拖动状态与快捷键调用逻辑。
- `src/ui/playback_timeline.h`：有界帧定位和单帧步进计算。
- `src/ui/quick_preview_window.cpp`、`src/ui/quick_preview_window.h`：预留控制条空间、接入输入与渲染、维护原有 GIF 换帧逻辑。
- `src/ui/video_preview.cpp`、`src/ui/video_preview.h`：独立媒体工作线程、异步 MFPlay 回调信箱、暂停 / seek / FrameStep 与生命周期管理。

视频文件打开及媒体接口调用不放在 UI 线程；每次打开使用独立状态信箱，旧文件不能覆盖新文件。退出时先隐藏并停止播放器，待 MFPlay 释放后再销毁其渲染窗口，退休消息校验身份。MFPlay 按需动态加载，缺少媒体组件时报告错误而不是让主程序启动失败。安全模式、云端占位文件仍沿用原有保护条件。

项目自动收集 UI 源文件，因此本次没有修改 CMake 配置。当前 SDK 的 MF 头文件在较新的 NTDDI 与 Windows 8.1 WINVER 混用时出现类型声明冲突，媒体实现翻译单元将 NTDDI 限定到现有 Windows 8.1 目标；没有提升产品最低系统版本。

## 实际验证

| 检查 | 结果 |
| --- | --- |
| `cmake --build build --target pulse --parallel 4` | 成功，生成 `build/pulse.exe` |
| `bench_data/playback-regression/run.cmd` | 26 项断言通过；包含真实 H.264 / AAC MP4 播放后端测试 |
| `bench_data/playback-regression/run-ui.cmd` | 33 项断言通过；链接当前生产对象，运行真实快速预览窗口与组件绘制 |
| 相关 UI 与测试文件编辑器诊断 | 未报告 error / warning |
| 本次源码及测试脚本空白 / diff 检查 | 通过；保留既有 CRLF 文件格式 |

后端测试覆盖：进度计算边界、播放时钟、暂停冻结、暂停中 seek、真实 FrameStep、连续逐帧、快速 seek、播放结束 / 重播、快速打开关闭、失败文件、旧状态隔离、退休渲染窗口释放。10 fps 视频单帧步进的实际媒体时钟增量为 1,000,000 个 100 ns 单位，即 100 ms。

窗口测试覆盖：真实 GIF 解码自动播放、Space 不再关闭动画、前后逐帧、解码等待期间连续点击、首尾定位、鼠标拖动、暂停状态保留、拖动恢复播放、捕获丢失、有限循环停止、视频控件与后端连通、切回静态图片后的清理和原快捷键行为。

渲染检查调用生产 `DrawPlayback` / `DrawHud`，分别核对中文浅色、英文浅色、中文深色 200% 和视频控制条；已检查文字与进度条无重叠、按钮文字无裁切。最窄 GIF 样例为 480 DIP（该机器 150% 缩放下 720 像素）。

### 测试中的修正与边界

- 初版将所有 seek 目标乘以“时长减一个时钟单位”，导致帧边界前一瞬间定位后第一次单帧时间增量只有一个时钟单位。现仅对真正的末尾目标做上限约束，原单帧时间断言保留并通过。
- GDI 以及 Present 后的实时交换链读回在此环境产生空白图，不能当作视觉验收。最终通过隔离 GPU 目标调用生产控件绘制，并保留非空白像素断言。保存的是控件与 HUD 的组件渲染，不是整个应用的桌面截图，不包含视频子窗口的实际画面。
- 视频真实播放、时间与逐帧通过独立播放器及生产窗口交互验证；没有完成所有编解码器、VFR/HDR、Windows N、Windows 8.1 实机或所有高对比度状态的验收。
- 视频目前只提供真实向前逐帧，不提供反向逐帧，也不把时间跳转或估算帧号伪装成反向逐帧。视频信息中的“逐帧 +N”是最近一次定位 / 播放以来成功完成的单帧次数，不是绝对帧序号。
- 视频格式支持取决于系统 Media Foundation 解码器；打不开的媒体显示错误，可沿用 Enter / 打开命令交给默认应用。没有安装额外解码器。
- 没有运行完整历史回归、全量发布脚本或完整 `pulse --selftest`。

## 复现与材料

测试只使用生成的隔离样本，不使用用户个人媒体文件。样本可用已有 FFmpeg 重建：

```bash
mkdir -p build/playback-check
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=160x90:rate=10 -f lavfi -i anullsrc=r=48000:cl=stereo -t 4 -c:v libx264 -pix_fmt yuv420p -g 10 -c:a aac -y build/playback-check/fixture.mp4
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=160x90:rate=5 -t 2 -loop 0 -y build/playback-check/fixture.gif
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=160x90 -frames:v 1 -y build/playback-check/fixture.png
```

Windows 命令提示符下运行：

```bat
bench_data\playback-regression\run.cmd
bench_data\playback-regression\run-ui.cmd
```

- 测试源码：`src/bench/playback_controls_test.cpp`、`src/bench/playback_ui_test.cpp`。
- UI 测试链接脚本：`bench_data/playback-regression/link-ui.ps1`，复用当前生产对象，不执行应用主入口或启动索引服务。
- 日志：`build/playback-check/tests.log`、`build/playback-check/ui-tests.log`。
- 组件渲染：`build/playback-check/gif-light-480.bmp`、`build/playback-check/gif-english.bmp`、`build/playback-check/gif-dark-200.bmp`、`build/playback-check/video-dark.bmp`；同目录保存了用于查看的 PNG 转换副本。

交付主程序为 `build/pulse.exe`，从现有 build 目录连同配套运行库运行。本轮未打包安装器、未安装、未替换或重启用户正在运行的 Pulse，也未提交 Git 改动。
