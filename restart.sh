#!/usr/bin/env bash
# restart.sh — restart the ubersdr_dsc service
#
# Usage:
#   ./restart.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/dsc"

cd "${INSTALL_DIR}"
echo "Stopping ubersdr_dsc..."
docker compose down
echo "Starting ubersdr_dsc..."
docker compose up -d --remove-orphans
echo "Done."
echo "  View logs : docker compose logs -f"
