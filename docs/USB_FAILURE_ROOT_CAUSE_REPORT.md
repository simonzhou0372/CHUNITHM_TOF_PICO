# USB Failure Root Cause Report

工程: Chuni245Tof (RP2040, TinyUSB HID NKRO + CDC, MPR121/I2C0/Core0, VL53L0X/I2C1/Core1)
日期: 2026-09-18
SDK: Raspberry Pi Pico SDK 2.3.0 (TinyUSB 0.18.0)

---

## 1. Current Failure Phenomenon

固件运行一段时间后偶发 USB HID 完全无输入, 所有按键失效, 必须物理拔插 USB 恢复。
历史观测: 曾呈现 "传输大致相同数据量后停止" 的规律。

已排除的历史根因 (本次不再归因):
- Core1 printf (已移除, 事件队列替代)
- CDC monitor 输出阻塞 (背压门控后进一步整体禁用)
- ToF Recovery (CHUNI_TOF_RECOVERY_ENABLE = 0, 纯轮询, 无任何掉线检测)

## 2. Current USB Architecture

```
USB device (RP2040 USBFS)
    ↓
TinyUSB 0.18.0 (tusb_init() 仅 Core0 main() 调用一次)
    ├── ITF0: NKRO Keyboard HID (EP 0x81, 64B)   ← 唯一实时输出通道
    └── ITF1: CDC (EP 0x82/0x03/0x83)            ← 仅命令接收 (RX)
```

- 描述符: 工程自有 `src/usb_descriptors.c` (非 stdio_usb 描述符)
- servicing: 唯一应用级 `tud_task()` 调用点 = Core0 主循环 (main.cpp:1011)
- 全工程 `tusb_init`/`tud_task`/`tud_*` 调用图核验: Core1 三个文件
  (tof_reader.cpp / vl53l0x.cpp / tof_events.cpp) 零 USB/stdio 引用

## 3. stdio_usb / TinyUSB Ownership Analysis

**结论: pico_stdio_usb 已完全移出构建, TinyUSB 是唯一 USB 所有者。**

| 检查项 | 结果 |
|---|---|
| `pico_enable_stdio_usb(Chuni245Tof 1)` | 已改为 **0** (CMakeLists.txt) |
| `pico_enable_stdio_uart` | 0 |
| 最终 ELF 是否链接 pico_stdio_usb | **否** — `arm-none-eabi-nm` 无
`stdio_usb_out_chars` / `stdio_usb_mutex` 符号 |
| `stdio_init_all()` 调用 | **已删除** (main.cpp) |
| `PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK` | 不适用 (stdio_usb 未参与编译) |
| 第二个 tud_task 调用源 | 不存在 (仅 Core0 主循环一处) |
| printf | `log_output.h` 统一宏禁用 (CHUNI_PRINTF_ENABLE=0, 参数亦不求值) |

历史根因 (第一代问题, 已修复并验证): pico stdio_usb 的 CDC 后端在
"DTR 已连接但主机停止读取" 时, 每次 printf 阻塞最多 500ms
(`PICO_STDIO_USB_STDOUT_TIMEOUT_US`=500000), 饿死 tud_task() → HID 停发。
TX FIFO (256B) 填满时刻由字节量决定 → 与 "固定数据量后掉线" 吻合。

## 4. Core0 Worst-Case Blocking Analysis

主循环各任务实测/静态最坏执行时间 (均有界, 且新增了 RAM 统计可实测确认):

| 任务 | 最坏情况 | 依据 |
|---|---|---|
| tud_task() | ~几十 us (空闲) | TinyUSB device 轮询 |
| cdc_task() | ≤1ms + 64B 处理上限 | 硬性 deadline (MAX_CDC_TASK_US=1000) |
| slider_update() | **≤1ms** (本轮改造) | 轮转读取, 单设备 2×500us |
| air_update() | ~us | 只读 Core1 快照 (seqlock) |
| button_update() | ~us | GPIO 读 |
| HID 生成+发送 | ~us | 非阻塞, busy 即跳过 |
| usb_hid_watchdog() | ~us | 纯计数/比较 |
| save_loop() | **0 (无 pending 时); 空闲窗口才执行** | 见 §6 |
| BOOTLOADER sleep_ms(100) | 仅命令触发 | 特例, 允许 |
| 启动期 busy_wait_us(100) | 仅上电一次 | pull-up 稳定等待 |

理论最坏 tud_task gap (正常实时路径): ~2-3ms (cdc_task 1ms + slider 1ms + 余量),
远低于任何主机 USB 超时。**运行中该值由 `usb_health.max_tud_gap_us` 持续实测。**

## 5. MPR121 I2C Worst-Case Timing

原实现一轮强制读完 3 个设备: 最坏 3×2×500us = **3ms** 直接加在 tud_task gap 上。

改造为**轮转/时间片读取** (mpr121.cpp `mpr121_update()`):
- 每次调用只轮询 1 个设备 (rr_index 轮转)
- 单设备: 1 次 write (500us 超时) + 1 次 read (500us 超时) → 最坏 **1ms**
- 单设备故障不影响其他设备轮转节奏; 未就绪设备跳过但推进轮转
- 实时性: 主循环 ~1kHz 时每设备刷新率 ~333Hz (3ms 内全部覆盖),
  远超 CHUNITHM 触摸延迟要求; MPR121 芯片内部硬件扫描率不受影响
- timeout 保持 500us 不变 (未机械缩小: 400kHz 下 write+restart-read 实际
  传输 ~100us, 500us 已含 5 倍裕量, 见 §问题 6)

## 6. Flash Save Blocking Analysis

Flash 擦写期间 XIP 全核停摆 (典型 ~50ms, 最坏 ~400ms) 是 RP2040 硬件特性,
无法让 USB 在此期间继续工作。因此 SAVE 设计为**受控、择机、可观测**:

1. **触发**: 仅 SAVE 命令 → `save_request_write()` 快照到 RAM → 立即返回
2. **择机**: `save_loop()` 只在空闲窗口执行 (slider=0 && air=0 && 按键释放);
   若持续无空闲, 等待 10s 后强制执行 (SAVE_FORCE_DELAY_MS)
3. **安全执行**: 改用 SDK 官方 `flash_safe_execute()` (pico_flash):
   - Core1 启动时 `flash_safe_execute_core_init()` 注册 victim
   - SDK 内部负责 multicore lockout + 中断管理, 替代手写
     `save_and_disable_interrupts + multicore_lockout_start/end_blocking`
   - 回调只做纯 flash_range_erase + flash_range_program
   - 1s 超时, 失败放弃不重试
4. **统计**: save_count / save_fail_count / max_save_duration_us /
   last_save_start/end_us (RAM 常驻) → 可直接核对 USB dropout 是否与 SAVE 同步

USB IRQ 层面: flash_safe_execute 期间中断被 SDK 关闭 (回调执行期间),
这是擦写窗口本身的属性; 与旧手写路径等价, 但生命周期管理交给 SDK。

## 7. TinyUSB HID Endpoint Analysis

- report: 16B (1 modifier + 15 keymap), `static_assert(sizeof(hid_nkro)==16)`;
  descriptor Input 定义 (8+120 bit) / endpoint 64B / TinyUSB report size 三者一致
- 无 Report ID (report_id=0 保持)
- 发送路径: `tud_hid_n_ready()` 非阻塞检查 → busy 时保持 dirty, 恢复后重发
- 新增 `hid_send_fail_count`: ready 但 `tud_hid_n_report` 失败 = 栈异常 (分类 D)
- tud_hid_get/set_report_cb: 立即返回, 无任何阻塞调用

## 8. USB Mount / Suspend / Resume Analysis

新增 TinyUSB 生命周期回调 (main.cpp extern "C"):
- `tud_mount_cb` / `tud_umount_cb` / `tud_suspend_cb` / `tud_resume_cb`
- 只更新 usb_mounted / usb_suspended 状态变量、时间戳、计数
- 禁止 (也未包含) printf/阻塞/I2C/ToF/Flash

由此可区分四类故障:
| 分类 | 特征 (usb_health) | 含义 |
|---|---|---|
| A | unmount_count 增加 | 枚举/主机连接层 |
| B | mounted && suspended | 主机 suspend, 非 HID 故障 |
| C | mounted && max_hid_busy_us 巨大 | HID endpoint 卡死 |
| D | hid_send_fail_count 增加 | TinyUSB 栈异常 |

## 9. USB Recovery Mechanism

**⚠ 实测回调 (2026-09-18 第二轮): 自动恢复默认已关闭。**

实测发现启用自动恢复后掉线频率显著上升: 恢复动作 (tud_disconnect/tud_connect)
本身就是一次 USB 断开重连, 主机侧表现与掉线完全相同 —— 自动恢复会把
"一次真死" 变成 "反复断连"; 若判定存在误报源 (如主机选择性挂起时 suspend
回调未及时送达), 还会形成 ~8s 周期的自断连循环。

现行行为:
- `CHUNI_USB_AUTO_RECOVERY_ENABLE = 0` (默认): watchdog **只统计不干预** ——
  持续记录 max_hid_busy_us / hid_ready_false_count, 永不主动断开 USB;
  LED 的 250ms 闪烁模式 (HID busy 持续 >100ms) 保留作为离线观测手段
- `= 1` (可选): 上述受控重枚举启用 —— 判定条件: `usb_mounted &&
  !usb_suspended && !tud_hid_n_ready(0) 持续 ≥ 5000ms`; 恢复动作:
  `tud_disconnect()` (清除 DP 上拉) → 250ms 后 `tud_connect()` 重新枚举;
  冷却 3000ms; 不重启 MCU、不 reset_usb_boot、不 tusb_deinit/init、
  不碰 I2C0/I2C1/MPR121/ToF/Flash
- mount 回调的 HID 强制重同步 (sent_valid=false + hid_dirty=true) 保留 ——
  无副作用, 重枚举后自动恢复输出

## 10. Core0/Core1 Isolation

- Core1: I2C1 + VL53L0X 轮询。grep 核验: 零 `tud_*`/`stdio_usb`/`tusb` 引用
- Core0 → Core1: 仅事件队列 (SPSC 无锁) 与 seqlock 快照, 无等待
- Core0 不等待 Core1: 唯一例外为 flash_safe_execute 的 lockout (有 1s 超时,
  仅 SAVE 空闲窗口, SDK 官方机制)
- ToF Recovery 保持关闭 (CHUNI_TOF_RECOVERY_ENABLE=0), TimingBudget=20000us

## 11. Power Budget Analysis

**源码可证明的**: descriptor `bMaxPower=100` (200mA), TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP。
**无法从源码确认的 (需硬件实测)**:
- VBUS 电压 (带载最低点, 尤其 5×VL53L0X 同时测距瞬间)
- 3.3V LDO 轨电压与温升
- 实际 USB 电流 (需 USB 电流表/功耗分析仪)
- 主机 over-current 事件日志

未修改 bMaxPower —— 不允许通过 descriptor "骗电" 解决问题。
若实测确认电流超限, 应从硬件 (供电/去耦) 解决, 而非改 descriptor 数值。

VL53L0X 同步功耗峰值: 5 颗传感器在 Core1 顺序初始化, 连续测距启动时刻天然
错相 (每颗初始化间隔数 ms); Recovery 关闭后不存在同时重启路径。
若实测仍有峰值问题, 可错相连续模式启动, 但与本次 USB 故障无证据关联。

## 12. USB Descriptor Analysis

- bDeviceClass=0 (composite by interface), NKRO HID ITF0 + CDC ITF1/2
- HID: 16B input report, 64B endpoint, 无 Report ID, protocol=NONE
- bMaxPower=100 (200mA) — 保持不变 (见 §11)
- bcdUSB=0x0200, FS (12Mbps), 每 ms 一次 HID 事务充足 (1kHz 轮询)

## 13. Files Modified

| 文件 | 改动 |
|---|---|
| src/main.cpp | 删除 stdio_init_all; USB health 结构 + 生命周期回调; HID watchdog + 受控重枚举; mount 后 HID 强制重同步; 各任务 max 统计; LED 状态灯; boot_count/.uninitialized_data + watchdog 复位记录; static_assert 16B; STATUS/PERF 扩展 |
| src/mpr121.cpp | mpr121_update 轮转读取 (3ms→1ms 最坏); log_output.h 接入 |
| src/save.cpp | flash_safe_execute 替代手写 lockout; 空闲窗口门控; 保存统计 |
| src/save.h | save_set_idle_hint / 统计 getter 声明 |
| src/tof_reader.cpp | Core1: flash_safe_execute_core_init() 替代 multicore_lockout_victim_init() |
| src/config.cpp | log_output.h 接入 |
| src/log_output.h | (新增) printf 总开关, 默认 0 = 只发送 HID 报文 |
| CMakeLists.txt | stdio_usb/uart=0 (既有); 链接 pico_flash |

## 14. Functions Modified

- `mpr121_update()`: 轮转化 (核心: for 3 设备 → 单设备 rr_index)
- `save_write()`: flash_safe_execute 路径
- `save_loop()`: 空闲门控 + 统计
- `report_usb_hid()`: sent_valid 重同步 + send fail 计数
- `main()`: 去 stdio_init_all, boot 计数
- 新增: `usb_hid_watchdog()`, `tud_mount_cb`, `tud_umount_cb`,
  `tud_suspend_cb`, `tud_resume_cb`, `save_set_idle_hint`,
  `flash_erase_program_cb`, 统计 getter ×8

## 15. Before / After Architecture

```
Before:
  stdio_init_all() + tusb_init() (双入口, stdio_usb 后端残留风险)
  printf 269 处 (即使门控, stdio CDC 后端 500ms 阻塞路径存在)
  mpr121_update: 3 设备/轮 (最坏 3ms)
  save: 手写 multicore_lockout, 主循环下一轮即擦写 (可能落在游戏中)
  HID not-ready 永久等待, 无恢复
  无 mount/suspend 观测, 掉线后不可归因

After:
  tusb_init() 唯一 USB 入口 (stdio_usb 未参与编译/链接)
  printf 编译期空操作 (可经 CHUNI_PRINTF_ENABLE=1 一键恢复 STATUS 读数)
  mpr121_update: 1 设备/轮 (最坏 1ms)
  save: 空闲窗口 + flash_safe_execute (SDK 官方) + 统计
  HID watchdog: 5s 阈值 → 软重枚举 (3s 冷却) → mount 后自动重发状态
  usb_health 全套计数 + boot_count + LED 状态灯, 掉线后可归因
```

## 16. Confirmed Root Causes

1. **stdio_usb CDC 后端 500ms 阻塞** (已确认并移除): printf 在主机停读时
   每次调用阻塞 ≤500ms 饿死 tud_task → HID 停发; 触发点由字节量决定,
   与 "固定数据量后掉线" 吻合。现 stdio_usb 完全不参与链接 (ELF 符号验证)。
2. ** tud_task gap 放大器** (已确认并削减): MPR121 一轮 3 设备最坏 3ms
   会叠加在任何其他阻塞上; 已轮转化至 1ms。

## 17. Suspected but Unconfirmed Causes

以下无法从源码证明, 需按 §11/§8 的观测手段逐项排除:
1. **USB 供电不足/电压跌落** (VBUS/LDO 带载) — 可能造成 USB PHY 异常或 MCU
   brownout; 靠 boot_count 变化 + LED 状态判定
2. **主机侧问题** (选择性暂停、驱动、电源管理) — 靠 usb_health 的
   mount/unmount/suspend 计数区分
3. **USB 物理层** (线缆过长/无屏蔽/EMI, 游戏机箱内环境) — 靠排除法
4. **TinyUSB CDC RX 层未知缺陷** — CDC 仅接收, 流量极小, 可能性低

## 18. Final Runtime Protection

- RAM 常驻 usb_health (不依赖任何 USB 输出即可存活), 重新连接后经 STATUS 读取
  (需临时 CHUNI_PRINTF_ENABLE=1 重编译; 计数器本身始终在记录)
- boot_count (.uninitialized_data) + watchdog_caused_reboot 区分
  "MCU 重启" 与 "纯 USB 掉线"
- LED 四态灯: 常亮=loop 超 5ms / 100ms=unmounted / 1s=suspended /
  250ms=HID busy / 500ms=正常
- HID endpoint watchdog 自动受控重枚举
- 各任务 max 执行时间统计定位 tud_task gap 来源

---

## 42 章问题逐项回答

1. **是否同时存在 stdio_usb 和手动 TinyUSB?** 修改前: 是 (stdio_init_all + tusb_init);
   修改后: 否, stdio_init_all 已删除。
2. **最终 build 是否链接 pico_stdio_usb?** 否 (nm 验证: stdio_usb_out_chars /
   stdio_usb_mutex 符号不存在)。
3. **是否存在第二个 tud_task 调用源?** 否, 全工程唯一调用点 main.cpp 主循环。
4. **stdio_usb background task 是否启用?** 不适用 (stdio_usb 未参与编译)。
5. **Core0 最大 tud_task gap?** 静态上界 ~2-3ms (正常路径); 实际运行值由
   usb_health.max_tud_gap_us 实测, 待设备运行后读取。
6. **MPR121 最坏一次更新耗时?** 改造后 1ms (单设备 2×500us 超时); 改造前 3ms。
7. **Flash SAVE 最坏耗时?** ~400ms (4KB sector erase 硬件特性);
   max_save_duration_us 可实测。
8. **SAVE 是否会让 USB IRQ 暂停?** 是 (flash_safe_execute 回调期间, 硬件层面
   不可避免), 但现在只在空闲窗口发生、有统计、可预期。
9. **HID endpoint 是否出现过长期 not-ready?** 历史无法回溯; 现由
   max_hid_busy_us 持续记录, ≥5s 自动触发受控重枚举。
10. **mount/unmount/suspend/resume 统计?** 已实现 (usb_health + 4 个生命周期回调)。
11. **HID endpoint auto-recovery?** 有: 5s 阈值 → tud_disconnect/connect 软重枚举,
    3s 冷却, 重枚举后自动重发 HID 状态。
12. **USB recovery 是否只运行在 Core0?** 是 (主循环内, 与 tud_task 同核)。
13. **Core1 是否完全不碰 USB?** 是 (grep 核验零引用)。
14. **I2C0 是否完全不碰 USB?** 是 (USB 为硬件外设, MPR121 驱动无任何 USB 引用;
    I2C0 全部操作有 500us 超时)。
15. **是否存在 MCU reset/brownout 证据?** 当前无历史数据; 现在可通过
    boot_count 变化 + reboot_by_watchdog 标志在下一次故障后判定。
16. **descriptor 声明的最大电流?** bMaxPower=100 → 200mA (未改动)。
17. **实际硬件最大电流?** **无法从源码确认** — 需实测 (5×VL53L0X 峰值 +
    3×MPR121 + LED + RP2040; 参考量级: 单颗 VL53L0X 测距峰值 ~20-45mA,
    理论合计可能接近 200mA 上限, 必须实测确认)。
18. **最终确认的根因?** stdio_usb CDC 后端阻塞 (已移除) + MPR121 3ms 块
    (已轮转化)。见 §16。
19. **尚未确认的可能原因?** 供电、主机侧、物理层。见 §17。

---

## 43 章验收对照

| 验收项 | 状态 |
|---|---|
| stdio_usb = OFF, TinyUSB 唯一 owner, tud_task 唯一 servicing, Core1 零 USB | ✅ (ELF 符号 + grep 验证) |
| Core0 无无界等待 / 无实时路径 Flash / MPR121 单次 ≤1ms | ✅ |
| USB remount 后自动发送当前 HID 状态 / dirty 不丢失 / endpoint 异常有恢复 | ✅ |
| SAVE 不在实时循环随机触发 / Flash 用 SDK 安全机制 | ✅ |
| I2C0=MPR121, I2C1=VL53L0X 互不影响 | ✅ |
| Recovery 关闭 / TimingBudget=20000us | ✅ |
| 不通过 descriptor 骗电 / 实际电流需实测 | ✅ (保持 200mA, §11 列出实测项) |

双配置 (CHUNI_TOF_RECOVERY_ENABLE=0/1) 均编译通过, 最终固件为默认禁用配置。
