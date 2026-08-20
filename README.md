# libsalvage

A receiver for RTP video that is expected to lose packets: FlexFEC recovery
first, then salvage of whatever the FEC could not rebuild.

It exists because the two halves of that job are usually missing from the same
place. Players treat a frame with a hole in it as a frame to drop, and FEC
libraries stop at "recovered or not". On a robot over LTE, both decisions are
wrong: a picture with one slice missing is still most of a picture, and the
difference between showing it and freezing is the difference between flying and
landing.

This is the receive side of the FlexFEC work in
[majestic](https://github.com/OpenIPC/majestic); the sender is the other half.
It is a plain C library with no dependencies beyond libc, so it can be linked
into a player, a GStreamer element, or a test harness.

## Status

| stage | state |
|---|---|
| FlexFEC (RFC 8627) recovery | **done** — verified against a real camera capture |
| RTP depacketisation (RFC 6184 / 7798) | not started |
| slice salvage and first-slice synthesis | not started |
| end-to-end under `netem` | not started |

## Building

```sh
make check          # unit tests + capture replay
make asan           # the same, under AddressSanitizer and UBSan
```

No configure step, no dependencies.

## What "verified" means here

`tests/camera-fec.pcap` is a real capture of a SigmaStar SSC30KQ camera
streaming H.264 with the majestic FlexFEC sender enabled: 3480 media packets
(PT 96) and 1399 repair packets (PT 101).

`salvage-pcap` replays it, drops packets on the way in, and compares every
packet the receiver rebuilds against the original that is still sitting in the
capture. That comparison is the entire point. A repair implementation that
parses cleanly and produces plausible bytes is worse than one that produces
nothing, because the damage reaches the picture silently and gets blamed on the
encoder.

```
$ make check
...
dropped by us   : 69
recovered and byte-exact vs original : 69
recovered but WRONG                  : 0
recovery rate: 100.0% of dropped packets
```

### Recovery against the theoretical ceiling

XOR FEC recovers a group that is missing exactly one member, and iterating that
rule ("peeling") is the best any XOR decoder can do. Running the peel offline
over the same capture and the same drop pattern gives an upper bound the
receiver can be measured against:

| loss | dropped | ceiling | recovered | wrong |
|---|---|---|---|---|
| 1 in 50 | 69 | 69 | **69** | 0 |
| 1 in 20 | 174 | 148 | **148** | 0 |
| 1 in 10 | 348 | 198 | **198** | 0 |
| 1 in 5 | 696 | 373 | **373** | 0 |

The receiver is at the ceiling at every rate. Recovering more than this needs a
lower code rate on the sender or a stronger code than XOR — not better receiver
code.

Bursts are where XOR FEC stops helping, and the numbers say so plainly: at the
same 1-in-50 event rate, recovery falls from 100% to 42% at bursts of 2 and
23.5% at bursts of 5, because a burst puts several losses inside one protection
group. This is the argument for interleaving protection groups rather than for
adding repair packets.

## Design notes

Three things in the recovery core were not obvious, and each was a bug first.

**The window is bounded by the newest sequence number known to exist, not the
newest one that arrived.** If `highest` only tracks arrivals, a loss at the
leading edge sits exactly one slot beyond the window and its repair packet is
discarded as too old — so the most recent loss, the one most worth recovering,
is the one that never is. A repair packet names sequence numbers, and a
recovered packet is a sequence number too; both have to move the window.

**"Outside the window" is two different situations.** A member behind the
window was released long ago and is never coming back, so its group is dead and
the repair packet can be dropped. A member ahead of the window is merely early.
Treating them alike throws away usable repair. This is the same bug as the one
above wearing a different hat, which is why both have tests.

**Recovery runs before anything is released, and repeats while it makes
progress.** A packet rebuilt by one repair packet is often the last one another
was waiting for, and a packet released from the window is gone for good.

## Layout

```
include/salvage/flexfec.h   public API
src/flexfec.c               recovery core
tests/flexfec.c             unit tests (repair packets built from the RFC)
tests/camera-fec.pcap       real capture: SSC30KQ, H.264, FlexFEC enabled
tools/salvage-pcap.c        capture replay with loss injection
```

The unit tests build their own repair packets straight from RFC 8627 rather
than calling a shared helper, so an encoder and a decoder that share a
misreading of the spec cannot both pass.

## Related

- [rkvdec-slice-lab](../rkvdec-slice-lab) — the measurements this is built on:
  what hardware decoders actually do with missing slices, on Rockchip, Intel
  VA-API and GStreamer, and why a lost *first* slice is categorically worse
  than any other.
- majestic branch `feat/rtsp-flexfec` — the sender.

## Licence

MIT. See [LICENSE](LICENSE).
