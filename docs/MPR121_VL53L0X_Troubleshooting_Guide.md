# MPR121 和 VL53L0X 技术实现指南

> 本文档详细说明 Chuni245Tof 项目中 MPR121 和 VL53L0X 的技术实现细节。

---

## 📋 目录

- [硬件配置](#硬件配置)
- [MPR121 实现](#mpr121-实现)
  - [阈值机制](#阈值机制)
  - [Filter 配置](#filter-配置)
  - [调试工具](#调试工具)
- [VL53L0X 实现](#vl53l0x-实现)
  - [测量参数](#测量参数)
  - [Air 检测逻辑](#air-检测逻辑)
  - [最小按下时间](#最小按下时间)
- [双核心架构](#双核心架构)
- [最佳实践](#最佳实践)

---

## 硬件配置

### MPR121 配置

| 组件 | I2C 总线 | 地址 | 功能 |
|------|---------|------|------|
| MPR121 #1 | I2C0 | 0x5A | Cell 21-32 |
| MPR121 #2 | I2C0 | 0x5B | Cell 9-20 |
| MPR121 #3 | I2C0 | 0x5C | Cell 1-8 |

### VL53L0X 配置

| 组件 | I2C 总线 | 地址 | 功能 |
|------|---------|------|------|
| VL53L0X #1-5 | I2C1 | 0x30-0x34 | Air 高度检测 |

---

## MPR121 实现

### 阈值机制

MPR121 使用两个阈值进行触摸检测：

```
Delta 值
   ↑
   |     Touch=20 ──────── 触发 Touch
   |        ↓
   |    <迟滞区> (2)
   |        ↓
   |  Release=18 ──────── 触发 Release
   |
   └────────────────────→ 时间
```

**检测逻辑**：
- 当 Delta > Touch Threshold 时 → 触发 Touch
- 当 Delta < Release Threshold 时 → 触发 Release
- 其中：Delta = FilteredData - Baseline

**参数说明**：

| 参数 | 作用 | 推荐值 |
|------|------|--------|
| Touch Threshold | 越低越容易触发 | 20 |
| Release Threshold | 越高越容易释放 | 18 |
| 迟滞区 | 防止抖动 | 2 |

### Filter 配置

经过大量验证的 Filter 参数：

```cpp
// Rising filter (手指离开后 Baseline 恢复跟踪)
mpr_write_byte(addr, MPR121_MHDR, 0x01);
mpr_write_byte(addr, MPR121_NHDR, 0x01);
mpr_write_byte(addr, MPR121_NCLR, 0x0E);
mpr_write_byte(addr, MPR121_FDLR, 0x00);

// Falling filter (手指按住时 Baseline 跟踪)
mpr_write_byte(addr, MPR121_MHDF, 0x01);
mpr_write_byte(addr, MPR121_NHDF, 0x05);
mpr_write_byte(addr, MPR121_NCLF, 0x01);  // 关键参数
mpr_write_byte(addr, MPR121_FDLF, 0x00);

// Touch filter
mpr_write_byte(addr, MPR121_NHDT, 0x00);
mpr_write_byte(addr, MPR121_NCLT, 0x00);
mpr_write_byte(addr, MPR121_FDLT, 0x00);

// Debounce = 0 (无去抖，最低延迟)
mpr_write_byte(addr, MPR121_DEBOUNCE, 0x00);

// CONFIG1 和 CONFIG2
mpr_write_byte(addr, MPR121_CONFIG1, 0x35);
mpr_write_byte(addr, MPR121_CONFIG2, 0x02);
```

**关键点**：
- `NCLF = 0x01`：Baseline 跟踪速度
- `NHDF = 0x05`：Baseline 跟踪滤波
- 不使用 Auto-Configuration

### 调试工具

通过 `DEBUG` 命令查看详细状态：

```
CH  Baseline Filtered  Delta  Touch
 0:    150     145       -5     0
 1:    148     160       +12    1
```

**字段说明**：
- **Baseline**：基准值
- **FilteredData**：滤波后的实时值
- **Delta**：差值（负值表示按下）
- **Touch**：当前状态

---

## VL53L0X 实现

### 测量参数

稳定性优先配置：

```cpp
// MeasurementTimingBudget = 20ms
// 稳定性 > 延迟 >> 精度

// VCSEL pulse periods
write_reg(addr, 0x27, 0x14);  // PRE_RANGE_VCSEL_PERIOD
write_reg(addr, 0x29, 0x14);  // FINAL_RANGE_VCSEL_PERIOD

// 超时设置（约 20ms）
write_reg16(addr, 0x28, 0x0100);  // PRE_RANGE_TIMEOUT
write_reg16(addr, 0x2A, 0x0100);  // FINAL_RANGE_TIMEOUT

// Signal rate limit
write_reg16(addr, 0x44, 0x0020);

// GPIO 配置 - 数据就绪中断
write_reg(addr, 0x0A, 0x04);

// 启动连续测量模式
write_reg(addr, 0x00, 0x03);
```

### Air 检测逻辑

#### 1. 只取最大值

```cpp
uint16_t max_value = 0;

for (int i = 0; i < 5; i++) {
    uint16_t dist = tof_reader_get_distance(i);
    
    // 只取有效范围内且大于当前最大值的
    if (dist < MIN_VALID_MM || dist > MAX_VALID_MM) {
        continue;
    }
    if (dist > max_value) {
        max_value = dist;
    }
}
```

#### 2. 分段检测

```cpp
// Air1: [offset, offset + pitch]
// Air2: [offset + pitch, offset + pitch*2]
// ...
// Air6: [offset + pitch*5, offset + pitch*5 + air6_range]

if (max_value >= offset_mm && max_value <= offset_mm + pitch_mm) {
    sensor_bitmap |= (1 << 0);  // Air1
}
// ...
```

**配置参数**：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| offset | 120mm | Air 起始高度 |
| pitch | 30mm | 每级高度 |
| air6_range | 150mm | Air6 检测范围 |
| min_hold | 100ms | 最小按下时间 |

**配置建议**：
- Offset ≥ 100mm（避免 Air1 误触）
- Pitch ≥ 30mm（避免快速抬手漏检）

### 最小按下时间

防止快速抬手时按键闪烁：

```cpp
static uint8_t hid_air_bitmap = 0;
static uint32_t air_press_time[6] = {0};

void air_update() {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint16_t min_hold_ms = 100;
    
    for (int i = 0; i < 6; i++) {
        bool sensor_on = (sensor_bitmap >> i) & 1;
        bool hid_on = (hid_air_bitmap >> i) & 1;
        
        if (sensor_on && !hid_on) {
            // 立即按下
            hid_air_bitmap |= (1 << i);
            air_press_time[i] = now;
        }
        else if (!sensor_on && hid_on) {
            // 检查是否达到最小按下时间
            if (now - air_press_time[i] >= min_hold_ms) {
                hid_air_bitmap &= ~(1 << i);
            }
        }
    }
}
```

---

## 双核心架构

### 设计思路

利用 RP2040 的双核心特性：
- **Core 0**：主循环（USB、MPR121、串口）
- **Core 1**：专用读取 VL53L0X

### 架构图

```
┌─────────────────────────┬───────────────────────────┐
│        Core 0            │           Core 1          │
│      (主循环)            │        (TOF 专用)          │
├─────────────────────────┼───────────────────────────┤
│ • USB HID 输出           │ • 循环读取 5 个 VL53L0X    │
│ • MPR121 Slider          │ • 数据缓冲更新             │
│ • 串口通信               │ • 错误统计                 │
│ • 最小按下时间处理        │ • 1ms 轮询周期             │
└─────────────────────────┴───────────────────────────┘
```

### 实现细节

#### 1. Core 1 读取任务

```cpp
static volatile uint16_t distance_buffer[5] = {0};
static volatile uint32_t update_time[5] = {0};

static void core1_main() {
    while (core1_running) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        
        for (int i = 0; i < 5; i++) {
            uint16_t dist = vl53l0x_read_distance(i);
            distance_buffer[i] = dist;
            update_time[i] = now;
        }
        
        sleep_ms(1);
    }
}
```

#### 2. 数据新鲜度检查

```cpp
#define MAX_DATA_AGE_MS 50

void air_update() {
    for (int i = 0; i < 5; i++) {
        uint32_t age = tof_reader_get_age(i);
        if (age > MAX_DATA_AGE_MS) {
            continue;  // 数据过时，跳过
        }
        uint16_t dist = tof_reader_get_distance(i);
        // ...
    }
}
```

### 性能对比

| 指标 | 改进前 | 改进后 |
|------|--------|--------|
| 主循环延迟 | ~10ms | ~0ms |
| 数据更新率 | 取决于主循环 | 每 ~5ms |
| 数据新鲜度 | 无检查 | 最大 50ms |

---

## 最佳实践

### MPR121

1. **阈值设置**
   - 使用高基准值（Touch=20, Release=18）
   - 保持小迟滞区（2-6）

2. **Filter 参数**
   - `NCLF = 0x01`（关键）
   - 不使用 Auto-Configuration

3. **调试**
   - 使用 `DEBUG` 命令查看 Baseline/FilteredData
   - 监控 I2C 错误计数

### VL53L0X

1. **测量参数**
   - MeasurementTimingBudget = 20ms
   - 非阻塞读取

2. **配置建议**
   - Offset ≥ 100mm
   - Pitch ≥ 30mm
   - 最小按下时间 50ms

3. **架构优化**
   - 使用双核心架构
   - 数据新鲜度检查

### 通用建议

1. **I2C 通信**
   - 使用超时机制
   - 统计错误计数
   - 使用非阻塞 API

2. **调试输出**
   - 减少 `printf` 频率
   - 使用统计信息

3. **性能优化**
   - 分离数据获取和处理逻辑
   - 使用缓冲区减少锁竞争

---

## 常见问题

### MPR121 触摸后不释放

**原因**：Baseline 跟踪参数不当，`NCLF` 值太大

**解决**：确保 `NCLF = 0x01`

### Air1 持续触发

**原因**：Offset 设置太低

**解决**：Offset ≥ 100mm

### Air 抬手后闪烁

**原因**：数据更新延迟

**解决**：使用最小按下时间（50ms）

---

> 文档版本：1.0  
> 最后更新：2026-08-18