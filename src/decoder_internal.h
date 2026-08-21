#ifndef SALVAGE_DECODER_INTERNAL_H
#define SALVAGE_DECODER_INTERNAL_H

/* Shared between the dispatcher (decoder.c) and each backend. A backend defines
 * one const SalvageDecoderVtable named salvage_decoder_<kind>_vt. */

#include <salvage/decoder.h>

typedef struct {
    void *(*create)(const SalvageDecoderConfig *cfg);
    int (*feed)(void *self, const uint8_t *annexb, size_t len);
    int (*finish)(void *self);
    int (*errors)(void *self); /* may be NULL */
    void (*free)(void *self);
} SalvageDecoderVtable;

#endif /* SALVAGE_DECODER_INTERNAL_H */
