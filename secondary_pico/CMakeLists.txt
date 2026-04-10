# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2_w)

# Locate the avionics root (3 levels up from this file)
get_filename_component(AVIONICS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include(${AVIONICS_ROOT}/cmake/deps.cmake)

# Pull in Pico SDK (must be before project())
set(PICO_SDK_ROOT ${PICO_SDK_PATH})
if (NOT EXISTS "${PICO_SDK_ROOT}/pico_sdk_init.cmake")
    message(FATAL_ERROR "Pico SDK not found at ${PICO_SDK_ROOT}. "
            "Initialize the submodule or set PICO_SDK_PATH.")
endif()
include(${PICO_SDK_ROOT}/pico_sdk_init.cmake)

# Pull in FreeRTOS RP2350 port
set(FREERTOS_KERNEL_ROOT ${FREERTOS_KERNEL_PATH})
if (NOT EXISTS "${FREERTOS_KERNEL_ROOT}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake")
    message(FATAL_ERROR "FreeRTOS Kernel not found at ${FREERTOS_KERNEL_ROOT}. "
            "Initialize the submodule or set FREERTOS_KERNEL_PATH.")
endif()
include(${FREERTOS_KERNEL_ROOT}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "Disable host -arch when cross-compiling" FORCE)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

project(gs_secondary_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# LoRa library — provides radio_hal (RadioLib + PicoHal for SX1276 + RF69)
if (NOT EXISTS "${LORA_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "LoRa drivers not found at ${LORA_ROOT}. Set LORA_ROOT.")
endif()
add_subdirectory(${LORA_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/lora)

# lr11xx_hal.c includes FreeRTOS headers; the config header lives in this
# project's include/ directory and must be forwarded to the library target.
target_include_directories(lr11xx PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

add_subdirectory(src)

add_compile_options(
    -Wall
    -Wno-format
    -Wno-unused-function
    -fpermissive
)
