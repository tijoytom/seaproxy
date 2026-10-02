#!/usr/bin/env bash

set -euo pipefail

readonly ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly SEASTAR_VERSION="${SEASTAR_VERSION:-seastar-25.05.0}"
readonly DEPS_DIR="${DEPS_DIR:-${ROOT_DIR}/.deps}"
readonly SEASTAR_SOURCE_DIR="${SEASTAR_SOURCE_DIR:-${DEPS_DIR}/seastar}"
readonly SEASTAR_PREFIX="${SEASTAR_PREFIX:-${DEPS_DIR}/seastar-install}"
readonly BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
readonly BUILD_TYPE="${BUILD_TYPE:-Release}"
readonly JOBS="${JOBS:-$(nproc)}"
readonly SEASTAR_CONFIG="${SEASTAR_PREFIX}/lib/cmake/Seastar/SeastarConfig.cmake"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "build.sh currently supports Linux only" >&2
    exit 1
fi

if [[ "${SKIP_DEPENDENCY_INSTALL:-0}" != "1" || ! -f "${SEASTAR_CONFIG}" ]]; then
    if [[ ! -d "${SEASTAR_SOURCE_DIR}/.git" ]]; then
        mkdir -p "${DEPS_DIR}"
        git clone \
            --branch "${SEASTAR_VERSION}" \
            --depth 1 \
            --recurse-submodules \
            --shallow-submodules \
            https://github.com/scylladb/seastar.git \
            "${SEASTAR_SOURCE_DIR}"
    else
        current_revision="$(git -C "${SEASTAR_SOURCE_DIR}" rev-parse HEAD)"
        expected_revision="$(
            git -C "${SEASTAR_SOURCE_DIR}" rev-list -n 1 "${SEASTAR_VERSION}" 2>/dev/null || true
        )"
        if [[ -z "${expected_revision}" || "${current_revision}" != "${expected_revision}" ]]; then
            echo "${SEASTAR_SOURCE_DIR} is not checked out at ${SEASTAR_VERSION}" >&2
            echo "Remove it or set SEASTAR_SOURCE_DIR to the correct checkout" >&2
            exit 1
        fi
    fi
fi

if [[ "${SKIP_DEPENDENCY_INSTALL:-0}" != "1" ]]; then
    if [[ "${EUID}" -eq 0 ]]; then
        (
            cd "${SEASTAR_SOURCE_DIR}"
            DEBIAN_FRONTEND=noninteractive ./install-dependencies.sh
        )
    elif command -v sudo >/dev/null 2>&1; then
        (
            cd "${SEASTAR_SOURCE_DIR}"
            sudo env DEBIAN_FRONTEND=noninteractive ./install-dependencies.sh
        )
    else
        echo "Installing dependencies requires root or sudo" >&2
        exit 1
    fi
fi

if [[ ! -f "${SEASTAR_CONFIG}" ]]; then
    (
        cd "${SEASTAR_SOURCE_DIR}"
        ./configure.py \
            --mode=release \
            --without-tests \
            --without-apps \
            --without-demos \
            --disable-dpdk \
            --enable-io_uring \
            --prefix="${SEASTAR_PREFIX}"
        ninja -C build/release -j "${JOBS}" install
    )
fi

cmake \
    -S "${ROOT_DIR}" \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_PREFIX_PATH="${SEASTAR_PREFIX}"
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
