# Ground Station

Pico 2W-based ground station for rocket tracking. Receives telemetry via dual LoRa radios, gets ground-station GPS position from Starlink over WiFi/MQTT, and drives two differential servo axes for antenna pointing.

## Hardware Overview

| Subsystem            | Interface  | Notes                                  |
| -------------------- | ---------- | -------------------------------------- |
| LoRa 1 (SX1276)      | SPI0       | 915 MHz telemetry RX                   |
| LoRa 2 (RFM69HCW)    | SPI1       | 433 MHz backup / command link          |
| GPS (ground station) | WiFi       | Position from Starlink router via MQTT |
| Servo 1 (Zenith)     | UART0 pins | Differential step/dir driver           |
| Servo 2 (Azimuth)    | UART1 pins | Differential step/dir driver           |
| Barometer            | I2C0       | Not populated – future expansion       |
| IMU + Magnetometer   | I2C1       | Not populated – future expansion       |

---

## Pico 2W Pinout

| Phys Pin | GPIO | Signal                | Notes                           |
| -------- | ---- | --------------------- | ------------------------------- |
| 1        | 0    | Servo 1 PUL+          | UART0 TX repurposed as GPIO     |
| 2        | 1    | Servo 1 PUL−          | UART0 RX repurposed as GPIO     |
| 4        | 2    | Servo 1 DIR+          | UART0 CTS repurposed as GPIO    |
| 5        | 3    | Servo 1 DIR−          | UART0 RTS repurposed as GPIO    |
| 6        | 4    | Servo 2 PUL+          | UART1 TX repurposed as GPIO     |
| 7        | 5    | Servo 2 PUL−          | UART1 RX repurposed as GPIO     |
| 9        | 6    | Servo 2 DIR+          | UART1 CTS repurposed as GPIO    |
| 10       | 7    | Servo 2 DIR−          | UART1 RTS repurposed as GPIO    |
| 11       | 8    | LoRa1 RST             | SX1276 / 915 MHz                |
| 12       | 9    | LoRa1 G0 (DIO0 / IRQ) | SX1276 / 915 MHz                |
| 14       | 10   | LoRa1 EN              | SX1276 / 915 MHz power enable   |
| 15       | 11   | LoRa2 RST             | RFM69HCW / 433 MHz              |
| 16       | 12   | SPI1 MISO — LoRa2     | RFM69HCW / 433 MHz              |
| 17       | 13   | SPI1 CS   — LoRa2     | RFM69HCW / 433 MHz              |
| 19       | 14   | SPI1 SCK  — LoRa2     | RFM69HCW / 433 MHz              |
| 20       | 15   | SPI1 MOSI — LoRa2     | RFM69HCW / 433 MHz              |
| 21       | 16   | I2C0 SDA              | Not populated (Barometer)       |
| 22       | 17   | I2C0 SCL              | Not populated (Barometer)       |
| 24       | 18   | SPI0 SCK  — LoRa1     | SX1276 / 915 MHz                |
| 25       | 19   | SPI0 MOSI — LoRa1     | SX1276 / 915 MHz                |
| 26       | 20   | SPI0 MISO — LoRa1     | SX1276 / 915 MHz                |
| 27       | 21   | SPI0 CS   — LoRa1     | SX1276 / 915 MHz                |
| 29       | 22   | LoRa2 G0 (DIO0 / IRQ) | RFM69HCW / 433 MHz              |
| 31       | 26   | I2C1 SDA              | Not populated (IMU / Mag)       |
| 32       | 27   | I2C1 SCL              | Not populated (IMU / Mag)       |
| 34       | 28   | LoRa2 EN              | RFM69HCW / 433 MHz power enable |

---

## Servo Connector Pinout

Each servo driver expects differential step/direction signals (e.g. RS-422 / open-collector driver).

| Signal | Servo 1 (Zenith) GPIO | Servo 2 (Azimuth) GPIO | JST Pin |
| ------ | --------------------- | ---------------------- | ------- |
| PUL+   | 0                     | 4                      | 1       |
| PUL−   | 1                     | 5                      | 2       |
| DIR+   | 2                     | 6                      | 3       |
| DIR−   | 3                     | 7                      | 4       |

---

## LoRa Module Connector Pinout

Both LoRa headers share the same signal ordering. SPI bus differs per module.

| Signal | LoRa1 (SX1276 / 915 MHz) | LoRa2 (RFM69HCW / 433 MHz) | JST Pin |
| ------ | ------------------------ | -------------------------- | ------- |
| EN     | GPIO 10                  | GPIO 28                    | 1       |
| G0     | GPIO 9                   | GPIO 22                    | 2       |
| SCK    | GPIO 18 (SPI0)           | GPIO 14 (SPI1)             | 3       |
| MISO   | GPIO 20 (SPI0)           | GPIO 12 (SPI1)             | 4       |
| MOSI   | GPIO 19 (SPI0)           | GPIO 15 (SPI1)             | 5       |
| CS     | GPIO 21 (SPI0)           | GPIO 13 (SPI1)             | 6       |
| RST    | GPIO 8                   | GPIO 11                    | 7       |
| GND    | —                        | —                          | 8       |
| 3V3    | —                        | —                          | 9       |

---

## I2C Headers (not populated)

| Bus  | SDA     | SCL     | Planned Devices   |
| ---- | ------- | ------- | ----------------- |
| I2C0 | GPIO 16 | GPIO 17 | Barometer         |
| I2C1 | GPIO 26 | GPIO 27 | IMU, Magnetometer |

---

## GPS

Ground-station position is obtained from a Starlink router over WiFi. No dedicated UART/GPIO pins are used. The Pico 2W connects to the router SSID and subscribes to a GPS MQTT topic (`gs/gps`).
