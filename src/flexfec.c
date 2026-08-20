#include <salvage/flexfec.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define RTP_HDR 12
/* Widest RFC 8627 mask, so the furthest a group can reach from its base. */
#define MAX_SPAN 110
/* Repair packets held while their group waits for stragglers. */
#define MAX_REPAIR 256

typedef struct {
    uint8_t data[SALVAGE_MAX_PACKET];
    uint16_t len;
    uint16_t seq;
    bool present;
    bool recovered;
} MediaSlot;

typedef struct {
    bool used;
    uint16_t sn_base;
    /* Bit i set means sn_base + i is a member. */
    uint8_t mask[14];
    uint16_t span;
    uint8_t bits;   /* XOR of P/X/CC (low 6) and M (bit 6) */
    uint8_t pt;
    uint16_t length;
    uint32_t ts;
    uint8_t payload[SALVAGE_MAX_PACKET];
    uint16_t payload_len;
} Repair;

struct SalvageFec {
    uint8_t media_pt, fec_pt;
    unsigned window;
    SalvageFecOutCb cb;
    void *ctx;

    MediaSlot *slots;      /* ring indexed by seq % window */
    bool started;
    uint16_t next_out;     /* oldest sequence number not yet released */
    uint16_t highest;      /* highest sequence number seen */

    /* FEC does not carry the media SSRC, so a rebuilt packet has to get it
     * from somewhere. It is a property of the session, not of the protection
     * group: learning it once is both simpler and more robust than borrowing
     * it from a surviving group member, which does not exist when the group
     * has one member and that member is the one we lost. */
    uint32_t media_ssrc;
    bool have_media_ssrc;

    Repair repair[MAX_REPAIR];
    unsigned repair_head;

    SalvageFecStats stats;
};

static bool mask_get(const uint8_t mask[14], uint16_t bit) {
    return (mask[bit / 8] & (0x80u >> (bit % 8))) != 0;
}

/* Distance forward from `base` to `seq`, modulo the 16-bit sequence space. */
static uint16_t fwd(uint16_t seq, uint16_t base) {
    return (uint16_t)(seq - base);
}

SalvageFec *salvage_fec_new(
    uint8_t media_pt, uint8_t fec_pt, unsigned window_packets,
    SalvageFecOutCb cb, void *ctx) {
    if (window_packets == 0 || window_packets > SALVAGE_WINDOW) {
        window_packets = SALVAGE_WINDOW;
    }

    SalvageFec *self = calloc(1, sizeof *self);
    if (self == NULL) {
        return NULL;
    }
    self->slots = calloc(window_packets, sizeof *self->slots);
    if (self->slots == NULL) {
        free(self);
        return NULL;
    }

    self->media_pt = media_pt;
    self->fec_pt = fec_pt;
    self->window = window_packets;
    self->cb = cb;
    self->ctx = ctx;
    return self;
}

void salvage_fec_free(SalvageFec *self) {
    if (self != NULL) {
        free(self->slots);
        free(self);
    }
}

void salvage_fec_stats(const SalvageFec *self, SalvageFecStats *out) {
    assert(self && out);
    *out = self->stats;
}

static MediaSlot *slot_of(SalvageFec *self, uint16_t seq) {
    return &self->slots[seq % self->window];
}

static void release(SalvageFec *self, uint16_t seq) {
    MediaSlot *s = slot_of(self, seq);
    if (s->present && s->seq == seq) {
        if (s->recovered) {
            self->stats.recovered++;
        }
        self->stats.out++;
        if (self->cb != NULL) {
            self->cb(s->data, s->len, s->recovered, self->ctx);
        }
    } else {
        self->stats.unrecovered++;
    }
    s->present = false;
    s->recovered = false;
}

/* Try every held repair packet; rebuild any group missing exactly one member.
 * Repeats while it keeps making progress, since one recovery can complete
 * another group. */
static void run_recovery(SalvageFec *self) {
    bool progress = true;
    while (progress) {
        progress = false;

        for (unsigned r = 0; r < MAX_REPAIR; r++) {
            Repair *rep = &self->repair[r];
            if (!rep->used) {
                continue;
            }

            unsigned missing = 0;
            uint16_t victim = 0;
            bool aged = false, early = false;

            for (uint16_t i = 0; i < rep->span; i++) {
                if (!mask_get(rep->mask, i)) {
                    continue;
                }
                const uint16_t seq = (uint16_t)(rep->sn_base + i);
                /* A member behind the window was released long ago and will
                 * never come back, so the group is dead. A member ahead of the
                 * window is merely early: its slot is still occupied by a
                 * packet awaiting release, and once that happens the group
                 * becomes workable. Treating the two alike throws away repair
                 * for the newest losses, which are exactly the ones at the far
                 * edge of the window. */
                const uint16_t back = fwd(self->next_out, seq);
                if (back > 0 && back < self->window) {
                    aged = true;
                    break;
                }
                if (fwd(seq, self->next_out) >= self->window) {
                    early = true;
                    break;
                }
                const MediaSlot *s = slot_of(self, seq);
                if (!(s->present && s->seq == seq)) {
                    missing++;
                    victim = seq;
                    if (missing > 1) {
                        break;
                    }
                }
            }

            if (aged) {
                rep->used = false;
                continue;
            }
            if (early) {
                continue; /* keep it; the window has yet to reach the group */
            }
            if (missing == 0) {
                rep->used = false; /* nothing left for it to do */
                continue;
            }
            if (missing > 1) {
                continue; /* XOR recovers one loss per group */
            }

            /* Undo the XOR of every present member; what remains is the
             * missing packet. */
            uint8_t bits = rep->bits;
            uint8_t pt = rep->pt;
            uint16_t length = rep->length;
            uint32_t ts = rep->ts;
            uint8_t body[SALVAGE_MAX_PACKET];
            memset(body, 0, sizeof body);
            memcpy(body, rep->payload, rep->payload_len);

            for (uint16_t i = 0; i < rep->span; i++) {
                if (!mask_get(rep->mask, i)) {
                    continue;
                }
                const uint16_t seq = (uint16_t)(rep->sn_base + i);
                if (seq == victim) {
                    continue;
                }
                const MediaSlot *s = slot_of(self, seq);
                const uint16_t plen = (uint16_t)(s->len - RTP_HDR);
                bits ^= (uint8_t)((s->data[0] & 0x3fu) |
                                  ((s->data[1] & 0x80u) >> 1));
                pt ^= (uint8_t)(s->data[1] & 0x7fu);
                length ^= plen;
                ts ^= ((uint32_t)s->data[4] << 24) |
                      ((uint32_t)s->data[5] << 16) |
                      ((uint32_t)s->data[6] << 8) | (uint32_t)s->data[7];
                for (uint16_t b = 0; b < plen; b++) {
                    body[b] ^= s->data[RTP_HDR + b];
                }
            }

            if (!self->have_media_ssrc ||
                length > SALVAGE_MAX_PACKET - RTP_HDR) {
                rep->used = false;
                continue;
            }

            MediaSlot *dst = slot_of(self, victim);
            dst->data[0] = (uint8_t)(0x80u | (bits & 0x3fu));
            dst->data[1] = (uint8_t)(((bits & 0x40u) << 1) | (pt & 0x7fu));
            dst->data[2] = (uint8_t)(victim >> 8);
            dst->data[3] = (uint8_t)victim;
            dst->data[4] = (uint8_t)(ts >> 24);
            dst->data[5] = (uint8_t)(ts >> 16);
            dst->data[6] = (uint8_t)(ts >> 8);
            dst->data[7] = (uint8_t)ts;
            memcpy(dst->data + 8, &self->media_ssrc, 4); /* network order */
            memcpy(dst->data + RTP_HDR, body, length);
            dst->len = (uint16_t)(RTP_HDR + length);
            dst->seq = victim;
            dst->present = true;
            dst->recovered = true;

            /* A rebuilt packet counts as seen: the window is bounded by the
             * newest sequence number known to exist, and recovery is one of
             * the ways we learn one exists. Without this a packet recovered
             * beyond `highest` is held for ever and never released. */
            if (fwd(victim, self->highest) < self->window) {
                self->highest = victim;
            }

            rep->used = false;
            progress = true;
        }
    }
}

/* Release everything that has fallen out of the window. */
static void advance(SalvageFec *self) {
    /* Last chance: a packet about to fall out of the window may still be
     * rebuildable, and once released it is gone for good. */
    if (fwd(self->highest, self->next_out) >= self->window) {
        run_recovery(self);
    }
    while (fwd(self->highest, self->next_out) >= self->window) {
        release(self, self->next_out);
        self->next_out++;
    }
}

static void store_repair(SalvageFec *self, const uint8_t *p, size_t len) {
    const uint8_t *f = p + RTP_HDR;
    const size_t flen = len - RTP_HDR;
    if (flen < 12) {
        return;
    }
    /* R and F must both be zero: this decoder implements the flexible-mask
     * form only, which is what the sender emits. */
    if ((f[0] & 0xc0u) != 0) {
        return;
    }

    Repair *rep = &self->repair[self->repair_head];
    self->repair_head = (self->repair_head + 1) % MAX_REPAIR;
    memset(rep, 0, sizeof *rep);

    rep->bits = (uint8_t)((f[0] & 0x3fu) | ((f[1] & 0x80u) >> 1));
    rep->pt = (uint8_t)(f[1] & 0x7fu);
    rep->length = (uint16_t)((f[2] << 8) | f[3]);
    /* Shift as uint32_t: a uint8_t promotes to int, and byte 4 with its top
     * bit set overflows a 32-bit int when shifted 24 places. */
    rep->ts = ((uint32_t)f[4] << 24) | ((uint32_t)f[5] << 16) |
              ((uint32_t)f[6] << 8) | (uint32_t)f[7];
    rep->sn_base = (uint16_t)((f[8] << 8) | f[9]);

    size_t off = 10;
    uint16_t w = (uint16_t)((f[off] << 8) | f[off + 1]);
    bool k = (w & 0x8000u) != 0;
    for (uint16_t i = 0; i < 15; i++) {
        if (w & (1u << (14 - i))) {
            rep->mask[i / 8] |= (uint8_t)(0x80u >> (i % 8));
            rep->span = (uint16_t)(i + 1);
        }
    }
    off += 2;

    if (k) {
        if (flen < off + 4) {
            return;
        }
        const uint32_t w2 = ((uint32_t)f[off] << 24) |
                            ((uint32_t)f[off + 1] << 16) |
                            ((uint32_t)f[off + 2] << 8) | (uint32_t)f[off + 3];
        k = (w2 & 0x80000000u) != 0;
        for (uint16_t i = 0; i < 31; i++) {
            if (w2 & (1u << (30 - i))) {
                const uint16_t bit = (uint16_t)(15 + i);
                rep->mask[bit / 8] |= (uint8_t)(0x80u >> (bit % 8));
                rep->span = (uint16_t)(bit + 1);
            }
        }
        off += 4;

        if (k) {
            if (flen < off + 8) {
                return;
            }
            uint64_t w3 = 0;
            for (int i = 0; i < 8; i++) {
                w3 = (w3 << 8) | f[off + i];
            }
            for (uint16_t i = 0; i < 64; i++) {
                if (w3 & (1ull << (63 - i))) {
                    const uint16_t bit = (uint16_t)(46 + i);
                    rep->mask[bit / 8] |= (uint8_t)(0x80u >> (bit % 8));
                    rep->span = (uint16_t)(bit + 1);
                }
            }
            off += 8;
        }
    }

    if (rep->span == 0 || rep->span > MAX_SPAN || off > flen) {
        return;
    }
    rep->payload_len = (uint16_t)(flen - off);
    if (rep->payload_len > SALVAGE_MAX_PACKET) {
        return;
    }
    memcpy(rep->payload, f + off, rep->payload_len);
    rep->used = true;

    /* A repair packet names sequence numbers that may never have arrived — and
     * the last member of a group is very often the one that was lost. Counting
     * the group's top as seen is what lets the window slide far enough to hold
     * it; without this the most recent loss is always one slot out of reach. */
    if (self->started) {
        const uint16_t top = (uint16_t)(rep->sn_base + rep->span - 1);
        if (fwd(top, self->highest) < self->window) {
            self->highest = top;
        }
    }
}

void salvage_fec_input(SalvageFec *self, const uint8_t *pkt, size_t len) {
    assert(self && pkt);

    if (len < RTP_HDR || len > SALVAGE_MAX_PACKET || (pkt[0] >> 6) != 2) {
        return;
    }
    const uint8_t pt = (uint8_t)(pkt[1] & 0x7fu);
    const uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);

    if (pt == self->fec_pt) {
        self->stats.repair_in++;
        store_repair(self, pkt, len);
        advance(self);
        run_recovery(self);
        return;
    }
    if (pt != self->media_pt) {
        return;
    }

    self->stats.media_in++;

    if (!self->have_media_ssrc) {
        memcpy(&self->media_ssrc, pkt + 8, 4); /* kept in network order */
        self->have_media_ssrc = true;
    }

    if (!self->started) {
        self->started = true;
        self->next_out = seq;
        self->highest = seq;
    }

    /* Behind the window: we already gave up on this sequence number and
     * released its slot, so accepting it now would corrupt whatever lives
     * there. `behind` is small only for genuinely late packets; for packets
     * ahead of us it wraps to something enormous. */
    const uint16_t behind = fwd(self->next_out, seq);
    if (behind > 0 && behind < self->window) {
        return;
    }

    /* Advance BEFORE storing. Slots are indexed modulo the window, so a
     * packet `window` ahead of next_out aliases next_out's own slot — storing
     * first would overwrite the packet the window is about to release, and it
     * would be reported lost while sitting in the buffer. */
    if (fwd(seq, self->highest) < self->window) {
        self->highest = seq;
    }
    advance(self);

    if (fwd(seq, self->next_out) >= self->window) {
        return; /* still unreachable after advancing */
    }

    MediaSlot *s = slot_of(self, seq);
    s->seq = seq;
    s->len = (uint16_t)len;
    s->present = true;
    s->recovered = false;
    memcpy(s->data, pkt, len);

    run_recovery(self);
}

void salvage_fec_flush(SalvageFec *self) {
    assert(self);
    if (!self->started) {
        return;
    }
    run_recovery(self);
    while (fwd(self->highest, self->next_out) < self->window) {
        release(self, self->next_out);
        if (self->next_out == self->highest) {
            break;
        }
        self->next_out++;
    }
}
