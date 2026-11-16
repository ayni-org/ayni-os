# ADR-0108：统一会话与工具结果，移除旧兼容和无限缓存

- **状态**：已接受用户选择；实施及本地验证记录见[独立实验记录](../../experiments/esp32_wifi_chat/WIFI.md#2026-11-16-统一会话和结果存储)。暂不OTA。
- **日期**：2026-11-16
- **关联**：ADR-0100、0104～0107；C-SDK-009/011、C-HVAL、C-POLICY-012/013；计划§14.1。

## 背景

用户明确Ayni尚未公开发布，不保留旧兼容，要求把前轮流程精简留下的重复实现清理干净。Native仍有自己的会话发布/擦除状态、SQLite正文缓存和原结果archive；ESP/portable已经使用共享Session/FileBodies。选不同Host时结果大小及保存路径分叉，零缓存预算还隐含无限模式。前轮净增代码也来自旧机制未完全删除和新所有权保证混在一起。

## 决策

1. Native直接采用共享Session和FileBodies，删除ConversationStore、独立history/results实现及其发布/清理状态。保留窄SqliteValues适配真实异步值I/O；换文件后端只改变值端口，换模型仍只改变Provider/协议装配，共用会话不认识SQL或厂商协议。
2. Session唯一拥有输入、原Goal挂接、发布、取消、擦除、正文确认和精确ACK。默认durable/retain保持；Native显式选择retain_and_append，ESP显式background。策略区别不再创建两套状态机。正常重开识别原Goal，不重建旧动作；当前权限、来源、停止、UNKNOWN不重试与实际持久屏障保持。通知先peek元数据，所需正文读成功再消费。显式取消即时尝试提交原Goal取消，避免下次Provider推进先执行仍排队的软件工作。
3. 原始结果统一保留规范值、域、证明和Call事实的小视图；普通/受管工具和Native/ESP共用实现，展示可另附摘要。删除HistoryPort、ArchivedResult、archive/result_chunk及预执行大小分叉。已执行长结果的分块摘要复用一个临时投影，每次决定仍查询当前Core来源；不能靠重新执行获得结果。
4. 临时投影在现有有界Turn槽中保留，仅覆盖当前大结果，结束/失效/缓存退休即释放；避免每块重新加载/解码完整结果。它与已解码提议共用一个私有、检查分配的同步所有权容器，保证DEFER工作副本不深拷贝。取舍是摘要期间持有一份完整投影；上限来自已准入结果及协议编码，不宣称ESP实测峰值下降。
5. FileBodies必须显式正数预算，删除无限模式：Native16 MiB，ESP/portable2 MiB；保留有限descriptor/owner及4 KiB在途块。SQLite5只存state(owner,object,generation,body)，两个排队元数据槽、一个worker，同优先级FIFO、普通值优先后台正文。DELETE+EXTRA提交和secure_delete保持文件端口及隐私保证（[SQLite同步说明](https://www.sqlite.org/pragma.html#pragma_synchronous)）；设置不替代掉电证据。
6. ABI0.56/Core schema10/ATC3/ConversationRecord6保持，私有turn8/ANRV2/SQLite5直接升级。Wi-Fi设置仅接受当前v2，删除v1 reader；不迁移或启动擦除。不兼容目录保留并明确拒绝，回退保留原目录与匹配旧二进制。消费者、契约和测试一起更新。

## 后果与备选方案

继续维护Native/ESP双会话和双正文实现会让权限/持久修复分散，拒绝此方案。把SQL同步塞进Session会阻塞Core，故只保留真实平台I/O边界；保留旧archive或无限缓存开关也被用户明确排除。共享Session仍保留候选/旧图和确认进度，这是异步借用、DEFER、隐私清理及持久ACK所需的不同所有权，不是兼容副本。

## 验证

实际正常软件、编译、独立审查及其限制集中在上述实验记录；未执行的新故障/恢复、掉电、硬件、性能和耐久证据仍明确未验证。本地结果不等同Linux Host验收或板端100 ms目标；本次不OTA、不请求云端或真实动作。

## 2026-11-16 复查续作

前述旧机制删除只覆盖Native会话/归档、结果接口及设置reader；ESP仍有旧USB目录注册，本轮按用户要求一并撤除usb.model.*、旧Binding及usb.chat GoalType，策略/端点统一当前Wi-Fi身份。属主的未知旧Goal明确UNSUPPORTED_VERSION并保留数据，避免Core保留旧Task为hold后启动无限等待；不修改原Call事实、UNKNOWN约束或自动擦除数据。

历史直接追加到既有decide事务工作副本，整组模型提议一次封装；普通及受管结果共用一次最终预算/超限摘要，本地list缩页候选的必要测量保留，已选成本不重计。历史压缩同一代持有一份规范投影，完成或失效释放；代价是摘要期间增加该投影的临时驻留，未扩大Turn槽。Native的BodyProgress只推进worker，不用全队列空闲门控；实际依赖读写仍按原durable/background规则DEFER，Session持久发布/清理/ACK不变。begin可同步交付已生成输入引用，Core外部准入校验保持。

记忆每个决定只read一次以采样当前来源和权限，初始化才格式化上下文。超过16 KiB操作缓存的大正文保留流式取舍：正常计数夹具预读20份64 KiB正文，选中一份正式读取时再读，共21次；不为节省这一次复制而额外常驻全部1.25 MiB正文。计数不是板端耗时或完整内存峰值证据。实际验证、设计自查及独立复查结果见[实验记录](../../experiments/esp32_wifi_chat/WIFI.md#2026-11-16-流程精简复查续作)。
