# Host 外层文件值

- **关联**：ADR-0093；`ayni/hosts/storage/file_value.h`；C-HOST-018/019。C++源码端口，不是新增C ABI或Core记录。
- **版本**：外层文件AYFV仍为格式1；当前ABI0.55、Core schema10、会话6/turn7、SQLite4、正文manifest1分别版本化。已有Native应用存储不迁移；未知外层格式保留并拒绝。

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

## 共享正文与排队（ADR-0104）

| ID | 规则 | 违反例 | 验证 |
|---|---|---|---|
| C-HVAL-007 | stage在正数缓存额度内只接受不可变RAM正文且立即可读；confirm请求后台保存，只有全部正文及owner manifest持久后返回OK；write使用同一实现且保留原持久成功语义。share产生独立scope引用，重复同scope幂等；释放一个所有者不影响其他所有者，最后释放才删除文件 | 未保存正文被trim丢弃；Call退休删除聊天持有正文；只排队就声明持久 | `FileBodies.staged_shared_payload_survives_owner_release_and_normal_reopen`、`ConversationDatabase.staged_aliases_share_one_payload_and_release_independently`；正常路径，异常恢复未验证 |
| C-HVAL-008 | 检查中的payload load也计入容量；单payload只计一次，引用/owner元数据另有界。负查询不占已接受正文额度；保存/删除失败明确可查询，拒绝清理时不得ACK。临时模型输入由对应原Call终态持久后的独立lease回收，不能释放另一Call或已确认owner | 多个等待加载绕过预算；删除队列满被静默丢弃；结束一个模型Call删掉另一个相同prompt | `FileBodies.pending_loads_and_absent_queries_respect_the_payload_budget`、`BackgroundSession.consecutive_turns_trim_then_reopen_and_delete_shared_history`；失败路径设计审查，新专项未执行 |
| C-HVAL-009 | Native/ESP队列优先执行Core记录，再执行普通外层值，再执行后台正文；相同优先级保持FIFO，借用中的操作不中断。FileBodies每次至多一个4 KiB文件借用。上下文只有完整正文/索引及外层文件成功才发布；隐私GC保留旧图并在实际排空后ACK | 大正文一次塞满队列；不同Core批次重排；旧索引未加载就丢弃GC游标并ACK | `BackgroundSession.consecutive_turns_trim_then_reopen_and_delete_shared_history`、既有`FileValues.*`/`ConversationSession.*`正常检查；板端P95未测 |

正文manifest为小端AYNB/版本1：magic/version/size/owner_count四u32、digest/reserved两u64，共32 B；每个owner为scope/id两u64，至多64个。canonical内容identity由digest导出，manifest使用base owner，4 KiB内容块使用base+1+chunk；manifest的CAS generation独立于Core及会话版本。每store至多4096个内容元数据项。只在全部chunk成功后写owner manifest；修改所有者时冻结写入镜像，后续变更须再次保存才确认。未保存正文不能清退，缓存清退保留owner metadata。
