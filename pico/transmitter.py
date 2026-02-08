# transmitter.py  --  GPS → LoRa beacon for the remote board
# Flash this onto the TRANSMITTER Pico 2W.
#
# Reads uBlox GPS, packs lat/lon/alt into a compact ASCII packet,
# and broadcasts it over LoRa at a configurable interval.

from machine import Pin, SPI
from utime import sleep_ms, ticks_ms, ticks_diff

import config
from gps import GPS
from sx127x import SX127x


def build_lora_radio():
    spi = SPI(0,
              baudrate=5_000_000,
              polarity=0,
              phase=0,
              sck=Pin(config.PIN_LORA_SCK),
              mosi=Pin(config.PIN_LORA_MOSI),
              miso=Pin(config.PIN_LORA_MISO))
    cs   = Pin(config.PIN_LORA_CS)
    rst  = Pin(config.PIN_LORA_RST)
    dio0 = Pin(config.PIN_LORA_DIO0)
    return SX127x(spi, cs, rst, dio0,
                  frequency=config.LORA_FREQUENCY,
                  bandwidth=config.LORA_BANDWIDTH,
                  sf=config.LORA_SF,
                  cr=config.LORA_CR,
                  tx_power=config.LORA_TX_POWER,
                  preamble=config.LORA_PREAMBLE,
                  sync_word=config.LORA_SYNC_WORD)


def main():
    print("[TX] Initialising GPS …")
    gps = GPS(config.GPS_UART_ID,
              config.PIN_GPS_TX,
              config.PIN_GPS_RX,
              config.GPS_BAUDRATE)

    print("[TX] Initialising LoRa …")
    lora = build_lora_radio()

    print("[TX] Waiting for GPS fix …")
    while not gps.fix.valid:
        gps.update()
        sleep_ms(100)
    print("[TX] GPS fix acquired:", gps.fix)

    interval_ms = int(config.TX_INTERVAL_S * 1000)
    seq = 0

    print("[TX] Transmitting …")
    try:
        while True:
            t0 = ticks_ms()

            # Keep the NMEA parser fed
            gps.update()

            if gps.fix.valid:
                # Packet format:  "lat,lon,alt,seq\n"
                # 6-decimal-place lat/lon ≈ 11 cm precision
                pkt = "{:.6f},{:.6f},{:.1f},{}\n".format(
                    gps.fix.latitude,
                    gps.fix.longitude,
                    gps.fix.altitude,
                    seq)
                try:
                    lora.send(pkt)
                    print("[TX] #{} → {}".format(seq, pkt.strip()))
                    seq += 1
                except OSError as e:
                    print("[TX] send error:", e)
            else:
                print("[TX] no fix")

            # Sleep for the remainder of the interval
            elapsed = ticks_diff(ticks_ms(), t0)
            remaining = interval_ms - elapsed
            if remaining > 0:
                sleep_ms(remaining)

    except KeyboardInterrupt:
        print("[TX] Stopped.")
    finally:
        lora.sleep()


if __name__ == "__main__":
    main()
