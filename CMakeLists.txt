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

cmake_minimum_required(VERSION 3.13)

# Pull in SDK
include(pico_sdk_import.cmake)

project(Chuni245Tof C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 解决 Windows 命令行长度问题
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_LIBRARIES 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_LIBRARIES 1)
set(CMAKE_C_RESPONSE_FILE_LINK_CXX 1)

# Initialize the SDK
pico_sdk_init()

# Source files
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/save.cpp
    src/mpr121.cpp
    src/slider.cpp
    src/vl53l0x.cpp
    src/tof_reader.cpp
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
    pico_multicore
)

# Enable USB stdio
pico_enable_stdio_usb(Chuni245Tof 1)
pico_enable_stdio_uart(Chuni245Tof 0)

# Extra outputs (UF2, etc.)
pico_add_extra_outputs(Chuni245Tof)

message(STATUS "")
message(STATUS "Chuni245Tof Configuration")
message(STATUS "  Project: Chuni245Tof Controller")
message(STATUS "  PICO_SDK_PATH: ${PICO_SDK_PATH}")
message(STATUS "")