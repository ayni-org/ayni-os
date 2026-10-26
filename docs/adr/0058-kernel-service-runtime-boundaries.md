# ADR-0058：Kernel 私有状态、服务与运行循环边界

- **状态**：已接受，P3 第一项实施中
- **日期**：2026-10-26
- **关联**：P3 F01/F14/F08；ADR-0051、0053、0057；C-POLICY-001、C-LIFE-003、C-HOST-001

## 背景

新增记忆实体之前，需要让 Kernel 的组合职责与领域编排分开。P2 基线的 ABI 入口直接读取或修改公开 Kernel 成员，WorkModel 同时承担回调标记，目录级约束只识别一级目录。已有持久屏障、调度前沿与回调前后授权检查必须保持。

## 决策

1. **私有状态与 facade。** `ayni_kernel` 全部字段私有。ABI 边界保留参数解码及生命周期门控，随后通过 const 查询视图或具体注册、会话、资源健康、观察和事件操作访问组合根；不提供可变 WorkModel/Catalogs getter。持久变更仍走统一意图和 journal。已有白盒测试只经 `KernelTestProbe` 访问字段。不为每个 C 函数建立另一层同签名转发器。
2. **分区。** `kernel/runtime` 负责生命周期、step、启动/重载、日志提交、存储维护与 ingress；`kernel/service` 负责 model、intent、decision、call、execution、command 与持久实体的编码/重放。`WorkModel` 移到 service/model，意图入口不再作为其定义的依赖。原有领域表仍归 work/authority/resources/knowledge，分区不改变其属主。
3. **共享端口。** 完成槽、Provider/执行端邮箱、attempt 分配和 ingress 锁机制在低层 `kernel/ports`。实例仍由 Kernel 拥有；service 通过原有有界端口借用完成邮箱，不反向依赖 runtime。config 只保留目录与配置类型；依赖具体 command 表的容量计算归 service/model。替换 Host/Provider 不要求改领域状态或移动其规则。
4. **可执行的依赖规则。** `architecture.json` 保留 service 内部各模块的边，runtime 可以引用 service，反向禁止。检查器采用已声明目录的最长前缀匹配，绝对式 include 与解析后的相对 include 使用同一规则；嵌套目录参与原有环检测。
5. **StepContext。** call-local 上下文显式携带固定 step 开始时间、正的 item/time 预算、借用时钟与报告截止时间。调度和 READY 前沿仍固定在开始时；授权/派发使用新鲜时钟，回调后重查保持。boot/维护仍按原有一次推进计项；耗尽预算后仍投影 more_ready 和下次截止。不扩大每步实际工作、不新增时钟采样或等待。
6. **CallbackGate。** Kernel 拥有单一 gate，WorkModel 借用；作用域守卫只包住调用方回调，嵌套调用退出恢复外层标记。ABI 门控读取同一个 gate；任何线程的 post 入口仍只读不可变端口并使用自身锁，不读 owner-thread gate/lifecycle。原 WorkModel 裸布尔删除，相关独立测试 fixture 自持 gate。
7. **兼容。** 行为、公共 ABI 0.39、schema 2 和各记录编码不变。内部 gate 借用可能改变 arena 大小，按内存需求 API 量测并在 PR 报告；调用方仍由 API 获取所需内存，不以内部对象布局作为 ABI。

## 后果

- API 无法直接修改 Kernel 字段；新增持久实体有清楚的 service 与领域属主位置。
- const 内部视图只供单 owner 线程同步查询，不保证跨 mutation 的快照，也不暴露到 C ABI。
- 配置操作保留现有解码位置与错误优先级；F10 的新注册入口规则随记忆落地，已有路径不因移动目录改变验证行为。
- 大部分 diff 是文件移动与 include 修正；审查分别看机械移动、facade、step/回调状态和检查器规则。

## 备选方案

- service 包含 runtime 的邮箱：会破坏单向边界；共享机制下移 ports。
- 全部 API 设为友元或暴露可变表 getter：无法收紧表示访问，不采用。
- 为保持 arena 字节数复制回调状态：引入双份事实，不采用。

## 验证

实现后的相关正常功能、独立审查与 CI 结果在 PR 记录。此结构改动不代表新增故障/恢复专项、完整 E1–E4 或硬件验收。
