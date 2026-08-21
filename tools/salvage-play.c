/* salvage-play — the reference receiver end to end.
 *
 * Replays a captured RTP session (media + FlexFEC repair) through the whole
 * pipeline — recover, depacketise, salvage — and hands each rebuilt picture to
 * the platform's hardware decoder behind one interface. It is the runnable
 * answer to "what does a robust receiver look like", and doubles as the
 * commissioning harness: point it at a target decoder, feed it a lossy capture,
 * and see how many pictures survive.
 *
 *   salvage-play --decoder gst  capture.pcap
 *   salvage-play --decoder mpp --codec h264 --dump out.yuv capture.pcap
 *
 * The decoder is a different component per platform (MPP on Rockchip, GStreamer
 * elsewhere); see include/salvage/decoder.h for why. Backends are compiled in
 * as their libraries are available; `--decoder` picks one, default auto.
 */

#include <salvage/decoder.h>
#include <salvage/depay.h>
#include <salvage/flexfec.h>
#include <salvage/salvage.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTP_HDR 12

typedef struct {
    SalvageFec *fec;
    SalvageDepay *depay;
    SalvageStage *stage;
    SalvageDecoder *dec;
    uint8_t media_pt, fec_pt;
    uint64_t pics, repaired;
    int frames;

    /* Modes, so one binary is the whole matrix: raw = no recovery, no salvage;
     * fec = recover only; default = recover then salvage. */
    bool no_fec, no_salvage;

    /* Gilbert-Elliott loss, same model as salvage-pcap: q = 1/mean_burst is
     * the chance of leaving the bad state, p = q*loss/(1-loss) of entering it.
     * Bursts are what defeat XOR FEC, so uniform loss would flatter it. */
    double ge_p, ge_q;
    bool ge_on, ge_bad;
    uint64_t rng, dropped;
} Play;

static void feed_decoder(Play *p, const uint8_t *annexb, size_t len, bool rep) {
    p->pics++;
    p->repaired += rep;
    const int f = salvage_decoder_feed(p->dec, annexb, len);
    if (f >= 0) {
        p->frames = f;
    }
}

static void on_pic(const uint8_t *pic, size_t len, bool repaired, void *ctx) {
    feed_decoder(ctx, pic, len, repaired);
}

static void on_au(const SalvageAu *au, void *ctx) {
    Play *p = ctx;
    if (p->no_salvage) {
        /* Hand the depacketiser's access unit straight to the decoder — this
         * is the "fec, no salvage" and "raw" path. */
        feed_decoder(p, au->data, au->len, false);
    } else {
        salvage_input(p->stage, au);
    }
}

static void on_out(const uint8_t *pkt, size_t len, bool recovered, void *ctx) {
    salvage_depay_input(((Play *)ctx)->depay, pkt, len, recovered);
}

/* xorshift64*, so a run of losses is reproducible from its seed. */
static double next_random(Play *p) {
    p->rng ^= p->rng >> 12;
    p->rng ^= p->rng << 25;
    p->rng ^= p->rng >> 27;
    return (double)((p->rng * 2685821657736338717ull) >> 11) /
           (double)(1ull << 53);
}

static bool ge_drop(Play *p) {
    bool lost;
    if (p->ge_bad) {
        lost = true;
        if (next_random(p) < p->ge_q) {
            p->ge_bad = false;
        }
    } else {
        lost = false;
        if (next_random(p) < p->ge_p) {
            p->ge_bad = true;
        }
    }
    return lost;
}

/* --- pcap ------------------------------------------------------------- */

static void replay(const char *path, Play *p) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return;
    }
    uint8_t gh[24];
    if (fread(gh, 1, sizeof gh, f) != sizeof gh) {
        fclose(f);
        return;
    }
    const uint32_t magic = (uint32_t)gh[0] | ((uint32_t)gh[1] << 8) |
                           ((uint32_t)gh[2] << 16) | ((uint32_t)gh[3] << 24);
    const bool le = (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d);
    uint32_t linktype;
    memcpy(&linktype, gh + 20, 4);
    if (!le) {
        linktype = __builtin_bswap32(linktype);
    }
    const size_t l2 = linktype == 276 ? 20 : (linktype == 113 ? 16 : 14);

    uint8_t rec[16], buf[65536];
    while (fread(rec, 1, sizeof rec, f) == sizeof rec) {
        uint32_t caplen;
        memcpy(&caplen, rec + 8, 4);
        if (!le) {
            caplen = __builtin_bswap32(caplen);
        }
        if (caplen > sizeof buf || fread(buf, 1, caplen, f) != caplen) {
            break;
        }
        if (caplen < l2 + 20) {
            continue;
        }
        const uint8_t *ip = buf + l2;
        if ((ip[0] >> 4) != 4 || ip[9] != 17) {
            continue;
        }
        const uint8_t *udp = ip + (size_t)(ip[0] & 0x0f) * 4;
        if ((size_t)(buf + caplen - udp) < 8) {
            continue;
        }
        size_t ulen = (size_t)((udp[4] << 8) | udp[5]);
        const size_t avail = (size_t)(buf + caplen - udp);
        if (ulen < 8 || ulen > avail) {
            ulen = avail;
        }
        const uint8_t *rtp = udp + 8;
        const size_t rlen = ulen - 8;
        if (rlen < RTP_HDR || (rtp[0] >> 6) != 2) {
            continue;
        }
        const uint8_t pt = (uint8_t)(rtp[1] & 0x7f);
        if (pt == p->media_pt) {
            if (p->ge_on && ge_drop(p)) {
                p->dropped++;
                continue;
            }
            if (p->no_fec) {
                on_out(rtp, rlen, false, p); /* straight to the depacketiser */
            } else {
                salvage_fec_input(p->fec, rtp, rlen);
            }
        } else if (pt == p->fec_pt && !p->no_fec) {
            salvage_fec_input(p->fec, rtp, rlen);
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *path = NULL, *out = NULL, *decname = "auto";
    double loss_pct = 0.0, mean_burst = 1.0;
    uint64_t seed = 1;
    SalvageCodec codec = SALVAGE_H264;
    SalvageDecoderConfig dc = {.tolerate_errors = true};
    SalvagePolicy pol = {SALVAGE_TRUNCATED_FORWARD, true};
    Play p = {.media_pt = 96, .fec_pt = 101};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--media-pt") && i + 1 < argc) {
            p.media_pt = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fec-pt") && i + 1 < argc) {
            p.fec_pt = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--codec") && i + 1 < argc) {
            codec = strcmp(argv[++i], "h265") == 0 ? SALVAGE_H265 : SALVAGE_H264;
        } else if (!strcmp(argv[i], "--decoder") && i + 1 < argc) {
            decname = argv[++i];
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            out = argv[++i];
            dc.dump_yuv = true;
        } else if (!strcmp(argv[i], "--annexb") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--drop-truncated")) {
            pol.truncated = SALVAGE_TRUNCATED_DROP;
        } else if (!strcmp(argv[i], "--no-synth")) {
            pol.synthesise_first_slice = false;
        } else if (!strcmp(argv[i], "--intolerant")) {
            dc.tolerate_errors = false;
        } else if (!strcmp(argv[i], "--no-fec")) {
            p.no_fec = true;
        } else if (!strcmp(argv[i], "--no-salvage")) {
            p.no_salvage = true;
        } else if (!strcmp(argv[i], "--loss") && i + 1 < argc) {
            loss_pct = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--mean-burst") && i + 1 < argc) {
            mean_burst = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else {
            path = argv[i];
        }
    }
    if (path == NULL) {
        fprintf(
            stderr,
            "usage: %s [--decoder auto|null|gst|mpp] [--codec h264|h265]\n"
            "       [--dump out.yuv | --annexb out.264] [--drop-truncated]\n"
            "       [--no-synth] [--intolerant] [--no-fec] [--no-salvage]\n"
            "       [--loss PCT] [--mean-burst N] [--seed S]"
            " [--media-pt N] [--fec-pt N] capture.pcap\n"
            "backends compiled in: %s\n",
            argv[0], salvage_decoder_backends());
        return 2;
    }

    dc.codec = codec;
    dc.out = out;
    dc.kind = !strcmp(decname, "null")  ? SALVAGE_DECODER_NULL
              : !strcmp(decname, "gst") ? SALVAGE_DECODER_GST
              : !strcmp(decname, "mpp") ? SALVAGE_DECODER_MPP
                                        : SALVAGE_DECODER_AUTO;

    if (loss_pct > 0.0) {
        const double loss = loss_pct / 100.0;
        p.ge_q = mean_burst > 1.0 ? 1.0 / mean_burst : 1.0;
        p.ge_p = loss < 1.0 ? p.ge_q * loss / (1.0 - loss) : 1.0;
        p.ge_on = true;
        p.rng = seed ? seed : 1;
    }

    p.dec = salvage_decoder_new(&dc);
    p.stage = salvage_new(codec, pol, 0, on_pic, &p);
    p.depay = salvage_depay_new(codec, 0, on_au, &p);
    p.fec = salvage_fec_new(p.media_pt, p.fec_pt, 512, on_out, &p);
    if (p.dec == NULL || p.stage == NULL || p.depay == NULL || p.fec == NULL) {
        fprintf(stderr, "init failed (decoder '%s' available? %s)\n", decname,
                salvage_decoder_backends());
        return 1;
    }

    replay(path, &p);
    salvage_fec_flush(p.fec);
    salvage_depay_flush(p.depay);
    const int frames = salvage_decoder_finish(p.dec);
    const int errs = salvage_decoder_errors(p.dec);

    SalvageStats sv;
    salvage_stats(p.stage, &sv);
    printf("pictures salvaged : %llu (%llu with a synthesised first slice)\n",
           (unsigned long long)p.pics, (unsigned long long)p.repaired);
    printf("first slice synth : %llu made, %llu could not be\n",
           (unsigned long long)sv.first_slice_synthesised,
           (unsigned long long)sv.first_slice_unrepaired);
    if (p.ge_on) {
        printf("dropped by us     : %llu\n", (unsigned long long)p.dropped);
    }
    printf("frames decoded    : %d\n", frames);
    if (errs > 0) {
        printf("decoder errinfo   : %d frames\n", errs);
    }

    salvage_decoder_free(p.dec);
    salvage_free(p.stage);
    salvage_depay_free(p.depay);
    salvage_fec_free(p.fec);
    return 0;
}
