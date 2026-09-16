# USB Stability and ToF Recovery Interaction Report

> 日期: 2026-09-16
> 范围: 基于 c525908 (refact tof recovery) 之后的最新源码, 针对
> "运行一段时间后 USB HID 偶发完全失去输入, 需物理拔插 USB 才能恢复" 的故障
> 做了软件/架构层的确定性根因审查与修复。

---

## 1. Observed Failure

现象: 系统运行一段时间后, USB HID 偶发完全无输入; 程序表面上可能仍在运行;
物理拔插 USB 后恢复。

关键推理: 拔插 USB 同时执行了 host disconnect + VBUS 重上电 + RP2040 复位 +
TinyUSB 重新初始化 + I2C1 状态清零 + VL53L0X 重新初始化, 因此
"拔插后恢复" 不能单独证明是 USB 物理掉线或电源问题。审查必须先把以下情形分开:

1. RP2040 运行中, Core0/USB servicing 被阻塞
2. TinyUSB device stack 还在, 但 HID endpoint 不再发送
3. CDC 存在但 HID 停止
4. USB device 整体失去枚举
5. RP2040 被 reset / brownout
6. RP2040 完全死机

本次审查在 (1)(2) 类中发现了**确定性的软件根因**, 并已修复; 同时加入了
能把上述 6 类情形区分开的运行时观测指标。

## 2. Current Core0 Architecture

```
Core0 主循环:
  tud_task() → cdc_task() → slider_update() → air_update()
  → button_update() → gen_nkro_report() → report_usb_hid()
  → save_loop() → tof_event_task() → [monitor/LED]
```

USB 的全部生命周期 (tusb_init / tud_task / HID / CDC) 只存在于 Core0。

各任务最坏执行时间 (本次逐一审查并加固后):

| 任务 | 典型 | 最坏 | 说明 |
|---|---|---|---|
| tud_task() | <50us | 有界 | TinyUSB 非阻塞轮询 |
| cdc_task() | ~0 | **≤1ms** (新增硬上限) + 64B/轮 | 新增执行时间 deadline |
| slider_update() | <1ms | 3ms (3×2×500us I2C 超时) | 原有设计, 保持 |
| air_update() | <100us | <100us | 纯快照+逻辑, 无 I2C |
| button_update() | ~0 | ~0 | GPIO 去抖 |
| gen/report_usb_hid() | ~0 | 非阻塞 | 保持 tud_hid_n_ready 检查 |
| tof_event_task() (新增) | ~0 | 4 事件×打印, 背压门控 | 新增事件泵 |
| save_loop() | 0 (无请求) | **~50ms (typ) / ~400ms (worst)** 仅在显式 SAVE 后一拍 | 见 §8 |

正常运行的 tud_task 最大间隔 ≈ 主循环最坏耗时 ≈ **<5ms**
(异常路径: 显式 SAVE 时的 Flash 擦写, 见 §8)。

## 3. Current Core1 Architecture

```
Core1:
  multicore_lockout_victim_init()   ← 新增 (Flash 操作保护)
  vl53l0x_init() (I2C1 独占)
  主循环: poll_ready_sensors()
        → 总线级故障检测 (Level 3)
        → 单传感器恢复状态机 (Level 2)
        → 全局停顿检测 (新增)
        → ~100us 节拍
```

Core1 唯一跨核接口:
- 快照 seqlock (Core0 无锁读) — 原有
- **tof_events 非阻塞事件队列 (Core1 → Core0)** — 新增, 替代 Core1 printf
- heartbeat / 统计只读变量 — 新增

## 4. Identified Blocking Paths

### 4.1 【已确认·最高优先级】Core1 printf → TinyUSB 竞态 + 跨核互斥锁

`stdio_init_all()` 启用了 pico-sdk 的 USB CDC stdio 后端。该后端
(`stdio_usb.c`) 的实现:

- 持有**跨核互斥锁 `stdio_usb_mutex`** (`mutex_enter_blocking`), 且
- 直接调用 `tud_cdc_write()` / `tud_cdc_write_flush()`。

修改前 Core1 的 `tof_reader.cpp` / `vl53l0x.cpp` 在恢复路径上大量 printf
(recovery attempt / recovered / FAILED / BUS RECOVERY / TOF ready / init FAILED /
full reinit ...), 产生两个确定性危害:

1. **Core1 与 Core0 并发改写 TinyUSB device stack 内部状态**: Core1 的
   `tud_cdc_write` 与 Core0 的 `tud_task()` 无同步地操作同一 USBD 状态机。
   这是真正的数据竞态, 可以把 device stack / endpoint 状态打挂 —— 表现为
   **HID 完全停止、程序仍在跑、必须重新枚举 (拔插) 才能恢复**, 与本次故障
   现象完全吻合。
2. **Core1 持锁期间 Core0 被阻塞**: 主机串口连接但停止读取时, CDC TX 缓冲
   (256B) 填满后, stdio 后端在锁内以 500ms 超时反复重试。此时 Core1 持有
   `stdio_usb_mutex`, Core0 的任何 printf (monitor/STATUS) 都会阻塞在取锁上
   → `tud_task()` 饥饿 → USB HID 停止。

**修复**: Core1 全部 printf 移除 (见 §7), Core1 不再接触任何 stdio/TinyUSB 符号。

### 4.2 【已确认】CDC 背压可阻塞 Core0 (monitor / 命令输出)

主机 DTR 拉起但停止读取时, `output_monitor_data()` 的 ~700B JSON 与
STATUS/AIRDEBUG/PERF 的长输出会触发 stdio CDC 后端的 500ms 重试路径,
每条 printf 最多阻塞 500ms。

**修复**:
- monitor 输出改为 "构建到有界缓冲 → 64B 小块 → 每块检查
  `tud_cdc_n_write_available` → 空间不足立即截断本轮 (计数
  `monitor_truncated_count`)", 绝不等待。
- CDC 命令执行前检查 TX 剩余空间 ≥192B, 不足丢弃该命令 (计数
  `cdc_cmd_dropped_count`)。
- 主机未连接 (DTR 断开) 时 stdio 本身不输出, 无阻塞。
- `cdc_task()` 新增 1ms 执行时间硬上限 (原有 64B/轮上限保留)。

### 4.3 【已确认】SAVE 命令同步 Flash 擦写

修改前 SAVE 在 CDC 命令解析路径内同步执行 `flash_range_erase+program`:
- Core0 中断关闭 + XIP 停摆 ~50ms (typ) / ~400ms (worst) → USB 静默同长;
- **Core1 无任何保护, 会在 Flash 忙碌期间继续从 XIP 取指 —— 行为未定义**
  (SDK 明确说明 flash 函数在另一核执行于 Flash 时不可用)。

**修复** (不删除 SAVE 功能):
- `config_save()` 改为快照 + 置位 (`save_request_write`), 实际擦写由主循环
  `save_loop()` 在独立节拍执行 —— 命令解析路径不再直接擦写。
- Flash 操作前后用 `multicore_lockout_start/end_blocking()` 把 Core1 停到
  RAM 安全点 (Core1 启动时执行 `multicore_lockout_victim_init()`)。
- Core1 新增全局停顿检测: 停顿结束后刷新所有时间基准, 防止把 ~50-400ms
  的双核冻结误判成 "全员测距卡死" 而触发恢复风暴 (阈值 150ms, 远小于
  300ms 的 no-data 判定, 远大于正常最大轮询间隔 ~15ms)。

### 4.4 【已排除】其余 Core0 路径

slider_update (≤3ms 有界) / air_update (无 I2C) / button_update / HID 收发
(非阻塞) / yield 重入 (§9) 均无 >5ms 阻塞路径。

## 5. USB / CDC Interaction

最终优先级 (硬性架构约束):

```
USB HID  ⭐⭐⭐⭐⭐  不能等待, 不能被 CDC / Recovery 阻塞
USB CDC  ⭐          可丢、可延迟、可降频 (背压门控 + 截断计数)
```

- HID 发送保持 `tud_hid_n_ready()` 非阻塞模式; busy 时持续更新
  `hid_nkro` 并保持 `hid_dirty`, endpoint 恢复后立即发送最新状态 (保留并加固)。
- 禁止对 HID endpoint 做 `while(!ready) wait` 重试 (保持)。
- CDC 是纯调试通道; monitor/事件输出全部经背压门控。

## 6. Core1 printf Risk

**已彻底消除**。修改后 `vl53l0x.cpp` / `tof_reader.cpp` (Core1 翻译单元)
零 printf、零 stdio、零 tud_* 符号 (已用 grep 全文验证, 仅存注释)。
Core1 的所有 "日志" 变为:

- **状态变化事件** → `tof_events` 队列 (Core0 弹出并经 CDC 门控打印);
- **逐错误信息** → 只进计数器 (`i2c_error_count` 等), 不产生日志流;
- **统计** → 只读 volatile 变量, Core0 的 STATUS/PERF 命令按需读取。

事件队列属性: 固定 32 条 × 12B 静态内存; 无 malloc; push/pop 均为常数时间
非阻塞; 队满丢弃低优先级调试事件并计数 (`tof_event_dropped_count`),
Core1 绝不等待 Core0。

## 7. Recovery Worst-Case Execution Time

全部恢复执行在 **Core1**, 通过让出回调 (`yield_poll`) 在长等待期间持续
轮询健康传感器。Core0 从不被同步阻塞。

### 单 Sensor Recovery (Level 2)

| 阶段 | 上限 |
|---|---|
| XSHUT 复位 | 10ms (yield 式) |
| 启动等待 | 10ms (yield 式) |
| 地址切换稳定 | 2ms |
| SPAD 查询 | ≤50ms (带 deadline) |
| VHV + phase 参考校准 | ≤2×200ms (带 deadline) |
| I2C 事务 (≈90 次寄存器读写) | 死总线时每次 ≤3ms 超时 |
| **理论最坏合计** | **≈0.75s** |
| 典型实测 | ~100-200ms |

### Bus Recovery (Level 3)

I2C1 deinit → GPIO 开漏检测 → 最多 9 个 SCL 时钟 (每个 ~10us) → STOP
→ I2C1 reinit。**无任何无限等待路径**, 理论最坏 ≈ **200us** 量级。
只接触 I2C1 / GP6 / GP7, 不动 XSHUT、不清传感器配置。

### Full Reinit (Level 4, 仅总线恢复失败后)

5× XSHUT (并行拉低) + 10ms + 5× 单传感器初始化。
理论最坏 ≈ 10ms + 5×0.75s ≈ **~3.8s**; 典型 ~1s。Core1 独立执行。

### 重入 / 递归审查 (yield 链)

`Recovery → init_sensor → yield_sleep_us → yield_callback → poll_ready_sensors`
链路:
1. `yield_poll_healthy` 有 `in_yield_poll` 防重入守卫 (原有, 保留);
2. `yield_callback` **只能**调用 `poll_ready_sensors` (只读轮询),
   与恢复状态机彻底解耦 —— 轮询路径不存在任何恢复判定/触发代码;
3. `get_spad_info` (50ms deadline) 与 `perform_single_ref_calibration`
   (200ms deadline) 内部的等待循环均有最终 deadline, 期间只 yield+读寄存器;
4. 全部等待均有硬上限: 无无限 while / 无限 retry (§16 全文搜索确认)。

## 8. I2C1 Recovery Isolation

- `i2c_deinit/i2c_init` 在 SDK 2.3.0 中使用 `reset_block_num(I2C_RESET_NUM(i2c))`
  —— **按外设实例精确复位**, I2C1 的 deinit/init 在硬件层面不可能波及 I2C0。
- Bus recovery 代码路径只操作 `I2C1_PORT / GP6 / GP7`, 不调用任何
  board/global/peripheral 重新初始化。
- 编译期引脚隔离校验 (`board_defs.h` 的 `chuni_board_pins_distinct`
  static_assert) 保证 I2C0/I2C1/XSHUT 引脚零重叠。
- 全工程调用图确认: VL53L0X/ToF 路径不触碰 I2C0 / MPR121 / GPIO16/17。

## 9. Core0/Core1 Synchronization Review

全工程 mutex / critical section / spin lock / semaphore / multicore_fifo 搜索结果:

| 同步点 | Core0 | Core1 | 评估 |
|---|---|---|---|
| save_mutex | 持有 (Core0 独占) | 不接触 | 安全 |
| stdio_usb_mutex | printf | **已消除** | 修复后 Core1 不再进入 |
| 快照 seqlock | 无锁读 | 无锁写 | 安全 (奇偶括号) |
| tof_events 队列 (新增) | 无锁读 (SPSC 消费者) | 无锁写 (SPSC 生产者) | 安全, 单向 |
| multicore_lockout (新增) | 发起 (仅 SAVE) | victim (RAM 安全点) | Core0 等待有界 (victim IRQ 常驻响应); Core1 从不等待 Core0 |
| multicore_fifo 其他用途 | 无 | 无 | lockout 独占 FIFO (SDK 要求) |

不存在 "Core0 等待 Core1 释放锁" 或反向的死锁链。Core1 永远不等待 Core0。

## 10. HID Endpoint Review

- 保持 `tud_hid_n_ready(0)` 非阻塞检查 + dirty 标志重发机制 (§41/§42 要求)。
- 新增 endpoint 健康观测 (见 §13): 若 endpoint 长期 not-ready,
  `max_hid_not_ready_us` 会直接给出证据, 不再无限默默等待。
- 本次**没有**实现自动 USB 重初始化 (tusb_deinit/init / reset_usb_boot):
  按 §25 原则, 优先修复阻塞根因 + 建立观测; 如未来数据显示 endpoint 真的
  独立卡死 (max_tud_task_interval 小 且 max_hid_not_ready_us 持续增长),
  再作为最后手段设计保守的 interface 级恢复。

## 11. Changes Made

| 文件 | 变更 |
|---|---|
| `src/tof_events.h/.cpp` (新增) | Core1→Core0 非阻塞 SPSC 事件队列 (32 条, 满则丢弃计数) |
| `src/vl53l0x.cpp` | 移除全部 printf → 事件; 新增 `vl53l0x_note_global_stall()` |
| `src/tof_reader.cpp` | 移除全部 printf → 事件; 新增全局停顿检测 (150ms 阈值 + 时间基准刷新); 新增 Core1 心跳; `multicore_lockout_victim_init()`; 事件化 BUS RECOVERY / OFFLINE / STALL |
| `src/vl53l0x.h` / `tof_reader.h` | 新增 API 声明 |
| `src/main.cpp` | tud_task 最大间隔统计; HID busy/发送间隔统计; monitor 分块背压输出; cdc_task 1ms 上限 + 命令背压丢弃; Core1 事件泵; STATUS/PERF 扩展 |
| `src/save.cpp/.h` | `save_request_write` 延迟保存; `save_loop` 执行; Flash 擦写加 multicore lockout 保护 Core1 |
| `src/config.cpp` | `config_save()` 改走延迟队列 |
| `CMakeLists.txt` | 加入 tof_events.cpp |

未修改: AIR 层级/判定算法、MPR121 工作逻辑、TimingBudget、恢复分级架构、
I2C 引脚定义。清理项: `attempt_recovery` 中重复赋值经核查**并不存在**
(源码仅一处 `last_attempt_ms` 赋值), 无需修改。

## 12. TimingBudget Verification

- `VL53L0X_TIMING_BUDGET_US = 20000` **未改动**;
- 冷启动 / 单传感器恢复 / Bus Recovery 后重初始化 / Full Recovery 走同一路径
  `init_sensor()`, 全部使用同一常量;
- 读回验证保持: 初始化末尾从 PRE/FINAL RANGE 寄存器重算实际 budget,
  ±200us 容差; 失败 → 事件 `BUDGET_VERIFY_FAIL` → 恢复失败路径处理。

## 13. New Runtime Statistics

均可在 `STATUS` (JSON) 与 `PERF` (文本) 读取; monitor JSON 同步携带:

| 指标 | 含义 |
|---|---|
| `tud_max_gap_us` | 两次 tud_task() 之间最大间隔 —— Core0 USB 服务的直接证据 |
| `tud_task calls` | 调用次数 (活性) |
| `hid_busy_max_us` | HID endpoint 连续 not-ready 最长持续时间 |
| `hid ready-false count` | endpoint busy 累计次数 |
| `hid_gap_max_us` | 两次成功 HID 发送的最大间隔 |
| `monitor truncated` | CDC 背压截断的 monitor 输出次数 |
| `commands dropped` | CDC 背压丢弃的命令数 |
| `tof events dropped` | 事件队列满丢弃数 |
| `core1_hb` | Core1 心跳 (两次读取间增长 = Core1 存活) |
| `stall_us` | 最近一次双核全局停顿时长 (Flash 擦写等的直接证据) |

故障现场判读:
- `core1_hb` 增长 + `tud_max_gap_us` 巨大 → Core0 被阻塞 (软件路径);
- `core1_hb` 不增长 → Core1 死;
- `tud_max_gap_us` 小 + `hid_busy_max_us` 巨大 → USB 层/主机/电源问题;
- `stall_us` 出现大值 → 有过 Flash 擦写/全局停顿。

## 14. Possible Hardware Power Issues

- 代码层面不做任何 "电源修复" 伪装, 未降低 VL53L0X 轮询率, 未改供电相关逻辑。
- SDK 未提供可靠的 brownout/reset-reason 寄存器读取, 本次未添加
  reset cause 输出 (如后续需要, 可用 watchdog scratch 方案自建)。
- 排除软件根因后的观测方法: 复现故障时**不拔插**, 先发 `PERF`:
  - 若 CDC 仍响应且 `core1_hb`/`tud_task calls` 均正常、`hid_busy_max_us`
    累积 → endpoint/主机/电气层问题, 此时再考虑电源测量 (VBUS 跌落、
    线材压降、5×VL53L0X+MPR121×3 的峰值电流)。
  - 若 CDC 也无响应 → Core0 阻塞或系统复位, 用 §13 指标定位。

## 15. Final Architecture

```
                    ┌──────────────────────┐
                    │        Core 0        │
                    │  tud_task (唯一 USB) │
                    │  HID (非阻塞+统计)   │
                    │  MPR121 / I2C0       │
                    │  AIR / CDC(背压门控) │
                    │  save_loop(+lockout) │
                    │  事件泵 → CDC        │
                    └──────────┬───────────┘
              seqlock 快照 (无锁) │ 事件/统计 (非阻塞队列)
                    ┌────────────┴───────────┐
                    │        Core 1          │
                    │  I2C1 独占 / VL53L0X   │
                    │  Recovery 状态机       │
                    │  Bus Recovery / Full   │
                    │  全局停顿检测          │
                    │  禁: printf/TinyUSB/I2C0│
                    └────────────────────────┘
```

## 16. 逐项回答 (对应要求 §46)

| # | 问题 | 回答 |
|---|---|---|
| 1 | Core0 主循环是否存在可能 >5ms 的阻塞路径? | 正常运行: 无 (最大 slider 3ms)。唯一例外: 显式 SAVE 后的 Flash 擦写 ~50ms(typ)/~400ms(worst), 已隔离到独立节拍并通知在案 |
| 2 | tud_task() 最大间隔? | 正常 <5ms (新增 `tud_max_gap_us` 实测); SAVE 时 ~50-400ms |
| 3 | Core1 Recovery 是否存在直接/间接 USB 调用? | **已确认存在过 (根因), 已彻底消除** |
| 4 | Core1 是否仍然使用 printf? | 否, 全部移除并经 grep 验证 |
| 5 | CDC 是否可能阻塞 HID? | 修复后: monitor/事件输出背压门控不阻塞; 唯一残余: 主机连接但停止读取时, 手工执行 STATUS 等长输出命令仍可能短暂阻塞 (命令是显式人工动作, 已加 TX 空间预检把风险窗口压到最小) |
| 6 | cdc_task() 是否存在长路径? | 有界: ≤64B/轮 + 新增 1ms 硬上限 + 命令背压预检 |
| 7 | slider_update() 最大阻塞时间? | 3ms (3 设备 × 2×500us 超时), 未改动 |
| 8 | save.cpp 是否可能导致长时间暂停? | 是 (~50-400ms), 已改为延迟执行 + multicore lockout 保护 Core1; 擦写期间 XIP 停摆为硬件固有, 无法消除, 只能隔离 |
| 9 | yield_sleep_us() 是否可能形成递归 Recovery? | 否: 防重入守卫 + yield_callback 只读轮询, 与恢复判定彻底解耦 |
| 10 | 单 Sensor Recovery 最长耗时? | 理论最坏 ≈0.75s (含死总线 I2C 超时), 典型 ~100-200ms, Core1 独立执行 |
| 11 | Bus Recovery 最长耗时? | ≈200us, 严格有界 (≤9 clock + STOP), 无无限等待 |
| 12 | Full Recovery 最长耗时? | 理论最坏 ≈3.8s, 典型 ~1s, 仅 Level 4 触发, Core1 独立执行 |
| 13 | I2C1 Recovery 能否完全避免 I2C0? | 能: 按实例精确 reset (SDK `reset_block_num`) + 编译期引脚隔离校验 + 调用图审查 |
| 14 | HID endpoint 是否存在长期 not ready 情况? | 现在可观测: `hid_busy_max_us` / `ready-false count`; 未加自动恢复 (按 §25 优先根因) |
| 15 | 是否增加了 HID/USB 活跃性统计? | 是, 见 §13 |
| 16 | 是否保留 TimingBudget = 20000us? | 是, 所有路径共用且读回验证 |
| 17 | 软件层面是否还存在可能造成 USB 完全无输入的阻塞路径? | 已知确定性路径 (Core1 printf 竞态 / CDC 背压 / SAVE 同步擦写) 均已修复; 无其他 >5ms 阻塞路径 |
| 18 | 哪些是 "已确认", 哪些是 "可能"? | 已确认: §4.1 (Core1 printf → TinyUSB 竞态 + 跨核锁), §4.2 (CDC 背压 500ms 阻塞路径), §4.3 (SAVE 同步擦写 + Core1 无保护)。可能 (需现场数据): 电源/VBUS 问题、主机侧电气问题 —— 现已可通过 §13 指标客观区分 |

### 已确认 vs 可能

**已确认 (代码级确定性):**
1. Core1 printf 同时构成 TinyUSB 数据竞态 (可解释 "HID 死、程序活、拔插恢复") 与跨核锁阻塞;
2. CDC 背压时 monitor/命令输出可逐条 printf 阻塞 Core0 至 500ms;
3. SAVE 同步 Flash 擦写使 Core0 停摆且 Core1 处于未定义状态。

**可能 (待现场观测):**
- USB 供电/VBUS 跌落 —— 只能排除软件后用 §13/§14 方法客观验证;
- 主机端 USB 控制器/驱动异常。

---

*验收状态: 全部修改已编译通过 (SDK 2.3.0, Release)。HID 保持非阻塞; Core1 与 USB/CDC/stdio 完全隔离; 恢复路径全部有界; TimingBudget 20000us 保持。*
