#!/usr/bin/env bash
# docker.sh — build the ubersdr_dsc Docker image
#
# All binaries (dsc_rx_from_ubersdr) are built from source inside
# the Docker image.  No host binaries are required.
#
# Usage:
#   ./docker.sh [build|push|run]
#
#   build  — build the image (default)
#   push   — build then push to registry (set IMAGE env var)
#   run    — run the image (set env vars below)
#
# Environment variables (build):
#   IMAGE      Docker image name/tag   (default: madpsy/ubersdr_dsc:latest)
#   PLATFORM   Docker --platform flag  (default: linux/amd64)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="${IMAGE:-madpsy/ubersdr_dsc:latest}"
PLATFORM="${PLATFORM:-linux/amd64}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

die() { echo "error: $*" >&2; exit 1; }

check_deps() {
    command -v docker >/dev/null || die "docker not found in PATH"
}

build() {
    check_deps

    # Create a temporary build context from the source tree only
    TMPCTX="$(mktemp -d)"
    trap 'rm -rf "$TMPCTX"' EXIT

    echo "Staging build context in $TMPCTX..."

    # Copy source tree (excluding build artefacts and git history)
    rsync -a --exclude='/build' \
              --exclude='.git' \
              "$SCRIPT_DIR/" "$TMPCTX/"

    echo "Building image $IMAGE (platform=$PLATFORM)..."
    docker build \
        --platform "$PLATFORM" \
        --tag "$IMAGE" \
        "$TMPCTX"

    echo "Built: $IMAGE"
}

push() {
    build
    echo "Pushing $IMAGE..."
    docker push "$IMAGE"
    echo "Committing and pushing git repository..."
    git add -A
    git diff --cached --quiet || git commit -m "Release $IMAGE"
    git push
}

run_image() {
    args=()
    [[ -n "${UBERSDR_URL:-}"  ]] && args+=(-e "UBERSDR_URL=$UBERSDR_URL")
    [[ -n "${DSC_FREQS:-}"    ]] && args+=(-e "DSC_FREQS=$DSC_FREQS")
    [[ -n "${WEB_PORT:-}"     ]] && args+=(-e "WEB_PORT=$WEB_PORT")

    PORT="${WEB_PORT:-6093}"

    docker run --rm -it \
        --platform "$PLATFORM" \
        -p "${PORT}:${PORT}" \
        "${args[@]}" \
        "$IMAGE" \
        "$@"
}

# ---------------------------------------------------------------------------
# Environment variable reference (for docker run -e ...)
# ---------------------------------------------------------------------------
#
#   UBERSDR_URL   UberSDR base URL (default: http://ubersdr:8080)
#   DSC_FREQS     DSC frequencies in Hz, comma-separated (default: 2187500)
#   WEB_PORT      Web UI port (default: 6093)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

case "${1:-build}" in
    build) build ;;
    push)  push  ;;
    run)   shift; run_image "$@" ;;
    *)
        echo "Usage: $0 [build|push|run [dsc_rx_from_ubersdr-args...]]" >&2
        exit 1
        ;;
esac
