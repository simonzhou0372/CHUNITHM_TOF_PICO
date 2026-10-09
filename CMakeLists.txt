# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.18)

# Pull in SDK
include(pico_sdk_import.cmake)

project(Chuni245Tof C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# Only the cooperative ToF driver requires C++20 coroutine state machines.
set_source_files_properties(src/vl53l0x.cpp PROPERTIES COMPILE_OPTIONS "-std=gnu++20")

# 解决 Windows 命令行长度问题
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_LIBRARIES 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_LIBRARIES 1)
set(CMAKE_C_RESPONSE_FILE_LINK_CXX 1)

# Initialize the SDK
pico_sdk_init()
include(cmake/PatchTinyUsb.cmake)

# Source files
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/save.cpp
    src/mpr121.cpp
    src/slider.cpp
    src/vl53l0x.cpp
    src/tof_reader.cpp
    src/tof_events.cpp
    src/air.cpp
    src/button.cpp
    src/usb_descriptors.c
)

# Executable
add_executable(Chuni245Tof ${SOURCES})

# Include directories
target_include_directories(Chuni245Tof PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

# Compile definitions
target_compile_definitions(Chuni245Tof PRIVATE
    PICO_STDIO_USB_ENABLE_CHARS_PITFALLS=0
)

# Link libraries
target_link_libraries(Chuni245Tof
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
    hardware_i2c
    hardware_flash
    pico_flash        # flash_safe_execute: SDK 官方双核 Flash 安全机制
    pico_multicore
)

# stdio 输出后端全部关闭 (只发送 HID 报文):
# pico stdio_usb 的 CDC 后端在 "主机停止读取" 时每次 printf 最多阻塞 500ms
# (PICO_STDIO_USB_STDOUT_TIMEOUT_US), 饿死 tud_task() 导致 HID 停发。
# 关闭后 SDK 的 stdio_usb 后端 (互斥锁 + 500ms 重试循环) 完全不参与编译;
# CDC 接口仍由 usb_descriptors.c / CFG_TUD_CDC 枚举, 仅用于接收命令。
pico_enable_stdio_usb(Chuni245Tof 0)
pico_enable_stdio_uart(Chuni245Tof 0)

# Extra outputs (UF2, etc.)
pico_add_extra_outputs(Chuni245Tof)

message(STATUS "")
message(STATUS "Chuni245Tof Configuration")
message(STATUS "  Project: Chuni245Tof Controller")
message(STATUS "  PICO_SDK_PATH: ${PICO_SDK_PATH}")
message(STATUS "")
