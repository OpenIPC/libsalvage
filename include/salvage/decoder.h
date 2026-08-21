#ifndef SALVAGE_DECODER_H
#define SALVAGE_DECODER_H

/* A robust hardware decoder behind one interface, with a different backend per
 * platform — because the right decoder, and the way to make it survive damage,
 * is not the same on each.
 *
 * The end-to-end measurements are the reason this is an interface and not a
 * single call. On Rockchip the robust decoder is MPP with base:disable_error
 * (it corrupts in place and keeps running); the naive configuration freeze-
 * latches. On Intel the robust decoder is GStreamer's vah264dec (it conceals a
 * lost slice by repeating a frame); stock ffmpeg-vaapi aborts on the first
 * error and produces nothing. Same loss, same recovered stream — opposite
 * failure modes — so the backend and its error policy are chosen per platform,
 * here, rather than assumed by the caller.
 *
 * Backends are compiled in as available:
 *   null  always      writes Annex-B, no decode (portable, the default)
 *   gst   libgstreamer appsrc -> h264parse -> decoder -> sink (Intel VA, etc.)
 *   mpp   librockchip_mpp   direct MPP decode with disable_error (Rockchip)
 */

#include <salvage/depay.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    /* Pick a backend by name at runtime; falls back to null if unbuilt. */
    SALVAGE_DECODER_AUTO,
    SALVAGE_DECODER_NULL,
    SALVAGE_DECODER_GST,
    SALVAGE_DECODER_MPP,
} SalvageDecoderKind;

typedef struct {
    SalvageDecoderKind kind;
    SalvageCodec codec;

    /* Keep decoding past a damaged frame instead of stopping. This is the knob
     * that separates a decoder that survives loss from one that gives up — MPP
     * base:disable_error, and the reason to prefer an error-tolerant GStreamer
     * decoder over stock ffmpeg-vaapi. Default on; a commissioning run may turn
     * it off to measure the difference. */
    bool tolerate_errors;

    /* Where decoded frames go, backend-dependent. For null and the file sinks:
     * a path, or NULL/"-" for stdout. For gst: overrides the sink element
     * (default a fakesink that counts frames). */
    const char *out;

    /* Optional: force decoded output to raw I420/NV12 to `out` as a file. */
    bool dump_yuv;
} SalvageDecoderConfig;

typedef struct SalvageDecoder SalvageDecoder;

/* Returns NULL on failure (e.g. a backend that is not built or no device). */
SalvageDecoder *salvage_decoder_new(const SalvageDecoderConfig *cfg);

/* Feed one access unit in Annex-B form (as the salvage stage emits). Returns
 * the number of frames the decoder produced from this and any buffered input,
 * or -1 on a fatal decoder error. */
int salvage_decoder_feed(SalvageDecoder *self, const uint8_t *annexb, size_t len);

/* Flush and return total frames decoded. */
int salvage_decoder_finish(SalvageDecoder *self);

/* Frames the decoder flagged with errinfo/discard, where the backend reports it
 * (MPP does; GStreamer conceals silently, so it returns 0). */
int salvage_decoder_errors(SalvageDecoder *self);

void salvage_decoder_free(SalvageDecoder *self);

/* Which backends were compiled in, for the CLI to report. */
const char *salvage_decoder_backends(void);

#endif /* SALVAGE_DECODER_H */
