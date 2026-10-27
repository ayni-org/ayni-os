# 实验夹具格式

P6 已实现的独立正常闭集见 [normal/README.md](normal/README.md)，使用 `fixture_schema=1` / `profile=m1-normal`。下文是原完整 schema 0 草案；故障/恢复动作及原完整实验仍未交付，不能作为正常 profile 输入。

本目录规定 E1–E4 用例的数据格式（计划 §10.1、ADR-0012 第 3 条），并给出 M1 要实现的用例清单 [cases.md](cases.md)。M0 只规定格式，不包含可运行的用例；第一批用例文件与运行器在 M1 随 Kernel 实现加入。格式版本 `fixture_schema` 为 0（experimental），M1 写完第一批用例后收敛为 1，此后只追加字段，已发布字段的含义不变。

## 为什么用数据而不是测试代码

E1–E4 先在电脑确定性模拟器通过，再在 ESP32 用同一契约复核（计划 §10.1）。如果两边各写一份测试代码，迟早出现“电脑上测的和板子上测的不是同一件事”。因此用例写成数据，两边的运行器执行同一份数据，断言逻辑放在与测试框架无关的 C++ 函数中；GoogleTest（Native）与 Unity（ESP32）只负责调用和报告（ADR-0012）。

## 文件与命名

```text
contracts/fixtures/
├── README.md          # 本格式
├── cases.md           # E1–E4 用例清单（M0 起维护）
└── e1/ e2/ e3/ e4/    # M1 起：每个用例一个 <case_id>.json
```

- **用例 ID**：`<实验>.<步骤>.<场景名>`，如 `E2.F3.query_not_executed`。步骤沿用计划的编号：E1/E3/E4 写 `S<n>`（计划中的“步骤 n”），E2 写 `F<n>`；场景名用 `snake_case` 描述场景（AGENTS.md「Testing Guidelines」）。用例 ID 一经发布不复用。
- **测试名**：运行器把用例 ID 映射为 `E2_F3.query_not_executed`（GoogleTest 的 `Suite.test`），失败报告直接指回用例文件。
- **文件名**与 `case_id` 一致；一个文件只有一个用例。变体（例如“支持查询”与“不支持查询”）各写一个文件，不在文件内用参数展开，避免长成一门配置语言。

## 用例结构（`fixture_schema` 0）

用例文件使用受限 JSON 子集（[`wire/json_subset.md`](../wire/json_subset.md)）：64 位整数写成十进制字符串（C-JSON-004），不允许重复键。

| 字段 | 类型 | 必填 | 含义 |
|---|---|---|---|
| `fixture_schema` | 整数 | 是 | 当前为 0 |
| `case_id` | 字符串 | 是 | 与文件名一致 |
| `experiment_step` | 字符串 | 是 | 计划中的步骤，如 `"E2 F3"`、`"E1 步骤 2"`；生成器核对它在计划中存在 |
| `clauses` | 字符串数组 | 是，非空 | 本用例证明的条款 ID；生成器核对每个 ID 已定义 |
| `description` | 字符串 | 是 | 一句话说明场景与预期 |
| `seed` | 十进制字符串 | 是 | 全部伪随机选择的种子 |
| `config` | 对象 | 是 | Kernel 配置与容量限额（C-LIMIT-*）、FakeClock 起点、时区规则版本；未写的项取 `limits.md` 的默认值 |
| `world` | 对象 | 是 | 初始世界：设备、资源、能力、Binding、Mock Provider 行为、实例、Grant、Task 运行限制、记忆记录。对象用夹具内的名字引用（如 `"@A"`、`"@model_slot"`），运行器按出现顺序确定性地分配真实 ID |
| `script` | 数组 | 是 | 有序动作，见下文“动作” |
| `invariants` | 数组 | 否 | 每次 `step` 之后都必须成立的断言（例如模型槽活跃数 ≤ 1） |
| `expect` | 数组 | 是 | 脚本结束后的断言，见下文“断言” |

`script` 中的动作可以带 `expect_status`，断言该动作的同步返回值（如 `AYNI_NOT_READY`）；没有写时，返回值不是 `AYNI_OK` 即失败。

## 动作

动作是封闭集合，按计划 §10.1 列出的夹具能力分组。新增动作须同时修改本表和两边的运行器；M1 写第一批用例时允许增删，`fixture_schema` 升为 1 后只能追加。

| 夹具能力（计划 §10.1） | 动作 | 含义 |
|---|---|---|
| FakeClock（含时区/校时跳变） | `advance_clock` | 推进单调时间（微秒） |
| | `set_wall_clock` | 设置绝对时间与是否可信，用于校时跳变与时区规则 |
| 可控事件顺序 | `submit_intent` | 以指定主体、渠道信任级别与 `command_id` 提交意图 |
| | `post_event` | 投递观察、遥测、本地输入或时钟可信事件 |
| | `step` | 调用 `ayni_step`，可写 `repeat` 次数或 `until_idle` 加上限；超过上限即失败 |
| 双 Mock Provider | `provider_respond` | 让某个挂起的请求以指定结果（完成、失败、取消、进度、无法取消、取消未知）和可选 usage 诊断事实返回 |
| | `provider_query_answer` | 设定权威查询的回答：`FOUND`、`NOT_ACCEPTED`（权威未执行）或 `UNKNOWN`（仅未找到记录） |
| 可重启存储 | `restart` | 丢弃全部易失状态，用同一份存储重建 Kernel；Host 侧计数器保留 |
| 批次中断/短写/丢失/损坏注入 | `storage_fault` | 对指定批次注入断电（在某条记录之后）、短写、丢失或 CRC 损坏 |
| 写结果注入 | `storage_result` | 让指定批次的写入以 `OK`、`FAILED` 或 `UNKNOWN` 完成，可设延迟到下一次重载之后 |
| 按屏障切断的 Hook | `break_at` | 在 `PREPARED`、`DISPATCHING` 或完成记录的“写入前/已写未持久/已持久”处切断（等同断电） |
| 带代际订阅 | `subscription_event` | 以指定 `(subscription_id, generation, sequence)` 投递订阅事件，可指定旧代际或旧 `boot_epoch` |
| 模拟通知呈现器 | `presenter_result` | 给出呈现成功、失败、未知、回执重复或回执丢失 |
| 模拟固件/Schema 版本 | `reconfigure` | 在 `restart` 前替换能力/Scene/GoalType 版本、Binding、策略或存储格式（E3、E4） |
| 独立副作用计数器 | （无动作） | Host 侧对每个 operation 真正执行副作用的次数，跨重启保留，只供断言读取 |
| 模拟执行端 | `executor` | 投递、丢弃、延迟或重放许可与续租；注入执行端重启、校准失效、急停与急停复位 |

## 断言

断言同样是封闭集合，读取的是 Kernel 公开查询结果、运行轨迹与 Host 侧计数器。“未执行”的证据只来自 Host 侧计数器，不只看可能已损坏的 Core 日志（计划 §10.1）。

| 断言 | 含义 |
|---|---|
| `side_effect_count` | 某能力或某 operation 的 Host 侧真实执行次数等于给定值 |
| `object_state` | 某对象（Goal、Task、ActionRecord、Grant、订阅、记忆记录）的状态或字段等于给定值 |
| `resource_holders` | 某资源当前或历史最大占用者数量满足给定上限 |
| `task_runtime` | Task 模型调用上限、已接受次数与限制暂停（ADR-0042）；无钱包/费用预留 |
| `trace_order` | 轨迹中若干事件按给定顺序出现 |
| `trace_absent` | 轨迹中不出现某类事件（例如撤销栅栏之后的新派发） |
| `executor_stopped_by` | 模拟执行端记录的停止时刻不晚于给定表达式（如 `revoke_at + L`） |
| `durable_seq_covers` | 某个对外输出释放时 `durable_seq` 已覆盖它的 `requires_seq`（C-COMMIT-004） |

## 运行记录

计划 §10.1 要求每次运行保存可复现所需的全部信息。运行器为每个用例写两份文件，编码同样使用受限 JSON 子集：

- `run.json`：用例 ID、`fixture_schema`、代码提交、工具链、ABI/协议/存储版本、配置、种子、宿主（native 或 esp32）、通过与否、第一个失败的断言。
- `trace.jsonl`：每行一条记录，字段为序号、`step` 序号、FakeClock 时间、类别（transition、commit、output、side_effect、permit、resource、task_runtime、subscription、fault）、相关 ID（command、transition、`commit_seq`、operation、`(boot_epoch, request_id)`、`logical_call_id`、订阅代际）与内容摘要。私有记忆正文、凭据与媒体内容只写摘要，不写原文。

CTest 运行时写入构建目录；作为验收证据归档时复制到 `reports/`，与代码版本一起提交。

## 生成与运行（M1、M2）

- `tools/` 下的生成器读取全部用例文件，检查结构、`case_id` 与文件名一致、`experiment_step` 存在、`clauses` 已定义，然后生成一个 C++ 源文件，把用例编译成只读表。
- Native 的 GoogleTest 运行器与 ESP32 的 Unity 运行器链接同一个生成文件和同一组断言函数。设备上不解析 JSON，固件也不携带用例文本。
- 逻辑轨迹在两边必须一致；真实耗时（中断延迟、Flash 写入时间等）另行测量，不与虚拟时间比较（计划 §10.2 确定性基线配置）。
- 多节点 FakeTransport（`MA-038`）与 R1 具身用例不在本格式的 M1 范围内，届时扩展动作表。
