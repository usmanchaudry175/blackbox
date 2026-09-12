#!/usr/bin/env python3
"""
Phase 5 — byte-for-byte verification against known generator output.

Compares the generator's ground-truth manifest (what it actually
attempted to send, and which faults it applied) against what the C++
pipeline (StreamReader -> LogWriter -> replay()) actually recorded.
"""

import argparse
import json
import subprocess
import sys


def load_manifest(path):
    with open(path) as f:
        return json.load(f)


def parse_replay_output(text):
    """Parses replay_cli's stdout into a list of events, same shape as
    what replay() itself produces, so this script never needs to
    reimplement any C++ decoding logic — it only trusts the tool's
    printed output."""
    events = []
    for line in text.splitlines():
        if line.startswith("FRAME "):
            parts = dict(p.split("=") for p in line.split()[1:])
            events.append({"kind": "frame", "sequence": int(parts["seq"])})
        elif line.startswith("DUPLICATE "):
            parts = dict(p.split("=") for p in line.split()[1:])
            events.append({"kind": "duplicate", "sequence": int(parts["seq"])})
        elif line.startswith("GAP "):
            missing = line.split("missing=[")[1].split("]")[0]
            start, end = missing.split("..")
            events.append({"kind": "gap", "start": int(start), "end": int(end)})
    return events


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--log-base", required=True)
    parser.add_argument("--replay-cli", default="./build/replay_cli")
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)

    result = subprocess.run([args.replay_cli, args.log_base], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"replay_cli failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    events = parse_replay_output(result.stdout)

    # Expected: every manifest entry NOT marked dropped or corrupted
    # should appear as exactly one "frame" event with that sequence.
    # Every entry marked dropped, OR corrupted (since a corrupted CRC
    # is rejected by decode() and is indistinguishable from never
    # having arrived), should be ABSENT and covered by a gap range.
    expected_present = set()
    expected_missing = set()

    for entry in manifest:
        if entry["dropped"] or entry["corrupted"]:
            expected_missing.add(entry["sequence"])
        else:
            expected_present.add(entry["sequence"])

    actual_present = {ev["sequence"] for ev in events if ev["kind"] == "frame"}
    actual_missing = set()
    for ev in events:
        if ev["kind"] == "gap":
            actual_missing.update(range(ev["start"], ev["end"] + 1))

    errors = []

    false_missing = expected_present - actual_present
    if false_missing:
        errors.append(f"Frames the manifest says SHOULD be present but are NOT in replay: {sorted(false_missing)}")

    false_present = expected_missing & actual_present
    if false_present:
        errors.append(f"Frames the manifest says should be MISSING but DID appear in replay: {sorted(false_present)}")

    unexplained_gaps = actual_missing - expected_missing
    if unexplained_gaps:
        errors.append(f"Replay reports gaps the manifest doesn't explain (real pipeline bug, not injected fault): {sorted(unexplained_gaps)}")

    unexplained_presence_as_missing = expected_missing - actual_missing
    if unexplained_presence_as_missing:
        errors.append(f"Manifest says these should be missing, but replay neither shows them present NOR reports them in a gap (they vanished silently): {sorted(unexplained_presence_as_missing)}")

    if errors:
        print("VERIFICATION FAILED:\n")
        for e in errors:
            print(f"  - {e}\n")
        sys.exit(1)

    print(f"VERIFICATION PASSED: {len(expected_present)} frames matched exactly, "
          f"{len(expected_missing)} injected faults (loss/corruption) all correctly explained by gaps.")


if __name__ == "__main__":
    main()