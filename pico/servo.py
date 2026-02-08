# servo.py  --  PWM servo control for RP2350 / Pico 2W  (MicroPython)

from machine import Pin, PWM
import config


class Servo:
    """Control a hobby servo (0-180 deg) on the given GPIO pin."""

    def __init__(self, pin_num,
                 freq=config.SERVO_FREQ,
                 min_us=config.SERVO_MIN_US,
                 max_us=config.SERVO_MAX_US):
        self._pwm = PWM(Pin(pin_num))
        self._pwm.freq(freq)
        self._min_us = min_us
        self._max_us = max_us
        self._period_us = 1_000_000 // freq
        self._angle = 90.0
        self.set_angle(90.0)            # centre on init

    def set_angle(self, angle):
        """Set servo position in degrees (0..180)."""
        angle = max(0.0, min(180.0, float(angle)))
        self._angle = angle
        pulse_us = self._min_us + (angle / 180.0) * (self._max_us - self._min_us)
        duty = int(pulse_us * 65535 // self._period_us)
        self._pwm.duty_u16(duty)

    @property
    def angle(self):
        return self._angle

    def deinit(self):
        self._pwm.duty_u16(0)
        self._pwm.deinit()
