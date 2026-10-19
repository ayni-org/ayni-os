# M0 GPIO 候选分配

- **工作包**：`MA-001`（计划 §1.3、§4.1、§7.1）
- **日期**：2026-10-19
- **结果等级**：资料推演。开发板不在手边，表中没有任何一项经过实测；板型确认和上电测量之后才能关闭（见 [board-feasibility.md](board-feasibility.md)）。

## 前提

计划 §1.3 假设现有板型是 ESP32-S3 N16R8（16 MB Flash、8 MB 八线 PSRAM）。本表按下面这种最常见的组合推演：

- 模组为 ESP32-S3-WROOM-1-N16R8，开发板与 ESP32-S3-DevKitC-1 引脚兼容；
- 摄像头使用板载 24 针 DVP 插座，接法与 esp32-camera 示例中的 `BOARD_ESP32S3_WROOM` 相同（市面上带摄像头插座的 N16R8 板大多采用这套接法）。

板子到手后，如果是不同的板型，只需重做“分配”一节，“约束”一节不变。

## 芯片与模组的约束

| GPIO | 约束 | 依据 | 本表的处理 |
|---|---|---|---|
| 26–32 | 连接模组内的 SPI Flash 与 PSRAM，不建议他用；WROOM-1 不引出 | ESP-IDF GPIO 文档 | 不用 |
| 33–37 | 八线 PSRAM 占用（SPIIO4–7、DQS）；N16R8 上 35/36/37 不可用，33/34 不引出 | ESP-IDF GPIO 文档；DevKitC-1 用户指南 | 不用 |
| 0 | 启动模式 strapping：内部弱上拉；复位时为低电平则进入下载模式 | 数据手册 v2.2 表 3-1、3-3 | 复用开发板 BOOT 键作录音键（见“风险”） |
| 3 | JTAG 信号源 strapping，没有内部上下拉；只有烧写 `EFUSE_STRAP_JTAG_SEL` 后才读取，默认 eFuse 下忽略 | 数据手册 v2.2 §3.4、表 3-5 | 作急停检测输入，外接上拉；**不得烧写 `EFUSE_STRAP_JTAG_SEL`** |
| 45 | VDD_SPI 电压 strapping：内部弱下拉，复位时为高会把 Flash/PSRAM 电压切到 1.8 V | 数据手册 v2.2 表 3-4 | 不用 |
| 46 | 启动模式与 ROM 日志 strapping：内部弱下拉；GPIO0 为高时取值不影响启动 | 数据手册 v2.2 表 3-1、3-3 | 作电机 PWM 输出，要求外部电路在复位时不把它拉高 |
| 19、20 | USB D-/D+，默认用于 USB-Serial-JTAG；改作 GPIO 后 USB-JTAG 失效 | ESP-IDF GPIO 文档；数据手册 v2.2 | 保留给烧录、日志与调试 |
| 43、44 | UART0 TX/RX，默认控制台与 ROM 日志 | 数据手册 v2.2 §3.3 | 保留 |
| 38 或 48 | DevKitC-1 v1.1 的 RGB LED 在 GPIO38，v1.0 在 GPIO48；带摄像头的板子常把 38 用作摄像头 PWDN | DevKitC-1 用户指南；esp32-camera `camera_pinout.h` | 视板型二选一，见“分配” |
| 47、48 | 由 VDD_SPI 或 VDD3P3_CPU 供电；ESP32-S3R8V/R16V 芯片上工作在 1.8 V，N16R8 所用的 ESP32-S3R8 为 3.3 V；复位时只有输入使能、没有内部上下拉 | 数据手册 v2.2 表 1-1、表 2-1 及其注 4 | 用于停止链；板型确认时核对模组不是 1.8 V 版本 |
| 上电毛刺 | GPIO1–18（15、16 即 XTAL_32K_P/N）有约 60 µs 低电平毛刺；GPIO18、19、20 另有高电平毛刺；21、38–48 不在列表中 | 数据手册 v2.2 表 2-2 | 电机与停止链输出不使用有高电平毛刺的引脚 |
| 1–10 | ADC1 通道（ADC1_CH0 = GPIO1） | 数据手册 v2.2 | 电池电压检测放在 GPIO1 |

## 分配

| 功能 | 信号 | GPIO | 方向 | 说明 |
|---|---|---|---|---|
| 摄像头 DVP | XCLK | 15 | 出 | 与 `BOARD_ESP32S3_WROOM` 相同 |
| | SIOD / SIOC | 4 / 5 | 双向 / 出 | SCCB（I²C） |
| | D0–D7 | 11、9、8、10、12、18、17、16 | 入 | |
| | VSYNC / HREF / PCLK | 6 / 7 / 13 | 入 | |
| | PWDN | 38（若板子如此连接） | 出 | 板上没有连接时 38 留作备用 |
| 音频（I2S0 全双工，共用时钟） | BCLK / WS | 41 / 42 | 出 | 麦克风与功放共用；录音与播放采样率须一致 |
| | DOUT → 功放 | 40 | 出 | |
| | DIN ← 麦克风 | 39 | 入 | 39–42 是 JTAG 引脚；调试改用 USB-JTAG |
| 电机（MCPWM0，经 DRV8833） | AIN1 / AIN2 | 2 / 14 | 出 | |
| | BIN1 / BIN2 | 21 / 46 | 出 | 46 复位时被内部下拉为低，电机输入为低 |
| 停止链 | HEARTBEAT（L2 → L1） | 47 | 出 | 外接 10 kΩ 下拉，复位与启动期间没有翻转沿；电路见 [`docs/hardware/stop-chain.md`](../../../docs/hardware/stop-chain.md) |
| | ESTOP_SENSE（MCPWM 故障输入） | 3 | 入 | 急停常开触点接地，外接 10 kΩ 上拉，经 1 kΩ 串联电阻接 GPIO；按下为低 |
| | ENABLE_READBACK | 48（RGB LED 在 48 时改用 38） | 入 | 经 100 kΩ 读回驱动器使能脚，作本机停止证据（C-STOP-006）；误配为输出时也无法使能驱动器 |
| 交互 | 录音键 | 0 | 入 | 开发板 BOOT 键 |
| | 状态灯 | 板载 RGB LED（38 或 48） | 出 | 与上一行互斥地使用 38/48 |
| 电源 | BATT_SENSE | 1 | 模拟入 | 电池分压后接 ADC1_CH0，用于欠压故障检测 |

合计使用 27 个 GPIO（摄像头 14、音频 4、电机 4、停止链 3、录音键 1、电池检测 1），保留 19/20、43/44 给烧录与日志，不用 45。余量只有 38 或 48 中的一个（取决于板型），不足以再接编码器；这与计划“v0.1 使用校准限时控制、不增加编码器”（§7.3）一致。

## 风险与待确认

| 项 | 影响 | 处理 |
|---|---|---|
| 板型未确认 | 摄像头接法、RGB LED 引脚、是否引出 38/47/48 都可能不同 | 板子到手后先拍照、读丝印与原理图，再重做“分配” |
| 录音键复用 GPIO0 | 上电时按住会进入下载模式；运行中正常 | 停止链使驱动器在固件未运行时保持失能，所以误入下载模式不会造成运动；如需避免，录音键改到 38/48 中的备用脚 |
| GPIO3 作输入 | 若将来烧写 `EFUSE_STRAP_JTAG_SEL`，外部上拉会改变 JTAG 来源 | 在 `docs/development.md` 与烧录流程中禁止烧写该 eFuse |
| 音频共用 I2S0 时钟 | 录音与播放必须同一采样率；计划要求录放冲突经资源协调（§7.2） | M3 测量；如需不同采样率，改用 I2S1 并占用 38/48 中的备用脚，届时状态灯改用外部 LED 或取消 |
| DRV8833 的 nFAULT 未接 GPIO | 过流/过热只能由读回使能与电流异常间接发现 | M3 评估是否以状态灯脚换 nFAULT |
| 摄像头 DMA/PSRAM 并发 | 引脚可用不代表带宽与内存足够（计划 §1.3） | M3 并发测量 |

## 来源

- ESP-IDF GPIO 文档（ESP32-S3）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html
- ESP32-S3 系列数据手册 v2.2 第 3 章（启动配置）：https://documentation.espressif.com/esp32-s3_datasheet_en.pdf
- ESP32-S3-DevKitC-1 v1.1 / v1.0 用户指南：https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html 、https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html
- esp32-camera 示例 `camera_pinout.h`（`BOARD_ESP32S3_WROOM`）：https://github.com/espressif/esp32-camera/blob/master/examples/camera_example/main/camera_pinout.h
- ESP-IDF MCPWM 文档（GPIO 故障与 OST 制动）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/mcpwm.html
