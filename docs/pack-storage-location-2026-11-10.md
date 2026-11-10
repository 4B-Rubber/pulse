# 增强包存储位置与卸载策略（可行性方案）

日期：2026-11-10。状态：待实施。
实施完成后，把结论并入 `docs/native-packages.md` 并删除本文件（保持仓库文档收敛）。

## 0. 结论

方案可行。第一版做三件事：

1. 增强包根目录支持三种位置：**用户目录（默认）、程序目录（仅可写时可选）、自定义目录**。
2. 卸载策略按位置绑定：程序目录**必删**；用户目录与自定义目录**可选保留**。
3. 切换位置**不搬迁**已有包：新下载走新位置，旧位置残留给出提示与清理入口。

不做：自动迁移、ProgramData 共享、提权下载。

## 1. 现状（代码事实）

- 包根目录固定为 `%LOCALAPPDATA%\Pulse\packs`，由 `src/common/preview_packs.h` 的 `PacksRoot()` 拼接，读环境变量 `LOCALAPPDATA`。
- 布局：`packs\<key>\<version>\`，`key` 取 `ffmpeg` / `images` / `raw` / `archive` / `content` / `ai`；`installed.json` 记录版本，`src/app/content_pack.cpp`、`src/app/ai_pack.cpp` 依编译期固化的 size + SHA-256 校验，通过后以只读句柄持有。
- 设置文件目前是 `PacksRoot()\packs.json`（`PacksSettingsPath()`），卸载器 `src/setup/setup_uninstall.cpp` 也从 `%LOCALAPPDATA%\Pulse\packs\packs.json` 读 `remove_on_uninstall`，默认 `true`（删除）。
- 包不在安装包 payload 内：`content_pack.h`、`ai_pack.h` 明确写"never ships in the installer"，首次使用时从本版本 release 下载。
- 预览宿主 `Pulse.Preview.exe` 与主程序共用 `preview_packs.h`（header-only），两者可能不在同一目录：安装版的预览宿主在 `DialogBridge\<hash>\`。

## 2. 目标与非目标

目标：

- 用户可选包位置，覆盖便携版、大容量盘、多磁盘机器。
- 卸载行为与位置语义一致，且不会误删用户目录里的其他文件。
- 旧配置（无位置字段）行为不变。

非目标（第一版）：

- 不自动搬运已下载的包；不做跨盘中断恢复。
- 不做 ProgramData 全机共享。
- 不做提权下载（程序目录不可写时直接禁用该选项）。

## 3. 位置与卸载规则

| 模式 | 实际根目录 | 可写要求 | 卸载时 |
|---|---|---|---|
| 用户目录（默认） | `%LOCALAPPDATA%\Pulse\packs` | 无 | `remove_on_uninstall` 决定，默认删除 |
| 程序目录 | `<安装目录>\packs` | 该目录必须可写，否则选项置灰 | 直接删除，不提供开关 |
| 自定义 | `<用户选择的目录>\packs` | 该目录必须可写 | `remove_on_uninstall` 决定，默认删除；只删 `packs` 子目录 |

补充规则：

- 程序目录模式在标准安装（`C:\Program Files`）下通常不可写，选择项置灰并说明原因；便携版正常可用。
- 自定义目录拒绝：盘根、`Windows`、`Program Files`、网络路径、只读设备、安装目录本身。
- 自定义目录失效（移动盘拔出、被删）时回退用户目录并记录日志，不中断启动。

## 4. 数据模型与路径解析

`packs.json` 新增字段（沿用扁平键，`preview_packs.h` 的 `PackSettings`）：

```
location: 0 用户目录 / 1 程序目录 / 2 自定义   (缺省 0)
custom_root: 用户选择的父目录绝对路径          (location=2 时必填)
app_root:   主程序解析出的安装根绝对路径        (location=1 时由主程序写入)
remove_on_uninstall: 已有字段，仅 location 0/2 生效
```

关键改动：**设置文件位置与包根解耦**。

- `PacksSettingsPath()` 改为固定 `%LOCALAPPDATA%\Pulse\packs.json`，不再依赖 `PacksRoot()`，避免"读设置才能知道包在哪、但设置又在包目录里"的循环依赖。
- `PacksRoot()` 改为：读设置 → 按 `location` 解析 → 校验可写 → 返回；任何异常回退 `%LOCALAPPDATA%\Pulse\packs`。
- 兼容旧路径：新位置读不到时回退读 `%LOCALAPPDATA%\Pulse\packs\packs.json`，读到后迁移到新位置并删除旧文件。
- 预览宿主（`Pulse.Preview.exe`）不能靠自身 module 路径推安装根，统一用 `app_root` + `\packs`；`app_root` 为空或不合法则回退用户目录。

## 5. 切换位置的流程（第一版）

1. 设置页选择新模式/自定义路径 → 校验（可写、非系统目录、非网络路径）。
2. 保存 `packs.json`，调用 `ForgetVerifiedContentPack()` / `ForgetVerifiedAiPack()` 释放只读句柄。
3. 新下载与后续校验一律走新根；已在运行的预览宿主在下次启动时读到新配置。
4. 若旧根仍有包，设置页显示"旧位置仍有 X MB"与「清理」按钮；清理只删旧根的 `packs` 子目录，不迁移。

切换不改动正在运行的预览宿主；它按进程启动时读取的根工作，属于预期行为。

## 6. 卸载

`src/setup/setup_uninstall.cpp`：

1. 读配置：先读 `%LOCALAPPDATA%\Pulse\packs.json`，读不到再读旧位置（兼容）。
2. 按 `location` 决定删除目标：
   - 0：`%LOCALAPPDATA%\Pulse\packs`；
   - 1：`<安装目录>\packs`；
   - 2：`<custom_root>\packs`，**不删 `custom_root` 本身**。
3. `remove_on_uninstall == false` 且 `location` 为 0 或 2：保留包目录，仅删除其余 `%LOCALAPPDATA%\Pulse` 内容与配置。
4. 自定义目录不可达：跳过删除，写日志，不阻断卸载。
5. 无论是否保留包，都删除 `packs.json`（新、旧两处）。

## 7. 可写性探测

- `IsWritableDirectory(path)`：创建并删除一个探针文件（`packs-write-probe.tmp`），成功即视为可写；不使用 ACL 推断。
- 主程序启动时对"程序目录"探测一次，结果用于置灰与回退；自定义目录在每次保存设置时探测。

## 8. 设置页 UI

文件：`src/ui/ui_settings_packs.cpp`（预览增强包页）。

新增一行「位置」，三个分段：用户目录 / 程序目录 / 自定义…。

- 程序目录不可写：分段置灰，悬停说明"当前安装目录只读"。
- 自定义：显示当前路径与「更改…」按钮（`IFileOpenDialog` + `FOS_PICKFOLDERS`），下方显示实际生效的 `packs` 路径。
- 「卸载时保留增强包」开关：仅用户目录与自定义目录显示；程序目录隐藏。
- 旧位置残留：一行提示 +「清理」。

字符串进 `tools/l10n/catalog.tsv`（编号从 6413 起），三语齐全后跑 `gen_strings.py`。

## 9. 测试隔离

`--test-instance` 已把 `LOCALAPPDATA` 重定向到临时目录。由于配置固定在该目录下，测试实例读不到真实的 app/custom 设置，自然回退用户目录，无需额外分支。实施后加一条用例，确认隔离实例不会在真实安装目录或真实用户目录下创建 `packs`。

## 10. 改动清单

| 文件 | 要点 |
|---|---|
| `src/common/preview_packs.h` | `PackSettings` 加 `location`/`custom_root`/`app_root`；`PacksSettingsPath()` 改固定路径 + 旧位置兼容读；新增 `ResolvePacksRoot()`、`IsWritableDirectory()`、自定义目录校验 |
| `src/app/settings_controller.{h,cpp}` | 读取/保存新字段；切换位置；旧包统计与清理；`state.root` 改为解析后的值 |
| `src/app/app_runtime.cpp` | `vm.settings_pack_location`、`custom_root`；程序目录可写标记；旧位置占用 |
| `src/ui/ui_view_model.h` | 对应 vm 字段与 `PackAction` 新值 |
| `src/ui/settings_layout_sections.h` | 位置行、自定义行、保留行、旧包提示行 |
| `src/ui/ui_settings_packs.cpp` | 绘制与交互 |
| `src/ui/ui_hit_test.cpp` | 命中区域 |
| `src/app/settings_ui.cpp` | 动作分发与搜索目标 |
| `src/setup/setup_uninstall.cpp` | 按位置删包、配置读取兼容、不可达跳过 |
| `tools/l10n/catalog.tsv` | 新增字符串 |

## 11. 验收清单

1. 默认（无新字段）行为与现在一致，包仍在 `%LOCALAPPDATA%\Pulse\packs`。
2. 选用户目录：下载、预览、AI 宿主定位均正常；关闭"卸载时保留"后卸载删包，开启后保留。
3. 选程序目录（便携版可写目录）：包落在 `<exe>\packs`；卸载必删；标准安装下选项置灰。
4. 自定义目录：包落在 `<所选>\packs`；切到新位置后新下载走新目录；旧位置显示占用并可清理；卸载只删 `packs` 子目录。
5. 自定义目录选盘根、`Windows`、网络路径时被拒绝。
6. 自定义目录被移除后启动回退用户目录并记录日志。
7. `--test-instance` 不在真实安装/用户目录产生 `packs`。
8. 旧版 `packs.json`（位于旧位置）升级后仍能读到设置。

## 12. 风险与回退

- **只读句柄**：切换位置与清理前必须调用 `ForgetVerifiedContentPack()` / `ForgetVerifiedAiPack()`，否则删除会失败。
- **预览宿主路径**：只能依赖 `app_root` 缓存，不能自推；缓存缺失即回退用户目录。
- **多用户机器**：程序目录模式下，非安装用户可能无写权限；第一版允许只读复用，更新失败时提示"该位置由其他用户维护"。
- **配置损坏或指向非法路径**：解析失败一律回退用户目录，并在日志中记录原值。
- 回退方式：`location` 缺失即视为 0，等价于改动前的行为。
