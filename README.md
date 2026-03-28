# Raspberry Pi Bot Box

This repo now has three pieces:

- `main.cc`: a Raspberry Pi-native C++ OLED dashboard for a 128x64 1.3" I2C display
- `deploy/`: install scripts and `systemd` units for the dashboard + Python bot
- `image/pi-gen/`: starter files for building a custom Raspberry Pi OS image

## What the OLED shows

The dashboard auto-rotates every 5 seconds between:
- system stats
- Python processes
- network info
- storage info
- bot status

The bot status page is intentionally customizable. Your Python bot can write short lines to:

`/opt/rpi-bot/state/dashboard.txt`

Example:

```text
BOT ONLINE
GUILDS: 12
LATENCY: 84MS
CMDS/HR: 531
LAST ERR: NONE
```

## Quick install on a Pi

On Raspberry Pi OS:

```bash
sudo bash deploy/install_pi_appliance.sh
```

That installs the OLED dashboard and starts it at boot.

When your bot code is ready:
1. put it under `/opt/rpi-bot/bot`
2. optionally create a venv in `/opt/rpi-bot/bot/.venv`
3. edit `/etc/default/rpi-discord-bot`
4. start the bot service:

```bash
sudo systemctl restart rpi-discord-bot.service
```
