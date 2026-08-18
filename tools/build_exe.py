#!/usr/bin/env python3
"""
Chuni245Tof Configuration Tool - Build Script
使用 PyInstaller 打包成 Windows exe
"""

import os
import sys
import shutil
import subprocess

# 配置
APP_NAME = "ChunitofPicoConfig"
MAIN_SCRIPT = "config_cdc.py"
ICON_FILE = None  # 如果有图标文件，设置为图标路径

# PyInstaller 参数
PYINSTALLER_ARGS = [
    "pyinstaller",
    "--onefile",                    # 打包成单个 exe
    "--windowed",                   # 不显示控制台窗口
    "--name", APP_NAME,             # 输出文件名
    "--clean",                      # 清理临时文件
    "--noconfirm",                  # 不询问确认
]

# 添加图标（如果存在）
if ICON_FILE and os.path.exists(ICON_FILE):
    PYINSTALLER_ARGS.extend(["--icon", ICON_FILE])

# 添加主脚本
PYINSTALLER_ARGS.append(MAIN_SCRIPT)

def main():
    print("=" * 60)
    print(f"打包 {APP_NAME}")
    print("=" * 60)
    print()

    # 检查主脚本是否存在
    if not os.path.exists(MAIN_SCRIPT):
        print(f"❌ 错误：找不到主脚本 {MAIN_SCRIPT}")
        return False

    # 检查依赖
    print("检查依赖...")
    try:
        import tkinter
        print("  ✓ tkinter")
    except ImportError:
        print("  ❌ tkinter 未安装")
        return False

    try:
        import serial
        print("  ✓ pyserial")
    except ImportError:
        print("  ❌ pyserial 未安装，正在安装...")
        subprocess.run([sys.executable, "-m", "pip", "install", "pyserial"], check=True)
        print("  ✓ pyserial 安装完成")

    print()

    # 清理旧的构建文件
    print("清理旧的构建文件...")
    dirs_to_clean = ["build", "dist", "__pycache__", f"{APP_NAME}.spec"]
    for item in dirs_to_clean:
        if os.path.isdir(item):
            shutil.rmtree(item)
            print(f"  ✓ 删除目录: {item}")
        elif os.path.isfile(item):
            os.remove(item)
            print(f"  ✓ 删除文件: {item}")

    print()

    # 运行 PyInstaller
    print("运行 PyInstaller...")
    print(f"  命令: {' '.join(PYINSTALLER_ARGS)}")
    print()

    try:
        result = subprocess.run(PYINSTALLER_ARGS, check=True)
        print()

        # 检查输出
        exe_path = os.path.join("dist", f"{APP_NAME}.exe")
        if os.path.exists(exe_path):
            file_size = os.path.getsize(exe_path) / (1024 * 1024)
            print("=" * 60)
            print("✓ 打包成功！")
            print("=" * 60)
            print(f"  输出文件: {exe_path}")
            print(f"  文件大小: {file_size:.2f} MB")
            print()
            print("使用方法:")
            print(f"  1. 双击运行 {APP_NAME}.exe")
            print("  2. 或从命令行运行:")
            print(f"     {os.path.abspath(exe_path)}")
            print()
            return True
        else:
            print("❌ 错误：找不到生成的 exe 文件")
            return False

    except subprocess.CalledProcessError as e:
        print()
        print(f"❌ PyInstaller 失败: {e}")
        return False
    except Exception as e:
        print()
        print(f"❌ 打包失败: {e}")
        return False

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)