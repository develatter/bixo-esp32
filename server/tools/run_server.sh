#!/usr/bin/env bash
# Start the Bixo server; the brain is the bixo profile of Hermes. PET_BRAIN=claude calls Claude
# directly instead.
#
#   tools/run_server.sh [PUBLIC_HOST]
#
# Settings come from the environment or server/.env (see .env.example). PET_SECRET is required;
# without PET_HERMES_KEY it reads the bixo Hermes profile's own key, and without PET_PUBLIC_HOST
# it uses this machine's Tailscale name.
#
# Expose it with Tailscale Funnel: tailscale funnel --bg 8003
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -f .env ]; then
  set -a
  . ./.env
  set +a
fi

[ -n "${PET_SECRET:-}" ] || { echo "PET_SECRET is not set (see server/.env.example)" >&2; exit 1; }
export PET_SECRET
if [ "${PET_BRAIN:-hermes}" = claude ]; then
  [ -n "${ANTHROPIC_API_KEY:-}" ] || { echo "ANTHROPIC_API_KEY is not set" >&2; exit 1; }
  export ANTHROPIC_API_KEY
else
  if [ -z "${PET_HERMES_KEY:-}" ]; then
    PET_HERMES_KEY="$(sed -n 's/^API_SERVER_KEY=//p' "${HERMES_HOME:-$HOME/.hermes}/profiles/bixo/.env")"
  fi
  [ -n "$PET_HERMES_KEY" ] || { echo "No PET_HERMES_KEY, nor API_SERVER_KEY in the bixo Hermes profile" >&2; exit 1; }
  export PET_HERMES_KEY
fi
PUBLIC_HOST="${1:-${PET_PUBLIC_HOST:-$(tailscale status --json | jq -r '.Self.DNSName | rtrimstr(".")')}}"
export PET_PUBLIC_URL="wss://$PUBLIC_HOST"
exec uv run python -m pet
