# 文件哈希与解除锁定

右键菜单里的两个文件工具：算校验和、去掉下载文件带来的“来自 Internet”标记。

## 行为

- **复制哈希**：选中单个普通文件（非目录、非虚拟位置）时，菜单提供“复制 SHA-256”“复制 MD5”。计算在后台线程流式进行（BCrypt，64 KB 分块），结果自动进剪贴板并弹一条提示；失败时报错而不是静默。
- **同一时间只有一个任务**：再次触发会替换上一个任务。被替换的 worker 即使已经越过取消检查，也不会把自己的摘要交给新任务——否则会出现 MD5 的值被标成 SHA-256。
- **退出时收尾**：窗口销毁时取消并 detach 哈希 worker；留着 joinable 线程会让进程退出变成 `std::terminate`。
- **解除锁定**：删除文件的 `Zone.Identifier` 备用数据流，等价于资源管理器属性页里的“解除锁定”。没有该标记时菜单项置灰；多选时逐个处理，失败的报错。

## 实现入口

- `src/app/file_hash.{h,cpp}`：`ComputeFileHash`（BCrypt）、`HasZoneIdentifier` / `RemoveZoneIdentifier`、`StartFileHashJob` / `CancelFileHashJob` / `TakeFileHashResult`。
- 菜单与命令：`src/app/app_commands.cpp`（`CmdHashSha256` / `CmdHashMd5` / `CmdUnblock`）；完成消息 `WM_FILE_HASH_DONE` 在 `src/app/app_main.cpp` 处理。
- 剪贴板复用 `ops::WriteClipboardText`（`src/ops/clipboard.cpp`，含剪贴板被占用时的重试）。
- 用例：`PULSE_SELFTEST_CASE=file-hash`、`pulse_ops_test`。
