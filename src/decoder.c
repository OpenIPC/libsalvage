#include <salvage/decoder.h>

#include "decoder_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The null backend is defined here; gst and mpp live in their own files and
 * register only when compiled in. */

struct SalvageDecoder {
    const SalvageDecoderVtable *vt;
    void *backend;
};

/* --- null backend: no decode, just count and optionally dump Annex-B ----- */

typedef struct {
    FILE *out;
    bool owns;
    int aus;
} NullDec;

static void *null_create(const SalvageDecoderConfig *cfg) {
    NullDec *d = calloc(1, sizeof *d);
    if (d == NULL) {
        return NULL;
    }
    if (cfg->out != NULL && strcmp(cfg->out, "-") != 0) {
        d->out = fopen(cfg->out, "wb");
        d->owns = d->out != NULL;
    }
    return d;
}

static int null_feed(void *self, const uint8_t *annexb, size_t len) {
    NullDec *d = self;
    if (d->out != NULL) {
        fwrite(annexb, 1, len, d->out);
    }
    d->aus++;
    return 1; /* one access unit "out" */
}

static int null_finish(void *self) {
    return ((NullDec *)self)->aus;
}

static void null_free(void *self) {
    NullDec *d = self;
    if (d != NULL) {
        if (d->owns && d->out != NULL) {
            fclose(d->out);
        }
        free(d);
    }
}

static const SalvageDecoderVtable NULL_VT = {
    null_create, null_feed, null_finish, null_free};

/* --- backends compiled in elsewhere ------------------------------------- */

#ifdef SALVAGE_WITH_GST
extern const SalvageDecoderVtable salvage_decoder_gst_vt;
#endif
#ifdef SALVAGE_WITH_MPP
extern const SalvageDecoderVtable salvage_decoder_mpp_vt;
#endif

static const SalvageDecoderVtable *pick(SalvageDecoderKind kind) {
    switch (kind) {
    case SALVAGE_DECODER_NULL:
        return &NULL_VT;
#ifdef SALVAGE_WITH_MPP
    case SALVAGE_DECODER_MPP:
        return &salvage_decoder_mpp_vt;
#endif
#ifdef SALVAGE_WITH_GST
    case SALVAGE_DECODER_GST:
        return &salvage_decoder_gst_vt;
#endif
    case SALVAGE_DECODER_AUTO:
        /* Prefer a real hardware backend if one was built; the platform that
         * ships it is the platform that has it. */
#ifdef SALVAGE_WITH_MPP
        return &salvage_decoder_mpp_vt;
#elif defined(SALVAGE_WITH_GST)
        return &salvage_decoder_gst_vt;
#else
        return &NULL_VT;
#endif
    default:
        return &NULL_VT;
    }
}

SalvageDecoder *salvage_decoder_new(const SalvageDecoderConfig *cfg) {
    SalvageDecoder *self = calloc(1, sizeof *self);
    if (self == NULL) {
        return NULL;
    }
    self->vt = pick(cfg->kind);
    self->backend = self->vt->create(cfg);
    if (self->backend == NULL) {
        free(self);
        return NULL;
    }
    return self;
}

int salvage_decoder_feed(
    SalvageDecoder *self, const uint8_t *annexb, size_t len) {
    return self->vt->feed(self->backend, annexb, len);
}

int salvage_decoder_finish(SalvageDecoder *self) {
    return self->vt->finish(self->backend);
}

void salvage_decoder_free(SalvageDecoder *self) {
    if (self != NULL) {
        self->vt->free(self->backend);
        free(self);
    }
}

const char *salvage_decoder_backends(void) {
    return "null"
#ifdef SALVAGE_WITH_GST
           " gst"
#endif
#ifdef SALVAGE_WITH_MPP
           " mpp"
#endif
        ;
}
