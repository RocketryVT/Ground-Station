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

project(gs_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)

pico_sdk_init()

# LoRa library — provides sx1276_hal (RadioLib) and lr11xx targets.
if (NOT EXISTS "${LORA_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "LoRa drivers not found at ${LORA_ROOT}. Set LORA_ROOT.")
endif()
add_subdirectory(${LORA_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/lora)

# lr11xx_hal.c includes FreeRTOS headers; the config header lives in this
# project's include/ directory and must be forwarded to the library target.
target_include_directories(lr11xx PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# GPS NMEA parser library — provides the 'gps' target.
if (NOT EXISTS "${GPS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "GPS library not found at ${GPS_ROOT}. Set GPS_ROOT.")
endif()
add_subdirectory(${GPS_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/gps)

# LIS3MDL magnetometer driver.
if (NOT EXISTS "${LIS3MDL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "LIS3MDL driver not found at ${LIS3MDL_ROOT}. Set LIS3MDL_ROOT.")
endif()
add_subdirectory(${LIS3MDL_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/lis3mdl)

# ISM330DLC IMU driver.
if (NOT EXISTS "${ISM330DLC_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "ISM330DLC driver not found at ${ISM330DLC_ROOT}. Set ISM330DLC_ROOT.")
endif()
add_subdirectory(${ISM330DLC_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/ism330dlc)

# ICM-20948 9-axis IMU driver.
if (NOT EXISTS "${NINE_AXIS_IMU_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "ICM-20948 driver not found at ${NINE_AXIS_IMU_ROOT}. Set NINE_AXIS_IMU_ROOT.")
endif()
add_subdirectory(${NINE_AXIS_IMU_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/nine_axis_imu)

# Ground-station math — provides the 'math_utils' target.
if (NOT EXISTS "${MATH_UTILS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "math_utils not found at ${MATH_UTILS_ROOT}. Set MATH_UTILS_ROOT.")
endif()
add_subdirectory(${MATH_UTILS_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/math_utils)

# Fusion AHRS library (x-io Technologies) — provides the 'Fusion' target.
if (NOT EXISTS "${FUSION_PATH}/Fusion/CMakeLists.txt")
    message(FATAL_ERROR "Fusion library not found at ${FUSION_PATH}. Set FUSION_PATH.")
endif()
add_subdirectory(${FUSION_PATH}/Fusion ${CMAKE_CURRENT_BINARY_DIR}/Fusion)

add_subdirectory(src)

add_compile_options(
    -Wall
    -Wno-format
    -Wno-unused-function
    -fpermissive
)
