# ToF I2C Isolation & Recovery Report

> 审查日期: 2026-09-09
> 审查范围: 全部 9 个翻译单元 + Pico SDK 2.3.0 `hardware_i2c` 底层实现
> 结论先行: **当前代码中不存在任何 "VL53L0X Recovery 触碰 I2C0/MPR121" 的软件路径。**
> 本次修改为防御性加固(编译期隔离校验 + 访问边界文档化), 未发现需要拆除的耦合代码。

---

## 1. Problem Description

任务假设: "VL53L0X Runtime Recovery 执行时, 不仅恢复 I2C1, 还会导致 I2C0 (MPR121) 被 deinit/reset/reinit, 中断 Touch/Slider 系统。"

要求: 从调用链追到 RP2040 I2C/reset/GPIO 最底层, 找到并消除 I2C0 与 I2C1 的 Recovery 耦合。

**审查结果: 该耦合在当前源码中不存在。** 逐层证据见第 3/4/5 节。全工程范围内:

- I2C0 外设、GPIO16/17 只被 `mpr121.cpp` 一个翻译单元接触
- I2C1 外设、GPIO6/7、XSHUT (GP1~GP5) 只被 `vl53l0x.cpp` 接触
- 全工程 **零处** 直接调用 `reset_block` / `unreset_block` / `RESETS_RESET_I2C0_BITS`
- 无共享 I2C 驱动状态、无统一 I2C 恢复函数、无 system/board/peripheral reinit
- 引脚映射零重叠 (XSHUT=1..5, I2C1=6/7, I2C0=16/17, 按键/LED=18/19/25/27/28)

若真机上观察到 MPR121 受扰, 软件层已被本报告排除 (第 4/5/15 节给出完整证明链),
应转向电气层排查 (共电源轨跌落、上拉强度、走线串扰、EMI)。

## 2. Existing I2C0 / I2C1 Architecture

```
                 ┌── VL53L0X #0 (addr 0x2A..0x2E)
                 ├── VL53L0X #1
Core1 ── I2C1 ──├── VL53L0X #2        GPIO6=SDA  GPIO7=SCL  XSHUT=GP1~GP5
                 ├── VL53L0X #3
                 └── VL53L0X #4
                      │
                      ├── vl53l0x_recover_sensor(i)   ← Level 1: 设备级
                      ├── vl53l0x_bus_recover()       ← Level 2: I2C1 总线级
                      └── vl53l0x_reinit_all()        ← Level 3: 最后手段

Core0 ── I2C0 ── MPR121×3 (0x5A/0x5B/0x5C) ── Slider   GPIO16=SDA  GPIO17=SCL
```

两条总线域的实现层面隔离 (审计实证):

| 资源 | 唯一接触者 | 初始化时机 |
|---|---|---|
| `i2c0` 外设, GPIO16/17, pull-up | `mpr121.cpp` (write/read 全部走 `I2C0_PORT`) | `mpr121_init()` ← `slider_init()` ← Core0 冷启动 (main.cpp:653) |
| `i2c1` 外设, GPIO6/7, pull-up | `vl53l0x.cpp` (全部走 `I2C1_PORT`) | `vl53l0x_init()` ← Core1 冷启动 (tof_reader_task) |
| XSHUT GP1~GP5 | `vl53l0x.cpp` (`xshut_pins[]`) | `vl53l0x_init()` + 各级恢复 |
| MPR121/Slider 符号 | 恢复路径中 **零引用** | — |

## 3. Root Cause of I2C0 Reset

**不存在。** 按 spec 要求逐项排查了所有可能的耦合通道, 结果:

| 排查项 (spec §30) | 搜索结果 | 判定 |
|---|---|---|
| 直接 `i2c_deinit(i2c0)` / `i2c_init(i2c0)` | 仅 `mpr121.cpp:237` 一处 init (冷启动) | 无恢复路径调用 |
| `reset_block` / `unreset_block` / `RESETS_RESET_*` | **全工程 0 处** (grep src/) | 无 |
| GPIO mux 批量重配置 | 恢复路径只操作 `I2C1_SDA/I2C1_SCL`/XSHUT 具名常量, 无循环遍历"所有 I2C GPIO" | 无 |
| GPIO pull-up 误配置 | 恢复路径 pull-up 仅作用于 GP6/GP7 | 无 |
| 共享驱动状态 (`i2c_inst*` 全局指针/current_i2c/bus_state) | **0 处** | 无 |
| 共享初始化函数 / `system_init` / `board_init` / `peripheral_init` | **0 处** | 无 |
| SDK 层 `i2c_init/i2c_deinit` 内部复位掩码 | `reset_block_num(I2C_RESET_NUM(i2c))` —— 按实例精确 (见下) | 硬件层亦隔离 |
| 引脚物理重叠 | XSHUT(1-5)/I2C1(6,7)/I2C0(16,17) 两两不同, 且已由 static_assert 编译期强制 | 无 |

SDK 2.3.0 底层证据 (`~/.pico-sdk/sdk/2.3.0/src/rp2_common/hardware_i2c/i2c.c:18-60`):

```c
static inline void i2c_reset(i2c_inst_t *i2c) {
    reset_block_num(I2C_RESET_NUM(i2c));      // ← 仅该实例的复位位
}
uint i2c_init(i2c_inst_t *i2c, uint baudrate) {
    i2c_reset(i2c); i2c_unreset(i2c); ...     // 实例精确, 无批量掩码
}
void i2c_deinit(i2c_inst_t *i2c) { i2c_reset(i2c); }
```

即: 总线恢复中的 `i2c_deinit(I2C1_PORT)` + `i2c_init(I2C1_PORT)` 在 RP2040 reset
controller 层面也只脉冲 `RESETS_RESET_I2C1` 一个位, `I2C0` 的复位位不被触碰。

## 4. Recovery Call Chain Before Modification

(本次审查时的实际代码, 逐函数核实)

```
Core1 tof_reader 状态机
├─ attempt_recovery(i)                          [tof_reader.cpp]
│   └─ vl53l0x_recover_sensor(i)                [vl53l0x.cpp]
│       ├─ gpio_put(xshut_pins[i], 0)           ← 仅 XSHUT[i]
│       ├─ yield_sleep_us() → 让出回调 → poll_ready_sensors()
│       │     └─ vl53l0x_read_distance_ex(i)    ← 仅 I2C1, 仅健康传感器
│       └─ init_sensor(i)                       ← 仅 I2C1 寄存器读写
│           (XSHUT→0x29 地址分配→完整配置→TB=20000us→读回验证→连续测距)
│
├─ run_bus_recovery()                           [tof_reader.cpp]
│   ├─ vl53l0x_bus_recover()                    [vl53l0x.cpp]
│   │   ├─ i2c_deinit(I2C1_PORT)                ← SDK 内部: reset_block_num(i2c1) 实例精确
│   │   ├─ gpio_set_function/init/dir/pull_up   ← 仅 I2C1_SDA(6) / I2C1_SCL(7)
│   │   ├─ 9-clock + STOP (开漏位拆)             ← 仅 GP6/GP7
│   │   └─ i2c_init(I2C1_PORT)                  ← 同上, 实例精确
│   ├─ 存活甄别: vl53l0x_probe_sensor(i)         ← 仅 I2C1, 读 MODEL_ID 不改配置
│   └─ 失败 → vl53l0x_reinit_all()              ← 仅 XSHUT[0..4] + I2C1
```

**链路终点全部落在 I2C1 / GPIO6/7 / XSHUT[0..4] 上, 没有分支通向 I2C0。**

## 5. Recovery Call Chain After Modification

与第 4 节完全一致 —— 本次没有改动任何恢复逻辑 (这是审查结论的自然结果:
不存在需要拆除的耦合)。新增的是 **防护层**, 不是行为变更:

```
VL53L0X Recovery API
        ↓ 只允许访问 (现在由编译器 + 模块边界双重保证):
    i2c1 外设 / GP6 GP7 / XSHUT[0..4] / VL53L0X 寄存器与状态
        ↓ 明确禁止 (现在有文档声明 + 编译期引脚校验):
    i2c0 / GPIO16 17 / MPR121 / Touch / Slider / USB / 板级初始化
```

新增防护:
1. `board_defs.h` —— `static_assert(chuni_board_pins_distinct())`: I2C0/I2C1/XSHUT/
   按键/LED 全部引脚两两不同的不变量交由编译器强制。未来任何人改引脚分配,
   若引入重叠, **编译立即失败**, 而不是上线后 MPR121 莫名受扰。
2. `vl53l0x.h` / `mpr121.cpp` —— Recovery Access Boundary 与 I2C0 独占声明的文档化。

## 6. I2C0 Protection Mechanism

四层防线 (本次修改后):

1. **翻译单元边界**: I2C0 只存在于 `mpr121.cpp`; 恢复代码 (tof_reader.cpp/vl53l0x.cpp)
   不 include mpr121.h, 无任何 MPR121/Slider 符号引用 (grep 实证)。
2. **内核边界**: I2C0 只在 Core0 访问 (mpr121_update 位于 Core0 主循环);
   I2C1 只在 Core1 访问。无跨核调用。
3. **SDK 实例精确复位**: `i2c_init/deinit` → `reset_block_num(I2C_RESET_NUM(i2c))`,
   复位掩码精确到单个外设实例。
4. **编译期引脚隔离不变量** (本次新增): `board_defs.h` static_assert。

## 7. I2C1 Sensor Recovery

- 入口: `vl53l0x_recover_sensor(index)`, 仅操作 `xshut_pins[index]` + `init_sensor(index)`
- 与冷启动共享同一条 `init_sensor()` 路径 (无第二套初始化实现)
- 长等待通过让出回调继续轮询其他健康传感器 (I2C1), 恢复不阻塞健康传感器数据
- **I2C1 外设本身不被 reset** (符合 spec §8: 设备级恢复优先, 不动外设)
- **I2C0: 全程零接触**

## 8. I2C1 Bus Recovery

- 入口: `vl53l0x_bus_recover()`, 触发条件为总线级证据 (全部就绪传感器同时静默≥60ms,
  或反复设备级恢复失败且无人应答), 不是单传感器错误
- 步骤: `i2c_deinit(i2c1)` → GP6/GP7 转 SIO 开漏 → SDA 卡低检测 → 9 时钟 + STOP
  (全程开漏, 严禁推挽高) → 验证释放 → `i2c_init(i2c1)` → 恢复 GPIO 功能
- GPIO/pull-up 操作严格限定 `I2C1_SDA`/`I2C1_SCL` 两个宏, 无批量循环
- **I2C0: 全程零接触** (包括 GPIO16/17 的功能、方向、pull-up 均不被读写)

## 9. Full VL53L0X Recovery

- 入口: `vl53l0x_reinit_all()`, 仅在总线恢复失败 (总线仍卡死) 时作为最后手段
- 范围: XSHUT[0..4] 拉低 → 逐颗释放 + `init_sensor(i)` (共用同一初始化路径)
- 不重新初始化 I2C1 外设 (总线恢复已做过, 或已判定外设可用)
- **I2C0 / MPR121: 零接触; USB/PIO/PWM/UART: 零接触; 无任何板级 reinit**

## 10. TimingBudget Verification

- 唯一配置源: `VL53L0X_TIMING_BUDGET_US = 20000us` (vl53l0x.cpp)
- 唯一配置点: `init_sensor()` 第 11 步 `set_measurement_timing_budget(…, 20000)`
- 冷启动、单传感器恢复、全量恢复全部经由同一 `init_sensor()` → **同一 TB 配置**
- 每次初始化都从寄存器读回实际 budget 并校验 (容差内), 失败则该传感器判恢复失败
- 串口验证: 上电每颗打印 `TOFx ready: TB=19981us` (20000us 配置经芯片重算的
  实际值, 属预期读数); 恢复成功后同样输出该行
- 本次修改未触碰任何 TB 相关代码

## 11. Files Modified

| 文件 | 修改内容 | 性质 |
|---|---|---|
| `src/board_defs.h` | 新增编译期引脚隔离 static_assert (~30 行) | 防护性新增 |
| `src/vl53l0x.h` | 新增 Recovery Access Boundary 文档块 (~10 行) | 文档 |
| `src/mpr121.cpp` | 新增 I2C0 独占声明 (~8 行) | 文档 |
| `TOF_I2C_ISOLATION_RECOVERY_REPORT.md` | 本报告 | 新增 |

**恢复逻辑零改动**: `vl53l0x.cpp` / `tof_reader.cpp` / `main.cpp` / `air.cpp` /
`tof_reader.h` 的可执行代码本轮无任何修改 (上一轮 Recovery 粒度重构保持不变)。

## 12. Functions Modified

无函数签名/逻辑修改。`board_defs.h` 新增两个 `constexpr` 函数
(`chuni_board_pins_distinct()` 等, 仅编译期求值, 零运行时开销)。

## 13. Functions Removed / Refactored

无。审查确认不存在 spec §4/§27 描述的 "统一 I2C 恢复公共函数" (如
`recover_i2c_bus()` 内部 reset 双总线) 或 `system_i2c_reinit()` —— 无可拆除对象。

## 14. Before / After Architecture

| 情况 | 修改前 | 修改后 |
|---|---|---|
| A: 单颗偶发错误 | 计数容忍, 不触发恢复 | 不变 |
| B: VL53L0X #2 真正掉线 | 仅 XSHUT[2] + init_sensor(2); I2C0 不动, MPR121 不动, 其余 4 颗不动 | 不变 |
| C: I2C1 卡死 | 仅 i2c1 deinit/GP6-7 位拆/reinit; I2C0 不动, MPR121 不动 | 不变 + 引脚隔离由 static_assert 强制 |
| D: I2C1 恢复失败 | 仅 XSHUT[0..4] + 逐颗 init; I2C0 不动, MPR121 不动 | 不变 |
| 引脚分配引入重叠 (人为失误) | 静默接受, 上线后表现不可预测 | **编译失败**, 隔离破坏在构建期暴露 |

## 15. Remaining Limitations

1. **软件层已证明无耦合, 但不能证明真机无电气耦合。** 若现场仍观察到 ToF 恢复时
   MPR121 受扰, 排查方向 (按可能性):
   - 共用 3V3 轨: 5 颗 VL53L0X 同时上电/恢复的浪涌导致电压跌落, MPR121 需要重新
     上电稳定时间 —— 示波器看 3V3 轨
   - I2C0 上拉强度不足或走线过长, 对 GP1-7 的 XSHUT 边沿串扰敏感
   - MPR121 电极/飞线拾取 EMI (与固件恢复行为时间上巧合)
2. static_assert 只覆盖 `board_defs.h` 内的引脚; `main.cpp` 中局部定义的
   BUTTON_ENTER_PIN(GP18)/BUTTON_2_PIN(GP19) 未纳入 (与总线引脚无重叠, 风险为零,
   如需可后续收编进 board_defs.h)。
3. C 翻译单元 (若未来引入 .c 文件) 不受 static_assert 保护 —— 当前工程全部为 C++,
   全部翻译单元都参与校验。

---

## 附录: Spec 三十六节 12 问逐答

1. **为什么 VL53L0X Recovery 会导致 I2C0 被 reset?** —— 不会。经全调用链审计 +
   SDK 底层核实, 不存在这样的路径 (第 3 节)。
2. **哪个函数/调用链导致了这个问题?** —— 无。最近的 "嫌疑" 是总线恢复中的
   `i2c_deinit(I2C1_PORT)`, 但其 SDK 实现 (`i2c.c:58-60`) 是实例精确复位。
3. **是否存在统一 I2C reset / reinitialization?** —— 不存在。无 `reset_block`
   调用, 无公共恢复函数, I2C0/I2C1 初始化分别封闭在 mpr121.cpp / vl53l0x.cpp。
4. **修改后 Sensor Recovery 是否会触碰 I2C0?** —— 否。仅 XSHUT[i] + I2C1 寄存器。
5. **修改后 I2C1 Bus Recovery 是否会触碰 I2C0?** —— 否。GPIO/pull-up/deinit/init
   全部限定 GP6/GP7 + i2c1 实例。
6. **修改后 Full VL53L0X Recovery 是否会触碰 I2C0?** —— 否。仅 XSHUT[0..4] + I2C1。
7. **MPR121 是否还会被重新初始化?** —— 否。`mpr121_init` 唯一调用点是 Core0 冷启动
   的 `slider_init()`; 恢复代码不含任何 MPR121 符号。MPR121 从头到尾连续运行。
8. **I2C0 SDA/SCL GPIO 是否可能被 ToF Recovery 修改?** —— 不可能。GP16/17 只被
   `mpr121.cpp:238-241` 配置一次; 且引脚两两不同已由 static_assert 编译期强制。
9. **RP2040 reset controller 是否只 reset I2C1?** —— 是。SDK 2.3.0 用
   `reset_block_num(I2C_RESET_NUM(i2c))` 逐实例复位, 无批量掩码 (i2c.c:18-26)。
10. **I2C0 是否能够在整个 ToF Recovery 过程中保持连续运行?** —— 能。Core0 主循环
    从不等待 Core1 恢复 (快照异步消费), MPR121 事务使用 500us `_until` 超时独立
    完成于 I2C0 上。
11. **Cold Boot 是否仍然能够正常初始化 I2C0 + I2C1?** —— 是。Core0: `slider_init →
    mpr121_init → i2c_init(i2c0)`; Core1: `vl53l0x_init → i2c_init(i2c1)`。本次
    零逻辑改动。
12. **TimingBudget 是否仍严格为 20ms / 20000us?** —— 是。单一配置点未动, 每次初始化
    (冷启动/恢复) 均写 20000us 并读回校验。
