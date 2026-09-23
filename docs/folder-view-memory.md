# 每个文件夹的视图记忆

Explorer 记得每个文件夹上次的查看方式和列宽，Pulse 之前对所有文件夹只有一个答案。现在视图模式、排序方式和明细列宽随文件夹一起记住，再次进入时恢复。

## 行为

- **记住什么**：视图模式、排序列与方向、明细模式的三个列边界。
- **什么时候记**：标签页停在一个真实文件夹上（导航、切换、重命名后的刷新），以及一次列宽拖动结束时。
- **什么时候用**：标签页进入文件夹时套用它；没有记忆就沿用当前标签页的设置。
- **只记真实文件夹**：虚拟位置（搜索结果、回收站、标签页）不进记忆，列边界也不参与。
- **键与上限**：路径经 `NormalizePath` 规范化（`C:\Temp` 与 `c:\temp` 是同一个），最多 500 个，超出按最近访问时间淘汰。`used` 只是访问时间，不算视图的一部分——只有它不同的两条算同一个视图。
- **多窗口**：保存前先读回磁盘，把另一个窗口新加或改过的条目并进来。本窗口没动过的条目以磁盘副本为准（视图取磁盘的，访问时间取两者较新的）；本窗口改过的以本窗口为准；另一个窗口新增的条目一并保留。
- **存储**：`%LOCALAPPDATA%\Pulse\folder-views.json`，形如 `{"version":1,"folder_views":[{"path","view","sort","direction","cols","used"}]}`。`cols` 与 session.json 的 `cols` 是同一种表示（万分比整数），由 `src/common/scaled_edges.h` 统一读写。

## 实现入口

- 存储与合并：`src/app/folder_views.{h,cpp}`（`FolderViewStore::Load/Find/Note/Save`、`SameView`、`TrimOldest`）。
- 记录与套用：`RememberFolderView` / `ApplyFolderView`，调用点见 `app_navigation.cpp`、`app_commands.cpp`、`app_input.cpp`。
- 用例：`PULSE_SELFTEST_CASE=folder-views`（覆盖多窗口合并与淘汰顺序）。
