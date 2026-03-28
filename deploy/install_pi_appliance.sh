#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this script as root on the Raspberry Pi." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

apt-get update
apt-get install -y build-essential i2c-tools python3 python3-venv python3-pip

if ! id -u rpi-bot >/dev/null 2>&1; then
  useradd --system --home /opt/rpi-bot --create-home --shell /usr/sbin/nologin rpi-bot
fi

usermod -aG i2c rpi-bot

install -d -m 0755 /opt/rpi-bot/bin
install -d -m 0755 /opt/rpi-bot/bot
install -d -m 0755 /opt/rpi-bot/state
install -d -m 0755 /opt/rpi-bot/src

g++ -std=c++17 -O2 -Wall -Wextra -pedantic \
  "${REPO_DIR}/main.cc" \
  -o /opt/rpi-bot/bin/oled-dashboard

install -m 0755 "${SCRIPT_DIR}/run_python_bot.sh" /opt/rpi-bot/bin/run_python_bot.sh
install -m 0644 "${SCRIPT_DIR}/oled-dashboard.service" /etc/systemd/system/oled-dashboard.service
install -m 0644 "${SCRIPT_DIR}/rpi-discord-bot.service" /etc/systemd/system/rpi-discord-bot.service

if [[ ! -f /etc/default/oled-dashboard ]]; then
  install -m 0644 "${SCRIPT_DIR}/oled-dashboard.env.example" /etc/default/oled-dashboard
fi

if [[ ! -f /etc/default/rpi-discord-bot ]]; then
  install -m 0644 "${SCRIPT_DIR}/rpi-discord-bot.env.example" /etc/default/rpi-discord-bot
fi

if [[ ! -f /opt/rpi-bot/state/dashboard.txt ]]; then
  install -m 0644 "${SCRIPT_DIR}/dashboard-status.sample" /opt/rpi-bot/state/dashboard.txt
fi

cp "${REPO_DIR}/main.cc" /opt/rpi-bot/src/main.cc
chown -R rpi-bot:rpi-bot /opt/rpi-bot

if command -v raspi-config >/dev/null 2>&1; then
  raspi-config nonint do_i2c 0 || true
fi

systemctl daemon-reload
systemctl enable oled-dashboard.service
systemctl enable rpi-discord-bot.service
systemctl restart oled-dashboard.service

echo
echo "Dashboard installed and started."
echo "Put your Python bot in /opt/rpi-bot/bot and then run:"
echo "  sudo systemctl restart rpi-discord-bot.service"
