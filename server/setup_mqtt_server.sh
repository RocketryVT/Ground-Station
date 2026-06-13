#!/usr/bin/env bash
set -euo pipefail

# Rocketry At VT MQTT server bootstrap.
#
# Usage:
#   ./setup_mqtt_server.sh
#
# Optional environment:
#   ROCKETRY_MQTT_PORT=1883
#   ROCKETRY_MQTT_USER=rocketry
#   ROCKETRY_MQTT_PASSWORD='replace-me'
#   ROCKETRY_MQTT_ALLOW_ANONYMOUS=false
#   ROCKETRY_MQTT_SESSION=rocketry-mqtt
#   ROCKETRY_MQTT_NO_ATTACH=1
#
# For a public, port-forwarded server, keep anonymous access disabled.
# For first LAN tests with current Pico firmware, use:
#   ROCKETRY_MQTT_ALLOW_ANONYMOUS=true ./setup_mqtt_server.sh

MQTT_PORT="${ROCKETRY_MQTT_PORT:-1883}"
MQTT_USER="${ROCKETRY_MQTT_USER:-rocketry}"
MQTT_PASSWORD="${ROCKETRY_MQTT_PASSWORD:-}"
ALLOW_ANONYMOUS="${ROCKETRY_MQTT_ALLOW_ANONYMOUS:-false}"
TMUX_SESSION="${ROCKETRY_MQTT_SESSION:-rocketry-mqtt}"

MOSQUITTO_CONF="/etc/mosquitto/conf.d/rocketry.conf"
MOSQUITTO_PASSWD="/etc/mosquitto/passwd"
ROCKETRY_DIR="/var/lib/rocketry-mqtt"
MOSQUITTO_LOG="/var/log/mosquitto/rocketry.log"
MESSAGE_LOG="/var/log/rocketry-mqtt/messages.log"
MONITOR_ENV="$HOME/.rocketry-mqtt-monitor.env"

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

sudo_write() {
  local path="$1"
  sudo tee "$path" >/dev/null
}

normalize_bool() {
  case "${1,,}" in
    1|yes|true|on) echo "true" ;;
    0|no|false|off) echo "false" ;;
    *)
      echo "Expected boolean value, got '$1'" >&2
      exit 1
      ;;
  esac
}

shell_quote() {
  printf "%q" "$1"
}

ensure_password() {
  if [[ "$(normalize_bool "$ALLOW_ANONYMOUS")" == "true" ]]; then
    return
  fi

  if [[ -n "$MQTT_PASSWORD" ]]; then
    return
  fi

  if [[ -t 0 ]]; then
    echo "MQTT authentication is enabled."
    read -r -s -p "Password for MQTT user '$MQTT_USER': " MQTT_PASSWORD
    echo
    if [[ -z "$MQTT_PASSWORD" ]]; then
      echo "Password cannot be empty when anonymous access is disabled." >&2
      exit 1
    fi
  else
    echo "Set ROCKETRY_MQTT_PASSWORD, or set ROCKETRY_MQTT_ALLOW_ANONYMOUS=true for LAN-only testing." >&2
    exit 1
  fi
}

install_packages() {
  if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y mosquitto mosquitto-clients tmux
  else
    echo "This bootstrap currently supports Debian/Ubuntu servers with apt-get." >&2
    echo "Install mosquitto, mosquitto-clients, and tmux manually, then rerun." >&2
    exit 1
  fi
}

write_mosquitto_config() {
  local anonymous
  anonymous="$(normalize_bool "$ALLOW_ANONYMOUS")"

  sudo mkdir -p /var/log/mosquitto /var/log/rocketry-mqtt "$ROCKETRY_DIR"
  sudo touch "$MOSQUITTO_LOG" "$MESSAGE_LOG"
  sudo chown mosquitto:mosquitto /var/log/mosquitto "$MOSQUITTO_LOG" /var/log/rocketry-mqtt "$MESSAGE_LOG" "$ROCKETRY_DIR"
  sudo chmod 0644 "$MOSQUITTO_LOG" "$MESSAGE_LOG"

  if [[ "$anonymous" == "false" ]]; then
    sudo mosquitto_passwd -b -c "$MOSQUITTO_PASSWD" "$MQTT_USER" "$MQTT_PASSWORD"
    sudo chown mosquitto:mosquitto "$MOSQUITTO_PASSWD"
    sudo chmod 0640 "$MOSQUITTO_PASSWD"
  fi

  {
    cat <<EOF
# Managed by Rocketry At VT setup_mqtt_server.sh
per_listener_settings true

listener ${MQTT_PORT} 0.0.0.0
protocol mqtt
allow_anonymous ${anonymous}
EOF

    if [[ "$anonymous" == "false" ]]; then
      cat <<EOF
password_file ${MOSQUITTO_PASSWD}
EOF
    fi

    cat <<EOF

persistence true
persistence_location /var/lib/mosquitto/
autosave_interval 30

# Keep these on for the tmux connection monitor.
connection_messages true
log_timestamp true
log_type all
log_dest file ${MOSQUITTO_LOG}

max_keepalive 120
message_size_limit 1048576
sys_interval 10
EOF
  } | sudo_write "$MOSQUITTO_CONF"
}

write_monitor_env() {
  local anonymous
  anonymous="$(normalize_bool "$ALLOW_ANONYMOUS")"

  {
    echo "MQTT_HOST=127.0.0.1"
    echo "MQTT_PORT=$(shell_quote "$MQTT_PORT")"
    echo "MQTT_ALLOW_ANONYMOUS=$(shell_quote "$anonymous")"
    echo "MQTT_USER=$(shell_quote "$MQTT_USER")"
    echo "MQTT_PASSWORD=$(shell_quote "$MQTT_PASSWORD")"
    echo "MOSQUITTO_LOG=$(shell_quote "$MOSQUITTO_LOG")"
    echo "MESSAGE_LOG=$(shell_quote "$MESSAGE_LOG")"
  } > "$MONITOR_ENV"
  chmod 0600 "$MONITOR_ENV"
}

restart_broker() {
  sudo systemctl enable mosquitto
  sudo systemctl restart mosquitto
  sleep 1
  sudo systemctl --no-pager --full status mosquitto >/dev/null
}

open_firewall_if_present() {
  if command -v ufw >/dev/null 2>&1 && sudo ufw status | grep -qi '^Status: active'; then
    sudo ufw allow "${MQTT_PORT}/tcp" comment "Rocketry MQTT"
  fi
}

tmux_cmd_prefix='source "$HOME/.rocketry-mqtt-monitor.env";
auth_args=();
if [[ "${MQTT_ALLOW_ANONYMOUS}" != "true" ]]; then
  auth_args=(-u "${MQTT_USER}" -P "${MQTT_PASSWORD}");
fi'

start_dashboard() {
  need_cmd tmux
  need_cmd mosquitto_sub

  if tmux has-session -t "$TMUX_SESSION" 2>/dev/null; then
    tmux kill-session -t "$TMUX_SESSION"
  fi

  tmux new-session -d -s "$TMUX_SESSION" -n connections \
    "bash -lc '$tmux_cmd_prefix
      echo \"Watching MQTT client connect/disconnect events from ${MOSQUITTO_LOG}\";
      tail -n 80 -F \"\${MOSQUITTO_LOG}\" | awk '\''/New connection|New client connected|Client .* disconnected|Socket error|not authorised|bad username|closed its connection/ { print strftime(\"%F %T\"), \$0; fflush(); }'\'''"

  tmux split-window -h -t "$TMUX_SESSION:connections" \
    "bash -lc '$tmux_cmd_prefix
      echo \"Subscribing to all application topics on \${MQTT_HOST}:\${MQTT_PORT}\";
      echo \"Appending traffic to \${MESSAGE_LOG}\";
      mosquitto_sub -h \"\${MQTT_HOST}\" -p \"\${MQTT_PORT}\" \"\${auth_args[@]}\" -v -t \"#\" \
        | awk '\''{ print strftime(\"%F %T\"), \$0; fflush(); }'\'' \
        | tee -a \"\${MESSAGE_LOG}\"'"

  tmux split-window -v -t "$TMUX_SESSION:connections.0" \
    "bash -lc '$tmux_cmd_prefix
      echo \"Broker \$SYS client counters\";
      mosquitto_sub -h \"\${MQTT_HOST}\" -p \"\${MQTT_PORT}\" \"\${auth_args[@]}\" -v \
        -t \"\$SYS/broker/clients/#\" -t \"\$SYS/broker/messages/#\" -t \"\$SYS/broker/load/messages/#\" \
        | awk '\''{ print strftime(\"%F %T\"), \$0; fflush(); }'\'''"

  tmux select-layout -t "$TMUX_SESSION:connections" tiled >/dev/null

  if [[ "${ROCKETRY_MQTT_NO_ATTACH:-}" != "1" ]]; then
    tmux attach -t "$TMUX_SESSION"
  else
    echo "tmux dashboard started: tmux attach -t $TMUX_SESSION"
  fi
}

print_summary() {
  local anonymous
  local auth_label
  local sub_example
  local pub_example
  anonymous="$(normalize_bool "$ALLOW_ANONYMOUS")"

  if [[ "$anonymous" == "true" ]]; then
    auth_label="anonymous"
    sub_example="mosquitto_sub -h <server> -p ${MQTT_PORT} -v -t '#'"
    pub_example="mosquitto_pub -h <server> -p ${MQTT_PORT} -t gs/demo -m '{\"ok\":true}'"
  else
    auth_label="username/password"
    sub_example="mosquitto_sub -h <server> -p ${MQTT_PORT} -u ${MQTT_USER} -P '<password>' -v -t '#'"
    pub_example="mosquitto_pub -h <server> -p ${MQTT_PORT} -u ${MQTT_USER} -P '<password>' -t gs/demo -m '{\"ok\":true}'"
  fi

  cat <<EOF

Rocketry MQTT server is running.

Broker:
  host: this server's LAN/public IP or DNS name
  port: ${MQTT_PORT}
  auth: ${auth_label}

Dashboard:
  tmux attach -t ${TMUX_SESSION}

Logs:
  client events: ${MOSQUITTO_LOG}
  topic traffic: ${MESSAGE_LOG}

Client examples:
  Subscribe:
    ${sub_example}
  Publish test:
    ${pub_example}

EOF

  if [[ "$anonymous" == "true" ]]; then
    cat <<EOF
Anonymous access is enabled. Use this only for isolated LAN testing.
Do not port-forward this broker to the internet with anonymous access enabled.

EOF
  fi
}

main() {
  ensure_password
  install_packages
  write_mosquitto_config
  write_monitor_env
  restart_broker
  open_firewall_if_present
  print_summary
  start_dashboard
}

main "$@"
