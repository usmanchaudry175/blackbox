#!/usr/bin/env bash
# kill -9 mid-write integration test: starts kill_probe writing frames
# continuously, SIGKILLs it after a short randomised delay, then
# verifies replay_cli recovers at least as many frames as were
# confirmed durable at the last checkpoint before the kill. Also times
# recovery (replay_cli wall-clock time, including process startup —
# not pure in-process replay() time).
#
# Not run as part of `ctest` — invoke manually:
#   ./scripts/test_kill_mid_write.sh [build_dir] [iterations]
set -uo pipefail  # no -e: a killed process legitimately exits non-zero (128+9)

BUILD_DIR="${1:-build}"
ITERATIONS="${2:-10}"
PROBE="${BUILD_DIR}/kill_probe"
REPLAY_CLI="${BUILD_DIR}/replay_cli"

if [[ ! -x "${PROBE}" ]]; then
    echo "error: ${PROBE} not found — build it first (cmake --build build --target kill_probe)" >&2
    exit 1
fi
if [[ ! -x "${REPLAY_CLI}" ]]; then
    echo "error: ${REPLAY_CLI} not found — build it first" >&2
    exit 1
fi

FAIL_COUNT=0
TOTAL_REPLAY_MS=0

for i in $(seq 1 "${ITERATIONS}"); do
    WORK_DIR="$(mktemp -d)"
    LOG_BASE="${WORK_DIR}/kill_log"
    PROGRESS_FILE="${WORK_DIR}/progress.txt"

    echo "--- Iteration ${i}/${ITERATIONS} ---"

    "${PROBE}" "${LOG_BASE}" "${PROGRESS_FILE}" &
    PROBE_PID=$!

    SLEEP_MS=$(( (RANDOM % 400) + 50 ))
    sleep "0.${SLEEP_MS}"

    kill -9 "${PROBE_PID}" 2>/dev/null
    wait "${PROBE_PID}" 2>/dev/null

    if [[ ! -f "${PROGRESS_FILE}" ]]; then
        echo "SKIP: killed before first checkpoint — nothing durable to verify"
        rm -rf "${WORK_DIR}"
        continue
    fi

    LAST_KNOWN_GOOD=$(cat "${PROGRESS_FILE}")
    echo "Last durable checkpoint: seq=${LAST_KNOWN_GOOD}"

    REPLAY_START_NS=$(date +%s%N)
    REPLAY_OUTPUT=$("${REPLAY_CLI}" "${LOG_BASE}")
    REPLAY_END_NS=$(date +%s%N)
    REPLAY_MS=$(( (REPLAY_END_NS - REPLAY_START_NS) / 1000000 ))
    TOTAL_REPLAY_MS=$((TOTAL_REPLAY_MS + REPLAY_MS))

    REPLAY_FRAME_COUNT=$(echo "${REPLAY_OUTPUT}" | grep -oP 'Frames:\s*\K[0-9]+')
    echo "Recovery time: ${REPLAY_MS} ms (${REPLAY_FRAME_COUNT} frames recovered)"

    if [[ -z "${REPLAY_FRAME_COUNT}" ]]; then
        echo "FAIL: could not parse frame count from replay_cli output" >&2
        echo "${REPLAY_OUTPUT}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        rm -rf "${WORK_DIR}"
        continue
    fi

    if (( REPLAY_FRAME_COUNT >= LAST_KNOWN_GOOD )); then
        echo "PASS: readback (${REPLAY_FRAME_COUNT}) >= last checkpoint (${LAST_KNOWN_GOOD})"
    else
        echo "FAIL: readback (${REPLAY_FRAME_COUNT}) is LESS than the last durable" \
             "checkpoint (${LAST_KNOWN_GOOD}) — data thought safely on disk was lost." >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    rm -rf "${WORK_DIR}"
done

echo ""
AVG_REPLAY_MS=$((TOTAL_REPLAY_MS / ITERATIONS))
echo "Average recovery time across ${ITERATIONS} iterations: ${AVG_REPLAY_MS} ms"

if (( FAIL_COUNT == 0 )); then
    echo "PASS: all ${ITERATIONS} kill -9 iterations recovered cleanly."
    exit 0
else
    echo "FAIL: ${FAIL_COUNT}/${ITERATIONS} iterations showed data loss beyond expectations." >&2
    exit 1
fi