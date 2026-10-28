# 受限 JSON 子集

- **关联**：计划 §3.4「消息限额」、§3.6；ADR-0009；`MA-007`、`MA-016`；第 10.6 节「事件与容量」「可维护性」（解析 fuzz）
- **属主**：协议适配层的解码器；领域校验器在 `domain/` 对应的属主模块

控制消息使用有限长 JSON（RFC 8259 的子集）。本文同时规定能力注册时允许使用的 JSON Schema 关键字子集。数值限额是初始值，M1 解析测试与 ESP32 测量后锁定，调整须记录 ADR。

## 帧限额

| 项目 | 初始值 |
|---|---:|
| 单个控制帧 | 8 KiB（8,192 字节，UTF-8 编码后） |
| 嵌套深度（对象与数组合计） | 8 |
| 集合长度 | 由字段 Schema 的 `maxItems` 限定，未声明即拒绝注册 |
| 字符串长度 | 由字段 Schema 的 `maxLength` 限定（按 UTF-8 字节计），未声明即拒绝注册 |

## 允许的 Schema 关键字

| 允许 | 说明 |
|---|---|
| `type` | `object`、`array`、`string`、`integer`、`number`、`boolean`；`null` 只在字段声明可为空时 |
| `properties`、`required`、`additionalProperties` | `additionalProperties` 只能为 `false`；未知可选字段的处理见 C-JSON-006 |
| `items`、`minItems`、`maxItems` | `maxItems` 必需 |
| `minLength`、`maxLength` | 按 UTF-8 字节计（与 JSON Schema 标准的码点计数不同，注册时说明）；`maxLength` 必需 |
| `minimum`、`maximum`、`exclusiveMinimum`、`exclusiveMaximum` | 数值字段必需给出有限范围 |
| `enum`、`const` | 枚举只含字符串或整数 |
| `x-ayni-unit`、`x-ayni-frame`、`x-ayni-time-base` | 单位、坐标系与时间基准（Ayni 扩展关键字） |

其余关键字（如 `pattern`、`oneOf`/`anyOf`/`allOf`/`not`、`patternProperties`、远程 `$ref`、`format`）在注册时拒绝。`pattern` 被排除是为了避免在 MCU 上运行正则引擎及其回溯开销。

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-JSON-001 | 控制帧超过 8 KiB 时在解析前拒绝，不分配超出帧预算的内存 | 收到 1 MiB 的 JSON，解码器先整段缓存再报错，ESP32 内存耗尽 | 解析 fuzz；`MA-007` |
| C-JSON-002 | 嵌套深度超过 8 时拒绝；解码器不使用不受限递归 | `[[[[...]]]]` 嵌套 10,000 层导致栈溢出 | 解析 fuzz；`MA-007` |
| C-JSON-003 | 字符串必须是合法 UTF-8，长度不超过字段 `maxLength`（字节）；非法序列拒绝，不替换为 U+FFFD | 含孤立代理项的名字被替换后存入记忆，删除时匹配不上 | 解析测试；`MA-007` |
| C-JSON-004 | 64 位整数（ID、计数、序号、`receiver_tick`、`commit_seq` 等）以十进制字符串表示；JSON 数字只用于 Schema 范围在 ±2^53 以内的值 | 64 位 `task_id` 以数字发送，浏览器端解析后末几位变成 0 | `MA-007` 往返测试；C-ID-002 |
| C-JSON-005 | 禁止 NaN、Infinity 与超出声明范围的数值；物理参数除 JSON 类型外还须经语义校验（单位、范围、校准有效性） | 转向角度为 `1e308`，类型检查通过后传给运动 Provider | 解析 fuzz；C-AUTH-016 |
| C-JSON-006 | 缺省、`null` 与未知字段的含义固定：缺省取 Schema 默认值；`null` 只在字段声明可为空时表示“明确为空”；同一协议主版本内未知的可选字段忽略；未知的必需功能（`required_features` 中列出）拒绝 | 新版本增加的必需安全字段被旧接收方当作可选字段忽略 | 解析测试；C-VER-010 |
| C-JSON-007 | 对象中出现重复键时拒绝整个消息 | `{"direction":"left","direction":"right"}` 在不同解析器中得到不同结果 | 解析 fuzz |
| C-JSON-008 | 枚举字段遇到未知值时拒绝，除非该字段声明为开放枚举并规定未知值的处理 | 未知的通知渠道值被当作默认渠道 | 解析测试 |
| C-JSON-009 | Schema 合法与语义合法分开检查：通过 Schema 的消息仍可被领域校验器拒绝，拒绝原因可区分 | 只做 Schema 校验，参数在范围内但场景未授权的转向被执行 | `MA-005`、`MA-007`；C-WIRE-014 |
| C-JSON-010 | 绝对时间以 RFC 3339 UTC 字符串表示并附时钟可信度；相对时长以整数毫秒字段（`*_ms`）表示；单调时间值不出现在跨设备消息中（C-TIME-002） | 消息携带发送方的单调时间戳，接收方与本机时钟比较 | `MA-038`；`MA-011` |
| C-JSON-011 | 物理量字段声明 `x-ayni-unit`（默认 SI 与弧度），需要时声明坐标系；外部单位须经显式转换进入；无法确定单位或坐标系时拒绝相关动作 | 角度以度传入，按弧度执行 | `MA-035`（可选）；R1 A1 |
| C-JSON-012 | 能力注册时只接受上表允许的 Schema 关键字，且每个字符串与集合都有上限；不满足时注册失败（`ayni_start` 返回校验错误） | 能力的输入 Schema 使用 `pattern` 或缺少 `maxItems`，运行时才发现无法有界解析 | M1 仅有 typed 注册，无 JSON Schema 入口；按用户 2026-10-28 决定（ADR-0079）留到 `MA-007`，关键字解析未验证；E4 typed 用例不替代本条 |
