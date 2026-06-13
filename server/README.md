# Rocketry MQTT Server

This folder contains the first-pass server bootstrap for the launch telemetry
MQTT broker.

It installs Mosquitto and starts a `tmux` dashboard with:

- client connection/disconnection events from the broker log
- live application topic traffic from `#`
- broker `$SYS` client/message counters

## Quick Start

On an Ubuntu/Debian server:

```bash
cd /path/to/Avionics/projects/ground_station/server
ROCKETRY_MQTT_USER=rocketry \
ROCKETRY_MQTT_PASSWORD='replace-with-a-real-password' \
./setup_mqtt_server.sh
```

Reconnect to the dashboard later:

```bash
tmux attach -t rocketry-mqtt
```

## Ports

Forward TCP `1883` to the server if clients need to connect from outside the
LAN. If `ufw` is active, the script opens `1883/tcp` automatically.

## LAN-Only Bring-Up

The current Pico MQTT firmware does not set a username/password yet. For local
LAN testing only:

```bash
ROCKETRY_MQTT_ALLOW_ANONYMOUS=true ./setup_mqtt_server.sh
```

Do not expose an anonymous broker to the public internet.

## Client Settings

Ground station app:

```bash
ROCKETRY_GS_MQTT_HOST=<server-ip-or-domain>
ROCKETRY_GS_MQTT_PORT=1883
ROCKETRY_GS_MQTT_USER=rocketry
ROCKETRY_GS_MQTT_PASSWORD=<same-password-used-on-server>
```

Pico firmware currently uses:

```c
#define MQTT_BROKER_HOST "..."
#define MQTT_BROKER_PORT 1883
#define MQTT_CLIENT_ID   "gs_pico"
```

For the internet-facing version, add username/password support to the Pico and
iOS clients before disabling `ROCKETRY_MQTT_ALLOW_ANONYMOUS`.

## Logging

Broker/client lifecycle log:

```text
/var/log/mosquitto/rocketry.log
```

Raw topic traffic captured by the tmux dashboard:

```text
/var/log/rocketry-mqtt/messages.log
```

This is not a complete history API yet. It is enough for bring-up and debugging.
For durable replay to late-joining ground stations, the next step is a small
collector service that subscribes to MQTT, stores messages in SQLite/Postgres,
and exposes a pull/sync endpoint.
