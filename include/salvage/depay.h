#ifndef SALVAGE_DEPAY_H
#define SALVAGE_DEPAY_H

/* RTP depacketisation for H.264 (RFC 6184) and H.265 (RFC 7798), assembling
 * access units in Annex-B form.
 *
 * The difference from an ordinary depacketiser is what it does with damage.
 * A normal one drops anything incomplete, because a player that shows half a
 * frame looks broken. Here the incomplete frame is the whole point: a picture
 * missing one slice is still most of a picture, and on a link that loses
 * packets it is the difference between a usable video feed and a frozen one.
 *
 * So nothing is silently discarded. Every access unit is delivered with a
 * table saying which NAL units are whole, which were cut short by loss, and
 * which slice of the picture each one carries — enough for the caller to
 * decide what to hand the decoder, which is a decision that has to be made per
 * platform rather than in a library: forwarding a truncated slice was
 * measured to help on Intel and hurt on Rockchip.
 *
 * Feed it the packets released by the FlexFEC layer, in sequence order. Gaps
 * are inferred from the sequence numbers, so no separate loss signal is needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SALVAGE_H264,
    SALVAGE_H265,
} SalvageCodec;

typedef struct {
    size_t offset;     /* into SalvageAu::data, at the start code */
    size_t len;        /* including the four-byte start code */
    uint8_t type;      /* NAL unit type, codec-specific numbering */
    bool complete;     /* false: fragments were lost from the tail */
    bool is_slice;     /* carries coded picture data */
    bool first_slice;  /* first slice of the picture, so it carries the
                        * picture-level parameters the rest depend on */
} SalvageNal;

typedef struct {
    const uint8_t *data;      /* Annex-B: start code + NAL, concatenated */
    size_t len;
    uint32_t ts;              /* RTP timestamp */
    const SalvageNal *nals;
    size_t nal_count;
    bool complete;            /* no loss anywhere in this access unit */
    bool has_first_slice;     /* a slice with first_mb_in_slice == 0 is present */
    unsigned lost_packets;    /* sequence numbers missing within this AU */
    unsigned recovered_packets; /* packets that FlexFEC rebuilt */
    unsigned dropped_nals;    /* NALs discarded: their leading fragment was
                               * lost, so no slice header survived */
} SalvageAu;

typedef void (*SalvageAuCb)(const SalvageAu *au, void *ctx);

typedef struct SalvageDepay SalvageDepay;

/* `max_au_bytes` bounds one access unit; 0 selects a default sized for 4K
 * keyframes. An access unit that would exceed it is truncated and marked
 * incomplete rather than growing without limit. */
SalvageDepay *salvage_depay_new(
    SalvageCodec codec, size_t max_au_bytes, SalvageAuCb cb, void *ctx);

void salvage_depay_free(SalvageDepay *self);

/* Feed one RTP media packet. `recovered` marks one rebuilt by FlexFEC. */
void salvage_depay_input(
    SalvageDepay *self, const uint8_t *pkt, size_t len, bool recovered);

/* Emit the access unit still being assembled, e.g. at end of stream. */
void salvage_depay_flush(SalvageDepay *self);

#endif /* SALVAGE_DEPAY_H */
