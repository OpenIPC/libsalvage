# libsalvage — guide for Claude Code

The receive half of a FlexFEC video link: FlexFEC (RFC 8627) recovery first,
then salvage of whatever the FEC could not rebuild. Plain C11, the core linking
nothing beyond libc. `README.md` carries the full rationale, the measured
results, and the design notes — read it before changing the recovery or salvage
core; this file is the operating guide.

## Build and test

```sh
make               # library + tools + unit tests (auto-detects decode backends)
make check         # REQUIRED before every push (see below)
make asan          # the same, under AddressSanitizer + UBSan
./scripts/e2e.sh   # end to end over a lossy channel (needs ffmpeg)
./scripts/hw-e2e.sh# end to end on a real hardware decoder
```

`make check` is the gate: it runs the unit tests and then replays
`tests/camera-fec.pcap` at four loss rates, reconciling **every** rebuilt packet
byte-for-byte against the original on the wire. A plausible-but-wrong
reconstruction fails here rather than reaching a decoder. Run `make asan` too
for anything touching `src/flexfec.c`, `src/depay.c`, `src/bits.c`, or
`src/retarget.c`.

## Hard constraints

- **The core stays libc-only.** The library, `salvage-pcap`, and the unit tests
  must build with a C11 compiler and nothing else. Do not add a dependency to
  them. Only `salvage-play` may link a hardware decoder, and only behind the
  existing `SALVAGE_WITH_GST` / `SALVAGE_WITH_MPP` guards in the `Makefile`, so
  the tree still builds a null player when neither is present.
- **Warning-clean.** `-Wall -Wextra -Wpedantic`, C11. No new warnings.
- **Verification is byte-exact, never by eye.** If `make check` prints `WRONG`,
  a non-`identity retarget`, or a changed byte count, the change is wrong — fix
  the code, do not loosen the check.
- **This is a public repository.** Do not reference any private or internal
  repository, branch name, host, or filesystem path in code, comments, commit
  messages, or docs. The sender is described only as "any RFC 8627 source."

## Invariants in the recovery core (each was a bug first — see README Design notes)

1. The recovery window is bounded by the newest sequence number *known to
   exist*, not the newest that *arrived*: repair packets and recovered packets
   both move the window, or the most recent loss is never recovered.
2. "Outside the window" is two situations: a member *behind* it is gone (drop
   its repair), a member *ahead* is merely early (keep it). Conflating them
   throws away usable repair.
3. Recovery runs before anything is released and repeats while it makes
   progress: one rebuilt packet is often the last another repair was waiting on.

The slice surgery's load-bearing test: rewriting a slice's `frame_num`/POC to
the values it already holds must return the original bytes exactly. `make check`
runs it over every slice in the capture.

## Layout

`include/salvage/*.h` public APIs · `src/flexfec.c` recovery core ·
`src/depay.c` RFC 6184/7798 depacketisation · `src/bits.*` bit reader +
emulation prevention · `src/retarget.*` SPS/PPS + slice-header offsets ·
`src/salvage.c` first-slice synthesis · `src/decoder*.c` backend dispatch (null
/ GStreamer / Rockchip MPP) · `tests/` unit tests + the real capture ·
`tools/` capture replay and the reference player.

## Working in this repo

- **`master` is protected — no direct pushes.** Every change, even a one-liner,
  goes through a pull request (`gh pr create`) that merges by squash.
- Get `make check` (and `make asan` where relevant) green **before** pushing the
  branch; there is no CI to catch it for you yet.
- Unit tests build their repair packets straight from RFC 8627 rather than from
  a shared helper, so an encoder and decoder sharing a misreading of the spec
  cannot both pass. Keep it that way.
