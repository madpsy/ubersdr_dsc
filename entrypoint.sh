#!/bin/sh
# entrypoint.sh — translate environment variables into dsc_rx_from_ubersdr flags
#
# Environment variables:
#   UBERSDR_URL    UberSDR base URL (default: http://ubersdr:8080)
#   DSC_FREQS      DSC frequencies in Hz, comma-separated
#                  (default: all 25 ITU DSC frequencies if not set)
#   WEB_PORT       Port for the web UI server (default: 6093)

set -e

URL="${UBERSDR_URL:-http://ubersdr:8080}"
PORT="${WEB_PORT:-6093}"

# Build argument list
set -- --sdr-url "$URL" --web-port "$PORT"

# Only pass --freqs if DSC_FREQS is explicitly set and non-empty;
# otherwise the binary uses its built-in default of all 25 DSC frequencies.
if [ -n "${DSC_FREQS:-}" ]; then
    set -- "$@" --freqs "$DSC_FREQS"
fi

exec /usr/local/bin/dsc_rx_from_ubersdr "$@"
