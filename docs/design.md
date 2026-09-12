# Design Decisions — Black Box Telemetry Pipeline

Running log of design decisions, the alternatives considered, and the reasoning.
Written as decisions are made, not retrospectively.

---

## D1 — Framing: COBS with 0x00 delimiter

**Decision:** COBS-encode each frame, delimited by `0x00`.

**Alternatives considered:** 1-, 2- or 4-byte magic sync marker.

**Reasoning:** A magic marker is probabilistic — it can occur by chance inside
payload data, and MPU-6050 output is far from uniformly random (a stationary
sensor produces long runs of `0x00`), so the naive false-match maths understates
the problem. COBS guarantees no `0x00` byte appears inside an encoded frame, so
the delimiter is unambiguous rather than merely unlikely. Recovery after
corruption is bounded: at most one frame is lost, because the next `0x00` is
guaranteed to be a real boundary.

**Cost:** 1 overhead byte per frame at this frame size (one additional byte per
254 consecutive non-zero bytes). Cheaper than the 2–4 bytes a marker would need.

---

## D2 — CRC inside the COBS encoding

**Decision:** Build raw frame → compute CRC over it → append CRC → COBS-encode
the whole thing → emit delimiter. Decode reverses this.

**Alternatives considered:** COBS-encode the payload, append the CRC raw.

**Reasoning:** CRC output is effectively uniform over its space, so a `0x00`
byte in it is expected, not exceptional. Appending it outside the encoding would
break the delimiter guarantee intermittently — worse than not having the
guarantee, because the failure is rare and non-obvious.

**General principle:** framing is the outermost layer. Any transformation that
guarantees a property of the byte stream must wrap everything beneath it, or the
inner layers can violate it.

**Consequence:** the decoder must COBS-decode before it can verify the CRC, so it
will sometimes decode garbage. A corrupted length byte could point past the
buffer end — the decoder must be defensive. Primary target for the Phase 1 fuzzer.

---

## D3 — Sequence number: 32-bit

**Decision:** 32-bit sequence number, one per sample.

**Wrap analysis at 100 Hz:**

| Width | Values | Wrap time |
|---|---|---|
| 8-bit | 256 | 2.6 seconds |
| 16-bit | 65,536 | ~11 minutes |
| 32-bit | 4.29e9 | ~497 days |

**Reasoning:** 16-bit wraps within a routine test run, which breaks index
uniqueness — "sequence 4000" would refer to six different frames in a one-hour
recording, and Phase 5's seek depends on a unique key. Host-side wrap extension
(the RTP technique) was considered and rejected: it cannot disambiguate how many
wraps occurred across a reconnection gap. 32-bit costs 2 extra bytes per frame
(~12 KB/min at 100 Hz) and makes the sequence number unique across wire, storage
and index — one representation everywhere.

---

## D4 — Session ID retained

**Decision:** Keep a session ID, set at device power-up.

**Purpose:** distinguishes a device reboot from a transmission gap. Note this is
*not* for wrap handling — D3 removed that need. Also lets replay refuse to stitch
two different sessions into one recording.

**Open:** width and generation method (random vs persistent counter); whether it
appears in every frame or once per session.

---

## D5 — No wall-clock time on the device

**Decision:** the device sends monotonic uptime only. UTC is joined on the
ground station.

**Reasoning:** the MCU has no RTC and no network — it cannot know wall-clock
time. The ground station records its own UTC alongside the device's uptime for
the first frame of a session, stored once in the log header; absolute times for
all other frames are derived from that mapping.

**Rejected:** stamping frames with ground-station receive time. Receive time is
not sample time — I2C transfer, buffering, transmission and OS scheduling add
variable delay, which would bake jitter permanently into a recording whose whole
purpose is faithful reconstruction.

---

## D6 — Amortised timestamp

**Decision:** send a device timestamp periodically (~1 Hz), not per frame.
Interpolate between markers.

**Alternatives considered:** per-frame timestamp (4 bytes/frame, ~24 KB/min);
no timestamp at all.

**Reasoning:** dropping it entirely makes clock drift invisible (a cheap crystal
runs 20–50 ppm off, and the sampling loop will not hit exactly 100 Hz), makes
missed samples indistinguishable from dropped packets, and makes the sample rate
an assumption rather than a measurement. Amortising gives drift measurement and
missed-sample detection for ~0.04 bytes/frame.

---

## D7 — Flags byte

**Decision:** a flags byte in the header, bit 0 = timestamp present.

**Alternatives considered:** a frame-type field; implicit by `seq % N == 0`.

**Reasoning:** same 1-byte cost as a type field, but the remaining seven bits are
available for later use — candidates include "buffered/backfilled rather than
live" (needed for store-and-forward) and "first frame after reboot". The implicit
scheme costs nothing but hides a coupling: the parser must know the interval, and
changing it would make old logs unreadable.

**Consequence:** frames now have two legal lengths. The parser's length check
must accept exactly base or base+4 — no longer "wrong length means corrupt".

---

## D8 — Payload: raw 16-bit sensor counts

**Decision:** transmit the six MPU-6050 values as raw 16-bit counts. Scale
factors applied on the ground station.

**Alternatives considered:** convert to floats on the device (24 bytes vs 12).

**Reasoning:** size is the lesser argument. The real reason is that a scale
factor applied on the device is baked into the recording permanently — if the
accelerometer was configured for ±4g and the ground software assumed ±2g, raw
counts let you fix one constant and reprocess, while converted floats mean the
flight is lost. A recorder should store what the sensor reported, not an
interpretation of it.

---

## D9 — Session ID: 8-bit, in every frame

**Decision:** 8-bit session ID present in every frame, plus a flags bit meaning
"first frame of session".

**Alternatives considered:** the flags bit alone, with the ID sent once at start
of session.

**Reasoning:** the flag and the ID solve different problems. A once-per-session
ID assumes the receiver sees the first frame, which it often will not — the
device transmits from power-up and the ground station may attach seconds later.
Store-and-forward makes this worse: backfilled frames are by definition never
first frames, so if the device rebooted during an outage there is no way to tell
backfilled session A from live session B — exactly the ambiguity the session ID
exists to resolve.

**Principle — self-describing records:** every frame carries enough metadata to
be interpreted with no prior context. The cost is bandwidth; the benefit is that
a reader joining mid-stream, or reading from the middle of a file, needs no
state. Standard practice in log and packet formats.

**Width:** 8 bits is sufficient because the ID only needs to distinguish
*consecutive* sessions on one device, not be globally unique.

**Open (deferred to Phase 8, firmware):** generation method. A persistent counter
needs EEPROM (~100k write cycles, fine at one write per boot); random needs an
entropy source the MCU does not really have — a fixed seed gives the same value
every boot, and seeding from a floating analogue pin is weak.

---

## D10 — Format version in flag bits, not a separate byte

**Decision:** format version occupies bits 5–7 of the flags byte. Version 1
currently. Version 7 reserved as an escape hatch meaning "extended version
follows".

**Alternatives considered:** a full version byte per frame (256 versions,
6 KB/min); version sent once per session.

**Reasoning:** once-per-session fails on mid-stream attach, and worse than for
the session ID — an unknown session is a labelling problem, an unknown version
means nothing can be parsed at all. It also creates a bootstrapping problem: to
read a version-carrying frame the parser must already know that frame's layout,
so that layout could never change. The ground-station log header has the same
hole, since the receiver only learns the version from a frame. Three spare bits
in a byte already being sent gives eight versions for free.

**Why the high bits:** the version appears as the leading nibble in a hex dump,
so it can be eyeballed. `0x21` reads as version 1, timestamp present.

**Flags byte, final:**

| Bit | Meaning |
|---|---|
| 0 | timestamp present |
| 1 | first frame of session |
| 2–4 | reserved — transmit zero, ignore on receive |
| 5–7 | format version (0–7), currently 1 |

Reserved bits are transmitted as zero and ignored on receive, so meaning can be
added later without breaking existing parsers.

---

## D11 — Endianness: big-endian throughout

**Decision:** all multi-byte fields big-endian, serialised byte by byte. No code
may rely on struct memory layout to produce wire bytes.

**Reasoning:** the usual argument for little-endian is that it matches both MCU
and host so a struct could be memcpy'd onto the wire — but fields are
hand-serialised anyway to avoid struct padding, so that saving was already
spent. Byte swapping is a single instruction on both ends and immeasurable at
100 Hz, so performance is not a real argument either way.

The deciding factors:

- **MPU-6050 native ordering.** The register map returns `ACCEL_XOUT_H` before
  `ACCEL_XOUT_L` — high byte first, i.e. big-endian. A big-endian wire format
  makes the firmware a pass-through for the payload: bytes go into the frame in
  the order they arrive from I2C, no swap, no per-field assembly for the largest
  part of the frame. The constrained device does the least work; the host, which
  has cycles to spare, does the conversion.
- **Debuggability.** Sequence 4,660 appears as `00 00 12 34` and reads left to
  right. Little-endian shows `34 12 00 00`, requiring mental reversal of every
  multi-byte field for the weeks spent reading hex dumps in Phases 1–3.
- **Convention.** Network byte order is big-endian, so the format aligns with
  what a reviewer expects.

**Host-side note:** do not use `ntohs`/`htons`. Write an explicit read helper
(shift the first byte left by eight, OR the second) — no platform dependency,
obviously correct, identical behaviour anywhere.

**Portability test:** the format is specified strictly enough that a decoder
could be written in Python or Rust from this document alone.

---

## D12 — Field ordering

**Decision:** flags, session ID, sequence, payload, optional timestamp, CRC.

**Forced positions:** flags must be first — the parser reads it to learn the
version and whether a timestamp is present, so it cannot sit at an offset that
depends on what it describes. CRC must be last, since it covers everything
before it.

**Timestamp after the payload, not before:** placing it before would make the
payload's offset conditional on a flag bit. After the payload, every fixed field
sits at a constant offset and only the timestamp itself is conditional. It is
also metadata about the sample rather than part of it, and hanging optional
extras off the end means adding another optional field later disturbs no
existing offset.

**Header fields ascend by scope:** flags describe the frame, session identifies
the run, sequence identifies the sample.

---

## D13 — Maximum encoded frame size

**Decision:** derive the wire buffer size from compile-time constants, never a
magic number.

```
encoded_max = n + ceil(n / 254)        // standard COBS worst-case bound
kMaxRawFrame  = 6 + 12 + 4 + 2 = 24
kCobsMax      = 24 + ceil(24/254) = 25
kMaxWireFrame = 25 + 1 (delimiter) = 26
```

Use integer ceiling division — `(n + 253) / 254` — rather than `ceil()` on
floats. Pair the constants with a `static_assert` on the total so that a future
payload change fails the build and forces the buffer sizing to be reconsidered,
rather than surfacing as a runtime overflow.

**Reader rule:** `kMaxWireFrame` is the maximum for a *valid* frame. A reader
scanning a corrupted stream may accumulate bytes without ever meeting a
delimiter, so once accumulated bytes exceed the maximum, the frame is
definitionally corrupt — discard and resynchronise at the next delimiter.
Without this check there is an unbounded buffer-growth path, which is a
denial-of-service bug in real protocol code and a primary fuzzer target.

---

## D14 — Corruption handling: two independent checks

**Trace of a flipped flags bit** (a 20-byte frame appearing to claim 24, or
vice versa):

1. **COBS decode yields an exact length.** The delimiter marks where the frame
   ended, so the actual length L is known independently of anything the flags
   claim.
2. **The length check catches it first.** Flags claim 24 bytes, L is 20 —
   mismatch, discard, resynchronise, before the CRC is even computed. The two
   legal lengths are therefore a cross-check rather than a weakness: the flags
   byte makes a claim about length, the framing layer knows the truth, and any
   disagreement is corruption.
3. **The CRC catches it regardless.** The flags byte at offset 0 is inside the
   CRC's covered range, so a flipped bit changes the computed value. CRC-16
   catches any single-bit error with certainty. The version bits are in the same
   byte and get the same protection.

**Design rule that follows:** always locate the CRC from the actual decoded
length — the last two bytes — never from the flags-implied length. Deriving
position from the framing layer rather than from frame contents keeps the
failure clean; otherwise a corrupted flags byte means comparing garbage to
garbage.

**Residual risk:** roughly 1 in 65,536 corrupted frames passes CRC-16 by chance.
At 100 Hz with 10% loss that is a false accept every few hours. Accepted rather
than pretended away; CRC-32 would cost 2 more bytes if it ever mattered.

---

## D15 — On-disk storage format: raw frame bytes, not wire format

**Decision:** Persisted log records store raw frame bytes only (flags,
session, sequence, payload, optional timestamp, CRC) — no COBS encoding,
no delimiter.

**Alternatives considered:** Store the full wire format (COBS + delimiter),
identical to what travels over the transport.

**Reasoning:** COBS and the delimiter exist to solve one specific problem —
finding message boundaries in a byte stream with no inherent framing,
where corruption is a real possibility in transit. A file is not that
environment: the writer controls every byte that goes in, and reads its
own file back with no adversarial channel in between. Framing against a
threat model that doesn't apply just costs bytes and CPU for no benefit.
Each record remains self-describing via its own flags byte (the
timestamp-present bit), exactly like the wire format — so a reader
doesn't need a delimiter or a length prefix to know where one record ends
and the next begins; it already knows from that single byte.

**Consequence:** `serialize_raw()` was extracted out of `encode()` as its
own function, so the exact same field-serialization logic is shared
between the wire path (`encode()` calls it, then COBS-encodes the result)
and the storage path (`LogWriter` calls it directly) — no duplicated
serialization logic to keep in sync.

---

## D16 — Ring buffer full-buffer policy: drop-newest

**Decision:** When the SPSC ring buffer is full, `try_push()` returns
`false` and the incoming item is discarded. The caller is responsible for
counting drops; the buffer itself doesn't decide what a drop means.

**Alternatives considered:** Overwrite-oldest — silently discard the
oldest unread item to make room for the new one.

**Reasoning:** Overwrite-oldest actively destroys data that hasn't yet
reached persistence. For a system whose entire purpose is faithful
recording, silently erasing something already captured is a strictly
worse failure mode than refusing a new item and making the refusal
visible via a counter. This is the same "flag reality, don't silently
normalize" principle already applied elsewhere — duplicates are reported
rather than suppressed (reader.cpp), gaps are reported rather than
absorbed (replay.cpp) — extended to backpressure specifically.

**Consequence:** A full buffer becomes an observable, countable event
(a producer-side drop counter) rather than an invisible one. Whether to
treat drops as fatal, log them, or just monitor them is left entirely to
whatever code owns the producer thread — the ring buffer's job is
correctness and reporting, not policy.

---

## D17 — Reordering policy: detect-and-report, no reassembly

**Decision:** `StreamReader` and `replay()` track sequence numbers as a
monotonic high-water mark. A frame arriving below the high-water mark (and not
an exact repeat of one already seen — see D19) is reported as out-of-order and
counted, but does not move the high-water mark backward, and is not held for
reassembly into sequence order.

**Alternatives considered:** a reorder buffer that holds late frames and
re-emits them in strict sequence order once gaps are filled or a timeout
elapses.

**Reasoning:** reassembly requires an unbounded (or arbitrarily-sized) buffer
and a timeout policy to decide when to give up waiting for a missing frame —
both add real complexity for a use case (telemetry replay/analysis) where the
consumer cares about *what arrived and when*, not receiving a strictly ordered
stream. Detect-and-report preserves arrival order in the output, which is
itself useful data (it shows exactly how disordered the transport was),
and keeps `StreamReader` and `replay()` stateless beyond a high-water mark and
a bounded seen-set (D19).

**Critical implementation detail:** the high-water mark must never move
backward on an out-of-order frame. Regressing it would corrupt every
subsequent gap and duplicate check, since those checks are defined relative to
the high-water mark. This was caught as a latent bug during Phase 6 and is
covered by an explicit regression test in both `test_reader.cpp` and
`test_replay.cpp` that would fail if the high-water mark were allowed to move
backward.

---

## D18 — Disk-full handling: `write_frame()` returns `bool`

**Decision:** `LogWriter::write_frame()` returns `bool` rather than throwing.
`open_new_segment()` and `rotate_if_needed()` follow the same pattern. A new
`write_failures()` stat counts failed writes. The constructor still throws if
the *first* segment fails to open — that is setup misconfiguration (bad path,
no permissions), a different failure class from disk filling up mid-run.

**Alternatives considered:** throw an exception on every write failure,
consistent with the constructor.

**Reasoning:** a disk filling up mid-capture is an expected, recoverable
operating condition for a long-running logger — not exceptional in the
sense that warrants unwinding the call stack. A caller (e.g. `capture_cli`)
should be able to observe the failure, log it, and keep running (the ring
buffer upstream can still absorb some backlog, or the operator can free space
and let writes resume) rather than the whole capture process crashing on the
first full disk. Setup misconfiguration is different: there is no reasonable
way to "keep running" without a working first segment, so throwing there is
still correct.

**Consequence:** every call site must check the return value. `capture_cli.cpp`
now checks it and reports `write_failures` in its stats output; any future
call site that discards the return value silently swallows disk-full
conditions.

---

## D19 — Duplicate detection: bounded seen-set, not high-water-mark equality

**Decision:** `StreamReader` and `replay()` each keep a bounded
`std::unordered_set<uint32_t>` of recently-seen sequence numbers (window size
1024, `kSeenWindow`), pruned as the high-water mark advances. A frame is a
duplicate if its sequence appears in this set, not merely if it equals the
current high-water mark.

**Alternatives considered:** duplicate defined only as `sequence ==
last_sequence_` (the original D17 implementation).

**Reasoning:** equality-with-high-water-mark only catches a repeat of the
single most recently accepted frame. Once the high-water mark has advanced
past a sequence, an exact repeat of that earlier sequence falls through to the
out-of-order check (D17) instead of being recognised as a duplicate — this was
caught as a test failure (`test_reader.cpp`, `duplicates_detected == 1`
expected, `0` observed) when a frame repeated two sequences behind the current
mark. A seen-set correctly distinguishes "this exact frame already arrived"
from "this sequence was never seen and is arriving very late," which the
high-water mark alone cannot do.

**Why bounded, not unbounded:** an unbounded set would grow for the lifetime
of a session, which is unacceptable for a long-running logger. 1024 is
generously larger than any realistic reorder distance for this transport, and
entries below `last_sequence_ - kSeenWindow` are pruned on every accepted
frame. A sequence reordered further back than the window will be
misclassified as out-of-order rather than duplicate — an accepted, documented
tradeoff rather than a silent one.

**Consequence:** the same seen-set logic now exists independently in
`StreamReader::update_sequence_tracking()` and in the `replay()` lambda.
Worth extracting into a shared helper (e.g. a `SequenceTracker` class) at
some point, since a fix applied to one and not the other is exactly what
happened here.

---

## Frame layout (final, version 1)

| Offset | Field | Bytes | Notes |
|---|---|---|---|
| 0 | Flags | 1 | version in bits 5–7, see D10 |
| 1 | Session ID | 1 | D9 |
| 2 | Sequence | 4 | big-endian, D3 |
| 6 | Payload | 12 | 6 × int16 raw counts, big-endian, D8 |
| 18 | Timestamp | 0 or 4 | present only if flags bit 0 set, D6 |
| 18 or 22 | CRC-16-CCITT | 2 | covers all preceding bytes, D2 |

**Totals:** 20 bytes base, 24 with timestamp. On the wire, COBS adds 1 overhead
byte plus the delimiter: **22 and 26 bytes**.

**Throughput at 100 Hz** with a timestamp every 100th frame: ~2,204 bytes/sec,
~129 KB/min, ~7.7 MB/hour. Comfortable on serial, workable on most radio links —
this is the figure that decides whether a LoRa link is ever viable.

**Decoder walk:** flags at 0 → extract version and timestamp bit → session at 1
→ sequence at 2 → payload at 6 → timestamp at 18 if set → CRC at the last two
bytes. Every offset is a constant or a constant plus a known conditional.

---

## Still open

- **Timestamp units and width** — ms since boot (32-bit wraps ~49 days) vs
  microseconds (wraps ~71 minutes)
- **Session ID generation** — deferred to Phase 8, see D9
- **`SequenceTracker` extraction** — `StreamReader` and `replay()` currently
  duplicate the seen-set/high-water-mark logic (D17/D19); a shared class would
  prevent a fix applied to one from being missed in the other, as happened
  during Phase 6.