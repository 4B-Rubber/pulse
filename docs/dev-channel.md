# dev 频道：构建、签名与发布

这个仓库（4B-Rubber/pulse）发布的是 **dev 频道**：产物名与显示的版本都带 `-dev` 后缀，更新清单由本仓库自己签名，更新源也指向本仓库。

## 版本与后缀

- `version.txt` 只放**纯数字** `x.y.z`：`CMakeLists.txt` 的 `project(VERSION)` 要求它是数字，客户端 `update_checker.cpp` 的 `ParseVersion` 也只认三段数字——后缀进去会让每次检查都变成 `InvalidManifest`。
- 后缀出现在两处：安装包名与程序名（`build_installer.bat` 传 `/DAppVersion=<版本>-dev`），以及设置里显示的版本（`PULSE_VERSION_LABEL`，见 `cmake/pulse_version.h.in`）。
- 发布一个**新** dev 版本时仍要把 `version.txt` 加一：更新器按数字比较，同数字不会提示更新。

## 更新清单与签名

- 清单 URL 指向本仓库：`PULSE_UPDATE_MANIFEST_URL`（`CMakeLists.txt` 的默认值，以及 `build_release.bat`、`build_win81.bat`、`scripts/build_release_ci.ps1` 里的显式传入）。
- 公钥：`cmake/update-public-key.txt`（65 字节未压缩点，小写十六进制），编译进客户端；私钥放在本仓库的 `PULSE_UPDATE_PRIVATE_KEY` Actions secret 里，**不要**提交进仓库。
- 私钥只在上游手里，所以本仓库必须用**自己的一对密钥**，否则客户端会以 `InvalidSignature` 拒绝本仓库的清单。
- 生成清单：`scripts/create_update_manifest.ps1`（ECDSA P-256 + SHA-256，IEEE-P1363 原始签名，`-ExpectedPublicKey` 会校验签名密钥与内置公钥一致）。
- 发布：`scripts/publish_release.ps1`（仓库已指向本 fork，资产名为 `PulseSetup-<版本>-dev[-win81].exe`，清单里的 `version` 保持数字）。

## 安装与共存

dev 包与稳定版**共用同一套身份**：AppId、安装目录、注册表项、AppUserModelID、单实例互斥体、`%LOCALAPPDATA%\Pulse` 数据目录。安装 dev 包会静默卸载并结束稳定版，随后复用同一份设置与会话。想要两者并存，需要把上述身份整体参数化成第二套。

## 用户可见的开关

设置 → 关于与诊断 → 自动检查更新（偏好键 `check_updates`，默认开）：关掉后不再后台检查、不再弹安装提示，这一步由 `src/app/app_updates.cpp` 的 tick 统一执行。卡片上的按钮仍是手动入口——检查、下载与安装都不受这个开关影响，关掉开关只表示不再自动提醒。
