#include <salvage/salvage.h>

#include "bits.h"
#include "retarget.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define TEMPLATE_MAX (256u * 1024u)
#define DEFAULT_PICTURE_BYTES (1u << 20)

struct SalvageStage {
    SalvageCodec codec;
    SalvagePolicy policy;
    SalvagePicCb cb;
    void *ctx;

    SalvageSps sps;
    SalvagePps pps;

    uint8_t *out;      /* Annex-B picture being built */
    size_t out_cap, out_len;
    uint8_t *scratch;  /* RBSP working buffer */
    size_t scratch_cap;

    /* The stand-in: the RBSP of the most recent complete first slice, kept so
     * a later picture that loses its own can borrow it. Only inter slices are
     * kept — replaying an IDR would tell the decoder to reset, which is a much
     * bigger lie than a stale slice.
     *
     * It is deliberately NOT dropped when an IRAP goes past, though that looks
     * like the careful thing to do. Tried both ways over the camera capture
     * with slice 0 lost from every third picture: keeping it gives +2.06 dB of
     * luma against no repair, dropping it only +1.54 dB, and the handful of
     * frames that come out worse are identical either way — they follow a
     * keyframe that lost its own first slice, so what they are really showing
     * is the decoder's concealment of that, not a stale stand-in. */
    uint8_t template_rbsp[TEMPLATE_MAX];
    size_t template_len;
    uint8_t template_nal_type;
    bool have_template;

    SalvageStats stats;
};

SalvageStage *salvage_new(
    SalvageCodec codec, SalvagePolicy policy, size_t max_picture_bytes,
    SalvagePicCb cb, void *ctx) {
    SalvageStage *self = calloc(1, sizeof *self);
    if (self == NULL) {
        return NULL;
    }
    self->out_cap =
        max_picture_bytes > 0 ? max_picture_bytes : DEFAULT_PICTURE_BYTES;
    self->scratch_cap = self->out_cap;
    self->out = malloc(self->out_cap);
    self->scratch = malloc(self->scratch_cap);
    if (self->out == NULL || self->scratch == NULL) {
        free(self->out);
        free(self->scratch);
        free(self);
        return NULL;
    }
    self->codec = codec;
    self->policy = policy;
    self->cb = cb;
    self->ctx = ctx;
    return self;
}

void salvage_free(SalvageStage *self) {
    if (self != NULL) {
        free(self->out);
        free(self->scratch);
        free(self);
    }
}

void salvage_stats(const SalvageStage *self, SalvageStats *out) {
    assert(self && out);
    *out = self->stats;
}

/* --- helpers ------------------------------------------------------------ */

static bool is_irap(SalvageCodec codec, uint8_t nal_type) {
    return codec == SALVAGE_H265 ? (nal_type >= 16 && nal_type <= 23)
                                 : nal_type == 5;
}

static bool parse_slice(
    SalvageStage *self, const uint8_t *rbsp, size_t len,
    SalvageSliceHeader *out) {
    return self->codec == SALVAGE_H265
               ? salvage_slice_h265(rbsp, len, &self->sps, &self->pps, out)
               : salvage_slice_h264(rbsp, len, &self->sps, &self->pps, out);
}

/* Copy a NAL, already carrying its Annex-B start code, into the picture. */
static bool emit_raw(SalvageStage *self, const uint8_t *nal, size_t len) {
    if (self->out_len + len > self->out_cap) {
        return false;
    }
    memcpy(self->out + self->out_len, nal, len);
    self->out_len += len;
    return true;
}

static bool emit_rbsp(SalvageStage *self, const uint8_t *rbsp, size_t len) {
    if (self->out_len + 4 > self->out_cap) {
        return false;
    }
    self->out[self->out_len++] = 0;
    self->out[self->out_len++] = 0;
    self->out[self->out_len++] = 0;
    self->out[self->out_len++] = 1;
    const size_t n = rbsp_escape(
        rbsp, len, self->out + self->out_len, self->out_cap - self->out_len);
    if (n == 0) {
        self->out_len -= 4;
        return false;
    }
    self->out_len += n;
    return true;
}

/* Rebuild the stored first slice as if it belonged to this picture. */
static bool synthesise_first_slice(
    SalvageStage *self, const SalvageSliceHeader *victim) {
    if (!self->have_template || self->template_len > self->scratch_cap) {
        return false;
    }
    /* Replaying an IDR into an inter picture, or the reverse, changes what the
     * decoder does with the whole stream. Only stand in for like with like. */
    if (self->template_nal_type != victim->nal_type) {
        return false;
    }

    memcpy(self->scratch, self->template_rbsp, self->template_len);

    SalvageSliceHeader h;
    if (!parse_slice(self, self->scratch, self->template_len, &h)) {
        return false;
    }
    /* The stand-in must be a first slice, or it does not carry what was lost
     * and its slice_segment_address points at the wrong part of the picture. */
    if (self->codec == SALVAGE_H265 ? !h.first_slice : h.first_mb != 0) {
        return false;
    }
    if (victim->has_frame_num && h.has_frame_num) {
        bits_patch(
            self->scratch, h.frame_num_off, h.frame_num_bits,
            victim->frame_num);
    }
    if (victim->has_poc && h.has_poc) {
        bits_patch(self->scratch, h.poc_off, h.poc_bits, victim->poc_lsb);
    } else if (victim->has_poc != h.has_poc) {
        return false; /* one has a POC field and the other does not */
    }

    return emit_rbsp(self, self->scratch, self->template_len);
}

static void remember_template(
    SalvageStage *self, const uint8_t *rbsp, size_t len, uint8_t nal_type) {
    if (len == 0 || len > TEMPLATE_MAX || is_irap(self->codec, nal_type)) {
        return;
    }
    memcpy(self->template_rbsp, rbsp, len);
    self->template_len = len;
    self->template_nal_type = nal_type;
    self->have_template = true;
}

/* --- the stage ---------------------------------------------------------- */

void salvage_input(SalvageStage *self, const SalvageAu *au) {
    assert(self && au);

    self->stats.pictures_in++;
    if (!au->complete) {
        self->stats.pictures_damaged++;
    }
    self->out_len = 0;

    /* First pass: absorb parameter sets, and learn this picture's identity
     * from any slice that survived. Every slice of a picture carries the same
     * frame_num and POC, so the one we lost can be described by the ones we
     * still have — which is what makes a stand-in possible at all. */
    bool have_identity = false, have_first_slice = false;
    SalvageSliceHeader identity = {0};

    for (size_t i = 0; i < au->nal_count; i++) {
        const SalvageNal *n = &au->nals[i];
        const uint8_t *body = au->data + n->offset + 4;
        const size_t body_len = n->len - 4;
        const size_t rbsp_len =
            rbsp_unescape(body, body_len, self->scratch, self->scratch_cap);
        if (rbsp_len == 0) {
            continue;
        }

        if (self->codec == SALVAGE_H265) {
            if (n->type == 33) {
                salvage_sps_h265(self->scratch, rbsp_len, &self->sps);
            } else if (n->type == 34) {
                salvage_pps_h265(self->scratch, rbsp_len, &self->pps);
            }
        } else {
            if (n->type == 7) {
                salvage_sps_h264(self->scratch, rbsp_len, &self->sps);
            } else if (n->type == 8) {
                salvage_pps_h264(self->scratch, rbsp_len, &self->pps);
            }
        }

        if (!n->is_slice) {
            continue;
        }
        SalvageSliceHeader h;
        if (!parse_slice(self, self->scratch, rbsp_len, &h)) {
            continue;
        }
        const bool first =
            self->codec == SALVAGE_H265 ? h.first_slice : h.first_mb == 0;
        if (first) {
            have_first_slice = true;
            if (n->complete) {
                remember_template(self, self->scratch, rbsp_len, h.nal_type);
            }
        }
        if (!have_identity) {
            identity = h;
            have_identity = true;
        }
    }

    /* Second pass: build the picture. A synthesised first slice goes ahead of
     * everything, because slices are decoded in raster order and the stand-in
     * covers the top of the frame. */
    bool repaired = false;
    if (!have_first_slice && have_identity && self->policy.synthesise_first_slice) {
        if (synthesise_first_slice(self, &identity)) {
            repaired = true;
            self->stats.first_slice_synthesised++;
            self->stats.slices_out++;
        } else {
            self->stats.first_slice_unrepaired++;
        }
    } else if (!have_first_slice) {
        self->stats.first_slice_unrepaired++;
    }

    for (size_t i = 0; i < au->nal_count; i++) {
        const SalvageNal *n = &au->nals[i];
        if (n->is_slice && !n->complete) {
            if (self->policy.truncated == SALVAGE_TRUNCATED_DROP) {
                self->stats.truncated_dropped++;
                continue;
            }
            self->stats.truncated_forwarded++;
        }
        if (!emit_raw(self, au->data + n->offset, n->len)) {
            break; /* the picture will not fit; send what we have */
        }
        if (n->is_slice) {
            self->stats.slices_out++;
        }
    }

    if (self->out_len == 0) {
        return;
    }
    self->stats.pictures_out++;
    if (self->cb != NULL) {
        self->cb(self->out, self->out_len, repaired, self->ctx);
    }
}
