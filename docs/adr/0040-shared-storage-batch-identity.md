# ADR-0040：批次身份解析归属

- **状态**：实施中
- **日期**：2026-10-24
- **关联**：MA-010；C-STORE-001、C-STORE-004、C-HOST-010；计划 §9.1

## 背景

批次布局归 durability/batch 所有，Native 入口、FileLog 扫描/去重/裁剪及模拟 Host 却各自解析 AYJB 与固定偏移。增加 Host 或修改公共格式时容易出现解释分歧。

## 决策

1. 公开 C99 storage_format.h，提供固定值类型、最小前缀长度及纯函数 ayni_storage_read_batch_identity；实现仍在 durability/batch/batch_codec.cpp，与写入器共享布局常量。Core 完整解码也调用该读取器。
2. 只检查空指针、最小长度和 magic，并复制 epoch/sequence/transition 三字段。失败不改输出；成功不代表 schema、CRC、marker 或语义身份有效。FileLog 只读取前缀，不能因更强的完整校验而改变原有恢复分类。
3. 所有 Host 保留原有 framing、opaque 数据回退、完整字节幂等比较、writer fence 和 trim 选择规则。Host 不包含 Core 私有头，不新增反向依赖或抽象层。
4. 新符号和值类型将 experimental ABI 升到 0.28；按 C-VER-006，调用者需重新编译。存储 schema 仍为 2，字节/记录版本及 Core 状态布局均不变，无存储迁移；回退旧二进制仍按其原有 schema 规则读取。

## 后果

新增或替换 Host 只依赖公开 helper，批次身份布局只有一个解析属主。公开格式读取器扩大了一个稳定且必要的边界；它没有提供新的权限或写入能力。固定值类型不是可扩展描述表，与既有 attempt_ref 值一致；C-VER-003 明确这一例外，后续布局变更需新的类型/函数版本。

## 备选方案

不让 Host 引用私有 batch_codec.h，避免耦合记录迭代和完整恢复；不在每个 Host 保留偏移常量，避免分散格式知识；不要求整批校验，因为 FileLog 的现有前缀读取不能满足该契约。

## 验证

本地 GCC Debug/Release、ASan/UBSan、TSan、Clang Debug、模拟器及工具检查通过；字节向量、失败不改输出和 C99 布局/链接检查通过。默认/最小工作内存仍为 212,096/34,176 B，对齐 8 B。命令与结果见 [#58](https://github.com/RobotSe7en/ayni/pull/58)。

批次部分先前已完成静态独立审查，固定值类型契约例外已修正。按用户 2026-10-24 后续指示，本次与 Task 记录修复合为同一 PR，合并范围只做作者自查，不再安排独立审查，不做植入缺陷测试。作者自查无未关闭发现；无硬件/耐久或合并后覆盖率结果。
