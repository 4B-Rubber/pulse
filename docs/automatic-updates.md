# 发布和自动更新

源码与发布页：<https://github.com/4B-Rubber/pulse>。本仓库发布的是 dev 频道（产物带 `-dev` 后缀），构建、密钥与发布细节见 [dev 频道](dev-channel.md)。

从 1.0.3 开始，客户端启动约 15 秒后检查更新，持续运行时每 6 小时再检查一次。检查在后台进行；有新版本时显示可点击的提示，同一运行会话不会重复提示同一个版本。可在「设置 → 关于与诊断 → 自动检查更新」里关闭后台检查；关闭后不联网检查、不弹安装提示，但卡片里的手动「检查更新」与「下载并安装」照常可用。用户点击后才下载安装包；下载期间可在设置中取消。安装包校验通过后启动安装向导，保留 Windows 管理员确认。正在执行文件操作或迁移索引时不启动安装。

1.0.2 及更早的安装包未配置更新源，需要手动安装一次 1.0.3 或更新版本。

## 设置开关

- **开关**：设置 → 关于与诊断 → 自动检查更新（`check_updates`，默认开）。关闭后不联网检查、不弹安装提示；卡片里的手动「检查更新」与「下载并安装」不受影响。

## 发布新版本

本仓库发布的是 dev 频道：安装包名与界面显示的版本带 `-dev` 后缀（`build_installer.bat` 与 CI 自动加，见 [dev 频道](dev-channel.md)），而 `version.txt` 与清单里的版本号保持数字。**每个新 dev 版本仍要递增 `version.txt`**——更新器按数字比较，同号不会提示更新。

1. 修改 `version.txt`，例如改为 `1.0.4`。
2. 在 `docs/releases/1.0.4.md` 写本次更新内容。
3. 提交、推送代码，再推送同名版本标签：

```powershell
git add version.txt docs/releases/1.0.4.md
git commit -m "release: prepare 1.0.4"
git push origin main
git tag v1.0.4
git push origin v1.0.4
```

GitHub Actions 的 **Build and publish Pulse** 工作流会构建、测试并打包两种版本。两者全部通过后，才发布 Release 并设为最新版本。单纯提交源码或上传 Actions 构建产物不会触发客户端更新。

| 客户端 | 安装包 | 更新清单 |
| --- | --- | --- |
| Windows 10 / Windows 11 x64 | `PulseSetup-版本-dev.exe` | `update-manifest.json` |
| Windows 8.1 x64 | `PulseSetup-版本-dev-win81.exe` | `update-manifest-win81.json` |

Release 正文自动提供上述系统说明与两个下载入口。不要重复覆盖已经公开发布的版本；修复发布问题时递增版本号。

## 更新校验

更新清单和安装包默认依次尝试 `https://ghproxy.net/`、`https://gh-proxy.com/` 加原始 Release 文件 URL，最后回退到 GitHub。无需开关或配置。网络错误或非 200 响应时切换到下一来源，每个来源最多请求一次；取消、写入失败和校验失败不会触发来源切换。切换前清空已下载内容，避免拼接不同来源的响应。

只对不含凭据或查询参数的 GitHub Release 文件地址启用公共加速，其他自定义更新域名仍直接访问。ghproxy.net 和 gh-proxy.com 是第三方公共服务，可用性由其运营方决定；未来官方对象存储/CDN 可通过现有清单地址配置接入。

清单采用 ECDSA P-256 签名，客户端内置的公钥位于 `cmake/update-public-key.txt`。签名覆盖版本号、最低系统版本、下载地址和安装包 SHA-256。客户端拒绝未通过签名、校验和不匹配、旧版本或不适用的系统版本；下载只允许 HTTPS，支持 GitHub 的 HTTPS 重定向。

签名私钥保存在**本仓库**的 Actions Secret `PULSE_UPDATE_PRIVATE_KEY` 中，备份应保存在仓库之外，不进入 Git。后续发布沿用此密钥和公钥，避免已安装客户端无法验证新版。上游的私钥不在本仓库手里，本仓库的客户端内置的必须是本仓库自己的一对密钥（见 [dev 频道](dev-channel.md)）：用上游签名、或沿用上游更新地址，都会让本仓库的客户端拿到不匹配的清单或安装包。

CI 使用 v143 和静态 VC 运行库，LumaText 使用仓库中 `third_party/lumatext` 的固定 SDK。`cmake/lumatext-sdk.json` 固定 `sdk-manifest.json` 的 SHA-256，`scripts/verify_lumatext_sdk.ps1` 在构建前逐一核对 DLL、导入库、头文件、CMake 导出和许可证，避免开发包与正式包混用。构建不依赖本机路径或私有 LumaText 仓库。SDK 仅供构建使用；普通用户下载 Release 顶部对应系统的安装包。
