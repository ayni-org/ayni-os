# 观测入口与媒体引用

- **关联**：计划 §2.2、§3.6、§9.1；ADR-0050、0052、0062、0063；MA-008B
- **属主**：`kernel/runtime/ingress` 传输，`knowledge/observation` 当前事实；Host/Provider 外部媒体存储持有字节
- **头文件**：`observations.h`、`media.h`、`subscriptions.h`；ABI 0.43，storage schema 2 不变

本页的可信 Host 入口不是订阅授权入口。订阅的 ACTIVE/租期/Task/来源验证由 C-SUB-001～005 与 ADR-0063 规定。现有 owner 标量发布保持有效。

| ID | 规则 | 反例 | 验证 |
|---|---|---|---|
| C-OBS-001 | 任意非 ISR Host 线程投递样本时复制所有短期视图；只在 RUNNING 当前 boot 接受，close 先关门再丢弃，销毁前 join 全部生产者；不在 ingress lock 内调用 wake/clock | worker 返回后 Core 才读取栈上的 source 字符串 | M1 已实现：`ObservationIngress.worker_stack_views_are_copied_and_wake_outside_lock`、`ObservationIngress.close_discards_accepted_samples_and_orderly_reopen_rejects_old_boot`；join 是 runtime 调用前提，未验证未协作线程退出 |
| C-OBS-002 | 样本 FIFO 有界、不合并/淘汰，满返回 BUSY 并计数；接受不保证最终知识可用；owner 在原到期工作和输入后按 item/time 预算消费，最多入口快照数量 | 摄像头样本挤掉本地按钮或存储完成 | M1 已实现：`ObservationIngress.full_fifo_refuses_samples_but_preserves_gap_and_local_input`、`ObservationIngress.owner_discards_expired_or_future_samples_and_respects_item_budget`；队列上界见 C-LIMIT-008 |
| C-OBS-003 | 每源 gap 以独立保留槽合并为最高代际/序号前沿，重复幂等；owner 在派发前使未越过前沿的样本不可用，直到新样本；计数不重复已知漏号 | 样本 FIFO 满导致 gap 丢失，Scene 继续使用旧测量 | M1 已实现：`ObservationIngress.full_fifo_refuses_samples_but_preserves_gap_and_local_input`、`ObservationIngress.later_gaps_count_only_new_losses_and_old_markers_are_idempotent`、`ObservationIngress.new_generation_gap_refuses_old_tail_and_explicit_zero_boundary_allows_first_sample`；本轮限正常顺序，竞态专项未验证 |
| C-OBS-004 | 当前 boot/源版本/代际/严格递增序号、采集时间、TTL、质量决定可用性；保留诊断不把过期/失效值变成新证据，媒体 TTL 可以收紧样本寿命 | 排队两秒后的样本把 TTL 从消费时重新计时 | M1 已实现：`ObservationIngress.owner_discards_expired_or_future_samples_and_respects_item_budget`、`ObservationApi.expiration_is_visible_without_erasing_the_last_fact`、`ObservationIngress.media_metadata_is_copied_and_expires_with_its_shorter_lifetime` |
| C-OBS-005 | 媒体引用只含身份/owner/用途/有限期限/大小/内容类型/源代际，不含任意路径或 URL；Core 不持有原始字节，引用 schema 与 source/generation/boot 配对 | 把任意 file 路径当作受控媒体，或旧 boot 帧进入新观察 | M1 已实现：`MockMedia.startup_and_schema_failures_are_explicit`、`ObservationIngress.media_owner_and_generation_cannot_be_fabricated_by_late_samples`；metadata 不证明真实字节存在、读/外发授权或内容真实性 |
| C-OBS-006 | M1 Mock 字节独立限制对象和每对象字节；读取要求完整 reference 与显式 owner/purpose，复制而不借出指针；过期、代际关闭、新 boot 清空回收，ID 不复用 | 已关闭摄像头代际的旧引用读取复用槽中的新帧 | M1 已实现：`MockMedia.copied_bytes_require_exact_reference_owner_and_purpose`、`MockMedia.object_and_byte_limits_reject_without_overwriting_and_expiry_reclaims`、`MockMedia.generation_close_and_new_boot_reclaim_without_reusing_identity`；调用者负责真实授权及触发生命周期回收，订阅 Mock 自动按关闭/过期/boot 回收（`SubscriptionFixture.two_tasks_share_capacity_but_keep_samples_media_and_close_isolated`）；真实媒体仍待 M3 |
