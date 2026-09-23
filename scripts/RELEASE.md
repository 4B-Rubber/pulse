# 发布流程

正式发布使用 GitHub Actions，同时生成 Windows 10 / 11 和 Windows 8.1 两种安装包。本仓库发布的是 dev 频道：安装包名为 `PulseSetup-<版本>-dev[-win81].exe`，清单与 `version.txt` 里的版本号保持数字，签名用本仓库自己的密钥（`PULSE_UPDATE_PRIVATE_KEY`），更新地址也指向本仓库。细节见 [dev 频道](../docs/dev-channel.md)，版本、标签、签名与清单配置见 [自动更新与发布](../docs/automatic-updates.md)。

本地验证使用 `build_release_ci.ps1`。`package_release.ps1` 为可选的 Authenticode 签名流程，需要自行配置代码签名证书；更新清单的 ECDSA 签名与 Windows 代码签名用途不同。
