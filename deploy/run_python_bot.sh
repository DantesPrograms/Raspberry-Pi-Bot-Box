#!/usr/bin/env bash
set -euo pipefail

BOT_WORKDIR="${BOT_WORKDIR:-/opt/rpi-bot/bot}"
BOT_ENTRYPOINT="${BOT_ENTRYPOINT:-${BOT_WORKDIR}/bot.py}"
BOT_VENV="${BOT_VENV:-${BOT_WORKDIR}/.venv}"
BOT_PYTHON="${BOT_PYTHON:-}"

if [[ -z "${BOT_PYTHON}" ]]; then
  if [[ -x "${BOT_VENV}/bin/python" ]]; then
    BOT_PYTHON="${BOT_VENV}/bin/python"
  else
    BOT_PYTHON="/usr/bin/python3"
  fi
fi

if [[ ! -f "${BOT_ENTRYPOINT}" ]]; then
  echo "Bot entrypoint not found: ${BOT_ENTRYPOINT}" >&2
  exit 1
fi

cd "${BOT_WORKDIR}"
exec "${BOT_PYTHON}" "${BOT_ENTRYPOINT}"
