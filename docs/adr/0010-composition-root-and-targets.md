# ADR-0010：组合根 `runtime/` 与可执行目标 `targets/`

- **状态**：已接受（设计基线，待实现与验证）
- **日期**：2026-10-18（初版）、2026-10-19（修订：收窄“唯一引用具体实现”的范围）
- **关联**：计划 §2.6、§8、§11；ADR-0006、ADR-0008

## 背景

- 计划要求 Core 不依赖具体 Host、Provider 或驱动，但没有规定由谁把它们装配在一起。若模拟器和 ESP32 固件各自装配，同一套装配逻辑（注册顺序、`ayni_start`、部署 Profile 的容量配置、控制与停止组件的连接）会出现两份，“只有一处引用具体实现”的规则也没有落点。
- `apps/`（模拟器与固件的可执行程序）与 `applications/`（运行在 Ayni 之上的助理应用层）名称相近。在 AgentOS 的语境里，“apps”通常指运行在系统之上的应用，即 `applications/` 承担的职责，因此混淆的来源是可执行程序目录的名称。

## 决策

1. 新增组合根 `runtime/`：按部署 Profile（`CORE_LITE`、`CORE_FULL`，以及未来的设备端点）装配 Kernel、Host、Provider、控制与停止组件，完成注册与 `ayni_start`。`runtime/` 是部署选择与跨组件装配的唯一位置；各 Host、Provider 与适配器仍可在内部封装自身的平台实现（例如 ESP-IDF Host 引用 ESP-IDF 驱动、ROS 适配器引用 ROS 客户端库），但不互相装配。初版写作“唯一引用具体实现的位置”，会误禁止适配器封装自身平台实现，已收窄。
2. 可执行程序与固件目录由 `apps/` 更名为 `targets/`（例如 `targets/simulator/`、`targets/esp32_assistant/`）。它们只调用 `runtime/`，不直接装配组件。
3. 保留 `applications/` 作为应用层目录名，与计划正文中“应用注册”“应用层”的用语一致。
4. `targets/` 指构建产物的入口，与 ESP-IDF 的芯片 target（`idf.py set-target`）是不同概念，计划 §8 对此另有说明。

## 后果

- 新增宿主或部署 Profile 时，只需改 `runtime/` 中的装配和新的 `targets/` 入口，Core 与 SDK 不变。
- 装配错误（注册缺失、Profile 容量与 Host 能力不符）可以在 `runtime/` 中统一测试。
- 文档中的计划命令路径改为 `build/native/targets/simulator/ayni-sim` 与 `targets/esp32_assistant/`。

## 备选方案

- **每个可执行程序自行装配**：装配逻辑重复，否决。
- **把装配放进 Core 或 SDK**：Core 会依赖具体实现，违反依赖规则，否决。
- **改名 `applications/` 而保留 `apps/`**：计划正文以“应用”指运行在 Ayni 之上的业务层，改名会使用语与目录不一致，否决。

## 验证

实现开始后，在 CI 的依赖检查中确认：跨组件引用 `hosts/`、`providers/`、`drivers/`、`control/` 与 `safety/` 具体实现的只有 `runtime/`，各组件对自身平台实现的内部引用不受此限；`targets/` 只依赖 `runtime/`；`examples/` 只依赖公开 SDK。
