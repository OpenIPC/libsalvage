/* Rockchip MPP decode backend.
 *
 * The salvage stage already delivers one whole access unit per call, so MPP is
 * fed one AU per packet (split_parse off) rather than left to re-find frame
 * boundaries in a byte stream — the receiver has done that work. disable_error
 * makes it decode past a bad frame instead of freeze-latching on a stale pool
 * buffer, which the end-to-end measurements showed is what separates a decoder
 * that survives loss from one that gives up.
 */

#include <salvage/decoder.h>

#include "decoder_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_mpi.h"

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup grp;
    FILE *out;
    bool owns_out;
    bool dump;
    int frames;
    int err_frames;
    bool eos;
} MppDec;

static void mpp_dump_frame(MppDec *d, MppFrame frame) {
    MppBuffer buf = mpp_frame_get_buffer(frame);
    if (buf == NULL || d->out == NULL) {
        return;
    }
    const RK_U32 w = mpp_frame_get_width(frame);
    const RK_U32 h = mpp_frame_get_height(frame);
    const RK_U32 hs = mpp_frame_get_hor_stride(frame);
    const RK_U32 vs = mpp_frame_get_ver_stride(frame);
    RK_U8 *base = mpp_buffer_get_ptr(buf);
    for (RK_U32 i = 0; i < h; i++) {
        fwrite(base + (size_t)i * hs, 1, w, d->out);
    }
    base += (size_t)hs * vs;
    for (RK_U32 i = 0; i < h / 2; i++) {
        fwrite(base + (size_t)i * hs, 1, w, d->out);
    }
}

static void mpp_drain(MppDec *d) {
    for (;;) {
        MppFrame frame = NULL;
        if (d->mpi->decode_get_frame(d->ctx, &frame) != MPP_OK || frame == NULL) {
            return;
        }
        if (mpp_frame_get_info_change(frame)) {
            /* Geometry known: hand MPP a buffer group sized to it, then commit.
             * Without the external group MPP has nowhere to decode into and
             * stalls after the first frame. */
            const RK_U32 buf_size = mpp_frame_get_buf_size(frame);
            if (d->grp == NULL) {
                mpp_buffer_group_get_internal(&d->grp, MPP_BUFFER_TYPE_ION);
            }
            mpp_buffer_group_limit_config(d->grp, buf_size, 24);
            d->mpi->control(d->ctx, MPP_DEC_SET_EXT_BUF_GROUP, d->grp);
            d->mpi->control(d->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&frame);
            continue;
        }
        if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
            d->err_frames++;
        }
        if (d->dump) {
            mpp_dump_frame(d, frame);
        }
        d->frames++;
        if (mpp_frame_get_eos(frame)) {
            d->eos = true;
            mpp_frame_deinit(&frame);
            return;
        }
        mpp_frame_deinit(&frame);
    }
}

static void *mpp_be_create(const SalvageDecoderConfig *cfg) {
    MppDec *d = calloc(1, sizeof *d);
    if (d == NULL) {
        return NULL;
    }
    d->dump = cfg->dump_yuv;
    if (cfg->out != NULL && strcmp(cfg->out, "-") != 0) {
        d->out = fopen(cfg->out, "wb");
        d->owns_out = d->out != NULL;
    } else if (cfg->out != NULL) {
        d->out = stdout;
    }

    if (mpp_create(&d->ctx, &d->mpi) != MPP_OK) {
        free(d);
        return NULL;
    }
    const MppCodingType coding =
        cfg->codec == SALVAGE_H265 ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    if (mpp_init(d->ctx, MPP_CTX_DEC, coding) != MPP_OK) {
        mpp_destroy(d->ctx);
        free(d);
        return NULL;
    }

    MppDecCfg mcfg = NULL;
    mpp_dec_cfg_init(&mcfg);
    d->mpi->control(d->ctx, MPP_DEC_GET_CFG, mcfg);
    mpp_dec_cfg_set_u32(mcfg, "base:split_parse", 0);
    mpp_dec_cfg_set_u32(
        mcfg, "base:disable_error", cfg->tolerate_errors ? 1 : 0);
    d->mpi->control(d->ctx, MPP_DEC_SET_CFG, mcfg);
    mpp_dec_cfg_deinit(mcfg);

    return d;
}

static int mpp_be_feed(void *self, const uint8_t *annexb, size_t len) {
    MppDec *d = self;
    MppPacket pkt = NULL;
    if (mpp_packet_init(&pkt, (void *)annexb, len) != MPP_OK) {
        return -1;
    }
    mpp_packet_set_pos(pkt, (void *)annexb);
    mpp_packet_set_length(pkt, len);

    /* MPP decodes asynchronously: put returns EAGAIN while its input queue is
     * full, and the queue only drains as frames are pulled out. Retry the put
     * with a short yield so the decode thread runs, draining frames each pass —
     * without the yield a tight put/drain loop starves the decoder and it
     * accepts nothing. */
    int done = 0, guard = 0;
    while (!done && guard++ < 20000) {
        if (d->mpi->decode_put_packet(d->ctx, pkt) == MPP_OK) {
            done = 1;
        } else {
            usleep(1000);
        }
        mpp_drain(d);
    }
    mpp_packet_deinit(&pkt);
    return d->frames;
}

static int mpp_be_finish(void *self) {
    MppDec *d = self;
    MppPacket pkt = NULL;
    mpp_packet_init(&pkt, NULL, 0);
    mpp_packet_set_eos(pkt);
    while (d->mpi->decode_put_packet(d->ctx, pkt) != MPP_OK) {
        usleep(1000);
    }
    mpp_packet_deinit(&pkt);
    for (int spin = 0; !d->eos && spin < 3000; spin++) {
        mpp_drain(d);
        usleep(1000);
    }
    return d->frames;
}

static int mpp_be_errors(void *self) {
    return ((MppDec *)self)->err_frames;
}

static void mpp_be_free(void *self) {
    MppDec *d = self;
    if (d == NULL) {
        return;
    }
    if (d->mpi != NULL) {
        d->mpi->reset(d->ctx);
    }
    if (d->ctx != NULL) {
        mpp_destroy(d->ctx);
    }
    if (d->grp != NULL) {
        mpp_buffer_group_put(d->grp);
    }
    if (d->owns_out && d->out != NULL) {
        fclose(d->out);
    }
    free(d);
}

const SalvageDecoderVtable salvage_decoder_mpp_vt = {
    mpp_be_create, mpp_be_feed, mpp_be_finish, mpp_be_errors, mpp_be_free};
