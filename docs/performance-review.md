# 性能优化空间：源码审查

本文保留优化前的审查基线；后续实现与编译记录见
[性能优化实现](performance-optimization.md)。

审查日期：2026-09-28。结论来自当时的生产源码与 `Plan.md`、`DevelopPlan.md`，
没有运行程序、测试、benchmark 或 profiler，也没有修改运行时实现。
下述优先级是基于重复工作与规模上限的判断，不是已测得的瓶颈排名或性能收益。

| 优先级 | 路径与源码证据 | 优化方向与约束 |
| --- | --- | --- |
| 高 | `src/ui/receive_view_model.cpp` 的 `ReceiveCoordinates` 每次构造都为全部记录 reserve 并逐条过滤；`src/ui/tui.cpp` 的 Custom 事件归一化与 `records_element()` 分别调用 `view_indices()` | 在记录集合或过滤器变化时更新坐标；同一轮事件与渲染复用结果。必须处理追加、淘汰、清空、搜索方向变化，并保留稳定 record ID 和“过滤先于 viewport 截断”的语义。 |
| 高 | `src/ui/tui.cpp` 的 ticker 每 25 ms 投递 Custom；`records_element()` 每次渲染最多重新格式化 200 条记录，不以终端实际高度限制格式化数量 | 将状态推进与重绘区分，按变化标记重绘；按可见高度构造记录行，并对不可变记录的安全显示文本使用有界缓存。保留 RX idle 分帧、notice 到期、completion、退出 deadline 与文本光标响应。 |
| 中 | `src/app/session_records.cpp` 的 `search()` 在 UI 调用链上扫描所有保留记录，对每条候选调用 `project_record()`，只用 10000 个匹配结果限制返回数量 | 使用有界增量搜索，或复用受预算控制的显示 projection。无匹配查询仍需扫描整个集合；搜索切换、记录淘汰与清空必须使旧结果失效。 |
| 中 | `src/logging/session_writer.cpp` 每条记录单独编码并调用 `append_line()`；`src/logging/linux_session_files.cpp` 的普通追加先刷新 inventory，随后 `ensure_capacity()` 再刷新一次，最终逐行 `write_all()` | 优先评估重复目录状态检查，再考虑有界批量编码/写入。不得移除目录变更监视、身份复核、配额、未知文件保护或 barrier 顺序；必须明确部分写与失败时的记录归属。 |

默认可见上限为 100000 条（`include/lazycom/config/types.hpp`）。在达到该条数、
每个定时事件均被处理且每轮重绘一次的条件下，仅两次坐标构造就约有
`100000 × 2 × 40 = 8000000` 次记录过滤/秒。64 位平台上，每个完整索引向量
的元素存储约 0.8 MB。这是代码路径的规模估算，不是实测 CPU、分配吞吐或 RSS。
搜索可额外触发坐标构造；worker 事件和事件合并会改变实际频率。

记录显示目前最多处理 200 行，已经避免构建全部记录的 Element 树；优化应进一步
减少不可见行和未变化记录的重复格式化。TXT/HEX/MIXED 的安全投影可能显著大于原始
payload，因此缓存必须有容量与字节上限，不能为全部历史记录永久保存三种显示文本。
搜索的 10000 个结果上限限制结果容器，不限制无匹配时的总扫描工作。

日志目录已有 inventory 缓存，并非每条记录都扫描整个目录。即使命中缓存，
`refresh_inventory()` 仍会读取目录状态与 inotify；普通追加路径的重复调用值得关注。
当前 `flush()` 不进行用户态批量写入，`flush_batch_bytes` 不能直接等同于 write 系统调用
的批大小。改变这里需要保留现有文件安全和 completion 契约。

串口 owner 已采用 `ppoll`/`eventfd`，UI 唤醒已有 pending 合并，UI 与日志已共享不可变
记录 payload；没有源码依据要求改用无锁队列或扩大内存上限。生产全局 128 MiB token
仍未完全接入，新增索引、显示缓存和批量缓冲必须纳入既有类别预算。

建议先处理接收坐标复用及重绘范围，再评估搜索与日志批量处理。具体收益与 2 Mbaud、
UI p95、8 小时 RSS 指标仍需后续明确授权的性能测量；本次不据此宣称已达标。
