#include "retarget.h"

#include "bits.h"

#include <string.h>

/* --- H.264 -------------------------------------------------------------- */

static void skip_scaling_list(BitReader *r, unsigned size) {
    int last = 8, next = 8;
    for (unsigned i = 0; i < size; i++) {
        if (next != 0) {
            next = (last + br_se(r) + 256) % 256;
        }
        last = next ? next : last;
    }
}

static bool profile_has_chroma_syntax(unsigned p) {
    switch (p) {
    case 100: case 110: case 122: case 244: case 44: case 83:
    case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

bool salvage_sps_h264(const uint8_t *rbsp, size_t len, SalvageSps *out) {
    BitReader r;
    br_init(&r, rbsp, len);
    br_u(&r, 8); /* NAL header */

    const unsigned profile_idc = br_u(&r, 8);
    br_u(&r, 8); /* constraint flags + reserved */
    br_u(&r, 8); /* level_idc */

    SalvageSps s = {0};
    br_ue(&r); /* seq_parameter_set_id */

    if (profile_has_chroma_syntax(profile_idc)) {
        const unsigned chroma_format_idc = br_ue(&r);
        if (chroma_format_idc == 3) {
            s.separate_colour_plane = br_u(&r, 1) != 0;
        }
        br_ue(&r); /* bit_depth_luma_minus8 */
        br_ue(&r); /* bit_depth_chroma_minus8 */
        br_u(&r, 1); /* qpprime_y_zero_transform_bypass_flag */
        if (br_u(&r, 1)) { /* seq_scaling_matrix_present_flag */
            const unsigned n = chroma_format_idc != 3 ? 8 : 12;
            for (unsigned i = 0; i < n; i++) {
                if (br_u(&r, 1)) {
                    skip_scaling_list(&r, i < 6 ? 16 : 64);
                }
            }
        }
    }

    s.log2_max_frame_num = br_ue(&r) + 4;
    s.pic_order_cnt_type = br_ue(&r);
    if (s.pic_order_cnt_type == 0) {
        s.log2_max_poc_lsb = br_ue(&r) + 4;
    } else if (s.pic_order_cnt_type == 1) {
        br_u(&r, 1); /* delta_pic_order_always_zero_flag */
        br_se(&r);
        br_se(&r);
        const uint32_t cycle = br_ue(&r);
        for (uint32_t i = 0; i < cycle && !r.overrun; i++) {
            br_se(&r);
        }
    }
    br_ue(&r);   /* max_num_ref_frames */
    br_u(&r, 1); /* gaps_in_frame_num_value_allowed_flag */
    br_ue(&r);   /* pic_width_in_mbs_minus1 */
    br_ue(&r);   /* pic_height_in_map_units_minus1 */
    s.frame_mbs_only = br_u(&r, 1) != 0;

    if (r.overrun || s.log2_max_frame_num > 32 || s.log2_max_poc_lsb > 32) {
        return false;
    }
    s.valid = true;
    *out = s;
    return true;
}

bool salvage_pps_h264(const uint8_t *rbsp, size_t len, SalvagePps *out) {
    BitReader r;
    br_init(&r, rbsp, len);
    br_u(&r, 8);
    br_ue(&r); /* pic_parameter_set_id */
    br_ue(&r); /* seq_parameter_set_id */
    br_u(&r, 1); /* entropy_coding_mode_flag */
    br_u(&r, 1); /* bottom_field_pic_order_in_frame_present_flag */
    if (r.overrun) {
        return false;
    }
    SalvagePps p = {.valid = true};
    *out = p;
    return true;
}

bool salvage_slice_h264(
    const uint8_t *rbsp, size_t len, const SalvageSps *sps,
    const SalvagePps *pps, SalvageSliceHeader *out) {
    if (!sps->valid || !pps->valid) {
        return false;
    }
    /* Interlace and separate colour planes insert fields ahead of the ones we
     * are looking for; pic_order_cnt_type != 0 means there is no POC lsb at
     * all. None of these appear in the streams this targets, and guessing at
     * them would silently patch the wrong bits. */
    if (sps->separate_colour_plane || !sps->frame_mbs_only ||
        sps->pic_order_cnt_type != 0) {
        return false;
    }

    BitReader r;
    br_init(&r, rbsp, len);
    const unsigned hdr = br_u(&r, 8);
    const uint8_t nal_type = (uint8_t)(hdr & 0x1fu);
    if (nal_type != 1 && nal_type != 5) {
        return false;
    }

    SalvageSliceHeader h = {.nal_type = nal_type};
    h.first_mb = br_ue(&r);
    h.slice_type = br_ue(&r) % 5;
    br_ue(&r); /* pic_parameter_set_id */

    h.frame_num_off = r.pos;
    h.frame_num_bits = sps->log2_max_frame_num;
    h.frame_num = br_u(&r, h.frame_num_bits);
    h.has_frame_num = true;

    if (nal_type == 5) {
        br_ue(&r); /* idr_pic_id */
    }
    h.poc_off = r.pos;
    h.poc_bits = sps->log2_max_poc_lsb;
    h.poc_lsb = br_u(&r, h.poc_bits);
    h.has_poc = true;

    if (r.overrun) {
        return false;
    }
    *out = h;
    return true;
}

/* --- H.265 -------------------------------------------------------------- */

static unsigned ceil_log2(uint32_t n) {
    unsigned b = 0;
    while ((1u << b) < n && b < 32) {
        b++;
    }
    return b;
}

static void skip_profile_tier_level(BitReader *r, unsigned max_sub) {
    br_u(r, 32);
    br_u(r, 32);
    br_u(r, 24);
    br_u(r, 8); /* general_level_idc */

    bool sub_profile[8] = {false}, sub_level[8] = {false};
    for (unsigned i = 0; i < max_sub && i < 8; i++) {
        sub_profile[i] = br_u(r, 1) != 0;
        sub_level[i] = br_u(r, 1) != 0;
    }
    if (max_sub > 0) {
        for (unsigned i = max_sub; i < 8; i++) {
            br_u(r, 2);
        }
    }
    for (unsigned i = 0; i < max_sub && i < 8; i++) {
        if (sub_profile[i]) {
            br_u(r, 32);
            br_u(r, 32);
            br_u(r, 24);
        }
        if (sub_level[i]) {
            br_u(r, 8);
        }
    }
}

bool salvage_sps_h265(const uint8_t *rbsp, size_t len, SalvageSps *out) {
    BitReader r;
    br_init(&r, rbsp, len);
    br_u(&r, 16); /* NAL header */
    br_u(&r, 4);  /* sps_video_parameter_set_id */
    const unsigned max_sub = br_u(&r, 3);
    br_u(&r, 1); /* sps_temporal_id_nesting_flag */
    skip_profile_tier_level(&r, max_sub);

    SalvageSps s = {0};
    br_ue(&r); /* sps_seq_parameter_set_id */
    const unsigned chroma_format_idc = br_ue(&r);
    if (chroma_format_idc == 3) {
        s.separate_colour_plane = br_u(&r, 1) != 0;
    }
    const uint32_t width = br_ue(&r);
    const uint32_t height = br_ue(&r);
    if (br_u(&r, 1)) { /* conformance_window_flag */
        br_ue(&r);
        br_ue(&r);
        br_ue(&r);
        br_ue(&r);
    }
    br_ue(&r); /* bit_depth_luma_minus8 */
    br_ue(&r); /* bit_depth_chroma_minus8 */
    s.log2_max_poc_lsb = br_ue(&r) + 4;

    const bool sub_layer_ordering = br_u(&r, 1) != 0;
    for (unsigned i = sub_layer_ordering ? 0 : max_sub; i <= max_sub; i++) {
        br_ue(&r);
        br_ue(&r);
        br_ue(&r);
    }
    const unsigned log2_min_cb = br_ue(&r) + 3;
    const unsigned log2_ctb = log2_min_cb + br_ue(&r);

    if (r.overrun || log2_ctb > 31 || s.log2_max_poc_lsb > 32 || width == 0 ||
        height == 0) {
        return false;
    }
    const uint32_t ctb = 1u << log2_ctb;
    const uint32_t ctbs_w = (width + ctb - 1) / ctb;
    const uint32_t ctbs_h = (height + ctb - 1) / ctb;
    s.slice_address_bits = ceil_log2(ctbs_w * ctbs_h);
    s.valid = true;
    *out = s;
    return true;
}

bool salvage_pps_h265(const uint8_t *rbsp, size_t len, SalvagePps *out) {
    BitReader r;
    br_init(&r, rbsp, len);
    br_u(&r, 16);
    br_ue(&r); /* pps_pic_parameter_set_id */
    br_ue(&r); /* pps_seq_parameter_set_id */
    SalvagePps p = {0};
    p.dependent_slice_segments_enabled = br_u(&r, 1) != 0;
    p.output_flag_present = br_u(&r, 1) != 0;
    p.num_extra_slice_header_bits = br_u(&r, 3);
    if (r.overrun) {
        return false;
    }
    p.valid = true;
    *out = p;
    return true;
}

bool salvage_slice_h265(
    const uint8_t *rbsp, size_t len, const SalvageSps *sps,
    const SalvagePps *pps, SalvageSliceHeader *out) {
    if (!sps->valid || !pps->valid || sps->separate_colour_plane) {
        return false;
    }

    BitReader r;
    br_init(&r, rbsp, len);
    const unsigned hdr = br_u(&r, 16);
    const uint8_t nal_type = (uint8_t)((hdr >> 9) & 0x3fu);
    if (nal_type > 31) {
        return false;
    }

    SalvageSliceHeader h = {.nal_type = nal_type};
    h.first_slice = br_u(&r, 1) != 0;
    if (nal_type >= 16 && nal_type <= 23) { /* IRAP */
        br_u(&r, 1); /* no_output_of_prior_pics_flag */
    }
    br_ue(&r); /* slice_pic_parameter_set_id */

    bool dependent = false;
    if (!h.first_slice) {
        if (pps->dependent_slice_segments_enabled) {
            dependent = br_u(&r, 1) != 0;
        }
        br_u(&r, sps->slice_address_bits); /* slice_segment_address */
    }
    if (dependent) {
        /* A dependent slice segment carries no picture-level fields at all, so
         * there is nothing to retarget and it cannot start a picture. */
        return false;
    }

    for (unsigned i = 0; i < pps->num_extra_slice_header_bits; i++) {
        br_u(&r, 1);
    }
    h.slice_type = br_ue(&r);
    if (pps->output_flag_present) {
        br_u(&r, 1); /* pic_output_flag */
    }

    if (nal_type == 19 || nal_type == 20) { /* IDR_W_RADL, IDR_N_LP */
        /* An IDR carries no slice_pic_order_cnt_lsb — POC is zero by
         * definition — so there is nothing to move it to another picture. */
        if (r.overrun) {
            return false;
        }
        h.has_poc = false;
        *out = h;
        return true;
    }

    h.poc_off = r.pos;
    h.poc_bits = sps->log2_max_poc_lsb;
    h.poc_lsb = br_u(&r, h.poc_bits);
    h.has_poc = true;

    if (r.overrun) {
        return false;
    }
    *out = h;
    return true;
}
