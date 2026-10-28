# 内建策略与对话值

`ayni_strategies` 是 Core 外的 C++17 组件，仅使用公开头文件。规则与 ToolLoop 通过 `configure` / `describe` / `ayni_register_strategy` 装配。配置中的指针和字符串借用到 Kernel 销毁，配置后不得修改；对象在拥有者线程使用，不允许并发回调或移动。

规则按 GoalType ID/version 选择一串固定的 `RuleStep`。步骤名为 `name.vN`，名称加版本不能超过 32 字节；参数或语义变化必须换步骤版本。已完成步骤从共同调用账本读取，不重发。已知失败结束任务，未知结果暂停。

ToolLoop 输入使用 `Conversation::begin(record, user, parameters)`，GoalType 参数验证器为 `Conversation::validate_turn`。模型和摘要配置均须是 MODEL Binding，摘要 Binding 的结果格式为 TEXT，主模型为 TOOL_CALLS；GoalType 要声明允许的工具及模型/工具授权需求。主模型与摘要请求都以 `AYNI_MODEL_INSTRUCTIONS_PREFIX` 把配置的指令（主模型另加当前时间）与对话数据分开，支持消息角色的 Provider 把指令作为 system 消息发送（ADR-0074）。它按稳定轮次/序号执行每批最多 8 个提议，先检查完整列表与步骤余量，每个提议仍经 Kernel 授权。默认 8 次模型调用（包括摘要），所有者提高上限后从原步骤继续。

容量：规则 8 条，每条 16 步；最近 3 轮，每条输入/回答 384 字节，摘要 512 字节，便携参数/检查点 4096 字节，提示词 8192 字节。调用和 Task 步骤容量由装配方另设，必须容纳工作；超限明确拒绝。组件不分配内存、不执行 I/O（唯一例外是 ToolLoop 可选的 `LocalClock` 非阻塞 UTC 端口，用来在每次主模型请求中写明当前本地时间与时区，见 ADR-0073；它只改变请求正文，意图、步骤键与检查点不依赖它）；每次回调至多扫描 64 个模型轮次，每轮 8 个工具，提示词追加与编解码都有硬上限。回调耗时只可按实际平台测量，不承诺硬件实时上限。

对话外层存储流程：

1. `begin` 生成明确快照，为一个新 Task 提交 Goal 参数。旧 record 保留。
2. Task 成功完成后，`ayni_copy_task_checkpoint` 复制其已持久的最终检查点；`BUSY` 时继续推进 Kernel。
3. `finish(current, checkpoint, candidate)` 核对 id/owner/generation/revision。用 `encode(candidate)` 生成便携字节，**先保存成功，再替换 current**。失败时旧轮次、旧摘要均保留。
4. 整段删除用 `erase(current, candidate)`，同样先保存成功再替换；新代际拒绝迟到结果。装配方取消在途 Task。单轮删除、Kernel 历史正文擦除属于 P3；这里不是全盘擦除 API。

P1 提供值与编码、Task 内持久候选；P2 接外层文件存储。`decode` 只接可信本地持久字节，`finish` 只接该 Task 的公开导出，均不是认证/权限边界。所有者认证和对话 id 分配由装配方负责。

检查点 reader 显式声明可接受的旧 writer ID/version，默认只接受自己当前版本。组件格式为版本 1：小端整数，文本为 u32 字节长度后接 UTF-8，拒绝尾字节。记录头 `AYNC + u32 version`；字段依次为 id、owner、generation、revision、last_round、summary_first、summary_last（u64），summary 文本，round_count（u32），每轮 number（u64）/user/answer。Task 格式 `AYNT + u32 version + u32 done + u64 base_revision` 后接同一记录字段及当前 input 文本。完整已完成格式的 revision 必须为 base+1，尚在处理时保持 base。旧实现不能读取的新版本直接暂停，不重置。

验证入口：构建 `ayni_strategies_tests`；本地 `ctest -R '^(StrategiesFixture|Conversation|BuiltinRules|BuiltinLoop|CheckpointReaders)'`。所有 Provider 回执为本地夹具，不调用实 API。架构决定见 [ADR-0056](../../docs/adr/0056-builtin-strategies-and-conversations.md)。
