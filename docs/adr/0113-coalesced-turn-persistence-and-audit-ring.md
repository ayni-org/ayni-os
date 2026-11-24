# ADR-0113：回合级粗粒度合并落盘、非阻塞答案呈现与异步定长审计环

- **状态**：已实施（阶段 4，待真机 OTA 实测）
- **日期**：2026-11-24
- **关联**：[low-latency-realtime-agentos-plan.md](../architecture/low-latency-realtime-agentos-plan.md) §6 阶段 4、ABI `0.60`（Core `storage_schema` 维持 `11` 不变）、`FileBodies` AYNB 格式 `2`、`AuditRing`、`AuditSink`、ESP32-S3 Wi-Fi Chat 集成

## 背景

阶段 1–3 已将多工具调用链的中间 WAL 写入与电机/传感反射延迟压至毫秒级，但每轮对话结束后的历史持久化（`Session` publication）在 ESP32-S3 LittleFS 上仍耗时 1.5–3.5 s，并阻塞最终答案上屏与下一句输入，原因有四点：

1. **AYNB 格式 1 放大每正文 I/O**：`FileBodies` 对每个小于 4 KiB 的正文都要串行执行 `manifest_read` → `chunk0 write+fsync+rename+dirsync` → `manifest write+fsync+rename+dirsync`（3 次 LittleFS 操作，删除需 2 次）。
2. **历史索引细粒度拆分**：`strategies/builtin/history.cpp` 为每个工具调用的短 `arguments` JSON、`continuation` 等独立调用 `save_body()`，导致 3 工具回合额外生成多个小正文文件。
3. **普通回合同步等待旧文件删除**：`Session` 在 `record_saved` 后同步阻塞在 `Phase::collecting` 直到 `collect_superseded()` 删完旧 `history_index` 与 `rounds` 才发 `CONTEXT_ACK`。
4. **答案展示耦合持久化完成**：`Chat` 等到 `CONTEXT_ACK` 落盘才推送 `Kind::answer`。
5. **瞬态路径审计缺口**：`EPHEMERAL` 调用、资源抢占与本地反射均绕过同步 WAL，需要不阻塞热路径的定长审计通道。

## 决策

1. **`FileBodies` 干净升级为 AYNB 格式 2（不兼容格式 1）**：
   - 沿用仓库开发期不维护旧格式 reader 或迁移器的约定（`contracts/domain/versions.md`、ADR-0108/0110），将正文 manifest 升级为 AYNB 格式 2；读取格式 1 或未知版本明确返回 `AYNI_UNSUPPORTED_VERSION` 并保留原文件不覆盖。
   - 头 32 字节（`magic="AYNB"`、`format=2`、`size`、`owner_count`、`digest`、`flags`、`reserved=0`），`size <= 4096 B` 时置 `flag_inline_payload = 0x1` 并将正文紧随 owner 表内联存入 manifest 文件（无独立 chunk0 文件）；`> 4096 B` 继续 4 KiB 分块。
   - 新建小正文直接执行乐观 CAS 创建（`previous_generation = 0, generation = 1`），跳过初始 `manifest_read` 探测；若磁盘已存在同 digest 文件触发 `AYNI_STALE`，自动无错回退到 `manifest_read → merge → manifest_write`。
   - 被清退的小正文在发生部分 `release`/`share` 时使用独立于 `cache_limit_` 的瞬态内联缓冲重载并重写 manifest，确保紧缓存预算下 `release` 永不因 `CAPACITY_EXCEEDED` 失败。
   - 小正文写入由 3 次降至 **1 次** LittleFS 操作、冷读由 2 次降至 **1 次**、删除由 2 次降至 **1 次**。

2. **回合级历史小载荷内联（`AYNDXI1` 编码不变）**：
   - `strategies/builtin/history.cpp` 复用 `AYNDXI1` 原生纯字面量段：对尚未绑定共享 `BodyRef` 且 `<= 512 B` 的工具 `arguments`、`continuation` 及短文本不再调用 `save_body()`，直接内联随 `history_index` 一次落盘；`> 512 B` 载荷及既有 `ANRV2`/`history_texts` 共享引用保持原样，不破坏阶段 2 的零 Flash `volatile_discard()` 与 `ANRV2` 可审计性。

3. **普通发布延迟 GC + 0 ms 提前答案预览**：
   - `runtime::conversation::Config` 追加 `PublicationCollection collection_mode = PublicationCollection::synchronous`（默认 `synchronous` 完整保留既有 `C-SDK-009` 五阶段契约）。
   - `PublicationCollection::deferred` 模式下，普通 `Save::publication` 在 `record_saved` 后立即排队 `collect_superseded()` 并直接推进 `CONTEXT_ACK → Session::ready()`；隐私擦除 `Save::erasure`（`forget`）与关闭前 `quiesce()` 仍强等 `collected() == AYNI_OK`。
   - 新增只读 `Session::preview_answer(std::string &out) const`，在 `checkpoint_ready` 填充候选记录后即可 0 ms 从 RAM 读取答案文本；`TurnResult::published` 仍严格在持久 `CONTEXT_ACK` 后才为 `true`。
   - ESP32 `Chat` 启用 `deferred`，在 `checkpoint_ready` 即刻推送 `Kind::answer`，`CONTEXT_ACK` 落盘后绑定自动语音（`ReplyVoice`），并允许下一句输入无缝排队不回滚在途发布（`C-HVAL-009`）。

4. **Core 异步定长审计环 `AuditRing`（ABI `0.60`，`storage_schema` 维持 `11`）**：
   - `ayni_config_v1` 尾部追加 `audit_capacity`（`sizeof == 200`，默认 `0` 禁用且零 arena 开销，上限 `1024`）。
   - 新增严格 C99 头文件 `<ayni/audit.h>`：`ayni_audit_record_v1`（104 B，零隐式填充）、`ayni_audit_info_v1`（40 B，零隐式填充）、`ayni_get_audit_info()`、`ayni_take_audit_records()`。
   - Core 在 7 类关键边界以 O(1) RAM 覆盖最旧策略写入审计记录（不写 WAL、不阻塞 `step()`），溢出递增 `dropped_overflow` 并由单调 `seq` 断号暴露：Call 终态（含 `EPHEMERAL` 与 `DURABLE`）、权限/Scene 拒绝、资源抢占、执行端隔离/释放、Reflex 触发/拒绝、Kernel 生命周期迁移。
   - `hosts/storage` 新增可选 `AuditSink`（`AYAU` v1 小端格式，A/B 双槽低优先级 `FileValueRequest::Priority::background` 轮转写入，遇前次 boot `STALE` 自动清理重置，写失败与背压完全隔离不限制 Core）。
   - ESP32 默认配置 `audit_capacity = 64`（~6.5 KiB PSRAM），由 `Chat::drain_audit_records()` 排空至现有 `DiagnosticLog`（`/api/diagnostics` 可查），默认零 Flash 磨损。

## 后果

- **正面**：小正文 I/O 降至 1 次原子操作；普通对话答案呈现延迟降至 0 ms，后台发布操作数大幅下降且不阻塞下一轮输入；瞬态调用、抢占与反射拥有零 WAL 阻塞的结构化审计轨迹。
- **兼容性**：ABI 升至 `0.60`（0.x 调用方需重编译）；`AYNI_STORAGE_SCHEMA_CURRENT` 保持 `11` 不变；外层正文 manifest 升级至 AYNB 格式 2，旧格式 1 文件被拒绝并保留。
- **边界**：`preview_answer()` 仅用于展示预览，不替代 `TurnResult::published` 持久确认；真实硬件端到端 P95 时延仍需后续 OTA 真机实测确认。

## 备选方案

- **兼容读取 AYNB v1**：会增加双版本编解码与回退复杂度，且不符合仓库当前开发期版本政策（ADR-0108/0110），故未采用。
- **将所有审计事件同步写入 Core WAL**：会重新引入阶段 1–3 消除的 Flash 同步阻塞，违背实时控制目标，故采用定长 RAM 环 + 异步低优先级排空。

## 验证

- ABI 0.60 布局与 C99 冒烟：`tests/contract/abi_layout.c`、`tests/contract/c_abi_smoke.c`、`tests/unit/kernel_layout_test.cpp`。
- AYNB v2、历史内联、延迟 GC、`AuditRing`、`AuditSink`：`tests/unit/file_bodies_v2_test.cpp`、`tests/unit/history_loading_test.cpp`、`tests/unit/deferred_publication_test.cpp`、`tests/unit/audit_ring_test.cpp`、`tests/unit/audit_sink_test.cpp`。
- 构建与门禁：Portable ASan/UBSan 全量单元测试、ESP32 Wi-Fi 主机测试、ESP-IDF 6.0.3 Xtensa `-Werror` 固件构建、`clang-format 18.1.8`、`check_deps.py`、`check_contracts.py`、`check_readability.py`、`git diff --check`（以实际执行结果为准）。
