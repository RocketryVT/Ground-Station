# gps.py  --  Lightweight uBlox / generic NMEA parser for MicroPython
# Parses $GxRMC and $GxGGA sentences over UART.

from machine import UART, Pin


class GPSFix:
    """Container for the most recent GPS fix."""
    __slots__ = ("latitude", "longitude", "altitude",
                 "speed_knots", "course", "satellites",
                 "fix_quality", "valid")

    def __init__(self):
        self.latitude    = 0.0
        self.longitude   = 0.0
        self.altitude    = 0.0      # metres (from GGA)
        self.speed_knots = 0.0
        self.course      = 0.0
        self.satellites  = 0
        self.fix_quality = 0        # 0=invalid, 1=GPS, 2=DGPS …
        self.valid       = False

    def __repr__(self):
        return ("GPSFix(lat={:.6f}, lon={:.6f}, alt={:.1f}, "
                "sats={}, valid={})".format(
                    self.latitude, self.longitude,
                    self.altitude, self.satellites, self.valid))


class GPS:
    """Non-blocking NMEA reader.  Call update() frequently."""

    def __init__(self, uart_id, tx_pin, rx_pin, baudrate=9600):
        self._uart = UART(uart_id, baudrate=baudrate,
                          tx=Pin(tx_pin), rx=Pin(rx_pin),
                          bits=8, parity=None, stop=1)
        self._buf = ""
        self.fix = GPSFix()

    # ── Public API ───────────────────────────────────────────────────
    def update(self):
        """Read all available bytes, parse complete sentences.
        Returns True if a new valid RMC or GGA was processed."""
        updated = False
        while self._uart.any():
            ch = self._uart.read(1)
            if ch is None:
                break
            ch = chr(ch[0])
            if ch == '\n':
                line = self._buf.strip()
                self._buf = ""
                if line.startswith('$'):
                    if self._parse(line):
                        updated = True
            elif ch != '\r':
                self._buf += ch
                # Guard against garbage flooding
                if len(self._buf) > 120:
                    self._buf = ""
        return updated

    # ── NMEA parsing ─────────────────────────────────────────────────
    def _parse(self, sentence):
        """Dispatch to the right handler.  Returns True on success."""
        # Strip checksum for easier splitting
        if '*' in sentence:
            sentence = sentence[:sentence.index('*')]
        parts = sentence.split(',')
        if len(parts) < 2:
            return False
        talker = parts[0]          # e.g. $GPRMC, $GNRMC, $GPGGA …
        msg_id = talker[-3:]       # RMC, GGA, …
        if msg_id == "RMC":
            return self._parse_rmc(parts)
        if msg_id == "GGA":
            return self._parse_gga(parts)
        return False

    def _parse_rmc(self, p):
        # $GxRMC,time,status,lat,N/S,lon,E/W,speed,course,date,...
        if len(p) < 10:
            return False
        status = p[2]
        if status != 'A':
            self.fix.valid = False
            return False
        lat = self._nmea_to_dd(p[3], p[4])
        lon = self._nmea_to_dd(p[5], p[6])
        if lat is None or lon is None:
            return False
        self.fix.latitude  = lat
        self.fix.longitude = lon
        self.fix.speed_knots = self._to_float(p[7])
        self.fix.course      = self._to_float(p[8])
        self.fix.valid       = True
        return True

    def _parse_gga(self, p):
        # $GxGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,...
        if len(p) < 11:
            return False
        quality = self._to_int(p[6])
        if quality == 0:
            return False
        lat = self._nmea_to_dd(p[2], p[3])
        lon = self._nmea_to_dd(p[4], p[5])
        if lat is None or lon is None:
            return False
        self.fix.latitude    = lat
        self.fix.longitude   = lon
        self.fix.fix_quality = quality
        self.fix.satellites  = self._to_int(p[7])
        self.fix.altitude    = self._to_float(p[9])
        self.fix.valid       = True
        return True

    # ── Helpers ──────────────────────────────────────────────────────
    @staticmethod
    def _nmea_to_dd(raw, hemi):
        """Convert NMEA ddmm.mmmm / dddmm.mmmm to decimal degrees."""
        if not raw:
            return None
        try:
            dot = raw.index('.')
            deg = int(raw[:dot - 2])
            minutes = float(raw[dot - 2:])
            dd = deg + minutes / 60.0
            if hemi in ('S', 'W'):
                dd = -dd
            return dd
        except (ValueError, IndexError):
            return None

    @staticmethod
    def _to_float(s):
        try:
            return float(s)
        except (ValueError, TypeError):
            return 0.0

    @staticmethod
    def _to_int(s):
        try:
            return int(s)
        except (ValueError, TypeError):
            return 0
