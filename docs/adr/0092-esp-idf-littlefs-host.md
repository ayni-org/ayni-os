# ADR-0092：FreeRTOS异步Host与专用LittleFS卷

- **状态**：已接受；实现、审查与实际验证见 [#121](https://github.com/RobotSe7en/ayni/pull/121)。
- **日期**：2026-10-31。
- **关联**：ADR-0006、ADR-0015、ADR-0089、ADR-0091；[独立接入计划](../architecture/independent-consumption-plan.md)。

## 背景

既有ESP Host只提供时钟、唤醒及ingress锁，不能启动现行Core存储协议。用户已选择专用LittleFS分区；新Host必须在有界step之外提供完整存储端口、正常关闭及真实完成语义。

## 决策

平台代码留在`hosts/esp_idf/`，通过同一源码SDK链接Core及HostStorage。FreeRTOS单worker服务有界队列，owner只入队和读取容量缓存；Core继续拥有逻辑调度、ToolLoop、持久提交与恢复。Native/ESP共享请求执行、epoch及预留计算，不复制文件机制。Host启动前分配内部Storage，关闭时排空队列、确认worker停放并删除任务，随后关闭文件，再允许Kernel销毁。静态任务回收限定IDF6.0.3默认legacy双核内核（CONFIG_FREERTOS_SMP=n），其状态查询先排除仍在任一CPU运行的任务；可选上游SMP内核的状态顺序不同，构建明确拒绝，需以后另行实现和验证join。

LittleFS固定joltwallet组件1.23.1，使用其公开raw API，固定内部缓存和同步ESP分区回调。VFS不支持已打开源/目标的rename，而共用引擎要求保持文件令牌跨替换存活，因此不使用VFS文件包装或复制日志。raw rename本身会使源句柄脱离目录；适配器在替换后重开目标，保留对外源令牌的持久写入关联。raw目录错误也可能晚于提交；若目录修改报错或重开失败，卷拒绝后续I/O及容量确认，保留关闭/正常重开；不可用卷的清理丢弃自供文件缓存并unmount，不再执行close隐式同步，不向脱离目录的句柄继续写入并报成功。依据见[锁版VFS实现](https://github.com/joltwallet/esp_littlefs/blob/v1.23.1/src/esp_littlefs.c)、[公开raw入口](https://github.com/joltwallet/esp_littlefs/blob/v1.23.1/include/lfs.h)及其[正常交换测试](https://github.com/joltwallet/esp_littlefs/blob/v1.23.1/test/test_littlefs_raw_api.c)。目录操作的同步确认来自LittleFS提交与ESP同步分区调用；硬件耐久仍待独立证据。

卷租约在平台模块内有锁、有界，排除其他Ayni实例并拒绝现有VFS挂载；部署须保证分区不另交外部writer。默认不格式化，显式初始化也仅处理全空白擦除态。未知非空白介质保留并拒绝。公开源码Host说明线程、生命周期、错误原因、容量与停止边界；ABI0.53、Core schema8及Host日志AYNL v1不变。

## 后果

独立ESP工程选择Host组件即可使用同一AgentOS机制；具体PSRAM布局、可信UTC、owner任务及应用由runtime组装。静态内部worker栈为24 KiB，实际余量仍须测量，不能把编译当作N16R8可运行或25%栈余量已通过。全芯片Flash驱动计数和共同正常场景随后续独立工程记录，不以逻辑日志字节代替物理写入量。

## 备选方案

不在Core引入FreeRTOS/LittleFS分支，不复制Native日志，不在VFS限制下关闭重开令牌来改变Native跨替换锁保证。未增加通用OS框架；只有真实需要共享的请求执行归入HostStorage。平台卷租约不进入Core，也不用于能力注册。

## 验证

锁定ESP-IDF6.0.3编译及生产文件适配器在真实LittleFS/RAM NOR介质上的正常场景通过；独立审查、修复及CI结果集中在 [#121](https://github.com/RobotSe7en/ayni/pull/121)。RAM介质不证明FreeRTOS实际运行、ESP Flash、掉电或耐久；本包未烧录、实机、使用付费模型或启动新故障专项。
