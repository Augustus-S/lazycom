# LazyCom AI 开发指南

本文件适用于整个仓库，定义 AI 辅助开发的默认约束。子目录中的更具体
`AGENTS.md` 可以为对应子树补充或覆盖这些规则。

## 1. 项目概览

LazyCom 是使用 C++20 开发的 Linux 优先 TUI 串口助手。项目使用：

- FTXUI：终端界面、输入和主循环。
- libserialport：串口枚举、配置和访问。
- toml++：配置文件解析和序列化。
- nlohmann/json：NDJSON 会话日志。
- tl::expected：通过项目的 `Result<T>` 别名返回显式错误。
- Catch2：单元和集成测试。

当前支持基线：

- Ubuntu 24.04 同代或更新的现代 Linux。
- CMake 3.28+ 和 Ninja。
- GCC 13+，或 Clang 18+ 及受支持的标准库。
- C++20，禁用编译器语言扩展。
- 默认使用仓库内固定版本依赖，配置和构建不得强制联网。

不要从旧报告复制测试数量或完成状态。当前源码、CMake 测试发现结果和本次实际
执行记录才是当前测试数量及验证状态的依据。

## 2. 事实来源

修改行为前必须阅读对应文档章节：

- `Plan.md` 是产品行为、交互语义、状态名称、限制和验收标准的事实来源。
- `DevelopPlan.md` 是架构、模块边界、并发、所有权、构建策略和质量门禁的事实来源。
- `CMakeLists.txt`、`CMakePresets.json` 和 `cmake/` 中的文件描述当前实际构建。
- `Fix.md`、审查报告和 `docs/` 中的文件提供历史背景和验证证据，但不能覆盖
  `Plan.md` 或 `DevelopPlan.md`。

如果 `Plan.md` 与 `DevelopPlan.md` 在产品行为上冲突，停止实现并请求设计决策，
不得在代码中静默选择一种解释。如果实现与文档不一致，编辑前先判断任务属于修复
实现还是有意修改规范。

## 3. 仓库结构

- `include/lazycom/`：按模块组织的项目头文件。
- `src/`：生产实现。
- `tests/unit/`：确定性的单元和组件测试。
- `tests/integration/`：文件系统、PTY、backend 和 Linux 集成测试。
- `cmake/`：项目选项、依赖、sanitizer 和 hardening 配置。
- `include/dependencies/`：固定版本的 header-only 依赖及元数据。
- `third_party/`：固定版本的源码依赖，按 vendored code 管理。
- `docs/`：实现报告和辅助文档。
- `build/`：生成的构建产物，禁止手工编辑。

主要生产 targets 为：

- `lazycom_base`
- `lazycom_core`
- `lazycom_config`
- `lazycom_logging`
- `lazycom_data_path`
- `lazycom_scheduler`
- `lazycom_diagnostics`
- `lazycom_serial`
- `lazycom_app`
- `lazycom_ui`
- `lazycom`

保持依赖指向更低层模块：

- 纯 model 和 encoding 代码不得依赖 FTXUI、libserialport 或裸 Linux fd。
- UI 不得实现串口所有权、文件安全策略或协议解析。
- libserialport 类型不得离开 serial 模块。
- Catch2 不得成为生产 target 的直接或传递依赖。
- 业务模块只能使用 diagnostics wrapper，不得直接使用 spdlog 类型或宏。

## 4. 修改原则

编辑前：

1. 阅读当前实现、相关测试和对应设计章节，不能只根据名称推断行为。
2. 检查工作树，保留用户或其他 agent 的无关修改。
3. 优先采用最小且完整的修复，任务未要求时避免大范围重构。
4. 修改异步或 I/O 路径前，明确失败路径、生命周期、持久化兼容性和资源上限。

实现过程中：

- 简单且仅使用一次的逻辑保持局部，不为缩短一个调用点创建新抽象。
- 没有持久数据、已发布 API 或明确用户需求时，不添加兼容层。现有配置和日志格式
  属于真实兼容边界。
- 保持稳定错误码和已发布 schema 的语义，不得复用已有错误码表达新含义。
- 实现与测试同步修改。可测试的行为修复应包含能覆盖原失败模式的回归测试。
- 有意改变产品行为时更新 `Plan.md`；改变工程契约时更新 `DevelopPlan.md`。
- 不修改历史报告来伪造新的验证证据。

## 5. C++ 与错误处理

- 使用 C++20，并遵循相邻源码的现有风格。
- 使用项目值类型、强类型 ID、枚举和 `Result<T>`，避免临时字符串协议、整数哨兵
  或用异常表示预期失败。
- 异常仅用于抛异常的第三方适配、标准库资源失败、线程入口和进程顶层边界。
- 构造失败不能留下可观察的半有效对象；正常可失败的构造使用返回 `Result<T>` 的
  factory。
- 异常对象不得跨线程传播；跨线程错误使用有界值类型。
- operation metadata、input mode、时间、session ID 和 generation 必须在事实发生处
  捕获，不得稍后从可变 UI 或 Application 状态推导。
- deadline 和 elapsed time 使用 `std::chrono::steady_clock`；墙上时钟仅用于用户可见
  UTC 时间和持久事件时间。
- 注释应少而精，只解释不明显的不变量、安全原因或生命周期约束。

正常 presets 启用项目警告并按错误处理。新增代码必须同时在 GCC 和 Clang 下保持
无警告。

## 6. 并发与所有权

- serial owner 是唯一可以持有或持续访问活动串口的线程。UI、日志、scanner 和
  其他 worker 不得取得 `sp_port*` 或串口 fd。
- POSIX 资源必须使用 RAII；不得跨模块传递具有所有权的裸 fd。
- 使用 `std::jthread`、`std::stop_token`、有界队列和显式唤醒。
- 未经设计批准，不引入 `std::async`、detached thread、通用线程池、无锁容器或
  coroutine。
- 每个被接受的异步 operation 必须在 admission 前预留可靠 completion 路径，并且
  最终产生一个可匹配的 terminal outcome。
- operation ID、connection generation、session ID 和 task generation 是不同概念，
  不得互相代替。
- 迟到或重复 completion 必须安全且幂等。迟到事件可以释放旧资源，但不能修改新
  session。
- 队列过载必须有界且可观察，不得阻塞 serial owner，也不能静默丢失必要 completion。
- stop 路径必须有显式 deadline，禁止新增无界 join 或可能永久等待的清理路径。
- worker 入口必须保留文档规定的异常边界。资源耗尽或不变量破坏使用固定
  FatalSignal 路径，不先分配包含动态文本的 Error。

## 7. 安全与资源边界

串口字节、设备描述、配置值、日志内容、路径和粘贴文本均视为不可信输入。

- 原始数据与终端安全显示 projection 必须分离，禁止向终端直接输出不可信控制字节。
- 保持 strict UTF-8、输入字节、队列、记录数和文件大小限制；尽可能在分配或提交前
  验证。
- 不得削弱现有分类内存上限。生产 Application 未端到端使用全局预算 token 前，
  不得宣称全局运行时预算已经闭环。
- TOML 语法、schema 或资源组合无效时必须拒绝整个候选，并保护已有文件不被覆盖。
  禁止把默认值合并进无效用户文档后回写。
- 配置 serializer 的输出必须能通过同一正式 parser round-trip。
- 保持安全文件规则：当前用户所有、私有权限、no-follow、基于目录 fd 的操作、文件
  上限和原子提交语义。
- `NotCommitted`、`Committed` 和 `CommittedDurabilityUnknown` 必须保持为不同结果。
- 不得删除未知、损坏、活动或身份不匹配的日志文件。
- 应用不得执行 `sudo`、修改设备权限、调用 `usermod`、修改 udev 规则或进行其他
  提权操作。应报告清晰权限错误，由用户或管理员处理系统配置。
- 未经规范明确允许，不得向内部 diagnostics 写入原始 RX/TX payload、发送草稿、
  quick-send 内容、凭据或完整用户路径。

## 8. TUI 约束

- 保持 connection state、interaction state 和 overlay 相互独立。
- F1 是唯一无条件应用快捷键。文本编辑器、模态 overlay 和 ReceiveBrowse 命令优先于
  Normal 快捷键。
- 鼠标操作必须调用与键盘相同的 command 和 guard。
- 颜色不得成为唯一状态标识，必须保留文本、标记、边框或其他装饰。
- transparent 模式的整个 Element 树不得引入背景色。
- 状态栏使用结构化 FTXUI Element，不先拼成只能整体着色的字符串。
- Receive cursor 使用稳定 record ID 和过滤后的坐标；过滤必须发生在 viewport 截断前。
- SYS 和 ERR 始终是安全文本；RX/TX 显示模式相互独立且不改变存储字节。

## 9. 依赖与 Vendored Code

- 默认使用仓库内固定版本依赖，不得替换为任意系统版本或添加 configure-time 下载。
- 应用功能修复不得修改 `third_party/` 或 `include/dependencies/`。
- 依赖升级作为独立变更处理，必须固定精确版本，保留或更新许可证，同步 dependency
  lock、checksum 和本地补丁说明，并运行完整相关验证矩阵。
- libserialport 保持 shared library，不得静态合并到应用。
- 未经产品和架构批准，不引入 Boost、Asio、协议库、脚本 runtime、plugin loader、
  iconv 或 libudev。

## 10. 构建与测试

所有命令从仓库根目录执行。

默认开发构建和测试：

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

迭代时运行定向测试：

```bash
ctest --preset gcc-debug -R "<test-name-regex>"
```

其他支持的 presets：

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug

cmake --preset gcc-release
cmake --build --preset gcc-release
ctest --preset gcc-release

cmake --preset gcc-debug-no-diagnostics
cmake --build --preset gcc-debug-no-diagnostics
ctest --preset gcc-debug-no-diagnostics

cmake --preset gcc-asan-ubsan
cmake --build --preset gcc-asan-ubsan
ctest --preset gcc-asan-ubsan
```

`gcc-tsan` configure/build/test preset 已存在。TSan 必须与 ASan/UBSan 分开运行，并要求
当前环境提供可用 libtsan：

```bash
cmake --preset gcc-tsan
cmake --build --preset gcc-tsan
ctest --preset gcc-tsan
```

验证策略：

- 仅文档修改：检查引用、路径、命令和内部一致性，通常不需要重新构建。
- 局部纯逻辑修改：构建 GCC Debug，运行定向测试，再运行完整 GCC Debug 测试。
- 跨模块、持久化、串口、日志、并发、CMake、依赖或发布敏感修改：除非环境阻塞，
  运行 GCC Debug、Clang Debug、GCC Release、no-diagnostics 和 ASan/UBSan。
- 并发修改在环境支持时额外运行 TSan。
- 硬件行为必须使用真实设备验证；PTY 测试不能证明 USB-UART 驱动、权限、拔插或
  高波特率行为。

未针对当前修改实际执行的 preset、sanitizer、硬件矩阵、压力测试或长时测试，不得
报告为通过。跳过或阻塞的验证必须明确说明。

## 11. 格式化与仓库卫生

- 只格式化本次修改的 C++ 文件，例如：

  ```bash
  clang-format -i <touched-files.cpp> <touched-files.hpp>
  ```

- 禁止手工编辑 `build/`、依赖生成产物或生成的 `compile_commands.json`。
- 不进行顺带格式化或无关清理。
- 保留工作树和暂存区中的无关修改。不得 reset、checkout、覆盖或删除非本次创建的
  工作。
- 未经明确要求，不执行 commit、amend、rebase、push、force-push 或修改 Git 配置。
- 不向仓库添加 secret、机器相关绝对路径、构建产物、测试日志或本地串口配置。

## 12. 完成标准

声明任务完成前：

1. 确认实现符合相关产品和工程契约。
2. 检查改动路径的正常、失败、取消、边界和 shutdown 行为。
3. 在可行时增加或更新没有该修复就会失败的测试。
4. 格式化本次修改的 C++ 文件，并按风险运行相应验证。
5. 审查最终修改，排除意外编辑、不安全兼容变更、过期文档和无界资源路径。
6. 报告修改内容、实际测试，以及剩余硬件、性能、sanitizer 或环境限制。
