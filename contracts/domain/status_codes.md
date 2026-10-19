# 状态码

- **关联**：计划 §3.2「错误」、§3.4；ADR-0006；`MA-005`
- **定义位置**：`include/ayni/ayni.h`（唯一来源；Core 内部不复制一份）

## 取值

0 表示成功，负数表示失败。已发布的值不改作他用；调用方把不认识的负值当作一般失败处理。状态码描述的是**这一次调用**的结果，不是用户文案，也不是领域对象的状态。

| 值 | 名称 | 含义 | 调用方可以做什么 |
|---:|---|---|---|
| 0 | `AYNI_OK` | 调用完成了它声明的效果 | — |
| -1 | `AYNI_INVALID_ARGUMENT` | 参数本身不合法：空指针、长度或对齐不符、缺少必需字段 | 修正调用；重试同一参数仍会失败 |
| -2 | `AYNI_INVALID_STATE` | 参数合法，但对象当前状态不允许该操作 | 等待状态变化后再试 |
| -3 | `AYNI_UNSUPPORTED_VERSION` | ABI、协议、存储或能力版本不受支持 | 升级或降级一方 |
| -4 | `AYNI_UNSUPPORTED` | 本构建或本设备不提供该功能（显式拒绝，不静默降级） | 改用其他能力或配置 |
| -5 | `AYNI_NOT_READY` | Kernel 不在 `RUNNING`，或依赖尚未就绪 | 等待生命周期事件后重试 |
| -6 | `AYNI_BUSY` | 暂时没有容量（如完成槽已满），形成背压 | 稍后重试同一请求 |
| -7 | `AYNI_CAPACITY_EXCEEDED` | 达到配置的容量上限或内存不足 | 释放资源或调整配置；不会自动恢复 |
| -8 | `AYNI_BUFFER_TOO_SMALL` | 调用方缓冲不足；需要的长度已写入输出参数（如有） | 按所需长度重试 |
| -9 | `AYNI_NOT_FOUND` | 引用的对象不存在于本机目录或记录中 | 不能据此推断外部动作未执行（C-STATUS-005） |
| -10 | `AYNI_DENIED` | 授权、主体或场景检查拒绝 | 取得授权；不应换个参数绕过 |
| -11 | `AYNI_EXPIRED` | 期限、租期、许可或去重低水位已过 | 重新准入或以新身份发起（遵守 C-CALL-008） |
| -12 | `AYNI_STALE` | 新鲜度证明（`receiver_tick`、观察有效期、依赖版本）不足 | 取得新证明后以同一幂等键重试 |
| -13 | `AYNI_BUDGET_EXHAUSTED` | 预算余额不足 | 等待周期重置或调整预算 |
| -14 | `AYNI_STORAGE_FAILED` | 持久化失败；相关输出未释放 | 查看诊断；Kernel 可能已进入受限栅栏 |
| -15 | `AYNI_REVISION_CONFLICT` | `expected_revision` 或依赖版本已过时 | 读取新版本，重新评估后再提交 |
| -16 | `AYNI_IDEMPOTENCY_CONFLICT` | 同一幂等键对应了不同的规范化参数摘要 | 换新的幂等键表示新意图 |
| -17 | `AYNI_RESULT_UNKNOWN` | 已越过可能派发边界，结果无法确定 | 查询或人工处理；不得自动重试（C-STATUS-004） |
| -18 | `AYNI_INTERNAL` | Core 检测到自身不变量被破坏 | 报告缺陷；Kernel 可能进入 `RESTRICTED` |

## 规则

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-STATUS-001 | 每个公开函数在文档中列出它可能返回的全部状态码；返回列表外的值视为缺陷 | `ayni_create` 在内存不足时返回 `AYNI_INTERNAL` | M0 已实现：`include/ayni/ayni.h` 为每个已实现函数列出返回码，`tests/unit/kernel_api_test.cpp` 覆盖 `ayni_create` 文档列出的每个失败码；`MA-005` 生命周期返回值表：`KernelLifecycle.return_values_follow_the_state_table`、`KernelLifecycle.step_rejects_bad_arguments_without_effect`、`KernelLifecycle.kernel_info_checks_its_output` |
| C-STATUS-002 | 失败调用不产生部分效果：输出参数要么不写，要么写入文档规定的值（如 `*out_kernel = NULL`），调用方内存保持不变 | 版本不符时 `ayni_create` 已经清零了调用方的内存块 | M0 已实现：`KernelApi.failed_create_leaves_memory_untouched` |
| C-STATUS-003 | 状态码区分“参数错”（`INVALID_ARGUMENT`）、“时机不对”（`INVALID_STATE`/`NOT_READY`/`BUSY`）、“不支持”（`UNSUPPORTED*`）与“被拒绝”（`DENIED`/`EXPIRED`/`STALE`/`BUDGET_EXHAUSTED`），不合并为通用失败 | 所有失败都返回 `-1`，调用方无法决定是否重试 | 评审；`MA-005` |
| C-STATUS-004 | `AYNI_RESULT_UNKNOWN` 不是失败：调用方不得因它自动重试或换身份重做，只能查询、对账或交给人工 | Provider 超时后 Kernel 自动用新 `operation_id` 重发 | E2 F3–F5 |
| C-STATUS-005 | `AYNI_NOT_FOUND` 只说明本机查不到记录；只有满足 C-CALL-010 的权威未执行结论才能推断未执行 | 远端查询返回 `NOT_FOUND`，调用方据此重新派发运动 | E2 查询夹具（“仅未找到记录”响应） |
| C-STATUS-006 | 可选的 `ayni_error_v1` 输出原因码与对象引用，保留分类原因；不包含凭据、私有记忆正文、媒体或个人数据 | 错误详情里带出 API Key 或用户语音转写文本 | M1 日志与诊断测试；`MA-005` |
| C-STATUS-007 | 状态码是 ABI 的一部分：新增值只能取尚未使用的负数，已发布值不重新编号 | 新版本把 `-6` 从 `BUSY` 改为 `DENIED` | 评审；C-VER-002 |
