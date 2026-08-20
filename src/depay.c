#include <salvage/depay.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define RTP_HDR 12
#define MAX_NALS 512
#define DEFAULT_AU_BYTES (1u << 20)
#define MARKER_TRUST_MIN 2

struct SalvageDepay {
    SalvageCodec codec;
    SalvageAuCb cb;
    void *ctx;

    uint8_t *buf;
    size_t cap, len;
    SalvageNal nals[MAX_NALS];
    size_t nal_count;

    bool have_ts;
    uint32_t ts;
    bool have_seq;
    uint16_t expect_seq;

    /* The marker bit is meant to end an access unit (RFC 6184 §5.1), and
     * trusting it lets an AU be emitted without waiting for the next packet.
     * But senders get it wrong — the majestic/smolrtsp sender sets it at the
     * end of every NAL, which on a sliced stream means every slice looks like
     * its own picture. So the marker has to earn trust: stay on timestamps
     * until it has been right MARKER_TRUST_MIN times running, and never
     * believe it again once it lies. Costs one packet of latency at startup
     * and none afterwards, and cannot mis-split even the first frame. */
    bool prev_marker;
    int marker_trust;

    /* Fragmentation-unit state. `cur` is the NAL being assembled; `orphan`
     * means we are stepping over fragments whose NAL we cannot use, either
     * because its leading fragment was lost or because we already truncated
     * it. */
    bool in_fu;
    bool orphan;
    SalvageNal *cur;

    unsigned lost, recovered, dropped;
    bool overflow;
};

SalvageDepay *salvage_depay_new(
    SalvageCodec codec, size_t max_au_bytes, SalvageAuCb cb, void *ctx) {
    SalvageDepay *self = calloc(1, sizeof *self);
    if (self == NULL) {
        return NULL;
    }
    self->cap = max_au_bytes > 0 ? max_au_bytes : DEFAULT_AU_BYTES;
    self->buf = malloc(self->cap);
    if (self->buf == NULL) {
        free(self);
        return NULL;
    }
    self->codec = codec;
    self->cb = cb;
    self->ctx = ctx;
    return self;
}

void salvage_depay_free(SalvageDepay *self) {
    if (self != NULL) {
        free(self->buf);
        free(self);
    }
}

/* --- access-unit assembly ---------------------------------------------- */

static void reset_au(SalvageDepay *self) {
    self->len = 0;
    self->nal_count = 0;
    self->in_fu = false;
    self->orphan = false;
    self->cur = NULL;
    self->lost = 0;
    self->recovered = 0;
    self->dropped = 0;
    self->overflow = false;
}

static void flush_au(SalvageDepay *self) {
    if (self->nal_count == 0) {
        reset_au(self);
        return;
    }

    SalvageAu au = {
        .data = self->buf,
        .len = self->len,
        .ts = self->ts,
        .nals = self->nals,
        .nal_count = self->nal_count,
        .complete = self->lost == 0 && self->dropped == 0 && !self->overflow,
        .lost_packets = self->lost,
        .recovered_packets = self->recovered,
        .dropped_nals = self->dropped,
    };
    for (size_t i = 0; i < self->nal_count; i++) {
        if (!self->nals[i].complete) {
            au.complete = false;
        }
        /* Not conditioned on completeness: the slice header sits at the very
         * front, so even a slice cut short by loss still delivers the
         * picture-level parameters the other slices depend on. */
        if (self->nals[i].is_slice && self->nals[i].first_slice) {
            au.has_first_slice = true;
        }
    }

    if (self->cb != NULL) {
        self->cb(&au, self->ctx);
    }
    reset_au(self);
}

/* --- NAL construction --------------------------------------------------- */

static SalvageNal *begin_nal(SalvageDepay *self, uint8_t type, bool is_slice) {
    if (self->nal_count >= MAX_NALS || self->len + 4 > self->cap) {
        self->overflow = true;
        return NULL;
    }
    SalvageNal *n = &self->nals[self->nal_count++];
    memset(n, 0, sizeof *n);
    n->offset = self->len;
    self->buf[self->len++] = 0;
    self->buf[self->len++] = 0;
    self->buf[self->len++] = 0;
    self->buf[self->len++] = 1;
    n->len = 4;
    n->type = type;
    n->is_slice = is_slice;
    n->complete = true;
    return n;
}

static void append(
    SalvageDepay *self, SalvageNal *n, const uint8_t *p, size_t len) {
    if (self->len + len > self->cap) {
        self->overflow = true;
        n->complete = false;
        return;
    }
    memcpy(self->buf + self->len, p, len);
    self->len += len;
    n->len += len;
}

/* first_mb_in_slice (H.264) and first_slice_segment_in_pic_flag (H.265) both
 * sit at the head of the slice header, and ue(v) codes zero as a single 1 bit,
 * so in both codecs "this is the picture's first slice" is the top bit of the
 * first byte after the NAL header. */
static void mark_first_slice(SalvageDepay *self, SalvageNal *n) {
    const size_t hdr = self->codec == SALVAGE_H265 ? 2 : 1;
    if (!n->is_slice || n->len < 4 + hdr + 1) {
        return;
    }
    n->first_slice = (self->buf[n->offset + 4 + hdr] & 0x80u) != 0;
}

static void emit_single(SalvageDepay *self, const uint8_t *p, size_t len) {
    uint8_t type;
    bool is_slice;
    if (self->codec == SALVAGE_H265) {
        if (len < 2) {
            return;
        }
        type = (uint8_t)((p[0] >> 1) & 0x3fu);
        is_slice = type <= 31;
    } else {
        if (len < 1) {
            return;
        }
        type = (uint8_t)(p[0] & 0x1fu);
        is_slice = type == 1 || type == 5;
    }
    SalvageNal *n = begin_nal(self, type, is_slice);
    if (n == NULL) {
        return;
    }
    append(self, n, p, len);
    mark_first_slice(self, n);
}

/* An aggregation packet is a run of 16-bit-length-prefixed NAL units. */
static void emit_aggregate(SalvageDepay *self, const uint8_t *p, size_t len) {
    size_t off = 0;
    while (off + 2 <= len) {
        const size_t sz = (size_t)((p[off] << 8) | p[off + 1]);
        off += 2;
        if (sz == 0 || off + sz > len) {
            break;
        }
        emit_single(self, p + off, sz);
        off += sz;
    }
}

/* A fragment that continues a NAL we do not have the head of is unusable: the
 * slice header is what a decoder needs to place the data, and it went with the
 * leading fragment. Count the NAL once and step over the rest. */
static void skip_orphan(SalvageDepay *self) {
    if (!self->orphan) {
        self->orphan = true;
        self->dropped++;
    }
}

static void fragment_start(
    SalvageDepay *self, const uint8_t *hdr, size_t hdr_len, uint8_t type,
    bool is_slice, const uint8_t *body, size_t body_len) {
    SalvageNal *n = begin_nal(self, type, is_slice);
    if (n == NULL) {
        self->in_fu = false;
        self->orphan = true;
        return;
    }
    append(self, n, hdr, hdr_len);
    append(self, n, body, body_len);
    mark_first_slice(self, n);
    self->cur = n;
    self->in_fu = true;
    self->orphan = false;
}

/* --- RFC 6184 ----------------------------------------------------------- */

static void parse_h264(SalvageDepay *self, const uint8_t *p, size_t len) {
    const uint8_t type = (uint8_t)(p[0] & 0x1fu);

    if (type == 28 || type == 29) { /* FU-A / FU-B */
        const size_t skip = type == 28 ? 2 : 4; /* FU-B carries a 2-byte DON */
        if (len < skip + 1) {
            return;
        }
        const uint8_t fu = p[1];
        const uint8_t orig = (uint8_t)(fu & 0x1fu);

        if (fu & 0x80u) { /* start */
            const uint8_t hdr = (uint8_t)((p[0] & 0xe0u) | orig);
            fragment_start(
                self, &hdr, 1, orig, orig == 1 || orig == 5, p + skip,
                len - skip);
        } else if (self->in_fu) {
            append(self, self->cur, p + skip, len - skip);
        } else {
            skip_orphan(self);
        }

        if (fu & 0x40u) { /* end */
            self->in_fu = false;
            self->orphan = false;
        }
    } else if (type == 24) { /* STAP-A */
        emit_aggregate(self, p + 1, len - 1);
    } else if (type == 25) { /* STAP-B: one DON, then the same layout */
        if (len > 3) {
            emit_aggregate(self, p + 3, len - 3);
        }
    } else if (type >= 1 && type <= 23) {
        emit_single(self, p, len);
    }
    /* 26/27 are MTAPs, which no encoder in this pipeline emits. */
}

/* --- RFC 7798 ----------------------------------------------------------- */

static void parse_h265(SalvageDepay *self, const uint8_t *p, size_t len) {
    if (len < 2) {
        return;
    }
    const uint8_t type = (uint8_t)((p[0] >> 1) & 0x3fu);

    if (type == 49) { /* fragmentation unit */
        if (len < 4) {
            return;
        }
        const uint8_t fu = p[2];
        const uint8_t orig = (uint8_t)(fu & 0x3fu);

        if (fu & 0x80u) { /* start */
            /* Rebuild the two-byte NAL header, keeping the forbidden bit,
             * layer id and temporal id from the payload header and putting the
             * fragmented unit's own type back into place. */
            const uint8_t hdr[2] = {
                (uint8_t)((p[0] & 0x81u) | (uint8_t)(orig << 1)),
                p[1],
            };
            fragment_start(self, hdr, 2, orig, orig <= 31, p + 3, len - 3);
        } else if (self->in_fu) {
            append(self, self->cur, p + 3, len - 3);
        } else {
            skip_orphan(self);
        }

        if (fu & 0x40u) { /* end */
            self->in_fu = false;
            self->orphan = false;
        }
    } else if (type == 48) { /* aggregation packet */
        emit_aggregate(self, p + 2, len - 2);
    } else if (type <= 47) {
        emit_single(self, p, len);
    }
}

/* --- input -------------------------------------------------------------- */

void salvage_depay_input(
    SalvageDepay *self, const uint8_t *pkt, size_t len, bool recovered) {
    assert(self && pkt);

    if (len <= RTP_HDR || (pkt[0] >> 6) != 2) {
        return;
    }
    /* A CSRC list or a header extension pushes the payload back; ignoring
     * either would feed their bytes to the NAL parser as if they were video. */
    size_t hdr = RTP_HDR + (size_t)(pkt[0] & 0x0fu) * 4;
    if (pkt[0] & 0x10u) {
        if (len < hdr + 4) {
            return;
        }
        hdr += 4 + (size_t)(((pkt[hdr + 2] << 8) | pkt[hdr + 3])) * 4;
    }
    if (len <= hdr) {
        return;
    }

    const uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    const uint32_t ts = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                        ((uint32_t)pkt[6] << 8) | (uint32_t)pkt[7];
    const bool marker = (pkt[1] & 0x80u) != 0;

    /* Judge the previous packet's marker now that the next timestamp is
     * visible: a marker followed by the same timestamp was not an AU end. */
    if (self->have_ts && self->prev_marker) {
        if (ts != self->ts) {
            if (self->marker_trust >= 0 && self->marker_trust < MARKER_TRUST_MIN) {
                self->marker_trust++;
            }
        } else {
            self->marker_trust = -1; /* latched off for the rest of the stream */
        }
    }
    self->prev_marker = marker;

    if (self->have_seq) {
        const uint16_t ahead = (uint16_t)(seq - self->expect_seq);
        if (ahead > 0x8000u) {
            return; /* behind what we have already consumed: a stale duplicate */
        }
        if (ahead > 0) {
            self->lost += ahead;
            /* Splicing the fragments either side of a hole would hand the
             * decoder bytes that were never adjacent, which is worse than a
             * short slice: it looks like valid data. Truncate instead. */
            if (self->in_fu) {
                self->cur->complete = false;
                self->in_fu = false;
                self->orphan = true;
            } else {
                self->orphan = false; /* a NAL's leading fragment may be gone */
            }
        }
    }
    self->have_seq = true;
    self->expect_seq = (uint16_t)(seq + 1);

    if (self->have_ts && ts != self->ts) {
        flush_au(self);
    }
    self->ts = ts;
    self->have_ts = true;

    if (recovered) {
        self->recovered++;
    }

    if (self->codec == SALVAGE_H265) {
        parse_h265(self, pkt + hdr, len - hdr);
    } else {
        parse_h264(self, pkt + hdr, len - hdr);
    }

    if (marker && self->marker_trust >= MARKER_TRUST_MIN) {
        flush_au(self);
    }
}

void salvage_depay_flush(SalvageDepay *self) {
    assert(self);
    flush_au(self);
}
