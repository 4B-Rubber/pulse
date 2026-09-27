# 后台 CPU 修复测试安装包

日期：2026-09-18。用户要求生成安装包，自行安装测试。

- 修复说明：docs/mcp-index-background-cpu.md。
- 在 MSVC x64 环境执行 cmake --build build --target pulse --parallel 2，确认产物最新，随后执行 build_installer.bat /skipbuild。
- Release 载荷检查通过，未包含内嵌 selftest dispatcher。
- Inno Setup 6.7.3 编译成功，整体命令退出码 0。
- 产物：dist/PulseSetup-1.0.32.exe，版本号仍为 1.0.32，包含此次源码修复。
- 文件大小：7864696 字节。
- SHA-256：DB59F01ED19B131F4C1ED263960D5C6B666EA4D66201BAD279DB66021AE76D9D。
- 编译器有 admin 安装模式使用 HKCU 区域的警告，对应安装脚本已有的用户自启动注册项；本次未改动安装行为。
- 仅生成安装文件，未执行安装包、未关闭用户程序、未重启索引服务，未额外运行功能测试。现场 CPU 效果由用户安装后验证。
