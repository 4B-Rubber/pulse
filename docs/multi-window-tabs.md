# 多窗口标签（一个窗口一组标签）

Pulse 默认是单实例：第二次启动把文件夹转发给已经运行的窗口就退出，标签因此只能堆在一条标签带里。`--new-window [路径]` 让新启动的进程跳过单实例互斥，成为**独立窗口**，每个窗口有自己的标签。任务栏按钮的右键菜单（jump list）里也有「新建窗口」。

## 行为

- **每个窗口一组标签**：次级窗口读 session 只取起始目录，**不写 session**，也不建托盘图标、不注册全局热键、关掉窗口就直接退出——托盘、全局热键、撤销栈和持久化都留给主窗口。
- **起始位置**：`--new-window <路径>` 的唯一标签就是该路径；不带路径时是**最近使用**（`pulse:recent`）；托盘菜单的「新建窗口」传当前文件夹。窗口按"已存在的 Pulse 窗口数 × 32 px"级联。
- **跨窗口拖标签**：标签拖出标签带后，光标下的 Pulse 窗口接收它，且**总是新开一个标签**——即使对方已经开着同一个文件夹（这一点与双击文件夹的"已打开就激活"不同：前者是"把这个标签搬过来"，后者是"打开它"）。拖动期间有一张跟随光标的浮动卡片。
- **合并**：被拖走的是源窗口的**最后一个**标签时，源窗口关闭（不写 session），接收方**接管**托盘图标、全局热键与 session。
- **拆窗口**：在没有任何 Pulse 窗口的地方松手，标签变成一个**新窗口**。
- **边界**：只带走"当前文件夹"——分屏布局、标签组、自定义标签名不跟随；拖动过程中在标签带内仍是普通重排，只有离开本窗口（或落到别的窗口）才触发交接。

## 实现入口

- `src/app/instance_launcher.{h,cpp}`：`LaunchNewWindow`（`ShellExecuteW --new-window`）、`PulseWindowUnderPoint`（`WindowFromPoint` → 沿 owner 链找类名 `PulseMainWindow`，跳过发起窗口和 `PulseTabDragGhost` 卡片）。
- `src/app/single_instance_coordinator.{h,cpp}`：`Acquire`（`Local\Pulse.Singleton`，`--new-window` 跳过）、`SendTabTransfer` / `DecodeTabTransfer`（`WM_COPYDATA`，id `'PTBT'`）、以及既有的 `SendOpenPathToWindow` / `DecodeOpenPath`（id `'PULS'`）。
- `src/ui/drag_ghost.{h,cpp}`：`TabDragGhost`——layered、topmost、不吃鼠标的浮动卡片，按抓取偏移贴光标。
- `src/app/app_input.cpp`：`HandOffTabUnderCursor`（松手时的落点判定与交接）、拖动期间的 `AppState::tabDragExternal`。
- `src/app/app_main.cpp`：`WM_COPYDATA` 接收与 `WM_CLOSE` 的"合并退出"分支。`src/app/app_runtime.cpp`：`OpenFolderInNewTab`、`AdoptSingletonOwnership`。

## 定向验证

只构建 `pulse`、`pulse_app_controllers_test`，未运行全部历史测试。

- `pulse_app_controllers_test.exe`：
  - `tab transfer reaches the window that takes the tab`（转移消息真的送到目标窗口，payload 往返一致）；
  - `tab transfer message is not the shell's open-path forward`、`shell open-path forward is not a tab transfer`（两个消息 id 双向不可混用）。
- `PULSE_SELFTEST_CASE=tab-handoff`（`pulse.exe --selftest`）：
  - `the drag card hangs by the grab offset`、`the drag card is hidden before the hand-off`（浮动卡片就位与交接前隐藏）；
  - `the window under the cursor takes the tab`、`the other window takes it when the cursor moves on`、
    `the window that owns the tab is never its own target`、`a window carrying the drag card's class is never a target`（落点解析）。
  - 日志：`bench_data\selftest_1b2_last.log`。
- 手工：两个实例 → 把一个标签拖到另一个窗口（对方多出一个标签）；把最后一个标签拖过去（源窗口关闭，托盘图标与热键跟过去）；拖到没有 Pulse 窗口的地方（变成新窗口）；任务栏按钮右键 →「任务 → 新建窗口」。

验证边界：编译通过与上面的受控用例不能替代真实的多窗口交互——卡片观感、DPI 缩放下的命中、以及把标签拖到"另一个进程正忙"的窗口时的手感都需要人工确认。
