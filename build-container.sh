#!/usr/bin/env bash

set -euo pipefail

readonly ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

image="${IMAGE:-seaproxy:dev}"
jobs="${JOBS:-2}"
push=0

usage() {
    cat <<'EOF'
Usage: ./build-container.sh [--image IMAGE] [--jobs JOBS] [--push]

Builds the SeaProxy OCI image with Docker or Podman. Images are local unless
--push is specified. Set CONTAINER_ENGINE to explicitly select an engine.
EOF
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --image)
            [[ "$#" -ge 2 ]] || {
                echo "--image requires a value" >&2
                exit 2
            }
            image="$2"
            shift 2
            ;;
        --jobs)
            [[ "$#" -ge 2 ]] || {
                echo "--jobs requires a value" >&2
                exit 2
            }
            jobs="$2"
            shift 2
            ;;
        --push)
            push=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! "${jobs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "--jobs must be a positive integer" >&2
    exit 2
fi

if [[ -n "${CONTAINER_ENGINE:-}" ]]; then
    engine="${CONTAINER_ENGINE}"
elif command -v docker >/dev/null 2>&1; then
    engine="docker"
elif command -v podman >/dev/null 2>&1; then
    engine="podman"
else
    echo "Docker or Podman is required to build the container image" >&2
    exit 1
fi

if ! command -v "${engine}" >/dev/null 2>&1; then
    echo "Container engine not found: ${engine}" >&2
    exit 1
fi

if [[ "${push}" -eq 1 ]]; then
    registry="${image%%/*}"
    if [[ "${image}" != */* ||
          ("${registry}" != *.* && "${registry}" != *:* && "${registry}" != "localhost") ]]; then
        echo "--push requires an explicitly registry-qualified image name" >&2
        echo "Example: ghcr.io/owner/seaproxy:0.1.0" >&2
        exit 2
    fi
fi

"${engine}" build \
    --build-arg "JOBS=${jobs}" \
    --tag "${image}" \
    "${ROOT_DIR}"

if [[ "${push}" -eq 1 ]]; then
    "${engine}" push "${image}"
fi

echo "Built ${image}"
