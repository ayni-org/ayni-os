# Ayni 契约

本目录是 Ayni 领域语义、Host/Provider 接口、线协议与持久化格式的规范来源（计划 §3.1、`MA-017`）。在 IDL 选定之前（ADR-0009），契约以 Markdown 条款加可编译的 C 头文件草案表达（ADR-0013）。

## 目录

| 目录 | 内容 | 版本域 |
|---|---|---|
| [`domain/`](domain/) | ID、时间、状态码、原语最小字段、生命周期、调用与绑定、提交与输出门控、授权与信任、资源与运行限制、执行许可与停止、五项产品语义、容量限额 | 领域语义（随能力/Scene/GoalType 版本） |
| [`host/`](host/) | Host 端口、Provider 接口、策略与 GoalType 接口 | ABI（`abi_major/minor`） |
| [`wire/`](wire/) | 消息信封、受限 JSON 子集 | 协议（`protocol_major/minor`） |
| [`storage/`](storage/) | 日志批次、快照与记录版本 | 存储（`storage_schema`） |
| [`fixtures/`](fixtures/) | E1–E4 实验的场景格式与用例清单 | 随契约版本 |

对应的 C 头文件：已实现部分在 [`include/ayni/ayni.h`](../include/ayni/ayni.h)，计划部分在 [`include/ayni/draft/`](../include/ayni/draft/)。草案头文件只做编译检查，没有符号；调用其中的函数会在链接时失败，不会得到伪造的成功结果。

## 条款格式

每条规则一行，四列：

| 列 | 内容 |
|---|---|
| ID | `C-<领域>-<三位序号>`，如 `C-COMMIT-003`。编号一经发布不复用；规则作废时保留编号并标注“已废止”及原因 |
| 规则 | 可判定的陈述：给定输入与状态，结果是否符合可以由测试或审查回答 |
| 反例 | 违反该规则时会出现的具体情形，用于写反向测试 |
| 验证 | 覆盖它的实验步骤、工作包或测试；已实现的条款以“M<n> 已实现”开头（`n` 为实现它的里程碑）并写明测试名 |

规则中的“必须/不得”是规范要求，“可以”是允许但不要求。除验证列写明“M<n> 已实现”（如“M0 已实现”“M1 已实现”）的条款外，全部条款都是待实现的设计要求，不代表已有能力。

## 修改规则

- 改变已发布条款的语义，必须同时更新：受影响的头文件草案或实现、对应测试、`fixtures/` 中的用例，以及计划或 ADR 中引用它的段落（AGENTS.md「Commit & Pull Request Guidelines」）。
- 新增条款取该领域下一个序号；跨领域的规则只写在属主领域，其他文档引用 ID，不复制正文。
- 契约与计划冲突时，以较新的、经评审合入的一方为准，并在同一 PR 中修正另一方；不允许两处长期不一致。

## 领域前缀与属主

| 前缀 | 文档 | Core 属主模块（ADR-0008） |
|---|---|---|
| `C-VER` | [domain/versions.md](domain/versions.md) | 全局 |
| `C-ID`、`C-TIME` | [domain/ids_and_time.md](domain/ids_and_time.md) | `foundation` |
| `C-STATUS` | [domain/status_codes.md](domain/status_codes.md) | 公开 ABI |
| `C-PRIM` | [domain/primitives.md](domain/primitives.md) | 各原语属主 |
| `C-LIFE` | [domain/lifecycles.md](domain/lifecycles.md) | `kernel`、`work` |
| `C-CALL`、`C-BIND`、`C-SUB` | [domain/calls_and_bindings.md](domain/calls_and_bindings.md) | `work`、`catalog` |
| `C-OBS` | [domain/observations_and_media.md](domain/observations_and_media.md) | `knowledge/observation`、`kernel/runtime/ingress` |
| `C-COMMIT`、`C-OUT` | [domain/commit_and_output.md](domain/commit_and_output.md) | `durability`、`kernel` |
| `C-AUTH`、`C-TRUST` | [domain/authority_and_trust.md](domain/authority_and_trust.md) | `authority` |
| `C-RES`、`C-BUDGET` | [domain/resources_and_budget.md](domain/resources_and_budget.md) | `resources` |
| `C-RUN` | [domain/runtime_limits.md](domain/runtime_limits.md) | `work/task`、`resources`、Provider |
| `C-PERMIT`、`C-STOP` | [domain/permits_and_stop.md](domain/permits_and_stop.md) | `resources`（签发）、`control/`、`safety/`（执行） |
| `C-PROD` | [domain/product_semantics.md](domain/product_semantics.md) | 应用层 + `work`、`knowledge` |
| `C-MEM` | [domain/memory.md](domain/memory.md) | `knowledge`、`work/provenance`、`work/privacy`、`kernel` |
| `C-LIMIT` | [domain/limits.md](domain/limits.md) | `kernel`（配置） |
| `C-HOST` | [host/host_ports.md](host/host_ports.md) | `kernel` 与各 Host |
| `C-NATIVE` | [host/native_runtime.md](host/native_runtime.md) | `runtime/` |
| `C-SDK` | [host/cpp_sdk.md](host/cpp_sdk.md) | 公开 C++ facade、`runtime/examples`、`examples/` |
| `C-FIX` | [fixtures/normal/README.md](fixtures/normal/README.md) | 正常夹具应用、SimNode 组合与生成器 |
| `C-EXP` | [fixtures/experiments/README.md](fixtures/experiments/README.md) | 完整实验运行器、模拟故障与证据 |
| `C-PROV` | [host/provider.md](host/provider.md) | `work`、`catalog` 与各 Provider |
| `C-USAGE` | [host/tool_usage.md](host/tool_usage.md) | 各扩展、runtime 与模型 Provider |
| `C-POLICY` | [host/policy_interface.md](host/policy_interface.md) | `kernel`、`catalog` |
| `C-WIRE` | [wire/envelope.md](wire/envelope.md) | `kernel`（受管入口）与 `protocol/` |
| `C-JSON` | [wire/json_subset.md](wire/json_subset.md) | `protocol/` |
| `C-STORE` | [storage/records.md](storage/records.md) | `durability` |

## 追踪矩阵

各条款与计划章节、ADR、工作包和实验的对应关系见 [traceability.md](traceability.md)。M0 结束时由 `reports/core/m0-review.md` 复核：计划 §2.5/2.7/2.8/2.10/3.4/3.6 与五项产品语义中的每条规则都至少有一个条款，每个条款都至少有一个验证点。
