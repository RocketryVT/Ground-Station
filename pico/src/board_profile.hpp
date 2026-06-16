#pragma once

// board_profile.hpp - operational device profile for the ground-station Pico.
//
// The gs_pcb_v1 board header owns the connector-to-GPIO map. This profile
// declares which devices this firmware expects on those connectors and their
// configured operating points.

#define APP_HAS_GPS      1
#define APP_HAS_RADIO    1
#define APP_HAS_SX1276   1
#define APP_HAS_RFM69    1
#define APP_HAS_IMU      1
#define APP_HAS_MAG      1
#define APP_HAS_STEPPERS 1

namespace Board {

inline constexpr RadioInstance Radios[] = {
    { RadioModel::SX1276,   Bus::SPI0, Pins::LORA0_NSS, 915.0f, "915-lora" },
    { RadioModel::RFM69HCW, Bus::SPI1, Pins::LORA1_NSS, 424.5f, "433-gfsk" },
};
inline constexpr int RadioCount = static_cast<int>(std::size(Radios));

inline constexpr GpsInstance Gpses[] = {
    { GpsModel::UbloxM10, Bus::UART0, /*baud*/230400, /*nav_hz*/10, "primary" },
};
inline constexpr int GpsCount = static_cast<int>(std::size(Gpses));

namespace Lora915 {
    inline constexpr float   BW_KHZ    = 125.0f;
    inline constexpr uint8_t SF        = 7;
    inline constexpr uint8_t CR        = 5;
    inline constexpr uint8_t SYNC_WORD = 0x12;
    inline constexpr int8_t  TX_DBM    = 20;
    inline constexpr uint16_t PREAMBLE = 8;
}

namespace Rfm433 {
    inline constexpr float    BR_KBPS    = 4.8f;
    inline constexpr float    FDEV_KHZ   = 5.0f;
    inline constexpr float    RXBW_KHZ   = 125.0f;
    inline constexpr int8_t   TX_DBM     = 20;
    inline constexpr uint16_t PREAMBLE   = 16;
    inline constexpr bool     HIGH_POWER = true;
}

} // namespace Board
