# 内存优化版测试安装包

日期：2026-09-18。用户要求生成安装包，自行实机测试。

- 优化说明及上一轮模块验证：docs/mcp-index-memory-optimization.md。
- 本轮检查 Release / PULSE_WITH_SELFTEST=OFF；再次构建 pulse、pulse_index，Ninja 确认无需重建，退出码 0。三处优化源码及新增测试的指纹与上一轮已验证版本一致。本轮未重复运行功能测试。
- Release 载荷检查通过，无内嵌 selftest dispatcher。
- 使用既有 installer/PulseSetup.iss、Inno Setup 6.7.3 编译，参数 /DAppVersion=1.0.32 /FPulseSetup-1.0.32-memory。只覆盖输出文件名，不修改应用版本或安装行为。
- 编译成功，整个打包与指纹核对命令退出码 0。等待进程的一次 100 秒超时不代表编译失败；最终编译耗时 157.296 秒。
- 产物：dist/PulseSetup-1.0.32-memory.exe。
- 文件大小：7876050 字节（约 7.51 MiB）。
- SHA-256：1999328168BCE73FA66DC2F6E2E74847920297F6299FF34C382666E3ACA04A62。
- 主程序 SHA-256：17F64A26A9F406094F713D62E2195AD4027F19C67D2D9DCF23C4925FED1E6690。
- 索引服务 SHA-256：5FC005CC83F48782477D082B08BD34F2C35B41F9F87B72F3C487C1EB60DDF84E。
- 七个 EXE/DLL 输入文件打包前后 SHA-256 一致，完整清单位于 dist/PulseSetup-1.0.32-memory.manifest.json。未运行安装器，也未对安装器进行解包验证。
- 原 dist/PulseSetup-1.0.32.exe 为此前 CPU 修复包，本轮未覆盖。内存优化测试请选带 -memory 后缀的新包，安装后的显示版本仍为 1.0.32，可用上述主程序/索引服务哈希辨识。
- 编译器仍有既有 admin + HKCU 安装区域警告；本轮未改用户自启动注册项或安装脚本。
- 未安装、未关闭 Pulse、未重启索引服务、未修改用户偏好或生产索引。实机内存与查询性能需用户安装后再观察。
