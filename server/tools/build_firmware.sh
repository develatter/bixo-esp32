#!/usr/bin/env bash
# Build the firmware for the SenseCAP Watcher, pointing at this server.
#
#   tools/build_firmware.sh [--check] [XIAOZHI_DIR] [PUBLIC_HOST]
#
# XIAOZHI_DIR defaults to this repository's root. PET_SECRET and PET_PUBLIC_HOST come from the
# environment or server/.env (see .env.example); without PET_PUBLIC_HOST it uses this machine's
# Tailscale name.
#
# --check only checks that it compiles, without the secret: the OTA URL gets a placeholder and the
# image is renamed to build/xiaozhi.check.bin so it can't be flashed by mistake.
#
# The OTA URL carries the server secret, handed to the container through the environment (never written to a tracked file). It does end up in
# build/sdkconfig and in the firmware image, which is unavoidable since the device has to know it.
set -euo pipefail

CHECK=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK=1
  shift
fi

SERVER_DIR="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "$SERVER_DIR/.env" ]; then
  set -a
  . "$SERVER_DIR/.env"
  set +a
fi
XIAOZHI_DIR="${1:-$(dirname "$SERVER_DIR")}"
PUBLIC_HOST="${2:-${PET_PUBLIC_HOST:-$(tailscale status --json | jq -r '.Self.DNSName | rtrimstr(".")')}}"

cd "$XIAOZHI_DIR"
if (( CHECK )); then
  SECRET="compile-check"
elif [ -n "${PET_SECRET:-}" ]; then
  SECRET="$PET_SECRET"
else
  echo "PET_SECRET is not set (see server/.env.example)" >&2
  exit 1
fi
unset PET_SECRET
export OTA_URL="https://$PUBLIC_HOST/xiaozhi/ota/$SECRET/"
unset SECRET

docker run --rm --platform linux/arm64 -e OTA_URL -v "$PWD:/project" -w /project espressif/idf:release-v6.1 bash -c \
  '. $IDF_PATH/export.sh >/dev/null && git config --global --add safe.directory /project && python3 scripts/build.py --language es-ES'

if (( CHECK )); then
  mv build/xiaozhi.bin build/xiaozhi.check.bin
  echo "Compile check OK (build/xiaozhi.check.bin is NOT flashable: placeholder OTA URL)"
fi
