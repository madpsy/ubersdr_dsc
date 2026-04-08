#!/usr/bin/env bash
# build.sh — build dsc_rx_from_ubersdr locally (no Docker)
#
# Requires: build-essential, cmake, libzstd-dev, libcurl4-openssl-dev,
#           libssl-dev, pkg-config, and IXWebSocket (cloned automatically)
#
# Usage:
#   ./build.sh [--clean|-c]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
IXWS_DIR="${SCRIPT_DIR}/IXWebSocket"

CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --clean|-c) CLEAN=1 ;;
    *) echo "Unknown option: $arg"; echo "Usage: $0 [--clean|-c]"; exit 1 ;;
  esac
done

echo "=== dsc_rx_from_ubersdr Build ==="
echo "Source: ${SCRIPT_DIR}/src"
echo "Build:  ${BUILD_DIR}"
echo ""

if [[ "${CLEAN}" -eq 1 ]]; then
  echo "--- Cleaning build directory ---"
  rm -rf "${BUILD_DIR}"
  echo ""
fi

# Clone IXWebSocket if not present
if [[ ! -f "${IXWS_DIR}/ixwebsocket/IXWebSocket.h" ]]; then
  echo "--- Cloning IXWebSocket ---"
  git clone --depth 1 https://github.com/machinezone/IXWebSocket.git "${IXWS_DIR}"
  echo ""
fi

mkdir -p "${BUILD_DIR}"

echo "--- Running cmake ---"
cmake -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DIXWS_ROOT="${IXWS_DIR}" \
      "${SCRIPT_DIR}"

echo ""
echo "--- Running make ---"
cmake --build "${BUILD_DIR}" --parallel "$(nproc)" --target dsc_rx_from_ubersdr

echo ""
echo "=== Build complete ==="
echo "Binary: ${BUILD_DIR}/src/dsc_rx_from_ubersdr"
echo ""
echo "Usage example:"
echo "  ${BUILD_DIR}/src/dsc_rx_from_ubersdr http://192.168.1.10:8073 --freqs 2187500 --web-port 6093"
