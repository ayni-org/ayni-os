# ESP32 触屏 Wi-Fi 实验

日常操作使用触屏和 Wi-Fi。USB 桌面应用、UART 会话协议和旧探针已删除，原电机能力迁入 Wi-Fi 版；串口仅用于烧录和脱敏诊断。Mac 后台运行独立的 ASR/TTS API。

RC003-MS 松键后整段 ASR，识别文字显示并自动提交。只有一个 Ayni 内置 ToolLoop、一套历史和一个输入入口，系统提示保持通用；模型决定直接回答还是通过当前请求预置的完整工具定义调用已注册的屏幕、扬声器、电机能力。默认最终文字显示在聊天记录中，不自动调用 TTS；模型调用的屏幕文档本轮保持，语音按用户请求由模型调用。没有自写工具循环，Core不改。

上下文预算是128 × 1024个估算token，满时允许原ToolLoop摘要；输出预留1800、摘要目标4096。768 KiB是模型正文/JSON的独立字节上限，不是上下文预算。大请求以24字节BodyRef加4字节模式传递，普通消息队列32 KiB。

硬件为ESP32-S3 N16R8、ST7789、FT6336U、MAX98357A及RC003-MS。电机GPIO38/39/40/41沿用PHYSICAL/PermitGate，启动保持低电平，触屏准备后模型可直接操作，不逐次人工确认；v2单次累计PWM输出上限60秒。电机页另有两路独立的50%/5秒自检按钮，复用公开SDK的Rules与原许可执行链，不请求模型。准备只初始化驱动资源，不代表电源测量。两路50%/5秒带电自检及模型直接驱动已由用户确认正常；完整128K峰值、60秒动作和独立停止链未验收。

先检查资源并设置正整数CMAKE_BUILD_PARALLEL_LEVEL、CTEST_PARALLEL_LEVEL，再运行 ./build.sh。固定ESP-IDF6.0.3；device.py deploy只写应用/分区表，保留配置、配对和数据，不自动擦卷。详见[Wi-Fi说明](WIFI.md)。历史usb.chat等私有身份只保留数据读取/旧任务取消用途，不代表仍有USB运行版本。

## 2026-11-11 共用模型协议

模型API/工具参数/SSE由Ayni::ModelProtocol统一处理，Native使用同一实现。Web设置和触屏“设置→模型选项”可选Chat Completions或Responses、生成时预览、Chat输出预算字段；保存后正常关闭并重启。旧NVS配置仍按Chat/非流式读取，Key不会回显。预览标为暂定文字，取消/换轮/清空立即清理；完整成功响应才交给原ToolLoop，语音仍由模型调用扬声器能力。

完整Schema和说明来自当前Task可见目录，模型只得到一个通用ayni_call_capability的完整分支工具，不需list/inspect往返。每请求一份tools，不写入历史；invoke/start/read/wait/cancel/close与原授权/许可语义保持。Responses续传包含有序Items和encrypted reasoning，绑定API/模型/目的地；更换选择时明确拒绝旧续传，用户显式建立新会话，绝不自动擦卷。主请求/工具结果/摘要预算使用真实API投影，服务usage独立报告。

软件验证使用本地合成HTTP/SSE与公开SDK，兼容决定见[ADR-0098](../../docs/adr/0098-shared-model-protocol-and-preloaded-tools.md)。当前屏幕设置预览见[截图](screenshots/model-settings-preview.png)。本轮只编译与软件验证，没有烧录、串口、线上模型或电机运行，未完成128K峰值、60秒动作或硬件停止链验收。

## 2026-11-12 共享交互会话

聊天复用`runtime/conversation::Session`，输入、Goal、终态、文件保存、Core发布/ACK和清空由同一实现管理。显式discard模式允许取消后继续，发布中的回答保留已确认历史，界面只抑制晚到展示。启动先取消旧实验工作，合成容量诊断使用独立类型、不进入真实历史。原实验History、BodyCache和ValueQueue已移除；正文缓存2 MiB预算及清退、8/16文件请求准入和不可复制命令客户端均使用共享接口。

按用户决定不迁移旧独立历史文件；新锚点对应的新会话正常持久和重开，Core任务/调用证据与配置保留，无启动自动擦卷。本轮未烧录或OTA，完整跨存储中断恢复及硬件内存峰值未验证，独立审查由用户后续安排。见[ADR-0100](../../docs/adr/0100-shared-interactive-runtime.md)。
