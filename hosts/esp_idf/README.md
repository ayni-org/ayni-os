# ESP-IDF Host

`hosts/esp_idf/`提供ESP32-S3 / ESP-IDF 6.0.3的Host源码组件，限定默认legacy双核内核（`CONFIG_FREERTOS_SMP=n`），可选上游SMP配置在构建时明确拒绝；当前编译与文件端口RAM介质验证通过；FreeRTOS实际运行、Flash耐久和ESP实机尚未验证。Core、ToolLoop和调度机制仍使用通用SDK原实现。

独立ESP-IDF工程将此目录加入`EXTRA_COMPONENT_DIRS`，组件依赖`esp_idf`；组件内部调用同一`cmake/AyniSDK.cmake`，选择HostStorage。LittleFS通过`idf_component.yml`固定`joltwallet/littlefs==1.23.1`，部署提交自己的`dependencies.lock`。不需要引入根Native工程、SQLite、CURL或测试fixture。

## 所有权和线程

Host在Kernel owner任务中构造，必须放在内部RAM，生命周期覆盖Kernel及所有Producer；拥有一个内部静态互斥量，非ISR任务可投递完成、唤醒和进入ingress。owner任务的通知index0专用于Host唤醒，`wait_for_wake`按微秒超时向上取整到tick，多次唤醒合并。单调时钟为`esp_timer`；默认未提供UTC；构造时可注入公开`UtcSource`，由实际校时服务明确报告trust，回调只在owner调用且不得阻塞。独立程序注入固定可信fixture时间，不作为设备校时服务的证据。

`open_storage(label, capacity, initialize_blank=false)`在create之前调用，会阻塞扫描介质；成功后table提供完整八存储端口。容量为1～16个排队请求，不含当前worker处理的一项；满时不保留请求，accepted恰好完成一次。借用的Kernel缓冲在完成投递后不再访问。文件I/O只在配置期或单个worker任务执行，`step`只入队，free_bytes只读缓存。

worker使用24 KiB内部静态任务栈、内部队列及文件缓存。`storage_memory_requirements`报告这些Storage结构的总字节/对齐；Host自身、Kernel arena及策略区域另行计账。Core/策略是否放PSRAM由runtime决定；闪存操作期间的cache/PSRAM可用性必须由具体sdkconfig及实际运行确认，不把Core放进硬件停止链。

Kernel STOPPED后、destroy之前调用`close_storage`：停止接收、完成已受理队列，worker发出完成信号并停放；owner等待其确实离开运行状态后删除任务，再关闭文件和释放内部内存。保留退出时的栈水位，`worker_stack_free_bytes`在close后仍可读取。不能只凭退出信号释放仍在另一CPU运行的静态栈。该确认依据锁版legacy内核的`eTaskGetState`先检查两CPU运行任务；上游SMP内核的查询顺序不同，不能沿用此证明。关闭前不允许其他任务并发调用Host的配置/关闭函数；外部不能恢复或删除该worker。

## 分区和文件

专用data/littlefs分区由此Host独占。平台内有两个有锁、有界的卷租约槽，拒绝第二个Ayni Host及已经存在的VFS挂载。整个租约期间，部署不能从外部VFS、raw API或其他Flash写入者访问该分区；Host不宣称能阻止任意外部直接改写Flash。生命周期与上限由本模块封装，Core无全局注册表。

默认只挂载、不格式化。显式允许初始化后，也仅在持有租约且全分区字节均为擦除态时格式化；未知非空白文件系统拒绝并保留。分区必须非加密、按4096字节块对齐且至少4块。查找、挂载、格式与打开日志失败均清理已取得的资源；错误按状态保留errno或raw LittleFS原因；空白扫描的读取错误不会被之前挂载失败的原因覆盖。

使用依赖公开的raw API及自有固定缓存，直接映射ESP分区同步读/写/擦除；内部bounce缓冲避免将PSRAM作为Flash操作的直接缓冲。没有同时挂载同一分区的VFS实例。选择raw API是因为VFS拒绝重命名已打开的源/目标，原生LittleFS允许该替换；raw rename会使原源句柄脱离目录，适配器在成功替换后重新打开目标，并保持上层源令牌不变，使后续追加/同步仍更新目标文件。原生目录修改报错可能发生于已经提交后的回读/清理，因此该错误或重开失败均将卷标为不可用，拒绝所有后续I/O和容量确认，只允许关闭清理及正常Host重开。不可用卷的close丢弃自供缓存，不触发LittleFS隐式同步；unmount只解除卷内存，避免按不确定关联重新提交。

`lfs_file_sync`确认文件；LittleFS目录修改已提交元数据并同步分区后才返回，因此namespace sync不再发起额外I/O。这里的同步实现依据锁版源码，不代表已完成实际掉电验证。空闲每秒及请求完成后刷新实际分配块计数，扣除两个元数据保留块；Host再扣未完成写入预留。无法建立样本时报STORAGE_FAILED并继续采样。普通Core提交、双槽快照与轮转仍由共用引擎处理，不加入另一套恢复策略。

共用日志引擎重新确认继承字节时，用独立只读token提供原字节，向原写token顺序重写，再确认file/namespace同步；不省略重写或持久化检查。LittleFiles打开同名新读token前，仅在已有写token含脏数据/未发布长度时同步它，保证读视图包含当前字节。这样避免在同一个LittleFS token上反复交替读写，导致每个4 KiB块都复制剩余文件尾部；Native仍保持原同步顺序和错误处理。

## 验证边界

`tests/portability/esp_files/`把生产LittleFiles和真实锁版LittleFS运行于RAM NOR介质，检查正常读写、打开文件替换、租约、未知介质保留及日志正常重开；这只验证文件适配算法，不验证FreeRTOS调度、ESP驱动、实际Flash或电气条件。独立平台共同场景与N16R8配置在后续包交付；本包没有烧录任何设备。

2026-11-01的普通重开回归增加同名读token视图与192 KiB日志；重开时实际program字节由4,837,312降至197,056，内容及后续追加保持一致。原交付的实机限制仍是历史边界；后续本机独立USB屏幕/扬声器实验的正常实机证据单独记录在实验目录，不据此声明通用硬件、掉电或耐久验收完成。
