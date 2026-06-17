#!/usr/bin/env python3
"""Forward secondary Pico USB altitude lines to the primary Pico.

Secondary output format:
    ALT,<alt_m>,<boot_ms>,<rssi>,<snr>

Primary accepts the same line on its USB console.
"""

from __future__ import annotations

import argparse
import json
import select
import socket
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:
    raise SystemExit("Install pyserial first: python3 -m pip install pyserial") from exc


def list_serial_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for port in ports:
        desc = port.description or "unknown"
        hwid = port.hwid or ""
        print(f"{port.device}\t{desc}\t{hwid}")


def open_port(path: str, baud: int) -> serial.Serial:
    return serial.Serial(path, baudrate=baud, timeout=0, write_timeout=0.25)


def decode_line(raw: bytes) -> str:
    return raw.decode("utf-8", errors="replace").strip()


def write_line(port: serial.Serial, text: str) -> None:
    port.write((text.rstrip("\r\n") + "\n").encode("ascii", errors="replace"))
    port.flush()


def mqtt_remaining_length(length: int) -> bytes:
    out = bytearray()
    while True:
        encoded = length % 128
        length //= 128
        if length:
            encoded |= 0x80
        out.append(encoded)
        if not length:
            return bytes(out)


def mqtt_utf8(text: str) -> bytes:
    data = text.encode("utf-8")
    return len(data).to_bytes(2, "big") + data


def mqtt_publish(host: str, port: int, topic: str, payload: str) -> None:
    client_id = f"usb-alt-bridge-{int(time.time())}"
    variable = mqtt_utf8("MQTT") + bytes([4, 2]) + (30).to_bytes(2, "big")
    packet = variable + mqtt_utf8(client_id)
    connect = bytes([0x10]) + mqtt_remaining_length(len(packet)) + packet

    topic_bytes = mqtt_utf8(topic)
    payload_bytes = payload.encode("utf-8")
    publish_packet = topic_bytes + payload_bytes
    publish = bytes([0x30]) + mqtt_remaining_length(len(publish_packet)) + publish_packet

    with socket.create_connection((host, port), timeout=0.5) as sock:
        sock.sendall(connect)
        ack = sock.recv(4)
        if len(ack) < 4 or ack[0] != 0x20 or ack[3] != 0:
            raise OSError(f"MQTT CONNACK failed: {ack!r}")
        sock.sendall(publish)
        sock.sendall(bytes([0xE0, 0x00]))


def parse_alt_line(text: str) -> tuple[float, int, float | None, float | None] | None:
    if not text.startswith("ALT,"):
        return None
    parts = text.split(",")
    if len(parts) < 2:
        return None
    alt_m = float(parts[1])
    boot_ms = int(float(parts[2])) if len(parts) > 2 and parts[2] else 0
    rssi = float(parts[3]) if len(parts) > 3 and parts[3] else None
    snr = float(parts[4]) if len(parts) > 4 and parts[4] else None
    return alt_m, boot_ms, rssi, snr


def publish_altitude_to_gui(host: str, port: int, text: str, lat: float, lon: float) -> None:
    parsed = parse_alt_line(text)
    if not parsed:
        return
    alt_m, boot_ms, rssi, snr = parsed
    payload = {
        "timestamp": int(time.time() * 1000),
        "boot_ms": boot_ms,
        "lat": lat,
        "lon": lon,
        "alt_m": alt_m,
        "alt_baro_m": alt_m,
        "alt_baro": alt_m,
        "vel_n": 0,
        "vel_e": 0,
        "vel_d": 0,
        "roll": 0,
        "pitch": 0,
        "yaw": 0,
        "state": "BARO_ONLY",
    }
    payload["rssi"] = rssi if rssi is not None else 0
    payload["snr"] = snr if snr is not None else 0
    mqtt_publish(host, port, "rocket/telemetry", json.dumps(payload, separators=(",", ":")))


def is_primary_noise(text: str) -> bool:
    noisy_prefixes = (
        "[imu] WHO_AM_I read failed",
        "[imu] init retry",
        "[mag] LIS3MDL init failed",
        "[mag] init retry",
    )
    return any(text.startswith(prefix) for prefix in noisy_prefixes)


def bridge(
    secondary: str,
    primary: str,
    baud: int,
    mqtt_host: str,
    mqtt_port: int,
    mqtt_enabled: bool,
    show_primary_noise: bool,
    lat: float,
    lon: float,
) -> int:
    with open_port(secondary, baud) as sec, open_port(primary, baud) as pri:
        print(f"Forwarding ALT lines: {secondary} -> {primary} @ {baud}")
        if mqtt_enabled:
            print(f"Publishing GUI telemetry to MQTT {mqtt_host}:{mqtt_port} topic rocket/telemetry")
        else:
            print("MQTT GUI publishing disabled.")
        print("Primary console is attached here too.")
        print("Type primary commands like: status, base 880 1000, arm, auto")
        print("Bridge commands: /quit")
        print("Press Ctrl-C to stop.")
        while True:
            readable, _, _ = select.select([sec, pri, sys.stdin], [], [], 0.1)

            if sec in readable:
                text = decode_line(sec.readline())
                if text:
                    print(f"secondary: {text}")
                    if text.startswith("ALT,"):
                        write_line(pri, text)
                        print(f"forwarded: {text}")
                        if mqtt_enabled:
                            try:
                                publish_altitude_to_gui(mqtt_host, mqtt_port, text, lat, lon)
                                print("gui: published rocket/telemetry")
                            except OSError as error:
                                print(f"gui: MQTT publish failed: {error}")

            if pri in readable:
                text = decode_line(pri.readline())
                if text:
                    if show_primary_noise or not is_primary_noise(text):
                        print(f"primary: {text}")

            if sys.stdin in readable:
                line = sys.stdin.readline()
                if line == "":
                    continue
                text = line.strip()
                if text == "/quit":
                    return 0
                if text:
                    write_line(pri, text)
                    if text.startswith("ALT,") and mqtt_enabled:
                        try:
                            publish_altitude_to_gui(mqtt_host, mqtt_port, text, lat, lon)
                            print("gui: published rocket/telemetry")
                        except OSError as error:
                            print(f"gui: MQTT publish failed: {error}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true", help="list serial ports and exit")
    parser.add_argument("--secondary", help="secondary Pico serial port")
    parser.add_argument("--primary", help="primary Pico serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mqtt-host", default="localhost")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--no-mqtt", action="store_true", help="do not publish relayed altitude to GUI MQTT")
    parser.add_argument("--show-primary-noise", action="store_true", help="show repeated primary IMU init retry logs")
    parser.add_argument("--lat", type=float, default=0.0, help="GUI placeholder latitude for baro-only telemetry")
    parser.add_argument("--lon", type=float, default=0.0, help="GUI placeholder longitude for baro-only telemetry")
    args = parser.parse_args(argv)

    if args.list:
        list_serial_ports()
        return 0

    if not args.secondary or not args.primary:
        parser.error("--secondary and --primary are required unless --list is used")

    return bridge(
        args.secondary,
        args.primary,
        args.baud,
        args.mqtt_host,
        args.mqtt_port,
        not args.no_mqtt,
        args.show_primary_noise,
        args.lat,
        args.lon,
    )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
