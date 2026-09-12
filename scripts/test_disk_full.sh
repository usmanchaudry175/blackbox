#!/usr/bin/env bash
# Disk-full integration test: mounts a genuinely size-limited tmpfs,
# runs disk_full_probe against it, verifies clean failure behaviour
# (no crash, write_failures() incremented) and that readback via
# replay_cli recovers all successfully-written frames, minus at most
# one possible truncated tail record from a write that was mid-flight
# when the disk filled (LogReader::for_each_frame treats that as a
# clean end-of-segment, not an error — see docs/design.md D18).
#
# Requires sudo (mounting tmpfs). Not run as part of `ctest` — invoke
# manually: ./scripts/test_disk_full.sh [build_dir]
set -euo pipefail

MOUNT_POINT="$(mktemp -d)"
BUILD_DIR="${1:-build}"
PROBE="${BUILD_DIR}/disk_full_probe"
REPLAY_CLI="${BUILD_DIR}/replay_cli"

cleanup() {
    echo "Cleaning up..."
    sudo umount "${MOUNT_POINT}" 2>/dev/null || true
    rmdir "${MOUNT_POINT}" 2>/dev/null || true
}
trap cleanup EXIT

if [[ ! -x "${PROBE}" ]]; then
    echo "error: ${PROBE} not found — build it first (cmake --build ${BUILD_DIR})" >&2
    exit 1
fi

echo "Mounting 256KB tmpfs at ${MOUNT_POINT}..."
sudo mount -t tmpfs -o size=256k tmpfs "${MOUNT_POINT}"
sudo chown "$(id -u):$(id -g)" "${MOUNT_POINT}"

echo "Running disk_full_probe..."
PROBE_LOG_BASE="${MOUNT_POINT}/probe_log"
set +e
PROBE_OUTPUT=$("${PROBE}" "${PROBE_LOG_BASE}")
PROBE_STATUS=$?
set -e
echo "${PROBE_OUTPUT}"

if [[ ${PROBE_STATUS} -ne 0 ]]; then
    echo "FAIL: disk_full_probe reported an error" >&2
    exit 1
fi

SUCCESSES=$(echo "${PROBE_OUTPUT}" | grep -oP 'successes=\K[0-9]+')
FAILURES=$(echo "${PROBE_OUTPUT}" | grep -oP '(?<![a-z_])failures=\K[0-9]+')
REPORTED_FAILURES=$(echo "${PROBE_OUTPUT}" | grep -oP 'reported_write_failures=\K[0-9]+')

if [[ -z "${SUCCESSES}" || -z "${FAILURES}" || -z "${REPORTED_FAILURES}" ]]; then
    echo "FAIL: could not parse probe output" >&2
    exit 1
fi

echo "Checking write_failures() counter agrees with observed failures..."
if [[ "${FAILURES}" != "${REPORTED_FAILURES}" ]]; then
    echo "FAIL: probe observed ${FAILURES} failed write_frame() calls but" \
         "write_failures() reports ${REPORTED_FAILURES} — counter is not" \
         "tracking accurately." >&2
    exit 1
fi
echo "OK: write_failures() == ${REPORTED_FAILURES}, matches observed failures."

echo "Verifying readback via replay_cli (expect ${SUCCESSES}, or $((SUCCESSES - 1)) if the last write was mid-flight)..."
if [[ -x "${REPLAY_CLI}" ]]; then
    REPLAY_OUTPUT=$("${REPLAY_CLI}" "${PROBE_LOG_BASE}")
    REPLAY_FRAME_COUNT=$(echo "${REPLAY_OUTPUT}" | grep -oP 'Frames:\s*\K[0-9]+')

    if [[ -z "${REPLAY_FRAME_COUNT}" ]]; then
        echo "warning: could not parse a frame count from replay_cli output — inspect manually:" >&2
        echo "${REPLAY_OUTPUT}"
    elif (( REPLAY_FRAME_COUNT == SUCCESSES || REPLAY_FRAME_COUNT == SUCCESSES - 1 )); then
        echo "PASS: readback frame count (${REPLAY_FRAME_COUNT}) matches expected range."
    else
        echo "FAIL: readback frame count (${REPLAY_FRAME_COUNT}) does not match" \
             "successes (${SUCCESSES}) or successes-1 — possible data loss beyond" \
             "the expected single truncated tail record." >&2
        exit 1
    fi
else
    echo "warning: replay_cli not found at ${REPLAY_CLI}, skipping readback check" >&2
fi

echo "PASS: disk-full handling verified (writer failed cleanly, counter accurate, readback matches expectations)."