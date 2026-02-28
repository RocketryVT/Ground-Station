add_executable(gs_pico
    main.cpp
    Tasks/WiFi/wifi_task.cpp
    Tasks/MQTT/mqtt_task.cpp
    Tasks/LoRa/lora_task.cpp
    Tasks/GPS/gps_task.cpp
    Tasks/Servo/servo_task.cpp
    Tasks/USB/usb_task.cpp
)

# WiFi credentials — pass at configure time:
#   cmake .. -DWIFI_SSID="MyNetwork" -DWIFI_PASSWORD="MyPass"
if (NOT DEFINED WIFI_SSID)
    set(WIFI_SSID "" CACHE STRING "WiFi SSID")
endif()
if (NOT DEFINED WIFI_PASSWORD)
    set(WIFI_PASSWORD "" CACHE STRING "WiFi password")
endif()

target_compile_definitions(gs_pico PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
)

target_include_directories(gs_pico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include   # FreeRTOSConfig.h  lwipopts.h
    ${CMAKE_CURRENT_SOURCE_DIR}             # shared.hpp
    ${LIBS_ROOT}/packets                    # SIGMA.hpp
)

target_link_libraries(gs_pico
    pico_stdlib
    hardware_spi
    hardware_gpio
    hardware_uart                              # GPS UART1
    pico_cyw43_arch_lwip_threadsafe_background # CYW43 WiFi + lwIP (IRQ background, NO_SYS=1)
    pico_lwip_mqtt                             # lwIP built-in MQTT client
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    sx1276_hal                                 # RadioLib + PicoHal.h
    gps                                        # NMEA parser (gps::GPSParser)
    math_utils                                 # GroundStationMath (haversine, azimuth, elevation)
    hardware_pwm                               # Servo PWM
)

pico_enable_stdio_usb(gs_pico 1)
pico_enable_stdio_uart(gs_pico 0)

pico_set_program_name(gs_pico "gs_pico")
pico_set_program_version(gs_pico "0.1")

pico_add_extra_outputs(gs_pico)
