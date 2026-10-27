# 统一助理应用

可信应用只用公开 ABI，拥有四类 GoalType 和一项策略；具体 Host/Provider 由 runtime 组装。Kernel 保留目标、权限、运行限制和持久事实的权威，见 ADR-0066。

`prepare_goal` 把明确的 `Parameters` 编码成 format1，并构造公开 GoalCreate；调用方提供实例和日历规则、通过管理会话提交。提醒最多 384 UTF-8 字节，其他类型不接受正文；整理明确选择最多四条来源。`Encoded` 的存储须活到同步提交返回，Kernel 随后保留副本。普通 C++ 应用接口使用 STL，分配失败可能抛出 bad_alloc；Kernel 回调边界将其转成容量错误。无异常 C++ SDK 随下一项交付。

- `assistant.reminder`：日历一次/每日/每周，notify 成功才完成，失败/未知保留待处理。
- `assistant.device`：周期 Mock 健康检查，无修复或物理动作。
- `assistant.organize`：低优先级周期，模型摘要和本地查重，未确认且带来源。
- `assistant.observe`：持续 Task-owned Mock 标量订阅，变化与可信 UTC 五分钟抽查；默认八次模型调用后停下。

`route` 为点名优先的确定性规则。`merge_suggestions` 只合并提交给它的建议，保留冲突与来源；不会修改 Kernel 或合并显式用户创建。`project` 返回并行活动事实及主标签；诊断调用查询单独保留内部 ID。

参数 format1：四字节 version=1/kind/source_count/reserved=0，随后 source_count 个小端 u64 ID，余下为提醒 UTF-8 正文。观察 checkpoint 为八个小端 u64：version=1、watching/analyzing、source boot、generation、sequence、value 的位表示、可信 UTC 秒（0 未知）、round。来源 generation 变化重新分析；稳定调用键保留旧 round 的事实。没有真实摄像头或媒体上传能力。
