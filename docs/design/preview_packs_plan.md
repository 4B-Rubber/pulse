# Pulse 预览增强包方案（FFmpeg 及其他）

> 状态：方案稿，未改代码｜配套效果图：`enhancement_packs_mockup.html`
>
> 依据的源码事实：
> - `src/ui/video_preview.{h,cpp}`：MFPlay，`missing_decoder` / `store_id`
> - `src/ui/preview_format_catalog.h`：支持格式卡片，以及 HEIF/HEVC/AV1/WebP 四个商店扩展
> - `src/preview_host/preview_router.h`：解码器表与回退链
> - `src/preview_host/archive_listing.h`：系统 libarchive `archiveint.dll`，Windows 10 1803 及以上
> - `src/app/update_transport.h` / `update_installer.h`：HTTPS 流式下载、加速镜像回退、哈希校验
> - `src/index/pdfium_text.*`：搜索索引

---

## 1. 现状与痛点

| 场景 | 现在 | 问题 |
| --- | --- | --- |
| 视频播放（Quick Look） | MFPlay 播放；缺解码器时置 `missing_decoder`，引导去微软商店 | HEVC 扩展收费；国内不少环境打不开商店；Windows N 版没有 Media Foundation；MKV 内的 FLAC/DTS、FLV、RMVB、TS 等经常只有声音或直接失败 |
| 视频缩略图、时长 | 依赖 Shell 缩略图提供程序（底层同样是 Media Foundation） | 上面这些格式在网格里只显示图标，没有时长徽标 |
| 音频 | MF 支持的格式（mp3/flac/m4a/ogg/opus 等） | APE、WV、TTA、DSF/DFF、AC3/DTS、AMR、MKA 等无法播放，也画不出波形 |
| 图像 | WIC + 原生解码；HEIF/AVIF/WebP 依赖商店扩展 | 没装扩展时 HEIC/AVIF 打不开；JPEG XL、EXR、RAW（CR3/NEF/ARW…）没有通用方案 |
| 压缩包 | ZIP 原生读取；7z/RAR/tar 走系统 `archiveint.dll` | Win8.1 版没有这个 DLL；分卷、加密文件名等情况支持有限 |
| 搜索 | 文本 + PDF（PDFium）建索引 | 截图、扫描件里的文字搜不到 |

共同特点：**能力很有价值，但体积大、许可证复杂，或者只有部分用户需要**。这正适合做成“可选预览增强包”，而不是塞进安装包。

---

## 2. 总体设计

### 2.1 原则

1. **用户自选**：Pulse 本体不变大；预览增强包按需下载，可随时卸载，卸载后行为与现在完全一致。
2. **进程隔离**：预览增强包的二进制只在 `preview_host` 或它派生的子进程里运行，永远不加载进 UI 进程。预览增强包崩溃只影响这一次预览，现有回退链继续兜底。
3. **复用现有基础设施**：
   - 下载复用 `ReadUpdateWithFallback`（大小上限、取消、超时，GitHub 失败自动走 `AcceleratedUpdateUrl` 镜像）；
   - 校验复用 `VerifyUpdateInstaller` 的 SHA-256 思路和更新清单的签名校验。
4. **无需管理员权限**：安装到 `%LOCALAPPDATA%\Pulse\packs\<id>\<version>\`，与现有“静默覆盖安装”的升级流程互不干扰。升级 Pulse 时保留预览增强包。
5. **许可证干净**：Pulse 只以独立可执行文件或 DLL 的方式调用预览增强包，不静态链接；每个预览增强包附带 LICENSE 和源码获取方式。

### 2.2 模块划分（新增 `src/packs/`）

| 模块 | 职责 |
| --- | --- |
| `pack_manifest` | 解析远程清单（JSON），校验 schema、签名和最低 Pulse 版本 |
| `pack_store` | 本地状态 `packs.json`：已装版本、文件哈希、启用开关、自定义路径 |
| `pack_installer` | 下载到临时目录 → 校验 SHA-256 → 解压（复用现有 zip 读取）→ 原子重命名为 `<version>` → 切换 `current` → 后台清理旧版本 |
| `pack_registry` | 能力查询：`Packs::Find(Capability::VideoThumbnail)` 返回可执行路径或 DLL 路径；线程安全快照；变更时通知 |
| `pack_runner` | 在 Job Object 中启动工具：随父进程结束、内存上限、BELOW_NORMAL 优先级、无 Shell、参数逐个转义、超时、管道读取有上限 |

### 2.3 远程清单（示例）

清单与 Pulse 更新清单放在同一位置，同样签名，同样走加速镜像。

```json
{
  "schema": 1,
  "packs": [{
    "id": "ffmpeg",
    "name": {"zh-CN": "FFmpeg 媒体增强", "zh-TW": "FFmpeg 媒體增強", "en": "FFmpeg Media"},
    "version": "7.1.1-pulse.1",
    "min_pulse": "1.0.53",
    "files": {"x64": {"url": ".../pulse-pack-ffmpeg-7.1.1-x64.zip", "sha256": "…", "size": 29360128}},
    "license": "LGPL-2.1-or-later",
    "source": "https://github.com/jimmgreen/pulse-packs/tree/ffmpeg-7.1.1",
    "capabilities": ["video.thumbnail", "video.probe", "video.playback", "audio.decode"],
    "extensions": [".mkv", ".webm", ".flv", ".ts", ".rmvb", ".ape", ".dsf", "…"],
    "entry": {"ffmpeg": "bin/ffmpeg.exe", "ffprobe": "bin/ffprobe.exe"}
  }]
}
```

### 2.4 与预览管线的接入点

- **`preview_host` 解码器表**：按能力条件追加条目，位置在“原生 → 内嵌缩略图 → Shell 提供程序”之后、“文本 / 十六进制”之前。原生和系统能处理的文件完全不受影响；预览增强包只补系统处理不了的部分。
- **路径下发**：`preview_host` 启动时读取 `pack_registry` 快照。安装、卸载后 Pulse 平滑重启 `preview_host`（现有机制已能处理宿主重启），然后自动重试当前预览。
- **格式目录**：`PreviewFormatGroups()` 合并预览增强包的扩展名。设置里“支持的格式”卡片中，由预览增强包提供的格式带“预览增强包”角标，安装后角标消失，不会和实际能力脱节。
- **缓存**：预览增强包生成的缩略图照常进入 `thumbnail_cache`。缓存键加入预览增强包版本号，升级后自然失效。

---

## 3. FFmpeg 媒体预览增强包（首个预览增强包）

### 3.1 构建与分发

- 自建**仅解码**的 LGPL 构建：关闭全部编码器和 GPL / nonfree 组件，启用 dav1d（AV1），保留内置的 HEVC、VP9、ProRes 等解码器。预估压缩包约 25–30 MB；第三方完整构建约 80 MB 以上。
- 只发布 `ffmpeg.exe` + `ffprobe.exe`。Authenticode 签名，与 Pulse 安装包使用同一证书。
- 托管在 GitHub Releases（`pulse-packs` 仓库），并由 `AcceleratedUpdateUrl` 提供国内镜像。
- **“使用已安装的 FFmpeg”**：检测 PATH，以及 winget、scoop、choco 的常见安装位置；用户也可以手动指定路径。用前跑一次 `-version` 确认版本。自定义路径不做哈希校验，界面上明确提示。

### 3.2 能力分期

| 阶段 | 能力 | 实现 |
| --- | --- | --- |
| A | **视频缩略图 + 时长徽标** | `ffmpeg -ss <10%> -i f -frames:v 1 -vf thumbnail,scale=… -f rawvideo -pix_fmt bgra pipe:1`。像素经管道读回，直接填进 `DecodeResult`；时长来自 ffprobe，喂给网格的播放时间徽标（`app_prefs.h` 已有此偏好） |
| A | **媒体信息** | `ffprobe -of json -show_format -show_streams`：编码、分辨率、帧率、HDR、码率、音轨、字幕、章节，进入详情窗格（`preview_properties`） |
| A | **故事板** | Quick Look 无法播放时，展示 9–16 张关键帧网格，鼠标横扫即可预览。成本低、观感好 |
| B | **Quick Look 播放兜底** | `missing_decoder` / `unsupported_audio` 时切到 FFmpeg 管线：视频以 rawvideo BGRA 按视口尺寸缩放后经管道读出，复用现有视频矩形的绘制；音频解码为 48 kHz s16 走 WASAPI；跳转通过重启并带 `-ss` 实现。定位是“预览级”播放，不追求完整播放器 |
| B | **音频格式** | APE/WV/TTA/DSF/DFF/AC3/DTS/AMR/MKA 等解码为 PCM，复用 `quick_preview_audio` 的波形和播放 |
| C（可选） | **libav 直连** | 如果 B 的管道方案在 4K 跳转和音画同步上不够用，再改为独立 media host 进程加载 libav* DLL（LGPL 动态链接），用 D3D11VA 硬解 |

### 3.3 新增可预览格式（示例）

- **视频**：flv、f4v、ts、m2t、vob、rmvb、rm、asf、ogv、mxf、divx、y4m，以及各种封装里的 HEVC、AV1、VP9、ProRes。
- **音频**：ape、wv、tta、dsf、dff、mka、ac3、dts、amr、caf。

### 3.4 体验

- **上下文提示，而不是让用户去设置里找**。Quick Look 遇到无法播放的视频时，就地显示：
  - “系统缺少 HEVC (H.265) 解码器，只能播放声音”
  - 主按钮【安装 FFmpeg 预览增强包 · 28 MB】，次按钮【从微软商店获取 HEVC 扩展】（保留现有路径）
  - 安装完成后当前预览自动重试。
- 网格里无法生成缩略图的视频，第一次悬停时在状态栏给出一次性提示（复用现有“操作提示 / 教学气泡”机制），可关闭。

---

## 4. 其他预览增强包候选

按“价值 / 成本 / 许可证风险”综合排序：

| 优先级 | 预览增强包 | 解决什么 | 技术与许可证 | 体积（估算） |
| --- | --- | --- | --- | --- |
| ★★★ | **现代图像格式** | HEIC/HEIF、AVIF、JPEG XL、OpenEXR、HDR、QOI；**不依赖商店扩展** | libheif（LGPL-3）+ libde265（LGPL-3）+ dav1d（BSD）+ libjxl（BSD）；以 DLL 形式在 `preview_host` 动态加载 | 8–10 MB |
| ★★★ | **RAW 相机照片** | CR2/CR3/NEF/ARW/DNG/RAF/ORF/RW2 等：缩略图、全尺寸预览、EXIF | LibRaw（LGPL-2.1 / CDDL 双许可）；缩略图优先取内嵌 JPEG，速度很快 | 2–3 MB |
| ★★☆ | **7-Zip 压缩包增强** | Win8.1 可用；RAR5、分卷、加密文件名、ISO、CAB、WIM 等更全的列表；后续可支持“解压到…” | 7z.dll（LGPL-2.1 + unRAR 限制条款：只能解压，不能用于实现 RAR 压缩） | 约 2 MB |
| ★★☆ | **OCR 图片文字识别** | 截图、扫描件、照片里的文字进入搜索索引（与 `index/` 管线对接）；Quick Look 可选中复制文字 | PaddleOCR 模型（Apache-2.0）+ ONNX Runtime（MIT），全部本地运行、不上传。轻量备选：系统 `Windows.Media.Ocr`，无需预览增强包，但中文效果一般 | 25–35 MB |
| ★☆☆ | **3D 模型预览** | STL/OBJ/glTF/GLB/3MF/FBX：缩略图和可旋转预览（系统 3D 查看器已下架） | assimp（BSD-3）加简单的 D3D 渲染 | 约 10 MB |
| ★☆☆ | **Office 高保真（借用已安装软件）** | 现有 docx/xlsx/pptx 原生模型是草图级；需要高保真时，借用用户已安装的 LibreOffice，`soffice --headless --convert-to pdf` 后交给现有 PDFium 渲染 | 不分发（LibreOffice 约 350 MB），只检测并接入；检测不到时显示“了解” | 0（借用） |

明确**不建议**的：

- **Ghostscript**（EPS/PS）：AGPL，分发和调用的合规成本高。
- **LibreDWG**（完整 DWG 渲染）：GPL；而且现有 `dwg_thumb` 已能显示内嵌缩略图。
- **ImageMagick 全家桶**：体积大、攻击面广，与上面几个预览增强包功能重叠。

---

## 5. 设置界面（详见效果图）

- 设置左侧导航新增 **“预览增强包”** 页，放在“重复文件”之后。
- 页面结构：
  1. 顶部摘要：已安装数量、占用空间、存放位置【打开】。
  2. 按类别分组（媒体 / 图像 / 压缩与文档 / 搜索 / 实验）。每个预览增强包一张卡片，包含：名称、一句话用途、能力标签、新增格式数（可展开）、体积、许可证与源码链接、状态（未安装 / 下载中 x% / 已安装 v / 有更新）和操作（安装 / 更新 / 卸载 / 启用开关）。
  3. **系统扩展（微软商店）**：现有 HEVC/HEIF/AV1/WebP 检测卡片移到这里，与预览增强包放在一起，用户一眼能看到两条路。
  4. **高级**：自定义 FFmpeg 路径（带自动检测）、自动更新预览增强包、下载源（自动 / GitHub / 加速镜像）。
- “通用 › Quick Look › 支持的格式”卡片中，预览增强包提供的格式显示角标，点击跳到对应预览增强包。

---

## 6. 安全与稳定

- 安装时校验清单签名和 SHA-256，并记录文件大小、修改时间和哈希。每次 Pulse 启动后先做廉价校验（大小 + 修改时间），后台再做一次完整哈希；不一致则停用该预览增强包并提示重新安装。
- `pack_runner`：
  - Job Object 限制内存（例如 1 GB）和进程数，`KILL_ON_JOB_CLOSE`；
  - 每次调用设超时：缩略图 5 秒，媒体信息 3 秒；
  - 管道输出有上限，防止异常文件拖垮预览宿主；
  - 离线云占位文件（`DecodeRequest::offline`）**一律不交给预览增强包**，避免触发下载。
- 失败计数：同一预览增强包连续崩溃或超时达到阈值后，本次会话内自动停用，在“关于与诊断”中可以看到原因。
- 隐私：所有预览增强包本地运行；除下载预览增强包本身外不联网。

---

## 7. 分期

| 阶段 | 内容 | 规模 |
| --- | --- | --- |
| P1 | 预览增强包框架（manifest / store / installer / registry / runner）+ 设置页 + FFmpeg 阶段 A（缩略图、时长、媒体信息、故事板）+ Quick Look 上下文提示 | L |
| P2 | FFmpeg 阶段 B（播放兜底 + 音频格式） | M–L |
| P3 | 现代图像格式包 + RAW 包（同为 `preview_host` 内的 DLL 解码器，共用一套接入代码） | M |
| P4 | 7-Zip 包（同时补齐 Win8.1 的压缩包预览） | S–M |
| P5 | OCR 包（接入搜索索引，需单独设计索引调度） | L |
| 以后 | 3D 模型、Office 高保真借用 | M |

验证沿用 Pulse 的 AGENTS.md 规范：

- 只构建受影响的目标；
- 下载、安装、卸载、更新的单元测试放在 `src/bench/`，可参考现有 `update_installer_test` 的写法；
- UI 改动附深色、浅色截图；
- 全部使用隔离数据目录（`PULSE_TEST_DATA_DIR`），不碰用户的预览增强包和设置。

---

## 8. 需要你拍板的问题

1. **FFmpeg 来源**：自建仅解码构建并托管在 `pulse-packs` 仓库（推荐），还是直接引导下载第三方构建？
2. **首批范围**：P1 只做 FFmpeg，还是 FFmpeg 和现代图像格式包一起做？
3. **入口**：除了设置页和上下文提示，安装程序（Inno Setup）里要不要加“可选预览增强包”勾选页？默认不勾选。
4. **卸载 Pulse 时**是否一并删除预览增强包目录？建议删除。

## 9. 决策记录（2026-10-05）

| 问题 | 决定 |
|---|---|
| 名称 | 界面与文档统一称为 **预览增强包** |
| FFmpeg 来源 | 自行编译 **仅解码** 的 LGPL 构建，签名 + SHA-256 校验 |
| 第一批范围 | **FFmpeg + 现代图像格式**（HEIC / AVIF / JPEG XL） |
| 安装程序 | 不加选项页，保持静默覆盖升级；增强包只在设置页或 Quick Look 提示中按需安装 |
| 卸载 Pulse | 默认删除 `%LOCALAPPDATA%\Pulse\packs`（设置中可改为保留） |
| 性能 | 接入必须不拖慢现有路径：只有原生解码器返回 Next/Failed 时才调用增强包；进程按需启动、限并发、带超时；结果写入现有缩略图缓存 |
