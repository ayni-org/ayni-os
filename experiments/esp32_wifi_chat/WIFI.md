# ESP32 触屏 Wi-Fi 语音实验

当前只保留 Wi-Fi 版，工程为 experiments/esp32_wifi_chat。USB 桌面端、UART 会话协议、旧探针及兼容构建已删除；电机能力迁入 Wi-Fi 版；串口只用于烧录和脱敏日志。日常操作在触屏完成，Mac 后台只需运行仓库外的 ../asr_tts 服务。

## 对话流程

RC003-MS按住语音键录音，松开后整段上传ASR，识别文字显示并自动提交。用户2026-11-08最终选择通用提示词及模型自主选择能力，取代此前“每次回答由应用自动朗读”的过渡方案。提示词只有通用助手身份；设备参数、用法和限制由公开SDK的Schema/能力说明提供，不写入系统提示。

同一个真实内置ToolLoop决定直接回答或使用四个固定工具按需发现、检查和调用设备。模型直接给出最终文字时只需一次请求，随后保存原Conversation/History格式并显示聊天记录；代码不再自动调用TTS或让模型确认应用播放。屏幕、扬声器、电机都是已注册能力，用户需求触发的设备工具流程由原ToolLoop处理其真实结果。显式屏幕文档本轮保留，聊天记录仍可切换查看。

上下文预算128 × 1024个估算token，包含预留1800输出token；摘要目标4096，满时仍使用原ToolLoop压缩。HTTP请求正文和投影JSON各最多768 KiB，经24字节BodyRef加4字节模式传递；普通队列32 KiB。worker按实长分配，投影后清零释放原正文；已排空的缓存可重建而不删磁盘正文，PCM只在能力调用期间分配。

128K是配置预算，不是完整长上下文硬件验收。估算token、字节长度、PSRAM峰值和Flash余量分别限制部署。旧usb.chat等私有身份仅为现有数据/取消收尾保留；未知结果不授权透明重试。

## 设置与操作

1. 触屏“设置 → Wi-Fi 名称”选择 2.4 GHz 网络，填写密码，保存并重启。首次可不填模型 Key。
2. 设置页显示 http://设备IP。同一局域网浏览器直接打开，粘贴模型 HTTPS 根地址、名称、Key 和语音 API 地址。密码/Key 不回显，留空保留。配置在当前开机期间冻结；空闲、保存完成且正常关闭后重启生效。
3. 当前语音 API 为 http://192.168.31.162:8000，模型为 https://api.deepseek.com / deepseek-flash。用户选择局域网 HTTP 无密钥语音，Web 配置无需验证码。请勿提交真实凭据。
4. 遥控器页面显式扫描、选择 RC003-MS 并连接；不会盲选首个广播。完成加密、GATT 和 ATVV 协商后才录音。主页可看转写/回答、翻页、取消及清空。

取消同步使录音及API代次失效，取消中不受理新请求；保存期间取消仍保留已生成的历史，但不发布晚到答复。扬声器的取消等待真实I²S停钟/排空，不伪造完成。清空只重置对话上下文，不擦 Core 分区。存储异常保留公开原因码，继续公开 shutdown 到排空关闭，提示复位而不自动擦卷。

## 电机迁移

原有 TA6586 驱动、PHYSICAL Call、Scene、Grant、独占 actuator、ExecutionBinding、PermitGate 和控制 worker 迁入 Wi-Fi 版；Core 不改。D0..D3 对应 GPIO38/39/40/41，PWM 上限90%，单次累计输出50–60000毫秒、5毫秒步进。PWM不代表实测转速、方向或位置。独立硬件超时和复位失能仍未验收。

主页“电机”进入准备页。启动不取得电机外设、GPIO保持低电平；驱动断电、轮子架空、断电开关就手后才点击准备。准备不代表电源测量。用户2026-11-08明确取消逐动作人工确认：准备完成后模型可直接执行受许可约束的动作，不再弹窗。准备期间所有页面顶栏有“停止”，独立原子标志不依赖队列空位；取消、断网、关闭会话和重启也请求停止。控制 worker 检查3秒心跳、许可及本地输出事实；停止锁定或初始化失败后须重启重新准备。

普通聊天和设备操作共用同一个 USER 入口、真实内置 ToolLoop、GoalType 及历史。模型直接回答时立即完成任务，应用显示聊天记录；需要设备时使用 Ayni 原有四个固定工具按需发现/检查/调用，不一次塞入所有设备Schema。屏幕、扬声器和电机都由公开 SDK 注册；提示词不指定设备或输出媒介，模型按用户需求选择能力。清空重置这一套上下文。主页显示最近3个真实阶段及模型轮次，状态页保留最近8个阶段；协议身份提议与实际Provider派发/播放/保存分别显示。进度只披露固定函数名和有效根binding_id，不包含参数正文、语音或模型内部思考。扬声器完成后若ToolLoop仍在处理工具结果，界面显示该阶段，直至最终文字和历史持久化完成。

电机接口升级到实验能力 v2，单次60秒上限由Schema、授权域、Provider与驱动共同约束；PermitGate过程上限65秒、Call期限90秒，留出有限准备时间。RMT保持1kHz及5毫秒波形，IDF 6.0.3将超过单硬件计数的有限循环拆成批次，中间有ISR重启间隙；duration_ms表示累计PWM输出时间，端到端墙钟稍长。PCNT使用溢出累加和高低限事件验证最多60000个正沿，不把“已接受”当完成。

新v2不冒充旧v1的5秒/人工确认语义；本轮已检查的现有Wi-Fi纯聊天卷保留。旧电机v1卷的完整重开不在本次兼容验收内，不自动迁移、删除授权或擦卷；可用本机旧应用备份回退。Grant容量20给旧根授权及新版本留余量。本轮电机未连接，协议与构建验证不代表60秒带电动作或停止链验收。

## 构建与部署

先检查主机内存、swap、CPU 和其他任务，明确设置正数并行度。本轮固件两任务、Native 一任务，总计不超过三。

```sh
CMAKE_BUILD_PARALLEL_LEVEL=2 CTEST_PARALLEL_LEVEL=1 bash build.sh
# 已安装 ESP-IDF 环境的 Python：
python device.py deploy
```

固定使用 ESP-IDF 6.0.3、yyjson 0.10.0、GoogleTest 1.17.0 与锁版 LittleFS。输出为 build/wifi-firmware/ayni_wifi_chat.bin。部署工具只写已验证的应用/分区表，保留 Wi-Fi、BLE 配对和 Ayni 数据；恢复备份也保留在本机忽略目录。不会自动烧录 bootloader、格式化非空卷或擦除旧数据。

## 资源与边界

- Core、公开 ABI0.54/schema9、内置 ToolLoop、Host 和 runtime 实现均未修改。Core 不等待网络、模型或播放。电机驱动在触屏准备时才分配硬件资源，GPIO38/39/40/41 启动保持低电平；本次不连接电机。
- Host 固定 16 槽共享存储队列，外部文件最多 8 个在途请求，为日志/维护留余量。Core Call 32 槽、Task Step 64；不以 Flash 是否写满判断队列容量。
- API 单 worker、3 个作业槽。总期限 70 秒包含排队和正文冷读；连接/单次上传最多 5 秒，读取按 1 秒轮询取消。无重定向、代理或请求自动重放。owner 和 API worker 均有 pthread 身份，满足 SDK FileBodies 的线程检查。
- 录音最长 30 秒、ATVV 压缩记录最多 256 KiB；整段完成后解码上传 WAV。超限、取消或异常结束整段拒绝，不提交半段录音。录音不落盘。
- speaker.speak_text接收1–1024 UTF-8字节，必须获得完整16 kHz mono signed-16 PCM WAV且最多30秒；超限明确失败、不截断。模型据能力说明选择文本与调用，应用不自动分段朗读。最终模型文字最多8 KiB，屏幕文档能力另有4 KiB限制。
- 中文字体为 GNU Unifont 16.0.04，SIL OFL 许可见 fonts/；未覆盖字符显示替代符。

## 验证记录

此前实机已确认触摸正常、触摸噪声消失、Wi-Fi/Web 保存与重启正常、RC003-MS 就绪、松键结束、识别文字可见，用户确认过模型回复与 TTS 成功。这些是当时的正常交互证据，不替代本版新的单次回答和 128K 验收。

实际存储事件捕获为 FENCED / STORAGE_REJECTED（lifecycle=7、restriction=1、durable=162），不是旧 schema 或 Flash 满的证据；共享队列预留和真实错误码已修复。用户此前只授权过一次旧 not_a_log 卷重建，没有增加后续自动擦卷权限。

本机 TTS API 测试：14 字短句完整 WAV 约 1.18–1.33 秒，音频约 3.5 秒；69 字短故事约 5.71 秒，音频 15.68 秒。均为 Mac 回环 API 耗时，不含云端模型、板端下载或 I²S 启播。

本版已通过21项Native正常检查、3项部署脚本检查、clang-format18及契约/diff检查。工具链为ESP-IDF6.0.3/GCC15.2.0、Native AppleClang21.0.0、CMake4.0.3；本地Native保留已知系统头shadow告警豁免。最终应用约2.57MB，应用分区余量38%；已写入并校验、启动到“Ayni就绪”、RC003-MS自动重连就绪，设备Web及语音readyz均HTTP200。此前自动朗读过渡版用户已确认普通回答和朗读正常、完成后能继续说话；其脱敏日志显示连续3轮各只有1次模型请求，ASR约1.18–2.20秒、模型约1.49–1.92秒、完整TTS下载约3.41–10.84秒（随回答长度变化）。这些本机忽略目录中的脱敏日志是仓库外证据，不包含语音/正文/Key。最新通用设备调用也已实机完成；一次扬声器请求记录4轮模型、模型HTTP各约1.36–1.89秒，最终历史保存约0.38秒，完整回合约30.1秒含发现/检查/持久化及播放。直接文字回答仍可一轮完成。

用户报告处理时亮度闪烁；仅启动后关闭Modem-sleep的候选仍闪。已查阅仓库外10-23的full-ps-none-touchfix-v2/RECOVERY.md：当时用户验收无闪，启动后、连接前及IP_READY均set/get PS_NONE。现按原流程补齐三点和扫描前检查，IP回调只发布pending，由UI读回成功后才发布ready，断开胜过晚到的成功；不改发射功率、背光引脚或供电。完整移植后用户仍报告闪烁；断开遥控器连接也仍闪，尚未做关闭控制器对照。最终保持原40MHz总线，采用8行DMA并增加153600B PSRAM shadow；只传变化条带、DMA确认后更新shadow，固定日志记录navigation_stripes以确认静态按钮未重写。整帧40MHz理论传输约30.7ms，状态更新通常只改少数条带。局部刷新日志显示模型交互时navigation_stripes=0；当时仍有整屏亮度闪动，不能归因为按钮重画或仅凭PS_NONE读回判为已修复。

按用户要求继续集中排查共存：实际配置已关闭BT_CTRL_MODEM_SLEEP、ESP_COEX_POWER_MANAGEMENT和PM，Wi-Fi/BT已分核。优先级切换候选已实机确认无效，已从代码移除。当前候选使用公开esp_wifi_force_wakeup_acquire，在IP_READY后持有一次额外RF引用；断开、Core关闭或重启前配对release。关闭后晚到IP不重获引用，释放失败保留failed并报告，不反复10ms重试。保留Wi-Fi/BLE与原40MHz，不改发射功率。申请日志status=0。用户随后分别确认空闲亮度稳定，以及识别、模型、播放全程稳定、执行步骤可见；这是本轮正常交互的目测验收，不是电气波形或耐久测量。待机功耗代价未测量。官方依据：[RF共存](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/api-guides/coexist.html)、[RF唤醒引用接口](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/api-reference/network/esp_wifi.html#_CPPv429esp_wifi_force_wakeup_acquirev)。

未开展新的故障、掉电、恢复、变异或耐久专项，也没有完整 Core CI/覆盖率结论。

协议参考：[Google Voice over BLE v1.0](https://wangefan.github.io/linux_kernel_driver/resources/Google_Voice_over_BLE_spec_v1.0.pdf)、[MI-AO 协议记录](https://github.com/fanxeon/mi-ao/blob/main/docs/PROTOCOL.md)、IDF 官方 nimble_central_utils。协议资料不替代 RC003-MS 实机证据。
