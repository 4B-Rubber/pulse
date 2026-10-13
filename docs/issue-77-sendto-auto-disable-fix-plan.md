# Issue #77 回归分析与修复计划：发送到被「慢扩展自动禁用」机制静默永久禁用

> 关联：jimmgreen/pulse#77（OPEN）。原始 bug（发送到无子菜单、点击无反应）已由 8b92775（v1.0.51）修复并保留在 v1.0.52。
> 追加反馈（clcm8629，Windows 10 专业版 22H2，OS 内部版本 19045.7725，Pulse 1.0.52 x64）：
> 「前两三个版本短暂能正常使用，但这几次更新后右键直接不显示『发送到(N)』了」，
> 且设置里「分享」分组开关是开着的，资源管理器原生菜单发送到正常。
> 状态：**已实现（2026-10-05）**，未提交。实现 = §2.1 方案 A + 方案 B（双侧）+ §2.2 迁移 + §2.3 设置页口径；
> §2.4 防御性改进未动（按计划单列）。验证见 §7。

## 1. 结论（根因）

**8b92775 的修复本身与 #65 的「慢扩展自动禁用」机制相互作用，产生新回归：发送到
handler 在运行 3 次超过 1 秒的查询后被静默、永久地禁用，且该禁用持久化到
context_menu.json，跨重启、跨版本升级生效。** 这解释了「短暂正常 → 彻底消失」：

1. **修复把 SendTo worker 变成整个默认菜单的串行影子查询。**
   `QueryOneHandler` 的 kSendToHandler 分支（`src/shell_host/ctx_handlers.cpp:424-470`）
   在识别出占位项后，用 `BindToHandler(BHID_SFUIObject)` 取原生默认菜单，并
   `SafeQueryContextMenu(..., CMF_SYNCCASCADEMENU)` 同步初始化全部级联子菜单。
   机器上所有遗留扩展（报告者装了 Bandizip、115 网盘「上传到115生活」、
   LocaleEmulatorPlus、Run With Ntleas 等）都被这一次串行查询重复覆盖。
   这些扩展各自还作为独立 worker 并行查询，但 SendTo worker 的 elapsed
   结构上必然 ≈ 它们的总和，永远是最后一个完成的。

2. **慢扩展上报与自动禁用。**
   shell_host `collect_slow()`（`src/shell_host/main.cpp:1212-1222`）：worker
   `elapsed_ms >= 1000`、或会话整体已 late（≥1s）时仍未完成的 worker，一律上报 slow。
   app 端 `WM_SHELLCTX_ITEMS`（`src/app/app_main.cpp:1566-1572`）对每个 slow clsid
   记 `RecordComTiming(HandlerCatalogKey(clsid), 1000)`（固定记 1000ms）。

3. **3 次即永久禁用。**
   `RecordComTiming`（`src/app/context_menu_prefs.cpp:156-174`）：timeout_hits ≥ 3 →
   `slow_ext[key].disabled = true`，写入 context_menu.json 持久化。

4. **禁用后彻底不再查询。**
   `DisabledHandlerClsids()`（`context_menu_prefs.cpp:121-138`）把 disabled 的 clsid
   传进 `EnumerateCtxHandlers` → `AddHandlersFromKey` 里 `IsDisabledHandler` 直接跳过
   （`ctx_handlers.cpp:108`）。发送到从此从右键菜单消失。

5. **用户侧无感知、无从恢复（这是「更糟」的核心）。**
   - 「管理右键项」的开关读的是 `ItemEnabled()`（`settings_controller.cpp:646`），
     它不查 `slow_ext`；`HandlerEnabled()` 才查（`context_menu_prefs.cpp:112-119`）。
     于是设置页里「发送到」仍显示**开启**，「分享」分组也是开的，菜单里却没有。
   - 若 handler 从未成功完成过一次查询（从未 `RecordSeen`），列表里连行都没有。
   - 禁用状态随 context_menu.json 存活于重启与升级，版本更新自然「修不好」。
   - 理论上在列表里把该行关再开可解禁（`SetItemEnabled(true)` →
     `SetComDisabled(false)` 清禁用），但两个开关口径不一致，用户不知道要这么做。

修 #77 之前：发送到显示但点了没反应（可发现、可绕过）；
修 #77 之后：整个条目静默消失且不可恢复（更糟）。与反馈描述完全吻合。

## 2. 修复方案

### 2.1 主修复：让 SendTo 不再承担默认菜单的串行查询成本

**方案 A（首选）：原生菜单查询去掉 `CMF_SYNCCASCADEMENU`。**
`CollectHandlerItems` 已对 send_to 槽做显式初始化：入口处
`InitMenuPopup(menu2, menu3, slot.hmenu, 0)`（`ctx_handlers.cpp:479`），收集子项时
`CollectSubmenuLeaves` 也按自有 80ms 预算调 `InitMenuPopup`（:521-524）。
同步级联在查询阶段是多余的：只拿菜单骨架即可，flyout 填充交给现有路径。
- 位置：`ctx_handlers.cpp:457-458`（native_hr 那次 SafeQueryContextMenu 的 flags）。
- 风险：个别 Windows 版本上 sendto flyout 依赖查询期的 SYNCCASCADEMENU 填充。
  失败模式是「空 flyout 显示为禁用」（#77 修复第三条已保证该 UI 状态），
  不会回到「点了没反应」，可接受且可观测。实测 Win10 22H2 + Win11 验证。

**方案 B（兜底，必须一起做）：SendTo handler 豁免慢扩展自动禁用。**
即使方案 A 生效，整体慢的机器上 SendTo 仍可能是最慢 worker（它至少要查一次
原生菜单），不能被 #65 机制杀死。二选一或都做：
- shell_host `collect_slow()` 跳过 `w->desc.clsid == kSendToHandler`；
- 或 app 端 `WM_SHELLCTX_ITEMS` 对 `HandlerCatalogKey(kSendToHandler)` 不记 timeout。
- 理由：#65 防的是「QueryContextMenu 内部挂死」；SendTo 最坏结果是空菜单显示禁用，
  不存在卡死危害，且它有 `__try` / `SafeQueryContextMenu` 包裹，5s stuck 机制仍兜底。

### 2.2 恢复存量用户（必须，含 issue 报告者）

已踩坑用户的 context_menu.json 里已有 sendto 的 `slow_ext.disabled=true`，
升级后必须自动解禁：
- 在 `FromJson` / `MigrateSeenKeys` 阶段加迁移：清除
  `HandlerCatalogKey(kSendToHandler)`（7ba4c740-9e81-11cf-99d3-00aa004ae837）的
  disabled 与 timeout_hits。可做成一次性迁移（带已执行标记）或干脆对 sendto
  永远忽略 slow_ext（与 2.1-B 一致，天然幂等，推荐后者）。

### 2.3 设置页口径一致性（必须）

- 「管理右键项」行状态应反映真实可变性：`ItemEnabled()` 或 UI 层补查
  `HandlerEnabled()`；对 slow_ext 禁用的行显示「因响应超时被停用」并提供恢复
  （点击即 `SetComDisabled(key,false)`）。避免再出现「开关全开但菜单里没有」的死角。

### 2.4 防御性改进（可选，控制范围，可单列 issue）

- `collect_slow()` 的 late 分支过于粗糙：会话 ≥1s 时把**所有**未完成 worker 一律记
  slow（`main.cpp:1218`），个体冤枉。至少只对已完成且 `elapsed_ms >= 1000` 的记录，
  未完成的交给 5s stuck 机制。动 #65 行为，需单独评估。
- `RecordComTiming` 的禁用无衰减：可加「N 次快速成功后重置 timeout_hits」或
  「disabled 仅当次会话生效，下次启动重试一次」。改动面较大，单列。

## 3. 测试与验证

### 自动化（现有目标：pulse_ctx_handlers_test、selftest_1b2、app_controllers_test）

1. `ctx_handlers_test`：SendTo fixture（`--sendto` 已支持）在 native 查询**不带**
   CMF_SYNCCASCADEMENU 时仍能收集到 sendto 子项（覆盖方案 A 的行为边界）。
2. slow 上报含 sendto clsid → 不进入 `slow_ext`（或记录但永不 disable）（方案 B）。
3. 加载带 `slow_ext["<sendto-key>"].disabled=true` 的 JSON → 迁移后解禁（2.2）。
4. 设置页 toggle 对 slow-disabled 行的状态显示与恢复路径（2.3）。
5. 保留 #77 原有 3 个用例（慢扩展在前、普通占位、空发送到）不回退。

### 手测矩阵

- Win10 22H2（报告者环境）+ Win11 各一轮；
- 携带慢扩展（可用现有 TestStuckHandler 思路造一个 >1s 的 fixture handler），
  连续右键 ≥5 次，确认发送到始终在、禁用计数不增长；
- .txt / .lnk / 多选 / 桌面背景各试一次发送到目标完整性；
- 升级场景：用带 disabled 的旧 context_menu.json 启动新版本，确认自动解禁。

## 4. 涉及文件

| 文件 | 改动 |
| --- | --- |
| `src/shell_host/ctx_handlers.cpp` | :457-458 去 CMF_SYNCCASCADEMENU（方案 A） |
| `src/shell_host/main.cpp` | `collect_slow()` 豁免 kSendToHandler（方案 B） |
| `src/app/app_main.cpp` | :1566-1572 slow clsid 记录端豁免（若 B 选 app 侧） |
| `src/app/context_menu_prefs.cpp` | sendto 的 slow_ext 忽略/迁移（2.2） |
| `src/app/settings_controller.cpp` + UI | 管理右键项状态口径与恢复入口（2.3） |
| `src/bench/ctx_handlers_test.cpp`、`src/app/selftest_1b2.cpp` | 上述用例 |

## 5. 风险与权衡

- 去 CMF_SYNCCASCADEMENU 后若个别系统 flyout 未填充：表现为灰置禁用（可接受、
  可诊断），必要时退一步只对 sendto 子菜单补一次 InitMenuPopup。
- 豁免 sendto 慢上报后，极端挂死由 5s stuck 兜底（菜单仍会出现，只是该 handler 缺席）。
- 2.3 触及设置 UI 文案与交互，注意与「管理右键项」现有行语义兼容。

## 6. 后续跟进（不阻塞本计划）

- 报告者菜单里「分享」也不见（r4 截图）：Win10 上 packaged Share verb 是否被
  `AddPackagedHandlers` 枚举到需单独核查，可能独立于本回归。
- 回复 issue 时请报告者删（或备份后重置）`%LOCALAPPDATA%\Pulse\context_menu.json`
  验证发送到是否立刻恢复 —— 可现场确证根因，再让其等修复版本。

## 7. 实现记录（2026-10-05）

改动 15 个文件（+95/-6），未提交：

| 项 | 位置 | 内容 |
| --- | --- | --- |
| 共享常量 | `src/ipc/ctx_menu_util.h` | `kSendToHandlerClsid` / `IsSendToHandlerClsid` / `SendToHandlerCatalogKey()` |
| 方案 A | `src/shell_host/ctx_handlers.cpp` | 原生查询去掉 `CMF_SYNCCASCADEMENU`（flyout 填充走现有 `InitMenuPopup` 路径） |
| 方案 B-1 | `src/shell_host/main.cpp` | `collect_slow()` 跳过 sendto worker |
| 方案 B-2 | `src/app/app_main.cpp` | `WM_SHELLCTX_ITEMS` 慢上报循环跳过 sendto clsid |
| 2.2 | `src/app/context_menu_prefs.cpp` | `RecordComTiming` 忽略 sendto key；`FromJson` 末尾幂等清除 sendto 的 `slow_ext` 条目 |
| 2.3 | `context_menu_prefs.h/.cpp` | 新增 `RowEnabled()`（`ItemEnabled && !ComDisabled`） |
| 2.3 | `src/app/app_runtime.cpp` + `settings_controller.cpp` | 行构建与 toggle 都用 `RowEnabled` → 慢禁用行显示为关、一次点击恢复 |
| 2.3 UI | `ui_renderer.h` + `ui_settings_context.cpp` | `SettingsRowView.slow_disabled`；行文字变暗 + 右侧小字原因（开关仍可点） |
| 2.3 l10n | `string_ids.h`(2525) / `strings.rcinc`(zh+en) / `strings_zh_tw.rcinc` / `localization.h` | `IDS_CONTEXT_SLOW_DISABLED`「因响应超时被停用，点按开关即可恢复」 |
| 用例 | `src/app/selftest_1b2.cpp` | 5 条 #77 用例：超时不记账 / 恒不禁用 / JSON 迁移解禁 / 行状态口径 / 开关恢复 |

验证结果：

- `pulse_ctx_handlers_test`：全 PASS，含 `--sendto` 真 Shell fixture —— **去掉 CMF_SYNCCASCADEMENU 后
  实装 SendTo 仍收集到 1 个可用目的地**（方案 A 行为边界实测通过）。
- `pulse_localization_test`：PASS（三种语言块键一致性）。
- `pulse.exe --selftest`：新增 5 条 #77 用例全 PASS；22 个 FAIL 全部是
  search history / advanced search / advanced edit 的真实弹窗键盘交互用例，
  与本次改动无关，属沙箱无交互桌面的环境限制（与 CI 诊断日志基线一致）。
- `pulse` + `pulse_shell` Release 构建通过。

遗留：手测矩阵（Win10 22H2 + Win11 连续右键、升级场景）待真机验证；§2.4 待单列 issue。
