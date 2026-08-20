#ifndef SALVAGE_SALVAGE_H
#define SALVAGE_SALVAGE_H

/* Slice salvage: make the most of a picture that arrived incomplete.
 *
 * Two things happen here, in order of how much they are worth.
 *
 * Synthesising a missing first slice is the single highest-value repair. Slice
 * 0 carries the picture-level parameters every other slice depends on, so
 * losing it is categorically worse than losing any other: measured on Rockchip,
 * 46.05 dB of damage across the whole picture instead of 66.10 dB confined to
 * one band, and on GStreamer the picture is lost entirely while ffmpeg gives up
 * on the stream. A stand-in built from the previous picture's first slice, with
 * its frame_num and POC retargeted to this picture, removes that cliff — and
 * makes stock unpatched ffmpeg decode 161 frames where it managed 25.
 *
 * Forwarding the slices that did arrive is the other half, and unlike the
 * first it is a policy question rather than a fact. A truncated slice helps on
 * Intel (0.49% dirty pixels against 0.90% if dropped) and hurts on Rockchip
 * (30.80% against 25.47%). The right answer is a property of the decoder, so it
 * is a parameter here and the commissioning harness in rkvdec-slice-lab exists
 * to measure it per platform rather than inherit someone else's guess.
 */

#include <salvage/depay.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    /* Hand a slice cut short by loss to the decoder anyway. */
    SALVAGE_TRUNCATED_FORWARD,
    /* Drop it, keeping only the slices that arrived whole. */
    SALVAGE_TRUNCATED_DROP,
} SalvageTruncatedPolicy;

typedef struct {
    SalvageTruncatedPolicy truncated;
    bool synthesise_first_slice;
} SalvagePolicy;

typedef struct {
    uint64_t pictures_in;
    uint64_t pictures_out;
    uint64_t pictures_damaged;         /* arrived with loss */
    uint64_t first_slice_synthesised;  /* holes filled from a retargeted slice */
    uint64_t first_slice_unrepaired;   /* missing, and no stand-in available */
    uint64_t truncated_forwarded;
    uint64_t truncated_dropped;
    uint64_t slices_out;
} SalvageStats;

/* `repaired` marks a picture that had something synthesised into it. */
typedef void (*SalvagePicCb)(
    const uint8_t *annexb, size_t len, bool repaired, void *ctx);

typedef struct SalvageStage SalvageStage;

SalvageStage *salvage_new(
    SalvageCodec codec, SalvagePolicy policy, size_t max_picture_bytes,
    SalvagePicCb cb, void *ctx);

void salvage_free(SalvageStage *self);

/* Feed one access unit, as produced by the depacketiser. */
void salvage_input(SalvageStage *self, const SalvageAu *au);

void salvage_stats(const SalvageStage *self, SalvageStats *out);

#endif /* SALVAGE_SALVAGE_H */
