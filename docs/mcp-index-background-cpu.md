# Pulse 后台索引 CPU 排查与隐藏窗口轮询修复

日期：2026-09-18。

## 结论与证据边界

已确认主窗口隐藏/最小化后仍定时请求文件夹变更摘要和明细的代码缺陷，并在源码中修复。尚未替换安装目录中的程序，也未进行修复后现场 CPU 对照，不能认定截图中的全部 CPU 占用均已定位或消除。

## 只读现场观察

- 安装版本为 1.0.32，PulseIndex 服务设置为自动启动，服务 PID 为 34724，命令行为 Pulse.Index.exe --service。服务独立于主窗口运行是现有设计。
- pulse.exe PID 27536 仍驻留，观察时 MainWindowHandle 为 0。用户配置启用了 global_search_enabled 和 change_tracking_enabled；即使 keep_running_on_close 为 false，全局搜索也会使关闭操作隐藏窗口，而不是退出。
- 另一个 Pulse.Index.exe PID 24884 的命令行为 --network-agent，不能与系统服务混为一谈。
- 通过 Win32_Process 的 KernelModeTime/UserModeTime 对服务采样：10.2842693 秒内 CPU 时间增加 2.625 秒。系统有 6 个逻辑处理器，相当于单核约 25.5%、整机约 4.25%。这不是截图时刻的 15.9%，也不是长期平均值。
- 另一个独立的约 15 秒窗口中，网络代理 CPU 时间增加 0.46875 秒，主程序增加 0 秒；不同时间窗口的样本不能相加。
- 文件名索引阶段日志末尾的几条记录中，全量 rebuild 计数为 0，merge 计数保持 55，没有支持“此时持续全量重建”的证据。这些日志并不覆盖所有客户端查询成本。
- .NET/GetProcessTimes 直接访问系统服务返回拒绝访问，改用只读 WMI 进程累计时间。不提升权限、不附加调试器、不修改服务。

## 已确认的代码路径

1. src/app/app_main.cpp 的关闭路径在全局搜索启用时调用 HideWindow。
2. src/app/tray_controller.cpp 的 HideWindow 仅隐藏窗口，不停止 UI 定时器。
3. src/app/app_change_tracking.cpp 的 TickChangeTracking 原来不检查主窗口可见性；摘要轮询间隔为 2 秒，符合条件的变更视图明细刷新间隔为 5 秒。
4. src/index/change_tracking.cpp 的 Summaries 在 journal revision 变化时重新聚合保留的事件及祖先路径。后台持续发生文件变化会使无用 UI 查询反复触发聚合。

以上确认了隐藏窗口仍能制造索引服务计算负载的路径；没有调用栈采样或停用客户端的 A/B 实验，因此不把它宣称为所有占用的唯一来源。网络代理的扫描负载也未在本次修改中处理。

## 修复范围

- src/app/change_tracking_polling.h：集中定义可轮询条件，要求有效、可见且非最小化的窗口。
- src/app/app_change_tracking.cpp：在摘要与明细的定时查询之前执行可见性检查；不可见时隐藏浮层并直接返回。
- 不禁用变更跟踪、不撤销后台记录租约、不关闭索引服务、不改用户设置；恢复窗口后原有刷新逻辑继续运行。
- 已经发出的单次查询不做强制中断；此修复阻止后续定时重复查询。
- src/app/change_tracking_app_test.cpp：既有轮询测试提供可见窗口句柄，避免把无窗口状态当作前台状态。
- src/bench/change_tracking_polling_test.cpp 与 CMakeLists.txt：新增独立、无生产管道连接的窗口状态回归目标。

## 实际验证

- 通过 MSVC x64 环境执行：cmake --build build --target pulse pulse_change_tracking_polling_test --parallel 2，退出码 0。CMake 自动重链接了主目标的必要依赖；没有调用全量发布构建或安装脚本。
- 执行 build/pulse_change_tracking_polling_test.exe，退出码 0，9 个检查通过：无句柄、创建隔离窗口、初始隐藏、可见、隐藏时 10000 次策略检查均禁止轮询、托盘恢复、最小化、最小化恢复、销毁。
- 该目标验证生产代码共用的窗口轮询策略，不等同于对已安装服务做 CPU 性能验收。
- src/app/app_change_tracking.cpp 和 src/bench/change_tracking_polling_test.cpp 的编辑器诊断无错误/警告。
- git diff --check 通过；Git 提示现有 autocrlf 换行转换策略，无空白错误。
- 当前构建 PULSE_WITH_SELFTEST=OFF，没有执行内嵌 change-app 测试，也没有运行完整历史回归。

## 部署与后续验收

修复产物在 build/pulse.exe；当前安装并驻留的旧版本仍未更新。本次未关闭用户进程、未重启服务、未替换安装目录文件、未删除索引或日志、未修改用户配置。已有未跟踪的 .workbuddy-ai/ 保持不变。

下一步需在用户确认后正常退出驻留主程序并更新运行版本，再分别观察可见、关闭到托盘、最小化、恢复的行为与服务 CPU。由于此次修复位于主程序查询发起端，重启未更新的索引服务本身不会应用该修复。若仍高占用，应对残余负载单独采样，检查查询订阅、服务工作线程和网络代理，不能以本次定向检查通过替代现场验证。
