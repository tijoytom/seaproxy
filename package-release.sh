#!/usr/bin/env bash

set -euo pipefail

readonly ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
readonly DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"

if [[ "$#" -ne 1 ]]; then
    echo "Usage: ./package-release.sh VERSION" >&2
    exit 2
fi

version="${1#v}"
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "VERSION must use semantic version format, for example 0.1.0" >&2
    exit 2
fi

project_version="$(
    sed -nE \
        's/^project\(seaproxy VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)$/\1/p' \
        "${ROOT_DIR}/CMakeLists.txt"
)"
if [[ "${version}" != "${project_version}" ]]; then
    echo "Release version ${version} does not match CMake version ${project_version}" >&2
    exit 1
fi

if [[ ! -x "${BUILD_DIR}/seaproxy" || ! -f "${BUILD_DIR}/CPackConfig.cmake" ]]; then
    echo "Build SeaProxy before packaging it: ./build.sh" >&2
    exit 1
fi

mkdir -p "${DIST_DIR}"
find "${DIST_DIR}" -maxdepth 1 -type f \
    -name "seaproxy-${version}-ubuntu24.04-amd64.*" -delete

cpack --config "${BUILD_DIR}/CPackConfig.cmake" -B "${DIST_DIR}"

(
    cd "${DIST_DIR}"
    sha256sum \
        "seaproxy-${version}-ubuntu24.04-amd64.deb" \
        "seaproxy-${version}-ubuntu24.04-amd64.tar.gz" \
        > "seaproxy-${version}-ubuntu24.04-amd64.sha256"
)
