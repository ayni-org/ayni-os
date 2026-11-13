# ESP32 触屏 Wi-Fi 实验

日常操作使用触屏和 Wi-Fi。USB 桌面应用、UART 会话协议和旧探针已删除，原电机能力迁入 Wi-Fi 版；串口仅用于烧录和脱敏诊断。Mac 后台运行独立的 ASR/TTS API。

RC003-MS 松键后整段 ASR，识别文字显示并自动提交。只有一个 Ayni 内置 ToolLoop、一套历史和一个输入入口，系统提示保持通用；模型决定直接回答还是通过当前请求预置的完整工具定义调用已注册的屏幕、扬声器、电机能力。首页使用九个应用图标；“语音回复”按钮开启后，每轮最终文字在保存后通过原扬声器能力朗读，开关跨重启保留，旧设备默认关闭。模型显式语音调用始终可用，本轮已显式发声也仍朗读最终文字。没有自写工具循环，Core不改。

上下文预算是128 × 1024个估算token，满时允许原ToolLoop摘要；输出预留1800、摘要目标4096。768 KiB是模型正文/JSON的独立字节上限，不是上下文预算。大请求以24字节BodyRef加4字节模式传递，普通消息队列32 KiB。

自动朗读先将Markdown转换为朗读文本，再按句末／词语边界分段；加粗、标题、列表及代码围栏等格式不传给TTS，正文和代码中的字面符号保留。链接保留标题，图片保留替代文字，表格按行及单元格分隔；屏幕和历史仍保留原文。显式speaker调用保持调用者提供的文本。解析器MD4C 0.5.2以SHA256锁定，仅被可选呈现与其测试链接，MIT许可见[声明](third_party/md4c-LICENSE.md)。

硬件为ESP32-S3 N16R8、ST7789、FT6336U、MAX98357A及RC003-MS。电机GPIO38/39/40/41沿用PHYSICAL/PermitGate，启动保持低电平，自动初始化资源、会话就绪后默认启用，不逐次人工确认；v2单次累计PWM输出上限60秒。电机页提供普通停用／启用、两路50%/5秒自检及独立停止；停止锁存至重启。初始化不代表电源测量或实际转动。历史两路自检及模型驱动已有用户确认，本版实际轮轴方向、完整128K峰值、60秒动作和独立停止链未验收。

当前界面静态预览：[应用首页](screenshots/app-launcher-preview.png)、[语音开关](screenshots/voice-reply-preview.png)、[电机控制](screenshots/motor-controls-preview.png)。三者由同一固件绘图代码生成，非实机照片；本轮决定及验证见[ADR-0101](../../docs/adr/0101-touch-apps-and-automatic-reply-voice.md)和[实验记录](WIFI.md#2026-11-13-应用图标自动语音与电机默认启用)。下述日期段落保留当时的行为与验证边界。

配置页采用统一卡片与分组：[网络](screenshots/network-settings-preview.png)、[模型与语音](screenshots/endpoint-settings-preview.png)、[模型选项](screenshots/model-options-preview.png)。示例值不来自真实凭据。完成固件字体生成后，在本目录使用`cmake -S tests/wifi/preview -B build/screen-preview`配置Native预览，按资源预算编译；`build/screen-preview/ayni_screen_preview output.ppm home|voice|motor|network|model|options`输出同一绘图代码的PPM，Native预览单独依赖系统zlib。

先检查资源并设置正整数CMAKE_BUILD_PARALLEL_LEVEL、CTEST_PARALLEL_LEVEL，再运行 ./build.sh。固定ESP-IDF6.0.3；device.py deploy只写应用/分区表，保留配置、配对和数据，不自动擦卷。详见[Wi-Fi说明](WIFI.md)。历史usb.chat等私有身份只保留数据读取/旧任务取消用途，不代表仍有USB运行版本。

## 2026-11-11 共用模型协议

模型API/工具参数/SSE由Ayni::ModelProtocol统一处理，Native使用同一实现。Web设置和触屏“设置→模型选项”可选Chat Completions或Responses、生成时预览、Chat输出预算字段；保存后正常关闭并重启。旧NVS配置仍按Chat/非流式读取，Key不会回显。预览标为暂定文字，取消/换轮/清空立即清理；完整成功响应才交给原ToolLoop，语音仍由模型调用扬声器能力。

完整Schema和说明来自当前Task可见目录，模型只得到一个通用ayni_call_capability的完整分支工具，不需list/inspect往返。每请求一份tools，不写入历史；invoke/start/read/wait/cancel/close与原授权/许可语义保持。Responses续传包含有序Items和encrypted reasoning，绑定API/模型/目的地；更换选择时明确拒绝旧续传，用户显式建立新会话，绝不自动擦卷。主请求/工具结果/摘要预算使用真实API投影，服务usage独立报告。

软件验证使用本地合成HTTP/SSE与公开SDK，兼容决定见[ADR-0098](../../docs/adr/0098-shared-model-protocol-and-preloaded-tools.md)。当前屏幕设置预览见[截图](screenshots/model-settings-preview.png)。本轮只编译与软件验证，没有烧录、串口、线上模型或电机运行，未完成128K峰值、60秒动作或硬件停止链验收。

## 2026-11-12 共享交互会话

聊天复用`runtime/conversation::Session`，输入、Goal、终态、文件保存、Core发布/ACK和清空由同一实现管理。显式discard模式允许取消后继续，发布中的回答保留已确认历史，界面只抑制晚到展示。启动先取消旧实验工作，合成容量诊断使用独立类型、不进入真实历史。原实验History、BodyCache和ValueQueue已移除；正文缓存2 MiB预算及清退、8/16文件请求准入和不可复制命令客户端均使用共享接口。

按用户决定不迁移旧独立历史文件；新锚点对应的新会话正常持久和重开，Core任务/调用证据与配置保留，无启动自动擦卷。本轮未烧录或OTA，完整跨存储中断恢复及硬件内存峰值未验证，独立审查由用户后续安排。见[ADR-0100](../../docs/adr/0100-shared-interactive-runtime.md)。
