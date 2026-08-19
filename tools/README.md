# Chuni245Tof Configuration and Monitoring Tool

## 🎯 功能概述

这是一个用于配置和监控的 Python GUI 工具，支持：

- ✅ 实时参数配置（MPR121 阈值、TOF AIR 设置）
- ✅ 实时性能监控（主循环延迟、HID 发送频率、TOF 数据更新）
- ✅ AIR 状态可视化（最大距离、传感器位图、HID 输出）
- ✅ TOF 传感器状态监控（距离、数据年龄、有效性）
- ✅ 调试命令快捷访问（STATUS、AIRDEBUG、PERF）

---

## 📦 依赖项

```bash
pip install pyserial
```

---

## 🚀 使用方法

### 1. 启动工具
```bash
cd tools
python config_cdc.py
```

### 2. 连接设备
1. 点击 **Refresh** 刷新串口列表
2. 选择 `COMx (Chuni245Tof)` 设备
3. 点击 **Connect** 连接

### 3. 配置参数
- **左侧面板**：配置参数
  - MPR121 触摸阈值
  - TOF AIR 检测参数
  - 读取/应用/保存配置

### 4. 实时监控（优化版：固件主动推送）
- **右侧面板**：实时监控
  - 设置监控间隔（默认 100ms，范围 50-5000ms）
  - 点击 **Start Monitoring** 开始监控
  - 固件将自动定期推送数据（无需Python端轮询）
  - 实时显示：
    - 性能统计（主循环延迟、HID 发送次数、TOF 数据更新）
    - AIR 状态（最大距离、传感器位图、HID 输出）
    - TOF 传感器状态（距离、数据年龄、有效性）
  - 点击 **Stop Monitoring** 停止监控

---

## 🔍 调试命令

### START_MONITOR（新增）
```bash
START_MONITOR <interval_ms>
```
启动固件主动推送模式，定期发送监控数据（JSON格式）。
- interval_ms: 监控间隔（50-5000ms）
- 示例: `START_MONITOR 100`

### STOP_MONITOR（新增）
```bash
STOP_MONITOR
```
停止固件主动推送模式。

### 监控数据格式（JSON）
```json
{
  "t": 12345,
  "air": {
    "max": 150,
    "sensor": 1,
    "hid": 1
  },
  "tof": [
    {"d": 150, "a": 5, "v": 1},
    ...
  ],
  "perf": {
    "loop_avg": 150,
    "loop_max": 500,
    "hid_sends": 1250,
    "tof_new": 125,
    "tof_poll_avg": 2500,
    "tof_poll_max": 5000
  }
}
---
```

### STATUS
```json
{
  "slider": "0x00000000",
  "air": "0x00",
  "perf": {
    "loop_avg_us": 150,
    "hid_sends": 1250,
    "tof_new_data": 125,
    ...
  }
}
```

### AIRDEBUG
```
AIR Debug:
  max_dist: 150 mm
  sensor_bmp: 0x01 (100000)
  hid_bmp: 0x01 (100000)
  sensors:
    TOF1: dist=150 mm, age=5 ms, valid=1
    TOF2: dist=120 mm, age=3 ms, valid=1
    ...
```

### PERF
```
Performance Statistics:
  Main loop:
    avg: 150 us
    max: 500 us
  HID:
    sends: 1250
  Core1 TOF:
    new_data: 125
    poll_avg: 2500 us
    poll_max: 5000 us
```

---

## 📊 性能指标解读

### Main Loop
- **loop_avg_us**: 主循环平均耗时（建议 < 500μs）
- **loop_max_us**: 主循环最大耗时（建议 < 2000μs）

### HID
- **hid_sends**: HID 报告发送次数（状态变化时递增）

### Core1 TOF
- **tof_new_data**: 真正获得新数据的次数
- **tof_poll_avg_us**: 轮询平均间隔（建议 2000-3000μs）
- **tof_poll_max_us**: 轮询最大间隔（建议 < 5000μs）

---

## 🎛️ 参数配置建议

### MPR121 触摸阈值
- **Touch Threshold**: 5（默认值）
  - 范围：1-255
  - 越低越容易触发

- **Release Threshold**: 3（默认值）
  - 范围：1-255
  - 越高越容易释放
  - 建议 Touch > Release（差值 ≥ 2）

### TOF AIR 设置
- **Start Height (offset)**: 120mm（推荐值）
  - 范围：40-200mm
  - 不建议 < 100mm（会导致 Air1 持续触发）

- **Step Height (pitch)**: 30mm（推荐值）
  - 范围：4-100mm
  - 不建议 < 30mm（快速抬手可能漏检）

- **Air6 Range**: 150mm（推荐值）
  - 范围：pitch-200mm
  - 控制 Air6 最高检测范围

- **Min Hold Time**: 50ms（推荐值）
  - 范围：10-500ms
  - 防止快速抬手时按键闪烁

---

## ⚠️ 注意事项

### 实时监控
- 监控间隔建议：50-500ms
- 太频繁（< 50ms）可能导致串口拥堵
- 监控期间不要发送其他命令

### 配置保存
- **Apply Config**: 临时应用（重启后失效）
- **Save to Flash**: 永久保存（重启后保留）

### 调试命令
- STATUS: 返回 JSON 格式状态
- AIRDEBUG: 返回 AIR 详细调试数据
- PERF: 返回性能统计
- DIST: 返回 TOF 距离原始数据
- AIR: 返回 AIR 位图

---

## 🐛 故障排除

### 连接失败
1. 检查 USB 线缆连接
2. 确认驱动已安装（Windows 自动识别为 CDC）
3. 尝试其他 USB 端口

### 监控无数据
1. 确认设备已连接
2. 检查波特率（115200）
3. 重启设备并重新连接

### 性能异常
- 如果 `loop_avg_us > 500`：
  - 检查是否有大量 I2C 错误
  - 查看传感器连接状态

- 如果 `tof_poll_avg_us > 5000`：
  - Core 1 可能被阻塞
  - 检查 VL53L0X 初始化状态

---

## 📝 更新日志

### v2.0.0 (2026-08-18)
- ✨ 新增实时监控功能
- ✨ 添加性能统计面板
- ✨ 添加 AIR 状态可视化
- ✨ 添加 TOF 传感器状态监控
- ✨ 优化 GUI 布局（左右分栏）
- 🔧 修复 CONFIG 命令解析（支持 min_hold 参数）
- 📊 新增调试命令（AIRDEBUG、PERF）

### v1.0.0
- 🎉 初始版本
- 基本配置功能
- MPR121 和 TOF 参数设置

---

## 📚 相关文档

- [主项目 README](../README.md)
- [性能优化报告](../docs/performance_report.md)
- [调试指南](../docs/debug_guide.md)