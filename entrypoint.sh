#!/bin/sh
# entrypoint.sh — translate environment variables into dsc_rx_from_ubersdr flags
#
# Environment variables:
#   UBERSDR_URL    UberSDR base URL (default: http://ubersdr:8080)
#   DSC_FREQS      DSC frequencies in Hz, comma-separated (default: 2187500)
#   WEB_PORT       Port for the web UI server (default: 6093)

set -e

URL="${UBERSDR_URL:-http://ubersdr:8080}"
FREQS="${DSC_FREQS:-2187500}"
PORT="${WEB_PORT:-6093}"

exec /usr/local/bin/dsc_rx_from_ubersdr \
    --sdr-url "$URL" \
    --freqs "$FREQS" \
    --web-port "$PORT" \
    "$@"
