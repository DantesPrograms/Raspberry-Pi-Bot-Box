#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"

mkdir -p "${BUILD_DIR}"

g++ -std=c++17 -O2 -Wall -Wextra -pedantic \
  "${REPO_DIR}/main.cc" \
  -o "${BUILD_DIR}/oled-dashboard"

echo "Built ${BUILD_DIR}/oled-dashboard"
