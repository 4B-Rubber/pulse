# Pulse 项目日志汇总

> 由 `.codebuddy/memory/` 的 `MEMORY.md` 与 7 个日期日志（共 167 KB）精简合并而成，时间截至 2026-09-23。
> **规则 / 约定 / 坑 / 硬边界 / 用户拍板 → 见同目录 `MEMORY.md`**；本文放功能域要点、版本与 PR 时间线、当前状态（两者的重叠部分已于 2026-09-23 精简时删掉）。
> **7 个日期日志已于 2026-09-23 删除；`MEMORY.md` 保留**，本文与它并存（本文覆盖其全部要点 + 日期日志精华 + 版本时间线）。
> 新增记录按 `YYYY-MM-DD.md` 写、定期并回本文。

---

## 一、项目与仓库（git）

- **Pulse**：Windows 文件管理器（开源），C++20 + Win32 + Direct2D + DirectComposition。上游 <https://github.com/jimmgreen/pulse>。
- **远端**：`origin` = fork `4B-Rubber/pulse`（SSH 别名 `github-4brubber`，密钥 `~/.ssh/id_ed25519_4brubber`；HTTPS 凭据属于另一个账号，推 fork 会 403）；`upstream` 只读，走 HTTPS 依赖本机代理 `127.0.0.1:7890`，代理没开就用 SSH origin。
- **无 `gh` CLI、无 token**（尝试过 6 个安装路径 + 环境变量 + 凭据管理器）：PR 只能由用户在网页创建/编辑。查上游 PR 免代理：`git ls-remote git@github-4brubber:jimmgreen/pulse.git "refs/pull/*/head"`。
- **密钥纪律**：私钥只留 `~/.ssh`，绝不进 OneDrive/仓库；`ssh-keygen` 非交互必须 `-N ""`；用户已拍板不加口令。
- **提交流程**：从 `dev` 建 `feature/<主题>` → `git rebase --onto origin/main <旧基点>`（PR 只含本批改动）；一个 PR 多 commit 是默认；提交消息英文，前缀 `ui:` / `fix(scope):` / `test:` / `release:`，说"为什么"。
- **提交纪律（用户明确纠正，务必遵守）**：改动写完、自测跑过**都不构成提交理由** —— 只有用户明确说"提交"才 `git commit`；在那之前只留工作区，并告诉用户改动在哪、怎么试（用户没实测 = 没测）。
- **版本号只有一个来源：根 `version.txt`**（CMake `project(Pulse VERSION …)`、`build_installer.bat` 的 `/DAppVersion`、脚本全读它）。**本地打包从不自增**；只有**发布**才改它 + 写 `docs/releases/X.md` + 打 `vX` 标签（`publish_release.ps1` 在版本已发布时会报错要求自增）。本地重出/覆盖重出就用当前号。
- **fork 分支现状**：`main` = 上游；`dev` 是用户自己的集成分支；`feature/multi-window-tabs` 是 PR 分支（基座 = 上游 main）。已清理过一批已合并分支。
- **git 坑**：① `reset --soft <中间提交>` 会连带把它之后的提交塞进索引；② 跨提交移动**同一文件**用 `cherry-pick` 必冲突 → 用 `git checkout <sha> -- <files>` + 手工补/删 hunk；③ `git rebase --continue` 会调 Sublime（本机无）→ 设 `GIT_EDITOR=:`；④ rebase 前把用户的 `.gitignore` stash、结束后 pop。
- **本地专属文件（不提交不上推）**：`docs/build.md`、`README.md` 的链接、`.gitignore`（= 上游原版 + 空行 + `.codebuddy/`）；`git add -A` 前先 `git status`。**`.codebuddy/` 目录不要删。**

### 当前发布链路（2026-09-23，来自会话实操）

- 打 `v1.0.36` 这类标签 → GitHub Actions（`release.yml`）跑 **windows + win81 两个 matrix job**（各自 build/test/package）→ **publish** job 下载产物、用私钥签名两份清单并发布 Release（4 个资产：两个安装包 + 两份 `update-manifest*.json`）。
- 手动触发要选 **tag** 作为 ref（选分支会卡在 `Validate release tag`）。
- CI 清单的 `minimum_windows_build` = **10240（普通）/ 9600（win81）**；本地 `create_update_manifest.ps1` 默认 19045，两者不同。**本地 `dist` 包与 Release 资产同名不同内容，不要互相覆盖**；要覆盖必须重新签名清单（`scripts/sign_update_manifest_local.ps1`）。
- 本机只有 Windows PowerShell 5.1（无 pwsh 7.2），所以 `create_update_manifest.ps1` / `publish_release.ps1` / `build_release_ci.ps1` **本地跑不了**（CI 里跑）。

---

## 二、构建 / 测试 / 发布环境

### 构建树

| 树 | 用途 | 建立方式 |
|---|---|---|
| 仓库内 `build\` | Release、`PULSE_WITH_SELFTEST=OFF`，打包依赖它 | `build_release.bat`（含配置 + 全量构建） |
| 仓库内 `build-selftest\` | 自测 ON，跑 `--selftest` 用例 | 无脚本，手动：`cmake -S . -B build-selftest -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUMATEXT_SOURCE_DIR= -DPULSE_WITH_LUMATEXT=ON -DPULSE_WITH_SELFTEST=ON -DPULSE_UPDATE_MANIFEST_URL=… -DPULSE_UPDATE_PUBLIC_KEY_HEX=<cmake\update-public-key.txt> -DPULSE_VERSION_LABEL=<version.txt>-dev` |
| `d:\builder\pulse-selftest`（用户日常 exe） | VS2026 生成器 + 自测；迭代 `cmake --build <dir> --config Release --target <t>` | 曾用 `cmake -S d:\OneDrive\Project\pulse -B <dir> -G "Visual Studio 18 2026" -A x64` |

- `build_release.bat` **构建全部 target** → 某个 bench/gallery 没跟上签名改动会整条挂（排查 `cmake --build build -- -k 0`）。
- **只重建包内需要的 5 个目标**：`pulse pulse_index pulse_shell pulse_preview pulse_document`（名字见 `CMakeLists.txt` 的 `pulse_add_version_resource(...)`）。
- 配置走 `third_party\lumatext` 预构建包（本机 `..\lumatext` 源码树不存在）→ `-DLUMATEXT_SOURCE_DIR= -DPULSE_WITH_LUMATEXT=ON`。
- **新增 `.cpp` 后必须先重新生成构建系统**（`cmake -S … -B …`），否则 LNK2019（新文件没进 GLOB）。
- 半配置状态（配置失败留下 `build.ninja` 但没有可用缓存）要先 `rmdir /s /q build` 再来。
- **用户会清理 `build` / `build-selftest` / `bench_data`**：不要假设它们还在；构建前先检查。

### 自测（`--selftest`）

- `pulse.exe` 是 **WIN32 子系统**：PowerShell 里 `& pulse.exe --selftest` **不会等待** → 用 `Start-Process -PassThru -Wait`，之后再看日志（`elapsed=0s`、日志只有 3 字节 = 没等）。
- 日志固定落在**当前工作目录**的 `bench_data\selftest_1b2_last.log`，运行中被独占。
- **exe 位置决定仓库根定位**：`WorkspaceRoot()` 按 exe 目录向上找 `CMakeLists.txt` + `src\app\selftest_1b2.cpp`（最多 4 层），找不到退回 `.`。直接跑 `d:\builder\pulse-selftest\Release\pulse.exe` = 固定 4 条 `IFileOperation` ops 用例假失败；**复制到仓库内浅目录（`build-run\`，被 `.gitignore` 的 `/build*/` 覆盖）再跑 = 0 failed**。
- 单用例入口：`PULSE_SELFTEST_CASE=<case>`，已有 case 包括 `prefs-persist`(21) / `prefs-registry`(31+) / `ctxmenu-persist`(21) / `places-recovery` / `folder-views`(13) / `file-hash`(9) / `shell-namespace`(10) / `rename-editor` / `rename-outside` / `filter-controls` / `operation-toast` / `blank-pane` / `change-app` / `change-ui` / `folder-shortcut` / `tab-groups` / `tab-handoff` / `context-verbs` / `settings-flow` 等。用例 exit=0 ⇔ 该用例 0 失败。
- 链接/跑自检前先杀 `pulse` / `pulse_shell` / `Pulse.Index` / `Pulse.Preview`（否则 `LNK1104`）；用户常开着 `D:\Builder\pulse-selftest\Release\pulse.exe`。
- 全量基线（历史值，随改动增长）：1105/0 → 1208/0 → 1241/4 → 1244/0 → 1246/4 → 1250/4 → 1257/4 → **1310/0 → 1311/0**。那"固定 4 条 ops 失败"的反复出现/消失，最终判定是**沙箱残留**（`bench_data\opstest` 存在残留时失败，清掉即过），不是纯环境问题。
- **`build\pulse_ops_test.exe` 是 ops 层的验证入口**（控制台，74 项，需同目录 `pulse_shell.exe`），不归 `--selftest` 管。**`bench_data\` 父目录被清掉时它会设成约 40 项假失败**（Setup 用单级 `CreateDirectoryW` 造 `bench_data\opstest\src|dst`）→ 跑之前先建 `bench_data`。
- 其它 bench：`pulse_preview_test.exe`（54 项）、`pulse_app_controllers_test.exe`、`pulse_index_test.exe`（有 1 条既有失败 `query: incremental typing can narrow`，断的是未改动的 `index_query.cpp`）。
- **偶发假失败**：`tab-handoff`（探针窗被盖住）、`sidebar: dragging thumb to top…`（`SetKeyboardState`）→ 重跑即过，不是回归。

### 可视化验证

- `pulse.exe --test-instance --shot <png> --size WxH [路径]`；加 `--shot-scale`、`--shot-language`。
- 设置页专用：`PULSE_TEST_SETTINGS_EXPANDED`（位掩码，页 1 维护区 = 2）、`PULSE_TEST_SETTINGS_SCROLL`（dip）、`PULSE_TEST_SETTINGS_FLOW=<日志绝对路径>`（**必须同时给 `--shot`**，否则进程正常启动但不跑用例 = "exit=1 且没有日志"）。
- 虚拟路径：`pulse:settings:index|about|context|duplicates`、`pulse:recent`、`pulse:recycle`。
- 详情面板截图加 `--shot-details`（自动选中第一行）；预览 handler 的 overlay 是独立 TOPMOST 窗口，**`--shot` 抓不到**。
- 视口外的行 hit-test 必失败（先把目标滚进视口）。
- 预览诊断：`PULSE_PREVIEW_BENCH=<文件>` 跑 `pulse_preview_test.exe`，打印宿主启动 + 每请求 ms + kind/status/error。

### 打包

- `build_installer.bat` = `build_release.bat` → `check_release_payload.ps1`（查自测 dispatcher）→ 校验 6 个产物（含 `lumatext.dll`）→ `scripts\stage_installer_runtime.ps1`（把匹配的 MSVC CRT 复制到 `build\msvc-runtime\`）→ ISCC → `dist\PulseSetup-<version>.exe`。**产物已在时用 `/skipbuild`**（不带参数会全量构建，很慢）。
- 本机已装 Inno Setup 6.7.3；ISCC 的 `PrivilegesRequired=admin` + HKCU 警告属预期。日志习惯 `d:\builder\installer-last.log`。
- 二进制抽查法：`[System.IO.File]::ReadAllBytes` + `[Text.Encoding]::Unicode.GetString`（ASCII 解码搜不到宽字符串）。
- **收尾习惯（用户要求）**：一轮结束后删掉项目内生成的中间产物（截图/探针脚本/夹具/自测沙箱残留），**保留** `dist\` 与 `build\`，`.codebuddy\` 不动；一次性 bat/msg 放 `d:\builder\`，项目外不动。

### 命令与工具坑

- 工具 shell 会在 **cmd 与 PowerShell 之间跳**（`Start-Process` 在 cmd、`cd /d`/`&` 在 PS 会报错）；别套 `powershell -Command "…"`（外层吃 `$p`/`$_`）。
- **多行命令 / here-string 会被工具经 cmd 包装拆坏**（"文件名、目录名或卷标语法不正确"）⇒ 要 `.bat` 就用 `write_to_file` 写文件再执行。
- 临时 `.bat` 里 `exit /b %ERRORLEVEL%` **放在 `if (...)` 块内会在解析时展开、恒为 0**（把失败伪装成成功）⇒ 用 `if errorlevel 1 exit /b 1`，或把取值放块外。同理 bat 里块内 `set` 后立刻 `%VAR%` 用不了。
- `Add-Type` 的 P/Invoke 必须 `CharSet=Unicode`；`$pid` 是只读自动变量不能接出参。
- 合成鼠标不可靠（桌面被抢占、目标窗口被盖），`SetForegroundWindow` 常被拒绝；侧栏/详情面板拖动结束依赖 `GetKeyState(VK_LBUTTON)`，`PostMessage` 驱动不了。
- **`search_content` 偶尔出现假 0 命中**（`WM_TIMER`、`quickPreview`、`quick_preview_window.h` 都返回过 0 而实际存在）⇒ **0 结果必须用 `read_file` 或 `git grep` 复核**。
- GUI 用例在高负载下偶发单点失败；判断回归连跑两次。

---

## 四、功能域要点

### 4.1 布局：面板宽度 / 侧栏 / 详情面板

- Explorer 实测（Win11 100% / 3440×1440）：无比例上限，`panel_max = 窗口宽 − 另一侧 − 列表最小宽度`；左栏最小 64 px（塌成轨道）、右栏 307 px。实现 `src/ui/panel_metrics.h`（`kListMinWidthDip` = **112**（用户偏好，Explorer 是 144）、`kSidebarMinWidthDip` 160、`kSidebarRailWidthDip` 48、`kDetailsMinWidthDip` 300、`kPanelWidthMaxDip` 4000 仅存储校验；轨道阈值 900/1000）。
- 窗口最小宽度 `ptMinTrackSize.x` = **320**（改前 640；窄窗口截图要先看这个）。
- **坑**：`sidebar_width_` 是像素、`sidebar_width_dip_` 是 DIP，混用会被重复缩放；存储放宽到 160–4000 / 300–4000 存"用户意图"，渲染时再 clamp。
- 拖拽冒烟 `build-selftest\smoke-pulse-panes.ps1`：自绘界面无 UIA → **光标形状探测**（`WM_SETCURSOR` 设 `IDC_SIZEWE`）；坑：命中点要"移出再移回"复核；PS 单元素数组会展开（`return , @($bands)`）。
- **侧栏分区**：`app::SidebarSectionId`（`Workspaces=0, QuickAccess, SavedSearches, Drives, Tags, Networks, Cloud=6, Count=7`）—— **前 6 个 id 与 session 旧掩码位一一对应，不要改顺序**；`ui::SidebarGroup::id` 是渲染/命中唯一身份。顺序/显隐 = `AppState::sidebarOrder`（默认 `{0,1,2,6,3,4,5}`，Cloud 在 Drives 上）+ `sidebarQuickAccessHiddenMask`。**坑**：`BuildWindowViewModel` 参数序 `…, collapsed_mask, hidden_mask, starred_expanded, sidebar_order, quick_access_hidden_mask`（collapsed 在 hidden 前，极易传串）。
- 星标与 OneDrive 是**无标题分区**（没有 header 文字，仍可拖动/折叠）；OneDrive 读 `HKCU\...\OneDrive\Accounts`（`Personal` + `Business1..16`），显示名 `GetUserNameExW(NameDisplay)` + 类型名，回退 `FOLDERID_OneDrive`（链接 `secur32`）。
- 内置快速访问链接（最近/桌面/Downloads/回收站）**不在 `places.quick_access_paths`**，顺序固定不可拖动；已删掉动态插入的「项目 <仓库根名>」行（保留 `FindGitRoot`/`Tab::git_root` 给命令面板）。
- 分区图标：工作区 `\xE8B7`、快速访问 `\xE8A9`、保存的搜索 `\xE721`、标签 `\xE8EC`、此电脑 `\xE977`、网络 `\xE968`。**加新 Fluent 图标 SVG 四步**：`assets/fluent/*.svg` → `resource.h` → `pulse.rc` RCDATA → `CMakeLists.txt` 的 `OBJECT_DEPENDS` → `FluentSvgIdForGlyph()`；显示色由 SVG 第一个渐变 stop 决定。
- rail：统一 `ui::SidebarRailLayout(width_px, scale)`；rail 下有 header 的分区**永远先输出一行 `SidebarSlot::Kind::Rail`**（点击映射 `SidebarHeader` 折叠）；空分区在窄栏不绘制。悬停提示默认开、延时 150 ms（用户拍板不做设置项）。
- 详情面板预览开关：`AppState::detailsPreviewEnabled`（默认 true）+ session `detailsPreview`（缺失视为 true）；开关在文件名行右侧（星标左），图标 `\xE890`；关闭 = 预览区塌陷 + **真不生成**（`previewH` = 0、`preview_on` 挡子进程与 handler 并清 `details_viewport_`）；多选/文件夹不给按钮。
- 隐藏项：`Tab::AllowsAttributes(DWORD)`（无 H 可见；有 H 需 `show_hidden_files`；H+S 还需 `show_protected_os_files`；回收站始终可见）+ `ShowsEveryEntry()`，`EntryVisible`/计数/`SelectAll`/`CollectFilterMatches`/filter-map 快路径全用它。设置页开关 index **16**，字符串 1979/1980。

### 4.2 预览管线（`Pulse.Preview.exe` + 详情面板）

- **分派**（`src/preview_host/main.cpp`）：① `IsDirectImage` → WIC（失败回落 Shell 缩略图）② `.svg` → `RasterizeSvgFile`（Direct2D）③ `.wmf`/`.emf` → `RasterizeMetaFile`（GDI+）④ 目录/`IsKnownShellPreview` → `MakeShellThumbnail`（缓存 → `IThumbnailProvider` → `WTS_EXTRACT` → `IShellItemImageFactory`）⑤ 其余 → `MakeTextOrHex`。
- **白名单** `src/common/preview_extensions.h`：`IsTextExtension`(33) / `IsImageExtension`(10 喂 WIC) / `IsVectorExtension` / `IsMetaFileExtension` / `IsNativeExtension` = 全部之和；**后两者刻意不进 `IsImageExtension`**（WIC 无解码器 → 只得到 `image-decode-failed`），但都进 `IsNativeExtension`（`preview_handler_host.cpp` 用它决定"自己渲染 vs 调 Shell 处理器"）。
- **性能特征（改渲染前必读）**：宿主**懒启动 + 常驻复用**（`Connect()` 只在无 pipe 时 `CreateProcessW`，无 idle 超时）；请求**单 worker 串行** + 去重；**每请求只有 1 s deadline**（超时判失败并 `StopChild()` 杀宿主 → 下次重建全部设备）；上限 `kPreviewMaxPixelSize = 1024`（GIF 512）、默认 512、分桶 128；LRU 16 MB 且 ≤128 项。UI 侧全异步（`Draw()` 返回 Pending，response 到达才 `InvalidateRect`）。
- **Shell 缩略图真实成本**：能出图的便宜（PDF 首 72.8 ms / 后续 5.5 ms）；**失败的最贵**（xlsx 首 231.6 + 41.5 ms/次、CAD 小 PDF 264.6 + 53 ms/次），因为失败也走完 4 条分支 → 已提 **Issue #9**。
- **SVG**（`svg_raster.{h,cpp}`，`ID2D1DeviceContext5::CreateSvgDocument`）：设备延迟创建（硬件 → WARP，`attempted` 记住不重试）、8 MB 上限（避开 1 s 超时）、按 viewBox 等比、`D2DERR_RECREATE_TARGET` 时 `ResetStack()`；渲染失败**回落文本预览**（旧 Windows 无该接口、超限、损坏）。API 坑：`GetRoot` 返回 **void**；`D2D1_SVG_VIEWBOX` 无 inline 重载，必须走 `(name, POD_TYPE_VIEWBOX, ptr, sizeof)`。实测首次 228 ms、预热中位 **0.65 ms**。
- **WMF/EMF**（`metafile_raster.cpp`，GDI+）：PBGRA + `LockBits` 逐行拷（stride 可能为负）。**纯 GDI 会静默跳过 EMF+ 记录、渲染空白**（Office 剪贴画 = EMF+ 封在 EMF 里）——这是唯一同时覆盖 WMF/纯 EMF/EMF+ 的渲染器。包含陷阱：`gdiplus.h` 要求 `<objbase.h>` 先在内，且先 `#undef min/max`。
- `d2d1`/`d3d11`/`dxgi` 不隐式链接，`svg_raster.cpp` 用 `LoadLibraryW` 按需解析（隐式只值 ~1 ms，属架构正确性）。
- **预览 handler 宿主**（`ui/preview_handler_host.*`）+ 防卡死：pdf/doc/xls/dwg 等非 native 类型走 `IPreviewHandler`（不是缩略图子进程），由 pulse.exe 内**唯一一条 STA** 串行承载，该 STA 拥有 TOPMOST overlay。三件套：shell 查询在 `g_association_mutex` **之外**；`Sync()` 开头看门狗 `RetireStalledApartment()`；`WH_MOUSE_LL` 随 `Disable()` 卸载。
- **两档预算**：`kCommandBudgetMs = 3000` + `kSlowOpenBudgetMs = 20000`；公寓发布 `working_identity`（正在打开的请求）——面板当前请求 == 它 → 长预算，否则短（Excel 冷启动 > 6 s、预热 1115 ms，3 s 会把"慢但会成功"判死 + 记 2 min 冷却、逐次翻倍）。测试钩子 `PULSE_PREVIEW_HANDLER_TEST_DELAY_MS` / `_OPEN_BUDGET_MS` / `_SLOW_OPEN_BUDGET_MS`。
- **两个"失败会粘住"的状态（已修）**：① `FindPreviewHandlerClsid` 的 negative 缓存改 **30 s TTL**（曾永久 → "第一次失败后再也预览不了"）；② `ClearSlowProvider()`：provider 成功返回过就清冷却。边界：provider 永不返回只能等它退出——**不能 kill Office**（LocalServer 可能就是用户正在编辑的实例）。
- **overlay 跟随窗口移动（已修）**：`PlaceOverlay()` 原每步都 `SetWindowPos(HWND_TOPMOST)` → 与 provider 跨进程子窗口线程握手 → 卡死"在后追"。修法：**纯移动用 `SWP_NOZORDER`**，只有显示/尺寸变化才重设 topmost。`Sync`/`Reposition` 本身 0.0 ms（老断言发现不了），要看公寓有没有真应用：probe "overlay follows a moving owner"（修前 16/16 丢步，修后 worst 0.7–1.4 ms / 16/16）+ `overlay_window_for_test()`。
- **CAD 兄弟扩展借用已按用户要求完整回退**（`.dxf/.dws/.dwt` 本机无 shellex）。诊断手法可复用：`reg query HKCR\<ext>\shellex\{e357fccd-…}`（缩略图）/{8895b1c6-…}（预览）+ CoCreateInstance 逐步验 HRESULT。
- **`HasInk()`（>1% 非白像素）断言必不可少**（"返回 Bitmap + 尺寸对"证明不了渲染成功）；缩略图缓存的失败结果会入缓存且不重试。
- 未做：WIC 强解 `.avif`/`.psd`、`.drawio`/ASCII `.stl` 显示源码、UTF-16 无 BOM 的 `.txt` 被判 Hex（只嗅探 256 B）。

### 4.3 右键菜单（Explorer 动词合并）

- 三层：Pulse 自绘（`BuildItemMenu`）→ 注册表静态动词（`app/shell_verbs.cpp`，含合成 `openas` = "打开方式…" 与 OpenWithList MRU `__openwith`）→ `pulse_shell` 的 COM `IContextMenu`。合并/过滤/排序/封顶在 `app/context_menu_controller.cpp`，**不做任何阻塞 Shell 工作**。
- 默认值对齐 Explorer：software / open_with / print / share / wallpaper / rotate / shortcut / system_extra 全 **true**，`open_with_com` **false**；`kDefaultExplorerCap = 48`。rank：软件子菜单 0 → 软件动词 1 → 打开方式 2 → 打印 3 → 发送到/系统项 4（**不要**按"是否子菜单"排）。
- **坑 1（issue #4 根因，已修）**：`C:\ProgramData\Pulse\shell-verbs.bin` 若在 `l10n::Initialize` **之前**播种，合成行中文文本是空串 → 被 `AppendShellSection` 丢弃、整条"打开方式"消失。排查 `PULSE_SELFTEST_CASE=context-verbs`（dump 缓存 / 实时枚举 / UI 过滤三层）。
- **坑 2**：`shell_verbs.cpp` 显式跳过 `printto`（只留 `print`）；`IsBuiltinContextVerb` 会丢掉与内置重复的 verb。目录键归一化 = `ipc::CatalogKey`（小写 + 去空格 + 去单字符加速键后缀）：`新建(N)`/`新建(W)`、COM 与 MRU 重复项合并；设置页条目 = "右键见过的项"目录（`seen` ≤400），文本取自系统实际菜单、不可重命名。
- Shell 对话框 owner：`OpsManager::SetUiWindow(hwnd)`（`app_main.cpp` 在 `ops.Start` 前设），`SHOpenWithDialog`/`SHObjectProperties`/`ShellExecuteEx` 统一用。"打开方式…"/"属性"走 open 队列**头部**（`EnqueueOpen(item, front=true)`），其余 FIFO。**已定案**：不自绘应用列表、不再优化系统对话框耗时。
- 打印与 Explorer 一致（只调 `print`，`printto` 不显示）—— **"打印…(printto)" 项用户口径是"以后可能做、默认关"**。

### 4.4 宿主编辑框与重命名

- **点击同窗口内部不改变键盘焦点**，而四类宿主编辑框（重命名/标签重命名/筛选/地址栏）只靠 `WM_KILLFOCUS` 收尾 → 框一直挂着。修法：`app_input.cpp` 的 `HandleLButtonDown` 开头对活动宿主编辑框 `SetFocus(hwnd)`；语义 = 点别处提交、Esc 取消。
- **销毁带焦点的宿主编辑框前必须武装防重入开关**（否则销毁同步派发 `WM_KILLFOCUS` → 重入 `HideRenameOverlay(true)` → `renameIndex = -1` → 越界）；判活用 `IsWindow()` 并总是置空句柄。
- **没有 LumaText 时不能隐藏原生光标**：`HideCaret` + 闪烁定时器只在 `LumaTextEnabled()` 时执行（探针 `build-selftest\check-caret.ps1`）。
- **重命名走进程内快路径**（`RenameInProcess()` / `MoveFileW`），**无 shell 回退**；冲突/非法名/占用直接报错（作者定的约束：只接受单一文件名、拒绝路径分隔符/保留名/非法名、仅大小写变化必须真落盘、批量项间响应取消并保留已完成项 undo、冲突不静默覆盖）。工具 `TestRenameCommitLatency()`、`build-selftest\drive-inplace-rename.ps1`（实测回车 22.5 ms / 失焦 17.1 ms）。
- **筛选框输入文字不可见**（焦点下划线在、字符不显示）：根因 = `app_hosted_edit.cpp` 的 `FilterEditProc` 仍回落 `DefSubclassProc`（`RenameEditProc` 已改走 `DefPresentedHostedEditProc`）→ **已定：提 issue、不动代码**，草案 `.git\ISSUE_FILTER_EDITOR_TEXT.md`。
- 输入法相关：`hide_caret` / `armed-first` / `drop` / `keep` 诊断、`PULSE_EDIT_TRACE=<file>`（宿主编辑框收到的每条消息）；`rename-editor` 5 条断言。用户用微信输入法（TSF）打中文，验收装 `dist` 的安装包。

### 4.5 多窗口 / 标签分组

- 多实例：`--new-window [path]` 跳过 `Local\Pulse.Singleton`；次级实例不写 session、无托盘、不注册热键、关窗即退、按已有窗口数 ×32 px 级联；入口 = 标签右键「在新窗口中打开」+ 托盘「新建窗口」（`src/app/instance_launcher.{h,cpp}`）。`--new-window <路径>` = 唯一标签即该路径；无路径 = `pulse:recent`。**坑**：位置参数解析曾把尾随开关当路径。任务栏 jump list 已做（AUMID `Pulse.FileManager`）。
- `session.json`/`app.json` 全局单份、多进程写会互覆盖（app.json 已做合并保护，session 见 §4.6）。
- **跨窗口拖标签**：光标出窗口 → `AppState::tabDragExternal`（浮动块停边缘、跳过重排、`IDC_SIZEALL`）；`src/ui/drag_ghost.{h,cpp}` 的 `TabDragGhost` = layered 顶层卡片（按抓取锚点贴光标；`UpdateLayeredWindow` + `CreateDIBSection` 预乘 alpha；**pulse.exe 必须 `GdiplusStartup`**，否则绘制静默失败）。松手 `HandOffTabUnderCursor()`：有目标 → `SendTabTransfer`（**恒 `NewTab`**，虚拟路径照传）；口外无目标 → `LaunchNewWindow` 拆新窗口；源只剩 1 标签 → `mergedAway` + 关窗 + 目标 `AdoptSingletonOwnership()` 接管托盘/热键/session。**坑**：① `SendMessageTimeoutW` 超时返回 0 但消息已送达 → `ERROR_TIMEOUT` 视为已送达；② 卡片贴光标挡落点 → 松手先 `Hide()` + `PulseWindowUnderPoint` 跳过 `PulseTabDragGhost`；③ 目标窗口弹出子窗口（预览 overlay）无 `PulseMainWindow` 类名 → 沿 **owner 链**找；④ 拖动起点门槛两处（外层 + 内层 `n0>=2`）都得放开；⑤ 拖动开始清 `hoverPath/hoverLabel/tooltipText`。
- **关最后一个标签 = 关窗口**：`WindowTabs::ClosingLastTab(idx)` → `CloseLayoutTab` 头 `PostMessageW(s.hwnd, WM_CLOSE)`；标签右键走 `TabController::Callbacks::close_window`；组菜单「关闭分组标签页」同路径。主实例可能只隐藏到托盘，次级直接退。空组清理 = `app::PruneEmptyGroups(WindowTabs&)`（4 处共用）。
- 分组模型：`LayoutTab::tab_group`（0 = 未分组）+ `TabGroup{id,name,color_rgb,collapsed}`；模型层 `app_model.h` + `tab_group_model.cpp`（`MoveTabRun`/`CollapsedChipBlockW`/`ChipBlockCrossed`/`DisplacedRestDelta`/`FindGroupRun`/`MoveTabGroupAcrossFreeTab`/`NormalizeGroupRuns`/`RunCrossCenter`）。增删组/入组/重命名/颜色/折叠在 `TabController`（`ShowGroupMenu` 8 色板；命令 id `CmdTab*` 含 `CmdTabJoinGroupBase=130` + 组下标）。pinned 永不进组。
- 几何：标签带 = `MainRenderer::TabStripMetrics` / `TabItemRect` / `TabGroupChipRect` / `TabPitchPx`，单位 **px**。**坑**：chip 150 ms 滑动动画期命中与画面不一致 → 已加 `TabGroupChipRectForHit`（静止位 + 动画偏移，绘制/命中共用），**别直接给 `TabGroupChipRect` 加 offset**（拖放代码把它当 rest 值再叠加 `chipOffsets`）。
- chip 交互：单击 chip = 折叠/展开，拖动 = 整组移动（状态机 `app_input.cpp`；曾被一行调试残留 `return DefWindowProcW` 埋成死代码）。拖标签入组 = **松手时一次提交**（只看最终左右邻居）；组是连续段，跨过整组时整段 `MoveTabRun` 跳一格；同一次 mousemove 只处理紧邻一格。
- **悬停卡片**：chip 悬停 150 ms 弹自绘卡片；行数据 = 纯函数 `TabGroupCardRows()`；矩形 `TabGroupCardRect` 出参 `visible_rows`（放不下**先砍成员行，尾部两行动作行恒在**；行高 32 DIP）。折叠组 = 成员行（**点行只 `SwitchTab`，组保持折叠** —— 用户明确要求）+ 分隔 + 「在组中新建标签页」`\xE710` / 「编辑组」`\xE70F`；展开组只有两行。对勾 `\xE73E` 两种（激活实心 / `lastActiveInGroup` 淡对勾，不落 session）。折叠组含激活标签时 chip 走"抬起"外观（顶部 2 DIP **组色**强调线）；**"右缘小圆点"被用户否掉**。`IDS_TAB_GROUP_EDIT = 1995`。
- 已知边界（用户已接受）：chip 斜向移出到卡片无"安全三角"；`CmdTabNewRight` 曾把组切两段（**2026-09-22 已改为不再切组**）；chip 命中/点击无自动化覆盖（合成鼠标不可靠）。

### 4.6 设置持久化（含文件夹接管、命名空间）

- **`app.json` 四条保证**（`AppPrefs` = `AppPrefsValues` 数据基类 + 私有 `disk_state_` + `loaded_from_file()`）：① `Save()` 前**读盘逐字段合并**（本进程改过的保留本地、没改的取盘上值；**保存成功后基准取内存**）→ 跨窗口/跨进程不再互相回滚；② **读不到就不覆盖**：文件存在却不可读 → 改名 `.bad`（失败则 `Save()` 返 false）；③ 写前留 `.bak`，主文件坏/截断（29 键出现不到一半）从备份自愈；④ 原子写临时名 = `path + L".tmp." + <pid>`（`src/common/utf8_file.h` 的 `QuarantineUnreadableFile()` / `KeepPreviousFileCopy()`）。`context_menu.json` 同套模板 + 截断闸门（`kStoredKeys` 6 键 / `kMinStoredKeys=3`）。
- **注册表对账 `ReconcileRegistryWithFile()`**：文件 = 意图真相源，注册表 = 可修复投影 —— 说开而缺/指向旧 `pulse.exe` → 写回自愈；指向别家 → 不抢；说关 → 清残留；**说关但注册表明确是我们的且在开 → 采用为开**。`!loaded_from_file_` 才走首次迁移。归属三档：宽松 `CommandIsPulse()`（文件名）/ 严格 `FolderOpenCommandIsOurs()`（全路径 = "已配置"）/ `CommandIsShellDefault()`（首 token 是 explorer.exe = 系统默认镜像，**可替换、不当残留清**）。唯一调用点 `settings_controller.cpp`。
- **文件夹关联接管范围（用户拍板）**：接管 `Directory`/`Drive`（`shell\open` + 类默认动词 `none`→`open`）与 `Folder`（`shell\open` + `shell\explore`；Folder 的动词在 HKCR 由委派处理器 `{11dbb47c-…}` 承载，必须在 verb 的 `\command` 下写**空 `DelegateExecute`** 遮蔽）。**Win+E 的 CLSID `{52205fd8-…}\shell\opennewwindow` 刻意不接管**（写坏 = Win+E 报错；风险最高）。"只剩 shell 默认动词"的半截残留只在 `\shell\open\command` 为空时成立（不能误删别家的默认动词）。卸载器 `DeleteFolderOpenVerb(ClassName, Verb)` 覆盖 Folder 的 open+explore（否则"卸载后文件夹打不开"）。Pascal 用 `=` 不是 `==`。
- **接不到的（硬边界）**：第三方"在文件夹中显示"的选中那一步（`explorer.exe /select,` 由 Explorer 自己处理，探针实测选中正确、不经类动词；`SHOpenFolderAndSelectItems` 同样只认 Explorer）；**走类动词的"打开文件夹"会被我们截走 → Pulse 开文件夹但没有选中**（文件名根本不在消息里）。同样接不到的还有 Win+E 的 CLSID、特殊命名空间、系统打开/保存对话框。用户口径：接管"一般可用"（双击文件夹/盘符进 Pulse ✓）；FDM 的「在文件夹中显示」仍弹 Explorer（它硬编码 `explorer.exe /select,`）—— 与 Win+E 同类，Files 也一样。
- **命名空间输入（已修，用户拍板保留接管、只修标签）**：任务栏"文件资源管理器"按钮（主页 `{F874310E-…}`）、桌面"此电脑/回收站"会把 `::{GUID}` 当 `%1` 交过来。唯一入口 = `ResolveIncomingPath()`（覆盖命令行 / `'PULS'` 转发 / `'PTBT'` 交接 / `--shot`）：主页 + 快速访问 → `pulse:recent`，回收站 → `pulse:recycle`，其余命名空间**不建标签**（窗口仍到前台）。判据 `fs::IsShellNamespacePath()`；`fs::NormalizePath()` 不再改写命名空间；`PlacesCatalog::RecordRecent/Load` 过滤并剔除脏数据（实机"最近使用" 98→96 自愈）。文档 `docs/shell-namespace-forward.md`，用例 `shell-namespace`(10)。
- **各 json 的收尾进度**（多窗口整份覆盖问题，逐文件）：
  - `tags.json`：文件在但读不出 → 先 `QuarantineUnreadableFile()`（存 `.bad`）再让默认色表写新文件；占用导致隔离失败就**不写**；写前 `.bak`。
  - `places.json`：`recent_items` / `starred_items` / `quick_access_paths` 三项**都已跨窗口合并**（各自有 `*_paths_at_load_` + `Merge*FromDisk()`；规则 = "盘上有、本地没有、且加载时也不在"才并入，加载时在却本地没了 = 用户删的，不复活；recent 按 `opened_at` 降序截 100，starred 按磁盘顺序追加后 `stable_partition` 成 folders-first）。⚠️ **`quick_access_paths` 只有实现、没加专门断言**。**`workspaces` 的合并仍欠**（要写 workspace 读取解析：name/root/layout/panes/views/`freq`，并处理"同名 root 本地优先"）。
  - `saved_searches.json`：`DefaultPath()` 改走 `GetPulseDataDir()`（原来自己拼 `%LOCALAPPDATA%\Pulse`，绕过重定向）；`saved_search_test.cpp` 需补 `GetPulseDataDir()` 桩（否则 bench 链接失败）。
  - `session.json`：恢复性已补（写前 `.bak`；读不出/空文件先隔离 `.bad` 再回默认）；**自动保存已做** —— 退出路径组装 `SessionSnapshot` 的 ~30 行抽成 `static void SaveWindowSession(AppState&, HWND)`，主窗口搭现有 UI 定时器（`WM_TIMER` + `kTimerUi`）按 `kSessionAutosaveMs = 30000` 节流，只在前台主实例保存。⚠️ **`src/app/*.cpp` 里 grep `SetTimer|WM_TIMER` 会误导性返回 0**（实际在 `app_main.cpp`）——加定时器前先搜 `kTimerUi`；**新增 `case WM_TIMER` 会编译报"case 值 275 已使用"**。
  - `search_history.json`：恢复性已补；**刻意不做合并**（价值低、多窗口交错记录可接受）。
  - `index-config.json`：`LoadMachineConfig`/`SaveMachineConfig` + 读盘合并（机器级、要管理员）。
- **加设置开关的完整链路（照抄这份）**：`app_prefs.{h,cpp}`（字段 + keys + 默认值 + ToJson + FromJson + 合并基线）→ `strings.rcinc` 中英 → `string_ids.h` → `localization.h`/`localization.cpp` 的 `kLastString` → `ui_settings_core.cpp` 的 `toggle(...)` → `settings_layout_sections.h` 的 `row()` → `ui_renderer_internal.h` 的矩形 → `ui_hit_test.cpp` 命中（`SettingsToggle` + index）→ `ui_settings_view.cpp` 滚动定位 → `ui_renderer.h` 的 `settings_*` 字段 → `app_runtime.cpp` 填 vm → `settings_controller.cpp` 的 `ToggleSetting(index)` → `settings_ui.cpp` 的 `{I::X,0,1}` 分组行。
- 设置页此前已做的：`file_hash_enabled`（index 18，"右键菜单显示文件哈希"）、`show_tooltips` + `tooltip_delay_ms`（4 档：150/400/800 + **自定义**，第 4 档弹出宿主编辑框 `app_hosted_edit.{h,cpp}` 的 `TooltipDelayEditProc`，回车提交/Esc 取消/失焦提交、`clamp(50,5000)` 越界 `MessageBeep`；cell 来自 `MainRenderer::TooltipDelayCustomCell(...)`）；`thumb_cache_row` +「清理」。
- 测试：`prefs-persist`(21) / `prefs-registry`(31→53) / `ctxmenu-persist`(21) / `places-recovery`(5→17)；注册表沙箱 `PULSE_TEST_REGISTRY_BASE`（仅 selftest）；`allow_registry = 有沙箱 || 数据目录未重定向`；`GetPulseDataDir()` 识别 `PULSE_TEST_DATA_DIR`。断言坑：**不能用 `size()/3`**（6 键的 1/3 恰好命中 3 键）。

### 4.7 安装器与升级

- **数据边界**：`%LOCALAPPDATA%\Pulse` = 用户配置（`app.json`/`session.json`/`places.json`/`context_menu.json`/`search_history.json`/`tags.json`/`folder-views.json`/`ThumbCache\`/logs/…）—— **安装器从不写**，升级保留；`%ProgramData%\Pulse` = 机器级（`index-config.json`/`Index\`/`shell-verbs.bin`），管理员写（SDDL：SYSTEM/Administrators 全权、Authenticated Users 只读 → 改索引配置必须提权）。
- **升级流程** = 先静默卸载旧版再原位覆盖：`PrepareToInstall` → `StopPulseApps`（**强杀** `pulse.exe`，刻意不用 `/T`）、`net stop PulseIndex` → `UninstallPreviousVersion`（从注册表定位旧卸载器跑 `/VERYSILENT`，找不到或非 0 → 中止）→ 复制（`UsePreviousAppDir/Tasks/Group=yes`，`[Files]` 全 `ignoreversion`）→ `ssPostInstall`：`--set-index-path` → `--install` → `pulse.exe --seed-shell-verbs`。
- **升级时的删除行为**：旧卸载器 `CleanupUserData := not UninstallSilent` → 静默升级不删用户数据 ✓。但 `[Registry]` 的 `uninsdeletevalue` 是 Inno 记录机制，**任何**卸载都删 `HKCU\...\Run\Pulse`；本树曾额外**无条件**删它 + `Directory|Drive\shell\open`（= 每次升级静默关掉"开机自启"和"用 Pulse 打开文件夹"）→ **PR #8** 改成 `if not UninstallSilent then`（从旧版升级的第一次仍会丢一次）。持久化自愈已能补回。
- 1.0.32 修的是"自动更新后主窗口继承管理员权限 → 无法从桌面拖入文件"。`taskkill /F` 不触发 `WM_DESTROY` → session 不写（属"丢改动"，不是"回默认"）。
- `[Run] --seed-shell-verbs` 每次安装都以管理员身份重写 `shell-verbs.bin`（**有意覆盖**）。

### 4.8 主题跟随系统（已修）

- 症状：设置里选「跟随系统」后，运行中改系统主题 Pulse 不跟。**根因**：`ui::ShouldUseDarkMode()` 先问 uxtheme 序号 132 `ShouldAppsUseDarkMode`，它是**进程级缓存**（翻转注册表后仍返回旧值；序号 106 `RefreshImmersiveColorPolicyState` 也刷不动）。
- 修法：`src/ui/FluentTokens.h` 的 `ShouldUseDarkMode` 改为**注册表优先**读 `HKCU\...\Themes\Personalize\AppsUseLightTheme`（0 = dark），uxtheme 只在值缺失时兜底。消息路径本来就对（`WM_SETTINGCHANGE` → 重算 `darkMode` + `ApplyAppWindowChrome` + 重绘）。
- 验证：探针 `d:\builder\ux-darkmode-probe.ps1` + 端到端 `d:\builder\verify-theme-follow.ps1`（读 `DWMWA_USE_IMMERSIVE_DARK_MODE`，翻注册表 + 只发 `WM_SETTINGCHANGE` → 1→0→1 ✓，注册表已复原）。用户手验口径：选「跟随系统」，改**应用模式**（不是 Windows 模式）。
- **快速预览窗跟随主题已接线**（`quick_preview_window.SetAppearance` 在 `ApplyAppWindowChrome` 调用）。**不加"翻转真注册表"的自测**（崩溃会把用户主题留在翻转态）。

### 4.9 自动更新（1.0.34 起）

- 更新进度条是 **1.0.34**（提交 `7fdc447`）引入的：`src/app/update_progress.h`（阶段/字节/百分比）、`update_status.h`（把进度映射成状态栏文字与百分比）、`app_updates.cpp` 10 Hz 重绘节流（仅下载/安装活跃、窗口可见且非最小化时）；绘制在窗口**底部状态栏右侧**（accent 文字 + 100 dip × 4 dip 圆角轨道），不是设置页卡片里。下载中显示「正在下载 xx% (x.x / x.x MB)」；总大小未知只显示「已下载 x.x MB」（`percent()` 返 -1 → 不画条）；安装阶段状态栏只显示「安装程序运行中…」（真正进度在 Inno 窗口）。
- 自动检查更新开关关闭时**只停后台检查与提醒**，不取消用户手动发起的下载/安装；更新卡片按钮可用性只看构建是否带更新服务（`UpdateChecker::Enabled()`），不看该开关。
- 更新清单用自签密钥签名，客户端内置公钥验签；本地签名工具 `scripts/sign_update_manifest_local.ps1`（因本机无 pwsh）。

### 4.10 其它已交付小功能

- **文件哈希 + 解除网络锁定**：`src/app/file_hash.{h,cpp}`（BCrypt 流式 SHA-256/MD5；`Zone.Identifier` 解除锁定）；菜单 `CmdHashSha256`/`CmdHashMd5`/`CmdUnblockFile`，结果经 `WM_FILE_HASH_DONE = WM_APP+65` 回 UI 线程；`BuildItemMenu(..., bool show_hash = true)` 只在单选非文件夹时给；设置项 `file_hash_enabled`（默认 true）。
- **缩略图磁盘缓存**：`%LOCALAPPDATA%\Pulse\ThumbCache\`（默认 256 MB LRU + 索引文件；`SetDiskBudgetForTest`）；设置页「清理」按钮。**已修的真 bug**：`TrimLocked` 删除循环误用常量 `kDiskBudgetBytes` 而不是本次读出的 `budget`（只有 `pulse_preview_test` 能暴露）。
- **每文件夹视图记忆**：`src/app/folder_views.{h,cpp}`（`FolderView{view,sort,direction,dividers,used}`；上限 500、按 `used` 递减修剪；写盘前读盘三方合并 + `.bak` + 原子写；`isolatedTest`/截图不读不写）。**两个坑**：① 键 = `fs::NormalizePath(path)`（带 `\\?\`）⇒ `Find()` 内部也要 normalize；② `used` 是**裸数字**，`ExtractString` 读不到（静默 0 → 修剪顺序乱）⇒ 用 `ValuePosition` + `_wcstoui64`。接线：`StartLoadingPath`（`ApplyFolderView` 在虚拟路径判定之后、shared snapshot / worker 排序判定之前）、`SetViewMode`、`SetSort`、列宽拖动释放。用例 `folder-views`(13)。
- **Ctrl+滚轮不再跳变（已修）**：原来 Ctrl+滚轮会在全部 8 档视图里步进并归零滚动 → 多选后滚动会"跳跃"。修法对齐 Explorer：Ctrl+滚轮**只在四个图标视图内**步进图标尺寸，Details/List/Content 下忽略。**坑**：`WM_MOUSEWHEEL` 的 delta 在 **wParam 高字**、按键状态（`MK_CONTROL`）在**低字**。
- **缩略图/更新等设置项**：`IDS_SETTINGS_THUMB_CACHE = 2014`、`IDS_SETTINGS_FILE_HASH 2015/2016`、`IDS_TOOLTIP_DELAY_CUSTOM 2017`（`kLastString` 一路跟到最高号）。
- **已删/已否决**：图片全屏查看器（F11）曾实现并验证，**用户验收后要求移除**（"直接全屏不好用、意义不大"）—— 边界：以后若做看图器必须**窗口化**（可调整大小、窗内再切全屏），且能边看图边操作文件。**文件夹大小（走索引）决定不做**（收益窄、成本全在公共底层；若以后做先做最薄的按需版本）。
- `` `--select`（打开并选中）整批撤销``（用户拍板，别再加）：曾实现 `pulse.exe --select` + `'PSEL'` 转发 + `RevealInFolder`/`SelectEntryByName`，已 `git checkout --` 10 个文件 + 手删 `selftest_1b2.cpp` 三处；**Pulse 内部自己的"打开文件所在位置"保留**（`CmdOpenPath` = 父目录 + 待选中 + `NavigateTo`；曾修过"把 `pending_selected_name` 设在 `NavigateTo` 之前导致从未选中"的真 bug）。

### 4.11 文件操作（ops）速度显示（2026-09-23，本批未提交）

- 背景：删除类操作（回收站删除/永久删除/还原/清空回收站）在"传输速度变化历史"里是平的零线 + 「当前速度: 无需估算」+「峰值: 0 B/s」。
- **根因（两层）**：① 数据层 —— 速度估计器 `TransferRateEstimator` 只在复制/移动的传输回调里被喂数据；shell 项目操作只报项目数、清空回收站虽轮询到字节却不更新速率字段；② UI 层 —— 采样与文案显式白名单 `type == Copy || Move`，其它一律推 0。
- **实现**：`OpSpeedBasis { None, Bytes, Items }` + `SpeedBasisFor(OpType)`（Copy/Move/EmptyRecycle → Bytes；RecycleDelete/RealDelete/RestoreRecycle → Items；其余 None）写进 `OpStatus`（`items_per_second`/`peak_items_per_second`/`speed_basis`）；清空回收站的轮询线程接估计器（它本来就在减回收站字节）；删除/还原在 shell 进度回调里按 `task_id` 重置并累计项目速率。
- UI（`file_operation_dialog.cpp`）：采样按 `speed_basis` 取对应速率；当前速度/峰值按单位格式化（`FormatSpeedText`：项目速率 ≥10 整数、≥1 一位小数、<1 两位小数、去尾随零 —— 否则删大目录时 0.x 项/秒会被取整成 `0 项/秒`）；「剩余时间」删除类改走 ETA；「剩余项目」清空回收站带剩余字节。新字符串 `IDS_OP_ITEMS_PER_SECOND`(**2026**)，`kLastString` 同步。
- `TransferRateEstimator` 新增 `SetEtaFloor()`：删除/还原类把"太慢不估算"的 1.0/秒 阈值降到 0（字节类保持 1 B/s），否则剩余时间永远停在「正在估算」。
- 测试：`pulse_ops_test` 新增 5 条（`SpeedBasisFor` 映射 / 删除任务声明 Items 基准 / 峰值单调 / 跑过预热期必须出现实时速度 / 必须出现剩余时间估算），实测 `[INFO] bulk realdelete: 3410 ms, live_speed=1`（400 文件删除）；`settings-flow` 的标签完整性检查扩到 2026。
- 验证状态：release 树编译过一次（含小数化 + ETA 阈值），之后又补了两个 include（`<cstdio>`/`<cwchar>`）**尚未编译**；`pulse_ops_test` 与目视均**待跑**。

---

## 七、版本与 PR 时间线

| 时间 | 事件 |
|---|---|
| ~09-11 | 上游 1.0.11 基础上修"就地重命名卡顿/编辑框不关闭"，提 **PR #1**；作者在自己分支整合（`0512d8e`/`f101997`，1.0.12–1.0.14），PR 由我们致谢关闭。作者指出 4 个正确性问题 → 改名约束定案（**以他的实现为准**） |
| 09-15 | PR #1 被上游合并（`0512d8e`，v1.0.13）；上游推进到 **1.0.30**（pdfium/sqlite 等第三方，体积大增） |
| 09-18/19 | 面板宽度（PR #5）、隐藏+受保护文件双开关（PR #6）并入上游 **1.0.33**；提交 **PR #7**（侧栏分区 + 右键菜单动词合并 + 预览开关，`Fixes #4`）、**PR #8**（安装器升级偏好）、**PR #10**（SVG + WMF/EMF 原生渲染）、**Issue #9**（shell 缩略图失败成本） |
| 09-20 | 多窗口 + 跨窗口拖标签（`c6baa62`）、分组 chip 整包（`d74e0f2`）、关最后标签 = 关窗（`c8d7b43` → amend `ed6a73a`）、设置持久化整批（app.json 四条保证 + 对账 + 注册表沙箱 + 文件夹接管）；分支 `feature/multi-window-tabs` 推 fork |
| 09-21 | 上游合了 #7/#8/#10（main = `187240e`）→ rebase 重放为 `c30c354`/`9d88449`/`9811b9f`/`f0d5967` + 文档 `c03c78f`；上游发 **1.0.34**（`7fdc447`，新增共享字符串 `IDS_UPDATE_* 1992–1997`）→ 第二次 rebase，本项目字符串整体挪到 **1998–2001**；fork 的 `main`/`dev` 对齐上游、清理已合并分支；"打开文件所在位置"（`630f94a`）+ 主题跟随修复（`6317528`）提交并推 PR 分支；`dist\PulseSetup-1.0.34.exe` 多版覆盖出包（8,128,689 → 8,134,000 → 8,133,053 → 8,133,676 B） |
| 09-22 | 用户问"值得添加什么功能" → 拍板做 9 项（后为 8 项）；持久化收尾 3 批 + 哈希/解除锁定 + 缩略图缓存 + index-config 合并 + 每文件夹视图 + 全屏看图（后删）+ 主题连带窗口 + chip 动画命中 + `CmdTabNewRight` 不切组 + 隐藏项淡化 + 视图菜单开关 + 悬停提示设置项 + 提示延迟自定义 + Ctrl+滚轮修复；全量自测 1310/0；`dist\PulseSetup-1.0.34.exe` 再次覆盖（8,160,037 → 8,156,138 → 8,161,473 B） |
| 09-23 | 发布 **1.0.36**（`c57e7f7`，设置页收尾：两个"移除"按钮各自命名、去重索引状态、列表补卡片、页 4 双份真源、页 2 显式布尔；修复 `kLastString` 停在 2019 导致 2020–2025 字符串画成空白、添加按钮宽度复用）；`edit_host.cpp` 修"IME 整段重写后光标回到行首"（原生 EDIT 兜底路径缺保护）→ CI 全绿、Release 发布 4 个资产（含 CI 签名清单，min build 10240/9600）；文件操作速度显示（本批未提交、未验证） |

---

## 八、当前状态与待办

**工作区（未提交，2026-09-23）**：文件操作速度显示整批（`ops_manager.{h,cpp}`、`file_operation_dialog.cpp`、`string_ids.h`、`localization.{h,cpp}`、`strings.rcinc`、`settings_flow_test.cpp`、`ops_test_main.cpp`）—— 编译过一批，最后两个 include 未编译；`pulse_ops_test` 与目视验证待跑。

**待上游 / 待用户**
- PR（分支 `feature/multi-window-tabs`）由用户维护正文；本机无 gh/token，需要改正文时给可粘贴文本。
- 筛选框输入文字不可见 → 提 issue、不动代码（草案在 `.git\ISSUE_FILTER_EDITOR_TEXT.md`）。
- `docs/build.md`、`README.md` 链接、`.gitignore` 保持本地专属。

**接手时该做的第一件事**
1. `git status` 看工作区；跑一次全量 `--selftest`（确认基线）与 `pulse_ops_test`（记得先建 `bench_data`）。
2. 发现"改动没生效"先比对进程/产物时间戳。
3. 用户说"提交"才提交；说"构建/出包"才构建。
