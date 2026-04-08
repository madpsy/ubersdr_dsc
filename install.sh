#!/usr/bin/env bash
# install.sh — fetch the docker-compose.yml from the ubersdr_dsc repo and start the service
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/madpsy/ubersdr_dsc/refs/heads/main/install.sh | bash
#   — or —
#   ./install.sh [--force-update]
#
# Options:
#   --force-update   Overwrite an existing docker-compose.yml (default: skip if present)
#
# When piping through bash, pass the flag via env var instead:
#   curl -fsSL ... | FORCE_UPDATE=1 bash

set -euo pipefail

REPO_RAW="https://raw.githubusercontent.com/madpsy/ubersdr_dsc/main"
INSTALL_DIR="${HOME}/ubersdr/dsc"
COMPOSE_FILE="docker-compose.yml"
FORCE_UPDATE="${FORCE_UPDATE:-0}"

# Parse flags when run directly (not piped)
for arg in "$@"; do
    case "$arg" in
        --force-update) FORCE_UPDATE=1 ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Dependency checks
# ---------------------------------------------------------------------------

command -v docker >/dev/null || die "docker not found in PATH — please install Docker first"
docker compose version >/dev/null 2>&1 || die "docker compose plugin not found — please install Docker Compose v2"

# ---------------------------------------------------------------------------
# Prepare install directory
# ---------------------------------------------------------------------------

mkdir -p "${INSTALL_DIR}"
cd "${INSTALL_DIR}"

# ---------------------------------------------------------------------------
# Fetch compose file
# ---------------------------------------------------------------------------

if [[ -f "${COMPOSE_FILE}" && "${FORCE_UPDATE}" != "1" ]]; then
    echo "${COMPOSE_FILE} already exists — skipping download (use --force-update to overwrite)"
else
    echo "Fetching ${COMPOSE_FILE} from GitHub..."
    curl -fsSL "${REPO_RAW}/${COMPOSE_FILE}" -o "${COMPOSE_FILE}"
    echo "Saved ${COMPOSE_FILE}"
fi

# ---------------------------------------------------------------------------
# Fetch helper scripts
# ---------------------------------------------------------------------------

for script in update.sh start.sh stop.sh restart.sh; do
    echo "Fetching ${script}..."
    curl -fsSL "${REPO_RAW}/${script}" -o "${script}"
    chmod +x "${script}"
    echo "Saved ${script}"
done

# ---------------------------------------------------------------------------
# Pull image and start service
# ---------------------------------------------------------------------------

echo "Pulling latest Docker image..."
docker compose pull

echo "Starting ubersdr_dsc..."
docker compose up -d --remove-orphans --force-recreate

echo ""
echo "Done. ubersdr_dsc is running."
echo "  View logs  : docker compose logs -f  (or ./update.sh)"
echo "  Stop       : ./stop.sh"
echo "  Start      : ./start.sh"
echo "  Restart    : ./restart.sh"
echo "  Update     : ./update.sh"
echo ""
echo "Edit ${INSTALL_DIR}/${COMPOSE_FILE} to configure UBERSDR_URL, DSC_FREQS, etc."
echo "Then run ./restart.sh to apply changes."
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  UberSDR Proxy Configuration"
echo "  This addon can be added as an UberSDR Proxy via the Admin interface with:"
echo ""
echo "    Name              : dsc"
echo "    Host              : dsc"
echo "    Port              : 6093"
echo "    Enabled           : true"
echo "    Strip Prefix      : true"
echo "    Rewrite WebSocket : false"
echo "    Rate Limit        : 100"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
