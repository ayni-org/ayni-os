# 能力用法自描述

- **关联**：ADR-0082；AP-01；C-CALL-014、C-PROV-015；ABI 0.47。
- **头文件**：`include/ayni/tool_usage.h`；纯 UTF-8 校验声明在 `ayni.h`。
- **当前实现**：OpenAI-compatible Provider、runtime 的 schedule 配对、assistant.schedule 软件能力；Mock 呈现设备通过本地 HTTP 夹具验证。配置期注册保持；摄像头热插拔、MCP/MHS/ROS adapter 仍为后续计划。

## 属主与边界

Core 提供允许列表与当前授权交集中的 `ayni_tool_v1`，拥有可调用身份、字段类型/单位/取值与正文上限。扩展提供 purpose、text_usage、result_usage、examples 及每字段解释；说明不授予权限、不执行示例、不改变类型或解析器。runtime 配对确切 Binding/Capability 及版本，管理说明来源的生命周期。模型 Provider 在 submit 合并并复制；Core 不建立第二份说明目录，不存储或持久化长文本。

lookup 是只读、同步、非重入且有界的 owner-thread 回调，不能等待网络/设备、调用 Kernel 或抛出异常。异步发现器须先准备可读描述，不能在 lookup 中发现设备。返回视图有效到下一次 lookup 或本次 submit 返回，消费者须在此前复制；port/context 活到模型 Provider quiesce 结束。创建失败、关闭和部分复制失败不接管调用借用，不发布半份说明。异步 worker 只读请求槽，不再访问 Core 或说明来源。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-USAGE-001 | 使用工具的真实模型请求必须有 exact Binding ID/version 与 Capability ID/version 的说明；所有说明结构只收当前 sizeof，reserved=0；缺项、失配或错误明确拒绝，无 ID-only/旧说明回退 | 用设备名作为唯一说明；新版本借旧参数文法 | M1 已实现：`OpenaiProvider.tools_without_a_usage_port_are_rejected_before_http`、`OpenaiProvider.missing_exact_usage_is_rejected_before_http`、`OpenaiUsage.invalid_description_is_rejected_before_http_without_truncation` |
| C-USAGE-002 | 每个工具所有身份/说明字符串总计至多4096 UTF-8字节，每字段解释至多256字节，均非空；字段数恰等权威视图且按 field_id 唯一匹配，不依赖数组顺序；超界拒绝，不截断 | 第9字段被忽略；超长说明被截成另一条文法；两个字段互换解释 | M1 已实现：`OpenaiProvider.exact_tool_and_field_description_budgets_reach_the_http_codec`、`OpenaiProvider.individually_bounded_strings_cannot_exceed_the_total_usage_budget`、`OpenaiUsage.invalid_description_is_rejected_before_http_without_truncation`、`OpenaiProvider.text_completes_through_public_abi_and_declares_remote_effects` |
| C-USAGE-003 | Provider 在 submit 内逐项读取并立即复制权威视图与用法；worker 只用固定请求槽。说明/源数组不在返回后借用，不把 callback 移到 worker | 第2次 lookup 覆盖第1工具；worker 回调已失效的 Core view | M1 已实现：`ToolUsage.copied_usage_survives_source_reuse_and_encodes_the_original_description`；`OpenaiProvider.text_completes_through_public_abi_and_declares_remote_effects` 的 owner-thread lookup 断言；调用链审查 |
| C-USAGE-004 | function 名仍为当前请求的 tool_N，参数仍为 field_ID/text；说明只进入工具 schema，类型、单位、范围、枚举、必填与 exact 回译仍来自权威视图；所有提议继续经过策略及 Core 授权/Call | 描述扩展可提议工具集合；示例直接触发电机 | M1 已实现：`OpenaiProvider.offered_function_becomes_exact_typed_values`、`OpenaiProvider.text_completes_through_public_abi_and_declares_remote_effects`、`NativeAssistant.model_schedule_tool_registers_a_goal_that_waits_for_owner_approval` |
| C-USAGE-005 | 无效说明在 HTTP 前以 REJECTED_NOT_RETAINED 返回，无借用、外发或模型副作用；Core 用既有失败结果表达，不伪造模型回复或继续旧说明 | 缺说明仍发送请求；保留 completion 后静默丢失 | M1 已实现：`OpenaiProvider.tools_without_a_usage_port_are_rejected_before_http`、`OpenaiProvider.missing_exact_usage_is_rejected_before_http`、`OpenaiUsage.invalid_description_is_rejected_before_http_without_truncation` |
| C-USAGE-006 | 说明按 Unicode scalar UTF-8 校验；通用 ayni_validate_utf8 与既有 payload 复用 foundation 同一规则，纯函数可在任意线程/回调调用，无分配、I/O 或 Kernel 访问 | Provider 和 Core 接受不同过长编码；把 NUL 当终止符截断 | M1 已实现：`ToolUsage.unicode_validation_is_public_pure_and_length_delimited`、`OpenaiUsage.invalid_description_is_rejected_before_http_without_truncation`；既有 `CallInputTest.text_rejects_invalid_utf8_and_accepts_unicode_scalar_boundaries` |

## 当前装配与扩展方式

ScheduleTool 拥有文法、相对日期/时区用法、返回值和示例；DemoProfile 使用其既有 Binding 声明配对只读 port。总提示词保留统一助理、用户意图与批准规则。合法登记仍只是等待所有者批准的 Goal，不因模型读取说明而获得新权限。

新增本机摄像头等能力时，backend/adapter 先按现有配置期注册契约提供 Capability、Provider、Binding 和资源；扩展再提供上述说明，runtime 将来源接入模型 port。长说明无需改 Core，实际数据采集仍须经过原授权/资源/异步调用。协议 adapter 将外部说明归一到这些公开类型；协议发现和热插拔的准入、版本、撤下与寿命仍按后续 AP 包交付，不能把本包 lookup 当发现协议。

每工具的4096 B说明槽与最多8工具/8字段都是明确上限；Native 模型 Provider 每个请求槽预留这些字节。它不是 ESP32 模型传输内存验证。无新存储布局，storage schema 保持3；ABI 0.47 要求所有消费者重编译。
