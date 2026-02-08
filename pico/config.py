# config.py  --  Shared configuration for transmitter & receiver boards
# Target: RP2350 / Pico 2W  (MicroPython)

# ── Pin assignments ──────────────────────────────────────────────────
# Servos (receiver only)
PIN_SERVO_AZ  = 0          # GP0  – azimuth  (pan)
PIN_SERVO_EL  = 1          # GP1  – elevation (tilt)

# LoRa SX127x  (SPI0)
PIN_LORA_SCK  = 2          # GP2
PIN_LORA_MOSI = 3          # GP3
PIN_LORA_MISO = 4          # GP4
PIN_LORA_CS   = 5          # GP5
PIN_LORA_RST  = 6          # GP6
PIN_LORA_DIO0 = 7          # GP7  – RX-done / TX-done interrupt

# uBlox GPS  (UART1)
PIN_GPS_TX    = 8           # GP8  (Pico TX → GPS RX)
PIN_GPS_RX    = 9           # GP9  (Pico RX ← GPS TX)
GPS_UART_ID   = 1
GPS_BAUDRATE  = 9600

# ── LoRa radio parameters (match bareman_tracker defaults) ───────────
LORA_FREQUENCY   = 915_000_000      # 915 MHz ISM band
LORA_BANDWIDTH   = 125_000          # 125 kHz
LORA_SF          = 9                # spreading factor
LORA_CR          = 5                # coding rate 4/5
LORA_TX_POWER    = 14               # dBm
LORA_PREAMBLE    = 8
LORA_SYNC_WORD   = 0x12

# ── Servo parameters ────────────────────────────────────────────────
SERVO_FREQ       = 50               # Hz  (20 ms period)
SERVO_MIN_US     = 500              # pulse for 0 deg
SERVO_MAX_US     = 2500             # pulse for 180 deg

# ── Tracking ─────────────────────────────────────────────────────────
# Compass heading the receiver antenna is physically pointing at rest.
# 0 = north, 90 = east, etc.  Adjust after installation.
HEADING_OFFSET   = 0.0

# How often the transmitter sends a GPS fix (seconds)
TX_INTERVAL_S    = 1.0

# How often the receiver updates the gimbal (seconds)
RX_UPDATE_S      = 0.25
