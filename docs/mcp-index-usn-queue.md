# USN 原始队列容量修复与定向验证

日期：2026-09-18。最终验证时间：18:05:54 +08:00。

## 结论与交付边界

已修复源码并完成受影响的 Release 生产目标构建；最终定向测试 **50 PASS / 0 FAIL**，另有原始 JSON 解析检查通过。此次没有生成安装包、覆盖安装目录或重启服务。

验证结束时，已安装的 `PulseIndex` 仍为 **Running / PID 25024**，主程序和索引程序哈希仍与 memory2 安装包一致。当前运行的服务尚未使用本轮修复，不能把隔离测试收益直接当作实机内存降幅。

## 问题与修复

此前 `UsnStream` 每次读取创建 256 KiB vector，收到小包后仅 `resize(received)`，随后移动到队列。容量仍为 256 KiB，而 64 MiB 限制只累计有效长度，导致小包严重放大保留内存。

本轮改动：

- 每个读取线程复用一个 256 KiB I/O 缓冲；队列只复制收到的有效包内容，不接管读取缓冲。
- 独立 `UsnPacketQueue` 使用链式节点；预算计入实际 vector capacity 与节点对象大小，移除未计费的 deque 存储及其残留容量。
- 每卷预算仍为 64 MiB，但现在约束的是上述计费字节，而不是逻辑包长。它不是整个进程、所有卷之和或分配器实际提交内存的硬上限。
- 保留 FIFO、包内完整性、原有 1 MiB 批次的整包越界行为及剩余数据唤醒。队列错误保持首个错误，出错后不继续消费或悄悄推进游标。
- 长队列逐节点销毁，避免 `unique_ptr` 链递归释放耗尽线程栈。
- 停止或等待失败时，先 `CancelIoEx`，再等待 `GetOverlappedResult(TRUE)` 完成，之后才复用或释放 buffer / OVERLAPPED。等待失败保留原始错误码。
- 读取缓冲或入队分配失败转为 `ERROR_NOT_ENOUGH_MEMORY` 并唤醒消费者；完成事件创建失败关闭卷句柄。
- 在既有低频内存诊断中加入 USN 队列当前量与各流峰值之和；没有新增采样线程、改变历史保留量或调用工作集裁剪。

预算含节点后，极端积压可能比原先仅按逻辑长度计费更早触发原有溢出失败路径；这是收紧无效预算所需的行为，不是静默丢弃事件。

## 隔离内存回归

Windows x64 / MSVC，257 个包，每包 160 B。旧行为作为独立参考与新队列比较。

| 指标 | 字节 | 说明 |
|---|---:|---|
| 有效包内容总量 | 41,120 | 含各包 8 B 下一游标头 |
| 旧队列 vector capacity 合计 | 67,371,008 | 64.25 MiB，逻辑字节预算却仅看到 41,120 B |
| 新队列 vector capacity 合计 | 41,120 | 约 40.16 KiB |
| 新队列 capacity + 节点对象 | 49,344 | 普通 C++ 分配计数器实测与计费一致 |
| 可复用 I/O 缓冲 | 262,144 | 每个活跃读取线程一个 |
| 新计费队列 + 一个 I/O 缓冲 | 311,488 | 约 304.19 KiB / 0.297 MiB |

输入缓冲覆写后，输出数据仍与旧队列逐字节相等，最终游标一致。排空后当前计费字节归零，峰值仍保留。

上述容量不含分配器记账、内部取整、队列/线程对象、消费者输出缓冲及其他子系统。它证明容量放大已在此夹具中消除，不证明工作集或私有提交会等额减少，也不归因 memory2 实机观察中的全部短时尖峰。

## 验证结果

最终批次均退出 0，原始输出保存在 `bench_data/usn-queue-fix-20260918/`。

| 检查 | PASS | FAIL | 范围 |
|---|---:|---:|---|
| `pulse_usn_packet_queue_test` | 23 | 0 | 小包容量、所有权、FIFO、游标、整包批次、精确预算、粘滞错误、异常长度、分配失败、20 万节点销毁、并发、唤醒、重叠 I/O 生命周期、JSON 字段 |
| `pulse_index_engine_test --usn-only` | 10 | 0 | 隔离夹具调用真实 CatchUpVolume；创建/重命名/删除顺序及路径、游标提交、头部包、溢出错误传播、两条流统计聚合与清零 |
| `pulse_index_engine_test --feed-only` | 16 | 0 | 分页、保留缺口、代际切换、历史不丢失、既有内存/时序日志 |
| `pulse_change_feed_memory_test --probe` | 1 | 0 | 5000 次进程内存采样，0 次普通 C++ 分配，3.111 ms |
| **合计** | **50** | **0** | 不包含首轮重复执行的 23 PASS |

额外用 PowerShell 解析未经过终端换行的原始 JSON：USN 字段值正确，原有 6 个阶段采样点保留。采样函数的 0 分配不包含 JSON 字符串构造或队列操作。

重叠 I/O 测试使用本测试进程创建的独立命名管道，覆盖：正常完成、停止取消、WAIT_FAILED、停止与完成同时到达、管道断开后的失败完成。每种路径返回后均确认操作不再为 IO_INCOMPLETE。这是通用 Windows 重叠 I/O 生命周期验证，不是 NTFS 日志端到端验证。

Engine 溢出用例注入流错误以验证消费端不推进游标；实际预算溢出机制在队列测试中以缩小预算验证。未启动真实卷读取器或安装后的新服务。

## 构建、诊断与变更范围

构建：Release / Ninja / MSVC 14.51.36231，`PULSE_WITH_SELFTEST=OFF`，并行度 2。

```text
cmake --build build --target pulse_usn_packet_queue_test pulse_index_engine_test pulse_change_feed_memory_test pulse_index pulse --parallel 2
```

该构建退出 0，输出无编译/链接警告或错误；没有运行全量构建脚本或全量自测。`git diff --check` 未报告空白错误；Git 提示部分工作副本文件未来会做 LF→CRLF 转换，这不是编译警告。

本轮源码范围：`src/index/usn_stream.h`、新增 `usn_packet_queue.h` / `usn_read_wait.h`、`index_memory_probe.h`、`index_engine.cpp` 的统计聚合、独立队列测试、新增 Engine 定向用例及 CMake 测试目标。此前已有修改保留。

新增 `memory.retained` 字段：

- `usn_streams`、`usn_queue_packets`、`usn_queue_payload_bytes`。
- `usn_queue_capacity_bytes`、`usn_queue_charged_bytes`。
- `usn_sum_stream_peak_capacity_bytes`、`usn_sum_stream_peak_charged_bytes`。
- `usn_queue_overflows`。

`payload_bytes` 包含游标头；`charged_bytes` 是 capacity 加节点对象。当前统计逐流取样，不是跨卷原子快照；峰值字段是当前存活流各自生命周期最大值之和，**不是同一时刻的全局峰值**。流被销毁/重建后，其峰值和溢出次数不再计入；原有 journal 失败日志继续记录错误。

本轮构建产物 SHA-256：

- `build/pulse.exe`：`7AFCE39CBBC0E808E821ADC8FE5E7282D6EFE69C6D6F8F15AAC0EB3A4D1D9C11`
- `build/Pulse.Index.exe`：`6D9317D1ED55416A9C920C31D730BAD8285CD7430FAAC08EFFF5067B32B74B80`

验证结束时的已安装产物 SHA-256（仍为 memory2）：

- 主程序：`DFC9BBA4DC177375701431B0B57748C71DD10B7FF5876FC2CD2A9045BA726E3F`
- 索引服务：`906E743BDF652F6D66A9B4A4B958A8992513766C5ADF9DC509470232BFFA9A52`

## 后续

当前可进入单独打包阶段，由用户安装后，再比较 USN 队列峰值、工作集、私有提交与 CPU。此次没有进行修复版实机 A/B、长期泄漏判断或生产 NTFS 日志停止/恢复验证，不能宣称已经消除所有运行时内存尖峰。

执行记录：首轮 `cmd_289a9543a3554d7c30b0cc97ad6b4d3569fea77f96d263fa`；最终构建 `cmd_ca4e9916f5492ab22a5403931bf1d5b6d79f8f96245b6da0`；最终验证 `cmd_851518b484e2c031104114d88b537b260015b8603ea50ffa`。原始汇总为 `bench_data/usn-queue-fix-20260918/results.json`。
