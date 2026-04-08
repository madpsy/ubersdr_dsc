#!/usr/bin/env bash
# start.sh — start the ubersdr_dsc service
#
# Usage:
#   ./start.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/dsc"

cd "${INSTALL_DIR}"
echo "Starting ubersdr_dsc..."
docker compose up -d --remove-orphans
echo "Done."
echo "  View logs : docker compose logs -f"
