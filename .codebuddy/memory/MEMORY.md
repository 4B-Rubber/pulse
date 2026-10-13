# Pulse 项目长期记忆（规则与约定）

> **规则手册**：只放跨会话必须遵守的事实、约定、坑与硬边界。
> 功能怎么实现的、哪一天做了什么、现在什么状态 → 见同目录 `SUMMARY.md`（日期日志已于 2026-09-23 删除）。
> 2026-09-23 精简重写：原 154 行 / 69 KB 压缩为规则形态，按主题重组，删掉历史叙述与实现细节。

---

## 一、项目与仓库（git）

- **Pulse**：Windows 文件管理器（开源），C++20 + Win32 + Direct2D + DirectComposition。上游 <https://github.com/jimmgreen/pulse>。
- **远端**：`origin` = fork `4B-Rubber/pulse`（SSH 别名 `github-4brubber`，密钥 `~/.ssh/id_ed25519_4brubber`；HTTPS 凭据属于另一个账号，推 fork 会 403）；`upstream` 只读，走 HTTPS 依赖本机代理 `127.0.0.1:7890`，代理没开就改用 SSH origin。
- **密钥纪律**：私钥只留 `~/.ssh`，绝不进 OneDrive/仓库；`ssh-keygen` 非交互必须 `-N ""`；用户已拍板不加口令。
- **提交流程**：从 `dev` 建 `feature/<主题>` → `git rebase --onto origin/main <旧基点>`（PR 只含本批改动）；一个 PR 多 commit 是默认；提交消息英文、说"为什么"，前缀 `ui:` / `fix(scope):` / `test:` / `release:`。
- **提交纪律（用户明确纠正，务必遵守）**：改动写完、自测跑过**都不构成提交理由** —— **只有用户明确说"提交"才 `git commit`**；在那之前只留工作区，并告诉用户改动在哪、怎么试（用户没实测 = 没测）。
- **版本号只有一个来源：根 `version.txt`**（纯数字 `x.y.z`；`-dev` 后缀只出现在安装包名与界面 `PULSE_VERSION_LABEL`）。**本地打包从不自增**；只有**发布**才改它 + 写 `docs/releases/X.md` + 打 `vX` 标签（`publish_release.ps1` 在版本已发布时会报错要求自增）。
- **无 `gh` CLI、无 token**（6 个安装路径 + 环境变量 + 凭据管理器都试过）：PR 只能由用户在网页创建/编辑，需要改正文时给他可粘贴的文本。查上游 PR 免代理：`git ls-remote git@github-4brubber:jimmgreen/pulse.git "refs/pull/*/head"`。
- **本地专属文件（不提交不上推）**：`docs/build.md`、`README.md` 的链接、`.gitignore`（= 上游原版 + 空行 + `.codebuddy/`）；`git add -A` 前先 `git status`。**`.codebuddy/` 目录不要删。**
- **分支现状**：`main` = 上游；`dev` = 用户自己的集成分支；`feature/multi-window-tabs` = PR 分支（基座 = 上游 main）。
- **发布链路（CI）**：打 `vX.Y.Z` 标签 → GitHub Actions 跑 windows + win81 两个 matrix job → publish job 用私钥签名两份清单并发布 Release（两个安装包 + 两份 `update-manifest*.json`）。手动触发要选 **tag** 作 ref（选分支会卡在 `Validate release tag`）。CI 清单的 `minimum_windows_build` = **10240（普通）/ 9600（win81）**，与本机 `create_update_manifest.ps1` 默认的 19045 不同。
- **git 坑**：① `reset --soft <中间提交>` 会连带把它之后的提交塞进索引；② 跨提交移动**同一文件**用 `cherry-pick` 必冲突 → 用 `git checkout <sha> -- <files>` + 手工补/删 hunk；③ `rebase --continue` 会调 Sublime（本机无）→ 设 `GIT_EDITOR=:`；④ rebase 前把 `.gitignore` 的本地改动 stash、结束后 pop。

---

## 二、构建 / 测试 / 发布环境

- **两棵构建树**：仓内 `build\`（Release、`PULSE_WITH_SELFTEST=OFF`，打包依赖它）用 `build_release.bat` 建立；仓内 `build-selftest\`（自测 ON）无脚本，手动配：`cmake -S . -B build-selftest -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUMATEXT_SOURCE_DIR= -DPULSE_WITH_LUMATEXT=ON -DPULSE_WITH_SELFTEST=ON -DPULSE_UPDATE_MANIFEST_URL="https://github.com/4B-Rubber/pulse/releases/latest/download/update-manifest.json" -DPULSE_UPDATE_PUBLIC_KEY_HEX=<cmake\update-public-key.txt 内容> -DPULSE_VERSION_LABEL=<version.txt>-dev`，再 `cmake --build build-selftest`。另 `d:\builder\pulse-selftest`（VS2026 生成器）是用户日常 exe。
- **只重建包内需要的 5 个目标**：`pulse pulse_index pulse_shell pulse_preview pulse_document`。`build_release.bat` 会构建**全部** target（某个 bench/gallery 没跟上签名改动会整条挂，排查 `cmake --build build -- -k 0`）。
- **新增 `.cpp` 后必须先重新生成构建系统**，否则 LNK2019；配置失败会留下 `build.ninja` 没有可用缓存 → 先 `rmdir /s /q build` 再来。
- **用户会清理 `build` / `build-selftest` / `bench_data`**（甚至删除进回收站）：不要假设它们还在，构建前先检查。
- **自测**：`pulse.exe` 是 WIN32 子系统 → PowerShell 里 `&` 不会等待，用 `Start-Process -PassThru -Wait`；日志固定在**当前工作目录**的 `bench_data\selftest_1b2_last.log`（运行中独占）。**exe 必须放仓库内浅目录**（`build-run\`，被 `.gitignore` 的 `/build*/` 覆盖），否则固定 4 条 `IFileOperation` 用例假失败。单用例入口 `PULSE_SELFTEST_CASE=<case>`，exit=0 ⇔ 该用例 0 失败。
- **ops 层验证入口是 `build\pulse_ops_test.exe`**（控制台，74 项，需同目录 `pulse_shell.exe`），不归 `--selftest` 管。**`bench_data\` 被清掉时它会有约 40 项假失败**（Setup 用单级 `CreateDirectoryW` 造 `bench_data\opstest\src|dst`）→ 跑之前先建 `bench_data`。
- **链接/跑自检前先杀** `pulse` / `pulse_shell` / `Pulse.Index` / `Pulse.Preview`（否则 `LNK1104`）。用户常开着 `D:\Builder\pulse-selftest\Release\pulse.exe`。
- **偶发假失败**（重跑即过，不是回归）：`tab-handoff`（探针窗被盖）、`sidebar: dragging thumb to top…`（`SetKeyboardState`）。
- **可视化验证**：`pulse.exe --shot <png> --test-instance [--shot-scale] [--shot-language] [--size WxH] [路径]`；设置页专用 `PULSE_TEST_SETTINGS_EXPANDED`（位掩码，页 1 维护区 = 2）、`PULSE_TEST_SETTINGS_SCROLL`（dip）、`PULSE_TEST_SETTINGS_FLOW=<日志>`（**必须同时给 `--shot`**，否则不跑用例 = "exit=1 且没日志"）；虚拟路径 `pulse:settings:index|about|context|duplicates` / `pulse:recent` / `pulse:recycle`；详情面板加 `--shot-details`。视口外的行 hit-test 必失败（先滚进视口）；预览 handler 的 overlay 是独立 TOPMOST 窗口，`--shot` 抓不到。
- **打包**：`build_installer.bat`（产物已在时用 **`/skipbuild`**）= `check_release_payload.ps1`（查自测 dispatcher）→ 校验 6 个产物 → `scripts\stage_installer_runtime.ps1`（复制匹配的 MSVC CRT）→ ISCC → `dist\PulseSetup-<version>.exe`。ISCC 的 `PrivilegesRequired=admin` + HKCU 警告属预期；日志习惯 `d:\builder\installer-last.log`。**本地包与 Release 资产同名不同内容，别互相覆盖**；要覆盖必须重新签名清单（`scripts/sign_update_manifest_local.ps1`，因本机无 pwsh）。
- **二进制抽查**：`[System.IO.File]::ReadAllBytes` + `[Text.Encoding]::Unicode.GetString`（ASCII 解码搜不到宽字符串）。
- **收尾习惯（用户要求）**：一轮结束后删掉项目内生成的中间产物（截图/探针脚本/夹具/沙箱残留），**保留** `dist\` 与 `build\`，`.codebuddy\` 不动；一次性 bat/msg 放 `d:\builder\`。

### 命令与工具坑（写脚本前必看）

- 工具 shell 会在 **cmd 与 PowerShell 之间跳**（`Start-Process` 在 cmd、`cd /d`/`&` 在 PS 会报错）；别套 `powershell -Command "…"`（外层吃 `$p`/`$_`）。
- **多行命令 / here-string 会被工具经 cmd 包装拆坏**（"文件名、目录名或卷标语法不正确"）⇒ 要 `.bat` 就用 `write_to_file` 写文件再执行。
- **临时 `.bat` 里 `exit /b %ERRORLEVEL%` 放在 `if (...)` 块内会在解析时展开、恒为 0**（把失败伪装成成功）⇒ 用 `if errorlevel 1 exit /b 1`，或把取值放块外；同理块内 `set` 后立刻 `%VAR%` 用不了。
- `Add-Type` 的 P/Invoke 必须 `CharSet=Unicode`；`$pid` 是只读自动变量不能接出参。
- **合成鼠标不可靠**（桌面被抢占、目标被盖）；`SetForegroundWindow` 常被拒绝；侧栏/详情面板拖动结束依赖 `GetKeyState(VK_LBUTTON)`，`PostMessage` 驱动不了。
- **`search_content` 偶发假 0 命中**（`WM_TIMER`、`quickPreview` 等都返回过 0 而实际存在）⇒ **0 结果必须用 `read_file` 或 `git grep` 复核**。
- "改动没生效"先比对**进程/产物文件时间戳**与改动时间。

---

## 三、代码与 UI 约定

- **四进程 + 命名管道**：`pulse.exe`（唯一 UI）、`Pulse.Index.exe`（MFT/USN 索引，支持服务 / `--network-agent` / `--content-agent`）、`pulse_shell.exe`（COM 隔离）、`Pulse.Preview.exe`（预览/缩略图）。IPC 在 `src/ipc/protocol.h`（16 B `MsgHeader` + 小端 payload）。
- **目录**：`src/app`（状态/控制器，核心 `app_state.h`）、`src/ui`（渲染 + 立即模式控件 + O(1) 虚拟化 `view_layout.h`）、`src/fs`、`src/ops`（单 worker 串行 + 撤销）、`src/index`、`src/bench`。
- **线程模型**：UI 单线程，worker 完成 `PostMessage(WM_*)`；取消用 generation / request id / epoch；阻塞 FS / 预览 / Shell 严禁上 UI 线程。
- **风格**：注释一律**英文**；新功能拆独立 `.h/.cpp`（不堆 `app_main.cpp`）；四空格缩进、`PascalCase` 类型 / `snake_case` 数据；`/W4` 不得新增警告。
- **`BuildVm(s, probe_details)` 默认 `true`，但 hit-test / 输入 / 光标路径必须传 `false`**（paint 才拥有探针副作用）。
- **布局常量**（`src/ui/panel_metrics.h`）：`kListMinWidthDip = 112`（用户偏好，Explorer 是 144；改它要同步 `app_controllers_test` 4 条期望 + `selftest_1b2` 3 处 + 冒烟脚本 `-ListMinDip`）、`kSidebarMinWidthDip` 160、`kSidebarRailWidthDip` 48、`kDetailsMinWidthDip` 300；**窗口最小宽度 320 DIP**（`ptMinTrackSize.x`，改前 640，窄窗口截图受它限制）。
- **`sidebar_width_` 是像素、`sidebar_width_dip_` 是 DIP**，混用会被重复缩放；存储只做合理性校验（160–4000 / 300–4000），渲染时再 clamp。
- **侧栏分区 id 不可改序**：`app::SidebarSectionId`（`Workspaces=0, QuickAccess, SavedSearches, Drives, Tags, Networks, Cloud=6, Count=7`）—— 前 6 个与 session 旧掩码位一一对应；`ui::SidebarGroup::id` 是渲染/命中唯一身份。**坑**：`BuildWindowViewModel` 参数序 `…, collapsed_mask, hidden_mask, starred_expanded, sidebar_order, quick_access_hidden_mask`（collapsed 在 hidden 前，极易传串）。
- **命中矩形与绘制必须同源**（曾踩：chip 150 ms 动画期命中/绘制不一致 → 用 `TabGroupChipRectForHit`；重复文件页 `dup_keep`/`dup_open` 双份真源；列表行 hover 填充需要卡片背景包住）。
- **`ComPtr` 没有 `operator bool`**，用 `.get()`；`D2D1_INTERPOLATION_MODE_HIGH_QUALITY` 在 D2D 1.1 不可见（用 `LINEAR`）。

### 加设置开关的完整链路（照抄这份，容易漏）

`app_prefs.{h,cpp}`（字段 + keys + 默认值 + ToJson + FromJson + 合并基线）→ `strings.rcinc` 中英 → `string_ids.h` → `localization.h` / `localization.cpp` 的 `kLastString` → `ui_settings_core.cpp` 的 `toggle(...)` → `settings_layout_sections.h` 的 `row()` → `ui_renderer_internal.h` 的矩形 → `ui_hit_test.cpp` 命中（`SettingsToggle` + index）→ `ui_settings_view.cpp` 滚动定位 → `ui_renderer.h` 的 `settings_*` 字段 → `app_runtime.cpp` 填 vm → `settings_controller.cpp` 的 `ToggleSetting(index)` → `settings_ui.cpp` 的 `{I::X,0,1}` 分组行。

---

## 四、字符串与本地化

- **新增 `IDS_*` 必须同步 `localization.cpp` 的 `kLastString`**，否则 `l10n::Get()` 超范围**静默返回空串**（表现为设置行只剩图标 + 开关、按钮文字整片空白）。
- **"文案放得下"的断言对空串恒真** ⇒ 必须同时断言非空。设置页描述单行绘制，中英都要放进 `行宽 − 126*scale`。
- **加 ID 前先查上游占用**；**rebase 解完冲突后跑一遍重复 ID 扫描**（撞号不编译报错、只取错文案 —— 曾与上游 `IDS_UPDATE_* 1992–1997` 撞号，本项目整体挪到 **1998–2001**）。
- 号段备忘：1979/1980 隐藏与受保护文件、1981–1985 侧栏分区菜单、1986 OneDrive、1988/1989 预览标签、1990/1991 预览开关、1995 编辑组、1998–2001 多窗口相关、2002 打开文件所在位置、2014 缩略图缓存、2015/2016 文件哈希、2017 提示延迟自定义、2020–2025 设置页新批、2026 「项/秒」。

---

## 五、已知坑（按域）

**预览 / 缩略图**
- 宿主**懒启动 + 常驻复用**，请求单 worker 串行，**每请求只有 1 s deadline**（超时 `StopChild()` 杀宿主 → 下次重建全部设备）；`kPreviewMaxPixelSize = 1024`（GIF 512）、默认 512、分桶 128、LRU 16 MB / ≤128 项。
- **失败路径最贵**（xlsx 首 231 ms + 41 ms/次、CAD PDF 264 ms + 53 ms/次），因为失败也走完 4 条分支 → Issue #9。
- 两处"失败会粘住"已修：`FindPreviewHandlerClsid` negative 缓存 **30 s TTL**；provider 成功返回过就 `ClearSlowProvider()`。**provider 永不返回只能等它退出——不能 kill Office**（可能是用户在编辑的实例）。
- 预览 handler 两档预算：`kCommandBudgetMs = 3000` / `kSlowOpenBudgetMs = 20000`（面板当前请求 == `working_identity` 时用长预算）。
- overlay 跟随：**纯移动必须 `SWP_NOZORDER`**（改 z 序会与 provider 跨进程子窗口握手 → 卡死"在后追"）。
- 纯 GDI 会**静默跳过 EMF+ 记录、渲染空白**（Office 剪贴画 = EMF+ 封在 EMF 里）→ 用 GDI+；`gdiplus.h` 要求 `<objbase.h>` 先在内 + 先 `#undef min/max`。
- `ID2D1SvgDocument::GetRoot` 返回 **void**；`D2D1_SVG_VIEWBOX` 无 inline 重载（走 `(name, POD_TYPE_VIEWBOX, ptr, sizeof)`）。`d2d1`/`d3d11`/`dxgi` 刻意不隐式链接（`LoadLibraryW` 按需）。
- **`HasInk()`（>1% 非白像素）断言必不可少**（"返回 Bitmap + 尺寸对"证明不了渲染成功）；缩略图缓存会缓存失败结果且不重试。
- 诊断：`PULSE_PREVIEW_BENCH=<文件>` 跑 `pulse_preview_test.exe`（打印宿主启动 + 每请求 ms + kind/status/error）。

**右键菜单**
- `shell-verbs.bin` 若在 `l10n::Initialize` **之前**播种，合成行文本是空串 → 整条"打开方式"消失（issue #4 根因）→ 排查 `PULSE_SELFTEST_CASE=context-verbs`（dump 缓存/实时枚举/UI 过滤三层）。
- `shell_verbs.cpp` 显式跳过 `printto`（只留 `print`）；`IsBuiltinContextVerb` 丢掉与内置重复的 verb。目录键归一化 = `ipc::CatalogKey`（小写 + 去空格 + 去单字符加速键后缀）。
- Shell 对话框 owner = `OpsManager::SetUiWindow(hwnd)`（`app_main.cpp` 在 `ops.Start` 前设）；"打开方式…"/"属性"走 open 队列**头部**。**已定案**：不自绘应用列表、不再优化系统对话框耗时。

**宿主编辑框**
- 点击同窗口内部**不改变键盘焦点**，而四类宿主编辑框只靠 `WM_KILLFOCUS` 收尾 → `HandleLButtonDown` 开头对活动编辑框 `SetFocus(hwnd)`（点别处提交、Esc 取消）。
- **销毁带焦点的宿主编辑框前必须武装防重入开关**（否则同步 `WM_KILLFOCUS` → 重入 → `renameIndex = -1` → 越界）；判活用 `IsWindow()` 并总是置空句柄。
- **没有 LumaText 时不能隐藏原生光标**（`HideCaret` + 闪烁定时器只在 `LumaTextEnabled()`）。
- 重命名走**进程内快路径**（`RenameInProcess()`/`MoveFileW`），**无 shell 回退**；只接受单一文件名（拒绝路径分隔符/保留名/非法名），仅大小写变化必须真落盘。
- 筛选框输入文字不可见：根因是 `FilterEditProc` 仍回落 `DefSubclassProc` → **已定：提 issue、不动代码**（草案 `.git\ISSUE_FILTER_EDITOR_TEXT.md`）。

**设置持久化 / 文件夹接管**（细节见 `SUMMARY.md §4.6`）
- 四条保证：`Save()` 前读盘逐字段合并（基准 `disk_state_`，保存后基准取内存）；**文件存在却不可读 → 改名 `.bad`，绝不覆盖**；写前 `.bak` + 截断自愈；原子写临时名 `path + L".tmp." + <pid>`。
- 注册表对账：**文件 = 意图真相源，注册表 = 可修复投影**；"说关但注册表明确是我们的且在开 → 采用为开"；`CommandIsShellDefault()`（explorer.exe 镜像）**可替换、不当残留清**。
- 接管范围：`Directory`/`Drive` + `Folder`（Folder 必须在 verb 的 `\command` 下写**空 `DelegateExecute`** 遮蔽 HKCR 委派处理器）；**Win+E 的 CLSID 刻意不接管**。Pascal 里用 `=` 不是 `==`。
- `src/app/*.cpp` 里 grep `SetTimer|WM_TIMER` 会**误导性返回 0**（实际在 `app_main.cpp` 的 `kTimerUi`）——加定时器前先搜 `kTimerUi`；新增 `case WM_TIMER` 会编译报"case 值 275 已使用"。

---

## 六、硬边界（做不到的事，别再试）

- **第三方"在文件夹中显示"的选中**：`explorer.exe /select,` 由 Explorer 自己处理（探针实测选中正确、不经类动词）、`SHOpenFolderAndSelectItems` 同样只认 Explorer。**走类动词的"打开文件夹"会被我们截走 → Pulse 打开但没有选中**（文件名根本不在消息里）。
- Win+E 的 Explorer CLSID、特殊命名空间、系统打开/保存对话框 —— 接不到。
- provider 永不返回时无法取消（不能 kill Office）。
- 悬停卡片斜向移出无"安全三角"宽限；chip 命中/点击无自动化覆盖（合成鼠标不可靠）。
- 自动化不能测"翻转真实注册表"的主题跟随（崩溃会把用户主题留在翻转态）。
- 用户不接受"直接全屏"的看图形态（图片全屏查看器已按此删除）——以后要做必须**窗口化**、能边看图边操作文件。

---

## 七、用户拍板清单

**做 / 已做**
- 功能候选批：除「打印」外全做，**不引入第三方库**（只用 Windows API + 仓库已有代码）。
- 文件夹关联接管 `Directory`/`Drive`/`Folder`；命名空间输入保留接管、只修标签。
- 星标/固定到快速访问的行也加"打开文件所在位置"；提示延迟第 4 档 + 输入框（`clamp(50,5000)`）。
- 悬停提示开关 + 延迟、隐藏项淡化、视图菜单快捷开关、文件哈希 + 解除网络锁定、缩略图磁盘缓存、每文件夹视图记忆。
- 提交纪律与 PR 口径（见 §一）。

**不做 / 已否掉**
- 图片全屏查看器（实现后要求删除）；文件夹大小（走索引）；`--select`（打开并选中，整批撤销）；Win+E CLSID 接管；写"翻转真注册表"的自测；自绘"打开方式"应用列表；CAD 兄弟扩展借用（完整回退）；chip 折叠态"右缘小圆点"。
- 审阅明确"不做"：抽整个 Save 骨架模板、合并 `app_prefs.cpp` 匿名 namespace、四标志折枚举、测试上 RAII、chip 矩形循环抽取。

**沟通口径（用户明确纠正过）**
- 只答他问的；不要把他顺口说的动机/背景当需求做方案，也不要顺手推荐"替代做法"当结论；额外建议最多一句并标明。
- 回答先给事实（代码位置/现状），再给可选方案。
- **改动攒批**：小改动不要立刻重建/出包，等他准备好时一起构建验证。
- 删文件/破坏性操作要按他说的范围精确执行（曾多删 `MEMORY.md`，已恢复）。

---

## 八、当前状态

- **未提交、未验证**：文件操作（删除/还原/清空回收站）速度显示修复 —— `src/ops/ops_manager.{h,cpp}`、`src/ui/file_operation_dialog.cpp`、`src/common/{string_ids.h,localization.h,localization.cpp,strings.rcinc}`、`src/app/settings_flow_test.cpp`、`src/bench/ops_test_main.cpp`；编译过一批，最后补的两个 include（`<cstdio>`/`<cwchar>`）未编译；`pulse_ops_test` 与目视待跑（详见 `SUMMARY.md §4.11`）。
- **PR**：`feature/multi-window-tabs` 由用户维护正文。
- **待用户**：筛选框输入不可见 → 提 issue（草案在 `.git\ISSUE_FILTER_EDITOR_TEXT.md`）；`docs/build.md` / `README.md` 链接 / `.gitignore` 保持本地专属。
- **接手第一件事**：`git status` 看工作区 → 跑全量 `--selftest` 与 `build\pulse_ops_test.exe`（先建 `bench_data`）确认基线；"改动没生效"先比对时间戳；用户说"提交/构建"才动。
