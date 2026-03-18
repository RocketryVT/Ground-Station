# Telemetry Ground Station Visualization

You need UV and mosquitto installed.

## Install UV

```bash
pip install uv
```

## Install mosquitto

Follow instructions at <https://mosquitto.org/download/>
On Ubuntu, you can install via apt:

```bash
sudo apt update
sudo apt install mosquitto mosquitto-clients
```

## Run mosquitto broker

```bash
mosquitto
```

## Run telemetry visualization server

Inside the `projects/ground_station/telemetry` directory, run:

```bash
source logger/.venv/bin/activate
python launch.py
```

You can press "o" to open the dashboard in your web browser.
