# TOF Recovery Refactor Report

> 日期: 2026-09-07 ｜ 主题: VL53L0X Recovery 从 "全局恢复" 重构为 "故障设备独立恢复"
> 关联文档: `TOF_AIR_FIX_REPORT.md`（上一轮 TimingBudget 全链路修复）

## 1. Problem Description

上一轮修复引入的 Recovery 机制中, 单传感器级恢复本身已经是设备级的, 但 **总线级故障路径的触发条件过宽、处理手段过重**, 造成 "过度恢复":

1. 触发条件 `failed_cnt >= 3`（单轮中任意 3 颗通信失败）意味着 **3 颗传感器各自出现偶发错误**（如 EMI 造成的一轮毛刺）就可能计入总线故障;
2. 触发条件 `polled_cnt >= 2 && failed_cnt == polled_cnt` 意味着 **2 颗传感器同时个体死亡**（与总线无关）在 ~2 轮（<1ms）内就会被误判为总线故障;
3. 一旦触发: **全部 5 颗传感器被强制下线、全部快照被清空、全部传感器 XSHUT 重初始化** —— 本来健康的传感器被无谓重启, 数据/序列号全部断层, 恢复时间从 ~100ms 膨胀到 ~500ms+。

同时, 单传感器掉线判定仅依赖 `连续错误 >= 10` 或 `无新数据 >= 500ms` 两个独立条件, 缺少 "连续错误 + 通信静默时长" 的联合确认。

## 2. Existing Recovery Architecture

修改前的调用链（审查结论, 非猜测）:

```text
Core1 poll (~100us)
  └─ poll_ready_sensors()
       └─ vl53l0x_read_distance_ex(i)
            ├─ NEW_DATA    → 更新快照[i]
            ├─ NOT_READY   → 等待 (连续错误不清零 ← 缺陷之一)
            └─ COMM_ERROR  → consecutive_errors[i]++ (驱动内)

主循环 section 3 (单传感器):
  consec >= 10 || since_data > 500ms
  └─ take_sensor_offline(i)      ← 只触碰传感器 i (XSHUT[i]/快照[i])
       └─ attempt_recovery(i)
            └─ vl53l0x_recover_sensor(i)   ← 只拉低 XSHUT[i]
                 └─ init_sensor(i)          ← 与冷启动同一路径, TB=20ms

主循环 section 2 (总线级, 问题所在):
  (failed_cnt >= 3 || (polled>=2 && failed==polled)) × 2 轮
  └─ 强制 5 颗全部下线 + 全部快照失效   ← 健康传感器被牵连
       └─ vl53l0x_bus_recover()         ← 9-clock + STOP
            └─ vl53l0x_reinit_all()     ← 无条件全量 XSHUT 重初始化
```

即: **Level 2 (单传感器) 路径已经是设备级的; 但 Level 3 的触发太敏感, 且 Level 3 无条件直接跳到 "全量重初始化", 没有分层。**

## 3. Root Cause

1. **触发混淆**: 用 "一轮中几颗传感器通信失败" 作为总线故障依据 —— 这个指标同时混入了 "多颗传感器同时个体故障" 和 "瞬时共模干扰" 两种非总线事件;
2. **处理一刀切**: 总线路径里没有 "先甄别、再最小化恢复" 的步骤, 直接全量重初始化;
3. **健康状态无通信级刻画**: 驱动只有 `last_new_data_ms`（数据产出）没有 `last_comm_ok_ms`（通信层存活）, 无法区分 "传感器失联" 与 "传感器活着但测距卡死", 也无法用 "全员通信静默" 这种真正可靠的的总线级证据。

## 4. Why All Sensors Were Being Reinitialized

- 旧 section 2 触发后第一件事就是 `for i: vl53l0x_force_offline(i); snapshot_invalidate(i);` —— 5 颗全部下线、全部快照清空;
- 随后无条件调用 `vl53l0x_reinit_all()`（全部 XSHUT 拉低 → 逐颗重新完整初始化）;
- 因此即使实际只有 1-2 颗真故障（或甚至只是瞬时毛刺）, 5 颗都会被重置。

本次修改从架构上删除了这条路径: 总线恢复后 **先探测甄别**, 只有探测无响应的传感器才进入单传感器恢复; 全量重初始化降级为 "总线恢复失败后" 的最后手段。

## 5. Offline Detection Changes

判定全部移到 "连续性 + 时间窗口" 联合模型上（无符号减法全部钳位, 防止时间戳超前导致的下溢误判）:

**状态 A — Transient Error (不恢复)**: 单次读失败/超时/清中断失败 → 错误计数 +1, 继续轮询。**任何成功通信（含 NOT_READY 轮询）立即清零连续错误** —— 偶发错误永远无法累积到阈值。

**状态 B — Confirmed Offline (进入该传感器的独立恢复)**, 满足任一:

| 分支 | 条件 | 针对场景 | 实际耗时 |
|---|---|---|---|
| B1 通信失联 | `连续通信错误 ≥ 8` **且** `距上次成功通信 ≥ 40ms` | 传感器死亡/掉线 | ~40-60ms (2-3 个测距周期) |
| B2 测距卡死 | `距上次新数据 ≥ 300ms`（通信正常） | 测距序列卡死/中断未置位 | 300ms (≈14 个测距周期) |

**总线疑似卡死抑制**: 当 ≥2 颗且**全部**就绪传感器都处于持续通信失败（连续错误 ≥2）时, 判定为总线级事件 —— B1 下线被抑制, 传感器保持就绪, 等待 Level 3 总线恢复 + 存活甄别统一处理。这防止了 "总线恢复冷却期内, Level 2 把全部本可幸存的传感器逐个 XSHUT 重初始化" 的次生损伤; 个体故障不受影响（健康传感器连续错误为 0, 永远不满足 "全部失败"）。

**阈值推导依据**（非拍脑袋）:
- 就绪传感器单次事务 ~0.1-0.3ms; 失败事务 3ms 超时; 测距周期 20ms（健康传感器每 ~21ms 必产出数据、每轮必有一次成功通信）;
- 4 健康 + 1 死亡 → 一轮 ~4ms → 8 次连续失败天然跨越 ≥32ms, 与 40ms 静默窗口一致;
- 健康传感器通信间隔上限 ~21ms(测距周期) + 让出轮询保障, 40ms 窗口 = 2 个测距周期, 不会误判;
- 单次瞬时错误 → 下一轮(≤100µs+事务时间)成功通信即清零, **永远不会触发恢复**。

**状态机（per-sensor, 融合于现有 ready/down 状态 + 驱动 health）**:

```text
RUNNING(ready&&!down) ──偶发错误──> 计数, 成功即回 RUNNING
        │ 连续8错+静默40ms, 或 300ms 无数据
        ▼
     OFFLINE(down) ──退避到期──> RECOVERING(XSHUT+重初始化, 期间让出轮询继续)
        ▲                          │ 成功 → RUNNING(快照等真实新数据)
        └──── 失败: 退避 200ms×2ⁿ (上限 10s), fail_count++ ──┘
```

## 6. Per-Sensor Recovery Design

- API 保持 `vl53l0x_recover_sensor(index)` —— **设备是唯一恢复单位**;
- 只操作: `XSHUT[index]`、`sensor_addr[index]`、`stop_variable[index]`、传感器 i 的寄存器;
- XSHUT[index] 拉低 10ms（让出式等待）→ 释放 → 传感器回到默认地址 0x29（此刻总线上唯一处于 0x29 的设备, **与其他传感器的 0x30-0x34 无冲突**）→ 写回独立地址并验证 → `init_sensor(index)` 完整初始化（与冷启动 100% 同一路径）;
- 退避: 首次立即, 失败后 200ms×2ⁿ（上限 10s）; 5s 内反复故障则退避升级（防反复重启风暴）;
- 恢复期间 **让出回调持续轮询其余健康传感器**（yield-callback 架构）, 单颗恢复 ~100ms 不影响其他传感器数据龄期;
- 目标传感器快照在判定掉线时立即失效; 其他 4 颗的快照/序列号/时间戳 **完全不动**。

## 7. I2C Bus Recovery Design

**触发（仅总线级证据, 二选一, 且距上次总线恢复 ≥ 500ms 冷却）**:

| 条件 | 定义 | 覆盖场景 |
|---|---|---|
| a. 全员静默 | 存在就绪传感器, 且 **所有** 就绪传感器通信静默 ≥ 60ms | 从机卡死 SDA / 外设异常 / 供电跌落（只要有一颗在通信就不满足） |
| b. 升级判定 | 存在 `fail_count ≥ 3` 的下线传感器, 且无任何就绪传感器在 200ms 内成功通信 | 总线卡死期间传感器已逐个下线 / 全部个体死亡（就绪数为 0, 条件 a 永远无法覆盖） |

**处理（Level 3 → 存活甄别, 关键改进）**:

```text
run_bus_recovery()
  ├─ vl53l0x_bus_recover()   ← 停外设 → 开漏 9-clock + STOP → 重初始化 I2C1
  │                            (不动任何 XSHUT, 不清任何传感器配置)
  ├─ 成功 → 探测甄别 (vl53l0x_probe_sensor, 只读 MODEL_ID):
  │     ├─ 响应 → 原样保留 (快照/序列号/状态不动, 继续测距)   ← 幸存者零损失
  │     └─ 无响应 → take_sensor_offline(i) → 走 Level 2 单独恢复
  └─ 失败 → Level 4 (见第 8 节)
```

最优场景（总线被卡死后释放）: 5 颗全部探测通过 → **零重初始化、零快照损失**, 总代价 ≈ 60ms 静默确认 + ~1ms 总线脉冲 + 5×探测(~6ms)。

**冷却与防空转**: 两次总线恢复间隔 ≥ 500ms; 若一次总线恢复 "无效果"（成功但无任何就绪传感器可甄别 —— 典型为全部传感器个体死亡而总线正常）, 下一次冷却按 ×2 递增（上限 ×32 = 16s）, 任何有效果的恢复将计数清零。时间差比较全部通过回绕安全的 `ms_since()`（int32 解释差值）, 32 位 ms 计数在 ~49.7 天回绕后依然正确。

## 8. Full Recovery Conditions

`vl53l0x_reinit_all()`（全部 XSHUT → 逐颗完整初始化）**只在一个条件下可达**:

> `run_bus_recovery()` 中 `vl53l0x_bus_recover()` 返回 false（9-clock + STOP 仍无法释放总线, SDA/SCL 仍被拉低）。

此时全部传感器无论如何都已无法通信, 全量重初始化 + 快照失效 + 状态重新播种是正确语义。除此之外的任何场景（单颗故障、多颗个体故障、瞬时共模毛刺、就绪数不足）都不会触碰全量路径。

## 9. VL53L0X Initialization Changes

- 冷启动 `vl53l0x_init()` 与恢复路径 **继续共用唯一的 `init_sensor(index)`**（无任何复制出来的第二套配置）;
- 驱动新增通信级健康刻画: `last_comm_ok_ms[i]` —— 任何成功的寄存器读取（含 NOT_READY 轮询、探测）都会打点; 初始化/恢复成功时同步打点（防止启动瞬间因时间戳为 0 被误判为静默）;
- `read_distance_ex` 的 NOT_READY 路径现在会清零连续错误（成功通信即证明未失联）;
- 新增 `vl53l0x_probe_sensor(index)`: 只读 MODEL_ID 验证存活, 不改任何配置, 成功时刷新通信时间戳并清零连续错误。

## 10. TimingBudget Verification

所有路径（冷启动 / `vl53l0x_recover_sensor` / `vl53l0x_reinit_all`）都经过同一个 `init_sensor()`:

- `set_measurement_timing_budget(addr, 20000)` → 写 `0x71` (FINAL_RANGE timeout, MCLK 域含 pre_range);
- `get_measurement_timing_budget()` 读回重算, **±200µs 验证门**, 预期 ~19981µs;
- 验证失败 → 初始化失败 → 该传感器进入独立恢复队列。总线上其他传感器不受影响。

恢复路径不存在 "default / 200ms TimingBudget" 的可能 —— 代码中只有这一个配置点（`VL53L0X_TIMING_BUDGET_US = 20000u`）。

## 11. Files Modified

| 文件 | 修改 | 原因 |
|---|---|---|
| `src/vl53l0x.cpp` | 新增 `last_comm_ok_ms` 打点; NOT_READY 清零连续错误; 新增 `vl53l0x_probe_sensor()`; init/recover/reinit_all 成功路径打点 | 提供 "通信层存活" 信号供掉线判定与总线甄别使用 |
| `src/vl53l0x.h` | 声明 `vl53l0x_probe_sensor` / `vl53l0x_get_last_comm_ok_ms`; 注释更新 | 新 API |
| `src/tof_reader.cpp` | 掉线判定改为 "连续错误+静默窗口" 联合模型; 总线检测改为 "全员静默/升级判定 + 冷却"; 总线恢复后增加存活甄别; Level 4 降为最后手段; 删除轮级 failed 计数 | 消除单/多传感器个体故障误触发全量恢复的路径 |
| `src/air.cpp`, `src/main.cpp`, `src/tof_reader.h` | **未修改**（AIR 算法与对外 API 零改动） | 需求约束 |

## 12. Function-Level Changes

**tof_reader.cpp**
- `poll_ready_sensors()`: 删除轮级 `round_sensor_failed[]` 标记（总线判定不再依赖轮级计数）; NOT_READY/COMM_ERROR 的处理注释化;
- 新增 `ms_since(stamp, now)`: 回绕安全的时间差计算（int32 解释差值, stamp 更新时返回 0）—— 修复普通钳位在 32 位 ms 计数 ~49.7 天回绕后使全部检测失明的问题, 同时保留 "让出轮询时间戳超前" 防护;
- 新增 `all_ready_silent(now_ms)`: 全部就绪传感器静默 ≥60ms 判定;
- 新增 `recovery_escalation_due(now_ms)`: fail_count≥3 且无人通信的升级判定;
- 新增 `bus_suspected_now()`: ≥2 颗且全部就绪传感器持续通信失败 → 抑制 B1 下线等待 Level 3（对抗验证确认的 major 问题修复: 防止冷却期内 Level 2 把全部传感器逐个 XSHUT 重初始化）;
- 新增 `run_bus_recovery(now_ms)`: Level 3 总线恢复 + 存活甄别; 失败时才进 Level 4 全量重初始化并重新播种状态; "无效果" 恢复计数使冷却 ×2 递增;
- 修复恢复调度门 `now_ms - last_attempt_ms` 的无符号下溢（3 个维度独立确认）: 统一改用 `ms_since()`, Level 4 重播种时 `last_attempt_ms` 打点为当前时刻使 200ms 基准退避真正生效;
- 删除: `bus_fault_rounds` / `BUS_FAULT_MIN_SENSORS` / 旧 section 2 的全量 force_offline + reinit_all 路径;
- section 3: 掉线判定改为 `(comm_dead && !bus_suspected) || range_stuck`, 全部时间差经 `ms_since()`。

**vl53l0x.cpp**
- `read_distance_ex()`: 状态寄存器读取成功即 `stamp_comm_ok()`; NOT_READY 路径清零 `consecutive_errors`;
- 新增 `stamp_comm_ok()` / `vl53l0x_probe_sensor()`;
- `vl53l0x_init()` / `vl53l0x_recover_sensor()` / `vl53l0x_reinit_all()`: 成功路径同步打点 `last_comm_ok_ms`;
- `vl53l0x_bus_recover()` / `vl53l0x_reinit_all()` / `init_sensor()`: 逻辑未变（bus_recover 本就不动 XSHUT; init_sensor 本就是共用路径）。

## 13. Before / After Recovery Flow

```text
修改前:
  any error → (单颗: 10连错) → 单颗恢复 …
  3颗同轮失败 或 2颗就绪全失败 ×2轮 → 全部下线+全部快照清空 → bus_recover → reinit_all(全量)

修改后:
  polling ──┬─ 瞬时错误(成功通信清零) ────────────────→ 继续, 不恢复
            ├─ 单颗确认掉线(8连错+40ms静默 / 300ms无数据) → 只恢复这一颗
            ├─ 全部就绪传感器同时静默 ≥60ms ──────────────→ bus_recover → 探测甄别
            │        ├─ 响应者: 原样保留继续测距
            │        └─ 无响应者: 各自的单传感器恢复
            └─ 反复恢复失败(fail≥3) 且 无人通信(200ms) ───→ bus_recover → 同上
                     └─ bus_recover 失败(总线仍卡死) ─────→ 全量重初始化 (最后手段)
```

**前后行为对照（实测逻辑推演, 非虚构）**:

| 场景 | 修改前 | 修改后 |
|---|---|---|
| 单次 I2C error | 计入连续错误（若连续累积 10 次即恢复; 与 NOT_READY 交错时也会累积） | 成功通信立即清零, 不恢复 |
| 3/5 颗同轮瞬时毛刺 ×2 轮 | **触发总线故障 → 5 颗全部重初始化** | 错误计数被成功通信清零, 什么都不发生 |
| 2 颗同时个体死亡 | **~2 轮内触发 "总线故障" → 健康的 3 颗被迫重置** | 2 颗各自独立恢复, 健康的 3 颗原地运行 |
| 4/5 颗死亡, 1 颗健康 | **触发总线故障（failed≥3）→ 健康的那颗也被重置** | 死亡 4 颗各自独立恢复, 健康颗完全不受影响 |
| 总线真卡死（全员失联） | bus_recover + **无条件全量重初始化**, 全部快照清空 | bus_recover + **探测甄别**: 幸存者零损失, 仅无响应者单独恢复 |
| 总线恢复冷却期内再次卡死 | （旧实现无冷却, 相当于再次全量重初始化） | Level 2 失联下线被抑制, 冷却结束后 Level 3 恢复总线 + 甄别, 幸存者零损失 |
| 总线恢复失败 | （不存在该分层） | 才执行全量重初始化（最后手段） |
| 传感器恢复失败 | 指数退避（已有） | 保持指数退避, 连续 3 次失败且无人通信才升级总线恢复 |
| 健康传感器数据 | 总线触发时全部快照被清空 | 任何情况下都不因别的传感器而清空 |

## 14. Impact on Healthy Sensors

- **零重初始化**: 健康传感器只在 "自己被确认掉线" 时才会被 XSHUT; 其他任何传感器的事件（掉线/恢复/总线恢复）都不会触碰它;
- **快照/序列号连续**: `snapshot_invalidate` 严格 per-index; seqlock 序列号只递增不复位; Core0 不会感知到 "全体重启";
- **数据龄期**: 单传感器恢复期间让出回调持续轮询健康传感器（龄期 << 50ms 上限）; 总线恢复期间（~1ms 脉冲 + 甄别探测）健康幸存者的测距在芯片端自主持续, 总线释放后立即恢复上报;
- **轮询节拍**: 主循环保持 ~100µs 节拍; 恢复动作全部让出式等待或短时（探测 5×≤6ms, 仅在确认总线事件后发生一次）。

## 15. Remaining Limitations

1. **总线恢复后的首帧数据**: 总线卡死期间芯片端仍在自主测距, 总线释放后第一条被读到的结果可能是卡死期间完成的测量（最旧 ≈ 卡死时长, 一帧之后即为全新数据）。50ms 数据龄期门限自然兜底; 按需求 "不得清空健康传感器快照", 未对幸存者快照做主动失效;
2. **全员静默与多颗同时个体死亡在信号上不可区分**（总线级证据的定义即如此）: 此时也会走一次总线恢复 —— 但探测甄别保证幸存者零损失, 误判代价仅为 ~1ms 总线脉冲 + 5 次探测;
3. **全灭系统的周期性总线恢复**: 5 颗全部个体死亡且总线正常时, 升级判定仍会周期性重试总线恢复（无害的 9-clock 脉冲, 不动任何配置）; "无效果" 恢复的冷却按 ×2 递增至上限 16s, 空转频率有界; 这是硬件故障状态下的可接受行为;
4. **从卡死到总线恢复的时延**: 总线真卡死时, 若之前已发生过一次总线恢复且冷却未到, Level 2 失联下线会被抑制、传感器保持就绪等待冷却结束后的 Level 3（最长 ≈ 500ms, 无效果递增时更长）; 期间 TOF 数据按龄期门限自然失效, 属预期;
5. 掉线判定/恢复全部在 Core1, Core0 只读快照 —— 无新增跨核竞争面; 全部时间差比较经回绕安全的 `ms_since()`, 长期运行（>49.7 天, 32 位 ms 回绕）不影响检测。

## 附: 验证方法

- **编译**: arm-none-eabi-g++ 15.2 (Pico SDK 2.3.0), Release `-O3`, 0 error / 0 warning;
- **对抗性代码审查**: 独立验证工作流, 19 个审查/验证代理（故障隔离审计 / 触发逻辑数值推演 / 并发与回归 三个维度, 每个发现由 2 名独立对抗验证者交叉确认, 默认立场为反驳）。确认并修复 3 类问题:
  - **[major] 冷却期次生损伤**: 总线恢复冷却期内若总线再次卡死, Level 2 会把全部传感器逐个 XSHUT 重初始化（~70ms 的事件变成 ~1.4s 中断 + 5 次全量重初始化）→ 新增 `bus_suspected_now()` 抑制规则修复;
  - **[minor] 恢复调度门无符号下溢**: `now_ms - last_attempt_ms` 是全文件唯一未钳位的时间差, Level 3 甄别/Level 4 重播种打点的时刻超前于本轮 `now_ms` 时下溢, 退避被绕过一次 → 统一 `ms_since()` + 重播种改为当前时刻打点;
  - **[minor] 32 位 ms 回绕失明**: 普通钳位在 ~49.7 天回绕后会把冻结时间戳的差值永久钳成 0, 全部掉线/静默检测失明 → `ms_since()` 以 int32 解释差值, 回绕安全;
  - 误报 2 项被对抗验证否决（含一项基于错误硬件前提的 "SDA 卡低时读返回 0x00" 假设 —— RP2040 的 DW_apb_i2c 在 SDA 卡低时事务只会超时/NACK, 不会成功返回）。
- **静态审查**: `grep` 全项目 `vl53l0x_*` / `I2C1` / `xshut` 调用点 —— I2C1 仅存在于 vl53l0x.cpp（Core1）; Core0 仅调用只读 getter; `vl53l0x_reinit_all` 仅被 `run_bus_recovery` 的总线恢复失败分支调用; `vl53l0x_init` 仅在 Core1 启动时调用一次。
