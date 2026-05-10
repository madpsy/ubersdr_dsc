#!/usr/bin/env bash
# docker.sh — build the ubersdr_dsc Docker image
#
# All binaries (dsc_rx_from_ubersdr) are built from source inside
# the Docker image.  No host binaries are required.
#
# Usage:
#   ./docker.sh [build|arm64|multi|push|run]
#
#   build  — build the image for linux/amd64 (default, uses buildx)
#   arm64  — build the image for linux/arm64 (Raspberry Pi, Apple Silicon, etc.)
#   multi  — build & push a multi-arch manifest (amd64 + arm64) to the registry
#   push   — build amd64, push to registry, then commit & push git repo
#   run    — run the image locally (set env vars below)
#
# Environment variables (build):
#   IMAGE      Docker image name/tag   (default: madpsy/ubersdr_dsc:latest)
#   PLATFORM   Docker --platform flag  (default: linux/amd64)
#   BUILDER    buildx builder name     (default: ubersdr_dsc_builder)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="${IMAGE:-madpsy/ubersdr_dsc:latest}"
PLATFORM="${PLATFORM:-linux/amd64}"
BUILDER="${BUILDER:-ubersdr_dsc_builder}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

die() { echo "error: $*" >&2; exit 1; }

check_deps() {
    command -v docker >/dev/null || die "docker not found in PATH"
    docker buildx version >/dev/null 2>&1 || die "docker buildx not available (Docker >= 19.03 required)"
}

# Ensure a buildx builder that supports multi-platform exists and is active.
ensure_builder() {
    if ! docker buildx inspect "$BUILDER" >/dev/null 2>&1; then
        echo "Creating buildx builder '$BUILDER'..."
        docker buildx create --name "$BUILDER" --driver docker-container --bootstrap
    fi
    docker buildx use "$BUILDER"
}

# Stage the build context into a temp directory (excludes build artefacts / git).
stage_context() {
    TMPCTX="$(mktemp -d)"
    trap 'rm -rf "$TMPCTX"' EXIT
    echo "Staging build context in $TMPCTX..."
    rsync -a --exclude='/build' \
              --exclude='.git' \
              "$SCRIPT_DIR/" "$TMPCTX/"
}

# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

build() {
    check_deps
    ensure_builder
    stage_context

    echo "Building image $IMAGE (platform=$PLATFORM)..."
    docker buildx build \
        --builder "$BUILDER" \
        --platform "$PLATFORM" \
        --tag "$IMAGE" \
        --load \
        "$TMPCTX"

    echo "Built: $IMAGE"
}

# Build both amd64 and arm64, push a combined manifest to the registry.
# This does NOT break any existing single-arch tags because it only touches
# the tag specified by $IMAGE (default: :latest).
multi() {
    check_deps
    ensure_builder
    stage_context

    local platforms="linux/amd64,linux/arm64"
    echo "Building multi-arch image $IMAGE (platforms=$platforms)..."
    docker buildx build \
        --builder "$BUILDER" \
        --platform "$platforms" \
        --tag "$IMAGE" \
        --push \
        "$TMPCTX"

    echo "Multi-arch manifest pushed: $IMAGE"
    echo "  Covered platforms: $platforms"
}

push() {
    # Build both amd64 and arm64, push a combined manifest, then commit & push the git repo.
    check_deps
    ensure_builder
    stage_context

    local platforms="linux/amd64,linux/arm64"
    echo "Building multi-arch image $IMAGE (platforms=$platforms)..."
    docker buildx build \
        --builder "$BUILDER" \
        --platform "$platforms" \
        --tag "$IMAGE" \
        --push \
        "$TMPCTX"

    echo "Pushed multi-arch manifest: $IMAGE"
    echo "  Covered platforms: $platforms"
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
    arm64) PLATFORM=linux/arm64 build ;;
    multi) multi ;;
    push)  push  ;;
    run)   shift; run_image "$@" ;;
    *)
        echo "Usage: $0 [build|arm64|multi|push|run [dsc_rx_from_ubersdr-args...]]" >&2
        exit 1
        ;;
esac
