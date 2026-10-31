# ADR-0093：Host外层文件值与共同异步队列

- **状态**：已接受；实现、审查及实际验证见 [#122](https://github.com/RobotSe7en/ayni/pull/122)。
- **日期**：2026-10-31。
- **关联**：ADR-0060、ADR-0089、ADR-0091、ADR-0092；[外层文件值契约](../../contracts/host/file_values.md)、[独立接入计划](../architecture/independent-consumption-plan.md)。

## 背景

Core持久化上下文锚点，但对话正文由外层存储管理。上下文发布/删除ACK必须跟随实际持久写入；只有Core日志不能验收该闭环。已有Native应用存储依赖具体文件/SQLite实现，新的独立ESP/Native共同应用需要真实、有限的外层持久通道。

## 决策

HostStorage增加按owner/object隔离的文件值机制，Native与ESP公开同一C++异步端口。文件值与Core日志请求使用同一有界worker队列和预留计算，不在step做文件I/O，不另起平台线程/文件系统。队列项显式区分Core请求与外层请求，先刷新空间缓存，再完成并唤醒owner。

外层文件固定AYFV格式1，保存key、generation、长度及CRC；payload保持调用方编码。生成文件名并为外层值保留value-前缀，FileLog在取得命名空间前拒绝该前缀以避免文件别名；当前Host主日志仍固定commit_log，没有受影响的既有消费者。最大65536字节；写入以previous/new generation作CAS，通过完整候选同步、替换和目录确认后才成功。同一已写版本的完全相同字节可重试，但重新写入同步候选，不将继承的可读缓存当作耐久证明。Host返回明确错误或未知，不自动ACK Core上下文。回调在worker执行，不能抛出/重入，context持续到Host关闭join。

文件帧与值机制使用同一Host CRC/小端及完整读取实现；Core仍使用自己的领域codec。ABI0.53、Core schema8、策略codec4和SQLite版本2均不变，新格式与既有应用数据库独立。未知文件值版本不加入reader/迁移器，保留并拒绝。

## 后果

共同应用可原样消费Core、ToolLoop及上下文机制；runtime只选择真实Host和存储端口。Conversation编码、上下文版本、隐私决策及缓冲生命周期属于其现有属主，Host只确认文件事实。新增上限和预留计入实际容量；ESP Storage字节查询自动包含加大的队列项。实际Flash写入量与栈仍待平台执行，正常文件检查不替代掉电、物理擦除或耐久证据。

## 备选方案

不伪造上下文ACK，不把正文改存Core领域记录，不为ESP复制Native数据库，也不额外挂载同一分区或为每个外层值启动线程。没有增加任意文件名/任意I/O回调接口；只增加当前两端共同应用确实需要的有界版本值。

## 验证

Native异步写入、CAS/重试、读容量、key隔离、关闭排空及最小正常重开验证结果，以及LittleFS/RAM介质和ESP编译边界集中在 [#122](https://github.com/RobotSe7en/ayni/pull/122)。新增变异、断电及恢复专项仍暂停；实机未执行。
