#!/usr/bin/env bash

set -euo pipefail

readonly ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
readonly REDIS_PORT="${REDIS_PORT:-16379}"
readonly SEAPROXY_PORT="${SEAPROXY_PORT:-17000}"
readonly LOG_DIR="${LOG_DIR:-${BUILD_DIR}/e2e-logs}"

mkdir -p "${LOG_DIR}"
redis_pid=""
seaproxy_pid=""

cleanup() {
    status=$?
    trap - EXIT INT TERM
    if [[ -n "${seaproxy_pid}" ]] && kill -0 "${seaproxy_pid}" 2>/dev/null; then
        kill "${seaproxy_pid}"
        wait "${seaproxy_pid}" 2>/dev/null || true
    fi
    if [[ -n "${redis_pid}" ]] && kill -0 "${redis_pid}" 2>/dev/null; then
        kill "${redis_pid}"
        wait "${redis_pid}" 2>/dev/null || true
    fi
    if [[ "${status}" -ne 0 ]]; then
        echo "Redis log:" >&2
        cat "${LOG_DIR}/redis.log" >&2 || true
        echo "SeaProxy log:" >&2
        cat "${LOG_DIR}/seaproxy.log" >&2 || true
    fi
    exit "${status}"
}
trap cleanup EXIT INT TERM

redis-server \
    --bind 127.0.0.1 \
    --port "${REDIS_PORT}" \
    --save "" \
    --appendonly no \
    --protected-mode no \
    >"${LOG_DIR}/redis.log" 2>&1 &
redis_pid=$!

for _ in {1..50}; do
    if redis-cli -h 127.0.0.1 -p "${REDIS_PORT}" PING >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "${redis_pid}" 2>/dev/null; then
        echo "Redis exited before becoming ready" >&2
        exit 1
    fi
    sleep 0.1
done
if ! redis-cli -h 127.0.0.1 -p "${REDIS_PORT}" PING >/dev/null 2>&1; then
    echo "Redis did not become ready" >&2
    exit 1
fi

"${BUILD_DIR}/seaproxy" \
    --listen-address 127.0.0.1 \
    --listen-port "${SEAPROXY_PORT}" \
    --redis-address 127.0.0.1 \
    --redis-port "${REDIS_PORT}" \
    --redis-mode standalone \
    --redis-pool-size 1 \
    --private-pool-size 1 \
    --private-max-connections 2 \
    --smp 1 \
    --memory 256M \
    --overprovisioned \
    >"${LOG_DIR}/seaproxy.log" 2>&1 &
seaproxy_pid=$!

go run "${ROOT_DIR}/tests/e2e/main.go" "${SEAPROXY_PORT}"
