# Host 外层文件值

- **关联**：ADR-0093；`ayni/hosts/storage/file_value.h`；C-HOST-018/019。C++源码端口，不是新增C ABI或Core记录。
- **版本**：外层文件AYFV格式1，独立于ABI0.53、Core schema8、策略codec4和SQLite版本2。已有Native应用存储不迁移；未知外层格式保留并拒绝。

## 条款

| ID | 规则 | 违反例 | 验证 |
|---|---|---|---|
| C-HVAL-001 | owner提交仅复制有界请求元数据；文件值和Core请求共用单一后台队列、空间预留及关闭屏障。缓冲在完成前存活且不变；完成在worker恰好一次，回调不阻塞、不重入、不抛出，context保持至Host join。关闭排空后释放，旧端口拒绝且不保留 | 读写发生于step；回调设置标记后立即销毁仍被worker使用的context；关闭丢掉已接受写入 | M1 已实现（正常生命周期）；`FileValues.close_answers_an_accepted_value_and_old_ports_refuse_new_borrows`、既有HostStorage/NativeHost回归 |
| C-HVAL-002 | key为正owner/object，文件名内部生成，payload至多65536字节。写入CAS按generation；新generation大于previous，0 previous只接受文件不存在。已写相同generation只接受完全相同字节，并重写/同步候选确认持久；不同字节/版本冲突返回STALE。只在文件同步、原子替换和目录确认后返回OK，歧义替换返回RESULT_UNKNOWN，不能据此ACK Core上下文 | 读取到旧缓存就当作持久确认；同版本不同内容覆盖；仅队列接受便确认文件发布 | M1 已实现（正常CAS与重开）；`FileValues.durable_generation_write_retry_and_normal_reopen_preserve_exact_bytes`、`esp_files_emulated_normal_medium`（RAM介质） |
| C-HVAL-003 | 读检查名称身份、版本、长度及CRC。BUFFER_TOO_SMALL只给所需大小/版本，payload只在OK时可用；未知格式、损坏、I/O及容量失败明确保留分类，拒绝覆盖无法确认的旧文件 | 错误伪装为空对话，读失败后使用部分输出，未知版本自动覆盖 | M1 已实现（正常key与容量）；`FileValues.owner_object_keys_and_required_read_capacity_remain_explicit`；未知/损坏与介质故障拒绝路径已实现但未新增专项执行 |
| C-HVAL-004 | 删除请求不带CAS、缓冲或字节，删除对应文件后确认目录；文件本不存在也返回OK。正文存储（ADR-0096）以此释放不再被引用的正文 | 删除失败仍报OK；删除请求携带数据被受理 | M1 已实现（正常删除）；`FileBodies.released_bodies_leave_no_files` |
| C-HVAL-005 | FileBodies正数预算限制resident payload bytes，0保持原默认；自动清退仅释放无借用且结果已被调用方取走的durable payload，保留文件/descriptor及scope/digest/长度供后续collect。自动回收保留已完成但未取走的读写结果；显式trim可放弃这类缓存，取消后不必再取结果。无法腾出预算返回CAPACITY_EXCEEDED，主动清退受阻返回BUSY，不循环等待互相清退的正文。ready包含未受理删除；不以清缓存代替删除，不放弃排队清理 | 清退丢失scope导致漏删文件、多正文反复清退一直NOT_READY、满队列删除未受理即报排空 | `FileBodies.trimming_and_a_payload_budget_preserve_referenced_disk_bodies`、`FileBodies.scope_collection_survives_automatic_eviction_trim_and_reload`、`FileBodies.unconsumed_load_resists_automatic_eviction_and_allows_explicit_trim`、`FileBodies.tool_loop_rejects_a_read_working_set_larger_than_the_cache`、`WifiReply.CacheTrimRetainsCleanupUntilNormalFileQueueHasCapacity`；ADR-0100 |
| C-HVAL-006 | ValueAdmission固定N槽，部署选择1..N额度；owner提交、worker完成，每次接受保留原完成上下文，拒绝不保留、不占槽；端口/槽/缓冲保持到Host join | 普通正文占满为Core预留的队列、完成通知使用已复用槽的上下文、拒绝仍消费额度 | `WifiStorage.OuterFilesLeaveRoomForCoreAndReleaseTheirSlotsOnCompletion`；ADR-0100 |

## AYFV 格式1

固定48字节头，小端：magic `AYFV`（0），格式u32（4），owner u64（8），object u64（16），generation u64（24），payload字节u32（32），payload CRC32（36），前40字节的CRC32（40），零保留u32（44）；随后恰好payload字节。CRC为CRC-32/ISO-HDLC。头部、业务Conversation编码和Core journal是三个独立边界。

文件值generation是自身CAS版本，独立于Core上下文generation；调用者还须核对payload携带的上下文、轮次及隐私版本。

文件名按完整64位十六进制owner/object生成，候选以`.candidate`结束；命名空间由Host独占，FileLog拒绝使用保留的`value-`前缀，避免主日志与外层候选文件别名。替换前验证已存header和payload；一旦写请求成功受理，调用者必须等完成后再修改缓冲。Host不代替Core上下文版本准入，也不自动发出上下文ACK。真实Flash旧块擦除、掉电与耐久仍需各自证据。
