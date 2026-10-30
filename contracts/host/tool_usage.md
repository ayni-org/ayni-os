# 能力用法自描述

- **关联**：ADR-0082、0085；AP-01/AP-03；C-CALL-014、C-PROV-015；ABI 0.50。
- **头文件**：`include/ayni/tool_usage.h`；纯 UTF-8 校验声明在 `ayni.h`。
- **当前实现**：OpenAI-compatible Provider、runtime 的 schedule 配对、assistant.schedule 软件能力；Mock 呈现设备通过本地 HTTP 夹具验证。配置期注册保持；摄像头热插拔、MCP/MHS/ROS adapter 仍为后续计划。

## 属主与边界

Core 拥有可调用身份、原机器 Schema、授权投影和当前可见集合。扩展提供 purpose、text_usage、result_usage、examples 及字段解释；说明不授予权限、不执行示例。runtime 在可信配置期将 exact Binding/Capability 版本及说明明确采纳到 catalog，后者复制有界值；运行期不回查说明来源，也不持久化说明。详见 [Task 目录契约](../domain/capability_directory.md)。

lookup 只用于配置期采纳：同步、有界、不得等待 I/O、重入 Kernel 或抛出异常。返回视图由采纳入口在返回前复制；失败不发布半份说明。异步发现器先准备候选，再由可信配置决定采纳。模型 Provider 不持有 lookup/context，不通过 worker 访问 Core。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-USAGE-001 | 采纳必须精确匹配 Binding ID/version 与 Capability ID/version，结构为当前 sizeof、reserved=0；缺项、失配或错误拒绝，无旧说明回退；未采纳内容不进入模型目录 | 用设备名代替契约配对 | `OpenaiProvider.missing_candidate_usage_is_not_adopted_or_sent_to_model`、`OpenaiUsage.invalid_description_is_rejected_during_trusted_adoption` |
| C-USAGE-002 | 每个工具所有身份/说明字符串总计至多4096 UTF-8字节，每字段解释至多256字节，均非空；字段数恰等权威视图且按 field_id 唯一匹配，不依赖数组顺序；超界拒绝，不截断 | 第9字段被忽略；超长说明被截成另一条文法；两个字段互换解释 | M1 已实现：`OpenaiProvider.exact_tool_and_field_description_budgets_are_copied_during_adoption`、`OpenaiProvider.individually_bounded_strings_cannot_exceed_the_total_usage_budget`、`OpenaiUsage.invalid_description_is_rejected_during_trusted_adoption`、`OpenaiProvider.text_completes_through_public_abi_and_declares_remote_effects` |
| C-USAGE-003 | catalog 在配置期读取并立即复制说明；模型请求只有稳定的四个 function 定义，worker 只消费已复制消息，不借用 Core 或说明源 | worker 回调已失效目录 | `ToolUsage.copied_usage_survives_source_reuse_and_encodes_the_original_description`、`OpenaiProvider.fixed_functions_need_no_catalog_usage_at_submit`；调用链审查 |
| C-USAGE-004 | 固定 list/inspect/call/shell 工具经普通 function calling 使用；详情按需读取完整原 Schema 与当前单一授权候选，参数以具名嵌套结构传入；提议只在后续普通 Call 中规范化并授权 | 说明或示例直接产生业务副作用 | `OpenaiProvider.offered_function_becomes_exact_typed_values`、`OpenaiProvider.described_mock_device_runs_through_the_normal_managed_call`、`NativeAssistant.model_schedule_tool_registers_a_goal_that_waits_for_owner_approval` |
| C-USAGE-005 | 无效说明在配置采纳前拒绝，不进入目录、不外发、不发布半份或旧说明；模型不需要 submit lookup 端口 | 缺说明仍把业务定义发送给模型 | `OpenaiProvider.missing_candidate_usage_is_not_adopted_or_sent_to_model`、`OpenaiUsage.invalid_description_is_rejected_during_trusted_adoption` |
| C-USAGE-006 | 说明按 Unicode scalar UTF-8 校验；通用 ayni_validate_utf8 与既有 payload 复用 foundation 同一规则，纯函数可在任意线程/回调调用，无分配、I/O 或 Kernel 访问 | Provider 和 Core 接受不同过长编码；把 NUL 当终止符截断 | M1 已实现：`ToolUsage.unicode_validation_is_public_pure_and_length_delimited`、`OpenaiUsage.invalid_description_is_rejected_during_trusted_adoption`；既有 `CallInputTest.text_rejects_invalid_utf8_and_accepts_unicode_scalar_boundaries` |

## 当前装配与扩展方式

ScheduleTool 拥有结构字段及跨字段规则、相对日期/时区用法、结构返回值和示例；DemoProfile 使用其既有 Binding 声明配对只读 port。总提示词保留统一助理、用户意图与批准规则。合法登记仍只是等待所有者批准的 Goal，不因模型读取说明而获得新权限。

新增本机摄像头等能力时，backend/adapter 先按现有配置期注册契约提供 Capability、Provider、Binding 和资源；扩展再提供上述说明，runtime 通过 directory.h 的可信采纳入口拷贝有界说明；实际数据采集仍须经过原授权/资源/异步调用。协议 adapter 将外部说明归一到这些公开类型；协议发现和热插拔的准入、版本、撤下与寿命仍按后续 AP 包交付，不能把本包 lookup 当发现协议。

每工具说明槽为 4096 B，每字段解释至多 256 B，机器 Schema 采用 AP-02 的独立树形容量；目录并不受单响应最多八个提议的限制。列表按当前上下文预算缩短完整条目，inspect 无法容纳完整 Schema 时明确容量拒绝。ABI 0.50/schema 6 直接升级所有消费者；历史 AP-01/AP-02/AP-03A 交付见 ADR-0082/0084/0085。Native 资源界限不代表 ESP32 验证。
