# 测试去重与辅助代码精简

日期：2026-09-20。此次修改仅涉及测试、测试构建设置和验证记录，生产代码未变。

统计基线为此次修改前的工作树。代码量统计 `tests/` 下 `.cpp/.hpp` 的物理行数，包含空行、注释和新增公共辅助代码，不含 CMake、文档、依赖及构建产物。

| 指标 | 修改前 | 修改后 | 减少 |
| --- | ---: | ---: | ---: |
| 测试代码行数 | 5,690 | 5,576 | 114（2.0%） |
| 完整 CTest 发现项 | 106 | 105 | 1 |
| 快速 CTest 发现项 | 103 | 102 | 1 |
| Catch2 SECTION 声明 | 68 | 66 | 2 |

删除重复场景后，保留覆盖的对应关系如下：

| 精简内容 | 保留覆盖 |
| --- | --- |
| 裸 PTY 普通收发与 HUP 两个 SECTION | 真实 serial owner 使用相同二进制字节验证收发，并验证 HUP 的错误和 Cleanup |
| 独立 PersistenceWorker 正常保存冒烟用例 | Application 通过真实文件后端保存、关闭并重新加载状态；safe-file 测试检查权限和原子提交 |
| Application fake backend 未使用的写失败开关 | 保留部分写后失败、错误码和 operation ID 测试 |
| 等待成功后重复验证同一条件的断言 | 等待谓词本身仍作为断言，后续数据和生命周期断言保留 |

PTY、fd RAII、完成事件收集及普通 deadline 等待统一放在 `tests/support/`。普通等待只观察后台状态，Application 的 `tick_until` 显式推进主线程事件；取消、关闭和尾数据测试保留原有时序约束。完成事件仍要求数量精确匹配，service 仍先于 PTY master 销毁。

本次实际执行了以下配置、构建和完整测试：

| Preset | 结果 |
| --- | --- |
| `gcc-debug` | 105/105 通过 |
| `clang-debug` | 105/105 通过 |
| `gcc-release` | 105/105 通过 |
| `gcc-debug-no-diagnostics` | 105/105 通过 |
| `gcc-asan-ubsan` | 105/105 通过 |
| `gcc-tsan` | 105/105 通过 |

`gcc-debug-fast` 另通过 102/102；完整矩阵包含两个 extended 测试和 bundled libserialport 构建回归。ASan/UBSan 与 TSan 分开运行，未报告 sanitizer 错误。ASan 构建和测试在沙箱外运行以兼容 LeakSanitizer，未关闭泄漏检查。

未执行真实 USB-UART、8 小时压力或硬件性能验证。既有历史报告保持原样。
