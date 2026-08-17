#!/usr/bin/env python3
"""
检测所有串口设备的 VID/PID
"""

import serial.tools.list_ports

VID = 0x0F0D
PID = 0x0092

print("=" * 50)
print("检测所有串口设备")
print("=" * 50)
print()

found_target = False

for port in serial.tools.list_ports.comports():
    print(f"端口: {port.device}")
    print(f"  描述: {port.description}")
    print(f"  VID:  0x{port.vid:04X}" if port.vid else "  VID:  None")
    print(f"  PID:  0x{port.pid:04X}" if port.pid else "  PID:  None")
    print(f"  制造商: {port.manufacturer}")
    print(f"  产品: {port.product}")
    print()

    if port.vid == VID and port.pid == PID:
        found_target = True
        print(f"  >>> 找到目标设备!")

print("=" * 50)
if found_target:
    print(f"找到 Chuni245Tof 设备!")
else:
    print(f"未找到 Chuni245Tof 设备")
    print(f"期望 VID: 0x{VID:04X}, PID: 0x{PID:04X}")
print("=" * 50)

input("\n按回车键退出...")