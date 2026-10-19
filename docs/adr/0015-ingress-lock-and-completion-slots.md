# ADR-0015：入口临界区、有界事件队列与完成槽

- **状态**：已接受（`MA-003` 部分在 M1 已实现）
- **日期**：2026-10-19
- **关联**：计划 §3.2「所有权/异步/线程」、§4.2；`MA-003`、`MA-005`、`MA-007`；E1 步骤 10；ADR-0006、ADR-0014

## 背景

Kernel 状态只由所有者线程修改，但事件（`ayni_post_event`）与异步结果（`ayni_completion_post`）来自 Host 的其他线程，例如网络、存储 I/O 与按键任务。计划 §4.2 要求事件入口有界、遥测按声明合并或丢旧、控制类完成使用受理前预留的槽，并且槽被重用后，迟到的完成不能被当作新尝试的结果。

生产者线程与所有者线程之间需要互斥。Core 的标准头文件白名单（ADR-0014，`tools/architecture.json` 的 `core_std_headers`）不含 `<atomic>`、`<mutex>`、`<thread>`；同步原语在 ESP32 上来自 FreeRTOS，在 Native 上来自 C++ 标准库，Core 不能直接依赖任何一方。因此要决定互斥由谁提供、队列满时怎样处理，以及完成槽怎样排除重复与迟到的结果。

## 决策

1. **Host 提供入口临界区。** 自 ABI 0.3 起，`ayni_host_v1` 追加 `wake`、`ingress_lock`、`ingress_unlock`，三者都是必需端口。锁可以从任意非 ISR 线程调用，不可重入。Kernel 每次持锁只做一步有界操作：一次事件队列操作（每个已入队条目最多读一次，移动的下标少于 `ingress_capacity` 个），或一个完成槽的状态变更。持锁期间不调用任何其他 Host 函数；`wake` 在解锁之后调用。Native Host 用 `std::mutex`；ESP-IDF Host 用静态分配的 FreeRTOS 互斥量。ISR 仍不调用 Kernel API，而是经 C-HOST-008 的桥接。
2. **事件入口是有界队列。** 容量和单个事件正文的上限由 `ayni_config_v1` 的 `ingress_capacity` 与 `event_body_max_bytes` 给出，存储从调用方提供的 Kernel 内存块中划出。队列满时从新到旧扫描一遍，按以下顺序决定：
   - 可丢弃事件（遥测类）覆盖同一订阅、同一非零合并键的最新一条，该条的 `replaced` 加一；
   - 没有可覆盖的，就逐出最旧的一条可丢弃事件；
   - 仍无空间，或新事件不可丢弃，返回 `AYNI_BUSY`。

   覆盖与逐出都计数，消费者从订阅序号的缺口看到 gap。合并键 0 表示从不覆盖。只覆盖同一流的最新一条，因此同一订阅的事件仍按投递顺序交付。
3. **完成槽在调用 Host 之前预留。** 槽数由 `completion_slot_count` 给出。Kernel 先取空槽，以 `(boot_epoch, request_id)` 命名这次尝试（`request_id` 在一次启动内递增），再调用端口；没有空槽时不调用端口，调用方得到 `AYNI_BUSY`。槽的状态 free → reserved → posted → free 全部在入口临界区内变更。

   `ayni_completion_post` 只接受槽当前等待的那次尝试；重复或迟到的完成返回 `AYNI_INVALID_STATE` 并计数。端口同步返回 `REJECTED_NOT_RETAINED` 时释放槽、不运行处理函数。未定义的返回值，或在拒绝之前就已到达的完成，计为 Host 违约，请求按在途处理，结果照常应用。处理函数在所有者线程、锁外运行，每个在途请求恰好一次。
4. **完成不经事件队列。** 事件队列满不影响完成的写入。进入 `STOPPED` 要求在途槽数为 0（C-HOST-018），这项判断随 `MA-005` 的生命周期实现。

## 后果

- 每个 Host 多实现三个函数。单线程 Host 可以让锁只做断言；模拟 Host 用真实互斥量，并把同一线程重复加锁变成立即中止。
- Core 不含原子操作，同一份 Core 在 Native、ESP-IDF 与模拟 Host 上行为一致，内存可见性由 Host 的锁保证。
- 生产者与所有者竞争同一把锁。最长的一步是向满队列投递：读一遍全部条目头，逐出时再移动少于 `ingress_capacity` 个 16 位下标，最后复制最多 `event_body_max_bytes` 字节。持锁时间与容量成正比，在 M1 压测与 M2 ESP32 测量中报告实测值。
- FreeRTOS 互斥量带优先级继承；Native 依赖操作系统调度。高优先级生产者的等待以上述一步为上限。
- 被覆盖或逐出的遥测不可恢复，只留下计数与序号缺口；不可丢弃事件只会被拒绝，不会被覆盖。
- ABI 升至 0.3：`ayni_config_v1` 与 `ayni_host_v1` 追加字段。按 C-VER-003，配置中未覆盖的容量取默认值；Host 表缺少新的必需端口时 `ayni_create` 返回 `AYNI_INVALID_ARGUMENT`。

## 备选方案

- **Core 内用无锁原子队列**：需要 `<atomic>`；覆盖与逐出要修改已入队的条目，多生产者的无锁实现复杂，难以在 ESP32 上验证。否决。
- **每个生产者一条单生产者单消费者队列**：生产者数量须事先固定，合并只能在单条队列内进行，跨队列的总容量与顺序难以界定。否决。
- **由各 Host 自行实现队列，Kernel 只拉取**：合并、逐出、计数与完成槽的身份校验会在每个 Host 重复实现，违背计划 §4.2「可靠性逻辑留在可移植 Core」。否决。
- **完成也进入事件队列**：队列满时存储确认会被拒绝或挤占普通事件，违背 C-LIMIT-008。否决。

## 验证

- `tests/unit/event_queue_test.cpp`（`IngressQueue.*`）：1 万事件按序无丢失；满队列对不可丢弃事件返回 `BUSY`；同一订阅、同一合并键覆盖最新一条；逐出最旧的可丢弃事件；合并键 0 不覆盖；遥测洪泛下每个订阅仍按序且计数守恒；四个生产者线程并发入队无丢失。
- `tests/unit/completion_pool_test.cpp`（`CompletionPoolTest.*`、`CompletionWake.*`、`KernelCompletions.*`）：槽耗尽时返回 `BUSY` 且不调用端口；结果只应用一次；槽重用后迟到的完成被忽略，另一线程持续投递旧尝试时也是如此；上一次启动的完成被忽略；拒绝时释放槽；未定义返回值与 `outcome` 0 计为 Host 违约；`wake` 时结果已可应用；持锁期间没有 Host 调用；并发完成各应用一次；事件队列满时完成照常写入。
- `tests/unit/native_host_test.cpp`、`tests/unit/sim_host_test.cpp`：Native Host 的唤醒合并与互斥；模拟 Host 记录持锁期间的 Host 调用，同一线程重复加锁时中止（死亡测试）。
- 变异检查（本地，未纳入 CI）：把槽的预留移到锁外，TSan 在 `CompletionPoolTest.stale_posts_racing_slot_reuse_are_ignored` 中报告数据竞争；把 `wake` 移到写入结果之前，`CompletionWake.result_is_applicable_when_the_owner_is_woken` 失败。
- 以上测试在本地以 GCC Debug、Release、ASan/UBSan、TSan 与 Clang Debug 运行通过，CI 的 `native` 任务运行同样的组合。
- ESP-IDF Host 只经 CI 的 ESP-IDF 构建编译，尚未在硬件上运行；M2 复核持锁时间与唤醒延迟。
