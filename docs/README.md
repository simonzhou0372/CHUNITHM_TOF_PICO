# Chuni245Tof 文档目录

## 核心文档

### [MPR121 和 VL53L0X 技术实现指南](./MPR121_VL53L0X_Troubleshooting_Guide.md)

详细说明了 MPR121 触摸控制器和 VL53L0X 激光测距传感器的技术实现：

- **MPR121 实现**：阈值机制、Filter 配置、调试工具
- **VL53L0X 实现**：测量参数、Air 检测逻辑、最小按下时间
- **双核心架构**：使用 RP2040 的 Core 1 专用读取 VL53L0X，实现最高刷新率
- **最佳实践**：参数配置、性能优化、常见问题解决

---

## 快速链接

- [MPR121 参数配置](./MPR121_VL53L0X_Troubleshooting_Guide.md#mpr121-参数)
- [VL53L0X 参数配置](./MPR121_VL53L0X_Troubleshooting_Guide.md#vl53l0x-参数)
- [Air 参数配置](./MPR121_VL53L0X_Troubleshooting_Guide.md#air-参数)
- [双核心架构说明](./MPR121_VL53L0X_Troubleshooting_Guide.md#双核心架构优化)

---

## 源码文件

| 文件 | 说明 |
|------|------|
| [src/mpr121.cpp](../src/mpr121.cpp) | MPR121 触摸控制器实现 |
| [src/vl53l0x.cpp](../src/vl53l0x.cpp) | VL53L0X 激光测距传感器实现 |
| [src/tof_reader.cpp](../src/tof_reader.cpp) | Core 1 专用读取任务 |
| [src/air.cpp](../src/air.cpp) | Air 检测逻辑 |
| [src/slider.cpp](../src/slider.cpp) | Slider 触摸检测 |
| [src/config.cpp](../src/config.cpp) | 配置管理 |
| [tools/config_cdc.py](../tools/config_cdc.py) | 串口配置工具 |