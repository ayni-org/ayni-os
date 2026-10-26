# ADR-0055：Native OpenAI-compatible Provider 与有界协议适配

- **状态**：已接受（用户 2026-10-26 批准的 P1 PR 2；实施细则）
- **日期**：2026-10-26
- **关联**：计划 §2.7、§3.3、§9.1；MA-008C 后半；ADR-0042 决定 4、14、15；ADR-0054；C-PROV-002/003/004/006/008/011

## 背景

Core 已交付 typed 工具视图与结果接口。现在需要用真实文本服务完成同一公开调用链，外部 JSON、HTTP、TLS 和凭据不能进入 Core，也不能让 Kernel 的 step 等待网络。本次仅实现非流式 Chat Completions，不装配 CLI 或策略。

## 决策

1. **组件边界。** `providers/openai_compat` 只含公开 ABI、Provider 自身头和外部库；`provider.cpp` 拥有配置、完成借用、工作线程与 libcurl，`protocol.cpp` 拥有 JSON 读写和工具映射。Native 根 CMake 引入 libcurl ≥7.85、Threads 和按发布包哈希固定的 yyjson 0.10.0（MIT）；ESP-IDF 的组合根不包含此目录。libcurl 使用其随发行版安装的 curl 许可，TLS 依赖由宿主软件包提供。
2. **调用声明。** 单一 `openai.chat` capability，UNARY/MODEL，FIELDS_TEXT → TOOL_CALLS，声明 EGRESS 与 BILLING、NOT_IDEMPOTENT、QUERY_NONE 和 CANCEL_REQUEST。Provider/端点/目的地 ID 由组合根配置；组合根须注册对应 IDENTIFIER 外发字段与 Binding。一次输入是一个 user message，工具由 submit 视图生成；多轮上下文的内容仍归后续策略。
3. **固定容量与线程。** configure 分配槽、正文、请求/响应缓冲和两个 yyjson 内存池，并启动一个线程运行 curl multi；curl/TLS 的内部内存由该库管理，不声称整个进程无堆。submit/cancel 在 Kernel owner 上非阻塞、不分配，只复制有限视图或设置取消请求；工具回调仅在本次 submit 内调用，绝不保存。release/acquire 发布槽；worker 拥有 running/ready 与 HTTP 状态，结果只经 `ayni_provider_post`。fatal multi 错误后 worker 留在排空模式，接住错误发生前已开始复制的 submit，直到显式 close；不因致命错误丢失借用。
4. **配置与外发。** base URL、model、密钥的环境变量名运行时提供；Provider 在 configure 时读密钥。只接受 HTTPS，以及本地协议夹具的数字回环 HTTP；拒绝 URL 用户信息、query、fragment。保留 TLS 校验，关闭环境代理、netrc、重定向、连接复用；不自动重试、换服务或调整参数。外发目的地 token 与 URL 的对应关系归可信组合根，模型不能选择 URL。密钥只在 Provider 的私有 HTTP header 内保留，关闭时清除；不输出 header、原始错误或正文。
5. **协议上限。** 请求 JSON 固定上限 1 MiB；响应默认 128 KiB、最大 1 MiB。JSON 解析前检查容器深度 ≤8，解析用固定内存池，随后每个数组与对象最多 64 项，拒绝重复键及非法 UTF-8。函数 arguments 是另一个有相同深度、容量边界的 JSON 对象。默认并发 2、最多 8；正文默认 4096 B、最大遵从 CALL_TEXT_BYTES_MAX；超时默认 30 秒、最大 300 秒，从 submit 开始含排队。显式 `max_output_tokens` 默认 1024、范围 1～8192，发送为 `max_tokens`，和字节上限分别约束；服务不支持时直接报告错误。
6. **工具映射。** 本次视图的第 i 项命名 `tool_i`，描述用受信 Binding ID；参数 `field_<field_id>` 保留 I64 区间与单位、ENUM/IDENTIFIER 的精确集合，另有必需的 `text` 字符串及字节上限说明。模型不能改变单位或路由范围。每个字段必须恰好一次，额外/缺失字段、未知工具、越界整数/枚举、正文超限均 FAILED/INVALID_ARGUMENT/MODEL_OUTPUT。列表超过 8 项、JSON 容器或总响应超限为 CAPACITY_EXCEEDED/MODEL_OUTPUT。列表编码大小由 `ayni_provider_post` 判定，不复制 Core 编码公式；post 拒绝时改报 FAILED，归还原借用。
7. **结果与错误。** 只接受一个 choice，文本须 `finish_reason=stop`，工具须 `tool_calls`；截断、正文与工具混合等不合格输出按 MODEL_OUTPUT 失败，不假装完整成功。HTTP 400/401、403/404/408/429/5xx 分别映射 INVALID_ARGUMENT/DENIED/NOT_FOUND/EXPIRED/BUSY/NOT_READY，其他状态为 UNSUPPORTED，原因 NONE，正文不进入 Core。HTTP 状态不证明未外发或未计费，失败的 effect 为 UNKNOWN。网络失败与超时均以 ABANDONED/RESULT_UNKNOWN 结束跟踪，effect UNKNOWN；当前 ABI 不另携带 transport 超时细因。配置/请求准备在任何发送前失败才可以报告 effect NONE。
8. **取消与关闭。** 取消移除本地 HTTP handle，先 post CANCEL_UNKNOWN，再 post ABANDONED，二者均不声称远端未执行；已经取得的终态优先于晚到取消。完成发表失败时保留借用与答案，只重试发表，不重发 HTTP；`status()` 暴露第一个非 BUSY 的本地错误。正常退出由 Kernel begin_shutdown 排空，成功销毁 Kernel 后调用 Provider close；close 在借用未还时返回 BUSY，成功路径 join worker 后释放存储。调用方必须保持已注册 context 的寿命，不依赖析构替代这一协议。
9. **验证分离。** 日常测试只用回环 HTTP 夹具，无外部模型请求。`ayni_openai_smoke` 不加入默认构建或 CTest，显式 `--live` 加运行时环境才会发起两次请求。用户选定 DeepSeek `deepseek-chat` / `https://api.deepseek.com`，并授权本轮小额验证；密钥经进程输入注入环境，没有保存到仓库或日志。未建立 GitHub Secret 或自动实 API workflow。

## 后果

- Core ABI 0.37、schema 2、Kernel arena 与容量不变，不重复量测 Kernel 内存。
- 替换另一家兼容服务仅改运行配置；服务格式不兼容时修改协议适配和夹具，不能通过重试/降级绕过契约。未实测服务不宣称兼容。
- yyjson 的文档内存有硬上限，libcurl/TLS 的内部连接分配不属于 Core 无堆保证。仅验证 Native Linux；流式、多模态、查询、CLI 和多轮策略仍不在此 PR。
- 已外发内容和可能产生的计费不能随本地取消撤回。usage 缺失保持未知，不建立费用账本。

## 备选方案

- 同步 curl easy：会阻塞 Kernel；否决。每请求一线程：增加线程与取消管理成本；选一个 multi worker。
- 在 worker 继续调用 Core 工具回调：违反视图寿命；改为 submit 内深拷贝。
- 复用 Core 私有编码器/validator：让 Provider 依赖 Core 表示。这里只验证协议及公开视图，列表编码上限交给公开 post；Kernel 保留自己的语义复核。
- 外部 JSON 使用无上限的动态 DOM：内存随网络输入增长；采用字节边界、深度预检、固定 yyjson pool 与容器长度限制。

## 验证

本地回环协议测试、Clang 与 ASan/UBSan 相关检查已通过；DeepSeek 短文本与 typed 工具提议通过公开 Kernel 调用链。独立审查发现的退出竞态和边界分类已修正，最终复核与 CI 证据集中在本 PR。未执行新的故障/恢复专项、变异活动或硬件验证；fatal multi 的排空交错仅做静态审查。
