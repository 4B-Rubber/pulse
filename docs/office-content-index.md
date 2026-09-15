# Office 与 PDF 正文索引

Pulse 的内容索引和文件夹内容扫描共用文档读取入口。普通文本仍使用原有编码读取器；文档解析仅在需要提取内容时启动 `Pulse.Document.exe`，已有全文索引的查询不加载解析器。

| 格式 | 提取方式 | 内容范围 |
| --- | --- | --- |
| DOCX | Windows OPC ZIP 包读取 + XmlLite | 正文、表格、页眉页脚、批注、脚注和尾注 |
| XLSX | Windows OPC ZIP 包读取 + XmlLite | 工作表名、实际使用的共享字符串、内联文字、单元格原始数值、公式缓存值、传统批注 |
| PPTX | Windows OPC ZIP 包读取 + XmlLite | 幻灯片文字、表格、备注、传统批注 |
| DOC、XLS、PPT、RTF | 本机 IFilter | 由已安装且与程序同位数的过滤器决定 |
| PDF | 本机 IFilter，MTA 线程及 IPersistStream 加载 | 文字层；无可提取文字时报告不支持，不冒充完整无匹配 |

新版 Office 不依赖 Office、Java、Tika 或新安装的运行环境。OPC 和 XmlLite 来自 Windows。旧版格式缺少过滤器时返回不支持，不能保证所有机器开箱可用。

## 进程与预算

- 按需文件夹搜索通过 `DocumentReadSession` 复用任务专属提取进程，任务返回时立即退出；后台索引保留一个串行、空闲 30 秒退出的提取进程。
- 单次请求等待上限 30 秒，取消或超时会结束子进程，下次请求重新启动。PDF 使用 MTA，旧 Office 保留 STA，避免改变旧过滤器线程模型。
- Job Object 限制子进程提交内存为 256 MiB，并禁止再创建子进程；宿主退出时一并结束子进程。
- 文档输入上限为独立配置和 512 MiB 中的较小值；普通文本仍使用原有 64 MiB 默认上限。文件检查在子进程内完成，解析期间保持只读句柄，禁止写入或替换被检查的文件。
- OOXML 相关部件的累计解压读取上限为 64 MiB，输出上限为 8 Mi 个 UTF-16 字符；同时限制 XML 深度、包部件数量和共享字符串表。
- IFilter 显式请求索引属性，避免本机旧 Office 过滤器返回零正文块。文档缓存带提取器版本，解析行为升级后自动重新提取，保留原有文件身份。
- 不将包解压到磁盘，不处理 DTD，不执行宏、脚本或外部链接。失败不提交不完整正文；现有索引状态会记录失败或跳过。

## 内容边界

这属于正文索引，不是 Office 页面渲染。Excel 数字保留底层数值，日期/数字格式不转换为显示文字；公式只取文件保存的缓存结果，不计算公式。扫描图片、嵌入对象、现代线程批注、加密文档以及宏专用文件扩展名不在本次内置支持范围。文档中的段落/单元格分隔用于搜索摘要，不等价于 Office 显示页码或行号。

新增支持的 Office/PDF 文件会在既有内容文件夹的下一次协调扫描中纳入索引；已提取文件继续使用原有大小、修改时间与文件版本检查，只有发生变化才重新提取。

PDF 同样进入内容索引格式集合。查询已缓存正文不启动 PDF 解析器；系统缺少兼容过滤器时报告不支持。当前不做 OCR，也不提供 PDF 页码／版面坐标；已有 OCR 文字层的结果仍取决于过滤器和文字层质量。

## 定向验证

```powershell
cmake --build build --target pulse_office_ooxml_test pulse_office_index_test pulse
.\build\pulse_office_ooxml_test.exe
.\build\pulse_office_index_test.exe
.\build\pulse_office_index_test.exe --worker-limits
.\build\pulse_office_index_test.exe --pdf-task
.\build\pulse_office_index_test.exe --legacy-filter
.\build\pulse_office_index_test.exe --document-budget
```

原生解析测试覆盖中文跨文本段拼接、表格、共享字符串引用、缓存值、备注、损坏包、DTD 和大小预算。集成测试覆盖独立进程复用、取消/恢复、全文索引查询、文件变更，以及故意挂起测试子进程后的超时恢复和空闲退出。`--pdf-task` 定向覆盖中文／英文 PDF、PDF 与 Office 交替解析、无文字层／损坏文件、预算、任务结束释放和缓存检索。1.0.29 在本机验证了27份有效XLS与5份DOC的提取和匹配，其他机器仍取决于已安装过滤器。加密拒绝测试目前使用复合文件签名样本，未使用真实加密 Office 文档。

实现参考：[Windows Packaging API](https://learn.microsoft.com/en-us/windows/win32/opc/packaging-api-overview)、[IFilter 初始化](https://learn.microsoft.com/en-us/windows/win32/api/filter/nf-filter-ifilter-init)、[IFilter 文本块](https://learn.microsoft.com/en-us/windows/win32/api/filter/nf-filter-ifilter-gettext)。
