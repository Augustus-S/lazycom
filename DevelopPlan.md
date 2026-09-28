# LazyCom C++ 开发与构建计划

## 1. 文档定位

- 本文档定义 LazyCom 的工程实现、构建系统、模块边界、并发接口、依赖管理、测试门禁和版本演进路线。
- `Plan.md` 是产品行为、交互、状态语义和验收标准的事实来源；本文档不得改变其中已确认的产品语义。
- 工具链、依赖最低版本、模块边界和实现方式以本文档为准，并与 `Plan.md` 已确认的 Ubuntu 24.04 和 libserialport 0.1.2 基线保持一致。
- 两份文档在产品行为上冲突时，先停止实现并完成设计审核，不在代码中隐式选择一种行为。
- 首版目标平台为现代 Linux，语言标准为 C++20。
- 首版只提供纯字节串口助手；工业协议、脚本和插件仅是非规范性研究候选，必须先在 `Plan.md` 完成产品审核才可进入实施路线。
- 当前工程状态：阶段 1 至阶段 6 的主要功能和本轮安全加固已落地；阶段 7 的真实 USB-UART 矩阵、8 小时性能/RSS 验收、完整 diagnostics worker 和发布诊断文档仍待完成。
- 生产记录路径已通过 `SessionRecords` 接入 `SessionSequencer` 和共享记录 token，UI 与日志持有同一不可变记录。记录及日志引用的预算已接入；其他运行时类别尚未全部接入，因此 128 MiB 全局 token 仍未闭环，各类别、队列、输入和文件尺寸硬上限继续生效。
- diagnostics 当前只有编译开关、spdlog 链接探针、`emergency_write()` 与 terminate/FATAL 最小路径，不具备本计划第 7.6、10.5 至 10.8 节描述的完整内部队列、worker、安全 sink、轮换和脱敏能力。

测试代码、测试项、Catch2、test presets 与打包验证脚本由独立 `../lazycom-test`
仓库维护。本仓库只构建生产 targets；未经用户明确指令，不新增或运行测试。
本文后续的测试、性能 spike 与阶段验收要求是验收依据，不是自动执行授权。
历史报告保持原样，不作为迁移后的当前验证结果。

## 2. 已确认的工程决策

| 项目 | 决策 |
| --- | --- |
| 语言 | C++20 |
| 构建系统 | CMake 3.28 或更高版本 |
| 最低编译器 | GCC 13 或 Clang 18 |
| TUI | FTXUI，固定已验证版本 |
| 串口库 | 固定验证 libserialport 0.1.2 shared library，允许显式替换为已验证的系统 0.1.2+ |
| 串口等待 | Linux `ppoll()` + `eventfd` |
| 配置 | toml++ |
| JSON | nlohmann/json |
| 错误返回 | `tl::expected`，通过项目 `Result<T>` 别名隔离 |
| 测试 | 独立 `../lazycom-test` 仓库中的 Catch2 3.x，仅按明确指令执行 |
| Header-only 依赖 | 固定源码放入 `include/dependencies/` |
| 内部诊断日志 | spdlog diagnostics facade/链接探针已编译；完整 worker 与安全 sink 待阶段 7，运行时默认关闭 |
| 线程 | `std::jthread`、`std::stop_token` 和有界队列 |
| 首版协议 | 纯字节，不加入 Modbus、SLIP、COBS、CRC 或插件运行时 |
| 异步模型 | 同步底层后端、异步服务命令与事件 |
| Asio | 首版不引入 |
| 插件 ABI | 首版不开放；后续是否提供尚未形成产品承诺 |
| 管理内存 | 记录及其 payload 与 UI/log 引用已接入共享预算；其他类别仍待闭环，RSS 目标为空载基线 +192 MiB |
| 会话日志 | NDJSON v1，UTF-8/base64 无损 payload |
| 连接状态 | 内部和 UI 统一为 Disconnected、Connecting、Connected、Disconnecting、Error |
| 会话日志状态 | 内部 Off、Waiting、Recording、Error；UI OFF、WAITING、REC、ERROR |
| Enter 发送草稿 | 成功提交后无条件清空；校验、未连接、队列或提交失败时保留，历史由 Alt+Up/Down 访问 |
| Receive 显示 | RX/TX 独立 TXT/HEX/MIXED；SYS/ERR 为 TXT 消息；四类独立可见 |

首版不依赖 C++20 coroutine、`std::async`、无锁容器或通用线程池。实现优先保证所有权清晰、可取消、有界和可测试。

## 3. 工具链与构建基线

### 3.1 编译器要求

- GCC 13+ 配合对应版本 libstdc++，或 Clang 18+ 配合受支持的 libstdc++。
- 配置阶段必须验证 `std::jthread`、`std::stop_token`、`std::span`、`std::byte` 和 `std::filesystem` 可用。
- 不依赖实现差异较大的 `std::format`、chrono 时区数据库或 C++23 `std::expected`。
- UTC 日志时间使用 `std::chrono::system_clock` 获取时间点，并通过经过测试的 Linux UTC 转换函数格式化。
- 单调 deadline 使用 `std::chrono::steady_clock`，不能使用墙上时钟。

### 3.2 CMake 基线

顶层构建必须设置：

```cmake
cmake_minimum_required(VERSION 3.28)

project(lazycom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

禁止在全局直接拼接不透明的编译参数。警告、sanitizer、覆盖率和 LTO 通过独立 interface target 或 CMake option 管理。

当前 CMake hardening 门禁：

- Release、RelWithDebInfo 和 MinSizeRel 通过独立 interface targets 应用 PIE、`-fstack-protector-strong`、`_FORTIFY_SOURCE=3`、RELRO、NOW 和 non-executable stack；每个编译/链接参数先经 CMake capability check。
- hardening 应用于全部 LazyCom 生产 targets；最终可执行文件额外应用链接 hardening。bundled libserialport 的 Release CFLAGS/LDFLAGS 同步启用经检测支持的 stack protector、FORTIFY、RELRO、NOW 和 noexecstack。
- sanitizer 组合在 configure 阶段互斥校验；TSan 不与 ASan/UBSan 同开，coverage 不与 LTO 同开。
- `CMakePresets.json` 已提供 `gcc-debug`、`clang-debug`、`gcc-release`、`gcc-debug-no-diagnostics`、`gcc-asan-ubsan` 和 `gcc-tsan` 的 configure/build presets；test presets 仅由 `../lazycom-test` 提供。

### 3.3 首版依赖

| 依赖 | 版本策略 | 链接方式 | 用途 | 许可关注 |
| --- | --- | --- | --- | --- |
| FTXUI | 固定已验证 tag/commit | CMake target | TUI、输入和主循环 | MIT |
| libserialport | 固定验证 0.1.2 | shared library | 枚举、配置和非阻塞 I/O | LGPL-3.0+ |
| toml++ | 固定 3.x tag | header-only target | TOML 解析和写入模型 | MIT |
| nlohmann/json | 固定 3.x tag | header-only target | NDJSON 编码和校验 | MIT |
| tl::expected | 固定 1.x tag | header-only target | C++20 `Result<T>` | CC0 |
| spdlog | 固定已验证 tag | 编译静态 target | 默认关闭的内部诊断日志 | MIT |
| Catch2（外部测试仓库） | 固定 3.x tag | 仅测试 target | 单元、性质和集成测试 | BSL-1.0 |
| Threads::Threads | 系统 | CMake imported target | C++ 线程运行时 | 系统 |

构建时工具：

- `pkg-config`：仅在显式系统 libserialport 模式下查找并验证版本。
- `socat`：可选，仅用于 PTY 集成测试。
- `gcov`/`llvm-cov`：可选覆盖率。
- ASan、UBSan、TSan：由编译器提供，分别运行。

### 3.4 依赖获取策略

- Ubuntu 24.04 系统仓库仅提供 libserialport 0.1.1，不能把系统 `pkg-config >= 0.1.2` 作为默认可满足前提。
- 默认离线依赖包固定携带 libserialport 0.1.2 源码并构建 shared library；允许通过 `pkg-config` 显式选择已经过 spike 和集成测试的系统 0.1.2+。
- 随附 libserialport 的 build/install 通过 GNU Make `--old-file` 使用预生成的 `aclocal.m4`、`configure`、`Makefile.in` 和 `config.h.in`，不因 checkout 时间戳重新运行 Autotools，也不改写 vendored 源码。普通构建不要求安装 Autoconf 或特定 Automake 版本；依赖升级必须提供匹配的预生成文件。
- libserialport 不静态并入主程序。发布包必须携带 LGPL 许可、对应源码、动态替换说明，并用受控 RUNPATH 或发行版依赖保证加载预期 shared library。
- nlohmann/json、toml++ 和 tl::expected 的已验证头文件固定放在 `include/dependencies/`，构建时不下载，也不替换为系统版本。
- `include/dependencies/DEPENDENCIES.lock` 记录每个 header-only 库的上游项目、精确版本、源归档 SHA-256 和导入日期。
- 每个 vendored 库必须保留原始许可证；升级时整体替换对应上游头文件并更新 lock，不能手工混合不同版本。
- CMake 通过独立 `lazycom_header_dependencies` interface target 将各依赖根目录标记为 `SYSTEM INTERFACE`，业务代码仍使用上游 include 名称，例如 `<nlohmann/json.hpp>`。
- `include/dependencies/` 只作为私有构建输入，不随未来 LazyCom SDK 安装，也不能被项目头文件通过相对路径引用。
- FTXUI 和 spdlog 使用仓库内固定源码；Catch2 连同许可证、版本锁和源码由 `../lazycom-test` 独立维护，生产构建不查找或构建 Catch2。
- spdlog 构建为独立静态 target，关闭其 examples、tests 和 benchmarks，使用其随库提供的 fmt 实现，不向其他模块暴露 fmt API。
- `LAZYCOM_BUILD_DIAGNOSTICS=OFF` 时仍生成同名 no-op facade target，保留稳定错误代码和 FATAL 所需的最小应急写入接口，但不获取或链接 spdlog。
- 所有 FetchContent 项目必须固定 tag 或 commit，不允许跟踪分支。
- 发布构建必须能够使用预先下载的源码包完成，不能强制在 configure 阶段联网。
- 依赖升级作为独立变更处理，必须运行完整测试和许可检查。

建议的 CMake options：

```text
LAZYCOM_BUILD_DIAGNOSTICS=ON|OFF
LAZYCOM_USE_SYSTEM_LIBSERIALPORT=ON|OFF
LAZYCOM_USE_SYSTEM_DEPS=ON|OFF
LAZYCOM_ENABLE_ASAN=ON|OFF
LAZYCOM_ENABLE_UBSAN=ON|OFF
LAZYCOM_ENABLE_TSAN=ON|OFF
LAZYCOM_ENABLE_COVERAGE=ON|OFF
LAZYCOM_ENABLE_LTO=ON|OFF
LAZYCOM_WARNINGS_AS_ERRORS=ON|OFF
```

`LAZYCOM_BUILD_DIAGNOSTICS` 默认 ON，当前使正式程序编译 diagnostics facade、spdlog 链接探针和最小应急接口；完整现场诊断 worker 尚未实现，运行时仍固定默认关闭。`LAZYCOM_USE_SYSTEM_LIBSERIALPORT` 默认 OFF，打开时要求 `pkg-config` 发现 0.1.2+ 并运行 backend smoke test。`LAZYCOM_USE_SYSTEM_DEPS` 只作用于 FTXUI、spdlog 等非 header-only 源码依赖，不能替换 `include/dependencies/` 中锁定的库，并且必须验证精确兼容版本。Sanitizer 选项相互校验。TSan 不与 ASan 同时启用，覆盖率和 LTO 不在同一构建中启用。

### 3.5 首版不引入的依赖

- 不引入 Boost 或 standalone Asio；单串口 owner 不需要通用异步执行器。
- 不单独向业务模块引入 fmt；spdlog 随附的 fmt 仅作为其私有实现依赖。
- 不引入 libmodbus；首版没有协议层，后续协议引擎也不能让第三方库取得串口所有权。
- 不引入 iconv 和 libudev；它们分别属于第二阶段编码和稳定设备身份功能。
- 不引入 Lua、Python 或动态插件加载器；它们仅是尚未获得 `Plan.md` 产品批准的非规范性候选。

## 4. 建议源码结构

```text
lazycom/
  CMakeLists.txt
  cmake/
    Dependencies.cmake
    ProjectOptions.cmake
    Sanitizers.cmake
  include/lazycom/
    app/
    base/
    diagnostics/
    model/
    serial/
    framing/
    encoding/
    scheduler/
    logging/
    config/
    ui/
  include/dependencies/
    DEPENDENCIES.lock
    licenses/
    nlohmann/
    toml++/
    tl/
  src/
    app/
    base/
    diagnostics/
    model/
    serial/
    framing/
    encoding/
    scheduler/
    logging/
    config/
    ui/
    main.cpp
  third_party/
    ftxui/
    libserialport/
    spdlog/
  docs/
  packaging/
```

测试目录和 Catch2 位于独立 `../lazycom-test` 仓库，该仓库通过 `LAZYCOM_SOURCE_DIR`
引用生产源码并复用 targets；默认目录为相邻 `../lazycom`。测试发现使用 `PRE_TEST`，
配置和构建不执行测试程序，生产构建不会反向加载测试仓库。

首版可根据代码量合并小目录，但依赖方向必须保持。不要为只有一个调用点的简单逻辑创建抽象层。

### 4.1 CMake targets

当前生产构建使用以下 targets；实现以根目录 `CMakeLists.txt` 为准：

- `lazycom_base`：Error、稳定错误代码、强类型 ID 和其他无 I/O 基础值类型。
- `lazycom_core`：应用版本及构建信息。
- `lazycom_data_path`：无 I/O 的分帧、编码、共享记录、sequencer 和预算模型。
- `lazycom_header_dependencies`：本地 header-only 依赖的 `SYSTEM INTERFACE` target。
- `lazycom_serial`：libserialport RAII、owner、扫描器及 owner 内部 scheduler。
- `lazycom_logging`：日志格式、队列、轮换和配额。
- `lazycom_config`：TOML schema、验证、安全读取和持久化 worker。
- `lazycom_diagnostics`：诊断 facade、spdlog 链接探针和最小应急接口；完整 worker、安全 sink 和轮换仍待实现。
- `lazycom_app`：生命周期、状态机、typed command/event 路由、可靠 completion 和 FATAL coordinator。
- `lazycom_ui`：FTXUI 组件和事件路由。
- `lazycom`：最终可执行文件。
- `lazycom_tests`：仅测试构建存在，链接 Catch2 并按标签拆分执行。

内部 target 默认不安装公共头文件。首版没有稳定 SDK，不承诺源代码或二进制兼容性。

### 4.2 依赖方向

以下箭头表示主要架构依赖，省略构建辅助 target 及可执行文件中的显式重复链接；完整直接链接以 `CMakeLists.txt` 为准：

```text
base        -> header_dependencies
diagnostics -> base, spdlog（仅 LAZYCOM_BUILD_DIAGNOSTICS=ON）
core        -> base
data_path   -> base, Threads
serial      -> base, config, Threads, libserialport
logging     -> base, data_path, Threads
config      -> base
app         -> core, serial, logging, config, data_path, diagnostics
ui          -> app, FTXUI
main        -> app, ui
```

- `model` 和纯逻辑模块不得依赖 FTXUI、libserialport 或 Linux fd。
- `ui` 不得包含串口配置、文件安全或协议解析实现。
- libserialport 类型不得离开 `serial` 模块。
- POSIX fd 必须由 RAII 类型管理，不能以裸 `int` 跨模块传递。
- 业务模块只能调用 `lazycom_diagnostics` wrapper，不能直接依赖 spdlog 类型或宏。
- FATAL coordinator 属于 app 主线程生命周期，不属于 diagnostics；底层 worker 发布固定 FatalSignal 并唤醒主线程，不能自行协调退出。
- Catch2 不得成为任何生产 target 的直接或传递依赖。
- `scheduler/` 是 `lazycom_serial` target 内部的纯 deadline 与 task generation 组件，不创建独立线程或独立 CMake target。

### 4.3 应用内部职责

- `Application` 保留连接状态机、命令守卫、事件归并顺序、跨组件断开 barrier 和正常/FATAL 退出协调。
- `SettingsCoordinator` 独占三份配置文档的身份、保留文本、read-only、dirty、保存 future 及 quick-send 待提交候选；提交结果返回给应用后再更新展示状态。
- `SessionLogCoordinator` 独占 writer、Start/End/Disable、rollover owner、backlog、重建和停止 deadline。它返回具名日志状态与关闭边界，不回调应用状态机。串口 Cleanup 和日志关闭仍是两个条件。
- `SessionRecords` 独占 framer、sequencer、可见记录、跨连接 record ID、RX/TX 统计和淘汰；`AppSnapshot` 借用其只读记录容器。
- `ReceiveViewModel` 管理过滤后坐标、viewport anchor、稳定 cursor 和搜索导航；TUI 保留输入优先级和 FTXUI 生命周期。覆盖层使用 `ModalKind` 与对应候选类型，沿用有界栈和父页面恢复。
- 设置接口直接传递具名候选和枚举；UI 只解析文本入口。Application 仍校验候选、连接锁和 stop gate。
- 公共 worker 信号与 operation/session 值类型分别放在 `base/worker_signals.hpp` 和 `model/session_types.hpp`；底层模块不反向包含 app。
- 严格 UTF-8、UTC 校验和安全 message projection 放在 `base/text`，payload 显示转义保持在 encoding。Linux 日志后端与异步 writer 分开编译，安全 fd 包装是平台私有实现。

## 5. 核心 C++ 类型与错误模型

### 5.1 Result

```cpp
enum class ErrorCode : std::uint32_t {
  ValidationInvalidValue   = 0x010001,
  SerialPermissionDenied   = 0x020001,
  SerialPortBusy           = 0x020002,
  SerialDeviceGone         = 0x020003,
  ConfigParseFailed        = 0x030001,
  LoggingDiskFull          = 0x040001,
  DiagnosticsUnavailable   = 0x050001,
  InternalInvariantBroken  = 0x090001,
};

enum class Operation : std::uint16_t {
  ValidateConfig,
  EnumerateDevices,
  OpenSerial,
  ConfigureSerial,
  ReadSerial,
  WriteSerial,
  CloseSerial,
  WriteSessionLog,
  SaveConfig,
};

struct Error {
  ErrorCode code;
  Operation operation;
  std::error_code cause;
  std::string detail;
  std::optional<SessionId> session_id;
  std::optional<OperationId> operation_id;
  std::source_location source;
};

template<class T>
using Result = tl::expected<T, Error>;

using Status = Result<void>;
```

- 每个 ErrorCode 在集中注册表中映射到稳定文本标识、domain 和默认用户消息，例如 `LC-VAL-1001`、`LC-SER-2001`、`LC-LOG-4001`、`LC-DIAG-5001` 和 `LC-INT-9001`。
- 错误代码集中定义并测试数值、稳定标识和 domain 一致性；已经发布的代码不得改义或复用。`Error` 不重复存储可从注册表推导的 domain。
- `cause` 保存 POSIX 或第三方可映射的底层错误；没有底层错误时为空。libserialport 的 OS 错误必须紧跟失败调用复制。
- `detail` 只用于有界、经过处理的动态上下文，不存储完整用户 payload，也不作为程序分支条件。
- 恢复等级不是 Error 的固有属性。同一错误在不同边界可以结束当前操作、停止日志、断开会话或触发进程退出，恢复动作由 app 状态机显式决定并测试。
- 预期内的验证失败、I/O 失败、队列满、取消和超时通过 `Result` 或异步错误事件表达。
- 构造失败不能产生半有效对象，使用返回 `Result<T>` 的工厂。
- 用户可见消息与稳定错误代码分离，以便后续翻译文本而不改变程序判断和诊断索引。

### 5.2 异常边界

- 异常只用于第三方抛出式 API、标准库资源异常和内部不可预期状态，不能代替正常的 `Result<T>` 控制流。
- 首版不定义项目业务异常层次；项目代码通过 `Result<T>` 传播预期失败，不为串口或配置错误创建异常子类。
- toml++、spdlog、文件系统和其他第三方异常在其适配层立即捕获并转换为统一 `Error`。
- 每个 `std::jthread` 入口分别捕获 `std::bad_alloc`、`std::exception` 和未知异常。普通第三方异常转换为 Error；资源耗尽和不变量破坏发布固定大小 `FatalSignal`，不能先构造动态 Error。
- 异常对象不能跨线程传播，不能把 `std::exception_ptr` 放入业务事件；跨线程只传递值类型 Error。
- 诊断 wrapper 的普通日志调用必须为 `noexcept` 语义，内部捕获 `spdlog::spdlog_ex` 和格式化异常，日志失败不能改变业务控制流。
- `try/catch` 只位于明确的适配层、线程入口和进程顶层，不能在每个业务函数重复捕获后继续运行。
- 进程安装最小 `std::terminate` handler，使用不分配内存的应急路径记录固定错误代码后调用 `std::abort()`；首版不拦截 SIGSEGV 等致命信号去调用不安全的 C++ 或 spdlog 逻辑。

### 5.3 FATAL signal

```cpp
struct FatalSignal {
  ErrorCode code;
  Operation operation;
  std::source_location source;
};
```

- `FatalSignal` 必须可在不分配内存、不格式化字符串且不依赖普通队列的路径中发布。
- 每个 worker 只有一个原子应急槽；第一个 fatal signal 获胜并通过既有 wake fd 唤醒主线程，后续信号只增加固定计数。
- worker 永不成为 fatal coordinator，也不等待或 join 自身。只有 UI 主线程负责进入 `FatalStopping` 和协调进程退出。

### 5.4 ID 与 generation

- `ConnectionGeneration`：每次连接请求递增，用于过滤尚未建立会话的迟到连接结果。
- `SessionId`：连接成功后创建，保持到匹配断开操作完成；Connected 接受常规 RX/TX，Disconnecting 只接受 owner 产生的已接受 TX 前缀、pending CR、RX 尾帧和最终 SYS/ERR 清理记录。
- `OperationId`：标识单次发送、连接、断开、扫描、保存和日志 barrier；生命周期完成事件优先按 OperationId 匹配。
- `TaskGeneration`：标识定时发送任务及替换、停止后的迟到请求。
- 类型使用轻量强类型封装，避免不同 ID 被意外比较或赋值。
- 计数溢出视为致命内部错误，不允许静默回绕后继续运行。

### 5.5 状态枚举契约

- `ConnectionState` 必须且只能包含 `Disconnected`、`Connecting`、`Connected`、`Disconnecting`、`Error`；状态事件、view model、UI 文本和测试 fixture 直接使用同一组名称，不定义映射别名。
- `Error` 仍是清理前可观察的瞬时生命周期状态，不能直接从错误跳过到 Disconnected。错误详情由独立 overlay model 保存，因此 ErrorDialog 可以跨 Disconnecting 保留到生命周期已经 Disconnected。
- Link renderer 永远只读取当前 `ConnectionState`，不能根据最近错误、通知或 ErrorDialog 是否存在推导 Link；这保证持久 ErrorDialog 不会伪造连接状态。
- `LogState` 必须且只能包含内部 `Off`、`Waiting`、`Recording`、`Error`；唯一用户映射分别为 `OFF`、`WAITING`、`REC`、`ERROR`，保留 Log 标签和 REC 缩写。
- 日志 `g` 命令实现 `Plan.md` 第 11.1 节的精确转换：Disconnected 下 Off/Waiting 切换，Connected 下 Off 尝试 Recording，Recording barrier 后关闭到 Off，Error 第一次只复位 Off，第二次才重试；Disconnecting 拒绝新启用。UI 在这些转换中分别显示 OFF/WAITING/REC/ERROR。
- `InteractionState::ReceiveBrowse` 持有稳定 current record ID 和有界的 `None | GPending | YPending | CountPending` 命令前缀状态。前缀解析属于当前交互状态，不得复用或调用 Normal 的 `g`、`y`、数字或后续键路由；无效后续键消费后清空，Esc 清空并返回 Normal，F1 仍走唯一无条件 Help 路由。

### 5.6 字节所有权

- 原始数据使用 `std::byte`。
- 只读参数使用 `std::span<const std::byte>`。
- 跨队列数据使用 `std::shared_ptr<const SessionRecord>`。每条记录拥有自己的 payload 和预算 token；UI 与日志共享该记录，按独立 seq 排序并逐条淘汰。
- payload 的全局预算 token 随共享记录生命周期持有并只计一次；每个 sink 仍统计自己的逻辑消息数和字节数以执行过载策略。
- 所有容器节点、字符串 capacity、索引和记录元数据按保守值计入 128 MiB 管理预算。
- UI 文本只是派生表示，不替代原始数据。

## 6. 同步与异步接口

### 6.1 同步底层接口

底层接口只由对应 owner 或 worker 调用，因此保持同步、短小并易于故障注入：

```cpp
class ISerialBackend {
public:
  virtual ~ISerialBackend() = default;

  virtual Result<std::vector<DeviceInfo>> enumerate() = 0;
  virtual Status open(const DevicePath&, const PortConfig&) = 0;
  virtual Result<BorrowedFd> native_wait_handle() const = 0;
  virtual Result<std::size_t> read_some(std::span<std::byte>) = 0;
  virtual Result<std::size_t> write_some(
      std::span<const std::byte>) = 0;
  virtual Status close() = 0;
};
```

实际实现可以把枚举和打开端口拆成两个类，避免 scanner 与连接 owner 共享状态。测试后端必须能注入部分写、返回 0、错误、延迟和设备消失。

### 6.2 异步服务接口

```cpp
using SerialCompletion = std::variant<
    ConnectCompletion,
    TxCompletion,
    DisconnectCompletion,
    TaskStartCompletion,
    TaskStopCompletion>;

class SerialService {
public:
  Result<OperationId> submit_connect(ConnectRequest request);
  Result<OperationId> submit_tx(TxRequest request);
  Result<OperationId> submit_task(StartTaskRequest request);
  Result<SubmitStatus> request_cancel_connect(CancelConnectCommand command);
  Result<SubmitStatus> request_disconnect(DisconnectCommand command);
  Result<SubmitStatus> request_stop_task(StopTaskCommand command);
  std::vector<SerialDataEvent> drain_data(std::size_t max_events);
  std::vector<SerialCompletion> drain_completions();
};
```

- `submit_connect()` 路由到普通 owner 命令队列，`submit_tx()` 路由到独立 TX 请求队列，各自执行 `Plan.md` 的消息数和字节配额。
- cancel、disconnect 和 stop-task 使用每类一个固定控制槽或原子状态加 eventfd，不进入普通或 TX 队列；重复请求返回已有 OperationId 或明确 already-pending 结果。
- 每个入口在同一临界区内执行同步验证、预留对应 completion slot、提交队列或控制槽并唤醒；任一步失败都不能留下半接受命令。
- 返回成功只表示命令被接受，不表示串口操作成功。
- 连接、发送、停止和断开的最终结果通过携带 OperationId 的 typed completion 返回；连接成功后数据事件携带 SessionId。
- UI 不持有 `future`，不等待后台线程，也不接受工作线程直接回调。
- completion mailbox 与普通数据事件队列分离，容量等于可接受 outstanding operation 数；已预留 completion 不能因数据队列满而丢失。
- mailbox 硬容量至少覆盖 queued TX、active TX、queued normal、active normal 和所有固定控制 operation 之和；控制槽的 completion 容量在初始化时永久保留，不能被普通操作占用。
- completion 可以重复安全忽略。生命周期结果按 OperationId 匹配；数据事件按 SessionId、连接状态和 `origin=normal|cleanup` 过滤，只有 DisconnectCompleted 匹配后才清除 SessionId。
- worker fatal 使用第 5.3 节固定应急槽，不伪装成可能入队失败的普通 SerialEvent。
- 同一可靠状态信号模式也用于日志 worker 的永久 `LogStatusSignal` 槽；自发日志错误不依赖 operation completion 的临时空位。

### 6.3 Worker stop protocol

- serial、session log、scanner 和 persistence worker 各自持有固定 stop 标志与 `WorkerStoppedSignal`。Application 持有成功创建的服务对象，不再维护镜像生命周期登记表；未创建的可选 worker 不参与等待。
- 主线程调用各服务的 `request_stop() noexcept`，设置 stop 标志并触发已有 eventfd 或 condition-variable predicate；路径不能分配内存或向普通队列 push。
- 各线程入口保留异常边界。所有可能阻塞的清理、flush、锁释放和 worker-owned 对象析构必须在发布 stopped 信号前完成，不要求额外的通用 trampoline 模板。
- 发布 `WorkerStoppedSignal` 后，线程进入 AtReturnPoint；此后只能执行不抛异常、不分配、不获取锁的固定 nonblocking wake，然后立即 return。
- AtReturnPoint 表示线程已到达不可阻塞的最终返回点，不声称内核线程已经完全退出。主线程观察该状态后才 join；deadline 超时直接进入 abort 回退。
- 正常退出和 FATAL 共用此路径。诊断后端的启停由 diagnostics 模块管理。stop/stopped 槽和 wake fd 属于 control/model 类别的预算接入范围，当前闭环状态见第 9.3 节。

### 6.4 UI 唤醒

- 工作线程将不可变数据事件和 completion 放入各自有界通道。
- 原子 `ui_wakeup_pending` 保证最多只有一个待处理的 FTXUI 自定义事件。
- 工作线程调用 FTXUI 支持的线程安全 PostEvent 入口，不操作组件树。
- UI 收到自定义事件后按预算 drain，清除 pending 标志，并处理清除与新入队之间的竞争。
- 大量 RX 不能产生同数量的终端唤醒事件。

### 6.5 不使用的异步方式

- 不使用 `std::async`，避免 future 析构隐式阻塞和执行策略不确定。
- 不使用 detached thread，所有线程都有明确 owner 和 join 路径。
- 不使用协程模拟并不存在的跨平台串口异步 API。
- 不让定时器线程直接写串口；定时 deadline 是 owner 状态机的一部分。

## 7. 线程与所有权模型

### 7.1 UI 主线程

- 唯一允许读写 FTXUI 组件和可见 UI 状态的线程。
- 负责输入路由、状态守卫、命令提交和后台事件归并。
- 不执行串口调用、设备枚举、日志 flush、fsync 或大规模解析。

### 7.2 串口 owner

- 进程生命周期内只有一个常驻 `std::jthread` serial owner；未连接时阻塞等待命令，重连不创建新线程。
- owner 独占 `sp_port*`、native serial fd 观察权、eventfd 和当前 TX 状态。
- 只有 owner 调用配置、非阻塞读写和 `sp_close()`。
- 定时发送 deadline、部分写 offset 和停止确认都由 owner 管理。
- `Scheduler` 仅由 owner 访问，`nearest_deadline()` 合并调度和 I/O deadline；UI tick 只归并状态与结果，不推进发送时钟。
- `submit_task()` 接收会话身份、执行快照和精确的预期替换 generation，预留固定控制 completion 后接纳。停止可以匹配活动 generation 或尚未完成的启动 operation。
- owner 在处理手工 TX 超时后调度，在每次写入前检查替换/停止意图。旧 TX 的已写前缀及 terminal outcome 先于匹配停止确认发布；确认后不会继续写旧任务。
- `OperationIdIssuer` 在 SerialService 内为应用提交和 owner 自动 TX 统一发号，避免两条线程上的 operation ID 冲突；task、connection 和 session generation 仍相互独立。
- UI、日志线程、scanner 和脚本都不能取得 `sp_port*` 或 serial fd。

### 7.3 会话日志 worker

- 独占当前日志 fd、缓冲、轮换和目录配额状态。
- 处理有界日志队列、flush deadline 和 barrier。
- 普通记录的队列计数和字节记账在出队的同一临界区内更新。普通追加只刷新一次目录 inventory，再使用该结果执行配额检查；轮换关闭旧文件后重新刷新，删除候选仍逐个复核身份、owner 和权限。
- 日志队列满载时停止记录并发出内部 `LogState::Error`，UI 显示 `Log:ERROR`，不能阻塞 serial owner。
- 日志错误事件不能递归写入已经失败的同一日志队列。
- 该 worker 实现用户会话日志，不承载 spdlog 内部诊断。

### 7.4 设备 scanner

- scanner 是进程生命周期内单一常驻 worker，不允许每次刷新创建线程。
- 最多一个扫描运行中、一个请求 pending；第三个请求同步返回 busy。每个接受请求生成 OperationId 并预留 completion，pending 不被后续刷新静默替换。
- 设备枚举异步执行，结果立即转换为 `DeviceInfo` 值对象。
- 释放 libserialport 列表后不能保留其中字符串或 `sp_port*`。
- 扫描结果携带 generation 和 OperationId；generation 防止旧结果覆盖列表，OperationId 保证每个已接受请求都有最终 completion。
- `DeviceInfo` 保存安全显示值、最终路径身份、owner uid、group gid、mode 和基于当前有效身份的权限预检结果；用户选择列表只能来自当前扫描快照，不提供设备路径或其他原始字符串输入。
- 启动扫描完成后聚合权限结果：发现候选全部拒绝时只发布一次红色 ErrorDialog 请求，部分拒绝时把拒绝状态留在对应 DeviceInfo 供 UI 标红；重复渲染或后台刷新不能重复弹出启动错误。
- 权限预检必须按 effective uid/gid 和 supplementary groups 判断，或使用等价的 effective-ID 检查，不能误用只按 real uid 的 `access()`。选择设备和提交连接时都重新检查，最终 `sp_open()`/底层 open 结果始终权威。
- 首次实现应验证枚举与活动串口 owner 并行调用 libserialport 的行为；如库级并发不可靠，则通过独立库调用锁串行化枚举和打开阶段，但不能在锁内执行持续 I/O。

### 7.5 持久化 worker

- 负责 `config.toml`、`quick_send.toml` 和 `state.toml` 的安全写入。
- 同一文件只允许一个进行中的保存操作。
- 每个 TOML 最多维护一个固定 `.bak`，不生成时间序列备份；备份使用相同目录 fd、0600、no-follow、尺寸上限和原子替换规则，备份失败时主文件事务返回 NotCommitted。
- 写入采用目录 fd、no-follow、独占临时文件、文件 fsync、rename 和父目录 fsync。
- 保存 completion 使用 `NotCommitted`、`Committed`、`CommittedDurabilityUnknown` 三态。rename 前失败为未提交；rename 成功但父目录 fsync 失败为已提交但重启耐久性未知。
- 两种已提交 completion 均携带本次临时文件对应的已提交身份，与序列化文档一起成为下次保存的冲突检测基线；UI 不得重读目标路径来采用其他写者的身份。
- UI 在后两种结果中提交已序列化的同一内存快照，并在耐久性未知时警告；未提交时保留旧内存状态和编辑内容。
- Session Log Settings 的 Directory、Maximum files、Maximum total size、Maximum file size 作为一个候选快照持久化，不提供逐字段运行时提交。`Save for Next Session` 和 `Save and Rotate Now` 都必须保存同一个完整候选及同一三态 completion。

### 7.6 内部诊断 worker

- 仅在诊断运行时启用后启动一个项目拥有的 diagnostics worker；业务线程只向独立有界队列提交有界记录。
- worker 在自己的线程中调用同步 spdlog formatter 和项目自定义安全 fd sink，不启用 spdlog async thread pool，也不使用 stock rotating file sink。
- 安全文件模块负责目录 fd、0600、no-follow、轮换和配额，不能与会话日志共享 queue、barrier 或状态机。
- 诊断写入不得反向调用 app command、session sequencer 或 UI。
- 正常关闭执行有 deadline 的 flush；FATAL 使用第 10.9 节的独立协调流程。

## 8. Linux 串口 owner 事件循环

### 8.1 native handle 风险边界

libserialport 对直接操作 OS handle 给出冲突警告，但其阻塞 I/O 文档以 `select()`/`pselect()` 说明“先等待、再调用 nonblocking API”的集成方式。该说明仅作为上游依据；LazyCom 正式实现只允许 `ppoll()`，并遵守：

- native fd 仅用于 `ppoll()` readiness 和连接后的 `fstat()`。
- 不对 native fd 调用 `read()`、`write()`、`close()`、`tcsetattr()` 或线路控制 ioctl。
- 实际读写、配置和关闭全部通过 libserialport。
- native fd 不复制、不跨线程、不暴露给上层。
- `ppoll()` 返回且 owner 停止等待后才能调用 `sp_close()`；关闭后立即把保存的 fd 设为无效。
- `sp_get_port_handle()` 失败时连接失败并报告 backend unsupported，不静默切换另一种时序模型。
- owner 只接受由当前扫描快照产生并在 app 层完成选择重检的 DeviceInfo；进入 Connecting 前再次解析最终字符设备、复核身份和 effective-ID 权限。测试后端可以通过非 UI 的明确接口注入 PTY 路径，不把该能力暴露为产品设备路径入口。
- 连接前权限检查只用于提前生成清晰错误；仍必须调用实际 open，且实际 `Permission denied`、busy、设备消失等结果覆盖预检。应用在所有路径都不得调用 chmod、usermod、修改 udev、sudo 或其他提权机制。

### 8.2 ppoll wait set

```text
serial fd: POLLIN | (pending TX ? POLLOUT : 0)
wake fd:   POLLIN
timeout:   nearest monotonic deadline or infinite
```

- `POLLIN` 常驻。
- 仅有未完成 TX 时监听 `POLLOUT`，避免 tty 长期可写导致忙循环。
- `eventfd` 使用 `EFD_NONBLOCK | EFD_CLOEXEC`，不使用 semaphore 模式。
- eventfd 写入 `uint64_t{1}`；读端循环读取到 `EAGAIN`。
- eventfd 计数饱和导致写入 `EAGAIN` 时无需重试，因为 fd 已经可读。
- 使用 `ppoll()` 的最近 deadline 处理 TX、空闲分帧、定时任务和停止超时，不额外引入 timerfd。

### 8.3 唤醒来源

- 新普通命令。
- 停止、取消和断开标志。
- `std::stop_token` callback。
- 应用关闭。

控制命令不依赖普通队列空位。生产者先提交命令或设置原子状态，再写 eventfd。

### 8.4 单轮处理顺序

1. 检查 stop、disconnect、当前 SessionId 和操作状态。
2. drain eventfd。
3. 处理高优先级控制状态。
4. 处理有限数量的普通命令。
5. 处理 `POLLIN`，按字节预算循环调用 libserialport nonblocking read。
6. 处理 `POLLOUT`，继续当前逻辑 TX 请求，不允许请求间字节交错。
7. 处理到期的 idle、TX 和定时任务 deadline。
8. 将新事件提交给 session sequencer。
9. 重新计算 interest set 和最近 deadline。

RX、TX 和命令每轮必须有预算；预算耗尽后使用零超时重新 `ppoll()`，保证控制命令和另一方向不会饥饿。

### 8.5 ppoll 错误

- `POLLNVAL`：内部 fd 生命周期错误，生成 ERR 并断开。
- `POLLERR`：尝试复制可获得的 libserialport/POSIX 错误，生成 ERR 并断开。
- `POLLHUP`：如同时有 `POLLIN`，先按预算读取已到达数据，再生成设备断开 ERR。
- `EINTR`：重新计算 deadline 后继续，不能重置相对超时。
- 未知 revents：生成内部错误，不能忙循环。

### 8.6 deadline 限制

- 所有 deadline 使用 `steady_clock` 绝对时间点。
- deadline 表示操作接受上限和超时语义，不代表 C++ 可以强制中断任意阻塞系统调用。
- owner 路径只使用非阻塞串口 I/O 和可由 eventfd 唤醒的 `ppoll()`，以使停止时间可控。
- owner 超过停止 deadline 时主线程进入 FATAL 回退并执行应急 `abort()`；不能调用会永久阻塞的 `jthread::join()`，其他线程仍不得并发调用 `sp_close()`。

## 9. 会话事件、排序和队列

### 9.1 Session sequencer

- RX、TX、SYS 和 ERR 进入同一个轻量序列化入口。
- Application 在串口事件入口校验 SessionId；sequencer 仅由主线程调用，校验 normal/cleanup 来源并分配严格递增 `seq`；SessionRecords 与 SessionLogCoordinator 分别接纳 UI 与日志引用。跨线程只传递不可变记录；不为主线程状态添加 mutex 或通用 sink 接口。
- sequencer 在预算准入后复制本条 payload 并构造共享记录，不执行文件 I/O、FTXUI 调用或日志编码。
- 每次准入产生一条不可变共享记录，不为单条数据建立额外的批次容器或 payload 所有权对象。
- UTC 时间不作为排序依据，`seq` 是唯一业务顺序。
- fan-out 不提供两个 sink 同时成功的事务语义。UI 满时淘汰旧记录并插入 seq gap；日志满时停止日志并通过永久 `LogStatusSignal` 报告内部 LogState::Error/UI `ERROR`，不占用 operation completion mailbox；二者都不能阻塞 serial owner。

### 9.2 队列实现要求

- 首版使用有界 mutex + condition variable/原子唤醒组合，不实现自研 lock-free queue。
- 容器优先使用 `std::deque`、`std::vector`、`std::optional` 和 `std::variant`；只有 profiling 证明必要时才评审专用 allocator 或 lock-free queue。
- 队列 push 返回明确的 accepted、full 或 stopped 结果。
- stop 与 disconnect 使用独立通道，不受普通队列配额影响。
- completion mailbox 按接受 operation 时预留的容量提供 must-deliver 保证，不与普通数据队列共用配额。
- 队列销毁前先停止生产者，再 drain 或按定义丢弃并等待 worker AtReturnPoint；只有确认线程已进入不可阻塞的最终返回点后才调用 join。
- `std::jthread` 没有 timed join。主线程等待 completion deadline，超时直接进入 FATAL abort 回退，不能先调用可能永久阻塞的 join。
- 每个队列的消息数和字节数必须与 `Plan.md` 默认值及硬上限一致。

### 9.3 全局内存预算

- `Plan.md` 的八类管理预算总计 128 MiB；`GlobalMemoryBudget`、共享记录 token 和 `SessionSequencer` 的实现与测试已经存在。
- 当前生产 `SessionRecords` 通过唯一 `SessionSequencer` 分配 session seq，以不可变 `SessionRecord` 保持逐条淘汰；`VisibleRecord` 借用记录中的字节与文本，日志 worker 直接编码同一记录。跨连接稳定的 record ID 仍独立递增。
- 已接入的 token 包括记录、payload 及其字段的保守元数据、UI 引用和日志队列/backlog 引用。共享对象计入 `UiRecords`，日志引用计入 `SessionLog`；即使 UI 清空，共享对象的 token 也保持到最后一个日志引用释放。TUI 的过滤坐标和安全文本缓存共享同一 `UiRecords` 预算，增量搜索的查询与结果存储计入 `Model`。
- ingress、TX、草稿/历史、framer scratch、配置解析/持久化、completion 和固定控制槽尚未全部接入。现有分类及配置组合上限继续生效，128 MiB 全局 token 仍不是端到端运行时硬边界。接入沿革见 [开发历史](docs/history.md)；阶段 7 必须完成剩余接入才能宣称闭环。
- payload 通过共享 budget token 按实际分配计一次；每个 sink 的逻辑队列配额独立统计，以便执行各自过载策略。
- UI 记录元数据、容器节点、字符串 capacity、搜索索引和 diagnostics 消息都必须记账。
- completion 对象、active operation、固定控制槽、OperationId 索引、LogStatusSignal、WorkerStoppedSignal 和 scanner pending 状态计入 control/model 类别，并参与配置组合验证。
- 分配前先原子预留预算，构造失败或对象销毁时归还；不能先分配后发现超限。分类上限在构造时验证总和不超过 128 MiB 且此后不可变，每次准入只检查所属分类；`total_used()` 是各分类当前值之和，仅供观察，不提供跨分类的原子快照。
- 预算耗尽按所属 sink 处理：TX 拒绝、UI 淘汰并 gap、日志停写、diagnostics overrun-oldest、RX ingress 丢失则应急报告并断开。

## 10. 配置、会话日志与诊断日志

### 10.1 TOML

- 三个文件 schema 分开建模，不使用一个通用字符串 map 表示全部配置。
- 解析分为语法解析、schema 校验、范围校验和运行时转换。
- 字段错误携带完整路径，例如 `queues.tx_max_mib`。
- 未知主版本拒绝加载；未知键警告并在允许重写的模型中保留。
- 语法错误后进入只读保护，不能用默认值覆盖用户文件。
- schema、范围或资源组合存在任何错误时拒绝整个候选快照并汇总错误，不能逐字段回退后提交混合配置。
- 启动时没有旧快照则使用完整内置默认值继续运行，但保持错误文件只读且不自动覆盖。
- TOML 配置不能突破编译期单项上限或 128 MiB 全局预算，日志配额不能用 0 取消磁盘硬边界。
- 应用只在用户显式保存时改写配置。保存结果使用 `NotCommitted`、`Committed`、`CommittedDurabilityUnknown`，语义与第 7.5 节一致。
- `[ui].background` 是强类型枚举，仅接受 `rose-pine` 或 `transparent`，内置默认和序列化默认均为 `rose-pine`；非法值使整个候选快照失败。
- `P/B/D/N/V/H` 的 UI model 由强类型预设表生成，不能把通用 Input 组件绑定到配置值。`P` 只消费扫描 DeviceInfo；`B` 固定为 300、600、1200、2400、4800、9600、19200、38400、57600、115200、230400、250000、460800、500000、921600、1000000、1500000、2000000。
- `D` 分别使用数据位 5/6/7/8、停止位 1/2、校验 None/Odd/Even/Mark/Space、流控 None/RTS/CTS/XON/XOFF 的强类型预设；`N` 只使用 None/LF/CR/CRLF；`V` 分别使用 RX/TX 的 TXT/HEX/MIXED 强类型值和非空 RX/TX/SYS/ERR 可见位集，SYS/ERR 不含 view 字段且始终为 TXT；`H` 只使用 TXT/HEX。
- `config.toml` 可以表示上述已定义值，但产品弹窗不提供任意 baud、路径或原始字符串编辑；schema 也拒绝不在固定 baud 表内的默认 baud。
- Receive schema 使用 `receive.rx_view` 和 `receive.tx_view`，序列化值仅为 `txt`、`hex`、`mixed`，默认均为 `txt`。legacy `receive.view` 加载时迁移到两个新字段，legacy `text` 映射为 `txt`；新旧字段并存时新字段优先并警告，下一次应用管理重写移除旧字段。
- `send.keep_after_send` 是已知废弃字段而非支持项或普通未知键。加载旧持久值时忽略并固定采用成功提交后清空，下一次应用管理重写移除该字段；迁移测试必须覆盖 true、false 和与未知键并存。
- Session Log Settings 只投影 logging directory 和三个现有配额字段；文件名、NDJSON schema、格式、方向和时间字段没有 UI candidate，也不得加入保存 command。

### 10.2 Session Log Settings 事务

- `G` overlay model 包含第一级列表、第二级字段编辑器、四字段完整 candidate、来源焦点和可恢复 ErrorDialog 栈；进入第二级只修改 candidate，不触碰生效快照。
- Directory 在进入编辑时把空配置解析为实际 XDG 默认绝对目录，不向 UI 暴露空字符串；Enter 通过安全文件模块验证：绝对且存在、最终组件不是符号链接、目录 owner 等于当前 euid，并按有效身份具有 write/search 权限。打开红色 ErrorDialog 后必须恢复第二级原内容、光标、选择和焦点；ErrorDialog 除 F1 外消费所有事件。
- Maximum files、Maximum total size、Maximum file size 复用 `Plan.md` 第 12.2 节硬范围和交叉约束；错误只标记候选，不做截断或部分提交。
- 两种保存操作都一次应用完整 candidate，并通过 persistence worker 异步持久化；连续配置保存合并为最新快照。`Save for Next Session` 不触碰活动 writer，在会话结束后重建。`Save and Rotate Now` 在 Recording 下通过 operation/completion 驱动结束当前文件、重建 writer 和创建新 header；后续失败进入日志 Error，已经安全关闭的旧文件不删除、不改写。
- Off、Waiting 或 Error 下没有活动文件，`Save and Rotate Now` 与下一会话保存相同 candidate，并返回明确 no-active-file 提示，不能生成虚假 rollover completion。
- 活动 rollover 与配置持久化结果分别具有 operation ID，但 app coordinator 只在定义的提交点切换完整快照；需要故障注入覆盖保存三态、barrier、旧文件关闭、新文件创建和目录配额失败。
- 已接受的 rollover 以 connection generation 和 session ID 保留所有权。断开和 shutdown 必须在原停止 deadline 内继续推进 Start/End、writer 重建和 backlog 排空；重复配置更新不得清除当前 session 的收尾责任。

### 10.3 NDJSON v1

- nlohmann/json 只封装在日志格式实现中，不扩散到核心 model 的公共接口。
- header 和 record 都有明确对象类型，schema 使用 major/minor 整数版本。
- 读取旧日志时执行类型和范围校验，不信任 JSON 内容。
- 每个逻辑 record 独占一行，包含 seq、UTC、会话相对 `elapsed_ns` 和 direction。
- RX/TX 可无损往返 UTF-8 时写 `encoding="utf8"`，否则写 base64；SYS 表示应用/系统生命周期事件，ERR 表示错误事件，二者都只写有界 TXT message，ERR 同时写稳定错误 code。
- 批量队列只减少内部锁和唤醒，不改变文件逐行记录、seq 或 barrier 语义。
- writer 维护 `processed_through_seq`。barrier 只有在此前记录已经处理并 flush 出用户态缓冲后确认，不把 flush 描述为物理介质 fsync。

### 10.4 文件安全

- 文件操作优先使用目录 fd 和 `openat` 风格 API，避免检查后路径替换。
- 应用创建目录为 0700，配置和日志文件为 0600。
- 使用 `O_CLOEXEC`、`O_NOFOLLOW` 和适用的独占创建标志。
- 配额删除只能作用于成功识别为本应用日志且不处于活动状态的文件。
- 所有文件名、设备元数据和日志内容都视为不可信输入。
- 用户选择的会话日志目录使用与 `G` Enter 相同的绝对、存在、no-final-symlink、euid owner、write/search 验证；应用不修复已有目录权限。

### 10.5 两类日志边界

- 会话日志是 `Plan.md` 定义的用户功能，记录经过严格语义定义的 RX、TX、SYS 和 ERR。
- 诊断日志是开发和支持人员排查 bug 的内部能力，使用 spdlog，不能复用会话日志状态机、文件格式或用户开关。
- 正式构建默认编译 `lazycom_diagnostics`，但每次进程启动时诊断日志都默认为关闭。
- 普通设置界面、命令面板、帮助页和 `config.toml` 不提供诊断开关。
- 开发或支持人员通过内部环境变量 `LAZYCOM_DIAGNOSTICS=1` 临时启用；该入口只记录在开发和支持文档中。
- 诊断初始化失败时应用继续运行；不能因为 bug 辅助设施不可用而阻止正常串口功能。
- 不分配内存的 `emergency_write()` 始终编译并可用；诊断未启用时不启动普通 sink，也不创建诊断文件。退出协调始终属于 app 主线程。
- 首次调用任何 libserialport API 前始终安装项目 debug handler。诊断启用时其输出映射到内部 DEBUG 队列；诊断关闭时安全丢弃并只累计固定计数，不能让 `LIBSERIALPORT_DEBUG` 默认 handler 写 stderr 污染 TUI。

### 10.6 等级和格式

项目 wrapper 暴露以下等级：

| 项目等级 | spdlog 等级 | 用途 |
| --- | --- | --- |
| TRACE | trace | 高频状态转换和详细执行路径 |
| DEBUG | debug | 开发期对象、队列和 deadline 信息 |
| INFO | info | 生命周期和重要操作完成 |
| WARNING | warn | 可恢复异常、降级和诊断丢弃 |
| ERROR | error | 操作失败或会话级致命错误 |
| FATAL | critical | 进程无法继续安全运行 |

- 恢复动作与诊断等级正交；同一 Error 可以记录 WARNING 或 ERROR，不能仅根据日志等级决定是否断开或退出。
- 每条诊断至少包含 UTC 时间、进程启动后的单调时间、等级、线程 ID、模块、源码位置和消息。
- 关联 Error 时额外记录稳定错误标识、注册表 domain、operation、SessionId、OperationId 和可用的底层 error_code。
- 业务模块只调用 `LC_DIAG_TRACE`、`LC_DIAG_DEBUG`、`LC_DIAG_INFO`、`LC_DIAG_WARNING` 和 `LC_DIAG_ERROR`；worker 顶层调用 `publish_fatal_signal()`，主线程和进程顶层边界调用集中式 `report_fatal()`。
- `SPDLOG_*` 宏、logger 指针和 sink 类型不能出现在 diagnostics 模块之外。
- spdlog 编译时保留 TRACE 及以上等级，运行时由内部 level 过滤；诊断关闭时 wrapper 必须避免格式化和堆分配。

### 10.7 文件、队列与轮换

- 默认目录为 `$XDG_STATE_HOME/lazycom/diagnostics/`，未设置时使用 `~/.local/state/lazycom/diagnostics/`。
- 目录权限为 0700，文件权限为 0600；使用安全文件模块创建并验证，不能依赖 umask。
- TUI 活动期间不配置 stdout 或 stderr sink，避免破坏 FTXUI 屏幕。
- 普通诊断使用项目有界队列、专用 diagnostics worker、同步 spdlog formatter 和自定义安全 fd sink；不启用 spdlog async thread pool 或 stock rotating sink。
- 默认队列上限为 4096 条且总计不超过 8 MiB，每条处理前限制为 2 KiB；默认保留 3 个文件且单文件最大 5 MiB。
- 队列满时采用 overrun-oldest，不能阻塞 serial owner；累计丢弃数量，并在恢复写入后生成一条 WARNING 汇总。
- 安全文件模块使用目录 fd、0600、no-follow 和独占创建完成轮换；flush、轮换和 sink 错误在 diagnostics 内部处理，不能递归调用同一失败 logger。
- 诊断关闭或退出时按 deadline 停止 consumer；超时不能阻塞串口 owner 的清理。

### 10.8 隐私和脱敏

- 诊断日志默认禁止记录 RX/TX 原始字节、发送草稿、快捷发送内容和完整会话日志行。
- 不记录环境变量值、编辑器临时文件内容、用户配置全文或可能包含凭据的脚本变量。
- 设备路径、USB 序列号和用户路径只有在问题定位确实需要时才记录经过转义或脱敏的值。
- 所有动态字符串先执行控制字符、换行注入和最大长度处理。
- FATAL 记录仍遵守脱敏规则；紧急情况不能成为泄露串口内容的理由。

### 10.9 FATAL 处理

`FATAL` 不是普通 `critical` 日志别名，而是集中式进程状态转换：

1. worker 只把固定 `FatalSignal` 写入自己的原子应急槽并唤醒 UI 主线程；worker 永不成为 coordinator。
2. UI 主线程通过原子状态保证只进入一次 FatalStopping；主线程自身发现 fatal 时直接执行同一流程。
3. 主线程拒绝新连接、发送、保存和脚本命令，执行固定文本应急写入，并在资源仍可靠时最佳努力提交 diagnostics critical 记录。
4. 主线程请求 serial owner、日志 worker、scanner、持久化和 diagnostics worker 停止。
5. 主线程通过第 6.3 节不分配内存的查询等待各 Running worker 的固定 `WorkerStoppedSignal`；确认其进入 AtReturnPoint 后才 join 对应 jthread，NotStarted worker 不加入等待集合。
6. 全部 stopped signal 在 deadline 内到达时，以非零状态退出，不继续返回主事件循环。
7. fatal 重入、资源耗尽、应急槽损坏或任一 stopped signal 超时，立即执行不分配内存的最小应急写入并调用 `std::abort()`；不能调用无期限 join。

- 应急写入不得依赖异步队列、格式化堆分配或已经损坏的 app model。
- 普通诊断关闭时，应急路径只写最小错误代码和固定文本，不能因此隐式开启或创建持久诊断文件。
- `std::abort()` 用于保留崩溃现场和允许系统生成 core dump；应用不自动修改系统 core dump 或 `ulimit` 策略。
- FATAL 路径不能从非 owner 线程直接关闭 `sp_port*`，即使即将 abort 也不能制造额外并发 UAF。
- `FatalSignal` 不能先转换为含 `std::string` 的普通 Error；OOM 路径不得依赖 spdlog、JSON 或 app model。
- 测试通过子进程分别验证受控退出和 abort 回退，不能让测试 runner 自身崩溃。

## 11. FTXUI 集成原则

- FTXUI 主循环只运行在主线程。
- UI 组件读取 view model，不直接持有 service 实现。
- Rosé Pine Moon 调色板集中定义为 FTXUI `Color::RGB` 值并提供 ANSI 近似回退，组件不得散落硬编码颜色；所有快捷键字面量由统一 decorator 渲染为粗体 `#FFFFFF`，状态始终同时保留文本标签。
- renderer 通过 `UiBackground` 选择装饰器：rose-pine 可以使用 Base/Surface/Overlay bgcolor；transparent 路径在整个 Element 树中不得调用或遗留任何 bgcolor，包括根节点、空白、footer、选择、通知和 overlay。transparent 下使用前景、边框、粗体、下划线和反选表达层级。
- 顶部状态行由可独立着色的结构化 `Element` 组合，不能先拼成一个只能整体着色的字符串；标题只承载 `LazyCom` 或会话名称。
- 顶部字段名称固定渲染为 `[NewLine<N>]`、`[View<V>]`、`[InputType<H>]`、`[Log<g/G>]`；View 同时投影 RX/TX 独立模式及 RX/TX/SYS/ERR 可见性。Link 直接渲染 ConnectionState 同名文本：Disconnected 红色、Connecting/Disconnecting Gold、Connected 绿色、Error 红色粗体反选。
- 顶部 `HW` 直接由 ConnectionState 派生：Disconnected 显示 `HW:UNLOCKED`；Connecting、Connected、Disconnecting、Error 显示 `HW:LOCKED`。LOCKED 禁止修改设备、baud、data bits、stop bits、parity、flow，断开清理完成后统一解锁，不能由弹窗焦点或最近错误推导。
- view model 分开保存待用串口配置和活动连接实际快照，Connecting、Connected、Disconnecting、Error 渲染该次活动快照，不能从可能变化的默认配置即时重建。ErrorDialog model 与 Link model 正交；生命周期回到 Disconnected 后仍可保留 dialog，但 Link 必须立即显示 Disconnected。
- 发送区使用受控多行 Input 模型、自有光标位置和有限高度视口；SendEdit 在 FTXUI 默认处理前拦截 Enter、Alt+Enter、Alt+Up/Down 和 Esc，保证发送、换行、历史与退出语义互不冲突。Enter 只有在解析、连接和队列/command 提交全部成功后才清空草稿并把提交快照写入有界历史；任一同步失败保留草稿和编辑状态，不读取 keep-after-send 配置分支。
- TUI 启用终端 bracketed paste 并在应用事件路由前收集有界文本，结束后验证 UTF-8 和编辑上下文再原子插入；粘贴期间后台 Custom 事件仍须推进 Application，粘贴内容不得落入按键命令或 job-control 路径。
- Input 模型必须显式归一化空草稿状态：Backspace 删除最后一个字符和成功发送清空后，cursor column、selection anchor 和 horizontal viewport origin 均为 0，渲染不能使用可用宽度作为空输入光标位置。
- SendEdit 的鼠标事件继续交给发送 Input，用于放置光标和选择草稿；接收区选择 handler 不能在 SendEdit 抢占鼠标。
- 未聚焦占位文本是派生显示 Element，不进入草稿；聚焦后隐藏占位、渲染真实光标，并独立设置活动边框和标题颜色。
- Receive 与 Input 一样由独立 bordered component 渲染。未聚焦标题固定明显显示 `<R> Receive` 并使用 Subtle 边框/标题；Normal 的 `R` command 聚焦后交互状态保持 ReceiveBrowse，标题改为 `Receive [Browse]` 并使用 Iris 活动边框/标题；Esc 返回 Normal 并恢复未聚焦样式，文本标签保证焦点不只靠颜色表达。
- Receive view model 以过滤后的稳定 record ID 列表和 current record ID 建模，每条可见 RX/TX/SYS/ERR 记录只生成一条物理显示行；MIXED 的 TXT/HEX 派生表示组合在同一行。current 行同时渲染固定文本游标标记和 Iris 样式，不能只靠颜色；新事件、重绘和视口变化不改变 current ID，当前项被过滤或淘汰时选择最近可见项，无记录时使用显式 empty cursor。
- ReceiveBrowse 事件处理器在 Normal/global 路由前解析 `j/k`、有界十进制 count+j/k、`gg`、`G`、`yy`。count 只作用于 `j/k`，不能作用于 `yy`；单个 `g/y/count` 保存为上下文前缀，无效后续键由该处理器消费并清空。Up/Down、PgUp/PgDn、Home/End、滚轮、鼠标选区、`i`、裸 `q` 和 Esc 保持 `Plan.md` 定义的既有行为。
- footer view model 分开生成运行指标行和当前 Normal/Input/Browse 上下文动作行，不重复顶部快捷键；`LOGQ` 只读取日志队列指标。
- 键盘和鼠标动作统一转换为 app command，执行同一状态守卫。
- overlay 栈、交互状态和连接状态分开建模。
- ErrorDialog 使用红色边框/标题，保存来源 overlay 的字段、光标和焦点；除 F1 外所有事件均由 dialog 消费，关闭事件本身也不得继续路由到底层组件。
- `P/B/D/N/V/H` 使用强类型列表、radio/check 项和按钮构建预设 popup，不复用自由文本配置控件；`G` 按第 10.2 节构建两级 candidate overlay。
- 接收区使用虚拟化或按可见范围生成 Element，不能每帧重建全部 100000 条记录的复杂树。
- 接收坐标在记录集合与方向过滤不变时复用；全部方向可见时直接使用原坐标，不分配索引。过滤索引由 UI 预算预留，容量不足时使用不分配内存的顺序扫描；记录追加、淘汰、清空与过滤切换必须刷新坐标。
- 接收区按终端与其他面板占用的行数限制渲染范围，最多构造 200 条记录行。安全文本节点缓存最多 200 条、2 MiB，按实际字符串 capacity 加节点开销计入 UI 预算；缓存不能持有被淘汰记录的 payload。
- 定时器和 worker 通过合并的 FTXUI closure 推进 Application 状态，只有可见状态变化时发送重绘事件；键盘、鼠标、终端 resize 和 FTXUI 动画仍使用原事件路径。25 ms 状态推进保留 idle 分帧、notice 到期及 completion/deadline 处理。
- 搜索在 UI 线程按捕获的查询、过滤、显示模式与最后记录 ID 分批推进，每批最多 256 条，并按 512 KiB 原始 payload/消息及 2 ms 时间片在记录边界让出。单条记录不拆分，可能超过字节或时间阈值，因此时间片不是硬实时保证。结果及查询存储先预留 Model 预算，旧查询取消或关闭时释放，最多保留 10000 个稳定 ID。
- Normal 和 ReceiveBrowse 使用 FTXUI 屏幕选区实现无修饰键左键拖选；滚轮和浏览键执行滚动命令时返回未消费，使 FTXUI 保留选区。定时 Custom 刷新同样不得主动清空选择。
- 复制内容在选区变化回调中通过 `GetSelection()` 捕获当前已渲染安全文本；编码前硬上限 1 MiB，超过上限返回错误且不截断。OSC 52 encoder 只接受该有界值，不允许原始串口字节直接形成控制序列。
- `y` 在 Normal 且选择非空时执行选区复制；ReceiveBrowse 的第一个 `y` 只建立前缀，`yy` 把 current record 的完整已渲染安全文本交给同一个 1 MiB 有界 OSC 52 encoder。`Ctrl+C` 只有终端转发、没有高优先组件消费且选择非空时复制选区；空选择 no-op。`Ctrl+Shift+C` 通常由终端自身拦截，帮助页说明限制但不注册为应用快捷键；F1 仍是唯一无条件路由。
- rose-pine 下选择使用 Iris 背景/Base 前景，transparent 下不发背景色而使用反选和双下划线。
- 搜索索引与原始记录 ID 关联，记录淘汰后同步移除索引。
- 终端宽高为零或极小时仍必须安全渲染，不允许负尺寸转换为巨大无符号值。
- 状态栏和 footer 根据显式优先级隐藏低优先字段；极小终端仍保留安全尺寸、关键文本状态和可辨识焦点。
- 所有设备文本在进入 FTXUI 文本节点前执行控制字符和 bidi 安全转义。

## 12. Linux 驱动与运行环境

首版最低运行和构建基线为 Ubuntu 24.04 档：Linux kernel 6.8、glibc 2.39、GCC 13 或 Clang 18、CMake 3.28。CI 可以使用更新工具链，但不得无意引入高于该基线的 ABI 或系统调用依赖。

LazyCom 不分发内核驱动。常见设备由 Linux 内核模块支持：

- `cdc_acm`
- `ftdi_sio`
- `cp210x`
- `ch341`
- `pl2303`

安装和诊断文档必须说明：

- 串口设备组可能是 `dialout`、`uucp` 或发行版自定义组。
- 不建议以 root 或 `sudo` 运行 LazyCom。
- ModemManager 可能扫描并暂时占用串口。
- brltty 在部分系统上可能抢占 CH34x 设备。
- 应用不自动修改用户组、设备 mode、udev 规则或系统服务，也不调用 `chmod`、`usermod`、`sudo` 或任何提权接口。
- `Permission denied`、`busy`、设备消失和不支持参数必须显示不同错误。
- 权限 ErrorDialog 显示最终设备路径、设备 owner/group/mode、当前有效用户，并给出核对 `dialout`/`uucp` 组、重新登录使组成员生效、联系管理员配置 udev 或排查占用的建议；不能建议以 root 启动作为常规修复。
- 若安装或终端 smoke-test 文档使用 `script(1)` 记录终端会话，Fedora 对应软件包名写为 `util-linux-script`；这不是 LazyCom 运行时依赖。PTY 集成测试仍以 `socat` 或内部测试后端为主。

## 13. 测试与质量门禁

### 13.1 外部测试仓库

既有测试层次、条目及执行入口迁至 `../lazycom-test/docs/engineering-test-plan.md`。
该仓库保留六套完整 test presets 和 `gcc-debug-fast`，只在明确指令下使用。
本仓库不包含 `include(CTest)`、测试子目录或 test presets；CI 只构建和打包。
测试仓库不配置 push、pull request、定时或构建后的自动测试。

### 13.2 backend viability spike

在主要功能开发前完成独立验证：

1. 从锁定离线源码构建并确认 libserialport 0.1.2 shared library；另测显式系统依赖模式。
2. 通过 `sp_get_port_handle()` 获取 Linux fd。
3. 使用 `ppoll()` 同时等待 serial fd 和 eventfd。
4. 实际读写仍通过 libserialport nonblocking API。
5. 验证空闲时没有 `POLLOUT` 忙循环。
6. 验证 eventfd 可在 RX 空闲、TX 堵塞和队列满时立即唤醒 owner。
7. 验证 PTY HUP、USB 拔出、部分写和 `EAGAIN`。
8. 至少测试 FTDI、CP210x、CH34x 或 CDC ACM 中三类设备。

如果该 spike 发现 native handle 与 libserialport 0.1.2 冲突，必须停止实施并发起设计变更审核。只有同步修改 `Plan.md`、本文档、依赖策略和验收标准后，才可选择单一 Linux termios 原生后端；不能在实现中维护多套未经验证的 event loop。

### 13.3 性能与资源 spike

在核心 UI 开发前使用 Release 构建完成可重复 microbenchmark：

- 输入速率固定为 2 Mbaud 8N1 对应的 200000 bytes/s，分别测试 1、64、1024 和 65536 字节逻辑 frame 分布。
- 分别测量分帧、SessionRecord 构造、UI/log fan-out、UTF-8/base64 判定和 NDJSON 编码，不能只测端到端平均值。
- 记录 bytes/s、records/s、单核与总 CPU、RSS、管理预算、队列高水位、分配次数和 UI 命令延迟。
- serial owner 在已连接无数据且没有 deadline 时必须无限期阻塞在 `ppoll()`；稳定后没有周期性 owner wakeup。
- 2 Mbaud 且 frame 不小于 64 字节时不得发生 RX ingress 丢失，UI 命令处理 p95 目标不超过 50 ms。
- 1 字节 frame 用例用于确定并公开 records/s 极限；超过极限必须进入已定义 gap/日志停止策略，不能静默声称支持。
- 内部队列使用有界批次降低锁与唤醒；首版不以 benchmark 为由直接引入 lock-free queue。

### 13.4 编译门禁

收到明确的完整验证指令后，在测试仓库使用以下矩阵；未执行部分须报告为未验证：

```text
Debug + GCC + tests
Debug + Clang + tests
Release + GCC + tests
Release + diagnostics runtime on/off
ASan + UBSan + tests
TSan + concurrency tests
```

- 项目代码启用严格警告，第三方依赖警告不升级为项目错误。
- CI 中 `LAZYCOM_WARNINGS_AS_ERRORS=ON`。
- ASan/UBSan 与 TSan 分开运行。
- `gcc-asan-ubsan` 测试 preset 设置 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，首个未定义行为报告必须使测试失败，不能仅依赖 CTest 摘要判断 sanitizer 状态。
- Release 构建必须验证没有依赖 assert 才成立的用户输入检查。
- Release hardening 必须保留 capability check，并验证 LazyCom targets 获得可用的 PIE、stack protector、FORTIFY、RELRO、NOW 和 noexecstack；bundled libserialport 使用对应的受支持 CFLAGS/LDFLAGS。

历史验证范围见 [开发历史](docs/history.md#验证记录)。迁移前的通过记录不表示
当前源码或发布产物已经完成测试；sanitizer 版本编译成功也不表示执行过运行检查。
三类真实 USB-UART 和 8 小时 2 Mbaud 性能/RSS 验收仍是阶段 7 门禁。

### 13.5 推荐警告

GCC/Clang 至少考虑：

```text
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wsign-conversion
-Wshadow
-Wformat=2
-Wundef
-Wnon-virtual-dtor
```

警告集合由编译器 ID 和版本条件控制。不要为了零警告加入无意义强制转换；先确认类型和范围设计。

### 13.6 退出条件

一个开发阶段只有同时满足以下条件才算完成：

- 功能满足 `Plan.md` 对应行为。
- 单元和集成测试通过。
- 新错误路径有测试或故障注入覆盖。
- 队列、字符串、文件和输入尺寸有硬上限。
- ASan/UBSan 无问题；并发阶段 TSan 无项目数据竞争。
- 用户可观察错误有稳定文本和上下文。
- 新错误代码在注册表中唯一，具有 domain、稳定标识、底层 cause 映射和边界恢复测试。
- 新异步命令在接受时预留 completion，具有队列满、重复完成和停止竞态测试。
- 管理内存记账不超过 128 MiB，Release RSS 满足 `Plan.md` 的 +192 MiB 目标。
- 诊断路径不泄露串口内容，不阻塞 serial owner，日志关闭时不产生文件。
- 文档同步更新。
- UI 阶段还必须通过五态 Link 与颜色、HW 锁定生命周期、RX/TX 独立 View、Rosé Pine Moon RGB/ANSI 回退、rose-pine/transparent 背景、白色粗体快捷键、文本标签、唯一 Log、活动配置快照、预设 popup、两级 G、无穿透 ErrorDialog、权限设备行、Receive/Input 边框和焦点、稳定记录游标及 Vim 核心路由、拖选/OSC 52、发送成功清空/失败保留、空输入列 0 光标、动态双行 footer 和零尺寸/极小终端测试。

### 13.7 Catch2 边界

- 测试仓库固定使用 Catch2 3.x；Catch2 不保存在生产仓库，不链接或传递到 `lazycom` 生产可执行文件。
- Catch2 的 `CHECK`、`REQUIRE`、section 和 generator 适合状态机、边界值和字节流用例，因此当前不同时引入 GoogleTest。
- GoogleTest/GoogleMock 在大型 fixture、typed test、death test 和复杂 mock 生态上更成熟，但本项目优先使用轻量 fake backend 和子进程测试，暂不需要第二套框架。
- 测试 target 使用 `Catch2::Catch2WithMain` 和 `catch_discover_tests()`；需要自定义进程入口的 FATAL 测试链接 `Catch2::Catch2`。
- Catch2 源码和许可证与生产依赖分开管理，`LAZYCOM_BUILD_TESTS=OFF` 时不得配置或编译 Catch2。

## 14. 首版实施阶段

### 阶段 0：工具链和高风险验证（部分完成）

- 创建 CMake 骨架、options 和依赖发现。
- 建立 `include/dependencies/`、精确版本 lock、SHA-256 和许可证清单。
- 固定 FTXUI、libserialport 0.1.2、toml++、nlohmann/json、tl::expected、spdlog 和 Catch2 版本。
- 验证 bundled libserialport shared library、系统替换模式、RUNPATH、LGPL 源码和许可产物。
- 验证 spdlog 静态 target、随附 fmt 隔离、自定义安全 fd sink 及 diagnostics 编译开关。
- 验证 GCC 13、Clang 18、CMake 3.28 和当前开发机工具链。
- 完成 libserialport 0.1.2 `ppoll + eventfd` viability spike。
- 完成第 13.3 节 2 Mbaud、NDJSON、批次和空闲 wakeup performance spike。
- 建立 GCC、Clang、sanitizer 和基础测试 CI。

退出条件：PTY 和至少三类 USB-UART 验证 owner 等待模型可用，空闲无忙循环，停止可唤醒；性能报告给出可支持的 bytes/s 和 records/s 边界。

当前状态：CMake、锁定依赖、bundled libserialport、PTY viability、编译器和 sanitizer presets 已建立；至少三类真实 USB-UART 和完整 performance spike 尚未完成，因此阶段 0 的硬件/性能退出条件仍未全部满足。

### 阶段 1：核心模型和 schema（主要功能已实现）

- 实现 `Result<T>`、ErrorCode 注册表、Operation、底层 `std::error_code`、稳定错误标识、强类型 ID 和 generation。
- 实现第三方异常适配、线程入口 catch 边界、固定 FatalSignal、五类 WorkerStoppedSignal 和仅主线程执行的 FatalStopping 状态转换。
- 实现统一五态连接、交互、可恢复 overlay 和 Off/Waiting/Recording/Error 日志状态机。
- 实现三个 TOML 完整快照 schema、范围、128 MiB 组合预算和安全读取；加入 legacy 单一 Receive View 到 `rx_view`/`tx_view` 及废弃 `send.keep_after_send` 的显式迁移/移除。
- 固定 NDJSON v1 header/record schema、UTF-8/base64 和 barrier 语义。
- 实现假时钟、假文件系统和测试数据 builder。

退出条件：状态机和 schema 边界测试完成，不依赖 FTXUI 或真实串口。

### 阶段 2：串口 owner（主要功能已实现）

- 实现 libserialport RAII 和立即错误复制。
- 实现设备扫描、权限元数据/预检和仅扫描结果选择；明确 PTY 路径只通过内部测试接口。
- 实现常驻 owner、eventfd、ppoll、控制通道、有界命令队列和预留容量的 completion mailbox。
- 实现连接、取消、非阻塞 RX/TX、部分写和断开 barrier。
- 实现 OperationId 生命周期匹配、SessionId 数据过滤和假后端故障注入。

退出条件：连接中取消、设备消失、普通事件队列满、可靠 completion、部分写和停止测试通过，状态机不会停留在过渡态。

### 阶段 3：数据路径（主要功能已实现，生产全局预算接入除外）

- 生产 `SessionRecords` 已使用 immutable `SessionRecord` 及其预算 token 和 `SessionSequencer`；UI/log 分别执行逻辑配额，运行时其余类别的全局预算接入仍待阶段 7 完成。
- 实现 CR、LF、CRLF、idle 和最大帧状态机。
- 实现安全 UTF-8、HEX 和 MIXED 表示、RX/TX 独立 display view，以及仅含有界 TXT message 的 SYS/ERR。
- 实现 session sequencer、严格 seq、独立 UI/log fan-out 和 seq gap。
- 实现 TX operation ID、deadline 和不可交错语义。

退出条件：随机字节流分帧后可无损重建，全部 0..255 字节安全显示测试通过。

### 阶段 4：核心 UI（主要功能已实现）

- 实现 app command facade 和 view model。
- 实现 Normal、SendEdit、ReceiveBrowse 和 overlay 栈。
- 实现集中式 Rosé Pine Moon palette、rose-pine/transparent renderer、粗体白色快捷键、结构化独立着色的五态及 HW 锁定顶部状态栏、带显式焦点标题的 Receive/Input 边框面板、受控多行发送 Input 和通知。
- view model 提供实际活动串口配置快照、RX/TX 独立 View、稳定 current record ID、发送焦点/光标/占位状态，以及分离的运行指标和动态上下文 footer。
- 实现 `P/B/D/N/V/H` 强类型预设 popup、两级 Session Log Settings candidate、红色无穿透 ErrorDialog、权限拒绝设备行和来源焦点恢复。
- 实现滚动、暂停、搜索、过滤、FTXUI 普通鼠标拖选、OSC 52 复制、1 MiB cap 和帮助页；实现 `j/k`、count+j/k、`gg`、`G`、`yy` 和上下文前缀隔离，SendEdit 保持鼠标光标/草稿选择行为。
- 实现成功 Enter 提交后始终清空、失败时保留、Alt+Up/Down 历史，以及 Backspace 删除最终字符后列 0/视口起点归一化。
- 完成 `Plan.md` 第 14 节快捷键和事件优先级。

退出条件：所有输入上下文测试通过，F1 唯一无条件且 ErrorDialog 无按键穿透，ReceiveBrowse 待定前缀不泄漏，后台事件不抢焦点；标题与顶部状态行无重复 Link/Log，五态名称、HW 锁定范围、RX/TX 独立 View、颜色和活动配置快照正确，持久 ErrorDialog 不伪造 Link；Receive/Input 边框及焦点标题、稳定一记录一行游标、Vim 核心命令、成功发送清空/失败保留、历史和空输入列 0 光标符合计划；所有 popup 和 G 事务符合计划；拖选、滚轮、SendEdit 鼠标、Normal `y`/ReceiveBrowse `yy`/Ctrl+C、OSC 52 cap 通过；两行 footer 随 Normal/Input/Browse 动态变化；rose-pine/transparent、精确 RGB、白色粗体快捷键、ANSI 回退、文本标签和极小终端安全渲染测试全部通过。

### 阶段 5：日志和持久化（会话日志与持久化主路径已实现）

- 实现 NDJSON v1 header/record codec、UTF-8/base64 无损恢复和截断尾行处理。
- 实现 Off/Waiting/Recording/Error 日志状态机及 OFF/WAITING/REC/ERROR UI 映射、有界记录队列、flush 和 `processed_through_seq` barrier。
- 实现安全创建、轮换、目录锁、配额和损坏尾行处理。
- 实现 Session Log Settings 完整 candidate 持久化、Directory Enter 安全验证、下一会话策略和 Recording 立即 rollover 流程。
- 实现 TOML 原子写入、三态提交、冲突检测、完整快照验证和只读保护。
- diagnostics 当前仅实现编译开关、spdlog 链接探针、`emergency_write()` 和 terminate/FATAL 最小路径；项目 diagnostics worker、自定义安全 fd sink、脱敏、轮换和丢弃计数仍待实现。
- 实现主线程 FATAL coordinator、worker FatalSignal、completion deadline、应急写入和 abort 子进程测试。
- 完成磁盘满、权限、符号链接和 fsync 故障注入。

当前会话日志顺序、原始数据恢复、FATAL 主线程协调及 deadline 后 abort 路径已有实现和测试；完整诊断日志退出条件尚未满足。

### 阶段 6：快捷发送和定时任务（主要功能已实现）

- 实现 20 个快捷发送槽和安全持久化。
- 实现单次发送和 10ms 至 24h best-effort 定时任务。
- 实现 replacement confirmation、missed、停止和 task generation。
- 将 scheduler 作为 owner 内部 deadline 状态机集成。

退出条件：停止确认后不会再出现该 task generation 的写入。

### 阶段 7：发布加固

- 执行 2 Mbaud、8 小时、日志轮换和 UI 压力测试。
- 验证 128 MiB 管理预算、RSS +192 MiB、1/64/1024/65536 字节 frame 分布和已声明 records/s 边界。
- 完成 ASan、UBSan、TSan 和编译器矩阵。
- 编写安装、权限、配置及 legacy 字段迁移、日志格式、快捷键、错误代码和故障诊断文档。
- 编写不进入普通用户帮助页的内部诊断启用、日志收集和 core dump 支持文档。
- 生成依赖及许可清单。
- 验证 `Plan.md` 全部首版验收标准。
- 在已接入的共享记录路径之外，补齐 ingress、TX、草稿/历史、scratch、配置、搜索、控制槽等类别的预分配预算与生命周期归还，完成 128 MiB 全局 token 闭环。
- 完成 diagnostics 有界队列、worker、安全 fd sink、轮换、脱敏、运行时启用和故障注入测试。

## 15. 第二阶段工程升级

### 15.1 多标签

- 把单会话 owner 抽象为受全局预算约束的 session runtime。
- 每个标签拥有独立 generation、owner、队列和日志状态。
- 增加最大线程和串口数量硬上限，所有标签默认共享首版 128 MiB；扩大总预算必须先更新 `Plan.md`、RSS 指标和验收测试。
- 后台标签只提交状态和徽标事件，不能操作焦点。

### 15.2 外部编辑器

- 使用 `posix_spawnp()`，不在多线程进程 fork 后运行 C++ 逻辑。
- 私有临时目录和文件分别使用 0700、0600。
- 无关 fd 全部设置 `CLOEXEC`。
- 回填前验证普通文件、所有者、大小和 UTF-8。

### 15.3 GB18030

- 通过 `find_package(Iconv)` 引入系统 iconv。
- 新增流式 decoder 接口，不改变原始字节事实来源。
- 测试跨块多字节序列、非法输入、状态重置和帧边界。

### 15.4 线路控制和设备身份

- DTR、RTS、Break 继续通过 serial owner 操作。
- 加入 libudev/sysfs 辅助模块，核心 model 不依赖 udev 类型。
- 稳定身份包含 interface number 和 `ID_PATH`，不只依赖 VID/PID/serial。
- 自动重连仍不随稳定身份功能自动加入。

## 16. 工业协议工作台候选研究（非规范性）

本节仅记录未来技术研究方向，不构成产品范围、版本承诺或实施授权。只有先在 `Plan.md` 完成独立产品审核并同步资源与验收标准后，才能启动对应开发。若获批准，所有协议能力仍须叠加在原始字节会话之上，不能替代或绕过现有 owner、日志、安全和配额模型。

### 16.1 协议核心

- 定义内部 `FrameDecoder`、`FrameEncoder`、`Checksum`、`ProtocolDecoder` 和 `TransactionMatcher` 接口。
- 协议结果引用原始块的 offset/length，不复制或修改原始事实数据。
- 支持 delimiter、固定长度、长度字段、静默间隔、SLIP 和 COBS 分帧。
- 支持参数化 CRC8、CRC16、CRC32、Modbus CRC16 和 LRC。
- 对 frame size、字段数、树深度、解析时间和诊断数量设置硬上限。
- 第一版只提供内置模块，不承诺公共 C++ 或动态插件 ABI。

SLIP、COBS 和 CRC 实现规模较小，应作为纯函数内部实现并使用标准测试向量，不为它们增加大型依赖。

### 16.2 Modbus RTU 主站

- 首先实现主站，不在同一版本加入从站模拟。
- 支持功能码 01、02、03、04、05、06、0F 和 10。
- 支持正常响应、异常响应、广播地址、CRC 校验和地址过滤。
- 事务包含 ID、请求快照、deadline、匹配器、重试策略和 generation。
- 写操作默认不自动重试，避免设备已执行但响应丢失造成重复副作用。
- 同一串口默认只有一个活动 Modbus 请求响应事务。
- 显示站号、功能码、地址、数量、寄存器、原始偏移和 CRC 状态。
- 字节序和浮点解释属于显示模板，不能改变原始寄存器值。

RTU 静默间隔使用单调时钟。必须说明 Linux 用户态只能观察应用读取时间，不能声称获得物理线路逐字节时间。主站响应解析优先利用预期长度、功能码和 CRC，静默间隔作为边界和恢复信号。

不让 libmodbus 打开或控制串口。可以在测试工具中使用 libmodbus 做差分验证，但生产二进制不链接它。

### 16.3 通用请求响应事务

- 支持超时、有限重试、匹配规则、断言和统计。
- 手工、快捷、定时和事务发送只在逻辑 TX 请求边界仲裁。
- 协议未匹配数据仍作为普通 RX 可见，不能被事务引擎吞掉。
- 取消、断开、脚本停止或协议切换使事务 generation 失效。
- 所有重试必须记录原因和次数。

### 16.4 协议字段视图

- 树形显示字段名称、值、类型、单位和校验状态。
- 每个字段关联原始 byte offset 和 length。
- 字段选择与 HEX 区域双向高亮。
- 畸形帧显示已成功解析字段和准确错误位置，同时保留完整原始帧。
- 协议 UI 只消费通用字段树，不直接依赖具体 Modbus 类型。

### 16.5 通用协议工作台

- 提供可配置 delimiter、长度字段、大小端、转义、校验和字段模板。
- 配置 schema 版本化，导入内容视为不可信输入。
- 模板编译与运行分开；运行时只使用通过校验的不可变表示。
- 模板不能执行任意系统调用或绕过发送确认。
- 首期不追求 Wireshark 等级的协议描述语言。

### 16.6 Lua 自动化

- 协议核心稳定后引入 Lua 5.4。
- 支持连接、发送、等待匹配、事务、断言、变量、条件和循环。
- 脚本调用转换为正常 app command，不能直接操作 fd、`sp_port*`、日志 fd 或 FTXUI。
- 脚本任务具有 generation，可停止且受连接生命周期约束。
- 对脚本内存、指令数、运行时间、输出量和 outstanding transaction 设置限制。
- 本地脚本默认视为用户可信代码，不虚假宣称 Lua 环境是安全沙箱。
- 若未来需要执行不可信脚本，应改为受限子进程，不在主进程增加伪沙箱。

### 16.7 插件平台

- 内置协议接口和字段 schema 稳定后再设计插件协议。
- 不直接暴露 C++ ABI，也不让插件动态库进入主进程地址空间作为第一方案。
- 优先进程外插件和版本化 IPC，以隔离崩溃、资源消耗和许可证。
- 插件输入是有界原始块、会话元数据和配置快照。
- 插件输出是字段树、诊断、匹配结果或发送建议。
- 插件发送建议仍需通过 app 状态守卫和 owner 队列，不能直接写串口。
- IPC 必须有消息尺寸、队列、超时、协议版本和进程退出处理。

### 16.8 协议测试

- 使用标准 CRC、SLIP、COBS 和 Modbus golden vectors。
- 对协议 decoder 执行随机字节和截断帧 fuzz test。
- 验证畸形长度字段不会导致越界或超大分配。
- 验证 timeout、异常响应、CRC 错误、未匹配帧和取消竞态。
- 用外部设备或模拟器测试 Modbus RTU 常用功能码。
- Lua 测试覆盖停止、资源上限、脚本异常和连接 generation 失效。
- 插件测试覆盖恶意尺寸、超时、崩溃、错误版本和退出重启。

本轮未选择录制与确定性回放，因此不把该能力纳入工业协议工作台范围。未来如增加，必须单独设计单调时间和日志格式升级。

## 17. 版本和发布策略

### 17.1 版本阶段

- `0.x`：内部架构和首版功能允许不兼容调整。
- `1.0`：完成 `Plan.md` 首版验收，稳定配置和日志主版本。
- `1.x`：第二阶段多标签、编码、线路和设备身份。
- 工业协议工作台、Modbus、Lua 和插件尚未进入规范性版本路线；版本号只能在 `Plan.md` 批准产品范围后确定。
- 若未来经 `Plan.md` 产品审核批准，再单独确定应用版本、Lua 交付阶段和插件协议版本，不能从本候选研究章节推导承诺。

### 17.2 兼容性

- 首版没有公共 C++ ABI。
- 配置和日志通过显式 schema version 管理。
- 不为尚未发布的内部类添加无依据兼容层。
- 已发布 schema 的迁移必须是显式、可测试且不覆盖原文件。
- 新功能默认不能扩大已有内存、文件或权限边界；确需扩大时必须经过独立审核并同步新的数值硬上限、RSS 指标和验收测试。

### 17.3 发布产物

- Linux 可执行文件或发行版包。
- 安装与运行依赖说明。
- 配置 schema 和示例。
- 日志格式说明。
- 快捷键和故障诊断文档。
- 稳定错误代码目录和内部诊断支持说明。
- 第三方依赖版本和许可清单。
- LazyCom 自身使用 GPL-3.0-only，发布包携带根目录 `LICENSE`；第三方组件保留原许可。
- libserialport 0.1.2 shared library、对应源码、LGPL 许可和动态替换说明。
- 已知限制，包括 native handle 风险和非实时定时语义。

### 17.4 自动预发布

`.github/workflows/release.yml` 在每次分支 push 和手动触发时，使用 Ubuntu 24.04
x86_64、GCC 13 和 Clang 18 执行 GCC Debug、Clang Debug、GCC Release、
no-diagnostics 和 ASan/UBSan 的生产构建。构建与打包成功后发布该次构建提交的
GitHub prerelease；Pull request 只验证和保留 Actions 产物。

每次运行使用独立 `build-<run_id>-<run_attempt>` 标签，上传名为 `lazycom` 的
DEB、RPM、AppImage、tar.gz 和统一 SHA-256 校验文件，不覆盖已有发布。包版本使用
`<项目版本>~pre.<run_number>.<run_attempt>.g<commit前12位>`，支持原生包升级。

`Runtime` 安装组件将程序安装到 `bin/lazycom`，将 bundled libserialport 安装到
应用私有 `lib/lazycom/`，同时携带对应源码、许可、文档及 `Terminal=true` 桌面项。
安装 RUNPATH 为 `$ORIGIN/../lib/lazycom`。该组件仅支持 Linux 默认 vendored 模式。
CPack 原生包前缀为 `/usr`，自动生成系统运行库依赖；私有库不作为公共 RPM 提供。
AppImage 工具和运行时固定版本及 SHA-256，下载只发生在显式打包步骤。

安装、启动、卸载和 AppImage 验证脚本及原有发行版矩阵由 `../lazycom-test` 管理，
仅在明确指令下执行。本仓库发布工作流不等待或宣称测试通过；发布说明必须明确
自动流程只完成构建与打包。真实 USB-UART 与长时性能验收仍未完成。

自动预发布不覆盖 TSan 运行检查、AppImage FUSE 挂载或桌面启动。
触发条件、下载、运行及动态库替换方法见 [自动构建与预发布](docs/releases.md)。

### 17.5 Fedora 原生 RPM 与 COPR 源码入口

`packaging/lazycom.spec` 使用 Fedora 工具链从 SRPM 构建，保留默认固定版本依赖，
不引入系统依赖替换或 configure-time 下载。CMake 安装目录支持
`CMAKE_INSTALL_LIBDIR`，默认 `lib`，Fedora spec 显式选择 `%{_lib}`；私有串口库
RUNPATH 根据 bin/lib 相对位置生成。原生 RPM 将许可证和文档放入发行版标准目录，
并保留 libserialport 对应源码。RPM 管理 strip 和 debuginfo，安装阶段不提前 strip。

libserialport 的 ExternalProject 继承 CMake C 编译参数、当前构建类型参数及共享库
链接参数，以保留发行版的调试和加固设置。依赖仍只构建 shared library。

`.copr/Makefile` 的 `srpm` target 调用 `packaging/build-srpm.sh`，从干净的已跟踪
工作树归档 `HEAD` 并注入完整提交号。`vX.Y.Z` 标签指向该提交时生成正式版本；
否则生成 `X.Y.Z~pre.<提交UTC时间>.g<commit前12位>`。CMake 与 spec 的上游版本
必须一致，RPM Release 含 `%{?dist}`。SRPM 自包含生产构建所需源码与固定依赖。

spec 不提供 `%check` 或任何测试入口，源码生成和打包不依赖测试仓库。COPR 项目
创建、目标 chroot、凭据及上传由维护者管理；生产仓库不自动触发 COPR 发布。

## 18. 首次开工顺序

项目开始实施时按以下顺序执行：

1. 检查工作区和工具链，不修改 `Plan.md` 已确认行为。
2. 创建 CMake 骨架和最小 `main.cpp` 构建目标。
3. 建立 `include/dependencies/` 并校验 header-only 依赖版本、SHA-256 和许可。
4. 固定并验证 FTXUI、bundled libserialport 0.1.2 shared library、spdlog、Catch2 及其许可和 target 边界。
5. 创建 Catch2 smoke test、diagnostics smoke test 和编译器/sanitizer presets。
6. 单独实现 libserialport `ppoll + eventfd` viability spike 和第 13.3 节 performance spike。
7. 只有串口与性能 spike 都通过并记录边界后，进入核心模型和完整 owner 实现。
8. 每个阶段按第 14 节退出条件完成，不跨阶段堆积未验证基础设施。

当前已知环境工具链满足计划基线，bundled libserialport 0.1.2 shared library 已纳入各构建 preset。`gcc-tsan` preset 已配置，要求可用 libtsan 运行库，并与 ASan/UBSan 分开运行；真实三类 USB-UART 和 8 小时性能/RSS 验证仍待具备硬件与运行环境后完成。
