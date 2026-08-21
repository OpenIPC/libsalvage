#ifndef SALVAGE_RETARGET_H
#define SALVAGE_RETARGET_H

/* Slice-header surgery: make a slice from one picture belong to another.
 *
 * Replaying a slice from the previous picture is the cheapest way to fill the
 * hole left by a lost one, but the replayed slice still carries the previous
 * picture's frame_num and pic_order_cnt_lsb. Decoders react differently and all
 * of them badly — libavcodec rejects the slice outright ("Frame num change from
 * 5 to 4"), GStreamer decodes it into the wrong picture, and Rockchip MPP
 * accepts it only because its frame-boundary detection never looks at
 * frame_num. For a lost FIRST slice a wrong frame_num makes MPP build the
 * previous picture's parameters, which wrecks quality from the next frame on.
 *
 * Both fields are fixed-width — u(log2_max_frame_num) and u(log2_max_poc_lsb)
 * in H.264, u(log2_max_poc_lsb) in H.265 — so retargeting needs no bit shifting
 * and no CABAC re-alignment: parse forward to the offset, patch in place, redo
 * emulation prevention. The RBSP length never changes.
 *
 * Validated against real decoders: 46.05 dB whole-picture damage became 66.10
 * dB confined to the lost slice, MPP errinfo went 60 -> 0, and stock unpatched
 * ffmpeg went from 25 decoded frames to 161.
 *
 * Deliberately narrow. Every function refuses stream shapes it does not fully
 * understand rather than guessing, because a slice that is silently wrong is
 * worse than no repair at all.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool valid;
    /* H.264 */
    unsigned log2_max_frame_num;
    unsigned pic_order_cnt_type;
    bool frame_mbs_only;
    /* both */
    unsigned log2_max_poc_lsb;
    bool separate_colour_plane;
    /* H.265 */
    unsigned slice_address_bits;
} SalvageSps;

typedef struct {
    bool valid;
    /* H.265 */
    bool dependent_slice_segments_enabled;
    bool output_flag_present;
    unsigned num_extra_slice_header_bits;
} SalvagePps;

typedef struct {
    uint8_t nal_type;
    unsigned first_mb;      /* H.264 */
    bool first_slice;       /* H.265 first_slice_segment_in_pic_flag */
    unsigned slice_type;

    bool has_frame_num;
    size_t frame_num_off;
    unsigned frame_num_bits;
    uint32_t frame_num;

    bool has_poc;
    size_t poc_off;
    unsigned poc_bits;
    uint32_t poc_lsb;
} SalvageSliceHeader;

/* All take a NAL including its header, with start code and emulation
 * prevention bytes already removed for the *_rbsp variants. Return false when
 * the stream shape is unsupported or the data ran out. */
bool salvage_sps_h264(const uint8_t *rbsp, size_t len, SalvageSps *out);
bool salvage_pps_h264(const uint8_t *rbsp, size_t len, SalvagePps *out);
bool salvage_slice_h264(
    const uint8_t *rbsp, size_t len, const SalvageSps *sps,
    const SalvagePps *pps, SalvageSliceHeader *out);

bool salvage_sps_h265(const uint8_t *rbsp, size_t len, SalvageSps *out);
bool salvage_pps_h265(const uint8_t *rbsp, size_t len, SalvagePps *out);
bool salvage_slice_h265(
    const uint8_t *rbsp, size_t len, const SalvageSps *sps,
    const SalvagePps *pps, SalvageSliceHeader *out);

#endif /* SALVAGE_RETARGET_H */
