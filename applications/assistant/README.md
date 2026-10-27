# 统一助理应用

可信应用只用公开 ABI，拥有四类 GoalType、一项策略和模型可调用的计划工具；具体 Host/Provider 由 runtime 组装。Kernel 保留目标、权限、运行限制和持久事实的权威，见 ADR-0066/0072。

`prepare_goal` 把明确的 `Parameters` 编码成 format2，并构造公开 GoalCreate；调用方提供实例和日历规则、通过管理会话提交。提醒最多 384 UTF-8 字节，其他类型不接受正文；整理明确选择最多四条来源。`Encoded` 的存储须活到同步提交返回，Kernel 随后保留副本。普通 C++ 应用接口使用 STL，分配失败可能抛出 bad_alloc；Kernel 回调边界将其转成容量错误。无异常 C++ SDK 随下一项交付。

- `assistant.reminder`：日历一次/每日/每周，notify 成功才完成，失败/未知保留待处理。
- `assistant.device`：周期 Mock 健康检查，无修复或物理动作。
- `assistant.organize`：低优先级周期，模型摘要和本地查重，未确认且带来源。
- `assistant.observe`：持续 Task-owned Mock 标量订阅，变化与可信 UTC 五分钟抽查；默认八次模型调用后停下。

没有文本路由：运行时把全部自然语言交给唯一的 ToolLoop 助理（ADR-0072）。`stop_request` 是唯一的确定性文本判定：去掉首尾空白与句末 `! . ！ 。` 后整句为“停/停下/停止/stop”，运行时据此停止而不调用模型。`project` 返回并行活动事实及主标签；诊断调用查询单独保留内部 ID。

`parse_schedule`/`format_schedule` 是所有者命令与模型计划工具共用的一套文法：`remind once|daily|weekly YYYY-MM-DDTHH:MM <POSIX-TZ> <正文>`、`check [秒]`、`observe`；整理因要明确选择来源，只有所有者能发起。日期按 Core 的民用时间拆分回验，时区用日历解析器校验；周期下限与正文长度仍由 `prepare_goal` 统一把关。`format_schedule` 是解析的精确逆，用于向所有者展示模型登记了什么。

`ScheduleTool`（`assistant/schedule_tool.h`）是本地 Provider `assistant.schedule`：ACTION/TOOL、不声明幂等、QUERY_NONE、CANCEL_NONE，入参为一段文法文本。每次调用以 Core 的 operation_id 标记来源：先按 operation_id 查已有 Goal，找到即复用，否则经 runtime 提供的 `GoalPort` 提交一次 GoalCreate，故同一操作重试不重复登记。模型来源的 Goal 另需 `consent_capability`（无 Provider、不发长期授权），所以创建后停在 PROPOSED，直到所有者 GOAL_APPROVE。文法错误或容量不足以 COMPLETED 回执返回给模型说明原因，未登记任何东西；其余失败为 FAILED、effect NONE。回执投递在 owner 线程 step 外进行，BUSY 时保留重试。

参数 format2：四字节 version=2/kind/source_count/origin（0 所有者、1 模型），模型来源随后一个小端 u64 operation_id，再是 source_count 个小端 u64 ID，余下为提醒 UTF-8 正文。format1 不再接受，旧目录在启动前拒绝。观察 checkpoint 为八个小端 u64：version=1、watching/analyzing、source boot、generation、sequence、value 的位表示、可信 UTC 秒（0 未知）、round。来源 generation 变化重新分析；稳定调用键保留旧 round 的事实。没有真实摄像头或媒体上传能力。
