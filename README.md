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
| RTP depacketisation (RFC 6184 / 7798) | **done** — byte-exact against the wire |
| slice salvage and first-slice synthesis | **done** — measured against a real decoder |
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

### Salvaging a picture that lost its first slice

Slice 0 carries the picture-level parameters the rest depend on, so losing it
is categorically worse than losing any other slice. The repair is to replay the
previous picture's first slice with its `frame_num` and POC retargeted to this
picture — both are fixed-width fields, so it is an in-place patch with no bit
shifting and no CABAC re-alignment.

Measured by dropping slice 0 from every third picture in the camera capture and
decoding both versions against the intact stream:

```
                        mean PSNR (luma)
no repair                    49.90 dB
first slice synthesised      51.96 dB     +2.06 dB
```

134 pictures improve, mean +1.78 dB and up to +3.93 dB. Twelve come out ~0.2 dB
worse; they all follow a keyframe that lost its own first slice, so what they
show is the decoder concealing that, not a bad stand-in. That was worth checking
rather than assuming: the obvious precaution — discarding the stand-in whenever
an IRAP goes past, on the grounds that pre-IDR content is stale — makes things
*worse* (+1.54 dB instead of +2.06 dB) and does not remove a single one of the
twelve. The code keeps the stand-in across IRAPs because the measurement said so.

Two constraints the code will not cross. A stand-in is only used when its NAL
type matches the picture it is standing in for, because replaying an IDR into an
inter picture tells the decoder to reset. And the picture's identity is read
from a slice that *survived* — every slice of a picture carries the same
`frame_num` and POC, so the one that was lost can be described by the ones that
were not.

What to do with a slice merely cut short by loss is a policy bit, not a
constant: forwarding it measured better on Intel (0.49% dirty pixels against
0.90%) and worse on Rockchip (30.80% against 25.47%). It is a parameter here.

## What building the receiver found in the sender

A receiver that checks its input is a test rig for whatever is transmitting.
Two faults turned up this way, neither visible from the sending side.

### The marker bit fired on every slice

RFC 6184 §5.1 puts the RTP marker on the last packet of an *access unit*.
Majestic set it on the last packet of every *NAL*. Measured on the wire from an
SSC30KQ at 2560×1920 with eight slices per picture:

```
markers 1062   pictures 131   markers not at a picture boundary: 931
```

A receiver that believes the marker therefore sees each slice as a whole
picture. That is a plausible route to "the hardware decoder can't handle
slices" — the decoder is handed eight one-slice pictures and does exactly what
it was told. Fixed in majestic (`sstar: mark the real end of an access unit`),
after which the same measurement gives 171 markers for 171 pictures, none
misplaced.

The depacketiser here does not simply trust the fix. The marker has to earn
trust — timestamps decide the boundary until the marker has agreed with them
twice running, and one disagreement disables it for the rest of the stream.

### Protection was inverted: the keyframe is the least protected part

The sender emits one repair packet per protection group and aligns groups to
slices, so the code rate follows slice size rather than importance:

| | groups | mean group | overhead | recovered at 1-in-5 |
|---|---|---|---|---|
| inter frames | 1300 | 1.4 packets | ~71% | effectively all |
| keyframes | 99 | 16.5 packets (max 26) | ~6% | almost none |

The consequence shows up per picture rather than per packet. At 1 in 5, 154 of
163 pictures arrive intact — and the nine that do not are **exactly the nine
keyframes**, each losing ~37 packets. Losing a keyframe costs the GOP; losing a
P-slice costs one band of one picture that can be concealed. The protection is
the wrong way round, and 40% of the link's overhead is being spent making the
cheap case cheaper.

This is not an argument for more FEC. It is an argument for bounding the group
size so the code rate is uniform, and then applying unequal protection
deliberately.

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

The slice surgery has one test that carries most of the weight: rewriting a
slice's `frame_num` and POC to the values it already holds must give back the
original bytes exactly. It runs the entire path — emulation prevention stripped
and restored, every `ue(v)` walked to find the offsets, the patch applied — so a
mistake anywhere shows up as a byte difference. `make check` runs it over all
1303 slices in the capture, not just the two the unit test embeds.

## Layout

```
include/salvage/flexfec.h   FlexFEC recovery API
include/salvage/depay.h     depacketisation API
include/salvage/salvage.h   slice salvage API
src/flexfec.c               recovery core
src/depay.c                 RFC 6184 / 7798 depacketisation, Annex-B output
src/bits.h  src/bits.c      bit reader, emulation prevention, bit patching
src/retarget.h  .c          SPS/PPS parsing and slice-header offsets
src/salvage.c               first-slice synthesis and truncated-slice policy
tests/flexfec.c             unit tests (repair packets built from the RFC)
tests/depay.c               unit tests (mostly about damage)
tests/salvage.c             unit tests, on real parameter sets and slices
tests/streamdata.h          those parameter sets and slices, from the capture
tests/camera-fec.pcap       real capture: SSC30KQ, H.264, FlexFEC enabled
tools/salvage-pcap.c        capture replay with loss injection
```

The depacketiser is checked the same way as the recovery core: replaying the
capture and reconciling every byte against the wire. 1321 NAL units, 3459260
payload bytes plus 5284 bytes of start codes, and an output file of exactly
3464544 bytes — so nothing is dropped, duplicated or spliced. The result
decodes as 163 frames at 2560×1920.

One picture in that capture decodes with an error, and it is worth saying why,
because the obvious conclusion is wrong. Picture 162 is missing the slice at
`first_mb_in_slice=15360` while the slice after it is present, and there is no
RTP sequence gap anywhere in the capture. The sender skipped a NAL; the network
did not lose one. Damage that arrives without a hole in the sequence numbers is
invisible to the FEC layer, which is why the salvage stage checks that slice
coverage is contiguous rather than trusting packet accounting.

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
