# Ground Station Hardware

2 signal pins per servo, 2 servos = 4 GPIO pins
Starlink GPS over WiFi
LIS3MDL magnetometer over I2C0
ISM330DLC IMU over I2C0
RFM95W (915mhz) [SX1276] FSK/(G)FSK/MSK/(G)MSK/LoRa/OOK module over SPI1
RFM69HCW (433mhz) [SX1231H] FSK/GFSK/MSK/GMSK/OOK module over SPI0

- Nosecone (Top) uses a RFM95W
- ADS (Bottom) uses a [LR1121] (433mhz) LoRa/(G)FSK

- Antenna Tracker has 2 high gain antennas, 433mhz and 915mhz
  - 433mhz antenna is connected to the RFM69HCW
  - 915mhz antenna is connected to the RFM95W

- Ground/Table has 2 low gain antennas, 433mhz and 915mhz
  - 433mhz antenna is connected to the RFM69HCW
  - 915mhz antenna is connected to the RFM95W

| Physical Pin | GPIO | Name      | Signal Designation                      |
|--------------|------|-----------|-----------------------------------------|
| 1            | 0    | I2C0 SDA  | A / B                                   |
| 2            | 1    | I2C0 SCL  | A / B                                   |
| 4            | 2    | I2C1 SDA  | A / B                                   |
| 5            | 3    | I2C1 SCL  | A / B                                   |
| 6            | 4    | UART1 TX  | **Servo 1 PUL-** top                    |
| 7            | 5    | UART1 RX  | **Servo 1 DIR-**                        |
| 9            | 6    | UART1 CTS | **Servo 2 PUL-** bot                    |
| 10           | 7    | UART1 RTS | **Servo 2 DIR-**                        |
| 11           | 8    | SPI1 RX   | LoRa1: MISO left SPI                    |
| 12           | 9    | SPI1 CSn  | LoRa1: CS                               |
| 14           | 10   | SPI1 SCK  | LoRa1: SCK                              |
| 15           | 11   | SPI1 TX   | LoRa1: MOSI                             |
| 16           | 12   | UART0 TX  | x                                       |
| 17           | 13   | UART0 RX  | x                                       |
| 19           | 14   | UART0 CTS | x                                       |
| 20           | 15   | UART0 RTS | x                                       |
| 21           | 16   | x         | LoRa0: EN right SPI                     |
| 22           | 17   | x         | LoRa0: G0                               |
| 24           | 18   | SPI0 SCK  | LoRa0: SCK                              |
| 25           | 19   | SPI0 TX   | LoRa0: MOSI                             |
| 26           | 20   | SPI0 RX   | LoRa0: MISO                             |
| 27           | 21   | SPI0 CSn  | LoRa0: CS                               |
| 29           | 22   | x         | LoRa0: RST                              |
| 31           | 26   | x         | LoRa1: RST                              |
| 32           | 27   | x         | LoRa1: G0                               |
| 34           | 28   | x         | LoRa1: EN                               |
| 38           | x    | GND       |                                         |
| 39           | x    | VSYS 5V*  | BAT-5V-DIODE-IN                         |
