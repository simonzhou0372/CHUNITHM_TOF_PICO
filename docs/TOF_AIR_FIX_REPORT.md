# TOF/AIR 系统稳定性与 TimingBudget 全链路修复报告

> 日期: 2026-09-03
> 范围: VL53L0X 驱动全量重写 + Core1 恢复状态机 + I2C1 总线恢复
> 不变项: AIR 检测算法 / MPR121 / Slider / USB HID / GPIO1 轮询方式 均未修改
> 参考基准: `.ref/VL53L0X.cpp` (Pololu VL53L0X Arduino 库, 本项目 README 声明的参考实现)

---

## 1. 问题概述

实际使用中暴露的症状:

| 症状 | 表现 |
|------|------|
| 迟触发 / 漏触发 | AIR 按键响应慢, 手快时偶尔丢触发 |
| 距离值可疑 | 部分传感器距离系统性偏移 |
| 传感器集体失联 | 一次 I2C 异常后 5 颗 TOF 同时无数据, 必须重新上电 |
| 单颗死亡拖累全局 | 一颗传感器故障后长时间运行数据质量劣化 |

根本原因不是单一 bug, 而是旧驱动从未真正完成初始化: **时序参数写错了寄存器**, 且**没有任何故障检测与恢复机制**。

## 2. 根因分析

### 2.1 已确认 (Confirmed)

**R1. TimingBudget 从未配置成功 —— 时序参数写到了错误的寄存器**

对照 `.ref/VL53L0X.h` 寄存器表逐个核实旧驱动 `vl53l0x.cpp` 的写入:

| 旧代码写入 | 旧代码意图 | 实际寄存器含义 | 后果 |
|-----------|-----------|---------------|------|
| `write_reg(0x27, 0x08)` | pre-range VCSEL 周期 | `PRE_RANGE_CONFIG_MIN_SNR` | **覆写了 pre-range 最小信噪比阈值** → 测量有效性判定被破坏 → 漏触发 |
| `write_reg16(0x28, 0x0080)` | pre-range timeout | `ALGO_PART_TO_PART_RANGE_OFFSET_MM` | **覆写了出厂 part-to-part 偏移校准** → 距离系统性偏移 |
| `write_reg16(0x2A, 0x0080)` | final-range VCSEL | 未定义寄存器 | 未知配置区被污染 |
| `write_reg(0x21, 0x06)` | 序列配置 | 未定义寄存器 (序列配置在 `0x01`) | 序列配置留在 boot 默认值 |
| 未写 `0x50/0x51/0x70/0x71` | — | 真正的 VCSEL/timeout 寄存器 | **从未被配置** |

结论: 旧代码注释声称的 "MeasurementTimingBudget = 20ms" **从未存在过**。传感器实际以 boot 默认配置 (约 33ms 级) 运行, 且三个校准/阈值寄存器被垃圾值覆盖。

**R2. 中断清除失败被静默忽略 → 旧数据标新 (stale-as-fresh)**

旧驱动的读取路径: 读中断状态 → 读距离 → 写 `0x0B` 清中断 (不检查返回值) → 报告数据。
若清中断的 I2C 写失败, 下一次轮询中断状态仍为"就绪", 会**再次读到同一次旧测量并当作新数据上报**, 时间戳被刷新 —— AIR 层判断和调试界面都被欺骗。

**R3. 连续测距启动方式错误**

旧代码直接写 `SYSRANGE_START = 0x03`, 既无 back-to-back 前导码 (stop variable 写回), 模式值也与 Pololu 参考的 `0x02` (MODE_BACKTOBACK) 不符。DSS (动态 SPAD 选择) 启用时 stop variable 前导码是必需的。

**R4. 初始化无任何返回值检查**

`write_reg_list` 为 void, stop variable 读取不检查, 任何一步 I2C 失败都被无视, 传感器带着半套配置"成功"上线。

### 2.2 很可能 (Likely)

**R5. 五颗传感器同时失联 = I2C1 总线锁死, 无恢复机制**

一次总线异常 (EMI / 上电时序 / 从机卡死) 使 SDA 被从机拉低后, 硬件外设无法自行恢复; 旧代码既没有总线恢复 (9-clock + STOP), 也没有 XSHUT 复位路径, 只能整机断电。

**R6. 单颗传感器故障永久化**

旧驱动对单颗传感器的 I2C 错误只做计数, 无下线、无退避、无恢复 —— 死亡传感器每轮轮询都产生 2 次 3ms 超时事务, 拖慢轮询节奏并使 AIR 该通道永久无效。

### 2.3 潜在 (Potential)

**R7. Core0 与 Core1 同时触碰 I2C1**

旧架构在 Core0 上调用 `vl53l0x_init()` (I2C 外设初始化 + 传感器配置), Core1 随后立即开始轮询。双核并发访问同一 I2C 外设存在启动期竞态窗口 (本次已一并消除: I2C1 完全归 Core1 所有)。

## 3. TimingBudget 验证

**统一常量 (唯一配置源)**:

```c
// src/vl53l0x.cpp
constexpr uint32_t VL53L0X_TIMING_BUDGET_US = 20000u;   // 20ms = ST 官方最小值
```

冷启动 (`vl53l0x_init`)、单传感器恢复 (`vl53l0x_recover_sensor`)、全量恢复 (`vl53l0x_reinit_all`) 三条路径共用同一个 `init_sensor()`, 其中只调用一次:

```c
if (!set_measurement_timing_budget(new_addr, VL53L0X_TIMING_BUDGET_US)) return false;
```

**寄存器级证据链 (冷启动 → API → 计算 → VCSEL → timeout 寄存器 → 连续模式):**

1. **tuning settings 加载** (80 项, 逐项对照 Pololu `init()` 第 138-233 行): `0x50=0x06` (pre VCSEL 14 PCLKs), `0x51/0x52=0x0096` (pre timeout), `0x70=0x04` (final VCSEL 10 PCLKs), `0x46=0x25` (MSRC timeout), 序列配置随后写 `0xE8` (pre_range + final_range + dss)。
2. **API**: `set_measurement_timing_budget(addr, 20000)` — Pololu `setMeasurementTimingBudget()` 逐行移植。
3. **计算** (0xE8 序列):
   - Start/End overhead: 1910 + 960 = 2870 µs
   - DSS (启用): 2 × (690 + msrc_dss_tcc_us) = 2 × (690 + 2029) = 5438 µs
   - Pre-range: 660 + pre_range_us = 660 + 8061 = 8721 µs (151 MCLKs × 53384 ns)
   - Final overhead: 550 µs
   - 已用合计: **17579 µs** → final 剩余 **2421 µs**
4. **VCSEL → MCLK 换算**: `usToMclks(2421, 10 PCLKs) = 63 MCLKs`; FINAL timeout 寄存器统计 pre+final 总 MCLK: `63 + 151 = 214` → `encodeTimeout(214) = 0x00D5` → **写入 `0x71/0x72 = 0x00D5`** (PRE timeout `0x51/0x52 = 0x0096` 保持不变)。
5. **连续模式**: stop-variable 前导码 (`0x80/0xFF/0x00/0x91/0x00/0xFF/0x80`) + `SYSRANGE_START = 0x02` (back-to-back)。
6. **读回验证 (init 必经门限)**: `get_measurement_timing_budget()` 从 `0x51/0x71` 寄存器**重算**实际 budget: `decode(0x00D5) − 151 = 63 MCLKs → 2402 µs`; 总计 `2870 + 5438 + 8721 + 550 + 2402 = 19981 µs`; `|19981 − 20000| = 19 µs ≤ 200 µs` 容差 → **验证通过**。任何偏差超容差, 该传感器初始化直接失败, 不允许带病上线。

启动日志每颗传感器打印实际值 (示例):

```
[VL53L0X] TOF1 ready: addr=0x30 TB=19981us spad=9 pre_reg=0x0096 fin_reg=0x00D5 status=0x0019
```

**结论: 最终实际运行 TimingBudget = 19981 µs ≈ 20 ms (MCLK 量化粒度 ≈ 38 µs, 偏差 19 µs 为量化舍入), 由寄存器读回验证强制保证。**

## 4. I2C 故障分析

| 故障类型 | 检测方式 | 响应 |
|---------|---------|------|
| 单事务失败 (NACK/超时) | `i2c_*_blocking_until` 返回值, 3ms 事务超时 | 计入 `consecutive_errors` + 累计错误计数; **不更新快照** |
| 测量未完成 (正常) | 中断状态寄存器 `0x13` bit0-2 == 0 | 返回 `NOT_READY`, 不计数不报错 —— 与通信失败严格区分 |
| 传感器死亡 | 连续 10 次通信错误 (~60ms 内累积) | 单传感器恢复 |
| 测距序列卡死 | I2C 正常但 500ms 无新数据 (20ms 周期下应约有 25 次) | 单传感器恢复 |
| 总线锁死 | 单轮 ≥3 颗失败, 或就绪 ≥2 颗且本轮全部事务失败; 连续 2 轮确认 | 9-clock 总线恢复 + 全传感器 XSHUT 恢复 |

关键区分: **"not ready" ≠ "I2C failure"**。只有真正的通信失败才计入错误计数; 测量进行中是正常状态。

## 5. 传感器恢复架构

```
状态: OK ──(10次连续I2C错误 / 500ms无新数据)──► RECOVERING ──成功──► OK
                                        │
                                        └──失败──► OFFLINE ──退避到期重试──► RECOVERING
```

- **恢复动作**: XSHUT 拉低 10ms → 释放 → `init_sensor()` 完整重初始化 (**与冷启动完全同一路径**, 包括地址分配、SPAD 管理、tuning settings、TimingBudget=20ms、读回验证、参考校准)。
- **退避**: 首次故障**立即**尝试; 失败后 200ms × 2^n (200/400/800/…/10000ms 封顶), 无限重试。
- **防抖**: 恢复成功后 5s 内再次故障视为反复故障, 退避随 fail_count 增长 —— 防止"激光坏了但 I2C 活着"的传感器陷入无限快速重启。
- **隔离**: 恢复中的传感器 `sensor_ready=false`, 轮询循环直接跳过 → **零 I2C 流量**, 不影响其他 4 颗。
- **数据诚实**: 进入恢复的瞬间快照 `valid=false`, 恢复成功后仍保持 invalid, 直到第一笔真实测量写入。全程不伪造数据。

**恢复期间的实时性 — 让出 (yield) 架构**: 初始化/恢复的长等待 (XSHUT 复位、SPAD 查询、参考校准, 最坏合计 ~470ms) 通过 `vl53l0x_set_yield_callback()` 注册的回调**持续让出**, 回调以共享的 `poll_ready_sensors()` 继续轮询健康传感器。健康传感器数据年龄在整个恢复过程中保持在 **~5ms 以内**, 远小于 50ms 过期阈值 → **单传感器恢复期间其他 4 通道 AIR 输出完全不中断**。

## 6. 总线恢复架构

触发条件 (连续 2 轮确认, 防毛刺):
- 单轮中 ≥3 颗传感器通信失败 (5 颗中 3 颗同时失败几乎不可能是个体问题); 或
- 就绪传感器 ≥2 颗且本轮全部 I2C 事务失败 (覆盖部分传感器已离线后总线才死亡的场景)。

恢复序列:

```
1. 全部传感器下线, 快照全部失效
2. i2c_deinit(I2C1) + 引脚切 GPIO (SIO, 输入 + 上拉)
3. 检测 SDA: 若被拉低 → 开漏方式发送最多 9 个 SCL 时钟脉冲
   (每个脉冲后检查 SDA, 释放即提前结束)
   —— 全程开漏: 拉低=输出0+方向OUT, 释放=方向IN(上拉拉高)
   —— 绝不推挽输出高 (VL53L0X 支持时钟拉伸, 推挽高会与从机输出级短路)
4. STOP 条件: SCL 高电平期间 SDA 低→高
5. 验证 SDA/SCL 均为高; 重新 i2c_init(400kHz) + 引脚切回 I2C 功能
6. vl53l0x_reinit_all(): 所有 XSHUT 拉低 10ms → 逐颗释放并完整初始化
   (同一时刻总线上只有一颗处于默认地址 0x29, 杜绝地址冲突)
7. 初始化失败的传感器进入常规单传感器退避恢复路径
```

注: 总线恢复 + 全量重初始化期间 (~600ms) 所有数据已失效 (总线本来就死了), 属于预期行为; 单传感器路径不会走到这里。

## 7. 初始化变更

旧初始化 (约 10 个寄存器写入, 无检查) → 新初始化 (完整 Pololu 序列, 每步检查返回值):

1. 存在性检查: `0xC0 == 0xEE` (MODEL_ID)
2. 地址分配: 写 `0x8A` → 读回验证 (避免 5 颗同址冲突)
3. 2V8 IO 模式: `0x89 |= 0x01`
4. Set I2C standard mode: `0x88 = 0x00`
5. Stop variable 读取 (`0x91`, bank 切换序列) — back-to-back 启动必需
6. MSRC 控制: `0x60 |= 0x12`; 信号率下限: `0x44 = 0x0020` (0.25 MCPS, Q9.7)
7. SPAD 管理: `getSpadInfo` (50ms 超时) → 读取 6 字节 SPAD map → 按 Pololu 算法重写使能表
8. **Tuning settings: 80 项寄存器写入**, 逐项对照 Pololu `init()` 第 138-233 行 (覆盖旧驱动污染的 0x27/0x28/0x2A 区域)
9. 中断配置: `0x0A=0x04` (样本就绪), `0x84 &~ 0x10` (低有效), `0x0B=0x01` (清除)
10. 序列配置 `0x01 = 0xE8`; **TimingBudget = 20000µs 写入 + 读回验证 (±200µs)**
11. 参考校准: VHV (`0x01=0x01`, cal 0x40) + phase (`0x01=0x02`, cal 0x00), 各带 200ms 超时; 恢复序列 `0xE8`
12. 启动连续测距: stop-variable 前导码 + `SYSRANGE_START = 0x02`
13. 最终读 `0x14` 确认传感器进入工作状态

每颗 ~100ms, 5 颗共 ~500ms, 全部在 Core1 执行。

## 8. 运行时轮询变更

保持不变的部分:
- **~100µs 轮询节拍** (`MIN_POLL_SLEEP_US = 100`), 无任何新增的稳定性延时
- 不使用 GPIO1/DataReady 中断 (硬件未连接), 纯寄存器轮询
- `MAX_DATA_AGE_MS = 50` 语义不变; 8190 = 无目标/超量程标记语义不变

变更的部分:
- 读取结果四态: `NEW_DATA` / `NOT_READY` (通信正常, 测量中) / `COMM_ERROR` / `NOT_INITIALIZED`
- **中断清除失败 = COMM_ERROR** → 该次数据不上报, 彻底堵死 stale-as-fresh
- 每笔成功测量: `sequence` 递增 (seqlock 括号), `timestamp = now`, 真实距离; I2C 错误**绝不**沿用旧距离
- 快照改为 **seqlock 协议**: 写者 `seq++ (奇) → payload → seq++ (偶)`, 读者要求偶数且前后一致 —— Core0 无锁读取, 任何撕裂都可检测 (重试 10 次)
- 健康状态与统计: `OK / DEGRADED / RECOVERING / OFFLINE`, 累计错误数、连续错误数、恢复次数均可查询

## 9. 修改文件清单

| 文件 | 变更 | 规模 |
|------|------|------|
| `src/vl53l0x.cpp` | **全量重写**: 完整 Pololu 移植 + TimingBudget 验证 + 恢复/总线恢复 + yield 架构 | +1074/-行级重写 |
| `src/vl53l0x.h` | 新 API: health/read_result 枚举, recover/bus_recover/reinit_all/force_offline/yield_callback, 健康查询 | 重写 |
| `src/tof_reader.cpp` | **全量重写**: Core1 恢复状态机 + seqlock 快照 + 总线检测 + 让出回调 | +437/-行级重写 |
| `src/main.cpp` | 移除 Core0 上的 `vl53l0x_init()` 调用 (I2C1 归 Core1) | 3 行 |
| `src/air.cpp` / `src/air.h` | **未修改** (git diff 与 HEAD 逐字节一致) | 0 |
| MPR121 / Slider / USB HID | **未修改** | 0 |

## 10. 重要代码变更

**统一 TimingBudget 常量与验证门限** ([src/vl53l0x.cpp](src/vl53l0x.cpp)):
```c
constexpr uint32_t VL53L0X_TIMING_BUDGET_US = 20000u;
constexpr int32_t  TIMING_BUDGET_VERIFY_TOLERANCE_US = 200;
```

**读回验证 (init 失败即下线)**:
```c
uint32_t actual_budget = 0;
if (!get_measurement_timing_budget(new_addr, &actual_budget)) return false;
int32_t diff = (int32_t)actual_budget - (int32_t)VL53L0X_TIMING_BUDGET_US;
if (diff < -TIMING_BUDGET_VERIFY_TOLERANCE_US || diff > TIMING_BUDGET_VERIFY_TOLERANCE_US) {
    printf("[VL53L0X] TOF%d budget verify FAILED: %luus (target %luus)\n", ...);
    return false;
}
```

**中断清除失败 → 通信错误 (堵死 stale-as-fresh)**:
```c
if (!write_reg(addr, REG_SYSTEM_INTERRUPT_CLEAR, 0x01)) {
    record_comm_error(index);
    return VL53L0X_READ_COMM_ERROR;   // 绝不上报这次读到的数据
}
```

**恢复退避调度** ([src/tof_reader.cpp](src/tof_reader.cpp)):
```c
// fail_count=0 → 立即尝试 (0ms); ≥1 → 200ms × 2^(n-1), 上限 10s
static uint32_t next_backoff(uint32_t fail_count) {
    if (fail_count == 0) return 0;
    uint32_t backoff = RECOVERY_BACKOFF_BASE_MS;
    for (uint32_t i = 1; i < fail_count && backoff < RECOVERY_BACKOFF_MAX_MS; i++) backoff *= 2;
    return backoff > RECOVERY_BACKOFF_MAX_MS ? RECOVERY_BACKOFF_MAX_MS : backoff;
}
```

**让出架构 (恢复期间健康传感器不停摆)**:
```c
// tof_reader.cpp: 注册 + 防重入
vl53l0x_set_yield_callback(yield_poll_healthy);
static void yield_poll_healthy(void) {
    if (in_yield_poll) return;
    in_yield_poll = true;
    poll_ready_sensors(to_ms_since_boot(get_absolute_time()));
    in_yield_poll = false;
}
// vl53l0x.cpp: 长等待期间让出 (校准轮询 / SPAD 轮询 / XSHUT 等待)
do {
    sleep_us(200); yield_poll();
    if (!read_reg(addr, REG_RESULT_INTERRUPT_STATUS, &status)) return false;
} while ((status & 0x07) == 0);
```

**总线恢复开漏时钟脉冲**:
```c
// SCL 低
gpio_put(I2C1_SCL, 0); gpio_set_dir(I2C1_SCL, GPIO_OUT); busy_wait_us(5);
// SCL 释放 (上拉拉高; 从机时钟拉伸则保持低, 下一脉冲重试)
gpio_set_dir(I2C1_SCL, GPIO_IN); busy_wait_us(5);
```

**Core0/Core1 分工**: `main.cpp` 只调用 `air_init() → tof_reader_init() → Core1 启动`; I2C1 外设与全部 5 颗传感器的初始化、轮询、恢复都在 Core1; Core0 只读取内存中的快照/计数器。

## 11. TimingBudget 寄存器级验证 (问答式)

**Q: 如何证明最终运行的是 20ms 而不是一个没人检查过的参数?**

验证链条共 6 环, 缺一不可:

1. **单一常量**: `VL53L0X_TIMING_BUDGET_US = 20000` 是全工程唯一定义, `vl53l0x_get_timing_budget_us()` 可在运行时查询。
2. **配置生效**: `set_measurement_timing_budget()` 是 Pololu 逐行移植, 经两轮独立代码审查 (共 15 个审查/验证代理) 确认与 `.ref/VL53L0X.cpp` 426-506 行一致 —— 包括"剩余预算直接转 MCLK 后**加上** pre_range MCLK"这一容易写反的关键步骤。
3. **寄存器落点**: 写入 `0x71/0x72 = 0x00D5` (encode(214)), `0x51/0x52 = 0x0096` 保持。
4. **读回重算**: `get_sequence_step_timeouts()` 从寄存器解出 timeout (对 `0x71` **减去** pre_range MCLKs, 与参考 951-957 行一致), 重算 budget。
5. **硬门限**: `|重算值 − 20000| ≤ 200µs`, 否则 `init_sensor()` 返回 false, 传感器不上线 (恢复路径同样受此约束, 三条路径不可能出现不同 budget)。
6. **启动日志**: 每颗传感器打印 `TB=xxxxxus pre_reg=0x0096 fin_reg=0x00D5`, 用户可现场核对。

预期读回值 19981µs 与目标差 19µs, 来自 final-range timeout 的 MCLK 量化 (1 MCLK @ 10 PCLKs ≈ 38µs), 属固有舍入而非配置错误。

## 12. 前后行为对比

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| 实际 TimingBudget | ~33ms boot 默认 (从未配置成 20ms) | 19981µs, 寄存器读回验证强制 |
| pre-range SNR 阈值 | 被写坏 (0x27=0x08) → 漏触发 | tuning settings 正确值, 漏触发消除 |
| 距离偏移 | 0x28 被写 0x0080 → 偏移 | 出厂校准恢复, 距离可信 |
| 中断清除失败 | 静默忽略 → 同一旧数据反复标新 | 判定 COMM_ERROR, 数据不上报 |
| 单颗传感器死亡 | 永久失联, 每轮拖慢轮询 | ~60ms 检出 → XSHUT 恢复 (~100ms), 其他 4 颗无感 (数据年龄 <5ms) |
| 总线锁死 | 全部失联, 只能断电重启 | 9-clock + STOP 总线恢复 + 全量 XSHUT 重初始化, 自动恢复 |
| 恢复失败 | — | 退避 200ms×2^n 至 10s 无限重试, 不产生总线压力 |
| 初始化 I2C 错误 | 全部无视, 半配置上线 | 任何一步失败即该颗下线, 进恢复队列 |
| 双核 I2C 竞态 | Core0 init 与 Core1 轮询存在窗口 | I2C1 完全归 Core1, 竞态消除 |
| 数据诚实性 | 旧数据可被标新 | seqlock + 时间戳自然过期 (50ms), 无伪造 |
| AIR 算法 | — | **零修改**, 检测层/迟滞/OR 合并/HID 映射全部不变 |

## 13. 剩余限制

1. **单传感器恢复仍占用 Core1 数十毫秒** (典型 ~30-80ms): 让出机制保证了其他传感器**数据**不超龄, 但恢复期间 Core1 的轮询节奏会有轻微抖动 —— 这是单核架构下的固有代价, 换取的是与冷启动完全一致的可靠初始化。
2. **总线恢复/全量重初始化 ~600ms 全通道无数据**: 总线已死时数据本就全部失效, 这是必要代价, 不是回归。
3. **冷启动 TOF 就绪延迟 ~500ms**: 5 颗 × ~100ms 完整初始化 (旧代码初始化是残缺的所以更快)。AIR 数据在上电后约半秒出现, 属预期。
4. **"I2C 活着但激光死亡"的传感器**: 每次恢复都成功但 500ms 后再次卡死, 退避使其最终稳定在最长 10s 一次的恢复周期, 永远无法"治愈"但也不再干扰其他通道。
5. **`ms` 时间戳 49.7 天回绕**: 所有间隔比较使用无符号减法 (`now - last`), 回绕安全。
6. **GPIO1 未连接**: 维持寄存器轮询, 无法使用 DataReady 中断降低 I2C 流量 (本次要求: 不引入 GPIO1 中断)。

## 14. 最终配置

```c
// ===== 统一配置 (src/vl53l0x.cpp) =====
VL53L0X_TIMING_BUDGET_US          = 20000    // TimingBudget, 唯一配置源
TIMING_BUDGET_VERIFY_TOLERANCE_US = 200      // 读回验证容差
VL53L0X_SIGNAL_RATE_LIMIT_Q97     = 32       // 0.25 MCPS @ Q9.7 → 0x44
I2C1_FREQ_HZ                      = 400000   // I2C1 速率
I2C_TRANSACTION_TIMEOUT_US        = 3000     // 单事务超时
XSHUT_RESET_DELAY_MS              = 10       // XSHUT 复位脉冲宽度
SENSOR_BOOT_DELAY_MS              = 10       // 释放 XSHUT 后启动等待

// ===== 恢复参数 (src/tof_reader.cpp) =====
RECOVERY_ERROR_THRESHOLD   = 10      // 连续 I2C 错误 → 恢复 (~60ms 检出)
RECOVERY_BACKOFF_BASE_MS   = 200     // 失败后退避基准
RECOVERY_BACKOFF_MAX_MS    = 10000   // 退避上限
NO_DATA_RECOVERY_MS        = 500     // 无新数据超时 (测距卡死检出)
RECOVERY_REPEAT_WINDOW_MS  = 5000    // 反复故障判定窗口
BUS_FAULT_MIN_SENSORS      = 3       // 单轮 ≥3 颗失败 → 总线故障
BUS_FAULT_ROUNDS_TRIGGER   = 2       // 连续 2 轮确认

// ===== 不变的既有配置 =====
MIN_POLL_SLEEP_US = 100               // 轮询节拍 ~100µs
MAX_DATA_AGE_MS   = 50                // (air.cpp) 数据过期阈值
MIN_VALID_MM = 50, MAX_VALID_MM = 800 // (air.cpp) 有效距离范围
序列配置 0xE8 (pre_range + final_range + dss), back-to-back 连续测距
```

**预期启动日志**:
```
[VL53L0X] TOF1 ready: addr=0x30 TB=19981us spad=... pre_reg=0x0096 fin_reg=0x00D5 status=0x....
[VL53L0X] TOF2 ready: addr=0x31 TB=19981us ...
...(5 颗)...
[TOF_READER] Core1 polling started
```

若某颗显示 `budget verify FAILED` 或 `init FAILED`, 说明该颗传感器硬件/接线异常, 将自动进入退避恢复; 若反复失败请检查该路 XSHUT 接线与传感器供电。

---

## 附: 验证方法

- **编译**: arm-none-eabi-g++ 15.2 (Pico SDK 2.3.0), Release `-O3`, **0 error / 0 warning**。
- **对抗性代码审查**: 两个验证工作流, 共 22 个独立审查/验证代理, 覆盖 4 个维度 (寄存器保真度 / TimingBudget 算术 / 恢复状态机与并发 / 集成回归), 全部发现经独立对抗验证后修复。已修复的代表性问题:
  - **读回方向错误 (critical)**: 读回 `0x71` 时对 final_range MCLK 必须减去 pre_range MCLK 而非加上, 否则 ±200us 验证门对每颗传感器都失败, 全部 TOF 黑屏。
  - **恢复阻塞轮询 (major)**: 单传感器恢复的长等待 (XSHUT/SPAD/校准) 会阻塞 Core1 ~470ms, 导致其他传感器数据超龄 —— 以让出回调 (yield callback) 架构解决。
  - **无符号下溢误判 (critical)**: `last_new_data_ms` 由轮询 (含让出轮询) 用采集时刻时钟打点, 可能比本轮状态机取样的 `now_ms` 更新, `now_ms - last` 下溢成 ~4.29e9 会把刚返回数据的健康传感器误判为 "无数据故障" 并级联触发 XSHUT 复位 —— 无符号减法统一钳位修复。
- **AIR 隔离确认**: `git diff HEAD -- src/air.cpp` 为空, 与 HEAD 逐字节一致。
