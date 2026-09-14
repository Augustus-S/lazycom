# LazyCom 初步修改计划

## 1. 执行基线与范围

- 计划日期：2026-09-09。
- 源码基线：`cbe09782b27946927f630ced9c54b38059319dd8`。
- 状态：源码与既有测试已做静态复核；实现、测试修改、编译和运行均未开始。
- 本文面向后续实施，描述拟修改位置，不表示相关能力已经实现或验证通过。
- 所有路径相对仓库根目录；行号对应上述提交的修改前位置。实施时用“路径 + 函数/类型/测试名”重新定位，禁止按旧行号机械替换。
- 本轮仅新增本文，不修改 `Plan.md`、`DevelopPlan.md`、生产代码、测试或依赖。
- 首批目标是单会话数据与交互可信度，不加入多标签、自动重连、协议解析、脚本、外部编辑器或新依赖。
- 既有 TOML 和 NDJSON 是持久化边界；本计划不改变格式，不引入旧内部接口的兼容层。

### 1.1 授权与恢复规则

1. 本计划完成后等待用户确认实施范围，不因计划中的复选框或命令自动开始编码。
2. 用户允许复用既有测试，但复用前必须再次检查测试正文、fixture、断言层级及发现名称；本文的复核不能替代实施时复核。
3. 新增或修改测试代码、编译均先取得明确授权。本轮不执行测试；后续执行前确认本次授权覆盖的构建和运行范围。
4. 每次恢复先检查 `git status --short` 和当前提交。若源码已变化，重新核对对应锚点并更新本文，保留其他人的修改。
5. 规范有冲突的任务保持阻塞，只暂停受影响任务，不把选择隐含在代码里。

## 2. 规范入口与待决策项

| 主题 | 规范锚点 | 实施约束 |
| --- | --- | --- |
| 覆盖层与事件优先级 | `Plan.md:135-160`；`DevelopPlan.md:717-723` | 覆盖层独立、可恢复；F1 唯一无条件快捷键；鼠标不能绕过 guard |
| 视口与选择 | `Plan.md:194-230`；`DevelopPlan.md:712-725` | 稳定 record ID；过滤先于截断；滚动保留选择；手动暂停独立 |
| 发送失败 | `Plan.md:265`；`DevelopPlan.md:708-709` | 同步拒绝保留编辑状态；admission 成功后才清空 |
| 事实时间与终结记录 | `Plan.md:462-474` | RX 首字节观察时间；TX 最后正返回时间；seq 才是排序依据 |
| 定时发送 | `Plan.md:547-561`；`DevelopPlan.md:434-440` | owner 真实边界仲裁；手工优先；最多一个 outstanding；停止有确认 |
| 日志生命周期 | `Plan.md:384-395,575-591` | 最终记录和日志关闭之后完成断开；错误详情不伪造 Link 状态 |
| worker 停止 | `DevelopPlan.md:401-432` | 固定停止通道、有 deadline、到达返回点后析构；正常主线程不等待 I/O |
| 预算 | `Plan.md:455-460`；`DevelopPlan.md:570-578` | 各 sink 配额与全局实际内存分别计数；生产接入前不声称闭环 |

**D1：窄屏状态优先级，待用户决策。** `Plan.md:192` 允许按优先级隐藏 Log 等字段，`:583` 又要求日志状态持续显示。`src/ui/tui.cpp:2216-2218` 目前只在宽度至少 156 列时显示 Log。建议常用宽度优先保留 Link 与 Log，极窄尺寸另定裁剪规则；是否提高 InputType/NewLine 优先级也需明确。未决策前不修改字段名称、优先级或增加第二条状态行。

**D2：退出搜索时目标在普通过滤下不可见。** 建议采用最近可见 record ID；若规范已有更具体规则则遵循，若无则先确认再实现跨视图回落。搜索期间保持命中、修复坐标混用不依赖此决策。

## 3. 顺序与检查点

任务编号用于跟踪，不等于一次提交。每个任务完成后应保持生产路径可用；同一文件的任务串行执行，禁止多个 agent 同时改 `application.cpp` 或 `tui.cpp`。

| 批次 | 任务 | 前置条件 | 检查点 |
| --- | --- | --- | --- |
| A | T01 发送结果；T02 通知与保存反馈 | 实施授权 | C1：失败不破坏输入，界面不误报保存成功 |
| B | T03 视口；T04 选择；T05 覆盖层 | 最小 UI 验证入口获准；D2 仅阻塞搜索退出策略 | C2：屏幕位置、选择和覆盖层可恢复 |
| C | T06 RX 元数据；T07 TX 元数据 | owner/数据事件接口复核 | C3：延迟 UI 不改变事实元数据 |
| D | T08 日志关闭边界；T09 writer 停止与重建 | 日志注入入口与测试修改获准 | C4：慢日志不提前完成断开，不阻塞正常 tick |
| E | T10 快捷槽；T11 帮助与状态布局 | T05；状态布局等待 D1 | C5：完整编辑与预览，窄屏帮助可访问 |
| F | T12 局部记录上限 | 保留现有日志与 UI 过载隔离 | C6：单条超限不能突破 UI 配额 |
| 后续 | T13 owner 调度；T14 全局数据路径；T15 发布诊断 | 首批稳定，分别细化接口并确认 | 不纳入首批完成承诺 |

T06/T07 属高优先级正确性修复，不能因为排在局部 UI 修复之后而在发布前跳过。若分配多名实施者，可并行核查和评审，代码改动仍按共享文件边界串行合并。

## 4. 首批任务

### T01：显式发送 admission 结果与编辑状态保持

**修改锚点：**

- `include/lazycom/app/application.hpp:171-176`：`submit_draft()` 声明与契约。
- `src/app/application.cpp:787-865`：`submit_tx()`、`submit_draft()` 的失败、历史写入和清稿分支。
- `src/ui/tui.cpp:749-755,1875-1899`：草稿同步、提交、历史切换。

**修改步骤：**

1. 使 `submit_draft()` 显式返回 admission 成功/拒绝结果，优先沿用项目 `Status/Result`；不把异步 terminal success 混入同步返回。
2. UI 仅在 admission 成功后清空并归零编辑状态；拒绝时不调用将光标移至末尾的整份同步。历史切换继续保留自己的定位规则。
3. 复查所有调用点与错误提示，保留“已接受后异步发送失败不能自动恢复并重发草稿”的语义。HEX 错误位置传递作为后续小任务，不扩大本次接口修改。

**验收：** 中间光标下未连接、非法 HEX、队列拒绝均保持草稿和编辑位置；成功提交清稿且历史可取回；空输入状态归零。

**测试复用：** R01、R02。现有 Application 断言不足以验证 FTXUI 光标，需经授权补最小组件断言；不能只增加 `route_key()` 枚举断言。

### T02：通知不遮蔽错误，保存反馈区分应用与持久化

**修改锚点：**

- `include/lazycom/app/application.hpp:56-89,268,299`：快照通知与保存接口。
- `src/app/application.cpp:510-512,1912-1947,2098-2118,2142-2188`：notice、日志设置、保存 admission/completion。
- `src/ui/tui.cpp:702,768-779,931,2385-2389`：UI 通知来源及显示优先级。

**修改步骤：**

1. 收敛 UI/Application 两个无关联字符串的显示仲裁。采用最小有界通知状态，携带严重度、更新身份和必要操作归属；不建设通用消息总线。
2. 复制等临时成功提示不能永久覆盖后来错误；过期使用单调时钟，不能依赖“下次按任意键清除”。关键失败和耐久性未知保持可见直到明确替换或确认。
3. 将保存 admission 结果传回调用者，移除无条件 `saved`。本进程设置生效与磁盘保存分别说明；completion 按文件/操作匹配，不由旧保存结果覆盖新候选的状态。

**验收：** “复制成功 → 保存失败/耐久性未知”显示后者；只读配置应用日志设置不显示已保存；连续配置编辑最终结果对应最新候选。保留三种 CommitState，不随意回滚本进程已应用设置。

**测试复用：** R03、R04；需经授权补 UI 最终显示和拒绝路径断言。新增通知状态必须有固定条数或单槽上限。

### T03：接收视口与搜索使用稳定锚点

**修改锚点：**

- `src/ui/tui.cpp:784-875`：可见索引与记录游标。
- `src/ui/tui.cpp:1144-1200`：搜索结果、定位和淘汰。
- `src/ui/tui.cpp:1626-1679,1840-1867`：刷新及搜索导航/退出。
- `src/ui/tui.cpp:2027-2096`：候选范围与实际 `yframe` 焦点。

**修改步骤：**

1. 分离 current record ID、viewport anchor、manual pause 和 at-bottom。正常尾随将最新记录置于可见区，不以“offset 为零”冒充渲染定位完成。
2. 搜索期间计数、导航、渲染统一使用搜索视图；普通过滤和搜索过滤不强制合并。offset 仅从相应视图派生。
3. 按实际面板高度确定视口或显式布局焦点，保留每条记录一行；新增、等量淘汰、过滤、resize 均按稳定 ID 归一化。退出搜索的不可见锚点策略按 D2 执行。

**验收：** 超过一屏及超过 200 条时最新记录可见；搜索隐藏于普通过滤的 RX 后 Custom 不跳位；暂停、返回底部互不替代；目标淘汰和空视图有确定回落。

**测试复用：** R01、R05。必须检查实际渲染记录 ID；索引测试不能替代屏幕断言。避免每帧构造全部记录的复杂 Element 树。

### T04：滚动保持 FTXUI 接收选区

**修改锚点：**

- `src/ui/tui.cpp:889-929,1681-1711,1901-1933`：Vim 命令、复制、滚轮和导航返回值。
- `src/ui/tui.cpp:2572-2573`：选区捕获回调。
- 只读依赖依据：`third_party/ftxui/src/ftxui/component/screen_interactive.cpp:805-807,851-865`。

**修改步骤：** 明确应用命令处理与 FTXUI selection 清除之间的适配；纯滚动保留选择，但无效前缀仍被消费且不落入 Normal；SendEdit 鼠标继续交给 Input。不得简单把全部已处理事件改成 `false`。

**验收：** 拖选后滚轮、箭头、分页、有效浏览命令和 Custom 刷新不主动清空已捕获内容；无效前缀不触发管理键；复制保持 1 MiB 上限和安全文本来源。

**测试复用：** R01 的前缀与路由守卫继续保留；补充验证必须经过 `OnEvent → HandleSelection` 等效真实链路，仅检查 `selected_text` 非空不足以证明高亮正确。禁止修改 vendored FTXUI。

### T05：覆盖层恢复绑定异步操作

**修改锚点：**

- `src/ui/tui.cpp:662-702,933-972,1053-1058`：覆盖层字段、Help、ErrorDialog、关闭。
- `src/ui/tui.cpp:1558-1561,1660-1667`：快捷槽保存等待与完成。
- `include/lazycom/ui/tui.hpp:13-22`：RouteMode 仅作为路由视图，不替代独立状态维度。

**修改步骤：** 将来源字段、光标、焦点和关联操作保存在有界 overlay frame 中；Help 压入/弹出来源而不复制零散返回变量。保存完成只结束或更新所属编辑 frame，不无条件关闭当前顶层。重复 F1 不无限增长栈，帧深度依据合法嵌套关系设上限。

**验收：** 保存中打开 Help，成功和失败均不自动关 Help；返回后无过期 Saving，失败候选保留；ErrorDialog 关闭事件不穿透，来源编辑状态恢复。

**测试复用：** R01、R06。需可控保存完成顺序，不能依赖真实磁盘“足够慢”制造竞态。范围限于恢复模型，不顺带重写所有弹窗。

### T06：RX 观察时间贯穿事件、分帧和记录

**修改锚点：**

- `include/lazycom/serial/service.hpp:90-99`：`SerialDataEvent`。
- `src/serial/service.cpp:1118-1162`：`read_ready()` 成功读取处。
- `src/framing/rx_framer.cpp:37-43,76-102`：帧首字节时间及 idle 比较。
- `src/app/application.cpp:900-975,1036-1042,1228`：帧投递、时间构造、会话时间基准。
- `include/lazycom/app/application.hpp:287-294,353`：记录入口和 session 时间状态。

**修改步骤：** 在 owner 正读取返回处捕获单调和 UTC 观察信息，传递到 framer；复用已有首字节时间，必要时扩展帧元数据，禁止在 UI drain 时重新取事实时间。核查 idle flush 与后续已排队读取的先后，避免仅新增时间字段却仍因 drain 上限错误截帧。session elapsed 的基准也在会话事实发生处捕获，不能简单用早于 UI 建立 session 的事件减去 UI 时刻后转无符号。

**验收：** owner 两次读间隔跨 idle gap，延迟 tick 后仍分为两帧；跨块 CRLF、尾帧不损失；UTC 不要求按 seq 递增；elapsed 不下溢。

**测试复用：** R07、R08。复用 framer 数据和 fake backend，需读取确认或可控时间，不通过任意 sleep 假定 owner 已读。

### T07：TX 模式与最后正返回时间贯穿终结事件

**修改锚点：**

- `include/lazycom/serial/service.hpp:44-47,90-99`：请求和事件不可变元数据。
- `src/app/application.cpp:787-865,971-975,1044-1050,2011-2078`：手工/快捷提交及日志投影。
- `src/serial/service.cpp:851,903,917,956,1007-1115`：`activate_tx()`、`write_ready()`、实际 `write_some()`、`finish_tx()`、`publish_tx_terminal()`。
- `tests/unit/data_path_test.cpp:449-490` 对应的 `TxOperation` 模型作为时间语义参照，不机械替换 owner 生命周期。

**修改步骤：** 输入模式在手工解析或快捷执行快照处捕获；每次正写返回更新最后接受时间，零写不更新时间。终结事件携带已接受前缀和对应元数据；Application 不再从全局当前模式推导。新增字段计入既有 admission/retained bytes 计数，不能削弱 terminal 槽预留。

**验收：** 排队后切换全局模式和不同模式快捷槽均记录原模式；部分写后超时/取消仍先 TX 前缀后 ERR；offset 为零不生成 TX；时间属于最后正返回而非 completion 消费时刻。

**依赖与测试：** 复用 T06 的事件时间表示，R08、R09；保留 NDJSON v1，不添加“设备已收到”语义。

### T08：日志终结结果之后再完成断开

**修改锚点：**

- `include/lazycom/app/application.hpp:305-317,354-359`：日志 owner、pending command、deferred completion。
- `src/app/application.cpp:1063-1103,1192-1217,1273-1305`：主动与故障 Cleanup/DisconnectCompletion。
- `src/app/application.cpp:1386-1404,1420-1513`：start/end 完成及重新开启判定。
- `src/app/application.cpp:2250`：`shutdown()` 共用清理边界检查。
- `src/app/state.cpp:147-162`：最终清除 session 的状态转换，优先保持状态机不变。

**修改步骤：** 将“未请求日志关闭”和“关闭已经结束”区分，明确保存该 session 的关闭终态；先处理最终记录，再提交 close，完成后才允许连接 completion 清除身份。统一主动断开、掉线和连接取消竞态；保留 operation/generation/session 匹配。

**关键陷阱：** `process_log_commands()` 在 `1496-1505` 允许 Disconnecting + Waiting 再次 `begin_log_session()`。延长 Disconnecting 时必须一起阻止清理后的重开，同时保留 pending Start 消费同一 session backlog 后关闭的合法路径。不能通过一直保留 `log_command_` 来假装屏障未结束。

**验收：** close 门闩未释放时 Link 为 Disconnecting 且 session/HW 锁仍有效；close 成功或已确认失败后清理一次；旧/重复 completion 不影响新会话；普通日志错误、超时回退和 OFF/ERROR 无活动文件路径均有限且可解释。

**测试复用：** R10、R11、R12。日志文件最终可读不能证明中间 Link 正确；需要 Application 日志 FS 注入，见第 6 节。

### T09：writer 停止协议与非阻塞重建

分成两个连续小任务，不能把 shutdown future ready 视为 worker 已经返回。

**T09a，停止协议。** 修改入口为 `include/lazycom/logging/session_writer.hpp:171-184`、`src/logging/session_writer.cpp:1275-1404,1621-1634`，以及 `src/app/application.cpp:1702-1719,2250` 的停止协调。复查已有 `wait_until_stopped()` 与 worker 返回点发布，优先复用固定状态；如缺少不分配 stop 请求，则补固定控制槽，不通过正常日志队列 admission 停止。普通文件失败与 bad_alloc/FATAL 分流，避免 OOM 路径先构造动态错误文本。

**T09b，异步重建。** 修改 `include/lazycom/app/application.hpp:303,333,368`、`src/app/application.cpp:1496-1505,1516-1546,1653-1672,1912-1947`。用有 deadline 的 pending 重建状态替代正时长 `wait_for`；正常 tick 只轮询就绪，确认旧 worker 到返回点后才销毁并激活替代 writer。连续设置只保留最新候选，不创建无界 retired writer 列表；退出期间不得再创建替代对象。

**验收：** 慢 close 不阻塞设置/tick；future 就绪但尚未返回时不析构；失败保留准确 Log:ERROR；FATAL 与超时仍使用既有有限回退；同一 writer 仅停止一次。

**依赖与测试：** T08 先稳定会话关闭归属；R11、R13。需把“文件 close 阻塞”和“worker 返回点延迟”分开注入，不能用前者代替后者。

### T10：强类型快捷槽编辑与发送预览

**修改锚点：**

- `include/lazycom/app/application.hpp:216-230`：槽位保存/执行入口。
- `src/app/application.cpp:1958-2008,2011-2078`：字符串拆分与快照执行。
- `src/ui/tui.cpp:1093-1104,1262-1284,1492-1515,2503-2509`：槽号解析、弹窗候选、提交和渲染。
- `src/config/schema.cpp:603-638`：现有合法值与长度约束，只读复用，不禁止 `|`。

**修改步骤：** 用现有 `QuickSendSlot` 候选替代 UI/Application 的六段字符串协议；删除采用明确槽位命令或空候选，不靠分隔符数量推断。构建 20 槽选择、独立字段、模式/实际后缀/字节数预览；执行仍提交不可变 bytes 快照，保存仍以提交三态决定更新内存。修改全部内部调用点，不保留无需求的旧字符串重载。

**验收：** `A|B`、多行 UTF-8、名称/备注含分隔符均可保存；槽 1/10/20、空槽、Tab/Home/End、默认 Cancel 正确；周期启动前可核对完整安全预览，保存失败候选保留。

**依赖与测试：** T05；R06、R14。本任务可先做强类型入口及同等字段能力，再做列表/预览，但不能把中间未完成 UI 声称为任务完成。

### T11：帮助可访问与状态布局

**修改锚点：** `src/ui/tui.cpp:1724-1728,2159-2224,2313-2367,2404-2425`。

**T11a，无需布局决策。** Help 改为可换行、可滚动内容；明确 F1/Esc 返回；覆盖层期间 footer 显示该层实际动作，不显示不可执行的 Normal 动作。保留终端 Ctrl+Shift+C 与 OSC 52 限制说明、透明背景约束。共享轻量命令描述仅用于减少重复，不引入通用注册框架。

**T11b，等待 D1。** 用户决定后先更新 `Plan.md:192,583` 及对应工程条款，再修改状态字段优先级和断点。不能在极窄宽度声称所有长名称都可见；需定义最小可用尺寸与降级提示。

**验收与测试：** R01 保留路由守卫；补 40×12、80×24、120 列及运行中 resize 的实际渲染检查。D1 未解决时只交付 T11a，不声称日志窄屏问题已解决。

### T12：可见记录的单条超限处理

**修改锚点：** `src/app/application.cpp:930-959`；`include/lazycom/app/application.hpp:30-39,70,74-75`。

**修改步骤：** 在 UI 接纳前核查单条逻辑开销，处理空队列下仍大于 maximum_bytes 的记录；超限走 UI gap/拒绝策略，不能插入后突破配额，也不能直接从整个 `enqueue_record()` 返回而跳过日志投递。确保 display_bytes 记账与所保留对象实际 capacity 一致，不以临时对象的 capacity 假定复制后对象相同。

**验收：** 1 MiB 缓冲与 1 MiB payload 加元数据不越界；空/非空队列、淘汰计数正确；UI 拒绝不阻断正常日志。该修复不宣称全局预算已经接入。

**测试复用：** R15 的 sink/gap 语义作为参照，Application fixture 扩展实际 UI 上限断言；不再构建一套独立预算框架。

## 5. 后续架构任务，不与首批混改

### T13：Scheduler 推进移入 owner

- 入口锚点：`include/lazycom/app/application.hpp:344-346`、`src/app/application.cpp:1250-1270,1595-1625,2011-2095`；owner 锚点：`src/serial/service.cpp:534,617-625,851-865`；命令边界：`include/lazycom/serial/service.hpp:170-185`。
- 先写任务启动/替换/停止的强类型命令与有界 completion 契约，再接入 owner 等待集合与真实 TX boundary，最后删除 Application 的执行推进，只保留 UI 投影。
- 同步核对 `CMakeLists.txt:62-69,90-100` 的 scheduler/serial 依赖方向，禁止因迁移引入循环。`SendCommand` 等既有 app 值类型不能演变为 owner 持有 Application。
- R09、R16 可复用纯 Scheduler 和 owner 部分写 fixture，但必须增加组合断言：不推进 UI tick 时任务仍按 best effort 工作；A 活动/B 定时待发/C 手工等待时边界先选 C；missed 不因 UI completion 消费延迟失真。
- 分为“命令与可靠 admission”“owner 调度接入”“Application 状态消费”三个步骤；接口未细化前不并行修改两端。

### T14：生产共享记录与全局预算

- 锚点：`include/lazycom/app/application.hpp:324-359`、`src/app/application.cpp:909-1015`、`include/lazycom/model/memory_budget.hpp:87`、`include/lazycom/model/session_sequencer.hpp:68`、`src/model/session_sequencer.cpp:81-104`。
- 分步接入生产预算所有权、共享记录 fan-out、各类别 admission；包括 `include/lazycom/serial/service.hpp:44-47,90-99` 和 `src/serial/service.cpp:1153` 的请求/事件分配，不仅替换可见 deque。
- 接入前列清 payload、容器/控制槽、framer scratch、历史、搜索、日志 backlog 的计费和释放点。复用现有 token/sequencer，不增加第三条记录路径。
- R15 只证明模型；需生产级小预算、失败归还和多 sink 共享验证，再做 RSS。未完成全部类别不得更新文档为“128 MiB 已闭环”。

### T15：诊断与第二阶段

- `DevelopPlan.md:11-13` 已标记完整 diagnostics、硬件及长时验收未完成。完整 diagnostics 单独计划安全 sink、脱敏、轮换和异常路径，不借会话日志保存内部调试数据。
- `Plan.md:32-40` 的人类可读导出、离线浏览和稳定 USB 身份单独设计；首批没有导出功能不算缺陷。
- 多标签等待单会话生命周期、生产预算和 worker 停止闭环。本文不提供其未经审核的接口方案。

## 6. 既有测试复核与复用表

以下用例在编写计划期间重新阅读过实现或由只读专项核查确认了断言范围，没有运行。表中的“缺口”是后续获准后需要补充的断言，不代表已获准新增测试。

路径简称只用于本节：U=`tests/unit/ui_routing_test.cpp`，A=`tests/unit/application_test.cpp`，D=`tests/unit/data_path_test.cpp`，S=`tests/unit/serial_service_test.cpp`，Q=`tests/unit/scheduler_test.cpp`，L=`tests/unit/logging_persistence_test.cpp`，M=`tests/unit/state_model_test.cpp`。

| 编号 | 用例名称与行号 | 已有断言 / 复用边界 |
| --- | --- | --- |
| R01 | U:14 `F1 is the only unconditional application route`；U:60 `editing browsing and search have contextual escape semantics`；U:91 `receive Vim commands parse bounded contextual sequences`；U:126 `bounded text editing moves and deletes on UTF-8 boundaries` | 纯路由、前缀计数、UTF-8 编辑；不经过真实 TUI handle/render/selection，不能证明视口和焦点 |
| R02 | A:279 `disconnected draft is retained and cannot create TX`；A:409 `send history enforces its configured byte limit` | 前者检查草稿、TX 和 notice，后者检查清稿和历史预算；缺失败后的真实光标/选区 |
| R03 | A:462 `rapid config edits persist the latest combined snapshot` | 最终配置包含组合修改；缺同步拒绝和最终通知显示顺序 |
| R04 | L:355 `persistence worker preserves atomic commit tri-state`；L:373 `persistence rejects read-only snapshots before filesystem access` | 保留三态/只读拒绝断言；不证明调用者没有覆盖错误 notice |
| R05 | A:855 `search direction is independent from the display filter` | 隐藏方向仍可搜索并返回 ID；缺 Custom/resize/退出搜索时屏幕锚点 |
| R06 | A:821 `quick-send deletion changes memory only after persistence commits` | 普通 AT 槽与删除提交后变化；缺保存失败、Help 叠加、字段内竖线分隔符 |
| R07 | D:86 `RX framer handles delimiters pending CR idle and maximum priority`；D:182 `RX idle checks are safe across extreme observation times` | 分隔与极端时间边界；没有证明生产 owner 时间传入或首字节时间完整贯穿 |
| R08 | D:449 `TX terminal helper records only accepted prefix then termination`；A:1054 `disconnect retains the written prefix of a partial TX` | 模型最后正返回时间和应用前缀保留；缺生产时间与 input_mode 断言 |
| R09 | S:456 `TX writes are partial nonblocking and never interleave requests`；S:928 `task stop cancels the matching partial write before confirmation` | 真实 owner 写序与停止确认；不证明周期任务由 owner 自主驱动 |
| R10 | A:1306 `pending log start closes before logging a reconnected session`；A:568 `logging settings roll an active file onto the new snapshot` | 最终重连/记录状态与文件内容；名称本身不证明日志 close 期间始终 Disconnecting |
| R11 | L:163 `session writer orders records and completes a flushed barrier`；L:207 `session flush failures complete barriers as writer failures`；L:265 `disable capacity reports an error and closes the producer window` | 水位、flush 失败、阻塞 close；缺 Application 状态与 worker 返回点时序 |
| R12 | M:39 `disconnect keeps its session until matching completion`；M:99 `cancelled connect retains a racing successful session until cleanup` | 纯连接身份守卫；不含真实日志关闭，作为状态机不变性回归 |
| R13 | A:534 `logging defaults rebuild an inactive writer`；L:224 `session worker exception boundary completes queued futures` | 配置/Waiting/Off 和普通异常 future 结算；不证明 UI 非阻塞或 OOM 固定停止路径 |
| R14 | Q:116 `quick send builds an owned execution from one of twenty slots`；Q:249 `replacement requires confirmation and old generation stops first` | 不可变执行和替换；不含 UI 字段编辑、预览及焦点 |
| R15 | D:279 `shared payload budget is charged once and released once`；D:322 `budget rejects before construction and returns all reservations`；D:359 `immutable batch returns metadata budget after every sink releases it`；D:426 `UI sink evicts oldest batches and preserves a sequence gap` | 模型计费/归还与 gap；没有覆盖 Application 复制链和单条 UI 超限 |
| R16 | Q:180 `busy immediate send waits for one boundary and misses later ticks`；Q:199 `fixed rate deadlines do not drift or accumulate requests`；Q:230 `manual requests win only when the writer reaches a boundary` | 测试主动提供 busy/manual boundary；不能替代 owner 集成仲裁 |

### 6.1 fixture 与最小验证入口

- A:34-206 `FakeBackend`、A:208-216 `test_paths`：可复用 RX、写限制、写失败与隔离路径。它们在匿名命名空间，不能直接跨翻译单元使用；只有第二个真实消费者出现时才提取共享 fixture，禁止复制整套 backend。
- S:30、S:276、S:329：串口 fake backend、factory、连接辅助；适合 owner 写入门闩及任务停止。新增时间控制必须和实际等待/唤醒一致，不能只替换 `now()` 留下真实 deadline 等待。
- L:21 `FakeLogState`、L:37 `FakeLogFileSystem`、L:73 close 门闩、L:146/154 builders：可复用慢 close/失败。`ApplicationDependencies` 当前在 `include/lazycom/app/application.hpp:92-99` 没有日志 FS 注入，需先批准最小依赖入口。
- D:53 `ScriptedSink`、D:75 `rx_draft` 与局部预算 fixture：复用有界 sink 语义，不以模型测试替代生产预算验证。
- `include/lazycom/ui/tui.hpp:143-159` 仅公开构造/run，`src/ui/tui.cpp:2557-2558` 固定创建默认 Application。应先确定最小的内部组件测试入口，允许注入 Application、输入事件和渲染尺寸，不把 Impl 全部公开，也不创建并行“测试版 UI”。
- 保存完成与 worker 返回点要有确定性控制；现有真实文件保存和等待循环不能稳定制造需要的竞态。
- `tests/CMakeLists.txt:1-16,22-41` 已包含主要用例并链接 UI。优先在现有文件中补 SECTION/参数化断言；只有现有结构确实无法表达时，申请新增测试文件并修改 target。

### 6.2 每次复用前的执行检查

1. 重新打开目标 TEST_CASE 全文和 fixture，确认仍测试生产使用的接口，而非已脱离生产的纯模型。
2. 写清“没有本次修复会失败的断言”，保留原断言，不为新实现弱化期待结果。
3. 检查连接/日志状态、时间门闩、超时、临时路径和 teardown；每个阻塞 fixture 必须在断言失败时也能解除等待。
4. 涉及测试修改先申请授权；未经授权只登记覆盖缺口，不编写例程。
5. 获准构建后先确认 CTest 当前发现名，再执行定向选择；结果为零项不算通过，不复述历史数量。

## 7. 验证策略与最终检查

以下仅为未来获准后的命令模板，不是本轮执行记录。所有命令从仓库根目录执行。

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug -N
ctest --preset gcc-debug -R "<本次发现结果确认的测试名称正则>"
ctest --preset gcc-debug
```

- 根据实际修改风险执行仓库验证矩阵：局部纯逻辑采用 GCC Debug 定向和完整测试；跨模块、串口、日志、持久化或并发修改需 GCC Debug、Clang Debug、GCC Release、no-diagnostics、ASan/UBSan；并发额外检查当前 libtsan 环境，不能沿用旧环境结论。
- 在 C1-C6 等可交付检查点合并相关验证，减少无意义重复；但失败修复后需重跑受影响项。是否授权完整矩阵在开始前确认。
- 只格式化本次修改的 C++ 文件；不编辑 build、依赖或历史报告。不为单个参数化断言单独搭建可执行文件。
- 真实硬件、终端复制能力、长时吞吐/RSS 和 OOM 故障注入单独登记，PTY 和静态阅读均不能替代。
- 每个任务最终检查正常、拒绝、取消、超限、迟到/重复完成及 shutdown 路径；检查没有新增无界队列、join 或持久化格式变化。

## 8. 进度登记

- [x] 确认基线提交、干净工作树及上次未落盘状态。
- [x] 复核主要源码锚点与既有测试实际断言范围。
- [x] 写入初步任务、依赖、待决策项和测试复用条件。
- [ ] 用户确认首批实施范围以及 D1/D2。
- [ ] 用户授权相应测试修改、编译与运行范围。
- [ ] T01-T12 实施及 C1-C6 验证。
- [ ] T13-T15 分别细化与审批。

后续实施者先从第 1.1 节恢复，不把本文当作已经完成的修复报告。
