add_executable(gs_secondary_pico
    main.cpp
    Tasks/WiFi/wifi_task.cpp
    Tasks/LoRa/lora1_task.cpp
    Tasks/LoRa/lora2_task.cpp
    Tasks/UDP/udp_send_task.cpp
    Tasks/USB/usb_task.cpp
)

target_include_directories(gs_secondary_pico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include   # FreeRTOSConfig.h  lwipopts.h
    ${CMAKE_CURRENT_SOURCE_DIR}             # shared.hpp
    ${LIBS_ROOT}/packets                    # SIGMA.hpp
    ${LORA_ROOT}/hal                        # PicoHal.h
    ${LORA_ROOT}/radio                      # SX1276Radio.hpp  RF69Radio.hpp
)

target_link_libraries(gs_secondary_pico
    pico_stdlib
    hardware_spi
    hardware_gpio
    pico_cyw43_arch_lwip_threadsafe_background
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    radio_hal                               # RadioLib + PicoHal (SX1276 + RF69)
)

pico_enable_stdio_usb(gs_secondary_pico 1)
pico_enable_stdio_uart(gs_secondary_pico 0)

pico_set_program_name(gs_secondary_pico "gs_secondary_pico")
pico_set_program_version(gs_secondary_pico "0.1")

pico_add_extra_outputs(gs_secondary_pico)
