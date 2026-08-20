#ifndef SALVAGE_FLEXFEC_H
#define SALVAGE_FLEXFEC_H

/* FlexFEC (RFC 8627) receiver: rebuild media packets lost in transit from the
 * repair flow.
 *
 * Holds a sliding window of media and repair packets. A repair packet whose
 * protection group is missing exactly one member can rebuild that member by
 * XOR; recovery is re-run until it stops making progress, because a packet
 * recovered by one repair packet can be the last one another was waiting for.
 *
 * The window is what bounds latency: a packet is only worth waiting for until
 * its frame's display deadline, so the caller sizes the window and everything
 * older is released whether recovered or not.
 *
 * Media and repair arrive interleaved on the same RTP session, distinguished
 * by SSRC and payload type (RFC 8627 §5.2), so both go in through the same
 * call and the window sorts them out.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One MTU with margin. Packets larger than this cannot be protected and are
 * passed through untouched. */
#define SALVAGE_MAX_PACKET 1600

/* Sequence numbers wrap at 16 bits; the window must be far shorter than that
 * for "is this packet older than the window" to stay unambiguous. */
#define SALVAGE_WINDOW 1024

typedef struct SalvageFec SalvageFec;

typedef struct {
    uint64_t media_in;      /* media packets handed to us */
    uint64_t repair_in;     /* repair packets handed to us */
    uint64_t lost;          /* gaps observed in the media sequence */
    uint64_t recovered;     /* gaps filled from the repair flow */
    uint64_t unrecovered;   /* gaps that left the window still missing */
    uint64_t out;           /* packets released to the caller */
} SalvageFecStats;

/* Called for each media packet leaving the window, in sequence order.
 * `recovered` marks a packet that was rebuilt rather than received. */
typedef void (*SalvageFecOutCb)(
    const uint8_t *pkt, size_t len, bool recovered, void *ctx);

SalvageFec *salvage_fec_new(
    uint8_t media_pt, uint8_t fec_pt, unsigned window_packets,
    SalvageFecOutCb cb, void *ctx);

void salvage_fec_free(SalvageFec *self);

/* Feed one RTP packet, media or repair; anything else is ignored. */
void salvage_fec_input(SalvageFec *self, const uint8_t *pkt, size_t len);

/* Release everything still held, e.g. at end of stream. */
void salvage_fec_flush(SalvageFec *self);

void salvage_fec_stats(const SalvageFec *self, SalvageFecStats *out);

#endif /* SALVAGE_FLEXFEC_H */
