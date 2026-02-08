# sx127x.py  --  Minimal SX1276/77/78/79 LoRa driver for MicroPython
# Designed for RP2350 / Pico 2W over hardware SPI.

from machine import Pin, SPI
from utime import sleep_ms, ticks_ms, ticks_diff

# ── SX127x register addresses ───────────────────────────────────────
_REG_FIFO              = 0x00
_REG_OP_MODE           = 0x01
_REG_FRF_MSB           = 0x06
_REG_FRF_MID           = 0x07
_REG_FRF_LSB           = 0x08
_REG_PA_CONFIG         = 0x09
_REG_OCP               = 0x0B
_REG_LNA               = 0x0C
_REG_FIFO_ADDR_PTR     = 0x0D
_REG_FIFO_TX_BASE      = 0x0E
_REG_FIFO_RX_BASE      = 0x0F
_REG_FIFO_RX_CURRENT   = 0x10
_REG_IRQ_FLAGS         = 0x12
_REG_RX_NB_BYTES       = 0x13
_REG_PKT_SNR           = 0x19
_REG_PKT_RSSI          = 0x1A
_REG_MODEM_CFG1        = 0x1D
_REG_MODEM_CFG2        = 0x1E
_REG_PREAMBLE_MSB      = 0x20
_REG_PREAMBLE_LSB      = 0x21
_REG_PAYLOAD_LEN       = 0x22
_REG_MODEM_CFG3        = 0x26
_REG_SYNC_WORD         = 0x39
_REG_DIO_MAPPING1      = 0x40
_REG_VERSION           = 0x42

# OpMode bits
_MODE_SLEEP            = 0x00
_MODE_STANDBY          = 0x01
_MODE_TX               = 0x03
_MODE_RX_CONTINUOUS    = 0x05
_MODE_RX_SINGLE        = 0x06
_LORA_MODE_BIT         = 0x80

# IRQ flags
_IRQ_TX_DONE           = 0x08
_IRQ_RX_DONE           = 0x40
_IRQ_CRC_ERROR         = 0x20

# Bandwidth lookup (Hz → register value)
_BW_TABLE = {
    7_800:   0,
    10_400:  1,
    15_600:  2,
    20_800:  3,
    31_250:  4,
    41_700:  5,
    62_500:  6,
    125_000: 7,
    250_000: 8,
    500_000: 9,
}

_FXOSC = 32_000_000
_FSTEP = _FXOSC / (1 << 19)        # ~61.035 Hz


class SX127x:
    """Blocking LoRa driver for MicroPython on SX1276/77/78/79."""

    def __init__(self, spi, cs, rst, dio0,
                 frequency=915_000_000,
                 bandwidth=125_000,
                 sf=9,
                 cr=5,
                 tx_power=14,
                 preamble=8,
                 sync_word=0x12):
        self._spi  = spi
        self._cs   = cs
        self._rst  = rst
        self._dio0 = dio0

        self._freq      = frequency
        self._bw        = bandwidth
        self._sf        = sf
        self._cr        = cr
        self._tx_power  = tx_power
        self._preamble  = preamble
        self._sync_word = sync_word

        self._cs.init(Pin.OUT, value=1)
        self._rst.init(Pin.OUT, value=1)
        if self._dio0 is not None:
            self._dio0.init(Pin.IN)

        self._reset()
        self._configure()

    # ── Low-level SPI helpers ────────────────────────────────────────
    def _read_reg(self, addr):
        self._cs(0)
        self._spi.write(bytearray([addr & 0x7F]))
        result = self._spi.read(1)
        self._cs(1)
        return result[0]

    def _write_reg(self, addr, val):
        self._cs(0)
        self._spi.write(bytearray([addr | 0x80, val & 0xFF]))
        self._cs(1)

    def _read_fifo(self, length):
        self._cs(0)
        self._spi.write(bytearray([_REG_FIFO & 0x7F]))
        buf = self._spi.read(length)
        self._cs(1)
        return buf

    def _write_fifo(self, data):
        self._cs(0)
        self._spi.write(bytearray([_REG_FIFO | 0x80]) + data)
        self._cs(1)

    # ── Init sequence ────────────────────────────────────────────────
    def _reset(self):
        self._rst(0)
        sleep_ms(10)
        self._rst(1)
        sleep_ms(10)

    def _configure(self):
        # Verify chip presence
        ver = self._read_reg(_REG_VERSION)
        if ver != 0x12:
            raise RuntimeError("SX127x not found (version=0x{:02X})".format(ver))

        # Enter LoRa sleep
        self._set_mode(_MODE_SLEEP)
        self._write_reg(_REG_OP_MODE, _LORA_MODE_BIT | _MODE_SLEEP)
        sleep_ms(10)

        # Frequency
        frf = int(self._freq / _FSTEP)
        self._write_reg(_REG_FRF_MSB, (frf >> 16) & 0xFF)
        self._write_reg(_REG_FRF_MID, (frf >>  8) & 0xFF)
        self._write_reg(_REG_FRF_LSB,  frf        & 0xFF)

        # FIFO base addresses
        self._write_reg(_REG_FIFO_TX_BASE, 0x00)
        self._write_reg(_REG_FIFO_RX_BASE, 0x00)

        # LNA boost
        self._write_reg(_REG_LNA, self._read_reg(_REG_LNA) | 0x03)

        # Auto-AGC
        self._write_reg(_REG_MODEM_CFG3, 0x04)

        # Modem config 1  (BW | CR | implicit-header=0)
        bw_val = _BW_TABLE.get(self._bw, 7)
        cr_val = self._cr - 4                   # 4/5→1, 4/8→4
        self._write_reg(_REG_MODEM_CFG1, (bw_val << 4) | (cr_val << 1))

        # Modem config 2  (SF | CRC on)
        self._write_reg(_REG_MODEM_CFG2, (self._sf << 4) | 0x04)

        # Preamble length
        self._write_reg(_REG_PREAMBLE_MSB, (self._preamble >> 8) & 0xFF)
        self._write_reg(_REG_PREAMBLE_LSB,  self._preamble       & 0xFF)

        # Sync word
        self._write_reg(_REG_SYNC_WORD, self._sync_word)

        # PA config  (PA_BOOST pin, power)
        pa = 0x80 | max(0, min(self._tx_power - 2, 15))
        self._write_reg(_REG_PA_CONFIG, pa)

        # OCP trimming to 100 mA
        self._write_reg(_REG_OCP, 0x2B)

        # Standby
        self._set_mode(_MODE_STANDBY)

    # ── Mode helpers ─────────────────────────────────────────────────
    def _set_mode(self, mode):
        self._write_reg(_REG_OP_MODE, _LORA_MODE_BIT | mode)

    def standby(self):
        self._set_mode(_MODE_STANDBY)

    def sleep(self):
        self._set_mode(_MODE_SLEEP)

    # ── Transmit ─────────────────────────────────────────────────────
    def send(self, data, timeout_ms=5000):
        """Send *data* (bytes/bytearray/str).  Blocks until TX done or timeout."""
        if isinstance(data, str):
            data = data.encode()
        self._set_mode(_MODE_STANDBY)
        # Point FIFO to TX base
        self._write_reg(_REG_FIFO_ADDR_PTR, 0x00)
        self._write_fifo(data)
        self._write_reg(_REG_PAYLOAD_LEN, len(data))
        # Clear IRQ flags and start TX
        self._write_reg(_REG_IRQ_FLAGS, 0xFF)
        self._set_mode(_MODE_TX)
        # Wait for TX done
        start = ticks_ms()
        while True:
            flags = self._read_reg(_REG_IRQ_FLAGS)
            if flags & _IRQ_TX_DONE:
                self._write_reg(_REG_IRQ_FLAGS, 0xFF)
                break
            if ticks_diff(ticks_ms(), start) > timeout_ms:
                self._set_mode(_MODE_STANDBY)
                raise OSError("TX timeout")
        self._set_mode(_MODE_STANDBY)

    # ── Receive ──────────────────────────────────────────────────────
    def receive(self, timeout_ms=2000):
        """Block until a packet arrives or *timeout_ms* expires.
        Returns (bytes, rssi, snr) or None on timeout."""
        self._set_mode(_MODE_STANDBY)
        self._write_reg(_REG_FIFO_ADDR_PTR, 0x00)
        self._write_reg(_REG_IRQ_FLAGS, 0xFF)

        # Map DIO0 to RX-done
        self._write_reg(_REG_DIO_MAPPING1, 0x00)

        self._set_mode(_MODE_RX_SINGLE)
        start = ticks_ms()
        while True:
            flags = self._read_reg(_REG_IRQ_FLAGS)
            if flags & _IRQ_RX_DONE:
                # CRC check
                if flags & _IRQ_CRC_ERROR:
                    self._write_reg(_REG_IRQ_FLAGS, 0xFF)
                    return None
                # Read payload
                nb = self._read_reg(_REG_RX_NB_BYTES)
                self._write_reg(_REG_FIFO_ADDR_PTR,
                                self._read_reg(_REG_FIFO_RX_CURRENT))
                payload = self._read_fifo(nb)
                # RSSI / SNR
                snr = self._read_reg(_REG_PKT_SNR)
                if snr > 127:
                    snr -= 256
                snr /= 4.0
                rssi = self._read_reg(_REG_PKT_RSSI) - 157
                self._write_reg(_REG_IRQ_FLAGS, 0xFF)
                self._set_mode(_MODE_STANDBY)
                return (bytes(payload), rssi, snr)
            if ticks_diff(ticks_ms(), start) > timeout_ms:
                self._set_mode(_MODE_STANDBY)
                return None

    def receive_continuous(self):
        """Start RX-continuous mode.  Use poll_rx() to check for packets."""
        self._set_mode(_MODE_STANDBY)
        self._write_reg(_REG_FIFO_ADDR_PTR, 0x00)
        self._write_reg(_REG_IRQ_FLAGS, 0xFF)
        self._write_reg(_REG_DIO_MAPPING1, 0x00)
        self._set_mode(_MODE_RX_CONTINUOUS)

    def poll_rx(self):
        """Non-blocking check in RX-continuous mode.
        Returns (bytes, rssi, snr) if a packet is ready, else None."""
        flags = self._read_reg(_REG_IRQ_FLAGS)
        if not (flags & _IRQ_RX_DONE):
            return None
        if flags & _IRQ_CRC_ERROR:
            self._write_reg(_REG_IRQ_FLAGS, 0xFF)
            return None
        nb = self._read_reg(_REG_RX_NB_BYTES)
        self._write_reg(_REG_FIFO_ADDR_PTR,
                        self._read_reg(_REG_FIFO_RX_CURRENT))
        payload = self._read_fifo(nb)
        snr = self._read_reg(_REG_PKT_SNR)
        if snr > 127:
            snr -= 256
        snr /= 4.0
        rssi = self._read_reg(_REG_PKT_RSSI) - 157
        self._write_reg(_REG_IRQ_FLAGS, 0xFF)
        return (bytes(payload), rssi, snr)

    # ── Diagnostics ──────────────────────────────────────────────────
    def get_rssi(self):
        return self._read_reg(_REG_PKT_RSSI) - 157

    def get_snr(self):
        v = self._read_reg(_REG_PKT_SNR)
        if v > 127:
            v -= 256
        return v / 4.0
