#!/usr/bin/env bash
# stop.sh — stop the ubersdr_dsc service
#
# Usage:
#   ./stop.sh

set -euo pipefail

INSTALL_DIR="${HOME}/ubersdr/dsc"

cd "${INSTALL_DIR}"
echo "Stopping ubersdr_dsc..."
docker compose down
echo "Done."
