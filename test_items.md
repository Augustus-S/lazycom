# LazyCom 测试项目人工审核清单

清点日期：2026-09-20。依据当前 `tests/` 源码、`tests/CMakeLists.txt`、`CMakePresets.json` 和本次 CTest 发现结果整理。描述的是实际断言的功能，不把测试名称或产品计划当作已覆盖的证据。

## 审核方法

- 每个 `Txxx` 是一项 CTest 测试；在“审核”栏填写 **保留 / 删除 / 合并到某编号 / 待定**。
- `Txxx.Fn` 是用例内的功能检查点；`Txxx.Sn` 是源码中的显式 `SECTION` 分支。可以只审核、删减其中一个检查点，不必整项处理。
- 勾选框表示“已审核”，不表示“保留”。部分删减意见直接写在对应检查点后；参数循环的实际输入、次数或边界列在正文。
- 英文原名可用于 CTest 搜索；源码链接指向当前文件和行。编号固定于本次清点，后续源码变化可能使行号和 CTest 顺序变化。

## 数量与执行入口

| 范围 | 数量 | 说明 |
| --- | ---: | --- |
| 完整 GCC Debug CTest 集合 | 105 | 104 个 Catch2 TEST_CASE + 1 个 CMake 构建回归 |
| `lazycom_tests` | 100 | 单元/组件测试及真实日志文件测试 |
| `lazycom_serial_backend_viability_tests` | 4 | Linux backend 三项与 PTY owner 一项 |
| `gcc-debug-fast` | 102 | 排除 T062、T063 的 extended 标签和 T105 的 build 标签 |
| 显式 SECTION 声明 | 66 | 已全部列为 S 子项；嵌套分支不等于独立 CTest 项，也不等于实际执行组合数 |
| 测试源码中的 static_assert | 12 | 单列为 C001–C012，不计入 105 项 |

本次只清点源码和发现测试，没有重新执行测试；不在此清单重复声明历史通过结果。第三方依赖自身未注册到项目 CTest 的上游测试不在这 105 项内。

| 编号范围 | 模块 / 源文件 | CTest 项 | SECTION |
| --- | --- | ---: | ---: |
| [T001–T018](#t001) | Application 应用协调 / [application_test.cpp](tests/unit/application_test.cpp) | 18 | 24 |
| [T019–T020](#t019) | 错误码与错误文本 / [base_error_test.cpp](tests/unit/base_error_test.cpp) | 2 | 0 |
| [T021–T031](#t021) | 配置安全文件与原子提交 / [config_safe_file_test.cpp](tests/unit/config_safe_file_test.cpp) | 11 | 2 |
| [T032–T042](#t032) | TOML 配置与兼容性 / [config_schema_test.cpp](tests/unit/config_schema_test.cpp) | 11 | 0 |
| [T043–T047](#t043) | RX 分帧、编码与预算 / [data_path_test.cpp](tests/unit/data_path_test.cpp) | 5 | 5 |
| [T048](#t048) | 依赖与诊断开关 / [dependency_smoke_test.cpp](tests/unit/dependency_smoke_test.cpp) | 1 | 0 |
| [T049–T056](#t049) | 日志 worker、共享记录与持久化 / [logging_persistence_test.cpp](tests/unit/logging_persistence_test.cpp) | 8 | 2 |
| [T057–T064](#t057) | NDJSON schema 与屏障 / [logging_schema_test.cpp](tests/unit/logging_schema_test.cpp) | 8 | 0 |
| [T065–T072](#t065) | 发送策略与纯调度器 / [scheduler_test.cpp](tests/unit/scheduler_test.cpp) | 8 | 0 |
| [T073–T087](#t073) | 串口 owner 与 scanner / [serial_service_test.cpp](tests/unit/serial_service_test.cpp) | 15 | 21 |
| [T088](#t088) | 致命信号并发 / [signals_completion_test.cpp](tests/unit/signals_completion_test.cpp) | 1 | 0 |
| [T089–T090](#t089) | 连接状态机 / [state_model_test.cpp](tests/unit/state_model_test.cpp) | 2 | 3 |
| [T091–T096](#t091) | 接收视图、按键与文本输入 / [ui_routing_test.cpp](tests/unit/ui_routing_test.cpp) | 6 | 2 |
| [T097–T100](#t097) | 真实日志文件与配额 / [logging_files_test.cpp](tests/integration/logging_files_test.cpp) | 4 | 0 |
| [T101–T103](#t101) | Linux/libserialport backend / [serial_backend_viability_test.cpp](tests/integration/serial_backend_viability_test.cpp) | 3 | 2 |
| [T104](#t104) | 真实 PTY owner 集成 / [serial_owner_pty_test.cpp](tests/integration/serial_owner_pty_test.cpp) | 1 | 5 |
| [T105](#t105) | 离线依赖构建 / [libserialport_build_test.cmake](tests/integration/libserialport_build_test.cmake) | 1 | 0 |

## 逐项清单

### Application 应用协调

<a id="t001"></a>

#### T001 启动状态与设备扫描

原名：`application restores startup state and integrates device scans`

源码：[tests/unit/application_test.cpp:319](tests/unit/application_test.cpp#L319)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T001.F1** 默认配置载入、扫描结果合并、串口选项校验、接收可见性恢复和权限提醒。
- [ ] **T001.S1** 默认 baud=115200，扫描得到指定设备；接受 baud=230400 和合法数据格式；拒绝 baud=0、全局 newline=Session、非法 newline/send mode/parity 枚举。（SECTION：`application loads defaults and merges scanner completion`；[源码](tests/unit/application_test.cpp#L321)）
- [ ] **T001.S2** 从真实 state.toml 恢复 RX=false、TX=true、SYS=false、ERR=true。（SECTION：`application restores receive visibility from state`；[源码](tests/unit/application_test.cpp#L348)）
- [ ] **T001.S3** 只接受本次扫描中的 /dev/null，拒绝不在结果中的 /dev/zero。（SECTION：`port selection accepts only the latest scan result`；[源码](tests/unit/application_test.cpp#L364)）
- [ ] **T001.S4** 全部设备被拒时提示 SerialPermissionDenied 且含 dialout；用户关闭提示后再次扫描不重复弹出。（SECTION：`startup scan publishes one alert when every device is denied`；[源码](tests/unit/application_test.cpp#L379)）

<a id="t002"></a>

#### T002 发送草稿准入

原名：`draft admission rejects disconnected oversized and invalid UTF-8 input`

源码：[tests/unit/application_test.cpp:410](tests/unit/application_test.cpp#L410)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T002.F1** 未连接时不能发送；超限和非法 UTF-8 输入不破坏已有草稿。
- [ ] **T002.S1** 未连接提交 AT+INFO，草稿保留、TX 字节为零，并提示 Not connected。（SECTION：`disconnected draft is retained and cannot create TX`；[源码](tests/unit/application_test.cpp#L413)）
- [ ] **T002.S2** 草稿限额 2 字节：A 后输入 A+中文或截断 UTF-8 都保留原 A，恰好 2 字节 AB 接受。（SECTION：`draft admission preserves valid UTF-8 and the previous contents`；[源码](tests/unit/application_test.cpp#L424)）

<a id="t003"></a>

#### T003 连接完成、取消竞争与活动串口配置

原名：`connection completion and cancellation preserve session data and hardware state`

源码：[tests/unit/application_test.cpp:443](tests/unit/application_test.cpp#L443)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T003.F1** 连接期间的会话数据和硬件配置快照在完成、取消及清理之间保持一致。
- [ ] **T003.S1** backend 在 open 时同时发布 ready+LF，Application 处理连接完成时仍保留 RX 记录。（SECTION：`receive data published with connect completion is retained`；[源码](tests/unit/application_test.cpp#L446)）
- [ ] **T003.S2** 连接前无 active_port_config；Connecting 即捕获 230400/7E2 配置，Connected 和 Disconnecting 保留，最终 Disconnected 清除。（SECTION：`application exposes the active serial configuration snapshot`；[源码](tests/unit/application_test.cpp#L464)）
- [ ] **T003.S3** owner 已 open 而主线程尚未处理完成时取消；最终断开、活动配置清除，真实日志仍保留 race 的 4 字节 RX。（SECTION：`cancel intent converges when owner connect wins the submission race`；[源码](tests/unit/application_test.cpp#L502)）

<a id="t004"></a>

#### T004 未录制时修改日志设置

原名：`inactive logging settings validate directories and rebuild the writer`

源码：[tests/unit/application_test.cpp:534](tests/unit/application_test.cpp#L534)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T004.F1** 校验日志目录，重建未活动 writer，并验证 OFF / WAITING 状态切换。
- [ ] **T004.S1** 空目录设置使用默认日志目录，保存 25 文件/128 MiB 总量/16 MiB 单文件上限；拒绝相对路径；日志开关可在 Off 与 Waiting 之间切换。（SECTION：`logging defaults rebuild an inactive writer`；[源码](tests/unit/application_test.cpp#L537)）
- [ ] **T004.S2** 拒绝不存在路径和普通文件；接受含竖线字符、权限安全的真实目录，以 NextSession 策略更新设置。（SECTION：`logging settings require an existing safe directory`；[源码](tests/unit/application_test.cpp#L561)）

<a id="t005"></a>

#### T005 配置异步保存与界面状态提交

原名：`configuration persistence serializes snapshots and gates visible changes`

源码：[tests/unit/application_test.cpp:594](tests/unit/application_test.cpp#L594)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T005.F1** 快速连续修改合并落盘；快捷发送变更在提交后可见；外部替换不成为新的保存基线。
- [ ] **T005.S1** 连续更改 baud、数据格式、newline 后读取真实 config.toml，必须是最后的完整组合快照。（SECTION：`rapid config edits persist the latest combined snapshot`；[源码](tests/unit/application_test.cpp#L597)）
- [ ] **T005.S2** 添加和删除快捷槽位 3 都先保留旧内存状态，异步持久化完成后才切换到新状态。（SECTION：`quick-send deletion changes memory only after persistence commits`；[源码](tests/unit/application_test.cpp#L618)）
- [ ] **T005.S3** 第一次保存已落盘、完成通知尚未消费时外部改写坏 TOML；后续修改和 shutdown 不覆盖这份外部内容。（SECTION：`save completion never adopts an external replacement as its baseline`；[源码](tests/unit/application_test.cpp#L639)）

<a id="t006"></a>

#### T006 日志启动未完成时重连或关闭

原名：`pending log starts converge through reconnect and shutdown`

源码：[tests/unit/application_test.cpp:669](tests/unit/application_test.cpp#L669)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T006.F1** 启动中的日志会话能够完成关闭，并接续重连或保留退出记录。
- [ ] **T006.S1** 连接后打开日志立即断开，再重连；最终连接为 Connected，日志为 Recording。（SECTION：`pending log start closes before logging a reconnected session`；[源码](tests/unit/application_test.cpp#L671)）
- [ ] **T006.S2** 日志启动尚未完成就 shutdown，真实日志仍包含 Serial session closed。（SECTION：`shutdown persists records queued behind a pending log start`；[源码](tests/unit/application_test.cpp#L692)）

<a id="t007"></a>

#### T007 发送历史容量与淘汰

原名：`send history enforces its configured byte limit`

源码：[tests/unit/application_test.cpp:708](tests/unit/application_test.cpp#L708)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T007.F1** 历史限额设为 1 MiB，依次发送两份 600,000 字节的 A、B 草稿；提交后草稿清空，完成后待发送数归零。
- [ ] **T007.F2** 连续向前翻历史只能得到最新的 B，旧 A 已按容量淘汰。

<a id="t008"></a>

#### T008 搜索方向、显示格式与可见性保存

原名：`search direction is independent from the display filter`

源码：[tests/unit/application_test.cpp:734](tests/unit/application_test.cpp#L734)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T008.F1** 即使显示过滤器隐藏 RX，按 RX 搜索仍找到 needle 并返回稳定 record ID；按 TX 搜索无结果；拒绝四种方向全部隐藏。
- [ ] **T008.F2** RX HEX、TX MIXED 独立渲染，SYS 保持文本；关闭后重新加载 state.toml，四种可见性与最后设置一致。

<a id="t009"></a>

#### T009 日志轮换期间关闭应用

原名：`shutdown completes an active log rotation before disconnecting`

源码：[tests/unit/application_test.cpp:794](tests/unit/application_test.cpp#L794)；标签：`[application][logging][shutdown]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T009.F1** 在三种轮换时序下，最终目录都保留 8 字节尾 RX 和 Serial session closed 记录。
- [ ] **T009.S1** 初始日志已经 Recording，再申请轮换后关闭。（SECTION：`rotation begins after the initial session start completes`；[源码](tests/unit/application_test.cpp#L805)）
- [ ] **T009.S2** 初始日志仍 Waiting，就申请轮换后关闭。（SECTION：`rotation is requested while the initial start is still pending`；[源码](tests/unit/application_test.cpp#L811)）
- [ ] **T009.S3** 初始日志仍 Waiting，申请轮换后先 disconnect、再 shutdown。（SECTION：`disconnect precedes shutdown while the initial start is pending`；[源码](tests/unit/application_test.cpp#L814)）

<a id="t010"></a>

#### T010 串口意外丢失后任务失效

原名：`unexpected serial loss invalidates a periodic task before reconnect`

源码：[tests/unit/application_test.cpp:843](tests/unit/application_test.cpp#L843)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T010.F1** 运行 1,000 ms 周期任务时注入读取失败，连接回到 Disconnected、任务回到 Idle；重连后任务不自动复活。
- [ ] **T010.F2** 重连前后可见记录的 record ID 严格递增。

<a id="t011"></a>

#### T011 断开时排空超过单批容量的 RX

原名：`disconnect drains more than 512 ordered RX events through cleanup`

源码：[tests/unit/application_test.cpp:881](tests/unit/application_test.cpp#L881)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T011.F1** 将 600 字节拆成 600 次单字节读取，在主线程处理前发起断开；最终 RX 计数和可见记录 payload 总量均为 600，避免 512 项批次边界丢尾数据。

<a id="t012"></a>

#### T012 部分发送后主动断开

原名：`disconnect retains the written prefix of a partial TX`

源码：[tests/unit/application_test.cpp:911](tests/unit/application_test.cpp#L911)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T012.F1** 提交 1,024 字节但仅允许写入 7 字节，再断开；TX 总计与可见 TX 记录都保留这 7 字节前缀。

<a id="t013"></a>

#### T013 发送失败的错误身份与记录顺序

原名：`TX errors retain their code and operation identifier`

源码：[tests/unit/application_test.cpp:937](tests/unit/application_test.cpp#L937)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T013.F1** 发送 failure 时写入 3 字节后设备丢失；可见 TX 在 ERR 之前，错误码为 SerialDeviceGone，operation ID 非零且对应 ERR 只有一条。
- [ ] **T013.F2** 重新解析真实日志，确认 3 字节 TX 在 LC-SER-2003 前且该错误只记录一次。

<a id="t014"></a>

#### T014 记录预算耗尽的降级与归还

原名：`record budget exhaustion stops logging and RX without a fatal error`

源码：[tests/unit/application_test.cpp:1001](tests/unit/application_test.cpp#L1001)；标签：`[application][logging][budget]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T014.F1** UI 记录预算设为 8 KiB，接收 8 KiB 加换行；断开连接、日志进入 Error、显示 gap 增加，且不进入 fatal_stopping。
- [ ] **T014.F2** 关闭并销毁 Application 后，预算计数归零。

<a id="t015"></a>

#### T015 worker 致命异常后的停止状态

原名：`fatal worker signal stops accepting operations and fails shutdown`

源码：[tests/unit/application_test.cpp:1027](tests/unit/application_test.cpp#L1027)；标签：`[application][shutdown]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T015.F1** 读取抛异常后进入 fatal_stopping 和 shutting_down；拒绝新的换行设置，断开请求不再改变连接状态，shutdown 返回失败。

<a id="t016"></a>

#### T016 扫描线程超时后的退出兜底

原名：`normal shutdown aborts instead of joining a timed-out scanner`

源码：[tests/unit/application_test.cpp:1049](tests/unit/application_test.cpp#L1049)；标签：`[application][shutdown]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T016.F1** 子进程中让 scanner 停在枚举，owner_stop_ms 和 log_barrier_ms 均设为 100；shutdown 必须在父进程 3 秒观察窗口内由 SIGABRT 终止，不能永久 join。

<a id="t017"></a>

#### T017 应用层手动发送优先级

原名：`busy manual sending keeps periodic requests behind all manual work`

源码：[tests/unit/application_test.cpp:1114](tests/unit/application_test.cpp#L1114)；标签：`[application]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T017.F1** 阻塞手动 A，提交周期 Q，再提交手动 B；解除阻塞后实际字节顺序必须为 ABQ。

<a id="t018"></a>

#### T018 连续修改日志目录与尾数据归属

原名：`repeated logging updates preserve a pending rotation and its tail`

源码：[tests/unit/application_test.cpp:1144](tests/unit/application_test.cpp#L1144)；标签：`[application][logging]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T018.F1** 连续 old → next → final 设置下，已产生的旧日志无不完整尾行；final 日志保留 12 字节尾 RX，以及正常关闭 SYS 或设备失败 ERR。
- [ ] **T018.S1** 等第一次轮换完成，再发第二次设置并断开；额外检查 next 目录日志无不完整尾行。（SECTION：`the first rotation completes before the second update`；[源码](tests/unit/application_test.cpp#L1173)）
- [ ] **T018.S2** 第一次轮换未完成时立即再次轮换，随后主动断开。（SECTION：`another immediate rotation followed by disconnect`；[源码](tests/unit/application_test.cpp#L1179)）
- [ ] **T018.S3** 第二次采用 NextSession，不能取消已经接受的轮换，随后主动断开。（SECTION：`next-session settings cannot cancel an accepted rotation`；[源码](tests/unit/application_test.cpp#L1180)）
- [ ] **T018.S4** 第一次轮换未完成时再次轮换，随后注入设备读取失败。（SECTION：`another immediate rotation followed by device loss`；[源码](tests/unit/application_test.cpp#L1183)）
- [ ] **T018.S5** 第一次轮换未完成时再次轮换，随后直接 shutdown。（SECTION：`another immediate rotation followed by shutdown`；[源码](tests/unit/application_test.cpp#L1186)）

### 错误码与错误文本

<a id="t019"></a>

#### T019 已发布错误码兼容性

原名：`published error codes retain their numeric values and identifiers`

源码：[tests/unit/base_error_test.cpp:8](tests/unit/base_error_test.cpp#L8)；标签：`[base]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T019.F1** 逐项固定 16 个 ErrorCode 的整数值与字符串标识：LC-VAL-1001、LC-SER-2001 至 2006、LC-CFG-3001 至 3004、LC-LOG-4001 至 4002、LC-DIAG-5001、LC-INT-9001 至 9002；另检查 SerialDeviceGone 的域为 serial。

<a id="t020"></a>

#### T020 错误详情的安全显示和长度边界

原名：`error detail is bounded UTF-8 safe and control visible`

源码：[tests/unit/base_error_test.cpp:43](tests/unit/base_error_test.cpp#L43)；标签：`[base]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T020.F1** LF、ESC、非法 0xFF 转为可见转义，合法 emoji 保留。
- [ ] **T020.F2** 512 字节上限：跨边界的两字节字符整字符舍弃，纯 ASCII 超长文本截到 512 字节。

### 配置安全文件与原子提交

<a id="t021"></a>

#### T021 安全读取、文件身份与摘要

原名：`safe reads retain file identity and a known SHA-256 digest`

源码：[tests/unit/config_safe_file_test.cpp:32](tests/unit/config_safe_file_test.cpp#L32)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T021.F1** 不存在文件返回空内容和 exists=false；读取实际状态文件时，原始字节、大小和 load_state_toml 的身份一致。
- [ ] **T021.F2** 文件内容 abc 的 SHA-256 与固定已知摘要一致。

<a id="t022"></a>

#### T022 不安全配置路径和权限拒绝

原名：`safe files reject symlinks sizes modes and NUL paths`

源码：[tests/unit/config_safe_file_test.cpp:61](tests/unit/config_safe_file_test.cpp#L61)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T022.F1** 拒绝符号链接读取/写入，5 字节文件在 4 字节上限下拒绝；失败写入不修改链接目标。
- [ ] **T022.F2** 拒绝文件模式 0700、0604、含 NUL 的路径，以及父目录权限 0500 下的缺失文件。

<a id="t023"></a>

#### T023 原子保存创建私有目录和文件

原名：`atomic writes create private nested directories and exact committed identity`

源码：[tests/unit/config_safe_file_test.cpp:90](tests/unit/config_safe_file_test.cpp#L90)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T023.F1** 在 umask=0777 下递归创建缺失目录，最终目录为 0700、普通文件为 0600。
- [ ] **T023.F2** 提交成功且内容精确；返回的 committed_identity 与重新读取的身份一致。

<a id="t024"></a>

#### T024 原子备份与不安全备份拒绝

原名：`atomic backup preserves the previous snapshot and refuses unsafe sources`

源码：[tests/unit/config_safe_file_test.cpp:118](tests/unit/config_safe_file_test.cpp#L118)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T024.F1** 正常替换生成 .bak 保存旧内容，再次替换后 .bak 更新为上一次版本。
- [ ] **T024.F2** .bak 为符号链接，或旧文件超过本次字节限额时拒绝提交，已有内容不变；共 replace / symlink / oversized 三组。

<a id="t025"></a>

#### T025 FIFO 配置路径不阻塞

原名：`configuration FIFO paths are rejected without waiting for a peer`

源码：[tests/unit/config_safe_file_test.cpp:155](tests/unit/config_safe_file_test.cpp#L155)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T025.F1** 分别在子进程中测试读取和原子写入；2 秒 alarm 内正常返回拒绝，不能等待 FIFO 对端。
- [ ] **T025.S1** FIFO 读取应快速返回拒绝。（SECTION：`read`；[源码](tests/unit/config_safe_file_test.cpp#L161)）
- [ ] **T025.S2** FIFO 原子保存应快速返回 NotCommitted。（SECTION：`atomic save`；[源码](tests/unit/config_safe_file_test.cpp#L162)）

<a id="t026"></a>

#### T026 保存后外部替换的身份冲突

原名：`committed file identity rejects a subsequent external replacement`

源码：[tests/unit/config_safe_file_test.cpp:184](tests/unit/config_safe_file_test.cpp#L184)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T026.F1** 第一次保存成功后通过 rename 换入外部文件；使用旧 committed_identity 再保存应 NotCommitted、无新身份，外部内容保留。

<a id="t027"></a>

#### T027 暂存后原文件原地修改

原名：`atomic writer detects in-place changes after staging`

源码：[tests/unit/config_safe_file_test.cpp:205](tests/unit/config_safe_file_test.cpp#L205)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T027.F1** stage 完成后修改目标内容并还原 mtime；commit 仍识别变化、拒绝覆盖，保留修改后的内容。

<a id="t028"></a>

#### T028 目标原先不存在时的创建竞争

原名：`atomic writer never replaces an unexpected newly-created target`

源码：[tests/unit/config_safe_file_test.cpp:230](tests/unit/config_safe_file_test.cpp#L230)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T028.F1** begin/stage 时目标不存在，提交前外部创建同名文件；commit 拒绝覆盖新目标。

<a id="t029"></a>

#### T029 配置目录锁的互斥与释放

原名：`atomic writer directory lock is nonblocking and released with owner`

源码：[tests/unit/config_safe_file_test.cpp:246](tests/unit/config_safe_file_test.cpp#L246)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T029.F1** 同目录已有原子写事务时，第二事务返回 busy；释放第一事务后重新开始成功。

<a id="t030"></a>

#### T030 原子写入调用方字节上限

原名：`atomic writer enforces the caller file limit`

源码：[tests/unit/config_safe_file_test.cpp:264](tests/unit/config_safe_file_test.cpp#L264)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T030.F1** 向 4 字节限额提交 5 字节，返回 NotCommitted 和错误；使用模拟文件系统。

<a id="t031"></a>

#### T031 原子提交的三态结果与身份

原名：`committed identity survives directory synchronization failures`

源码：[tests/unit/config_safe_file_test.cpp:274](tests/unit/config_safe_file_test.cpp#L274)；标签：`[config][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T031.F1** 正常、目录同步返回错误、目录同步抛异常三组分别得到 Committed 或 CommittedDurabilityUnknown，均保留正确的 device/inode/size 身份。
- [ ] **T031.F2** Begin、Stage、Commit 三处故障均为 NotCommitted 且无 committed_identity。

### TOML 配置与兼容性

<a id="t032"></a>

#### T032 配置默认值、枚举解析与序列化

原名：`config defaults and enums obey the published schema`

源码：[tests/unit/config_schema_test.cpp:33](tests/unit/config_schema_test.cpp#L33)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T032.F1** 最小 version=1 文档得到 ConfigSnapshot 默认值，可写，序列化后重新解析快照相同。
- [ ] **T032.F2** 大小写混合的 parity、flow_control、send mode/newline、RX/TX view、background 均接受；序列化输出规范小写值。

<a id="t033"></a>

#### T033 无效配置整份拒绝及字段定位

原名：`config rejects whole invalid candidates and reports every field`

源码：[tests/unit/config_schema_test.cpp:69](tests/unit/config_schema_test.cpp#L69)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T033.F1** 逐项输入错误语法、version=2、background=solid、baud=12345、日志目录含 NUL，均拒绝且只读，并返回对应错误路径。
- [ ] **T033.F2** 一份配置同时含 baud=0、data_bits 类型错误、非法 parity、idle_gap_ms=0、visible_buffer_mib=49、日志大小组合错误、tx_max_mib=9、connect_ms=99；返回默认快照并报告各错误字段。
- [ ] **T033.F3** 直接校验 snapshot 也拒绝日志路径 NUL。

<a id="t034"></a>

#### T034 未知配置字段往返保留

原名：`config unknown keys and values survive a changed snapshot`

源码：[tests/unit/config_schema_test.cpp:115](tests/unit/config_schema_test.cpp#L115)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T034.F1** 修改 RX 视图后序列化再解析，已知快照正确；future_root、ui.future_ui、send.future_send 的值保留，解析前后都报告未知字段警告。

<a id="t035"></a>

#### T035 旧配置迁移与新字段优先

原名：`legacy settings migrate while explicit receive views take precedence`

源码：[tests/unit/config_schema_test.cpp:145](tests/unit/config_schema_test.cpp#L145)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T035.F1** 三组 keep_after_send / 显式视图组合：旧 receive.view=text 映射到 RX/TX TXT，显式 RX HEX/TX MIXED 优先。
- [ ] **T035.F2** 对旧字段给警告，重写删除 keep_after_send 和旧 view，保留未知字段，重新解析快照一致。

<a id="t036"></a>

#### T036 配置中的分类及组合内存预算

原名：`managed budgets count payload metadata and current TX plus draft`

源码：[tests/unit/config_schema_test.cpp:179](tests/unit/config_schema_test.cpp#L179)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T036.F1** 同时超限时报告 UI、日志、RX ingress、TX、历史、owner/scratch 和总预算路径。
- [ ] **T036.F2** TX 预算除排队 payload 外计入当前 TX 和草稿；记录数或消息数很大时，即使 payload MiB 很小，元数据仍使 UI/TX/owner 预算超限。

<a id="t037"></a>

#### T037 快捷发送稀疏槽位与坏输入

原名：`quick-send schema validates sparse empty and malformed slots`

源码：[tests/unit/config_schema_test.cpp:209](tests/unit/config_schema_test.cpp#L209)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T037.F1** 接受槽位 1、20，保留中间空槽，并接受一个缺少 index 的附加条目；检查槽位 20 的 HEX 模式。
- [ ] **T037.F2** 拒绝坏 HEX、非法 newline、重复 index 并定位字段；21 个条目触发 slots 上限。

<a id="t038"></a>

#### T038 快捷发送字段及模型总预算

原名：`quick-send snapshot validation bounds text and aggregate model memory`

源码：[tests/unit/config_schema_test.cpp:246](tests/unit/config_schema_test.cpp#L246)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T038.F1** 名称 65 字节、非法 UTF-8 内容、备注 257 字节分别报告错误。
- [ ] **T038.F2** 四个槽位各放 1 MiB 内容，触发 model_and_search 总预算限制。

<a id="t039"></a>

#### T039 TOML 输入文档硬上限

原名：`TOML parsers reject oversized documents before parsing`

源码：[tests/unit/config_schema_test.cpp:269](tests/unit/config_schema_test.cpp#L269)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T039.F1** config 超过 1 MiB、quick_send 超过 4 MiB、state 超过 64 KiB 时返回 $document 错误。

<a id="t040"></a>

#### T040 快捷发送满槽的反复清空与重填

原名：`quick-send full slots preserve unknown keys through repeated clear and refill`

源码：[tests/unit/config_schema_test.cpp:285](tests/unit/config_schema_test.cpp#L285)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T040.F1** 填满 20 槽，对槽位 1 做三轮清空/重填；每次重写后重新解析都等于目标快照，根和槽位上的未知字段都保留。

<a id="t041"></a>

#### T041 TOML 序列化后的可读性和组合预算

原名：`TOML serialization obeys next-load byte and combined memory limits`

源码：[tests/unit/config_schema_test.cpp:324](tests/unit/config_schema_test.cpp#L324)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T041.F1** 拒绝超限的 state 保留文档；1 MiB NUL 内容经过 TOML 转义膨胀后不得输出超限 quick_send 文档。
- [ ] **T041.F2** 已知槽位内容 2.5 MiB 与未知保留字段 1 MiB 单独都合法，组合序列化超预算时拒绝。

<a id="t042"></a>

#### T042 UI 状态的间隔、槽位与可见性

原名：`state validates intervals and visibility while preserving unknown keys`

源码：[tests/unit/config_schema_test.cpp:346](tests/unit/config_schema_test.cpp#L346)；标签：`[config]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T042.F1** 接受间隔 0、10、86,400,000 ms 和槽位 20；拒绝间隔 9、86,400,001 ms 及槽位 21。
- [ ] **T042.F2** 四类可见性正确解析并往返保存，未知 future_ui 保留；四类全隐藏时整份拒绝且只读。

### RX 分帧、编码与预算

<a id="t043"></a>

#### T043 RX 分帧的分隔符、空闲与大小边界

原名：`RX framing preserves delimiter idle and size boundaries`

源码：[tests/unit/data_path_test.cpp:55](tests/unit/data_path_test.cpp#L55)；标签：`[data-path][framing]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T043.F1** 五个分支分别检查跨读取 CRLF、单独 CR、尾帧 flush、最大帧优先及极端观察时间。
- [ ] **T043.S1** abc+CR 与下一读取的 LF+def+LF 合成两帧，CRLF 不被错误拆开。（SECTION：`CRLF is joined across reads`；[源码](tests/unit/data_path_test.cpp#L59)）
- [ ] **T043.S2** a+CR 后读取 b，先输出 a+CR，flush 再得到 b。（SECTION：`pending CR is emitted before a non-LF byte`；[源码](tests/unit/data_path_test.cpp#L68)）
- [ ] **T043.S3** 49 ms 不 flush，50 ms 输出含 pending CR 的尾帧；断开 flush 保留无分隔符 last。（SECTION：`idle and disconnect flush pending and tail data`；[源码](tests/unit/data_path_test.cpp#L77)）
- [ ] **T043.S4** 最大帧 2 字节时 A+CR+LF 分成 A+CR 和 LF，大小规则优先。（SECTION：`maximum frame wins over CRLF`；[源码](tests/unit/data_path_test.cpp#L88)）
- [ ] **T043.S5** 时钟从 min 到 max 时排出旧数据；从 max 观察 min 不误触发 idle，最终 flush 保留尾数据。（SECTION：`idle checks across extreme observation times`；[源码](tests/unit/data_path_test.cpp#L95)）

<a id="t044"></a>

#### T044 固定种子的随机分帧重建

原名：`RX framing reconstructs seeded random streams`

源码：[tests/unit/data_path_test.cpp:112](tests/unit/data_path_test.cpp#L112)；标签：`[data-path][framing][property]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T044.F1** 种子 0x4C415A59，200 轮；随机长度 0–1,023、最大帧 1–64 字节、读取块 1–31 字节和任意 0–255 字节。
- [ ] **T044.F2** 各次 push 加最终 flush 的所有帧拼接后必须与输入逐字节相同。

<a id="t045"></a>

#### T045 密集分隔符的内存容量

原名：`RX delimiter floods retain only small frame capacities`

源码：[tests/unit/data_path_test.cpp:141](tests/unit/data_path_test.cpp#L141)；标签：`[data-path][framing]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T045.F1** 65,536 字节最大帧配置下，一次输入 2,048 个 CRLF；产生 2,048 个精确帧，各帧 capacity 及 capacity 总和均小于最大帧大小。

<a id="t046"></a>

#### T046 字节显示、严格 UTF-8 与发送解析

原名：`encoding projects safe text and parses complete TX payloads`

源码：[tests/unit/data_path_test.cpp:162](tests/unit/data_path_test.cpp#L162)；标签：`[data-path][encoding]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T046.F1** 遍历 0–255 字节，TXT 输出不含原始 NUL/ESC/CR/LF；HEX 长度为 767；检查 MIXED 的 A 与 41 双表示及反斜杠转义。
- [ ] **T046.F2** UTF-8 欧元符号分两次输入后合成；帧尾残缺 F0 和坏续字节 E2+A 可见转义；bidi U+202E 转义。
- [ ] **T046.F3** TXT 保留合法 CRLF、拒绝 C0 AF；HEX 接受 0x 前缀、空格/Tab/CRLF。拒绝 0、0001、0x、0x0、gg、01\v02、0xx1、01,02、00 1 02，检查错误偏移 0 或 3。

<a id="t047"></a>

#### T047 内存预算预留、移动与归还

原名：`memory budget rejects overcommit and releases moved reservations`

源码：[tests/unit/data_path_test.cpp:215](tests/unit/data_path_test.cpp#L215)；标签：`[data-path][budget]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T047.F1** 分类额度设为 3 字节，预留满额后新增 1 字节失败；移动 token 不重复计费，reset/析构后总量为零。
- [ ] **T047.F2** 分类上限之和超过全局限额时，预算对象构造抛 invalid_argument。

### 依赖与诊断开关

<a id="t048"></a>

#### T048 依赖版本与诊断编译开关

原名：`compiled dependency versions are available`

源码：[tests/unit/dependency_smoke_test.cpp:5](tests/unit/dependency_smoke_test.cpp#L5)；标签：`[dependencies]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T048.F1** libserialport 版本受支持且精确为 0.1.2。
- [ ] **T048.F2** 启用 diagnostics 的构建报告 compiled_in=true、backend_version>0；关闭构建报告 false 和 0。

### 日志 worker、共享记录与持久化

<a id="t049"></a>

#### T049 日志写入顺序与刷写屏障

原名：`session writer orders records and completes a flushed barrier`

源码：[tests/unit/logging_persistence_test.cpp:177](tests/unit/logging_persistence_test.cpp#L177)；标签：`[logging][worker]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T049.F1** 入队 seq=1、2 后 barrier(2) 确认；模拟文件中顺序正确且只 flush 一次；明确不声称 fsync 保证。

<a id="t050"></a>

#### T050 生产记录共享、淘汰和预算生命周期

原名：`production records share storage through UI eviction and log completion`

源码：[tests/unit/logging_persistence_test.cpp:196](tests/unit/logging_persistence_test.cpp#L196)；标签：`[application][logging][budget]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T050.F1** UI 和日志持有同一个 SessionRecord/payload；阻塞日志写入后清空 UI，日志引用仍保留内存，barrier 完成后引用和预算释放。
- [ ] **T050.F2** 可见条数上限 2 时淘汰旧记录并计 gap；finish 后拒绝 append，新 session 的 record ID 继续增长而 sequence 从 1 开始；Cleanup 后拒绝普通事件。
- [ ] **T050.F3** 8 KiB 预算下验证先淘汰再重试；外部仍持有旧引用时返回 BudgetExhausted、不推进 sequence，释放后归零。

<a id="t051"></a>

#### T051 日志队列超限的终止状态

原名：`session queue overload is nonblocking and terminal`

源码：[tests/unit/logging_persistence_test.cpp:284](tests/unit/logging_persistence_test.cpp#L284)；标签：`[logging][worker]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T051.F1** 队列字节限额 1，记录入队返回 QueueFull，writer 转 Error，barrier 返回 WriterFailed。

<a id="t052"></a>

#### T052 日志 I/O 失败和异常的完成通知

原名：`session IO failures and exceptions complete queued futures`

源码：[tests/unit/logging_persistence_test.cpp:296](tests/unit/logging_persistence_test.cpp#L296)；标签：`[logging][worker]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T052.F1** flush 返回错误、append 抛异常两组都必须结束已提交 future：barrier=WriterFailed、writer/shutdown=Error。
- [ ] **T052.S1** flush 返回 LoggingDiskFull 错误。（SECTION：`flush returns an error`；[源码](tests/unit/logging_persistence_test.cpp#L299)）
- [ ] **T052.S2** append 在 worker 边界抛 runtime_error。（SECTION：`append throws at the worker boundary`；[源码](tests/unit/logging_persistence_test.cpp#L300)）

<a id="t053"></a>

#### T053 重复关闭的合并与等待者上限

原名：`disable shares one close while enforcing its waiter capacity`

源码：[tests/unit/logging_persistence_test.cpp:311](tests/unit/logging_persistence_test.cpp#L311)；标签：`[logging][worker]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T053.F1** 阻塞 close，连续 256 个 disable 共享一次关闭；期间拒绝新记录，第 257 个等待者返回容量错误。
- [ ] **T053.F2** 解除阻塞后 256 个 future 全部成功、close 只执行一次；disable 不退出线程，shutdown 才完成 worker 停止。

<a id="t054"></a>

#### T054 日志屏障/控制容量耗尽不悬挂

原名：`barrier and control hard limits never strand futures`

源码：[tests/unit/logging_persistence_test.cpp:351](tests/unit/logging_persistence_test.cpp#L351)；标签：`[logging][worker]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T054.F1** 提交 400 个尚无对应记录的 barrier，再请求 disable；最终为 Off，所有 barrier future 都就绪并返回 WriterFailed。

<a id="t055"></a>

#### T055 持久化只读拒绝与提交身份

原名：`persistence rejects read-only saves and preserves committed identity`

源码：[tests/unit/logging_persistence_test.cpp:369](tests/unit/logging_persistence_test.cpp#L369)；标签：`[config][persistence]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T055.F1** 只读 config 保存返回 ReadOnly 和错误；模拟目录同步失败的 state 保存返回 CommittedDurabilityUnknown，完成通知携带 State 类型、已提交身份和序列化文档，身份大小与文档一致。

<a id="t056"></a>

#### T056 持久化重叠请求及停止排空

原名：`persistence rejects overlapping saves and drains accepted files on stop`

源码：[tests/unit/logging_persistence_test.cpp:391](tests/unit/logging_persistence_test.cpp#L391)；标签：`[config][persistence]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T056.F1** config 提交阻塞时重复保存同文件返回 Busy，另一个 state 保存可接受。
- [ ] **T056.F2** request_stop 后 20 ms 内不能假称已停止，新保存返回 Stopping；放行后在限时内完成两个 Committed 结果并退出。

### NDJSON schema 与屏障

<a id="t057"></a>

#### T057 NDJSON 文件头、设备元数据和版本

原名：`NDJSON header preserves metadata and version compatibility`

源码：[tests/unit/logging_schema_test.cpp:49](tests/unit/logging_schema_test.cpp#L49)；标签：`[logging][schema]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T057.F1** 文件头以 LF 结尾，版本 1.0；非法 UTF-8/含 NUL 的 product 走 Base64，重新解析头部字段一致。
- [ ] **T057.F2** major=2 返回 UnsupportedMajorVersion；同 major 的 minor=7 和未知字段接受。

<a id="t058"></a>

#### T058 NDJSON payload 编码与规范性

原名：`NDJSON payload encoding is byte exact bounded and canonical`

源码：[tests/unit/logging_schema_test.cpp:84](tests/unit/logging_schema_test.cpp#L84)；标签：`[logging][schema]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T058.F1** 含 NUL/ESC/中文/LF 的合法 UTF-8、1 MiB NUL、0–255 全字节分别往返精确，物理行不超限且只有末尾一个 LF；必要时回退 Base64。
- [ ] **T058.F2** 拒绝孤立续字节、过长编码、代理区、超过 Unicode 上限和截断 UTF-8，合法 emoji 接受。
- [ ] **T058.F3** Base64 AaD/fg== 精确恢复；非规范 padding 位 /x== 拒绝；应以 UTF-8 表示的 A 写成 QQ== 也拒绝。

<a id="t059"></a>

#### T059 NDJSON 记录字段、UTC 和安全消息

原名：`NDJSON record fields and safe messages obey their schema`

源码：[tests/unit/logging_schema_test.cpp:119](tests/unit/logging_schema_test.cpp#L119)；标签：`[logging][schema]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T059.F1** 拒绝 seq=0，接受 uint64 最大 elapsed_ns；拒绝负 elapsed、非闰年 2 月 29 日、24 点、60 秒、非 Z 时区和空小数秒，接受合法闰日及小数秒。
- [ ] **T059.F2** ERR 中 NUL/ESC 转为安全文本并保留 LC-SER-2003，拒绝 SERIAL-2003；SYS 正常消息可往返。

<a id="t060"></a>

#### T060 NDJSON 物理行与不完整尾行恢复

原名：`NDJSON physical lines preserve complete records and ignore unterminated tails`

源码：[tests/unit/logging_schema_test.cpp:170](tests/unit/logging_schema_test.cpp#L170)；标签：`[logging][schema]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T060.F1** 拒绝行内原始 LF/CR 和额外空行，接受 CRLF 结尾；payload 中换行经 JSON 转义，不增加物理行。
- [ ] **T060.F2** 整文档拒绝重复 seq；追加截断 JSON 或完整但无 LF 的 JSON 时，仅恢复前两条完整记录，标记 IncompleteTail。

<a id="t061"></a>

#### T061 不可信 NDJSON 的资源硬上限

原名：`untrusted NDJSON bounds bytes nesting and structural complexity`

源码：[tests/unit/logging_schema_test.cpp:216](tests/unit/logging_schema_test.cpp#L216)；标签：`[logging][schema]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T061.F1** 拒绝超过 2 MiB 的物理行、Base64 对应 payload 超过 1 MiB、超过 16 MiB 的整文档。
- [ ] **T061.F2** 未知字段嵌套 20 层或增加 300 个字段时，返回 LimitExceeded；此项仍在快速集。

<a id="t062"></a>

#### T062 NDJSON 整文档编码的数量和字节上限

原名：`NDJSON encoder enforces record and accumulated byte limits`

源码：[tests/unit/logging_schema_test.cpp:258](tests/unit/logging_schema_test.cpp#L258)；标签：`[logging][schema][extended]`；快速集：否（完整集执行）。

审核：____；合并目标 / 理由：____。

- [ ] **T062.F1** 构造 100,001 条记录，编码返回 LimitExceeded。
- [ ] **T062.F2** 构造 13 条各含 1 MiB NUL 的记录，累计编码超过 16 MiB 时拒绝；带 extended 标签，快速集排除。

<a id="t063"></a>

#### T063 NDJSON 整文档解码的记录数上限

原名：`NDJSON document enforces its record count limit`

源码：[tests/unit/logging_schema_test.cpp:277](tests/unit/logging_schema_test.cpp#L277)；标签：`[logging][schema][extended]`；快速集：否（完整集执行）。

审核：____；合并目标 / 理由：____。

- [ ] **T063.F1** 构造字节数仍小于 16 MiB、但含 100,001 条 SYS 的合法逐行文档，解码返回 LimitExceeded；带 extended 标签，快速集排除。

<a id="t064"></a>

#### T064 屏障必须被覆盖目标序号的 flush 确认

原名：`barriers require a flush covering their own target`

源码：[tests/unit/logging_schema_test.cpp:297](tests/unit/logging_schema_test.cpp#L297)；标签：`[logging][barrier]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T064.F1** 目标 seq=5，在只处理至 4 时，先前 flush 无论成功还是失败都不能确认屏障；处理至 5 后仍需新的 flush。
- [ ] **T064.F2** 新的失败 flush 得到 FlushFailed，重试成功才 Confirmed；检查已处理/已刷写序号、不保证 fsync、拒绝序号倒退。

### 发送策略与纯调度器

<a id="t065"></a>

#### T065 发送内容与换行策略

原名：`payload policy resolves suffixes and fails atomically`

源码：[tests/unit/scheduler_test.cpp:52](tests/unit/scheduler_test.cpp#L52)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T065.F1** TXT 的 None/LF/CR/CRLF/Session 五种策略得到精确后缀；HEX 41 54 加 LF 得到 AT 加换行。
- [ ] **T065.F2** 坏 HEX、坏 UTF-8、Session 无有效继承值、非法 mode、追加 LF 后超 1 MiB 都失败且不留下半成品；超大且 UTF-8 非法时优先 InvalidText。

<a id="t066"></a>

#### T066 快捷发送执行快照与槽位校验

原名：`quick send owns its execution and validates the selected slot`

源码：[tests/unit/scheduler_test.cpp:103](tests/unit/scheduler_test.cpp#L103)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T066.F1** 槽位 20 的 HEX 内容、继承 CR 与 slot ID 构造为独立执行快照；之后修改源槽内容不会改变待执行字节。
- [ ] **T066.F2** 槽位 0 返回 SlotOutOfRange，空槽 1 返回 EmptySlot。

<a id="t067"></a>

#### T067 单次任务的启动和完成

原名：`scheduler validates intervals and retires a one-shot task`

源码：[tests/unit/scheduler_test.cpp:133](tests/unit/scheduler_test.cpp#L133)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T067.F1** 9 ms 间隔拒绝且保持 Idle；0 ms 作为单次任务，无下一 deadline。
- [ ] **T067.F2** 首次 TX 边界只发一次且 generation 匹配；完成后回到 Idle，旧 generation 不再可接受。

<a id="t068"></a>

#### T068 固定频率不漂移、不积压

原名：`fixed rate deadlines do not drift or accumulate requests`

源码：[tests/unit/scheduler_test.cpp:155](tests/unit/scheduler_test.cpp#L155)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T068.F1** 10 ms 周期下在 10、39、75 ms 处理迟到 tick，missed_count 分别按跳过周期累计，下一 deadline 为 40、80 ms。
- [ ] **T068.F2** 正在发送时不再入队，不用无关完成解锁；下一请求 sequence=2，失败完成不增加 sent_count。

<a id="t069"></a>

#### T069 手动优先与首发触发器保留

原名：`manual priority preserves only the initial pending trigger`

源码：[tests/unit/scheduler_test.cpp:184](tests/unit/scheduler_test.cpp#L184)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T069.F1** 三组组合：首发未执行且未显式处理 deadline、首发未执行且显式处理、首发已完成。
- [ ] **T069.F2** 手动占用时累计错过周期，只保留首次待触发；后续错过的周期不补发，下一有效 deadline 恢复发送。

<a id="t070"></a>

#### T070 任务替换确认与迟到完成

原名：`replacement requires confirmation and rejects stale completion`

源码：[tests/unit/scheduler_test.cpp:221](tests/unit/scheduler_test.cpp#L221)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T070.F1** 未确认替换时保持旧 generation；确认后先请求停止旧任务并立即拒绝旧代新发送。
- [ ] **T070.F2** 错误停止确认忽略；匹配确认后启动新槽/新 generation；旧 TX 完成不能解除新任务 outstanding。

<a id="t071"></a>

#### T071 任务停止与断开的代际失效

原名：`stop and disconnect invalidate generation until owner confirms`

源码：[tests/unit/scheduler_test.cpp:257](tests/unit/scheduler_test.cpp#L257)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T071.F1** request_stop 后即使经过 deadline 或旧 TX 完成，也保持 Stopping，直到 owner 匹配确认才 Idle。
- [ ] **T071.F2** 停止后不再发送；重启 generation 增加，断开使其立即失效。

<a id="t072"></a>

#### T072 调度器计数器与时间溢出

原名：`scheduler exhaustion fails activation or requests an explicit stop`

源码：[tests/unit/scheduler_test.cpp:286](tests/unit/scheduler_test.cpp#L286)；标签：`[scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T072.F1** generation 达 uint64 上限、初始 deadline 溢出时启动失败并保持 Idle。
- [ ] **T072.F2** 末次 deadline、跨完整时钟范围、发送 sequence 溢出三组运行中场景转 Stopping；只发布一次对应 automatic stop，旧代失效，确认后 Idle。

### 串口 owner 与 scanner

<a id="t073"></a>

#### T073 命令队列满时取消连接或停止

原名：`open cancellation and stop settle accepted work under saturation`

源码：[tests/unit/serial_service_test.cpp:268](tests/unit/serial_service_test.cpp#L268)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T073.F1** 普通命令额度 1，阻塞 open 后第二个 connect 被拒；取消和停止仍能结束已接受连接操作。
- [ ] **T073.S1** 普通队列满时，取消仍有独立控制完成路径；最终 Connect Cancelled，另有 DisconnectCompletion。（SECTION：`cancel has a reserved control completion`；[源码](tests/unit/serial_service_test.cpp#L282)）
- [ ] **T073.S2** stop 后拒绝新 connect/cancel；解除 open 阻塞后 Connect Cancelled，worker 限时停止。（SECTION：`stop closes all connect producers`；[源码](tests/unit/serial_service_test.cpp#L287)）

<a id="t074"></a>

#### T074 设备扫描积压上限与停止

原名：`scanner bounds pending work and settles active work during stop`

源码：[tests/unit/serial_service_test.cpp:306](tests/unit/serial_service_test.cpp#L306)；标签：`[serial][scanner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T074.F1** 阻塞枚举后检查扫描请求容量与 stop；完成通知携带原 operation ID/generation，最终发布 worker stopped 信号。
- [ ] **T074.S1** 一个 active 扫描加一个 pending 可保留，第三请求拒绝；两个 completion 的代际正确，设备路径为 /dev/fake。（SECTION：`one pending request is retained and the third is rejected`；[源码](tests/unit/serial_service_test.cpp#L327)）
- [ ] **T074.S2** 枚举尚未返回就 stop，立即拒绝新扫描；已有请求返回 Cancelled。（SECTION：`stop rejects new work before enumerate returns`；[源码](tests/unit/serial_service_test.cpp#L332)）

<a id="t075"></a>

#### T075 串口部分写不交错、手动优先

原名：`partial TX never interleaves and manual TX wins the next boundary`

源码：[tests/unit/serial_service_test.cpp:359](tests/unit/serial_service_test.cpp#L359)；标签：`[serial][owner][scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T075.F1** 模拟写入序列 2/0/1/64/1/64/1/64；手动 Abcd 阻塞时提交任务 Qq 和手动 Bb。
- [ ] **T075.F2** 最终字节为 AbcdBbQq，三个 TX 和任务启动各完成一次，TX operation 顺序及结果正确，无 fatal。

<a id="t076"></a>

#### T076 TX 满载时断开/停止打断部分写

原名：`disconnect and stop interrupt partial TX despite saturated admission`

源码：[tests/unit/serial_service_test.cpp:389](tests/unit/serial_service_test.cpp#L389)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T076.F1** TX 容量 1，ABC 的 backend 调用被阻塞，新 D 拒绝；取消后只保留 A 前缀，completion=Cancelled。
- [ ] **T076.F2** 事件按 TX → ERR → Cleanup 发布，Cleanup origin 正确；断开完成或 worker 停止都限时结束。
- [ ] **T076.S1** backend 写调用中申请 disconnect；得到 TX Cancelled 和 DisconnectCompletion。（SECTION：`disconnect during the backend call`；[源码](tests/unit/serial_service_test.cpp#L402)）
- [ ] **T076.S2** backend 写调用中申请 owner stop；得到 TX Cancelled 并限时退出。（SECTION：`owner stop during the backend call`；[源码](tests/unit/serial_service_test.cpp#L406)）

<a id="t077"></a>

#### T077 RX 满载时可靠 TX 事件保留

原名：`successful TX events survive saturated RX ingress`

源码：[tests/unit/serial_service_test.cpp:426](tests/unit/serial_service_test.cpp#L426)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T077.F1** RX 只能保留一个 chunk，先积压 R，再成功发送 A/B；连接保持，事件按 RX R → TX A → TX B 保留 operation ID 和内容，无 overflow/fatal。

<a id="t078"></a>

#### T078 终态数据保留期间的容量预留

原名：`retained terminal data keeps its admission and releases it on drain`

源码：[tests/unit/serial_service_test.cpp:458](tests/unit/serial_service_test.cpp#L458)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T078.F1** 消费 completion 不等于消费数据；TX 和跨会话 Cleanup 的预留需等对应数据 drain 后释放，RX 与 TX 字节额度独立。
- [ ] **T078.S1** TX completion 已消费但数据未 drain 时，连续新发送仍拒绝；drain TX 数据后恢复准入。（SECTION：`TX message reservation survives completion consumption`；[源码](tests/unit/serial_service_test.cpp#L463)）
- [ ] **T078.S2** 连续三会话的 Cleanup 不 drain，第四次 connect 因 cleanup capacity full 拒绝；清掉三个 Cleanup 后连接成功。（SECTION：`cleanup reservations survive across sessions`；[源码](tests/unit/serial_service_test.cpp#L481)）
- [ ] **T078.S3** TX 额度 1 MiB 下保留 700 KiB 发送，第二份拒绝；即使超过 RX 的 4 KiB 配额，仍可独立接收 R，drain 后恢复 TX。（SECTION：`TX byte reservations cannot consume the independent RX quota`；[源码](tests/unit/serial_service_test.cpp#L498)）

<a id="t079"></a>

#### T079 TX 超时、已发前缀和下一请求期限

原名：`TX deadlines stop partial writes and restart only for the next request`

源码：[tests/unit/serial_service_test.cpp:528](tests/unit/serial_service_test.cpp#L528)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T079.F1** 两种超时都报告 TimedOut、已接受 1 字节及 SerialOperationTimedOut；TX A 在 ERR 前，错误 operation ID 正确。
- [ ] **T079.S1** 第一请求写 1 字节后零进展，100 ms 超时；排队的下一请求获得自己的新期限，之后恢复 backend 仍能成功。（SECTION：`zero progress times out and the queued request gets a fresh deadline`；[源码](tests/unit/serial_service_test.cpp#L536)）
- [ ] **T079.S2** backend 写调用阻塞 150 ms，返回正数部分写时已超过 100 ms deadline，立即超时并保留 1 字节。（SECTION：`a positive partial write returns after its deadline`；[源码](tests/unit/serial_service_test.cpp#L543)）

<a id="t080"></a>

#### T080 零字节写入的重试退避

原名：`zero-byte TX writes use bounded retry backoff`

源码：[tests/unit/serial_service_test.cpp:572](tests/unit/serial_service_test.cpp#L572)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T080.F1** backend 持续返回 0，50 ms 内 owner wait 增量限定为 3–20，避免不重试或忙循环；TX 总超时配置 500 ms。

<a id="t081"></a>

#### T081 owner 停止的完成通知和生命周期槽

原名：`owner stop settles producers and publishes its fixed lifecycle slot`

源码：[tests/unit/serial_service_test.cpp:587](tests/unit/serial_service_test.cpp#L587)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T081.F1** 无工作和有已接受工作两组，最终在限时内发布 Serial/AtReturnPoint 固定停止信号，无 fatal。
- [ ] **T081.S1** 空闲 owner 在 ppoll 等待中也能被 stop 唤醒。（SECTION：`idle ppoll is woken`；[源码](tests/unit/serial_service_test.cpp#L590)）
- [ ] **T081.S2** 三个已接受 TX 和一个任务停止控制请求全部得到完成；stop 后新 TX/disconnect/task stop/cancel 都拒绝。（SECTION：`accepted TX and task control all complete`；[源码](tests/unit/serial_service_test.cpp#L591)）

<a id="t082"></a>

#### T082 停止定时任务的写入屏障和配额释放

原名：`task stop closes admission and confirms only after partial TX settles`

源码：[tests/unit/serial_service_test.cpp:631](tests/unit/serial_service_test.cpp#L631)；标签：`[serial][owner][scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T082.F1** 周期 ABC 只接受 A 后停止；TX Cancelled 必须先于匹配 TaskStopCompletion，之后 30 ms 不再写入后缀。
- [ ] **T082.F2** 只 drain TX 前缀仍占用 TX 配额，连 ERR 也 drain 后才可重新发送；核对 owner_order、保留字节数和队列归零。
- [ ] **T082.S1** stop 到达时 backend 写调用尚未返回；此时新的任务替换拒绝，放行后先结束部分 TX 再确认停止。（SECTION：`stop arrives before the backend call returns`；[源码](tests/unit/serial_service_test.cpp#L640)）
- [ ] **T082.S2** backend 已写入 A 后才 stop，仍只保留前缀并按同样顺序确认停止。（SECTION：`stop arrives after the backend accepted a prefix`；[源码](tests/unit/serial_service_test.cpp#L644)）

<a id="t083"></a>

#### T083 owner 任务取消与精确替换确认

原名：`owner task commands settle cancellation and reject stale replacement`

源码：[tests/unit/serial_service_test.cpp:690](tests/unit/serial_service_test.cpp#L690)；标签：`[serial][owner][scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T083.F1** 手动 M 阻塞期间提交周期 Q，分别验证未开始取消、整体停止和替换竞争，无 fatal。
- [ ] **T083.S1** 按 pending start operation ID 停止尚未启动的 Q；start=Cancelled、stop=Succeeded，只写 M，任务 Idle。（SECTION：`a pending task can be stopped before it starts`；[源码](tests/unit/serial_service_test.cpp#L699)）
- [ ] **T083.S2** 整体 stop 将已接受但未开始的任务启动完成为 Cancelled，只保留 M。（SECTION：`shutdown settles the accepted start without sending it`；[源码](tests/unit/serial_service_test.cpp#L712)）
- [ ] **T083.S3** Q 被确认替换为 R 后，再拿 Q 的旧 generation 确认替换为 S，应失败并要求重新确认；当前代不变，实际 MQR。（SECTION：`replacement is tied to the exact task that was confirmed`；[源码](tests/unit/serial_service_test.cpp#L722)）

<a id="t084"></a>

#### T084 owner 独立推进任务边界及准入

原名：`owner advances task boundaries without a main thread wake`

源码：[tests/unit/serial_service_test.cpp:748](tests/unit/serial_service_test.cpp#L748)；标签：`[serial][owner][scheduler]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T084.F1** 无需主线程唤醒也能在手动超时后执行任务、替换旧任务；额外验证执行快照容量上限。
- [ ] **T084.S1** 手动 M 超时后恢复 backend 写入能力，但不主动唤醒 owner；初始单次 Q 仍能自行完成。（SECTION：`manual timeout releases an initial one-shot trigger`；[源码](tests/unit/serial_service_test.cpp#L754)）
- [ ] **T084.S2** OLD 部分写 O 时确认替换为 NEW；旧 TX Cancelled 先完成，再启动/发送 NEW，实际 ONEW，事件顺序为 O/ERR/NEW。（SECTION：`confirmed replacement stops the unwritten old-task suffix`；[源码](tests/unit/serial_service_test.cpp#L774)）
- [ ] **T084.S3** 三组准入拒绝：payload capacity 超 1 MiB、name capacity=1024、note 长度=257；不写数据、不发布完成、任务仍 Idle。（SECTION：`task admission bounds retained payload and metadata capacity`；[源码](tests/unit/serial_service_test.cpp#L797)）

<a id="t085"></a>

#### T085 过期 session 拒绝与设备丢失清理

原名：`late sessions are rejected and device loss performs cleanup`

源码：[tests/unit/serial_service_test.cpp:826](tests/unit/serial_service_test.cpp#L826)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T085.F1** 当前 generation 下错误 session ID 的发送被拒绝；模拟设备消失后 connected/disconnecting 都为 false，并产生带错误的 Cleanup。

<a id="t086"></a>

#### T086 故障关闭过程中提前关闭 TX 准入

原名：`fault transition closes TX admission before backend close`

源码：[tests/unit/serial_service_test.cpp:844](tests/unit/serial_service_test.cpp#L844)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T086.F1** 写入失败后让 backend.close 阻塞，此时新发送已返回 stale or disconnected；放行后原 TX Failed，事件为 ERR → Cleanup，无 fatal。

<a id="t087"></a>

#### T087 设备丢失保留部分 TX 与错误顺序

原名：`device loss after a partial write reports the accepted prefix`

源码：[tests/unit/serial_service_test.cpp:870](tests/unit/serial_service_test.cpp#L870)；标签：`[serial][owner]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T087.F1** ABCD 先写 2 字节再失败；completion=Failed、accepted_bytes=2；事件为 TX AB → SerialDeviceGone ERR（原 operation ID）→ Cleanup，连接完成清理。

### 致命信号并发

<a id="t088"></a>

#### T088 致命信号并发发布只有一个胜者

原名：`concurrent and later fatal publishers preserve exactly one winner`

源码：[tests/unit/signals_completion_test.cpp:37](tests/unit/signals_completion_test.cpp#L37)；标签：`[signals]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T088.F1** 16 个线程同时发布，恰好一次成功，保留来源 line<16，additional_count=15。
- [ ] **T088.F2** 随后发布不同 OOM 信号不能覆盖首个 Serial/Invariant 错误，只将 additional_count 加到 16。

### 连接状态机

<a id="t089"></a>

#### T089 连接状态机的身份与过期事件过滤

原名：`connection lifecycle rejects stale identities through disconnect and reconnect`

源码：[tests/unit/state_model_test.cpp:11](tests/unit/state_model_test.cpp#L11)；标签：`[state]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T089.F1** 拒绝未连接时的断开、重复 connect、错误 operation/generation/session；正确连接接受匹配发送。
- [ ] **T089.F2** Disconnecting 仍保留当前 session 并接受同会话 RX/Cleanup；错误清理完成不得释放 session，匹配完成后才 Disconnected。
- [ ] **T089.F3** 重连必须采用新 generation，旧代或旧 session 的事件拒绝，新会话事件接受。

<a id="t090"></a>

#### T090 连接失败或取消后的匹配清理

原名：`connect failures and cancellation converge through matching cleanup`

源码：[tests/unit/state_model_test.cpp:76](tests/unit/state_model_test.cpp#L76)；标签：`[state]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T090.F1** 三种状态机路径：连接失败、取消与成功竞争、取消后失败；均按对应操作/代际/session 完成清理。
- [ ] **T090.S1** 连接失败先进入 Error，清理完成前拒绝重连；经 Disconnecting 和匹配 completion 后结束。（SECTION：`connection errors must clean up through disconnecting`；[源码](tests/unit/state_model_test.cpp#L81)）
- [ ] **T090.S2** 取消后迟到连接成功时保持 Disconnecting 和真实 session；拒绝缺失/旧 cleanup 身份，匹配新 cleanup 才释放会话。（SECTION：`cancelled connect retains a racing successful session until cleanup`；[源码](tests/unit/state_model_test.cpp#L93)）
- [ ] **T090.S3** 取消后连接失败仍等待对应 cleanup；错误 generation 拒绝，匹配清理接受。（SECTION：`cancelled connect failure still waits for matching cleanup`；[源码](tests/unit/state_model_test.cpp#L124)）

### 接收视图、按键与文本输入

<a id="t091"></a>

#### T091 接收视图稳定坐标、过滤与淘汰

原名：`receive coordinates retain stable IDs through filtering and eviction`

源码：[tests/unit/ui_routing_test.cpp:19](tests/unit/ui_routing_test.cpp#L19)；标签：`[ui]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T091.F1** 构造跨会话 sequence 重置但 record ID 连续的 6 条 RX/TX；先过滤 RX，再计算两行 viewport、游标移动与搜索锚点。
- [ ] **T091.F2** 淘汰旧记录后游标归到仍存在的 ID，清理过期匹配；列表清空后 cursor/anchor 为空、匹配数归零。

<a id="t092"></a>

#### T092 快捷键路由、上下文优先级与连接 guard

原名：`key routing respects mode priority and connection guards`

源码：[tests/unit/ui_routing_test.cpp:72](tests/unit/ui_routing_test.cpp#L72)；标签：`[ui]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T092.F1** 用源码 routes 表逐行检查模式、按键、连接、设备弹窗状态及搜索过滤项焦点的映射；具体全部组合见后附路由表。

下表展开该用例的全部 41 组输入。后三个布尔值依次为 connected、device_modal、search_filter_focused；“是/否”对应 true/false。每行可单独填写审核意见。

| 子项 | 上下文 | 按键 | 已连接 | 设备弹窗 | 搜索过滤项获焦 | 预期动作 | 审核 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T092.R01 | Modal | `F1` | 否 | 否 | 否 | Help | ____ |
| T092.R02 | Modal | `Q` | 否 | 否 | 否 | None | ____ |
| T092.R03 | Modal | `q` | 否 | 否 | 否 | None | ____ |
| T092.R04 | Search | `C` | 是 | 否 | 否 | None | ____ |
| T092.R05 | Search | `q` | 是 | 否 | 否 | None | ____ |
| T092.R06 | SendEdit | `g` | 是 | 否 | 否 | None | ____ |
| T092.R07 | SendEdit | `q` | 是 | 否 | 否 | None | ____ |
| T092.R08 | Confirm | `q` | 是 | 否 | 否 | None | ____ |
| T092.R09 | Help | `q` | 是 | 否 | 否 | None | ____ |
| T092.R10 | ErrorDialog | `q` | 是 | 否 | 否 | None | ____ |
| T092.R11 | ErrorDialog | `Enter` | 是 | 否 | 否 | Escape | ____ |
| T092.R12 | Normal | `C` | 否 | 否 | 否 | Connection | ____ |
| T092.R13 | Normal | `g` | 否 | 否 | 否 | ToggleLog | ____ |
| T092.R14 | Normal | `G` | 否 | 否 | 否 | LoggingConfig | ____ |
| T092.R15 | Normal | `f` | 否 | 否 | 否 | None | ____ |
| T092.R16 | Normal | `f` | 是 | 否 | 否 | QuickExecute | ____ |
| T092.R17 | Normal | `S` | 是 | 否 | 否 | None | ____ |
| T092.R18 | Normal | `q` | 是 | 否 | 否 | Quit | ____ |
| T092.R19 | Normal | `Q` | 是 | 否 | 否 | None | ____ |
| T092.R20 | Normal | `y` | 是 | 否 | 否 | CopySelection | ____ |
| T092.R21 | SendEdit | `Enter` | 是 | 否 | 否 | Submit | ____ |
| T092.R22 | SendEdit | `Alt+Enter` | 是 | 否 | 否 | InsertNewline | ____ |
| T092.R23 | ReceiveBrowse | `PgUp` | 是 | 否 | 否 | PageUp | ____ |
| T092.R24 | ReceiveBrowse | `i` | 是 | 否 | 否 | Edit | ____ |
| T092.R25 | ReceiveBrowse | `q` | 是 | 否 | 否 | Quit | ____ |
| T092.R26 | ReceiveBrowse | `C` | 是 | 否 | 否 | None | ____ |
| T092.R27 | ReceiveBrowse | `Q` | 是 | 否 | 否 | None | ____ |
| T092.R28 | ReceiveBrowse | `y` | 是 | 否 | 否 | None | ____ |
| T092.R29 | Search | `F3` | 是 | 否 | 否 | SearchNext | ____ |
| T092.R30 | Search | `End` | 是 | 否 | 否 | End | ____ |
| T092.R31 | Modal | `F5` | 否 | 是 | 否 | Scan | ____ |
| T092.R32 | Modal | `F5` | 否 | 否 | 否 | None | ____ |
| T092.R33 | Modal | `Tab` | 否 | 否 | 否 | FocusNext | ____ |
| T092.R34 | Modal | `Shift+Tab` | 否 | 否 | 否 | FocusPrevious | ____ |
| T092.R35 | Search | `Tab` | 是 | 否 | 否 | FocusNext | ____ |
| T092.R36 | Search | `Right` | 是 | 否 | 否 | None | ____ |
| T092.R37 | Search | `Right` | 是 | 否 | 是 | SelectNext | ____ |
| T092.R38 | Search | `Space` | 是 | 否 | 是 | SelectNext | ____ |
| T092.R39 | Confirm | `Right` | 是 | 否 | 否 | SelectNext | ____ |
| T092.R40 | Confirm | `Left` | 是 | 否 | 否 | SelectPrevious | ____ |
| T092.R41 | Confirm | `Enter` | 是 | 否 | 否 | Apply | ____ |

<a id="t093"></a>

#### T093 接收区 Vim 命令解析

原名：`receive Vim commands parse bounded contextual sequences`

源码：[tests/unit/ui_routing_test.cpp:137](tests/unit/ui_routing_test.cpp#L137)；标签：`[ui]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T093.F1** 检查 j/k、10j、11k、gg/G、yy 的动作或 count，以及 g/y 的 Pending 状态。
- [ ] **T093.F2** 10yy 和 0j 拒绝；1000001j 仍为向下移动，但 count 截为 1,000,000。

<a id="t094"></a>

#### T094 UTF-8 编辑与容量边界

原名：`bounded text editing moves and deletes on UTF-8 boundaries`

源码：[tests/unit/ui_routing_test.cpp:155](tests/unit/ui_routing_test.cpp#L155)；标签：`[ui]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T094.F1** 以 A+中文+emoji 验证左移、Backspace、Delete 都按完整字符边界操作。
- [ ] **T094.F2** 插入中文恰好达到 4 字节上限成功，再插 ASCII 超限拒绝；截断 UTF-8 插入拒绝。

<a id="t095"></a>

#### T095 括号粘贴的整批拒绝

原名：`bracketed paste rejects unsafe input atomically`

源码：[tests/unit/ui_routing_test.cpp:175](tests/unit/ui_routing_test.cpp#L175)；标签：`[ui]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T095.F1** 非文本上下文、超容量、非法 UTF-8 三类输入都消费到结束标记后拒绝；超容量分支另检查返回文本为空。
- [ ] **T095.S1** Normal 非文本上下文把 i/q 当粘贴内容消费至结束，不触发快捷键，最后拒绝。（SECTION：`non-text contexts consume everything through the end marker`；[源码](tests/unit/ui_routing_test.cpp#L177)）
- [ ] **T095.S2** 2 字节容量下 A+中文+q 整批拒绝并返回空文本；Search 中截断中文 UTF-8 在结束时拒绝。（SECTION：`invalid UTF-8 and overflow reject atomically`；[源码](tests/unit/ui_routing_test.cpp#L187)）

<a id="t096"></a>

#### T096 FTXUI 粘贴事件所有权与上下文变化

原名：`bracketed paste adapts owned FTXUI events and detects context changes`

源码：[tests/unit/ui_routing_test.cpp:207](tests/unit/ui_routing_test.cpp#L207)；标签：`[ui]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T096.F1** 逐事件输入中文、Return、NEXT 和结束标记；上下文 ID 不变时完成并得到 中文+LF+NEXT。
- [ ] **T096.F2** 中途上下文 ID 从 4 变 5 时拒绝并返回空文本；两组结束后 active=false。

### 真实日志文件与配额

<a id="t097"></a>

#### T097 真实日志目录权限、私有文件及恢复

原名：`Linux logs use safe directory permissions and recoverable private files`

源码：[tests/integration/logging_files_test.cpp:41](tests/integration/logging_files_test.cpp#L41)；标签：`[integration][logging][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T097.F1** 三组目录：缺失则自动创建 0700；已有 0750 接受且保持权限；已有 0720 拒绝。
- [ ] **T097.F2** 在 umask=0777 下启动，合法组写入 00 FF 7E、barrier 后结束；文件为 0600，重新解析仅有一条且 payload 精确。

<a id="t098"></a>

#### T098 真实日志目录的独占锁

原名：`Linux session backend enforces its process directory lock`

源码：[tests/integration/logging_files_test.cpp:85](tests/integration/logging_files_test.cpp#L85)；标签：`[integration][logging][file]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T098.F1** 同一目录启动两个 SessionWriter：第一个 Recording，第二个 Error；测试是在同一进程创建两个 writer。

<a id="t099"></a>

#### T099 真实日志轮换及损坏/空文件保护

原名：`Linux log quotas rotate valid logs and preserve damaged or empty files`

源码：[tests/integration/logging_files_test.cpp:99](tests/integration/logging_files_test.cpp#L99)；标签：`[integration][logging][quota]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T099.F1** rotation 组：限制 2 文件、每文件一条记录，写入三条后仅留两文件且包含最新 seq=3，文件均可解析。
- [ ] **T099.F2** damaged bytes 组：损坏文件占满总字节预算时启动失败，4,096 字节原文件保留。
- [ ] **T099.F3** damaged and empty count 组：损坏文件与空文件耗尽数量额度时启动失败，两文件大小不变。

<a id="t100"></a>

#### T100 外部修改使日志配额缓存失效

原名：`quota cache rescans after an external directory change`

源码：[tests/integration/logging_files_test.cpp:170](tests/integration/logging_files_test.cpp#L170)；标签：`[integration][logging][quota]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T100.F1** writer 启动后外部增加 4,096 字节损坏日志；下一条写入发现实际额度不足，barrier=WriterFailed，外部文件保留。

### Linux/libserialport backend

<a id="t101"></a>

#### T101 libserialport fd 与 Linux 等待机制

原名：`libserialport native fd works with ppoll and eventfd`

源码：[tests/integration/serial_backend_viability_test.cpp:111](tests/integration/serial_backend_viability_test.cpp#L111)；标签：`[integration][serial][linux]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T101.F1** 真实 PTY 用 libserialport 配为 115200/8N1/无流控，取得 native fd 并验证字符设备；分别验证空闲唤醒和 TX 满载时唤醒/恢复。
- [ ] **T101.S1** 空闲 POLLIN 等待 30 ms 超时；20 ms 后写 eventfd 可在 1 秒内唤醒，串口 fd 本身没有伪就绪。（SECTION：`idle wait blocks and eventfd wakes it`；[源码](tests/integration/serial_backend_viability_test.cpp#L137)）
- [ ] **T101.S2** 每次写 64 KiB、最多 512 次填满 PTY，确实出现部分写和 0 字节（libserialport 对 EAGAIN 的返回）；满载时仍观察到 eventfd，读空后恢复 POLLOUT。（SECTION：`blocked TX returns partial writes and EAGAIN without hiding wakeups`；[源码](tests/integration/serial_backend_viability_test.cpp#L155)）

<a id="t102"></a>

#### T102 串口有效访问权限探测

原名：`serial permission probe reports effective access`

源码：[tests/integration/serial_backend_viability_test.cpp:229](tests/integration/serial_backend_viability_test.cpp#L229)；标签：`[integration][serial][linux][permission]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T102.F1** PTY 初始报告 Allowed；仅在非 root 用户下，chmod 0000 后报告 PermissionDenied，再恢复权限。root 执行时不检查拒绝分支。

<a id="t103"></a>

#### T103 串口被占用时无 fd 泄漏

原名：`busy libserialport open does not leak its temporary fd`

源码：[tests/integration/serial_backend_viability_test.cpp:247](tests/integration/serial_backend_viability_test.cpp#L247)；标签：`[integration][serial][linux]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T103.F1** 用 flock 独占 PTY 后连续 16 次 sp_open；每次失败为 EAGAIN/EWOULDBLOCK，native handle 为 -1。
- [ ] **T103.F2** 通过 /proc/self/fd 核对尝试前后 fd 数相同。

### 真实 PTY owner 集成

<a id="t104"></a>

#### T104 真实 PTY 的 owner、周期发送和连接生命周期

原名：`real PTY owner exchanges bytes and settles connection lifecycle`

源码：[tests/integration/serial_owner_pty_test.cpp:55](tests/integration/serial_owner_pty_test.cpp#L55)；标签：`[integration][serial][pty]`；快速集：是。

审核：____；合并目标 / 理由：____。

- [ ] **T104.F1** 真实 LibserialportBackend + SerialService + PTY 验证取消竞争、无 UI tick 的周期任务、双向二进制收发及清理；后两条清理分支嵌套在收发分支内。
- [ ] **T104.S1** 立即取消连接的竞争：若取消被接受，收到 Cancelled 和清理完成；若连接已抢先完成，则正常 disconnect 并收完成。（SECTION：`a queued connection can be cancelled or has already connected`；[源码](tests/integration/serial_owner_pty_test.cpp#L58)）
- [ ] **T104.S2** 10 ms 周期无需 UI tick 即发送至少三次 Q；operation ID 不重复，停止确认后再等 40 ms 无新增字节，任务 Idle。（SECTION：`periodic deadlines run without a UI tick and stop is a write barrier`；[源码](tests/integration/serial_owner_pty_test.cpp#L75)）
- [ ] **T104.S3** 空闲 50 ms wait_count 增量不超过 2；RX 精确传输 00 41 80 FF，TX 精确传输 FE 42 00 7F，TX completion 成功。（SECTION：`an active connection exchanges bytes without idle spinning`；[源码](tests/integration/serial_owner_pty_test.cpp#L132)）
- [ ] **T104.S4** 属于 T104.S3 的子分支：正常 disconnect 完成后连接状态清空，并有 Cleanup origin 的事件。（SECTION：`normal disconnect`；[源码](tests/integration/serial_owner_pty_test.cpp#L171)）
- [ ] **T104.S5** 属于 T104.S3 的子分支：关闭 PTY master 模拟 HUP，连接状态清空，Cleanup 必须带错误。（SECTION：`peer hangup`；[源码](tests/integration/serial_owner_pty_test.cpp#L177)）

### 离线依赖构建

<a id="t105"></a>

#### T105 bundled libserialport 离线构建回归

原名：`bundled_libserialport_build`

源码：[tests/integration/libserialport_build_test.cmake:1](tests/integration/libserialport_build_test.cmake#L1)；标签：`[integration][build]`；快速集：否（完整集执行）。

审核：____；合并目标 / 理由：____。

- [ ] **T105.F1** 复制 vendored libserialport，把 Autotools 输入时间改新，并令 ACLOCAL/AUTOMAKE/AUTOCONF/AUTOHEADER 全部不可用。
- [ ] **T105.F2** 分别用 Ninja 和 Unix Makefiles 配置、构建及安装依赖；必须生成 libserialport.so。
- [ ] **T105.F3** aclocal.m4、configure、Makefile.in、config.h.in 的 SHA-256 必须不变；带 build 标签，快速集排除。

## 编译期检查：不单独出现在 CTest 中

以下 12 条是测试源码中的 `static_assert`。失败会阻止测试程序编译，六套构建会重复验证同一规则。

| 编号 | 功能 / 预期 | 源码 | 审核 |
| --- | --- | --- | --- |
| C001 | 间隔 0 ms 合法，表示单次任务。 | [scheduler_test.cpp:22](tests/unit/scheduler_test.cpp#L22) | ____ |
| C002 | 间隔 1 ms 非法。 | [scheduler_test.cpp:23](tests/unit/scheduler_test.cpp#L23) | ____ |
| C003 | 间隔 9 ms 非法。 | [scheduler_test.cpp:24](tests/unit/scheduler_test.cpp#L24) | ____ |
| C004 | 间隔 10 ms 合法。 | [scheduler_test.cpp:25](tests/unit/scheduler_test.cpp#L25) | ____ |
| C005 | 间隔 86,400,000 ms 合法。 | [scheduler_test.cpp:26](tests/unit/scheduler_test.cpp#L26) | ____ |
| C006 | 间隔 86,400,001 ms 非法。 | [scheduler_test.cpp:27](tests/unit/scheduler_test.cpp#L27) | ____ |
| C007 | generation 从 41 正常增长到 42；最大 OperationId 增长返回 Overflow 且原值不变；已耗尽 IdSequence 发号返回 Overflow，输出值仍为原 9。 | [signals_completion_test.cpp:19](tests/unit/signals_completion_test.cpp#L19) | ____ |
| C008 | FatalSignal 可平凡复制。 | [signals_completion_test.cpp:31](tests/unit/signals_completion_test.cpp#L31) | ____ |
| C009 | WorkerStoppedSignal 可平凡复制。 | [signals_completion_test.cpp:32](tests/unit/signals_completion_test.cpp#L32) | ____ |
| C010 | FatalSignal 大小不超过 64 字节。 | [signals_completion_test.cpp:33](tests/unit/signals_completion_test.cpp#L33) | ____ |
| C011 | diagnostics::emergency_write 为 noexcept。 | [signals_completion_test.cpp:34](tests/unit/signals_completion_test.cpp#L34) | ____ |
| C012 | diagnostics::install_terminate_handler 为 noexcept。 | [signals_completion_test.cpp:35](tests/unit/signals_completion_test.cpp#L35) | ____ |

## 构建矩阵：重复运行同一套功能测试

配置、编译器和 sanitizer 验证的是同一组功能在不同构建条件下的表现，不增加独立功能项。这里列出入口，便于单独审核执行频率。

| 编号 | Preset | 验证目的 | 审核 / 执行频率 |
| --- | --- | --- | --- |
| M001 | `gcc-debug` | GCC Debug、严格警告、完整测试 | ____ |
| M002 | `gcc-debug-fast` | 复用 GCC Debug 产物，排除两个 extended 和一个 build 项 | ____ |
| M003 | `clang-debug` | Clang 编译兼容性和完整测试 | ____ |
| M004 | `gcc-release` | 优化构建下的完整测试 | ____ |
| M005 | `gcc-debug-no-diagnostics` | diagnostics 编译关闭时的完整测试；T048 使用相反的编译分支 | ____ |
| M006 | `gcc-asan-ubsan` | 内存错误、泄漏及未定义行为检测；与 TSan 分开 | ____ |
| M007 | `gcc-tsan` | 数据竞争检测；要求可用 TSan 环境 | ____ |

## 公共测试辅助代码：不属于独立测试用例

以下五个头文件没有独立 TEST_CASE，但会随所用测试编译执行；删除其使用者时可一并判断是否还有保留价值。文件内的 fixture 初始化断言属于前置条件，不另计为功能测试。

| 编号 | 文件 | 功能 | 审核 |
| --- | --- | --- | --- |
| H001 | [temporary_directory.hpp](tests/support/temporary_directory.hpp) | 创建/清理私有临时目录；UmaskGuard 保存和恢复进程 umask | ____ |
| H002 | [fake_atomic_file_system.hpp](tests/support/fake_atomic_file_system.hpp) | 注入 Begin、Stage、Commit、DirectorySync、DirectorySyncException；返回受控文件身份 | ____ |
| H003 | [logging_builders.hpp](tests/support/logging_builders.hpp) | 构造字节、日志头、日志记录及带预算 token 的 SessionRecord | ____ |
| H004 | [serial.hpp](tests/support/serial.hpp) | fd RAII、PTY 初始化、限时收集并精确核对完成通知数量 | ____ |
| H005 | [wait.hpp](tests/support/wait.hpp) | 基于 steady_clock 的普通条件等待，不调用 Application::tick | ____ |

ApplicationHarness、SerialFixture、LogFixture/PersistenceFixture 和 PtyOwner 仍位于各自测试源文件，负责模拟依赖、真实临时文件、事件收集和退出清理。SerialFixture 的 drain_events 还共用检查事件数、owner_order 严格递增、保留字节核算及 drain 后队列归零；这些断言随调用它的串口用例执行。

## 本清单的覆盖边界

- PTY 验证虚拟终端与 owner 集成，不能证明真实 USB-UART 驱动、拔插、高波特率吞吐和设备权限配置。T102 的拒绝权限分支在 root 下跳过。
- UI 六项自动测试主要检查视图模型、路由、命令解析、编辑和粘贴；没有在这些用例中自动启动完整 TUI 验证鼠标、配色、透明背景或真实终端复制。
- 未将产品/工程计划中的真实设备矩阵、8 小时压力、硬件性能/RSS 验收当成已有自动测试。审查这些验收要求时应另看 [Plan.md](Plan.md) 和 [DevelopPlan.md](DevelopPlan.md)。

重新发现测试或按英文名选择用例：

```bash
ctest --preset gcc-debug --show-only=json-v1
ctest --preset gcc-debug-fast --show-only=json-v1
ctest --preset gcc-debug -R "<英文测试名正则>"
```
