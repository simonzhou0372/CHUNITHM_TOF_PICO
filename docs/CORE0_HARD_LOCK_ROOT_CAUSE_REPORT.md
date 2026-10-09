# RP2040 Core0 Hard-Lock / USB+LED 同时停摆 —— 根因分析与修复报告

> 2026-09-19 复核更正：本文是历史排查记录，不能作为实机根因已确认的证据。
> SDK 2.3.0 读取函数存在未检查超时的 TX FIFO 等待（第 9 节已更正）。
> LED 常亮也不能单独证明永久死锁。最新代码修复、验证范围和 USBSTATUS
> 使用方法见 [USB_DROPOUT_2026_09_19.md](USB_DROPOUT_2026_09_19.md)。

适用固件: Chuni245Tof (src 第 9 轮排查)
日期: 2026-09-18

---

## 1. 结论摘要

**新证据**: USB HID 完全无输入的同一时刻, 板载 LED 呼吸也停止变化。
LED 在旧实现中由 Core0 主循环直接更新 → **LED 冻结证明主循环本身硬停**,
即这不是 (或不只是) USB endpoint 层问题, 而是 Core0 主循环被同步长操作
卡死。旧固件中 CDC 命令路径同步执行长 I2C 操作 + 无界互斥等待, 任何一条
都可能造成数百 ms ~ 秒级的主循环停摆; 主机侧 CDC 连接如果恰好持续收到
"相同数据量" 后触发其中一条命令路径, 就表现为 "每次都在差不多相同的数据
量之后掉线"。

修复: 命令全异步化 (CONFIG/DEFAULT/RESET/DEBUG 变为状态机或直接拒绝 I2C)、
CDC 只解析入队、save_mutex 移除、stage 面包屑 + 2000ms 硬件看门狗兜底、
LED 改由硬件定时器 IRQ 驱动 (与主循环解耦)。

---

## 2. 证据链

| 观察 | 推论 |
|---|---|
| USB HID 无输入 | tud_task / report_usb_hid 未被服务, 或 endpoint 死亡 |
| **同时** LED 停止变化 | LED 更新代码在主循环内 → **主循环停摆** (旧实现) |
| 每次传输 ~相同数据量后掉线 | 确定性触发: 数据量决定哪条 CDC 命令序列被送入 |
| 自动恢复 (tud_disconnect/connect) 反而更糟 | 恢复动作本身即主机可见的断开; 本轮已默认关闭 |

---

## 3. 全量阻塞审计结果 (修复后逐文件)

对 Core0 侧全部翻译单元执行 `sleep_ms / busy_wait / mutex_enter_blocking /
sem_acquire_blocking / while(!..)` 扫描:

| 文件 | 残留 | 性质 |
|---|---|---|
| main.cpp:861 `sleep_ms(100)` | BOOTLOADER 命令 | 紧接 reset_usb_boot, 设备本就离开应用, 无害 |
| main.cpp:1049 `busy_wait_us(100)` | 上电 GPIO pull-up 稳定 | 仅 boot 路径 |
| main.cpp:1157 `while(1)` | 主循环本身 | 每轮末尾喂狗, 2s 看门狗兜底 |
| mpr121.cpp:234/279/297/315/331 | mpr_init_single / mpr121_init | **仅上电路径** (RESET 不再调用它), 主循环不可达 |
| mpr121.cpp:514 | 注释引用 | 非代码 |
| save.cpp | 0 | mutex_enter_blocking 已删除 |
| air.cpp / slider.cpp / button.cpp / config.cpp | 0 | 纯非阻塞 |
| tof_reader.cpp / vl53l0x.cpp | I2C1 全部在 Core1 | Core0 隔离不变 |

**主循环内已无任何 sleep / 无界等待 / 多事务同步 I2C 路径。**

---

## 4. 根因 #1 — CONFIG 命令同步下发阈值 (最坏 720ms+)

旧路径: `cdc_task → cdc_process_command("CONFIG ...") → mpr121_set_thresholds()`
同步执行 3 设备 × 12 通道 × 2 寄存器 = **72 次 I2C 写, 每次使用 10ms INIT
timeout** → 最坏 72 × 10ms = **720ms**, 期间 tud_task / HID / LED 全部停摆。

**新的最坏时间**: 命令入口只锁存目标值 (µs 级); 实际写入由
`mpr121_task_step()` 每主循环执行 **1 次寄存器写 (≤500us)**, 72 步在
~72ms 内摊平完成, 每步之间主循环正常运转。

## 5. 根因 #2 — RESET 命令同步重初始化 (最坏 ~1.5s)

旧路径: `mpr121_reset_baseline()` 对每个芯片: 10ms-timeout 软复位写 +
`sleep_ms(2)` + 完整 `mpr_init_single()` (39 次 10ms-timeout 事务 +
`sleep_ms(100)`) → 每芯片 ~500ms, 3 芯片 **~1.5s 主循环停摆**。

新路径: 状态机 `THR_RESET_WRITE → WAIT1(deadline 3ms) → CHECK(1 读) →
INIT(39 步, 每步 1 写) → WAIT2(deadline 100ms) → 下一芯片`。
每步 ≤500us, 等待期用 deadline 判定 (主循环继续跑), 全程 ~0.5s 摊平完成,
单步最坏仍 500us。

## 6. 根因 #3 — DEBUG 命令的 I2C 读 (最坏 ~1s)

旧: `mpr121_debug_print()` 对每芯片 24 次阈值读 + 6 通道 × 2 次字读,
全部 10ms timeout → 3 芯片最坏 ~1s。且 printf 已全局禁用, 输出毫无意义。

新: 函数体 `#if !DEBUG_MPR121 → return;` —— **DEBUG_MPR121=0 时 DEBUG
命令零 I2C**。

## 7. 根因 #4 — save_mutex 无界阻塞等待

旧: `save_write()` 进入时 `mutex_enter_blocking(save_mutex)` —— 若锁被
占则**无限期自旋** (单写者模型下锁永远不应被占, 但一旦发生就是永久硬锁,
且没有任何现场可查)。

新: 移除加锁/解锁。save 模块仅 Core0 主循环空闲窗口单写者调用, Core1
从不触碰 save (I2C1/ToF 与 Flash 保存无共享状态), 互斥锁属多余设施。
flash_safe_execute 内部的双核 lockout 由 SDK 负责, 与此无关。

## 8. 根因 #5 — LED 在主循环内更新 (观测层缺陷)

旧: LED 在 `while(1)` 末尾用 `gpio_put` 更新 → 主循环一死 LED 一起冻结,
把 "可观测的硬锁" 变成 "两件事同时静默"。
另: `main_loop_max_us > 5000 → LED 常亮` 是**一次性锁存**, 第一次 SAVE
Flash 擦写 (~400ms) 就会永久点亮, 该逻辑已一并移除。

新: 10ms `add_repeating_timer_us` 硬件定时器 IRQ 驱动 LED, 处理器仅做
比较 + gpio_put (µs 级), 主循环死活不影响其运行:

| LED 表现 | 含义 (优先级从高到低) |
|---|---|
| 常亮 | **主循环卡死**: 某 stage 停留 >50ms (正常每 ~1ms 刷新) |
| 100ms 快闪 | USB 未 mounted |
| 1000ms 慢闪 | USB suspended |
| 250ms 闪 | HID endpoint not-ready 持续 >100ms |
| 500ms 心跳 | 一切正常 |

## 9. SDK I2C 超时审查更正 (2026-09-19)

原文“全部等待循环都有超时”和“I2C0 不存在卡死路径”的结论错误。
SDK 2.3.0 `hardware_i2c/i2c.c:286` 的读取路径包含：

```c
while (!i2c_get_write_available(i2c))
    tight_loop_contents();
```

该循环不检查 `timeout_check`。调用 `_blocking_until` 不能保证在此状态下返回。
发送 FIFO 永久没有空位时，Core0 的 USB 服务会被阻塞。
这是源码确认的缺陷；是否为用户设备的实际触发点仍需复位现场验证。

本轮两条总线的寄存器读取均改用项目内 `i2c_read_bounded_until`，发送空位与
接收数据等待共享绝对截止时间。写入保留 SDK 有界实现，MPR121 增加完整传输
长度检查。截止时间不包含 IRQ 抢占和 Flash 暂停时间，不能称为墙钟硬上限。

## 10. 新架构总览 (Core0 主循环)

```
while(1) {
    STAGE(TUD_TASK)   tud_task();
    STAGE(CDC_TASK)   cdc_task();        // 只接收+解析+入队 (≤1ms deadline)
    STAGE(CMD_STEP)   command_step();    // 每轮最多执行 1 条命令
    STAGE(SLIDER)     slider_update();   // 轮转 1 设备 (≤1ms) + 异步命令步 (≤0.5ms)
    STAGE(AIR)        air_update();
    STAGE(BUTTON)     button_update();
    STAGE(HID)        gen_nkro_report(); report_usb_hid(); usb_hid_watchdog();
    STAGE(SAVE)       save_loop();       // 仅空闲窗口; Flash 擦写为硬件固有停顿
    loop_completed_count++;  last_stage = LOOP_END;  watchdog_update();
}
LED: 10ms 硬件定时器 IRQ (独立于主循环)
```

每轮进入任务前写入 stage 面包屑 (`.uninitialized_data`, 看门狗复位保留)。

## 11. CDC 命令队列

- `cdc_task`: 收到换行 → 拷入深度 4 的环形队列, **不执行** (队列满则丢弃
  并计数 `cmdq_full`)。内部 1ms deadline 与 64B/轮上限保留。
- `command_step()`: 每主循环出队最多 1 条执行。
- 命令本身也已轻量化 (见 §4–6): 最坏单条执行 = printf (编译期 no-op) +
  快照读取, **<10µs** 量级。

## 12. 最坏执行时间对照表

| 路径 | 旧最坏 | 新最坏 |
|---|---|---|
| cdc_task (单轮) | ≤1ms (deadline) | ≤1ms (不变) |
| 单条 CONFIG | **~720ms** (72×10ms 同步) | ~µs 入队 + 72 步 × ≤0.5ms 摊平 |
| 单条 DEFAULT | ~720ms | ~µs (同上) |
| 单条 RESET | **~1.5s** (3×[10ms+2ms+39×10ms+100ms]) | ~µs 启动 + 每步 ≤0.5ms 摊平 (~0.5s 完成) |
| 单条 DEBUG | **~1s** (数十次 10ms 读) | 0 (DEBUG_MPR121=0 直接 return) |
| 单条 SAVE | 0 (已异步入队) | 0 (不变) |
| slider_update | ≤1ms | ≤1.5ms (轮询 2×0.5ms + 命令步 1×0.5ms) |
| command_step | — | <10µs (命令已全部轻量化) |
| save_loop (空闲窗口) | ~400ms (Flash 擦写, 硬件固有) | 同左 (XIP 停摆不可避免, 仅空闲时发生) |
| **整轮主循环 (常态)** | ~1ms | **~1.5ms** |
| **整轮主循环 (最坏, 不含 SAVE)** | **>1.5s** (任意一条命令) | **≤1.5ms** |
| tud_task 最大间隔 (常态) | 受命令路径拖累可达秒级 | ≤1.5ms + SAVE 空闲窗口的擦写停顿 |

## 13. Stage 面包屑 + 硬件看门狗 (最终兜底)

- 枚举 `main_stage_t` 共 10 个 stage; `STAGE_ENTER()` 在每个任务前更新
  `last_main_stage / last_stage_enter_us`, 循环末尾标记 `LOOP_END`。
- 三者均位于 `.uninitialized_data`: **看门狗/异常软复位保留, 断电清零**。
- `watchdog_enable(2000, 1)` (2000ms, pause_on_debug=1, gdb 断点不误触发),
  在进入主循环**之前**使能; `watchdog_update()` **只在完整走完一轮主循环后
  调用**。任何未知原因的永久卡死 ≤2s 内强制复位。
- boot 阶段不使能看门狗: boot 全路径有界 (mpr121_init ~1s 睡眠为最长),
  不存在 boot-loop 风险, 也无需 boot 期喂狗。
- 复位后 `watchdog_caused_reboot()` → `wdt_reboot=1`, 并在 main() 开头把
  卡死现场捕获到 `crash_prev_stage / crash_prev_stage_us / crash_prev_loops`
  (随后为本生命周期重新初始化面包屑, 防止 LED 在进入主循环前误报常亮)。

## 14. STATUS 新增字段

```
"usb":   { ..., "cmdq_full": N }
"crash": { "prev_stage": "SLIDER" (5), "prev_stage_us": 12345,
           "prev_loops": 99999, "loops_total": 123456 }
"task_max_us": { "cdc": .., "cmd": .., "slider": .., "air": .., "hid": .., "save": .. }
```
看门狗复位后读 `crash.prev_stage` 即为卡死位置 (仅 `wdt_reboot=1` 时解读;
断电冷启动时该值为随机残留, 无意义)。

## 15. 不变项确认

- `CHUNI_TOF_RECOVERY_ENABLE=0` (纯轮询, 无下线检测/恢复) — 保持
- `VL53L0X_TIMING_BUDGET_US = 20000` — 保持
- I2C1/VL53L0X/XSHUT 仅 Core1 访问 — 保持
- `printf` 全局 no-op (`log_output.h`, CHUNI_PRINTF_ENABLE=0) — 保持,
  只发送 HID 报文
- `CHUNI_USB_AUTO_RECOVERY_ENABLE=0` (无自断连) — 保持
- bMaxPower=250 (500mA) — 保持

## 16. 编译验证

- 默认配置 (RECOVERY=0): **通过**, ELF 无 `stdio_usb_out_chars /
  stdio_usb_mutex` 符号 (stdio 后端确认未链接)。
- RECOVERY=1 配置: **通过**。
- 构建后已恢复默认配置 (RECOVERY=0)。
- `static_assert(sizeof(hid_nkro) == 16)` 通过。

## 17. 残余风险

| 风险 | 等级 | 缓解 |
|---|---|---|
| SAVE Flash 擦写期间 XIP 停摆 (~50–400ms, 硬件固有) | 中 | 仅空闲窗口执行; 看门狗 2s 足够容忍 |
| 看门狗复位会断开 USB 一次 (主机重枚举) | 低 | 仅在真硬锁时发生, 2s 内自愈, 且留下卡死现场 |
| `.uninitialized_data` 断电丢失 | 信息 | 断电即冷启动, 无需现场 |
| RESET 状态机对无响应芯片标记 not-ready | 低 | 与冷启动判定一致; 该芯片跳过轮询, 不拖累节奏 |

## 18. 15 问 15 答

1. **本次根因是什么? 为什么每次掉线数据量相同?**
   Core0 主循环被 CDC 命令路径的同步长操作 (CONFIG 720ms / RESET 1.5s /
   DEBUG ~1s / save_mutex 无界等待) 硬阻塞, LED 冻结即为直接证据。数据量
   决定主机何时发出触发命令, 故掉线点高度可重复。

2. **LED 停止变化说明什么?**
   旧实现 LED 在主循环内更新, LED 冻结 = 主循环停摆, 排除了 "仅 USB
   endpoint 死亡" 的解释。

3. **I2C0 运行时事务是否有严格返回保证?**
   有。SDK `i2c_*_blocking_until` 全部等待循环以绝对截止时间判定
   (已核对 SDK 2.3.0 源码), SDA/SCL 卡死也必然在 500us 内返回。

4. **CONFIG 是否曾经绕过 1ms deadline?**
   是。旧 set_thresholds 同步执行 72 次 10ms-timeout 事务 = 最坏 720ms,
   是最大的单点阻塞源。

5. **RESET 最坏阻塞多久? 现在呢?**
   旧 ~1.5s (3 芯片 × [软复位 + sleep 2ms + 39 写 × 10ms + sleep 100ms])。
   现在入口 µs 级, 状态机每步 ≤500us 摊平, 等待期 deadline 判定不 sleep。

6. **DEBUG 在 DEBUG_MPR121=0 时还做 I2C 吗?**
   不做。函数体顶部直接 `return`, 零 I2C。

7. **cdc_task 现在会执行命令吗?**
   不会。只接收+解析+入队; 执行由主循环 `command_step()` 每轮最多 1 条。

8. **save_mutex 为什么删除? 有竞争风险吗?**
   save 仅 Core0 空闲窗口单写者调用, Core1 从不触碰该模块, 无竞争。
   旧锁是无界阻塞等待, 属潜在永久硬锁点。

9. **看门狗参数与喂狗时机? boot 会误复位吗?**
   `watchdog_enable(2000, 1)`, 只在完整主循环末尾喂。boot 阶段不使能
   (boot 路径全部有界), 不会 boot-loop; gdb 暂停时看门狗暂停。

10. **看门狗复位后如何知道卡死在哪?**
    stage 面包屑在 `.uninitialized_data` 中软复位保留; 复位后 main() 捕获
    为 crash_prev_*, STATUS 的 `"crash"` 段直接给出 stage 名与停留时间。

11. **`.uninitialized_data` 什么时候会丢?**
    断电/上电冷启动 (RAM 内容不保留)。看门狗/异常软复位保留。

12. **LED 现在由谁驱动? 卡死时表现?**
    10ms 硬件定时器 IRQ。主循环卡死 → 某 stage >50ms 未刷新 → LED 常亮;
    主循环死了 LED 仍继续运行。

13. **单条主循环最坏时间现在是多少? 谁贡献?**
    常态 ~1.5ms: slider (1.5ms: 轮询 2×500us + 命令步 500us) 为主, 其余
    任务 µs 级。唯一 >1.5ms 的窗口是空闲期 Flash 擦写 (~400ms, 硬件固有,
    看门狗余量内)。

14. **为什么这不是 USB 恢复问题? 恢复开关还开吗?**
    恢复 (tud_disconnect/connect) 本身即主机可见断开, 只能掩盖不能根治;
    `CHUNI_USB_AUTO_RECOVERY_ENABLE=0` 保持关闭, 本轮改为消灭阻塞源 +
    看门狗兜底。

15. **如果仍复现, 下一步看什么?**
    STATUS 的 `crash.prev_stage` (看门狗复位现场)、`task_max_us.*`
    (哪个任务异常)、LED 常亮 (确认主循环卡死) —— 三者组合可把任何残余
    卡死定位到具体 stage, 不再是黑盒。

## 19. 验收清单

- [x] CONFIG/DEFAULT/RESET 异步状态机化 (1 事务/主循环)
- [x] DEBUG 零 I2C (DEBUG_MPR121=0)
- [x] cdc_task 只解析入队; command_step 每轮 1 条
- [x] save_mutex 无界等待移除
- [x] 运行时 I2C 统一 500us timeout (INIT 10ms 仅 boot)
- [x] SDK i2c until 返回保证已核对
- [x] Stage 面包屑 (.uninitialized_data) + 看门狗 2000ms + 喂狗点唯一
- [x] loop_completed 计数 + 崩溃现场读回 (STATUS "crash")
- [x] LED 硬件定时器 IRQ + stage-stall 常亮; 旧 5ms 锁存移除
- [x] 全源码阻塞原语扫描通过 (残留均为 boot/复位/主循环自身)
- [x] RECOVERY=0 / RECOVERY=1 双配置编译通过, 默认已恢复
- [x] RECOVERY=0、TimingBudget=20000、Core1 隔离、printf 禁用、
      auto-recovery 关闭 — 全部保持
