# 第二轮内存优化与诊断安装包

日期：2026-09-18。用户要求打包后自行运行，由助手在后续观察中定位。

- 说明及上一轮已通过的定向验证：docs/mcp-index-memory-feed.md。
- 本轮关键源码 SHA-256 与已验证版本一致，Release / PULSE_WITH_SELFTEST=OFF；再次构建 pulse、pulse_index，Ninja 报告无需重建，退出码 0。本次纯打包未重复运行功能测试。
- Release 载荷检查通过；索引程序二进制包含 change_flush_snapshot、feed_page_capacity_bytes、merge_flattened 诊断标记。该标记检查不替代运行验证。
- 使用既有 installer/PulseSetup.iss，Inno Setup 6.7.3，参数 /DAppVersion=1.0.32 /FPulseSetup-1.0.32-memory2；只改变输出文件名，不改安装行为和显示版本。
- 编译成功，耗时 156.312 秒。
- 产物：dist/PulseSetup-1.0.32-memory2.exe。
- 文件大小：7880971 字节（约 7.52 MiB）。
- 安装包 SHA-256：61D9F55471DB991FC4D562E304D1DA31551496488994963E89690A512A1BA9E8。
- 主程序 pulse.exe SHA-256：DFC9BBA4DC177375701431B0B57748C71DD10B7FF5876FC2CD2A9045BA726E3F。
- 索引程序 Pulse.Index.exe SHA-256：906E743BDF652F6D66A9B4A4B958A8992513766C5ADF9DC509470232BFFA9A52。
- 七个 EXE/DLL 输入文件打包前后指纹一致，原 CPU 修复包与上一轮 -memory 包均未改变。完整清单：dist/PulseSetup-1.0.32-memory2.manifest.json。
- 既有 admin + HKCU 安装区域警告仍存在，本轮未修改该设置。
- 未执行安装器、未解包验证、未关闭用户程序、未重启服务、未改变生产配置或索引。

## 用户运行后的定位步骤

1. 用户安装并打开程序后通知助手。先核对上述已安装文件指纹、服务 PID 和启动时间，避免误测同为 1.0.32 的旧版本。
2. 按先前方式关闭主窗口并保持后台运行，不主动清内存或重建。确认实际索引目录中的 pulse-index-timing.jsonl 新 PID 记录含 memory 字段。
3. 初次检查日志已生效，再对照接近此前 30–35 分钟运行时长的数据，观察旧代 feed 是否在快照换代后释放、保留容量是否趋稳。
4. 结合工作集、私有提交、阶段调用增量及 change_flush_before / snapshot / after、merge_before / flattened 的最新采样值定位。各点 max_* 不一定来自同一次操作，不能直接相减；映射文件字节数不能当作驻留内存。
5. 此安装包仅提供已验证优化和取证能力，尚无它的部署后实机结果，不预先宣称约 460 MiB 波峰已经解决。
