# CHUNITOF PICO

```
   __|   |  |   |  |    \ |  _ _|  __ __|    _ \    __| 
  (      __ |   |  |   .  |    |      |     (   |   _|  
 \___|  _| _|  \__/   _|\_|  ___|    _|    \___/   _|   

        Chunithm Controller for RP2040 Pico
```

[![RP2040](https://img.shields.io/badge/MCU-RP2040-blue)](https://www.raspberrypi.com/products/rp2040/)
[![Platform](https://img.shields.io/badge/Platform-Raspberry%20Pi%20Pico-green)](https://www.raspberrypi.com/products/raspberry-pi-pico/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-orange)](https://isocpp.org/)
[![Framework](https://img.shields.io/badge/Framework-Pico%20SDK-yellow)](https://github.com/raspberrypi/pico-sdk)
[![USB](https://img.shields.io/badge/USB-NKRO%20Keyboard-purple)](https://www.usb.org/)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)

A precision Chunithm controller using capacitive touch sensors and TOF distance sensors, designed for RP2040 Pico.

---

## 📖 Table of Contents

- [简介](#-简介)
- [特性](#-特性)
- [快速开始](#-快速开始)
- [硬件](#-硬件)
- [固件](#-固件)
- [组装](#-组装)
- [配置工具](#-配置工具)
- [开发路线图](#-开发路线图)
- [文档](#-文档)
- [致谢](#-致谢)

---

## 🎮 简介

CHUNITOF PICO 是一个基于 **RP2040 Pico** 的 Chunithm 控制器项目，模拟 **NKRO 键盘输入**，可直接被游戏识别。

### 核心技术

| 组件 | 技术 | 数量 |
|------|------|------|
| **触摸检测** | MPR121 电容式触摸控制器 (I2C0) | 3 个 (36 通道) |
| **空中感应** | VL53L0X 激光测距传感器 (I2C1) | 5 个 |
| **处理核心** | RP2040 双核心架构 | 1 个 |
| **USB 输出** | NKRO 键盘 (TinyUSB) | - |

### 双核心架构

```
Core 0 (主循环)          Core 1 (TOF 专用)
├─ USB HID 输出          ├─ VL53L0X 轮询读取
├─ MPR121 Slider         ├─ 数据缓冲更新
├─ 串口通信              └─ 错误统计
└─ 最小按下时间处理
```

---

## ✨ 特性

- ✅ **完美触摸检测**：经过大量验证的 MPR121 参数，零误触
- ✅ **6 级空中感应**：VL53L0X 阵列实现高度检测
- ✅ **双核心架构**：Core 1 专用读取 TOF，主循环零延迟
- ✅ **NKRO 键盘输出**：无需额外驱动，直接识别
- ✅ **实时配置**：串口调试工具动态调整参数
- ✅ **参数持久化**：配置保存到 Flash，重启后保留
- ✅ **I2C 错误监控**：实时统计通信错误，便于调试

---

## 🚀 快速开始

### 1. segatools.ini 配置

本控制器模拟键盘输入，需要在 `segatools.ini` 中注释所有内容：

```ini
[chuniio]
;所有内容全部在开头加上";"注释掉
; If you wish to sideload a different chuniio, specify the DLL path here
;path=chuniio.dll
; YubiDeck use this
;path=yubideck.dll
; TASOLLER use this
;path=tasoller.dll
; TASOLLER PLUS use this
;path=tasoller_plus.dll
; Brokenithm use this
;path=brokenithm.dll
; chuniio-mux.dll is a aggregate version of io
; this will mount all the above io at the same time
; and no need to adjust any configuration
; you can use Yubideck/TASOLLER/TASOLLER PLUS/Brokenithm and keyboard at the same time
; if you have any problem after mount this dll, please try to mount the dll you need only
; and please do not rename the dll, otherwise it will not be loaded
;path=chuniio-mux.dll
```

本控制器还提供了一个模拟卡片输入的按键。默认绑定的是ENTER键，需要在 `segatools.ini` 中设置virtual card entry（当然，如果你有读卡器，就可以不设置）：
```ini
[aime]
enable=1
```

### 2. 默认键位映射

| Slider 位置 | 按键 | Slider 位置 | 按键 |
|------------|------|------------|------|
| Cell 1-8   | Q W E R T Y U I | Cell 21-28 | L ; ' \ Z X C V |
| Cell 9-16  | O P [ ] A S D F | Cell 29-32 | B N M , |
| Cell 17-20 | G H J K | | |

| Air 高度 | 按键 |
|---------|------|
| Air 1-6 | 4 5 6 7 8 9 |

默认键位对应的`segatools.ini` 配置（io3均为默认值）：

```ini
[ir]
enable=1
ir1=0x34    ; 4
ir2=0x35    ; 5
ir3=0x36    ; 6
ir4=0x37    ; 7
ir5=0x38    ; 8
ir6=0x39    ; 9
[slider]
enable=1
cell1=0x51    ; Q
cell2=0x57    ; W
cell3=0x45    ; E
cell4=0x52    ; R
cell5=0x54    ; T
cell6=0x59    ; Y
cell7=0x55    ; U
cell8=0x49    ; I
cell9=0x4F    ; O
cell10=0x50   ; P
cell11=0xDB   ; [
cell12=0xDD   ; ]
cell13=0x41   ; A
cell14=0x53   ; S
cell15=0x44   ; D
cell16=0x46   ; F
cell17=0x47   ; G
cell18=0x48   ; H
cell19=0x4A   ; J
cell20=0x4B   ; K
cell21=0x4C   ; L
cell22=0xBA   ; ;
cell23=0xDE   ; '
cell24=0xDC   ; \
cell25=0x5A   ; Z
cell26=0x58   ; X
cell27=0x43   ; C
cell28=0x56   ; V
cell29=0x42   ; B
cell30=0x4E   ; N
cell31=0x4D   ; M
cell32=0xBC   ; ,
```

---

## 🔧 硬件

### 材料清单 (BOM)

| 类别 | 元件 | 型号/规格 | 数量 | 备注 |
|------|------|----------|------|------|
| **主控** | MCU | Raspberry Pi Pico | 1 | 或 RP2040 最小系统板 |
| **触摸** | 电容触摸 IC | MPR121 | 3 | I2C 地址: 0x5A, 0x5B, 0x5C |
| **测距** | 激光测距 | VL53L0X 模块 | 5 | I2C 地址动态分配 |
| **连接器** | 排针 | 2.54mm | 若干 | 根据需要 |
| **PCB** | 主板 | - | 1 | Gerber 文件见 Release |

> 📋 完整 BOM 表格见 [GitHub Releases](https://github.com/simonzhou0372/CHUNITHM_TOF_PICO/releases)

### Pin Out

#### I2C0 (MPR121)

| 信号 | GPIO | 引脚 | 连接 |
|------|------|------|------|
| SDA0 | GP16 | Pin 21 | MPR121 SDA |
| SCL0 | GP17 | Pin 22 | MPR121 SCL |

#### I2C1 (VL53L0X)

| 信号 | GPIO | 引脚 | 连接 |
|------|------|------|------|
| SDA1 | GP6 | Pin 9 | VL53L0X SDA |
| SCL1 | GP7 | Pin 10 | VL53L0X SCL |
| XSHUT1 | GP8 | Pin 11 | TOF1 XSHUT |
| XSHUT2 | GP9 | Pin 12 | TOF2 XSHUT |
| XSHUT3 | GP10 | Pin 14 | TOF3 XSHUT |
| XSHUT4 | GP11 | Pin 15 | TOF4 XSHUT |
| XSHUT5 | GP12 | Pin 16 | TOF5 XSHUT |

#### 物理按键

| 按键 | GPIO | 引脚 | 功能 |
|------|------|------|------|
| ENTER | GP18 | Pin 24 | 模拟回车键 ，绑定的是模拟刷aime卡|
| 2 | GP19 | Pin 25 | 模拟数字 2 键 |

#### 其他

| 信号 | GPIO | 引脚 | 连接 |
|------|------|------|------|
| LED | GP25 | 板载 | 状态指示 |

> 📌 Pin out 可在 `src/board_defs.h` 中修改

---

## 📦 固件

### 下载

> 📥 固件发布在 [GitHub Releases](https://github.com/simonzhou0372/CHUNITHM_TOF_PICO/releases)，包含：
> - `Chuni245Tof.uf2` - 固件文件
> - `gerber_xxx.zip` - PCB 制造文件
> - `BOM.csv` - 材料清单
> - `case.dxf` - 外壳图纸
> - `config_cdc.exe` - 参数配置软件

### 烧录方法

烧录方法：
1. 按住RP2040 PICO开发板的 **BOOTSEL** 按钮
2. 连接 USB 线到电脑
3. 松开 **BOOTSEL**
4. 拖放 `Chuni245Tof.uf2` 到 RPI-RP2 驱动器
5. 自动重启完成

---

## 🔨 组装

### 焊接指南

待补充

### 外壳安装

待补充

---

## ⚙️ 配置工具

### GUI 配置工具

两种方式运行配置工具：

**方式 1：直接运行 exe（推荐）**
```bash
config_cdc.exe
```

**方式 2：Python 脚本**
```bash
python tools/config_cdc.py
```

功能：
- ✅ 触摸阈值调整
- ✅ Air 高度范围配置
- ✅ 最小按下时间设置
- ✅ 参数保存到 Flash

### 串口命令

通过串口终端 (115200 8N1) 发送命令：

| 命令 | 功能 | 示例 |
|------|------|------|
| `CONFIG?` | 查询当前配置 | `CONFIG?` |
| `CONFIG` | 设置参数 | `CONFIG 20 18 120 40 150 50` |
| `SAVE` | 保存到 Flash | `SAVE` |
| `DEFAULT` | 恢复默认值 | `DEFAULT` |
| `STATUS` | 查询状态 | `STATUS` |
| `DEBUG` | MPR121 调试信息 | `DEBUG` |
| `HELP` | 帮助信息 | `HELP` |

### CONFIG 参数说明

```
CONFIG <touch> <release> <offset> <pitch> <air6_range> <min_hold>

参数:
  touch      - 触摸阈值 (5-30, 默认 20)
  release    - 释放阈值 (1-25, 默认 18)
  offset     - Air 起始高度 mm (40-200, 默认 120)
  pitch      - Air 每级高度 mm (4-100, 默认 30)
  air6_range - Air6 检测范围 mm (pitch-200, 默认 150)
  min_hold   - 最小按下时间 ms (10-500, 默认 100)
```

---

## 🗺️ 开发路线图

### TODO

- [ ] **GUI 程序功能增强**
  - [ ] 键位配置可自定义（当前固定映射）
  - [ ] 生成对应的 `segatools.ini` 配置文件
  - [ ] Pin Out 可视化编辑器（为了不用我提供的硬件方案，自己做硬件的用户）
  - [ ] 32 个触点到 MPR121 位置的映射配置
  - [ ] 配置导入/导出功能

- [ ] **固件功能扩展**
  - [ ] 游戏手柄模式 (HID Gamepad)
  - [ ] 混合模式 (Gamepad + Keyboard)

- [ ] **文档完善**
  - [ ] 焊接教程（照片/视频）
  - [ ] 外壳组装教程

---

### 源码结构

```
src/
├── main.cpp          # 主程序
├── config.cpp        # 配置管理
├── mpr121.cpp        # MPR121 触摸控制器
├── vl53l0x.cpp       # VL53L0X 测距传感器
├── tof_reader.cpp    # Core 1 读取任务
├── air.cpp           # Air 检测逻辑
├── slider.cpp        # Slider 检测逻辑
└── usb_descriptors.c # USB 描述符
```

---

### 参考项目

- [Adafruit_MPR121](https://github.com/adafruit/Adafruit_MPR121) - MPR121 驱动参考
- [pololu-vl53l0x](https://github.com/pololu/vl53l0x-arduino) - VL53L0X 驱动参考
- [TinyUSB](https://github.com/hathach/tinyusb) - USB 协议栈
- [Pico SDK](https://github.com/raspberrypi/pico-sdk) - RP2040 SDK
- [Chu Pico](https://github.com/whowechina/chu_pico) - 软件架构参考
---

## 📄 License

MIT License - 详见 [LICENSE](LICENSE) 文件

---

> Made with ❤️ by CHUNITOF Team(More like 1 person, me)