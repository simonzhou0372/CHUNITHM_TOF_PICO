# USB / ToF 非阻塞排查与修复（2026-09-20）

## 结论与证据范围

现象：前一版降低了 USB 掉线频率，但仍会掉线并在约 2 秒后恢复；有时 MPR121 和 USB 正常，五颗 VL53L0X 都不再提供数据。

本轮确认并修复了源码中的阻塞和恢复缺陷；未连接实机/SWD，不能把某一条路径断言为现场唯一根因。约 2 秒与现有运行期看门狗期限吻合，但需要重连后的 `USBSTATUS wdt` 才能确认发生过看门狗复位。

| 确认的问题 | 影响 | 当前处理 |
| --- | --- | --- |
| TinyUSB RP2040 `reset_ep0()` 在 USB IRQ 内无限等待 `abort_done` | 若硬件不完成 ABORT，中断不能退出；USB/主循环/LED 更新可能一起停止，随后看门狗复位 | IRQ 只检查一次，未完成则交主循环继续；1 ms 仍未完成时关闭 USB IRQ，拆卸并在 250 ms 后重新初始化 USB |
| 上轮有界读取在超时后只请求 `enable=0`，未确认禁用完成；寄存器地址写和读仍是分开的同步调用 | 外设可能保留活动事务/FIFO 状态，后续访问持续失败；有超时仍会占用整个调用时间 | 两条总线均换成 `AsyncI2c`，一笔完整的 pointer/RESTART/read/STOP 跨循环推进，等待禁用完成后才修改地址；异常只复位对应 I2C 控制器 |
| ToF 自动恢复默认关闭 | Core1 即使活着，也可能一直报错，五颗没有自动恢复 | 默认启用分级恢复，全部为协作式任务 |
| 初始化、SPAD/参考校准、XSHUT/总线恢复有同步等待和递归让出；Core1 使用 `sleep_us` | 恢复本身延迟调度，且依赖共享 alarm pool | 固定内存 C++20 状态机，每次只推进一个叶任务；等待用 deadline/suspend 返回调度器；Core1 不再使用 sleep/alarm pool |
| 多颗 ToF 共同复位后回到 `0x29`，逐颗恢复可能把地址写同时发给多颗 | 修改成相同地址，无法可靠恢复全部传感器 | 目标保持 XSHUT 低时先检查 `0x29`；若仍有应答，停止单颗恢复，所有 XSHUT 拉低后逐颗重新分配地址 |
| Flash 协作进入/退出各允许等待 1000 ms | 加上擦写可能超过 2 s 看门狗期限 | 先检查 Core1 已注册 Flash 协作且心跳新鲜，进入/退出各限制 10 ms |

共同复位的地址冲突已在模型测试中复现：加入检查前，测试在“向多个同址传感器写配置/地址”的断言处失败；加入检查后，两颗同时复位和五颗同时复位均恢复到 `0x30`～`0x34`。这证明了恢复路径缺陷，不证明实机存在供电瞬断。

## 实现与保留项

- `src/i2c_async.h`：每次 `step()` 最多推进一个 RX 字节和一个 TX 命令，不等待 FIFO、ACK、STOP、禁用或复位完成。MPR 事务截止时间 500 us，ToF 3000 us；超时后的本控制器复位完成检查最多再给 100 us，检查本身同样不忙等。若调用方被外部暂停，截止时间在下一次调用时处理。
- `src/cooperative_task.h`：仅 Core1 使用 24 个固定的 1024 字节帧，不依赖堆或跨核分配器锁。分配失败返回默认失败结果并增加诊断计数。管理任务与采样任务共用唯一 I2C1 所有者；其他健康传感器可在单颗初始化期间继续采样。
- `src/tof_reader.cpp`：心跳从冷启动开始持续更新，发布 stage/进展时间；共享快照增加内存屏障。个体失败退避重试，总线持续失联才升级恢复；不直接强制重启 Core1，避免打断持锁中的 SDK 代码。
- `src/mpr121.cpp`：冷启动、轮询、CONFIG、RESET 全部分步推进；保留完整读成功后更新触摸位图、失败保留旧状态的语义。连续 CONFIG 会补做完整一轮，确保已经写过的通道也得到最终值。DEBUG 使用缓存，不另起同步 I2C 访问。
- `cmake/PatchTinyUsb.cmake`：只修补构建目录中的驱动副本，保留上游许可证，不修改用户 SDK。匹配点变化时配置阶段直接报错，要求重新审查补丁。仅 ToF 翻译单元要求 C++20，CMake 最低版本 3.18。
- `src/main.cpp`：USB 故障重初始化期间暂停 TinyUSB 类接口访问，传感器和主循环继续工作；重新枚举后发送当前 HID 状态。BOOTLOADER 的 100 ms 等待也改为 deadline。

相对本轮开始时，`air.cpp`、`slider.cpp`、`button.cpp`、`config.cpp`、`usb_descriptors.c` 未改变。触摸硬件判定、阈值/滤波寄存器、键位/HID 格式、距离有效性（0 或 ≥8190 处理为 8190）、保持时间和 20 ms 测距预算保持不变。VL53L0X tuning 表与本轮基线逐字节相同。改变的是 I/O 调度、故障处理及恢复策略，轮询频率因此需在实机确认。

## 仍然存在的同步边界

“非阻塞”覆盖本项目的传感器运行、初始化和恢复流程，以及已识别的 EP0 ABORT 无限等待。并非宣称整个 SDK 或芯片在任意硬件故障下都能非阻塞。

- 显式 `SAVE` 仍通过 SDK Flash 安全机制擦写 XIP Flash，期间两核与 USB 服务会暂停。当前架构下不能把 Flash 擦写改成普通轮询，同时又继续从同一 Flash 安全执行。保存请求仍在空闲窗口执行，等待超过原有 10 秒上限可强制执行；本轮没有改动配置存储格式或保存策略。
- 启动时的 Core1 launch、USB 控制器初始化、SDK 短临界区/硬件自旋锁仍存在；GPIO 启动采样有一次 100 us 等待。它们不是传感器运行循环的一部分。未知 IRQ 故障或硬件故障仍由原有看门狗兜底。
- 持续拉低的物理总线不能由软件保证恢复；状态机能保持心跳和重试，但实机电平/供电仍需测量。

## 刷机与诊断

固件：`build/Chuni245Tof.uf2`。刷入后通过 CDC 串口逐行发送以下命令（行尾换行即可），读取回复；不需要打开 printf。`USBSTATUS` 应包含 `rev=20260920`。

```text
USBSTATUS
TOFSTATUS
```

两条命令每条最多回复一行；主机不读取或发送缓冲不足时跳过，不等待。故障前、恢复后分别保留一组即可；ToF 异常时可间隔 1 秒连读两次。

| 字段 | 判断方式 |
| --- | --- |
| USB `boot`、`wdt`、`prev` | boot 增加且 wdt=1 表示看门狗复位；prev 是当时主循环阶段，不能当作 IRQ 内的 PC |
| `usb_abort` | 本次 MCU 启动以来确认的 EP0 ABORT 超时数；它增加且 boot 不变表示 USB 局部恢复，而不是整机重启 |
| USB `gap_us` | 本次启动以来 USB 服务间隔最大值；显式 SAVE 也会使它增大 |
| TOF `hb`、`age_us` | 连续读取 hb 增加、age_us 很小表示 Core1 正在调度；hb 不变且 age_us 持续增长才支持 Core1 停止推进 |
| TOF `stage` | 1=Flash 协作注册，2=驱动推进，3=采样，4=恢复管理，5=本轮完成；单次读到某个值不代表卡死 |
| TOF `job` | 0=无管理任务，1=启动，2=单颗恢复，3=总线清理，4=全量重新分配/初始化，5=存活探测 |
| TOF `io` | 0=空闲，1=本控制器复位，2=等待禁用完成，3=事务；单次值仅代表快照 |
| TOF `ready`、`data_ms` | ready=1f 表示五颗已初始化；data_ms 按 TOF1～5 表示快照年龄，4294967295 表示无有效快照。ready 本身不保证正在产出新样本 |
| TOF `reset`、`err` | I2C1 控制器异常复位次数及各传感器采样错误；恢复时探测空地址产生的 NACK 也可能计入 reset |
| TOF `frames_fail` | 应为 0；非零表示固定任务帧不足或尺寸不适用，需要检查构建和调用深度 |

## 验证记录

- Pico / RP2040、SDK 2.3.0、ARM GCC 15.2 Release 编译通过，生成 ELF/BIN/UF2。
- 原生模型使用实际驱动和 Core1 循环，覆盖 FIFO/RX/STOP/disable/reset 卡住、SPAD/校准不完成、单颗掉线、共享总线卡住、两颗/五颗共同复位、MPR 并行读取及 CONFIG/RESET。
- USB 测试从 CMake 生成的实际 DCD 副本提取函数，验证延后处理、只提交一次 SETUP、1 ms 超时与计时回绕、异常关闭 IRQ、B0/B1 原分支。
- ELF 不再链接 `i2c_read_blocking` / `i2c_write_blocking` / `sleep_us` / `sleep_ms`。这不等价于所有 SDK 路径无任何同步等待。
- 检测层文件与本轮基线比对未改变；代码空白检查通过。测试命令及模型限制见 [tests/README.md](../tests/README.md)。

仍需实机长时间运行，以及 USB 重枚举、单颗传感器掉线、共同复位、显式 SAVE 后恢复的检查。未进行实机烧录或声称完成硬件验证。

## 上游源码依据

核对的是本机 SDK 配套 TinyUSB 提交 `86ad6e56c1700e85f1c5678607a762cfe3aa2f47` 的 [RP2040 DCD](https://github.com/hathach/tinyusb/blob/86ad6e56c1700e85f1c5678607a762cfe3aa2f47/src/portable/raspberrypi/rp2040/dcd_rp2040.c)，以及 SDK 2.3.0 的 [I2C](https://github.com/raspberrypi/pico-sdk/blob/2.3.0/src/rp2_common/hardware_i2c/i2c.c) 与 [Flash 安全协作](https://github.com/raspberrypi/pico-sdk/blob/2.3.0/src/rp2_common/pico_flash/flash.c)。上游代码解释了可能的等待路径，不替代现场日志。
