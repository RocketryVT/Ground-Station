add_executable(gs_pico
    main.cpp
    Tasks/WiFi/wifi_task.cpp
    Tasks/NTP/ntp_task.cpp
    Tasks/MQTT/mqtt_task.cpp
    Tasks/Demo/demo_task.cpp
    Tasks/LoRa/lora_task.cpp
    Tasks/LoRa/lora2_task.cpp
    Tasks/GPS/gps_task.cpp
    Tasks/Starlink/starlink.cpp
    Tasks/UDP/udp_recv_task.cpp
    Tasks/I2C/i2c_task.cpp
    Tasks/IMU/imu_task.cpp
    Tasks/Mag/mag_task.cpp
    Tasks/Baro/baro_task.cpp
    Tasks/Fusion/fusion_task.cpp
    Tasks/Stepper/cl57te.cpp
    Tasks/Stepper/stepper_task.cpp
    Tasks/USB/usb_task.cpp
)

target_include_directories(gs_pico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include   # FreeRTOSConfig.h  lwipopts.h
    ${CMAKE_CURRENT_SOURCE_DIR}             # shared.hpp
    ${LIBS_ROOT}/packets                    # SIGMA.hpp
    ${LORA_ROOT}/hal                        # PicoHal.h
    ${LORA_ROOT}/radio                      # Radio.hpp  SpiDevice.hpp  sx1276/  rf69/  sx1231/
    ${FUSION_PATH}/Fusion                   # Fusion.h (AHRS)
)

target_link_libraries(gs_pico
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_timer
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
    pico_lwip_sntp
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    radio_hal                               # RadioLib + PicoHal (SX1276 + RF69)
    gps                                     # NMEA parser (unused now, kept for future)
    lis3mdl                                 # LIS3MDL magnetometer driver
    ism330dlc                               # ISM330DLC accelerometer/gyroscope driver
    math_utils                              # haversine / azimuth / elevation
    Fusion                                  # x-io Fusion AHRS (Madgwick-derived)
)

pico_enable_stdio_usb(gs_pico 1)
pico_enable_stdio_uart(gs_pico 0)

pico_set_program_name(gs_pico "gs_pico")
pico_set_program_version(gs_pico "0.2")

pico_add_extra_outputs(gs_pico)
