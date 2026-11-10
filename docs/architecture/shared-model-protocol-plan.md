# 共用模型协议实施计划

- **范围**：用户2026-11-10要求ESP32复用Ayni模型接入，支持OpenAI兼容Chat Completions、Responses及流式，协议行为遵循OpenAI；同时完成已要求的完整工具预置。允许修改Ayni，不另写实验工具循环。
- **状态**：2026-11-11阶段交接（任务自11-10开始）。用户准备关闭Mac，要求阶段提交推送，由远程机器继续；明确不用烧录。完整功能尚未完成。
- **分支**：feat/esp32-preloaded-tools；已快进到用户合并#130/#131后的main（028bc45）。后续继续这个分支，不从旧WIP分支恢复。

## 接手先读

本文件、AGENTS.md、docs/workflow.md、reports/core/m1-progress.md交接状态、docs/ayni_v1.0.md、ADR-0085/0086/0095/0096/0097。保留用户已经决定的产品行为：

- 一套真正的内置ToolLoop/历史/输入，不另写循环。普通最终文字一轮请求，工具操作由模型决定，最终文字显示屏幕；语音同样由模型调用扬声器能力。
- 完整工具定义通过标准tools参数随当前请求传递，取消模型侧按需发现；不把定义逐轮追加到历史。每请求一份定义，跨请求重传不等于上下文累积。
- 上下文128×1024估算token、输出预留1800、摘要目标4096、单工具结果预算4096；满时允许额外摘要。
- OpenAI原生协议行为；不把Responses改名当Chat请求，不猜测或偷偷改写工具参数。模型提议仍须通过原Core授权/Scene/PermitGate。
- 本轮只做软件、提交推送。不要烧录、连接/复位串口、运行电机、擦卷、调用付费线上模型或处理PR合并。PR由用户处理。

## 当前代码已经做了什么

1. 新可选SDK组件Ayni::ModelProtocol（providers/model_protocol）。从Native openai_compat搬出请求编码、回答/工具参数解析、usage处理和有界JSON校验；不依赖curl、ESP-IDF、线程或Key。
2. Native的protocol.h只适配Config；ESP的api.cpp复用共享write_request，model_codec.cpp只把共享Answer复制到实验消息队列，重复JSON/工具解析已删除。实际接口仍只有原先非流式Chat Completions。
3. AyniYyjson.cmake统一同一固定版本源码导入，ESP使用source-only方式，实验不再维护第二份FetchContent。共享组件声明静态链接需要的Core公共编解码符号依赖；没有包含Core私有头。
4. Buffer显式处理malloc失败，释放前擦除；DOM先于解析池销毁。ESP可用有界Writer，Native保留string包装。原协议错误分类、原始参数字面值和UNKNOWN不重试规则保持。
5. model_messages.hpp加入尚未接入实际消费者的基础编码：request-only工具定义封套、绑定assistant/calls帧的continuation、显式ReplyEncoding。旧帧继续读取，只有有continuation时写assistant.v2/calls.v2；Chat默认拒绝丢弃continuation。已补独立正常测试。

以下全部仍待完成：API类型/stream配置、Responses请求和回答、SSE、流式预览、continuation结果存储和ToolLoop消费、正式完整工具预置、对应真实投影预算、ESP Web设置及端到端矩阵。不要将当前阶段提交描述为“已支持流式/Responses”。

## 边界

1. 从Native Provider抽出无HTTP/线程/凭据的可选SDK组件ModelProtocol。Native保留libcurl传输、取消和发布；ESP保留esp_http_client与本机任务，移除重复请求/模型输出解析。Core不依赖JSON、HTTP或特定模型。
2. 两套原生wire格式分别投影：Chat Completions的messages、嵌套function工具及tool_call_id；Responses的input Items、平铺function工具及call_id/function_call_output。不把Responses当作更换URL的Chat接口。
3. SSE按字节增量处理，支持UTF-8跨块、完整事件边界、工具参数拼接及终态/usage。可见增量只是暂定展示；完整参数和成功终态经过校验前，不向ToolLoop交付可执行提议。中断、取消及结果未知不自动重试。
4. 保持原ToolLoop负责调用、配对、授权、上下文压缩和操作生命周期。新增可选完整预置模式，只读取当前Task获准目录。工具定义属于当前请求，不加入会话历史；默认发现模式供既有消费者保持行为。
5. Responses有序output Items及reasoning/encrypted_content必须保留并按协议续传，不能只提取text和function。continuation与对应assistant帧绑定；压缩只总结规范对话/调用事实，不把加密内容总结后伪造续传。版本化内部正文格式，并分析旧消费者、跨API切换及回退行为。
6. 估算与实际出站使用同一投影；包括完整工具及必要续传Items。报告usage与前置估算分开，避免将不同wire投影的字符锚点混用。流式回调在传输worker执行，借用视图只活到回调返回；回调不得重入Core或执行工具。

## 顺序

1. 抽取现有Chat协议组件并切换Native/ESP消费者，原正常行为回归。
2. 实现版本化请求工具及回复continuation/历史帧，ToolLoop正式支持完整预置，保留当前持久记录的reader。
3. 实现两种API的非流式投影/解码及有序工具/续传往返，再加入共享SSE累计器和增量回调。
4. 接入两种HTTP传输及ESP Web端API/stream配置。运行正常协议矩阵、公开SDK集成和ESP-IDF编译，独立审查并修复。

## 后续实施细节

### 1. 共用协议与两种API

在共享组件增加显式API类型和stream选项，由协议组件决定path。Native和ESP只做HTTP、取消、期限、正文存储及结果发布，不再各自拼JSON或解析SSE。默认保留旧消费者的Chat/非流式行为，新增选项才改变wire；已提供extra_parameters用于显式服务扩展，禁止覆盖协议拥有的字段。

Chat使用嵌套function定义、messages、assistant.tool_calls及tool_call_id；Responses使用平铺function定义、input Items、function_call/call_id和function_call_output，输出预算字段也不同。不要把output item的id当作call_id。采用完整本地历史、显式store:false，不暗中依赖服务端previous_response_id。OpenAI新模型所需输出预算字段须显式支持，不能靠模型名字猜测。

### 2. 续传、压缩与兼容

Responses的有序output包含reasoning/encrypted_content，需要与对应assistant/calls帧绑定后原样续传。当前Continuation仅有codec/data，不代表已实现此功能。共享解码器必须从同一响应产生规范文本/工具提议和原Items，验证二者ID、函数、参数及顺序一致；不能把任意用户JSON提升为provider状态。

Provider与ToolLoop须配置相同ReplyEncoding；普通文本即使包含帧标记也不能自行选择解码方式。工具调用时可用原公共tool_calls编码的assistant_text承载版本化回复封套，保持Core只存opaque BodyRef；完成真实消费者及容量分析后再确认此设计。现有Core ABI0.54/schema9尚未改动，不为本功能顺手改Core容量或擦旧数据。

修改strategies/builtin/support.cpp的ModelResult/解析、tool_loop.cpp的接收/完成、必要的harness_summary.cpp消费者，确保续传进入对应历史组。现compact直接截取原history字节，必须改为规范摘要输入：不把opaque/encrypted Items发送给摘要模型，不拆散完整组，受保护的最新工具续轮保持完整；原Items随旧组一起移除。权限失效、清空及上下文重建要同时清掉续传，不留Provider私有缓存。跨API/模型/目的地变更要有明确拒绝或显式重置规则，不静默丢数据或自动清空。

新增正文格式需ADR、消费者/回退分析、相关契约及测试。C ABI不变不等于编码兼容未变。继续复用旧格式reader，普通模型回答、History/UI和summary不能显示内部封套。

### 3. 正式工具预置与token预算

在真正ToolLoopConfig增加可选预置方式，默认保留旧发现模式，ESP启用预置。只在strategy decide有效期内通过公共snapshot list/inspect捕获当前Task获准的定义，并在借用到期前复制；不从全部注册表绕过visibility。保持原disclosure capture/revalidate。

完整Schema和说明来自实际注册契约，不手抄屏幕、扬声器、电机参数。可继续使用ayni_call_capability的完整分支Schema，保留invoke/start/read/wait/cancel/close原语义；不需要为7个别名重写执行器。删除模型侧list/inspect往返，但不删除Core逐调用授权。通用系统提示不指定语音、运动或固定业务。

请求封套只属于本次模型BodyRef，不能追加到history。共享组件校验其中定义后按所选API写tools。预算须与真实投影共用writer，包括续传及完整工具；基本SDK可用显式计量端口注入API投影，避免强依赖HTTP/JSON库。不能只改主请求计量：tool_loop中的当前结果余量、ToolBudget::fits/estimate_tool_reply、摘要请求、reported usage字符锚点也须核对Responses封装。保留原用户token设置，字节容量是独立内存上限。

### 4. SSE及界面

共享增量累计器接受任意字节分块、CRLF、多data行和UTF-8跨块。Chat支持content/tool_calls分片、finish_reason、末尾choices=[]的usage及DONE；Responses处理原生text/arguments/item事件和completed/failed/incomplete终态，保留output顺序。arguments.done不代表整次响应完成；成功必须同时有完整协议终态及HTTP完成。

增量只是暂定预览，回调在I/O worker，视图只活到回调返回；不可重入Core或执行工具。完整工具参数和最终成功校验前不向策略发布提议。中断/取消保留UNKNOWN，不自动重试。既有Core PROGRESS只推进序号、不携带文本；UI预览可走明确边界，最终保存/完成仍只发生一次。

Web/触屏配置增加API类型及stream设置，仍用原配置快照、保存后正常重启及Key遮罩；不要在UI写实现术语或暴露内部封套。保留DeepSeek现有显式thinking扩展。

## 远程机器开始方式

先确认工作区干净并读取本分支最新提交，再按机器资源选择正整数并发度。下面仅为可执行入口，远程尚未运行：

    git fetch origin
    git switch feat/esp32-preloaded-tools
    git pull --ff-only
    export CMAKE_BUILD_PARALLEL_LEVEL=1
    export CTEST_PARALLEL_LEVEL=1
    cmake -S tests/independent/model_protocol -B build/model-protocol-checks -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
    cmake --build build/model-protocol-checks
    ctest --test-dir build/model-protocol-checks --output-on-failure

该独立工程编译共用协议和Native Provider，不选择Linux-only Host。Linux还应按仓库工作流运行受影响的原OpenAI Provider/策略检查及最终适用ASan CI；新增测试已经接入tests/unit/CMakeLists.txt。依赖固定版本在仓库CMake中；可按docs/development.md复用AYNI_DOWNLOAD_DIR或离线FetchContent源码。

这台Mac的缓存、build日志、ESP-IDF环境、私密会话导出、../asr_tts模型和服务均不随Git推送。远程不需要它们即可完成协议/SDK软件；ESP编译使用固定ESP-IDF6.0.3环境或既有CI配置，不执行deploy。

## 阶段验证与自查

- 当前检查点：共用协议/帧9项、Wi-Fi回归36项通过，ESP-IDF6.0.3/GCC15.2.0单并发编译通过，应用0x278580字节、分区余量37%；未烧录。
- Mac独立共用协议/帧正常检查使用AppleClang与ASan/UBSan；本机已有calendar_api.cpp参数timezone与Darwin全局符号触发shadow，独立构建仅加-Wno-error=shadow，未改Core来掩盖平台差异。完整根Native工程因Host明确限制Linux而未在Mac执行。
- 抽取阶段Wi-Fi原36项正常检查及ESP-IDF6.0.3/GCC15.2.0编译通过；最终检查点重新编译/验证结果以提交说明为准。没有烧录或运行硬件，没有新增恢复/故障/耐久/变异活动。
- 独立只读审查未发现阶段提交阻断；修复共享target的Core公共符号链接依赖与非法RequestOptions默默降级。删除注释的现行说明已迁移，Kernel无新增字段，JSON解析/参数规则只有共享实现，版本及边界使用命名常量。
- check_contracts与diff检查通过；readability无新机械违规。完整check_deps在这台Mac只剩6个已忽略Finder .DS_Store文件的扫描报错，未删除用户文件；实际新增组件依赖规则已收紧为providers:model_protocol。远程干净checkout应重跑全检查。
- 不要引用已撤回实验专用preload方案的42项测试作为当前证据。那份仅在本机忽略build中保留的patch不属于续作基础，当前Git源码才是基线。
- 可选容量WIP分支feat/application-context-resources不属于本功能依赖，未合并；其外置正文旧方案已由main的BodyRef替代，不整体cherry-pick。

## 完成标准

两种API×流式/非流式的文本/工具/usage正常矩阵通过；Responses有序reasoning及工具结果多轮往返；完整预置不产生目录往返、不累积定义，真实投影预算一致；Native/ESP复用共享实现，Web可配置；已有单轮文本、语音和许可调用流程回归；兼容/隐私/生命周期审查及适用检查完成后提交推送，交由用户处理PR。不把编译或本地夹具当作OpenAI/DeepSeek线上、硬件或128K峰值实证。

官方协议依据：

- https://developers.openai.com/api/docs/guides/function-calling
- https://developers.openai.com/api/docs/guides/migrate-to-responses
- https://developers.openai.com/api/docs/guides/reasoning
- https://developers.openai.com/api/docs/guides/streaming-responses
- https://developers.openai.com/api/reference/resources/responses/streaming-events

## 验证及限制

使用固定合成数据与本地HTTP/SSE夹具，不记录Key、私人聊天、语音或原始模型响应。覆盖两种API×流式/非流式、文本、工具、usage、分块边界和多轮配对；确认每请求一份tools、历史不积累定义。继续现有正常检查，不新增故障/恢复/变异活动。实机烧录前确认当前电机驱动断电；不由本计划推定硬件、128K峰值或停止链验收完成。PR及合并由用户处理。
