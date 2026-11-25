# ADR-0115：未决操作账本与工作容量分离

- **状态**：已实施；正常软件、独立审查及最终固件编译与分区检查通过，已正常OTA并完成启动自确认。
- **日期**：2026-11-25
- **关联**：[调用契约](../../contracts/domain/calls_and_bindings.md)、[记录格式](../../contracts/storage/records.md)、ADR-0045、ADR-0110～0114；ABI 0.62 / schema 12。

## 背景

工作、活跃调用、未决副作用与审计的保留期限不同。把未知操作留在 CallTable 并永久钉住已结束 Task，会耗尽工作容量；把归档状态加入 CallTable 则混合正文、运行借用和历史生命周期。取消朗读时已有部分输出也不能仅凭“发生过输出”推导 UNKNOWN。

## 决策

用户要求实施独立账本方案，并允许按实际字段采用 84 B 增量记录，不机械限制提交或编码细节。采用四层正交生命周期：Goal/Task 管工作；CallTable 只管未退休工作与 Reflex 活跃调用；`work/operation/OperationLedger` 管已退休 Task 的持久未决操作；AuditRing 与可选异步 AuditSink 管终态证据。

1. Speaker 只有 I²S disable、delete、park 全部成功才确认停止。已停的部分取消返回 CANCELLED/OK/PARTIAL，未确认输出停止仍为 ABANDONED/RESULT_UNKNOWN。`speaker.speech`/`speaker.speak_text` 改为 Tier 0 EPHEMERAL，ReplyVoice 明确 CANCEL_ON_REBOOT；朗读块还须等原 Provider 终态回执才释放。
2. Tier 0 UNKNOWN 存活期 HOLD，禁止自动重试；工作终止、停止和借用归还后可退休，保留原有零 WAL 行为，由 AuditRing 记录 EPHEMERAL 终态。物理停止未确认或借用未归还时不能退休。
3. OperationLedger 用独立有界 caller arena，不持有 domain/input/result 正文池。保留完整 CallContract（包括 durability、dedupe、资源及 units）与独立 business_contract 字节区。查询直接复用 `ProviderRegistry::matches_saved_call()`，不能把同 Provider ID/version 当作足够证明。
4. 退休先整批统计容量、只读预检每条，再不可失败地迁入并释放 Call 正文引用/槽位。迁移依原 Goal/Task 替换记录重建，沿用同一个 WAL 批次，交接本身不写额外记录，不新增 fsync 或同步等待。live 与 replay 传完整 boot/transition/commit stamp。
5. 独立对象 `AYNI_OBJECT_UNRESOLVED_OP`、紧凑 `ayni_get_unresolved_op`、QUERY 和 ACKNOWLEDGE 管理命令均经原 owner management、精确命令身份与 CAS。QUERY 是原操作只读对账，不恢复 Task，不执行旧请求。ACK 是人工接受并关闭未知事项，不伪造效果已知；删除回执保留原 CAS，包括最大 revision。
6. 任何合法 call_capacity 启用账本后都多一个专属 ProviderMailbox 槽（最大 257）。活跃调用最多占自身容量；可推进的账本查询准备/派发先于普通进度消息消费，持续 progress 不能饿死查询。
7. 权威已知结论或人工结案立即移除账本行，同时保留固定容量 PendingAudit。只在 closure commit_seq <= 当前 writer durable_seq 后发出 OPERATION_RESOLVED；人工结案带 ACKNOWLEDGED。队列满时 ACK 返回 BUSY、查询回执延后消费；不丢审计、不追加额外 WAL。fenced reload 清除未落盘义务，已恢复的历史提交不重新发射 RAM 审计。
8. ABI 0.62 要求全部消费者重编译；storage schema 12 在头部拒绝并保留旧 schema 11 卷，无迁移器或自动擦卷。缩容放不下历史记录按 C-STORE-015 返回 no_room/STATE_EXCEEDS_CAPACITY；以足够容量启动并清理后才缩容。
9. Core 配置默认 unresolved_op_capacity=0，不分配槽位区域；共享 demo profile（native_node/native_cli）与 ESP 配置显式启用 16。CLI `/unresolved`、统一 `/query`、`/ack` 和 ESP 无正文诊断补齐消费者。

## 后果

未知项不自动淘汰。账本满仅阻止需要迁入未知项的工作退休，不阻止不需账本的 Tier 0/Reflex 正常使用；活跃工作表仍有自身容量限制。完整契约证明按配置占独立 arena，内存要求由公开工具复核，不把 Native 数字等同板端总内存或 PSRAM 峰值。

有限表扫描、查询调度与 Flash 争用仍有成本；“零额外 WAL”专指工作退休交接及 Tier 0 路径，显式管理对账/结案有自己的正常 transition。审计环维持 104 B 记录和原覆盖最旧/溢出计数语义，不能把异步审计当成永久无损档案。

## 备选方案

保留 Task：工作容量与未决历史互相阻塞。原 CallTable 内 archived/延迟淘汰：混合正文和生命周期，读者与回收状态复杂。另起归档事务：增加写放大与交接等待。削减保存契约：可能误认改变语义的 Provider。以上均不采用。

## 验证

相关正常便携 ASan/UBSan 回归、独立 Wi-Fi 软件、C99/C++ ABI、Native 变更消费者编译、arena 查询及最终 ESP-IDF 编译/分区检查通过；独立审查发现已修复并复核，无遗留发现。未创建远程 PR；实际命令、结果、工具版本和四项自查保存在仓库外临时目录 `/tmp/ayni-operation-ledger/`，不是远端 CI 或完整 Linux Native 验证。固件保留上游 md4c 编译告警与 Kconfig NOTE，未放宽仓库告警策略。初次软件验证未执行 OTA、USB、Flash 擦卷、云端模型/语音或真实动作；不启动通用故障、恢复、变异、耐久或实体停止链专项。正常关闭重开和 codec 输入边界不替代新格式异常恢复与硬件验收。


### 2026-11-26 审查修复

用户要求全部修复Neon审查项。合法未知/取消和旧boot查询终态不再误记Provider违规，原操作仍保持UNKNOWN；fenced期间保留持久查询回执，重载后只归还旧borrow、不采纳其已知结论。已排队终态优先消费，不在期限边沿重复取消。快照账本命令须发生于退休后，已移除回执不能指向仍活跃的Call。

ESP朗读枚举按部署call_slots缩小栈数组，诊断容量加编译期约束；账本arena按slots/audits/proofs一次初始化并断言对齐，停稳三元组规则归work/execution共用。ABI0.62发布前将192 B视图尾部预留位定义为query_authority（偏移188），Core/CLI/ESP同步；原语义来自保存的契约，无新持久字段或schema变化，消费者统一重编译。

本轮正常软件、限定fenced查询回执重载、快照命令边界、独立复查与最终固件编译/分区检查通过；审计背压下的期限就绪投影遗漏也已补齐并回归。Xtensa编译栈报告确认朗读轮询帧缩小，未测真机栈水位。没有恢复通用故障/恢复专项或执行设备操作，限定回归不代表新格式异常恢复完整验收。实际命令、结果、栈报告及四项自查存于仓库外`/tmp/ayni-ledger-neon-fixes/`，本轮未运行完整Linux Native/远端CI。


### 2026-11-26 正常OTA与指定旧数据清理

用户随后要求OTA，并明确授权清理设备旧数据。设备由d9f476e/schema11经隔离构建的一次性过渡镜像升级到44278a8/ABI0.62/schema12。旧Core/Host真实关闭和整镜像校验完成后，复用ResetWorker核对并清理唯一ayni分区，再选择新应用槽；配置、密钥、配对和偏好分区保留。产品源码不包含过渡擦除逻辑，也不据此授权未来自动清理。

正常升级、空卷启动、自确认和配置摘要核对通过；未请求模型、语音或真实运动。镜像、隔离源码/补丁和实际部署记录在仓库外本机忽略目录`experiments/esp32_wifi_chat/build/ota-ledger-44278a8-20261126/`；详细结果见[实验记录](../../experiments/esp32_wifi_chat/WIFI.md#2026-11-26-独立账本ota与旧数据清理)。正常OTA不替代新格式异常恢复、掉电回退、独立硬件停止链或耐久验收。
