# 人工架空电机验线

此入口用于人工在场的短脉冲硬件诊断，不运行 Ayni Core，不开放模型运动能力，也不替代 `PHYSICAL` action、许可、联锁和独立停止链的正式接入/验证。Ayni 软件版本只有 `motor.status` 与 `motor.plan`，不会转动电机。

已知配置：用户确认 TA6586、D0/1/2/3→GPIO42/41/40/39；3V3 辅助输出留空，只共地及四根控制线；驱动独立 10 V、电机额定 12 V、车轮架空、有独立断电开关。电机电流和驱动板停止时限未测量。烧录/重启期间保持驱动电源关闭；握手通过后由在场用户开电并备好断电开关，再执行已授权的单个短脉冲。

固件上电默认四脚低，不自动运动。新 ARM 的 nonce 必须严格增大、两秒内使用、硬件调用前消费；一次 ARM 只允许一个 PULSE。频率为 1 kHz，占空比 1..20%，持续 50..300 ms，均为人工诊断软件界限，不是电机实测额定值。ESP32-S3 RMT 发送一个 PWM symbol 并进行有限次数硬件循环，结束电平为低；固定 SDK 已确认 S3 支持硬件自动停止循环。没有无限 PWM，也不依赖主循环准时发 STOP 来结束正常波形。异常尝试停止输出并锁定故障，不自动再次转动。

验线协议 v2：除仅可停止的 plain STOP 外，CHECK、ARM、PULSE 都须带完整 CRC32，格式为 `body *8hexcrc`。握手为 `AYNI MOTOR PROBE/2`，版本不匹配不降级。CLI 先执行 CHECK：逐脚绑定、读取实际 GPIO matrix/输入电平、解除路由，不提交波形；通过后才 ARM。每次 PULSE 前又检查只有目标脚路由到 RMT，其余三脚均为普通 GPIO 低电平。释放通道时必须显式切回 `SIG_GPIO_OUT_IDX`；固定 SDK 的 `gpio_reset_pin` 和 RMT 删除本身不会解除旧 matrix 路由，直接重新使能旧脚会在通道复用时串脚。本轮初版实际发现该问题后停止后续脉冲、用户断电、修正并完成无波形逐脚隔离检查；不能把修正前“同一个电机转动”的结果记为电机 2 通过。

这里没有外部超时看门狗或完整启动失能电路，复位/掉电/异常停止上限未经验证；独立电源开关是人工台架的物理切断方式。不能将此测试记为 C-STOP/C-PERMIT 全通过，不能据此开放无人值守或模型控制。

构建：`bash motor_probe/build.sh`，产物在 `build/motor-probe/`。使用已验证的 IDF Python 执行 `device.py deploy --variant probe` 仅写原应用/分区表；不写 bootloader、NVS、audio 或 Ayni 日志。通用 `idf.py flash` 仍禁止。

`motor_probe.py` 不自动烧录或复位，识别固定 USB 身份。无 `--channel` 时进行握手、无 PWM 路由检查并在退出时发 STOP；带 `--channel 1 --arm-isolated-bench` 才会请求一次默认 10% / 200 ms 正向输入脉冲。负方向加 `--direction negative`。结果未知时只发 STOP，不重试 ARM/PULSE。DONE 仅表示有限波形结束并已命令四脚低，不能证明轴转动、机械停止、方向、转角或硬件停止链性能；这些需要另行观察或测量。

诊断后先由用户关闭驱动电源，再 `device.py deploy --variant motor` 回到含软件电机查询/计划的 Ayni 候选；`--variant stable` 回到已验证的屏幕/朗读构建。旧星图固件恢复资料和 `device.py restore` 保持原样。
