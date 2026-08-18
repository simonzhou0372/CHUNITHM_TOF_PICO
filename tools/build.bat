@echo off
REM Chuni245Tof Config Tool - Windows Build Script
REM 使用 PyInstaller 打包成 exe

echo ============================================================
echo Chuni245Tof Config Tool - Windows Build
echo ============================================================
echo.

REM 检查 Python
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found. Please install Python 3.x
    pause
    exit /b 1
)

REM 检查 PyInstaller
python -m pip show pyinstaller >nul 2>&1
if errorlevel 1 (
    echo [INFO] Installing PyInstaller...
    python -m pip install pyinstaller
)

REM 检查 pyserial
python -m pip show pyserial >nul 2>&1
if errorlevel 1 (
    echo [INFO] Installing pyserial...
    python -m pip install pyserial
)

echo.
echo [INFO] Starting build...
echo.

REM 运行 Python 打包脚本
python build_exe.py

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b 1
) else (
    echo.
    echo [SUCCESS] Build completed!
    echo.
    echo Output file: dist\Chuni245TofConfig.exe
    echo.
    pause
)