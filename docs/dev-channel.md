# dev 频道：构建、签名与发布

这个仓库（4B-Rubber/pulse）发布的是 **dev 频道**：产物名带 `-dev` 后缀，更新清单由本仓库自己签名，更新源指向本仓库。

## 版本与后缀

- `version.txt` 只放**纯数字** `x.y.z`：`project(VERSION)` 要求数字，客户端 `update_checker.cpp` 的 `ParseVersion` 也只认三段数字——后缀进去会让每次检查都变成 `InvalidManifest`。
- 后缀出现在安装包名（`scripts/pack_setup.bat` 的 `PULSE_SUFFIX`，本机 `build_installer.bat`）以及设置与「关于」里显示的版本（`PULSE_VERSION_LABEL`，见 `cmake/pulse_version.h.in`；`PULSE_VERSION_STRING` 保持数字，更新比较还在用它）。
- 发布**新** dev 版本时仍要递增 `version.txt`：更新器按数字比较，同数字不提示更新。

## 更新清单与签名

- 清单 URL 指向本仓库：`CMakeLists.txt` 的默认值，以及 `scripts/build_release_ci.ps1`、本机 `configure_release.bat` / `build_selftest.bat` 的显式传入。
- 公钥 `cmake/update-public-key.txt`（65 字节未压缩点，小写十六进制）编译进客户端；私钥放在本仓库的 `PULSE_UPDATE_PRIVATE_KEY` Actions secret 里，**不要**提交进仓库。
- 私钥只在上游手里，所以本仓库必须用**自己的一对密钥**，否则客户端会以 `InvalidSignature` 拒绝本仓库的清单。
- 生成清单：`scripts/create_update_manifest.ps1`（ECDSA P-256 + SHA-256，IEEE-P1363 原始签名，`-ExpectedPublicKey` 校验签名密钥与内置公钥一致）。
- 发布：`scripts/publish_release.ps1` 指向本 fork，资产名为 `PulseSetup-<版本>-dev[-win81].exe` 与 `Pulse-<版本>-dev-portable-win-x64.zip`，清单里的 `version` 保持数字。

## 预览增强包

增强包（ffmpeg / images / raw / archive）目前仍从**发布它们的那次 release** 下载：`src/app/pack_catalog_generated.h` 里的地址保持指向上游 `jimmgreen/pulse` 的对应 tag。发布脚本里的 `ensure_release_packs.py`（校验并上传 pack 到本仓库 release）被改成 opt-in——设 `PULSE_PUBLISH_PREVIEW_PACKS=1` 才执行，因为把副本镜像过来需要先把 pack 载荷准备好（`--asset-directory dist/preview-packs` 或 `--source-tag`）。

## 发布流程

推 `v<version>` 标签触发 `.github/workflows/release.yml`：两个 channel 各自构建 + 跑测试 + 打包成 artifact，publish job 汇总、签名清单、`gh release` 上传并 `--draft=false --latest` 发布。本机没有 PowerShell 7 与 `gh` CLI，本地能做的是等价三件套：`build_release.bat` → `build_installer.bat` → `powershell -File scripts\check_release_payload.ps1 -BuildDir build`。

## 安装与共存

dev 包与稳定版**共用同一套身份**：AppId、安装目录、注册表项、AppUserModelID、单实例互斥体、`%LOCALAPPDATA%\Pulse` 数据目录。安装 dev 包会静默卸载并结束稳定版，随后复用同一份设置与会话。
