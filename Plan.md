# LazyCom TUI 串口助手设计计划

## 1. 文档状态

- 当前阶段：阶段 1 至阶段 6 的主要功能和本轮安全加固已经实现；阶段 7 的真实硬件、长时性能、完整内部诊断和发布验收仍待完成。
- 当前程序已包含 CMake 工程、串口 owner、数据路径、核心 TUI、会话日志、配置持久化、快捷发送和定时任务，不再处于“尚未实施”状态。
- 测试代码与测试清单由独立 `../lazycom-test` 仓库维护；本仓库不新增测试项，不自动运行测试。新增或执行测试须有用户明确指令。`docs/test-cleanup-results.md` 仅记录历史验证，不能作为迁移后当前源码的通过证据。
- 生产记录路径已接入 `SessionSequencer`、共享 payload 和记录引用预算，UI 与日志复用不可变记录；其余运行时类别尚未全部接入，128 MiB 全局 token 仍未闭环。
- diagnostics 当前只有编译开关、`emergency_write()` 和 terminate/FATAL 所需的最小路径；完整内部诊断队列、worker、安全 sink、轮换和脱敏链路仍待实现。
- 首版目标平台为 Ubuntu 24.04 同代或更新的现代 Linux。

## 2. 产品定位与版本范围

LazyCom 是一款使用 C++ 和 FTXUI 开发的 TUI 串口助手，面向嵌入式设备、调试串口和工控设备的数据收发与诊断。

### 2.1 首版范围

- 单串口会话，不开放多标签。
- 类 Neovim 的 Normal、编辑和弹窗交互模型。
- 串口扫描、预设设备选择、连接、断开及状态反馈。
- 常见串口硬件参数配置。
- UTF-8 安全文本和 HEX 数据收发。
- RX 和 TX 各自独立的 TXT、HEX、MIXED 接收区显示；SYS 和 ERR 固定为 TXT。
- RX、TX、系统事件和错误的明确区分。
- 可滚动、可暂停跟随、可搜索和可过滤的接收区。
- 20 个持久化快捷发送槽。
- 单次发送和受限范围的定时发送。
- 有界内存、发送队列、日志队列和后台事件模型。
- 版本化 NDJSON 会话日志。
- TOML 默认配置、快捷发送配置和运行偏好。

### 2.2 第二阶段

- 多标签独立串口会话。
- 标签连接状态图标。
- 通过命令面板调用外部 TUI 编辑器。
- GB18030 编码。
- DTR、RTS、Break 主动线路控制及线路输入状态。
- 更稳定的 USB 身份和设备重插识别。
- 人类可读导出。
- 日志离线查看和分析。

### 2.3 暂不纳入

- 终端键盘接管模式。
- ANSI、VT100 或其他终端模拟功能。
- 自动重新连接。
- 自动恢复上次连接和运行中的定时任务。
- Windows 和 macOS 正式支持。

终端接管与终端模拟兼容性复杂，未来如重新引入，应作为独立模块设计。

## 3. 已确认的核心决策

- 应用启动和连接成功后均保持 Normal 状态，不自动进入编辑态。
- 发送草稿在断开时也允许编辑，但不能发送。
- Enter 发送成功后始终清空输入；发送失败或未连接时保留草稿，历史仍可通过 Alt+Up/Down 取回。
- 连接后只锁定串口硬件参数。
- 接收显示、发送模式、换行、日志运行开关等运行参数可以在线修改。
- 定时发送周期为 0 或 10 至 86400000 毫秒。
- `interval_ms = 0` 表示单次发送。
- `interval_ms = 10..86400000` 表示尽力而为的定时发送，不提供实时性保证。
- 外部编辑器、多标签、GB18030 和主动线路控制移到第二阶段。
- 首版日志必须明确记录顺序和时间语义，不能声称无法证明的物理线路送达。
- 首版以 Ubuntu 24.04 档为最低环境：Linux kernel 6.8、glibc 2.39、GCC 13 或 Clang 18、CMake 3.28。
- 串口使用 libserialport 0.1.2，借用 native fd 仅用于 `ppoll()` readiness 和连接后 `fstat()`。
- 各消费端过载相互隔离：UI 显示 gap、日志安全停写；只有原始 RX ingress 已知丢失才断开会话。
- 应用管理的动态内存硬预算为 128 MiB，RSS 验收目标为空载基线增加不超过 192 MiB。

## 4. 技术选型

### 4.1 语言与构建

- C++20。
- CMake。
- Linux 优先。
- Release/RelWithDebInfo/MinSizeRel 在编译器和链接器支持时启用 PIE、`-fstack-protector-strong`、`_FORTIFY_SOURCE=3`、RELRO、NOW 和 non-executable stack；选项先经 CMake 能力检查，不把不支持的参数硬编码为所有平台前提。
- 已提供独立 `gcc-tsan` configure/build presets；test presets 由 `../lazycom-test` 维护；TSan 与 ASan/UBSan 互斥。

### 4.2 首版依赖

- FTXUI：TUI 渲染、输入、鼠标、弹窗和主循环。
- libserialport 0.1.2：串口枚举、配置和非阻塞读写。
- toml++：TOML 解析和应用管理文件写入。
- nlohmann/json：NDJSON 会话日志编码和校验。
- tl::expected：C++20 的显式错误返回。
- spdlog：当前用于 diagnostics 编译/链接 facade；完整内部诊断日志 worker 待阶段 7 实现，不承载用户会话日志。
- Catch2 3.x：仅由独立 `../lazycom-test` 仓库提供，生产仓库不依赖它。

首版不需要 iconv。GB18030 加入第二阶段时再引入系统 iconv。

### 4.3 依赖策略

- toml++、nlohmann/json 和 tl::expected 的固定源码放在 `include/dependencies/`，构建时不联网下载。
- FTXUI、spdlog 使用仓库内固定源码，发布构建必须支持离线源码包；测试仓库独立保存固定 Catch2。
- 非 header-only 依赖的系统模式默认关闭；用户显式启用时必须匹配锁定兼容版本并通过对应 smoke test，不能用任意系统版本替代已验证源码。
- libserialport 固定以 shared library 动态链接；默认构建随附 0.1.2，只有明确验证的系统 0.1.2+ 才可替换。
- 发布包保留 libserialport 的 LGPL-3.0+ 许可、对应源码和动态替换条件。

## 5. 正式状态模型

应用状态拆成三个正交维度，不能使用一个枚举混合全部行为。

### 5.1 连接状态

```text
Disconnected -> Connecting
Connecting -> Connected | Error | Disconnecting(cancel)
Connected -> Disconnecting | Error
Error -> Disconnecting
Disconnecting -> Disconnected
```

- `Disconnected`：未打开串口，允许修改硬件参数。
- `Connecting`：正在打开和配置串口，禁止重复连接和硬件配置。
- `Connected`：串口可用，只锁定硬件参数。
- `Disconnecting`：正在停止任务和关闭资源，禁止重新连接和硬件配置。
- `Error`：连接或 I/O 错误的瞬时状态，记录原因后进入 Disconnecting 完成清理，再回到 Disconnected。
- Connecting 期间再次请求连接控制表示取消；资源清理完成前不能启动新 generation。
- 内部枚举、事件、测试和 UI 的 Link 文本统一且仅使用 `Disconnected`、`Connecting`、`Connected`、`Disconnecting`、`Error` 这五个名称。状态机处理失败时必须经过 Error；若 Error、清理和 completion 在同一 UI tick 内完成，Link 可以直接渲染 tick 结束时的真实生命周期状态。
- `ErrorDialog` 可以在 Error 经 Disconnecting 回到 Disconnected 后继续保留错误详情；此时 Link 必须显示真实生命周期状态 `Disconnected`，不能为了保留错误提示而继续显示或伪造 `Error`。

### 5.2 交互状态

- `Normal`：默认状态，解释管理、配置和运行快捷键。
- `SendEdit`：发送输入框编辑状态，普通字母全部作为内容。
- `ReceiveBrowse`：Receive 面板焦点状态，保留稳定的当前可见记录游标，滚动、Vim 核心命令及其待定前缀由接收区优先处理。

应用启动、连接成功和断开完成后保持 Normal。连接成功只更新状态并显示提示，不抢占输入焦点。

断开状态允许进入 SendEdit 准备草稿。提交发送时如果未连接，应保留草稿并显示“未连接”，不能创建 TX 记录。

Normal 下按 `R` 聚焦 Receive 面板并进入 `ReceiveBrowse`；进入后内部交互状态保持 `ReceiveBrowse`，Esc 明确返回 Normal。ReceiveBrowse 的 `g`、`y` 和十进制 count 待定前缀属于接收区上下文，必须先于 Normal 或全局动作处理，不能把前缀或其后续按键泄漏为连接、日志、退出、复制选择等动作。

### 5.3 覆盖层状态

- `None`
- `Search`
- `Modal`
- `Confirm`
- `ErrorDialog`
- `Help`
- `CommandPalette`

覆盖层使用可恢复栈而不是单值枚举。Help 可以临时覆盖 Search、Modal、Confirm 或 ErrorDialog，并保存完整来源覆盖层、字段值、焦点及交互状态。Help 再次触发或取消后恢复来源。命令面板关闭后恢复来源状态；配置、运行和确认弹窗关闭后返回 Normal。

### 5.4 事件优先级

输入事件按以下顺序路由：

1. 活动覆盖层。
2. 活动文本编辑组件。
3. 当前交互状态。
4. 有限的全局快捷键白名单。

F1 是唯一无条件应用快捷键。除 F1 外，应用管理、命令面板、退出和复制等动作不得穿透活动文本输入或模态弹窗；`Ctrl+C` 只有在终端确实转发该按键、当前没有更高优先级组件消费且接收区存在非空选择时才执行复制。

ReceiveBrowse 的待定 `g`、`y`、count 前缀视为当前交互状态的一部分。有效后续键完成对应接收浏览命令；无效后续键由 ReceiveBrowse 消费并清除前缀，不得继续路由到 Normal 或其他应用动作；Esc 清除前缀并返回 Normal，F1 仍按全局规则打开 Help。

鼠标操作必须调用与键盘相同的命令和状态守卫，不能绕过连接状态、配置锁或确认流程。

### 5.5 异步操作标识与代次

- 每次连接请求生成新的 `connection_generation`，成功打开后再创建并关联新的 `session_id`；两种 ID 不能互相转换或替代。
- `session_id` 在 Disconnecting 完成前保持有效。开始断开后拒绝新的用户 TX 请求和新的常规业务读取，但允许 owner 提交同一 session 的已接受 TX 前缀、pending CR、RX 尾帧及最终 SYS/ERR 清理记录。
- 连接、发送、断开、扫描、保存和日志 barrier 都有独立 `operation_id`，生命周期完成事件按 operation 匹配，不能仅因 generation 失效而丢弃。
- RX、TX 和会话错误携带 `session_id`；模型只接受当前会话且当前状态允许的数据事件。
- 迟到数据事件只能释放旧资源，不能修改新连接；旧操作的幂等完成事件可以安全忽略。
- 定时任务继续使用独立 task generation，不能与连接 generation 混用。

## 6. 主界面设计

### 6.1 顶部会话标题

首版仅有一个会话，顶部标题只显示 `LazyCom` 或会话名称，不重复显示 Link、Log 或配置状态。

### 6.2 顶部配置状态栏

标题下方使用唯一权威的顶部状态行：

```text
Status:Normal | Link<C>:Disconnected | HW:UNLOCKED | [Port<P>:/dev/ttyUSB0] | [Baud<B>:115200] | [Serial<D>:8N1 Flow:None] | [NewLine<N>:CRLF] | [View<V>:RX=TXT TX=HEX/RX+TX+SYS+ERR] | [InputType<H>:TXT] | [Log<g/G>:OFF]
```

- 显示交互状态和连接状态，两者不能混为一个字段。
- Link 文本与内部状态严格同名，只能是 `Disconnected`、`Connecting`、`Connected`、`Disconnecting`、`Error`。`Disconnected` 只表示当前没有活动串口连接，不一定是故障；持久错误原因由红色 `ErrorDialog` 显示，不能改写 Link。
- 未选择设备时显示 `Port<P>:Unset`。
- `Connecting`、`Connected`、`Disconnecting` 和 `Error` 期间，设备、波特率和串口硬件字段显示该次活动连接实际使用或尝试使用的配置快照，而不是被外部修改的新默认值；只有 `Disconnected` 显示待用配置。
- `HW:LOCKED` 表示设备、波特率、数据位、停止位、校验和流控在非 Disconnected 的连接生命周期中锁定，即 Connecting、Connected、Disconnecting 和 Error 均不可修改；断开清理完成进入 Disconnected 后显示 `HW:UNLOCKED` 并恢复可编辑。
- 顶部配置字段固定命名为 `[NewLine<N>]`、`[View<V>]`、`[InputType<H>]` 和 `[Log<g/G>]`，不使用 NL 或 Send 等别名。
- `[View<V>]` 同时显示独立的 RX/TX 模式和 RX/TX/SYS/ERR 可见性，模式标签固定为 TXT、HEX、MIXED。
- 窗口变窄时依次优先保留 `Status`、`Link`、`HW`、`Port`、`Baud`，再按 `Serial`、`Log`、`NewLine`、`View`、`InputType` 的顺序保留，其余字段可以隐藏但不能产生第二条状态行。

### 6.3 接收记录区

Receive 使用与 Input 对应的有边框面板并占据主要空间：

- 未聚焦时标题必须明显显示 `<R> Receive`，使用 Subtle 非活动边框和标题；Normal 下按 `R` 聚焦面板并进入 ReceiveBrowse。
- 聚焦时内部状态保持 `ReceiveBrowse`，标题显示 `Receive [Browse]` 及最相关浏览提示，边框和标题使用 Iris 活动样式；焦点差异同时依赖标题文本和边框，不能只依赖颜色。Esc 取消待定前缀并返回 Normal，恢复未聚焦标题和样式。
- 每一物理显示行恰好对应一条当前过滤后可见的 RX、TX、SYS 或 ERR 记录；MIXED 的 TXT 与 HEX 表示也必须保持在同一条记录行内，不能拆成两条游标行。
- ReceiveBrowse 保存按稳定 record ID 标识的当前记录游标，并用固定文本游标标记及 Iris 样式明确标出当前行，不能只靠颜色。新记录到达、普通重绘和仅视口滚动不能任意改变当前记录；记录被过滤或淘汰时移动到最近的仍可见记录，无可见记录时明确为空游标。
- `j`/`k` 分别向下/向上移动当前记录，十进制 count 前缀只适用于 `j`/`k`，例如 `10j`、`11k`；count 有界且不得用于 `yy`。`gg` 移到最早可见记录，`G` 移到最新可见记录，`yy` 复制当前记录的完整已渲染安全文本。
- `yy` 复用现有有界 OSC 52 路径，编码前 payload 上限仍为 1 MiB，超限拒绝、不截断且游标不变；空游标时无动作。
- 单个 `g`、单个 `y` 和尚未完成的十进制 count 只建立 ReceiveBrowse 待定前缀。有效序列完成命令；无效后续键被 ReceiveBrowse 消费并清除前缀，不能触发 Normal 或全局动作。

接收区还支持：

- 键盘和鼠标滚动。
- 自动跟随最新记录。
- 手动暂停。
- 搜索和上一个、下一个匹配。
- 搜索在提交时固定查询、方向、显示模式和当前记录范围，分批更新结果并显示进行中状态；期间可导航已找到的匹配。再次提交或切换方向替换原查询，退出搜索取消剩余工作，新到达的记录需再次提交后才参与匹配。已淘汰记录不保留在结果中，结果最多 10000 条。
- RX、TX、SYS、ERR 各自独立的可见性开关，至少保留一个方向/类型。
- RX 和 TX 分别独立选择 TXT、HEX、MIXED 并可在线修改；SYS 和 ERR 始终以 TXT 显示，不受 RX/TX 模式影响。
- 明确清空当前会话全部内存记录。
- Normal 和 ReceiveBrowse 中使用 FTXUI 的屏幕选区对当前已渲染安全文本直接按住鼠标左键拖动选择，不要求 Shift、Alt 等修饰键；原有鼠标滚轮滚动继续可用。
- SendEdit 中鼠标仍用于放置和拖动发送编辑器光标或选择草稿，不得被接收区选择手势劫持。

自动跟随状态由两个条件组成：

```text
auto_follow = !manual_pause && at_bottom
```

- 向上滚动只改变 `at_bottom`，不能取消或创建手动暂停。
- 回到底部不能取消手动暂停。
- 搜索旧记录时进入临时浏览状态并暂停自动跟随。
- 退出搜索时允许保持当前位置或返回最新记录。
- 选择内容取自拖动完成时屏幕上可见的安全文本；普通键盘或滚轮滚动不主动清除已捕获内容，其他已处理的 UI 动作可以开始新的选择或清除旧选择。
- rose-pine 背景模式下，非空选择使用 Iris 背景和 Base 前景；transparent 模式下不发出背景色，改用反选和双下划线。颜色不能成为唯一选择标识。
- Normal 中 `y` 是可靠的选区复制键；ReceiveBrowse 中 `y` 是 `yy` 的待定前缀，只有 `yy` 复制当前记录，不能因存在选区而把第一个 `y` 解释为选区复制。终端转发 `Ctrl+C` 时，仅在接收区选择非空时复制选区。两种复制都通过同一受控 OSC 52 路径，编码前 payload 硬上限为 1 MiB，超限拒绝且不截断。
- `Ctrl+Shift+C` 通常由终端模拟器自身拦截并复制终端原生选择，应用无法可靠接收或绑定；帮助页必须说明该限制，不把它列为 LazyCom 权威快捷键。

### 6.4 消息标识

不能只使用颜色区分：

```text
← RX  [12:30:01.125] READY\r\n
→ TX  [12:30:03.442] AT+INFO\r\n
! SYS [12:30:04.001] Serial device disconnected
× ERR [12:30:04.006] Write failed: device unavailable
```

- RX 使用 `← RX`。
- TX 使用 `→ TX`。
- 系统事件使用 `! SYS`；SYS 表示应用或系统生命周期事件，只承载 TXT 文本消息，不表示串口原始字节。
- 错误使用 `× ERR`；ERR 表示错误事件，只承载 TXT 文本消息，并可关联稳定错误代码。

### 6.5 发送区

- 未聚焦时标题为 `<i> Input [TXT]` 或 `<i> Input [HEX]`，空草稿显示占位文本 `[Use <i> to input.]`。
- `i` 进入聚焦的多行编辑器；占位文本立即消失，显示真实文本光标，并使用绿色边框和标题表示焦点。
- 聚焦时标题包含 `Enter Send`、TXT 模式下的 `Alt+Enter Newline`、`Alt+Up/Down History` 和 `Esc Normal`；HEX 模式不显示 Newline 提示。
- 可显示多行草稿的有限高度视口。
- Send 按钮。
- 当前换行后缀。
- 快捷发送和定时发送状态。

规则：

- 断开时允许编辑草稿。
- 断开时的聚焦、光标、占位文本和历史行为与连接时相同，仅提交发送被状态守卫拒绝。
- 未连接时提交发送只提示错误，不清空草稿。
- 连接时发送当前完整草稿，内部换行保持不变，发送后缀只追加一次。
- Enter 发送请求通过内容解析、连接状态和队列守卫并成功提交后，始终立即清空草稿和输入选择，光标回到列 0；不存在“发送后保留”配置。提交前失败，包括 TXT/HEX 校验失败、未连接、队列满或命令提交失败，均保留草稿、选择和可继续编辑的位置。
- 成功提交的内容在清空前加入有界发送历史，仍可通过 Alt+Up/Alt+Down 取回；清空输入不清空历史。
- 多行光标使用 Up/Down；Alt+Enter 插入换行；发送历史使用 Alt+Up/Alt+Down，避免冲突。
- 支持 bracketed paste 的终端中，粘贴内容在结束标记到达后一次插入当前文本编辑器；其中的换行不能触发发送，控制序列不能执行快捷键。非文本上下文、非法 UTF-8、超限或期间编辑上下文变化时拒绝整段粘贴。
- 草稿修改超出输入字节上限或包含非法 UTF-8 时保留原草稿，不按字节截断候选。
- Backspace 删除最后一个字符后，空输入的光标必须保持在输入视口起点的列 0，视口同步回到起点，绝不能跳到输入区右边缘。

### 6.6 底部运行状态与上下文提示

底部固定为职责分离的两行：

- 第一行只显示运行指标：RX/TX 累计字节数、当前内存记录使用量、TX/ingress/日志队列使用量、手动暂停、离开底部、搜索状态，以及当前定时发送槽位、周期、发送次数和 missed 计数。日志队列指标标记为 `LOGQ`，只表示队列占用，不是第二个 Log 状态；日志状态仅由顶部 `Log<g/G>` 字段表示。
- 第二行只显示当前交互状态可执行的动态动作提示。顶部已标记的 `C/P/B/D/N/V/H/g/G` 不在底部重复。

上下文提示示例：

```text
Normal: i Input | R Receive | / Search | ? Help | q Quit
Input: Enter Send | Alt+Enter Newline | Alt+Up/Down History | Esc Normal
Browse: j/k Move | [count]j/k | gg/G First/Last | yy Copy Record | PgUp/PgDn Page | i Input | q Quit | Esc Normal
```

TXT Input 显示 Newline，HEX Input 不显示；空间不足时按当前状态最相关动作从左到右保留，不能挤掉独立的运行指标行。

### 6.7 不可信文本安全显示

- 串口数据、USB 元数据和会话名称都视为不可信输入。
- ESC、C0/C1、NUL 和不可显示控制字符必须转义为 `\xNN` 或明确标记。
- 双向文本控制字符必须转义或高亮。
- 不允许设备内容直接产生 ANSI、OSC、超链接或剪贴板控制序列。只有用户明确执行复制命令后，应用才能用经过长度限制和正确编码的受控 OSC 52 序列写入选择内容。

### 6.8 主题与颜色

首版前景与组件调色板使用 Rosé Pine Moon，并由 `config.toml` 的 `[ui].background` 选择背景策略：

| 名称 | RGB |
| --- | --- |
| Base | `#232136` |
| Surface | `#2A273F` |
| Overlay | `#393552` |
| Text | `#E0DEF4` |
| Muted | `#6E6A86` |
| Subtle | `#908CAA` |
| Love | `#EB6F92` |
| Gold | `#F6C177` |
| Rose | `#EA9A97` |
| Pine | `#3E8FB0` |
| Foam | `#9CCFD8` |
| Iris | `#C4A7E7` |
| Success | `#9CCF7C` |

- `background = "rose-pine"` 是默认值：Base 用于应用背景，Surface 用于面板，Overlay 用于弹窗和浮层。
- `background = "transparent"` 时所有组件、空白、选择、通知和弹窗都不得发出任何背景色属性；仍使用 Rosé Pine 前景色、边框、粗体、下划线和反选等非背景手段保持层级及选择可见。
- Text 用于主要文本，Muted 用于禁用文本，Subtle 用于次要文本和非活动边框。所有快捷键字面量及快捷键提示前景固定为粗体 `#FFFFFF`，不因背景策略变化。
- Love 用于 ERR、危险、失败和 `Disconnected`；Gold 用于警告及 `Connecting`/`Disconnecting`；Rose 用于 TX；Pine 用于可操作配置；Foam 用于 RX 和信息；Iris 用于选择以及 ReceiveBrowse 的活动边框和标题；Success 用于 `Connected`、成功通知以及发送编辑器的活动边框和标题。
- `Error` 使用 Love 红色、粗体并反选；`ErrorDialog` 使用红色边框和错误标题，在 transparent 模式下也不能补发背景色。`Disconnected` 只使用红色前景，不使用粗体反选冒充 Error。
- `Disconnected`、`Connecting`、`Connected`、`Disconnecting`、`Error`、RX/TX/SYS/ERR 和焦点标题等文本标签始终保留，颜色不能成为唯一语义载体。
- FTXUI 使用 `Color::RGB(r, g, b)` 集中定义这些颜色；终端不支持真彩色时回退到最接近的 FTXUI ANSI 颜色，同时保持全部文本标签、边框和焦点差异。

## 7. 设备发现、配置与连接

本节只定义功能。快捷键统一在第 14 节。

### 7.1 设备发现

- 启动时调用 libserialport 枚举可发现的串口。
- 启动扫描对每个候选最终字符设备执行基于当前有效身份的权限预检。若所有已发现候选均无访问权限，只显示一次红色 `ErrorDialog`；若仅部分候选无权限，设备弹窗保留这些候选并把对应行标红，同时显示 `Permission denied` 文本。
- 每次打开设备选择弹窗前重新枚举，不能直接使用上次结果。
- 弹窗内允许再次刷新。
- 用户界面和产品流程只允许从本次扫描结果中选择设备，不提供设备路径文本输入或原始字符串编辑。
- PTY 集成测试可以通过测试后端或内部测试接口传入明确路径，不要求 PTY 出现在枚举列表；该内部能力不暴露到用户 UI。
- 扫描结果立即复制为纯值对象，释放 libserialport 列表后不保留 `sp_port*`。
- 每次扫描具有 generation，旧扫描结果不能覆盖新结果。
- 设备扫描由进程生命周期内单一常驻 scanner worker 执行，不为每次刷新创建线程。
- scanner 最多有一个运行中请求和一个有界 pending 请求；第三个请求同步返回 busy。每个已接受扫描都有 OperationId 和预留 completion，pending 不得被静默覆盖。
- 允许常见 `/dev/serial/by-id` 符号链接；选择时解析最终目标并记录 `st_dev`、`st_ino` 和 `st_rdev`。
- 权限预检只是诊断提示，不是安全授权或打开成功保证；设备选择时和提交连接前都必须重新检查，真正的 libserialport open 结果始终是最终权威。

首版不承诺稳定 USB 重插身份。USB VID、PID、序列号、制造商和产品名称只用于辅助显示。

### 7.2 配置作用域

| 作用域 | 配置 | 连接后行为 |
| --- | --- | --- |
| 硬件连接快照 | 设备、波特率、数据位、停止位、校验位、流控 | 锁定 |
| 运行发送 | TXT/HEX、换行后缀 | 允许修改 |
| 纯显示 | RX/TX 各自 TXT/HEX/MIXED、RX/TX/SYS/ERR 可见性 | 允许修改 |
| 全局默认 | 日志路径、配额、快捷发送槽、新会话默认值 | 允许修改；活动任务继续使用快照 |

`P/B/D/N/V/H` 都打开预设选择弹窗，只能选择明确条目，不提供原始字符串或任意数值编辑：

- `P`：仅列出本次扫描到的设备；无设备路径输入项。
- `B`：300、600、1200、2400、4800、9600、19200、38400、57600、115200、230400、250000、460800、500000、921600、1000000、1500000、2000000 baud；最终是否支持由驱动和设备决定。
- `D`：分别从以下预定义串口字段中选择并组成完整快照，不接受自由文本：数据位 5、6、7、8；停止位 1、2；校验 None、Odd、Even、Mark、Space；流控 None、RTS/CTS、XON/XOFF。
- `N`：None、LF、CR、CRLF。
- `V`：RX 和 TX 分别选择 TXT、HEX、MIXED，并为 RX、TX、SYS、ERR 分别设置可见性；至少保留一个方向/类型。SYS 和 ERR 不提供模式选择并始终为 TXT。
- `H`：TXT、HEX。

硬件配置值：

- 数据位：5、6、7、8。
- 停止位：1、2。
- 校验位：None、Odd、Even、Mark、Space。
- 流控：None、RTS/CTS、XON/XOFF。

### 7.3 连接流程

- 未选择设备时，请求连接先重新扫描并打开“选择并连接”弹窗。
- 普通设备配置弹窗只执行“选择”，不能自动连接。
- 选择设备和提交连接时分别重新执行权限检查；拒绝时打开红色 `ErrorDialog`，显示设备最终路径、当前 owner/group/mode、当前有效用户及可操作建议。建议只说明核对 `dialout`/`uucp` 等设备组、管理员配置的 udev 规则或设备占用情况，不执行修改。
- 打开前重新解析路径并核对最终对象是字符设备，比较 `st_dev`、`st_ino` 和 `st_rdev`；Linux 后端打开后必须取得 native fd 并再次 `fstat`，获取失败或对象不一致都由 owner 立即关闭并报告连接失败。
- 进入 Connecting 前获得当前进程内的端口租约。
- 打开后一次性构造和应用完整串口配置。
- 任一配置失败立即进入 Error，再经 Disconnecting 停止 I/O、关闭端口并释放租约，清理完成后进入 Disconnected。
- 预检通过后仍必须实际 open；open 返回的 `Permission denied`、busy、设备消失或其他错误覆盖预检判断并作为权威结果。权限错误使用 `ErrorDialog`，其他连接错误至少写入持久 notice 或 ERR 记录；ErrorDialog 消费按键，关闭前不得有按键穿透到连接、配置或发送动作。
- 所有 libserialport OS 错误必须在失败调用后立即复制，再执行其他 API。
- Connecting 期间允许取消，取消后进入 Disconnecting；owner 完成清理前不能重新连接。
- 连接成功后保持 Normal，不自动聚焦输入框。

### 7.4 断开流程

断开按固定 barrier 执行：

1. 进入 Disconnecting 并拒绝新发送。
2. 使定时任务 generation 失效。
3. 取消尚未开始的发送请求。
4. 当前非阻塞写调用返回后终止剩余字节；已接受前缀按 TX 部分写语义记录。
5. 冲洗 pending CR 和未完成 RX 尾帧。
6. 关闭串口并释放租约。
7. 提交最终 SYS 或 ERR。
8. 等待日志 barrier。
9. 关闭当前日志文件。
10. 进入 Disconnected 和 Normal。

日志跟踪开关仍开启时，断开后进入内部 `Waiting`、界面 `WAITING`，下次连接创建新文件。

连接、单次 TX 请求、owner 停止和日志 barrier 都使用单调时钟 deadline。内置默认分别为 5s、5s、5s 和 5s，允许 TOML 在 100ms 至 60s 的硬范围内调整。超时后仍只能由 owner 清理和关闭 `sp_port`，其他线程不得并发调用 `sp_close()`。

## 8. 串口 I/O 与并发模型

### 8.1 单一 owner

- 每个连接由唯一串口 owner 工作单元持有 `sp_port`。
- UI、日志线程和定时器不能直接调用 libserialport。
- owner 使用非阻塞读写和可唤醒事件循环。
- 发送和关闭通过命令队列交给 owner。
- 控制、停止和断开命令使用高优先级通道，不能排在大量数据后面。
- 只有 owner 在读写循环停止后调用 `sp_close()`。
- serial owner 在线程生命周期内常驻，连接和断开只改变其内部资源状态，不为每次重连创建新线程。

### 8.2 有界队列

内置默认值：

| 队列 | 消息数上限 | 字节上限 | 满载行为 |
| --- | ---: | ---: | --- |
| TX 请求队列 | 256 | 4 MiB | 拒绝新请求并提示 |
| 日志队列 | 4096 | 8 MiB | 停止日志并进入内部 Error，UI 显示 ERROR |
| 普通 owner 命令队列 | 256 | 1 MiB | 拒绝普通命令并提示 |
| RX ingress | 4096 个块 | 4 MiB | 设置 overflow 应急状态并安全断开 |
| UI 可见记录 | 100000 条 | 32 MiB | 淘汰最旧记录并显示 seq gap |

- 停止、取消和断开不进入普通命令队列，使用独立原子标志和 wake fd，保证普通队列满时仍可执行。
- overflow/gap 使用独立固定应急状态，不依赖已经满载的 ingress。
- 队列限制允许 TOML 调小或在不可突破的硬上限内调大。
- 单个 RX block 最大 64 KiB，单个 TX 请求和发送草稿最大 1 MiB，发送历史最多 1000 条或 8 MiB，搜索结果最多保留 10000 个记录 ID。
- UI 更新通知采用“最多一个 pending”的合并机制，不复制每条原始数据。
- 记录和日志使用不可变引用计数字节块，避免 UAF 和无界重复复制。
- 日志变慢或 UI 暂停时仍受同一上限约束。
- 应用不能保证操作系统或硬件缓冲永不溢出；任何已知应用层丢失必须产生 ERR 和 gap 计数。
- 连接、发送、停止、断开、保存和 barrier 的完成事件使用独立可靠 completion mailbox；普通数据队列满不能使状态机永久停留在过渡态。
- 接受异步命令时必须同时预留其 completion 容量；不能接受一个未来无法报告最终结果的命令。
- stop gate 一旦关闭就拒绝新的生产请求；gate 关闭前已返回 Accepted 的连接、扫描、TX、控制和持久化操作仍必须产生最终 completion，或在无法履约时升级为 FATAL，不能静默遗失 future/operation。
- worker 无法继续运行时使用单槽固定大小应急状态通知主线程，不依赖普通队列或堆分配。
- 日志 worker 永久拥有一个固定 `LogStatusSignal` 状态槽；自发的满载、磁盘错误和停写状态通过该槽唤醒 UI，不依赖某个已接受 operation 的 completion 预留。
- serial、session log、scanner、persistence 和 diagnostics worker 各有固定 `NotStarted | Running | AtReturnPoint` 生命周期槽、stop 控制槽和 `WorkerStoppedSignal`。未构建、未启用或允许降级的初始化失败保持 NotStarted；主线程只等待曾进入 Running 的 worker。正常退出与 FATAL 共用该不分配内存的路径。

### 8.3 全局内存预算

应用管理的动态内存硬预算为 128 MiB，按以下类别设置不可同时突破的上限：

| 类别 | 硬预算 |
| --- | ---: |
| UI 可见记录及其元数据 | 48 MiB |
| 会话日志 pending 记录 | 16 MiB |
| RX ingress | 8 MiB |
| TX 队列、当前 TX 和发送草稿 | 8 MiB |
| 发送历史 | 16 MiB |
| 内部诊断队列 | 8 MiB |
| owner 命令、分帧和工作 scratch | 16 MiB |
| model、搜索结果和其他有界状态 | 8 MiB |

- 不可变记录被多个 sink 引用时，全局预算按实际 payload 只计一次；各 sink 仍按逻辑字节分别执行自己的过载策略。
- 容器节点、字符串 capacity、索引和记录元数据按保守估算计入所属类别，不能只统计 payload 长度。
- operation completion、active operation、固定控制槽、OperationId 索引、LogStatusSignal 和 WorkerStoppedSignal 的对象及容器开销计入 model/control 类别。
- 配置只能在类别硬预算内调整；任何组合都不能扩大 128 MiB 总预算。
- 额外 64 MiB RSS 空间留给线程栈、FTXUI、allocator、shared library 和不可精确预留的运行时开销。
- 当前实现状态：生产 `SessionRecords` 已使用唯一 `SessionSequencer`，UI 与日志共享同一不可变记录及其预算 token；UI 引用、过滤坐标、安全文本缓存、增量搜索存储、日志队列和 rollover backlog 引用也已记账。ingress、TX、草稿/历史、scratch、配置和控制槽等类别仍未全部接入，因此不能宣称 128 MiB 运行时全局 token 已端到端闭环。

### 8.4 事件顺序与时间

- 每个连接具有唯一 session event sequencer。所有 RX、TX、SYS、ERR 先由 sequencer 校验会话资格并分配严格递增的 `seq`，再分别提交给 UI 和日志 sink。
- RX 时间为 owner 成功读取该帧首字节的应用观察时间。
- RX 帧可能在空闲分帧完成后才进入 sequencer，因此 UTC 时间不保证随 seq 单调；seq 是唯一业务排序依据。
- TX 逻辑请求结束时，对已知被 OS 接受的前缀生成一条 TX 记录；时间为最后一次正返回的接受时间。
- TX 请求 `offset = 0` 时不生成 TX；部分成功后失败、取消或超时时，先记录已接受前缀，再生成带 `operation_id` 和终止原因的 ERR。
- TX 成功不代表目标设备已接收，也不代表字节已经离开硬件。
- SYS 和 ERR 在同一事件序列中排序。
- 日志过滤只能形成有序流的子序列。
- 最终日志 barrier 必须排在最终 SYS/ERR 之后，并向断开流程返回成功或失败。
- fan-out 不要求两个 sink 原子地同时成功：UI 满载时淘汰最旧记录并形成明确 gap；日志满载时停止该日志文件，不能反向阻塞或断开串口。
- sequencer 只是短临界区内的编号和投递逻辑，不创建额外 actor 线程，也不在临界区执行编码、文件 I/O 或大块复制。

## 9. 数据处理

### 9.1 原始字节原则

- 原始字节是事实来源。
- 文本和 HEX 只是显示或输入表示。
- 解码失败不能丢弃或替换原始数据。
- 搜索、日志和统计引用同一原始记录 ID。

### 9.2 RX 分帧

内置默认值：

- `idle_gap_ms = 50`。
- `max_frame_bytes = 65536`。

规则：

1. 遇到 LF、CR 或 CRLF 时结束当前帧。
2. 分隔字节属于该帧，按顺序拼接帧内容必须得到原始字节流。
3. CR 进入 pending 状态，等待下一字节或空闲 deadline。
4. 下一字节为 LF 时合并为 CRLF；否则先提交 CR 帧，再处理新字节。
5. 空闲、断开、关闭或退出时提交 pending CR 和非空尾帧。
6. 最大帧优先于 CRLF 合并，每个输出帧连同分隔符都不得超过 `max_frame_bytes`。
7. 若 CR 使帧恰好达到上限，立即提交该帧并清除 pending；后续 LF 单独成帧。
8. 达到 `max_frame_bytes` 时立即分段，不能等待完整帧后再检查。
9. 空闲时间是“最后一次成功读取后的应用观察空闲”，不是物理线上每字节时间。
10. `RxFramer` 不为小帧预留或保留 `max_frame_bytes` capacity；大量 CR/LF 小帧不能各自携带 64 KiB 容量。

### 9.3 UTF-8 安全显示

- 首版文本模式使用 UTF-8。
- 解码器保留跨读取边界的不完整序列。
- 在帧边界仍不完整的字节使用 `\xNN` 显示。
- 反斜杠、控制字符、NUL、ESC 和双向控制字符使用可逆转义。
- MIXED 模式同时显示安全 TXT 和原始 HEX，并在 Receive 中保持一条记录一行。

### 9.4 TX 请求

- TXT 在入队前完整、严格地转换为字节向量。
- HEX 在入队前完整解析并校验。
- 解析或转换失败时一个字节也不能发送。
- 每个发送请求拥有 `operation_id`、immutable bytes、offset、deadline 和 generation。
- 一个逻辑请求完成或终止前，不能插入另一个请求的字节。
- 正返回值记录为已被 OS 接受的前缀。
- 错误调用中无法确认的部分不能伪造为已发送。
- TX deadline 从 owner 开始处理请求时计算；非阻塞写返回 0 时等待下一次可写事件并执行退避，不能忙循环。
- TX 超时使用稳定错误码 `LC-SER-2005`，取消使用 `LC-SER-2006`。存在 OS 已接受字节时，先发布已接受 TX prefix，再发布对应 ERR；若会话随后关闭，Cleanup 必须排在二者之后。零接受字节时不伪造 TX 记录。

HEX 允许空格、Tab、CR 和 LF 作为分隔符，也允许每字节可选 `0x` 前缀。

## 10. 快捷发送与定时发送

### 10.1 快捷发送槽

- 固定提供 20 个跨会话共享槽位。
- 每个槽包含名称、TXT/HEX 模式、内容、换行策略和备注。
- 空槽显示 `[Input please.]`。
- 非空内容在列表中安全转义并按宽度截断。
- 删除槽位前确认，默认焦点为 Cancel。
- 槽位编辑在配置结果为已提交或已提交但耐久性未知时更新内存；未提交时保留编辑内容和旧槽位状态。
- 名称最大 64 个 UTF-8 字节，备注最大 256 个 UTF-8 字节，解码后的发送内容最大 1 MiB。

### 10.2 执行弹窗

- 使用列表选择 1 至 20，不把 `10` 至 `20` 当作单个快捷键。
- 可使用独立编号输入字段，输入完整编号后确认。
- 空槽不能执行。
- 显示完整内容预览、模式和换行策略。
- 保存上次选择的槽位和 `interval_ms`，但不恢复运行状态。

### 10.3 定时语义

- `interval_ms = 0`：只发送一次。
- `interval_ms = 1..9`：非法。
- `interval_ms = 10..86400000`：立即发送一次，然后按周期持续发送。
- 使用单调时钟和固定速率 deadline。
- 最多存在一个 outstanding 定时请求。
- writer 忙时跳过已错过触发点，增加 missed 计数，不能堆积。
- 手工发送优先级只在逻辑请求边界生效，不能插入正在部分写入的请求。
- 启动新任务且已有任务时必须确认替换，默认焦点为 Cancel。
- 只有完整成功的请求增加已发送次数。
- 首次立即触发时 writer 已忙则等待一个可用请求边界，不额外排队；后续重复触发仍按 missed 规则处理。
- 停止、替换、断开或关闭时使 task generation 失效，清除尚未开始的请求，并要求 owner 终止正在部分写入的该任务剩余字节。
- UI 只有收到 owner 停止确认后才显示任务已停止；确认之后不能再发生该 task generation 的写入。
- 所有周期都是 best effort，不承诺 10ms 级实时精度。

## 11. 日志设计

### 11.1 日志状态机

```text
Off -> Waiting
Waiting -> Recording | Off | Error
Recording -> Waiting | Off | Error
Error -> Off
Off -> Waiting/Recording  重新启用并重试
```

- 内部和 app 事件语义统一且仅使用 `Off`、`Waiting`、`Recording`、`Error`；用户界面固定显示 `OFF`、`WAITING`、`REC`、`ERROR`，保留顶部 `Log` 标签和 `REC` 缩写。
- `Log:OFF`：未启用。
- `Log:WAITING`：已启用但当前没有活动文件；通常是未连接，或正在异步创建/重建 writer。
- `Log:REC`：当前文件正在接收记录。
- `Log:ERROR`：创建、配额或写入失败，已经停止记录。
- Waiting 创建文件失败或 Recording 写入失败都会进入 Error。
- Error 下关闭日志会复位到 Off；修复配置后重新启用时进入 Waiting，已连接时尝试创建新文件并进入 Recording。

内置默认状态为 Off，允许 TOML 改为默认 Waiting。界面必须仅在第 6.2 节的顶部状态行持续显示 `OFF`、`WAITING`、`REC` 或 `ERROR`。

`g` 的运行转换固定如下：

- Disconnected + OFF：`g` 进入 WAITING；WAITING 下 `g` 回到 OFF。
- Connected + OFF：`g` 先进入 WAITING 并异步创建当前会话的新日志，成功进入 REC，失败进入 ERROR。
- REC 下 `g` 执行 barrier、flush 和关闭，成功进入 OFF；不能仅清除标识而继续写文件。
- ERROR 下第一次 `g` 只确认并复位到 OFF，不自动重试；再次 `g` 才按当前连接状态进入 WAITING 或尝试 REC。
- Disconnecting 期间不接受新的启用请求；清理前已经处于 REC 时完成 barrier，若跟踪仍启用则断开完成后进入 WAITING。

### 11.2 文件格式

文件使用 UTF-8 NDJSON，每一行都是独立 JSON 对象。第一行是 `header`，后续行为 `record`：

```json
{"type":"header","format":"lazycom-log","version":{"major":1,"minor":0},"started_at":"2026-07-22T04:30:00.001Z","session_id":"a1b2c3","device":{"path":"/dev/ttyUSB0"},"serial":{"baud":115200,"data_bits":8,"parity":"none","stop_bits":1,"flow_control":"none"}}
{"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03.442Z","elapsed_ns":3441000000,"direction":"TX","encoding":"utf8","content":"AT+INFO\r\n","input_mode":"txt"}
{"type":"record","seq":2,"time_utc":"2026-07-22T04:30:03.468Z","elapsed_ns":3467000000,"direction":"RX","encoding":"utf8","content":"READY\r\n"}
{"type":"record","seq":3,"time_utc":"2026-07-22T04:30:05.120Z","elapsed_ns":5119000000,"direction":"RX","encoding":"base64","content":"AaD/fg=="}
{"type":"record","seq":4,"time_utc":"2026-07-22T04:31:10.005Z","elapsed_ns":70004000000,"direction":"ERR","code":"LC-SER-2003","message":"Serial device disconnected"}
```

规则：

- `type` 只使用 `header` 和 `record`；`direction` 只使用 RX、TX、SYS、ERR。
- RX/TX 只有在严格 UTF-8 解码并重新编码后逐字节等于原始数据时优先使用 `encoding="utf8"`，否则使用 base64；若严格 UTF-8 的 JSON 转义结果会超过 2 MiB 物理行上限，也改用 base64。两种形式都必须无损恢复原始字节。
- TX 可以记录独立 `input_mode` 说明用户输入来源，但它不改变原始字节事实。
- SYS/ERR 使用 `message`；ERR 同时记录稳定 `code`，动态文本必须经过控制字符和长度处理。
- `seq` 是唯一业务顺序；`elapsed_ns` 是从会话开始计算的单调时间，`time_utc` 是带 UTC 时区的观察时间。
- 不完整尾行由读取器报告并忽略，不能当成有效对象；此前完整行仍可读取。
- 元数据字节只有在严格 UTF-8 有效时写入文本字段；否则使用明确的 base64 伴随字段无损保存。
- JSON 字符串必须正规转义，不能包含非法 UTF-8、未转义换行或终端控制注入。
- 未知次版本字段可以忽略，未知主版本必须拒绝解析。
- 队列可以批量传递多个 record 以降低锁和唤醒开销，但文件中每个逻辑 record 仍独占一行并保留自己的 seq。
- 不可信 JSON 在构造完整 DOM 前受限：物理行最大 2 MiB、嵌套深度最大 16、结构事件最大 256；整文档辅助编解码最大 16 MiB 且最多 100000 条 record。生产 writer 仍按行处理，不以整文档接口替代流式写入。

### 11.3 文件安全与耐久性

- 应用新建日志目录权限为 0700，日志文件权限为 0600，不依赖 umask；已有用户目录可以保留组读取/搜索权限，但不能允许 group/other 写入。
- 日志目录从根目录开始逐组件以目录 fd 和 `O_NOFOLLOW` 打开或创建；最终目录必须由当前 euid 所有、可写且可搜索，并拒绝 group/other writable，不能只检查最终路径字符串。
- 默认文件名只包含 UTC 时间和随机 ID，不暴露会话名称或 USB 序列号。
- 文件创建使用不覆盖、拒绝符号链接的方式。
- 日志每秒或达到批量阈值时 flush。
- 正常断开和退出时 flush 并等待日志 barrier。
- 不承诺每条记录 `fsync`；崩溃时最多允许损失最近一个 flush 周期。
- 日志失败立即停止 writer，从内部 Recording 进入 Error，UI 清除 REC 并显示 ERROR。
- barrier 携带目标 seq；writer 只有在处理完队列中位于 barrier 前的所有记录并把用户态缓冲写出后才能确认 `processed_through_seq`。
- barrier 成功不代表物理介质已经持久化；会话日志只承诺 write/flush 语义，配置文件另行使用 fsync 耐久流程。

### 11.4 配额与轮换

内置默认值：

- `max_files = 100`。
- `max_total_size_mib = 1024`。
- `max_file_size_mib = 64`。
- 三个配额都必须是正数，不能通过 0 或其他值取消磁盘硬边界。

规则：

- 单文件达到上限后轮换并关闭旧文件。
- 配额按实际 NDJSON 序列化字节数计算，包含 header、record、转义和换行。
- 每次追加前计算 projected size；超过单文件上限时先轮换，再写入完整记录。
- 单个 header 加一条记录超过非零单文件上限时进入内部 Error/UI ERROR，不能写入半行。
- 只删除已关闭且可验证为 LazyCom 支持版本的日志。
- 活动文件、未知文件、目录和符号链接不能删除。
- 有效候选按 `(started_at, mtime, filename)` 从旧到新删除。
- 损坏或空的受控 LazyCom 前缀文件同时计入 `max_files` 和总容量，但不自动删除，并提示人工处理。
- 日志目录使用进程级锁；无法获得锁时禁用自动清理并进入内部 Error/UI ERROR。
- 采用目录 fd 和不跟随符号链接的删除方式，避免 TOCTOU。
- `max_files` 统计活动、关闭有效日志以及受控前缀的损坏/空日志；总容量统计这些文件的实际大小。未知文件不计入 LazyCom 配额。
- 创建和追加前在目录锁下执行清理；无法在不删除活动文件的情况下满足配额时停止日志，不继续增长。
- writer 缓存目录 inventory，并用 inotify 与目录 stat 变化使缓存失效；外部创建、删除、移动或属性变化会触发重扫。监视到日志目录自身被移动、删除或失去 watch 时立即终止当前日志 writer 并进入 ERROR，不能继续写已脱离配置路径的目录 fd。

### 11.5 Session Log Settings

`G` 打开两级 `Session Log Settings`，不提供文件名、NDJSON schema、日志格式、方向集合或时间格式配置。

第一级固定显示当前候选值和以下操作：

- `Directory`
- `Maximum files`
- `Maximum total size`
- `Maximum file size`
- `Save for Next Session`
- `Save and Rotate Now`
- `Cancel`

选择前四项之一进入第二级，只编辑该项。配置中的空 directory 在打开弹窗时解析并显示实际 XDG 默认绝对目录，不把空字符串作为可提交 UI 值。数字字段继续使用第 12.2 节现有硬范围，非法输入保持第二级内容和焦点并显示字段错误。Directory 在 Enter 时必须同时满足：绝对路径、已经存在、最终目录本身不是符号链接、由当前 euid 所有，并且按当前有效身份可写且可搜索。验证不能只依赖路径字符串或 `access()` 的 real uid 语义。

Directory 验证失败时打开红色 `ErrorDialog`；关闭后返回原第二级弹窗，保留未提交内容、光标和焦点。ErrorDialog 必须消费全部非 F1 输入，任何按键不得穿透并意外保存、轮换或关闭底层弹窗。

四个值组成单一候选设置：第二级确认只更新候选，不改变运行设置；`Cancel` 丢弃整个候选。保存时完整候选一次更新到当前进程配置并异步持久化，连续保存合并为最新快照；持久化失败会明确报告，但不回滚本次进程内配置。`Save for Next Session` 不触碰当前 REC writer，在下一次进入 REC 前重建 writer。`Save and Rotate Now` 若当前为 REC，则先结束当前文件，再按新设置重建 writer 并创建下一文件；重建或新文件创建失败时进入日志 ERROR，已安全关闭的旧文件保持可读，不伪造成功回滚。

活动 REC 下必须明确提供两种策略：下一会话生效，或立即 rollover。WAITING/OFF/ERROR 下 `Save and Rotate Now` 保存候选但没有活动文件可轮换，界面明确提示“已保存；下次进入 REC 生效”，不得伪造一次 rollover。保存配置的三态耐久结果仍按第 12 节处理。

已接受的 rollover 在断开或退出期间继续完成，把同一 session 暂存的记录和最终清理事件写出后关闭；连续保存设置不能取消进行中的收尾责任。失败须明确进入日志 ERROR，不能静默丢弃暂存记录并报告成功。

## 12. TOML 配置与持久化

### 12.1 文件分层

```text
$XDG_CONFIG_HOME/lazycom/config.toml       用户默认配置
$XDG_CONFIG_HOME/lazycom/quick_send.toml   应用管理的 20 个快捷槽
$XDG_STATE_HOME/lazycom/state.toml         应用管理的运行偏好
```

若 XDG 变量未设置，分别回退到 `~/.config/lazycom` 和 `~/.local/state/lazycom`。路径必须解析为绝对路径，不执行 Shell 展开。

### 12.2 `config.toml`

首版 schema 示例：

```toml
version = 1

[serial.defaults]
baud = 115200
data_bits = 8
stop_bits = 1
parity = "none"
flow_control = "none"

[send]
mode = "txt"
newline = "none"
max_draft_bytes = 1048576
history_max_entries = 1000
history_max_mib = 8

[receive]
rx_view = "txt"
tx_view = "txt"
idle_gap_ms = 50
max_frame_bytes = 65536
visible_buffer_mib = 32
visible_max_records = 100000

[ui]
background = "rose-pine"

[logging]
default_enabled = false
directory = ""
max_files = 100
max_total_size_mib = 1024
max_file_size_mib = 64
flush_interval_ms = 1000
include_system = true
include_error = true

[queues]
tx_max_messages = 256
tx_max_mib = 4
owner_command_max_messages = 256
owner_command_max_mib = 1
rx_ingress_max_blocks = 4096
rx_ingress_max_mib = 4
log_max_messages = 4096
log_max_mib = 8

[timeouts]
connect_ms = 5000
tx_ms = 5000
owner_stop_ms = 5000
log_barrier_ms = 5000
```

- 空日志目录表示使用 XDG 默认目录。
- `[receive].rx_view` 和 `[receive].tx_view` 分别只允许 `txt`、`hex`、`mixed`，内置默认均为 `txt`；SYS 和 ERR 没有 view 配置，始终按 TXT 文本消息显示。RX/TX/SYS/ERR 可见性继续作为各自独立的运行偏好保存。
- 旧持久配置 `receive.view = "text"|"hex"|"mixed"` 在加载时迁移为相同值的 `receive.rx_view` 和 `receive.tx_view`，其中 legacy `text` 映射为 `txt`；应用管理的下一次配置重写移除旧 `receive.view`。若新旧字段同时存在，以 `rx_view`/`tx_view` 为准并警告旧字段被忽略。
- `send.keep_after_send` 不再是受支持配置。旧持久值无论 true/false 都被忽略，加载后采用“成功 Enter 发送始终清空”；应用管理的下一次配置重写移除该旧字段。它是已知废弃键，不按未知键保留。
- `[ui].background` 只允许 `rose-pine` 或 `transparent`，默认 `rose-pine`；transparent 的实现要求见第 6.8 节。
- 未知键发出警告；应用重写配置时必须在 TOML 数据模型中保留未知键，不能静默删除。
- 未知主版本拒绝加载。
- 首版启动时加载一次，不自动热重载。
- 用户可通过命令面板显式 Reload；活动连接继续使用硬件快照。
- 配置语法错误时进入只读保护，不能用默认值覆盖原文件。
- 语法正确但任一已知字段类型错误、越界、枚举非法或整体资源组合不合法时，拒绝整个新快照并列出全部字段错误；不能提交部分新值与部分默认值的混合配置。
- 定时范围 0 或 10 至 86400000 是程序固定常量，不允许 TOML 扩大。
- 所有队列、草稿、历史、帧和超时配置都有编译期硬上限；TOML 只能在文档允许范围内收窄或调整，不能取消内存边界。
- UI 配置只在显式选择“保存为默认”后写入 `config.toml`；应用前检查文件自加载后是否被外部修改，冲突时拒绝覆盖并要求 Reload。
- 应用写入使用规范化 TOML，保留未知键和值，但不保证保留原注释布局；写入前创建受限权限备份并在确认框中说明。
- 每个 TOML 最多保留一个固定名称 `.bak`，不创建按时间累积的备份。备份遵守同一 owner、0600、no-follow、读取尺寸和原子替换规则，并计入三个文件各自的磁盘尺寸硬上限。

首版硬范围：

| 项目 | 合法范围 |
| --- | --- |
| `idle_gap_ms` | 1..60000 |
| `max_frame_bytes` | 256..65536 |
| `visible_buffer_mib` | 1..48 |
| `visible_max_records` | 1000..1000000 |
| 单个草稿或 TX 请求 | 最大 1 MiB |
| 发送历史 | 最大 10000 条且最大 16 MiB |
| TX 队列 | 最大 8 MiB，包含当前 TX 和草稿预算 |
| RX ingress | 最大 8 MiB |
| 会话日志队列 | 最大 16 MiB |
| 普通 owner 命令队列 | 最大 2 MiB |
| 任一消息数或块数队列 | 最大 65536 |
| 连接、TX、owner 停止、日志 barrier 超时 | 100..60000 ms |
| `max_files` | 1..10000 |
| `max_total_size_mib` | 1..65536 |
| `max_file_size_mib` | 1..1024，且不大于总量 |

枚举值大小写不敏感，序列化时统一使用小写。字段类型错误、越界或非法枚举不做截断，整个候选快照保持未提交；启动时没有旧快照则使用完整内置默认快照，但不得自动覆盖错误文件。

### 12.3 `quick_send.toml`

```toml
version = 1

[[slots]]
index = 1
name = "Query"
mode = "txt"
content = "AT+INFO"
newline = "session"
note = ""
```

- index 范围为 1 至 20，且不得重复。
- 缺失 index 表示空槽。
- mode 只允许 `txt`、`hex`；newline 只允许 `session`、`none`、`lf`、`cr`、`crlf`。
- TXT 必须是有效 UTF-8；任意二进制内容必须使用 HEX。
- 应用采用同目录临时文件、0600、fsync、rename 和父目录 fsync。
- 保存结果分为未提交、已提交、已提交但目录耐久性未知。未提交时保留原内存和编辑弹窗；后两种都提交已序列化的内存快照，耐久性未知时显示明确警告。

### 12.4 `state.toml`

```toml
version = 1
last_quick_send_slot = 1
last_interval_ms = 0
```

只保存：

- 上次快捷发送槽位。
- 上次合法 `interval_ms`。
- 非敏感 UI 偏好。

不保存：

- 上次连接状态。
- 正在运行的定时任务。
- 接收和发送记录。
- 临时发送历史。

`state.toml` 使用与 `quick_send.toml` 相同的同目录临时文件、0600、fsync、原子 rename 和父目录 fsync 流程。

- rename 前失败表示未提交，目标文件和内存快照保持旧值。
- rename 成功后即使父目录 fsync 失败，新文件也已对当前进程可见，结果必须报告为“已提交但重启耐久性未知”，不能伪装成完全未保存。

### 12.5 文件安全规则

- XDG 应用目录由当前 euid 所有，新建目录权限为 0700，三个 TOML 文件权限为 0600。
- 三个 TOML 文件均设置最大读取尺寸，超过上限拒绝加载。
- 自定义日志目录必须是绝对且已存在的目录，最终目录本身不得是符号链接，必须由当前 euid 所有，并且按当前有效身份可写、可搜索；`G` 的 Directory 在 Enter 时执行同一验证。
- 不盲目 chmod 用户已有目录；权限不安全时拒绝记录并说明原因。
- 所有应用写入使用目录 fd、no-follow、独占临时文件和原子替换。
- 三个配置文件共享每目录一个 0600、no-follow 的非阻塞事务锁；锁忙立即返回，不在 worker 中无限等待。
- 新目标提交使用 `renameat2(RENAME_NOREPLACE)` 语义拒绝覆盖竞态；已有目标在事务开始和 rename 前复核身份。POSIX 对同 UID 不合作写者替换已有目标仍存在最终窄竞态，作为已知限制保留。
- 新建配置目录按绝对路径逐组件 no-follow 打开/创建，每创建一级都 fsync 其父目录；临时文件和 `.bak` 完成文件 fsync，rename 后再 fsync 目标父目录，并保留三态提交语义。

## 13. 安全与防误操作

- 所有破坏性确认默认聚焦 Cancel。
- 确认按钮使用具体动作名称，不使用模糊 Yes/No。
- 清空记录显示将删除的内存记录数，并说明日志不受影响。
- 关闭会话或退出时根据实际风险确认：连接中、连接过渡中、活动日志、定时任务、非空草稿或未记录数据。
- 配置、日志和状态文件拒绝符号链接；串口路径按第 7.1 节允许并解析 `/dev/serial/by-id` 等符号链接，再按第 7.3 节执行打开前后验证。
- 所有用户可控文件名使用白名单和长度限制。
- 设备和串口文本不能直接作为终端控制序列渲染。
- 设备权限预检、选择重检和连接前重检只提供早期诊断，实际 open 始终权威；错误信息给出可操作的 owner/group/mode 和发行版设备组指导，但应用绝不执行 `chmod`、`usermod`、修改 udev 规则、提权或以 root 重新启动自身。
- 不建议用户以 root 或 `sudo` 运行 LazyCom；权限文档区分设备节点权限、组成员资格尚未在当前登录会话生效、设备被占用和设备消失。
- 首次调用任何 libserialport API 前始终安装安全 debug handler；内部诊断开启时转入诊断队列，关闭时安全丢弃并只累计固定计数。即使用户设置 `LIBSERIALPORT_DEBUG` 也不能直接污染 TUI。
- 搜索、命令、数值、目录、快捷发送和通用弹窗输入均有按用途设置的字节上限；插入必须是严格 UTF-8，光标移动、删除和按上限截断只能落在 UTF-8 code point 边界。

## 14. 首版快捷键

本节是首版快捷键的唯一权威清单。

### 14.1 上下文规则

- 字母区分大小写。
- 文本组件优先处理编辑键。
- 模态弹窗优先处理自己的键。
- F1 是唯一无条件穿透到应用的帮助键。
- `y` 在 Normal 且接收区存在非空选择时复制；在 ReceiveBrowse 中首个 `y` 只进入 `yy` 待定前缀，第二个 `y` 复制当前记录。
- `Ctrl+C` 只有在终端转发、没有更高优先级组件消费且接收区选择非空时复制；选择为空时绝不触发复制，也不作为退出键。
- ReceiveBrowse 的 `g`、`y` 和 count 待定前缀由接收区优先消费；无效序列不得泄漏为 Normal 或全局动作。count 仅能修饰 `j`/`k`，不能修饰 `yy`。
- 其他应用动作只在表格指定状态生效。

### 14.2 全局

| 快捷键 | 功能 |
| --- | --- |
| F1 | 打开或关闭快捷键帮助；关闭时恢复完整来源覆盖层 |

### 14.3 Normal 管理与焦点

| 快捷键 | 功能 |
| --- | --- |
| `?` | 打开快捷键帮助 |
| `q` | 退出程序；存在风险时确认 |
| `C` | 已断开时连接，Connecting 时取消，已连接时断开 |
| `R` | 聚焦带边框的 Receive 面板并进入 ReceiveBrowse |
| `i` | 进入发送编辑态；断开时只编辑草稿 |
| `E` | 打开命令面板 |
| `X` | 确认后清空当前会话全部内存记录 |
| Ctrl+L | 重新绘制 TUI，不删除记录 |
| Space | 切换手动暂停 |
| `/` | 打开搜索覆盖层 |
| F5 | 重新扫描串口设备 |
| `y` | 接收区选择非空时通过 OSC 52 复制，最大 1 MiB |
| Ctrl+C | 终端转发且接收区选择非空时执行与 `y` 相同的复制 |

`Q` 和 `S` 在首版不分配功能。`?` 仅在 Normal 打开 Help，不能从编辑、搜索或弹窗穿透；F1 仍是唯一无条件打开或关闭 Help 并恢复完整来源状态的按键。

### 14.4 Normal 大写配置键

| 快捷键 | 作用域 | 连接后 |
| --- | --- | --- |
| `P` | 串口设备 | 锁定 |
| `B` | 波特率 | 锁定 |
| `D` | 数据位、停止位、校验、流控 | 锁定 |
| `N` | 发送后缀 | 可用 |
| `V` | RX/TX 各自 TXT/HEX/MIXED 显示及 RX/TX/SYS/ERR 可见性 | 可用 |
| `H` | TXT、HEX 发送模式 | 可用 |
| `G` | 两级 Session Log Settings：目录和三项配额 | 可用；候选整体提交，可选下一会话或立即 rollover |
| `F` | 20 个快捷发送槽 | 可用；活动任务使用快照 |

所有配置键打开预设选择弹窗，不循环切换；`P/B/D/N/V/H` 不接受原始字符串或任意值编辑，`G` 仅允许第二级编辑已定义的目录或数值字段。

### 14.5 Normal 小写运行键

| 快捷键 | 条件 | 功能 |
| --- | --- | --- |
| `g` | Normal | 按第 11.1 节在 OFF/WAITING/REC/ERROR 间切换；ERROR 时先复位到 OFF，再次触发后重试 |
| `f` | 已连接 | 打开快捷发送和定时发送弹窗 |

### 14.6 发送编辑态

| 快捷键 | 功能 |
| --- | --- |
| Enter | 成功提交完整草稿后始终清空；失败或未连接时保留草稿 |
| Up/Down/Left/Right | 移动多行光标 |
| Alt+Enter | 在草稿中插入换行 |
| Alt+Up/Alt+Down | 浏览发送历史并保留未发送草稿哨兵 |
| Esc | 保留草稿并返回 Normal |

Backspace 删除最后一个字符时，光标和输入视口必须停在列 0 起点，不能跳到右边缘。发送成功后的空输入同样从列 0 开始；Alt+Up/Alt+Down 仍可浏览历史。

### 14.7 接收浏览

| 快捷键 | 功能 |
| --- | --- |
| `j`/`k` | 移到下一条/上一条可见记录；一行即一条 RX/TX/SYS/ERR 记录 |
| 十进制 count + `j`/`k` | 按有界计数移动，例如 `10j`、`11k` |
| `gg`/`G` | 移到最早/最新可见记录；`G` 不取消手动暂停 |
| `yy` | 通过既有 1 MiB 有界 OSC 52 路径复制当前已渲染安全记录 |
| Up/Down | 与 `k`/`j` 相同地移动当前记录 |
| PgUp/PgDn | 按页移动视口并把当前记录保持在可见范围 |
| Home/End | 与 `gg`/`G` 相同；End 不取消手动暂停 |
| `i` | 直接进入发送编辑态 |
| `q` | 退出程序；存在风险时确认 |
| Esc | 返回 Normal |
| 鼠标滚轮 | 滚动记录 |
| 鼠标左键拖动 | 无修饰键选择当前已渲染安全文本 |
| Ctrl+C | 终端转发且选择非空时通过 OSC 52 复制选区，最大 1 MiB |

ReceiveBrowse 只承诺本表列出的 Vim 核心命令。单个 `g`、单个 `y` 和 count 是上下文待定前缀；count 只适用于 `j`/`k`，`yy` 不接受数字前缀。无效后续键由 ReceiveBrowse 消费并清除前缀，不能泄漏到 Normal 或全局动作；Esc 即使存在待定前缀也返回 Normal，F1 仍是唯一无条件穿透键。裸 `q` 只在没有待定前缀时按本表退出，不得穿透 SendEdit、Search、Modal、Confirm、ErrorDialog、Help 或其他覆盖层。`Ctrl+Shift+C` 的终端拦截限制见第 6.3 节，不属于本权威清单。

### 14.8 搜索覆盖层

| 快捷键 | 功能 |
| --- | --- |
| Enter | 查询输入聚焦时提交；按钮聚焦时执行当前按钮 |
| F3 | 下一个匹配 |
| Shift+F3 | 上一个匹配 |
| Tab/Shift+Tab | 切换查询、方向过滤和操作按钮 |
| Left/Right 或 Space | 过滤字段聚焦时选择 RX、TX、SYS、ERR 或 All |
| Esc | 关闭搜索并保持当前位置 |
| End | 关闭搜索并返回最新记录 |

### 14.9 通用弹窗

| 快捷键 | 功能 |
| --- | --- |
| Up/Down | 移动选项 |
| Tab/Shift+Tab | 切换字段或按钮 |
| Enter | 应用、编辑或确认当前项 |
| Esc | 取消当前操作 |
| F5 | 仅在设备选择弹窗内再次扫描 |

### 14.10 快捷发送弹窗

| 快捷键 | 功能 |
| --- | --- |
| Up/Down | 选择槽位或操作项 |
| Home/End | 跳到第 1 或第 20 槽 |
| Tab/Shift+Tab | 切换槽位、编号输入、周期和操作按钮 |
| Enter | 确认完整编号、单次发送、启动任务或停止任务 |
| Delete | 在配置弹窗中确认后清空槽位 |
| Esc | 取消并返回 Normal |

## 15. 建议模块边界

- `app`：生命周期、可靠 completion、generation 和命令路由。
- `model`：会话、记录、草稿、配置快照和统计。
- `serial`：libserialport owner、设备枚举、连接和 I/O。
- `framing`：RX pending CR、空闲和最大帧分段。
- `encoding`：UTF-8 安全显示和 HEX。
- `scheduler`：定时任务、missed 计数和 generation。
- `logging`：有界队列、格式、轮换、配额和安全文件操作。
- `config`：三个 TOML 文件的 schema、验证和安全写入。
- `ui`：状态栏、接收区、发送区、搜索和弹窗。

模块目标是可替换串口、时钟和文件系统进行故障注入，不为简单逻辑过度拆分。

## 16. 实施阶段

### 阶段 1：schema 与核心模型（主要功能已实现）

- 固定依赖和测试框架。
- 定义状态机、ID/generation、typed command/event、可靠 completion 和配置作用域。
- 定义三个 TOML schema、日志格式版本，以及废弃 `send.keep_after_send` 和 legacy 单一 Receive View 向 `receive.rx_view`/`receive.tx_view` 的迁移。
- 实现安全配置加载和应用管理文件写入。

### 阶段 2：串口 owner（主要功能已实现）

- 实现枚举、启动权限预检、仅扫描结果设备选择、连接快照和错误复制；显式 PTY 路径仅留在内部测试接口。
- 实现非阻塞可唤醒 owner。
- 实现有界 TX/RX 队列和关闭 barrier。
- 使用假后端验证连接、取消、可靠完成、迟到事件和阻塞点。

### 阶段 3：数据路径（主要功能已实现）

- 实现 RX pending CR、idle 和最大帧状态机。
- 实现安全 UTF-8、HEX 和 MIXED 派生表示，RX/TX 独立模式，以及仅含 TXT 消息的 SYS/ERR。
- 实现 operation_id、部分写入和有界发送队列。

### 阶段 4：核心 UI（主要功能已实现）

- 实现 Normal、SendEdit、ReceiveBrowse 和覆盖层。
- 实现统一五态 Link、`HW:LOCKED`/`HW:UNLOCKED`、顶部 `[NewLine<N>]`、`[View<V>]`、`[InputType<H>]`、`[Log<g/G>]` 配置状态栏和 rose-pine/transparent 背景策略。
- 实现 `P/B/D/N/V/H` 预设弹窗、两级 `G` 候选事务、红色无穿透 ErrorDialog 和权限诊断设备行。
- 实现带边框及显式焦点样式的 Receive/Input 面板、稳定当前记录游标、滚动、暂停、搜索、普通鼠标拖选、OSC 52 复制和 1 MiB 上限；实现 `j/k`、仅用于 `j/k` 的 count、`gg`、`G`、`yy` 及隔离的待定前缀，保留 SendEdit 鼠标光标语义和滚轮。
- 实现 Enter 成功提交后无配置分支地清空输入、失败时保留草稿、Alt+Up/Down 历史，以及 Backspace 删除末字符后列 0/视口起点不跳变。
- 按第 14 节实现快捷键和帮助页。

### 阶段 5：日志（会话日志与持久化主路径已实现）

- 实现 NDJSON v1、seq、UTC/单调时间和 UTF-8/base64 无损判定。
- 实现内部 Off、Waiting、Recording、Error 和界面 OFF、WAITING、REC、ERROR。
- 实现有界日志队列、flush、轮换、配额和目录锁。
- 实现 Session Log Settings 候选整体提交、Directory Enter 验证、下一会话应用和活动 REC 立即 rollover。
- 实现安全文件创建、清理和损坏尾行处理。

完整 diagnostics worker 不计入本阶段当前完成项；目前仅有 emergency/terminate 最小路径，内部诊断队列、安全 sink 和轮换仍归入阶段 7 剩余工作。

### 阶段 6：快捷发送与定时器（主要功能已实现）

- 实现 20 个快捷槽和安全持久化。
- 实现单次发送和 10ms 至 24h 定时任务。
- 实现 replacement confirmation、generation、missed 和停止语义。

### 阶段 7：加固与首版发布（待完成）

- 完成压力、故障注入、真实 USB-UART 和终端兼容测试。
- 编写安装、权限、配置、日志格式和快捷键文档。
- 验证所有首版验收标准。
- 在已接入的共享记录链路之外，补齐其余运行时类别的预分配预算与归还，完成 128 MiB 全局 token 闭环。
- 实现完整 diagnostics worker、内部有界队列、安全 fd sink、轮换、脱敏和运行时启用链路。

## 17. 验证归属与性能指标

既有单元、集成和 UI 测试条目保存在
`../lazycom-test/docs/product-test-plan.md`；实际测试清单见
`../lazycom-test/test_items.md`。本仓库保留产品性能与验收契约，不维护测试项。
只有用户明确要求时才可在测试仓库新增或执行测试，以下指标不构成执行授权。

### 17.4 压力指标

- 2 Mbaud，日志关闭，连续运行 8 小时。
- 2 Mbaud，日志开启并轮换，连续运行 8 小时。
- 短测覆盖 1、64、1024 和 65536 字节 frame 分布；1 字节 frame 用例用于测量并声明 record/s 上限，不能只测试大块吞吐。
- UI 停留在历史位置和搜索状态时持续接收。
- RSS 目标不超过空载基线加 192 MiB。
- 应用管理的 payload、容器和索引记账任何时刻不超过 128 MiB。
- 队列未超限时，应用层原始字节性质测试零丢失。
- 队列超限时必须产生可观察 ERR/gap，不能静默丢失或继续显示正常 REC。
- UI 保持可响应，断开和退出在可配置超时内完成。
- 空闲已连接状态必须阻塞在 readiness wait，不允许周期忙轮询；记录平均 CPU、wakeups/s 和命令唤醒延迟。
- 吞吐报告同时记录 bytes/s、records/s、CPU、RSS、队列高水位和 UI 响应延迟。

## 18. 首版验收标准

- 顶部标题只显示 `LazyCom` 或会话名称；唯一顶部状态行使用统一五态 Link、`HW:LOCKED`/`HW:UNLOCKED` 和 `[NewLine<N>]`、`[View<V>]`、`[InputType<H>]`、`[Log<g/G>]`，完整表达硬件锁、RX/TX 独立显示模式、实际活动串口配置和唯一 Log 状态。
- Disconnected 为红色、Connecting/Disconnecting 为 Gold、Connected 为绿色、Error 为红色粗体反选；持久 ErrorDialog 不会伪造 Link 状态。
- 发送区的未聚焦标题和占位、`i` 聚焦、多行真实光标、绿色焦点样式及上下文标题符合第 6.5 节，断开时仍可编辑。
- Receive 面板具有边框；未聚焦标题明显显示 `<R> Receive`，`R` 聚焦后状态保持 ReceiveBrowse 并显式改变标题和 Iris 边框，Esc 返回 Normal。
- 底部运行指标与动态动作提示严格分为两行，`LOGQ` 不冒充 Log 状态，顶部快捷键不在底部重复。
- 默认 rose-pine 和可选 transparent 背景符合配置；transparent 不发出任何背景色，快捷键始终为粗体 `#FFFFFF`，真彩色、ANSI 回退及极小终端下所有状态仍有文本标签。
- 启动和连接成功后保持 Normal，输入框不自动聚焦。
- 断开状态可以编辑草稿，提交不会发送且草稿保留。
- 成功 Enter 发送始终清空草稿并保留可通过 Alt+Up/Down 访问的历史；校验、未连接、队列或提交失败保留草稿，旧 `send.keep_after_send` 被忽略并迁移移除。
- Backspace 删除最后字符后光标和输入视口保持在列 0 起点，绝不跳到右边缘。
- 用户只能从 libserialport 本次扫描到的设备中选择，产品 UI 不提供设备路径文本入口；PTY 测试仍可使用内部明确路径。
- 每次打开设备选择前重新扫描。
- 启动权限预检、部分拒绝红色设备行、选择/连接重检和实际 open 权威结果符合第 7 节；错误指导可操作且应用从不修改系统权限或提权。
- `P/B/D/N/V/H` 只提供计划固定的预设选择，不允许原始字符串编辑；baud 集合与第 7.2 节完全一致。
- 硬件参数连接期间锁定，显示和发送参数连接期间可修改。
- `HW:LOCKED` 精确覆盖 Connecting、Connected、Disconnecting、Error 的设备、波特率、数据位、停止位、校验和流控，进入 Disconnected 后显示 `HW:UNLOCKED` 并解锁。
- 串口 owner 可以在读写、定时发送和错误状态下安全停止。
- RX 分帧可无损重建原始字节，并具有 64 KiB 硬上限。
- RX/TX 各自独立的 TXT、HEX、MIXED 不会把设备控制字符直接输出到终端；SYS 是应用/系统生命周期事件、ERR 是错误事件，二者只显示 TXT 文本消息，RX/TX/SYS/ERR 各有可见性开关。
- TX 请求不会交错，部分写入和失败可追踪。
- 所有后台队列和 UI 缓冲均有界。
- 关键 completion 在普通事件队列满载时仍可到达，连接状态不会永久停在过渡态。
- 20 个快捷发送槽可以安全保存、加载、编辑和单次发送。
- 定时发送只接受 0 或 10 至 86400000 毫秒，并且停止后没有迟到发送。
- NDJSON 日志具有 seq、UTC/单调时间、方向、UTF-8/base64 无损内容和明确时间语义。
- 日志内部使用 Off、Waiting、Recording、Error，用户准确显示 OFF、WAITING、REC、ERROR，`g` 转换符合第 11.1 节。
- `G` 两级 Session Log Settings 只配置目录和三项配额，候选整体提交；目录失败恢复内容与焦点，活动 REC 支持下一会话或立即 rollover，文件名/schema/format 不可配置。
- 日志文件权限、轮换、配额和安全删除符合计划。
- 配置语法或 schema 错误不会部分应用或覆盖原文件；三态保存结果与可见文件状态一致。
- legacy 单一 Receive View 可迁移到 `receive.rx_view` 和 `receive.tx_view`，新配置只使用 `txt`/`hex`/`mixed`；旧字段及 `send.keep_after_send` 在应用管理的重写中移除。
- 第 14 节快捷键按上下文生效且不会穿透错误组件。
- ReceiveBrowse 具有稳定当前记录游标，一行对应一条可见 RX/TX/SYS/ERR 记录，并支持 `j/k`、十进制 count+j/k、`gg`、`G`、`yy`；待定 `g/y/count` 不泄漏全局动作，count 不用于 `yy`。
- `q` 只在 Normal 和 ReceiveBrowse 触发退出，`Q` 未分配，F1 是唯一无条件穿透键。
- Normal/ReceiveBrowse 支持普通鼠标拖选并保留滚轮，SendEdit 鼠标保持光标语义；Normal 的 `y` 和被终端转发的 Ctrl+C 仅复制非空选择，ReceiveBrowse 的 `yy` 复制当前安全记录，OSC 52 payload 不超过 1 MiB，帮助说明 Ctrl+Shift+C 限制。
- 压力和故障注入测试达到第 17 节指标。

## 19. 第二阶段升级

### 19.1 多标签会话

- 每个标签具有独立连接、草稿、记录、滚动位置和 generation。
- 切换标签统一进入 Normal，草稿和浏览位置保留。
- 后台标签事件不能抢焦点，只更新徽标和非模态通知。
- 增加最大标签数和可滚动标签栏；默认所有标签共享首版 128 MiB。若实测必须扩大总预算，需单独审核并同步新的数值硬上限、RSS 指标和验收测试，不能由多标签隐式扩大。
- 再引入 Alt+数字及可靠的前后标签快捷键。

标签状态图标：

- `●`：已连接。
- `○`：断开。
- ASCII 回退为 `+` 和 `-`。
- 同时保留 LINK 文本状态，不能只依赖图标或颜色。

### 19.2 外部编辑器

- 编辑当前草稿，不创建空白草稿覆盖已有内容。
- 首选入口是命令面板的“Edit current draft externally”；是否增加 `/editor` 命令别名在第二阶段单独审核。
- 使用 `posix_spawnp()`，不在多线程进程中执行不安全的 fork 后 C++ 逻辑。
- 私有临时目录 0700、文件 0600。
- 所有无关 fd 使用 `CLOEXEC`。
- 限制回填文件大小，并验证普通文件、所有者和 UTF-8。
- 编辑器期间后台直接更新有界 session core，不积累无界 UI 事件。
- 返回时重新检查 connection generation；已断开则保留内容并回到 Normal。

### 19.3 编码和线路

- 引入 iconv 和 GB18030 流式解码测试。
- 增加 DTR、RTS 请求值显示，不能冒充真实输出采样。
- RTS/CTS 启用时禁用手工 RTS。
- Break 具有明确持续时间和异常清理。
- 断开时定义线路最终策略，并注明驱动不保证无毛刺。

### 19.4 稳定设备身份

- Linux 增加 udev/sysfs 辅助后端。
- 身份包含 interface number 和稳定 `ID_PATH`。
- 不能仅凭 VID、PID、序列号合并多口转换器。
- 稳定身份用于重插提示，自动重连仍需单独审核。

## 20. 后续实施门禁

1. 不创建业务源码前，先完成 `DevelopPlan.md` 阶段 0 的依赖、串口和性能 spike。
2. libserialport native fd readiness spike 未通过时停止实施并发起设计变更审核；只有同步修改两份计划、依赖和验收标准后才可改用单一 Linux termios 后端。
3. 可靠 completion、128 MiB 管理预算、NDJSON v1 和配置三态提交必须先有故障注入测试设计。
4. 所有入口继续严格执行已确认的 0 或 10ms 至 24h 定时范围。
5. 第二阶段功能默认不能扩大首版已经固定的安全、资源和所有权边界；任何扩大必须单独审核并同步明确硬上限、RSS 指标和验收测试。
