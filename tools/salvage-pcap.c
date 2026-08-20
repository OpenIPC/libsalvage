/* salvage-pcap — replay a captured RTP session through the FlexFEC receiver,
 * dropping packets on the way in, and check that what comes out is what went
 * in.
 *
 * The capture holds the originals, so every recovered packet can be compared
 * byte for byte against the packet it is supposed to be. That is the only
 * check worth making: a decoder that produces plausible bytes is worse than
 * one that produces none, because the damage reaches the picture silently.
 */

#include <salvage/depay.h>
#include <salvage/flexfec.h>
#include <salvage/salvage.h>

#include "../src/bits.h"
#include "../src/retarget.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTP_HDR 12
#define MAX_SEQ 65536

typedef struct {
    uint8_t *data;
    uint16_t len;
} Orig;

static Orig originals[MAX_SEQ];
static uint64_t verified, wrong, unknown;
static bool was_dropped[MAX_SEQ], was_recovered[MAX_SEQ];

/* Access-unit accounting, fed by the depacketiser behind the FEC layer. */
static SalvageDepay *depay;
static SalvageStage *stage;
static FILE *annexb;
static uint64_t pics_out, pics_repaired;
static uint64_t retarget_checked, retarget_failed;
static bool verify_retarget;

static void check_retarget(const SalvageAu *au);
static uint64_t au_total, au_complete, au_no_first_slice, au_nals,
    au_partial_nals, au_dropped_nals, au_lost_packets;

static void on_au(const SalvageAu *au, void *ctx) {
    (void)ctx;
    au_total++;
    au_complete += au->complete;
    au_no_first_slice += !au->has_first_slice;
    au_nals += au->nal_count;
    for (size_t i = 0; i < au->nal_count; i++) {
        au_partial_nals += !au->nals[i].complete;
    }
    au_dropped_nals += au->dropped_nals;
    au_lost_packets += au->lost_packets;
    if (verify_retarget) {
        check_retarget(au);
    }
    if (stage != NULL) {
        salvage_input(stage, au);
    } else if (annexb != NULL) {
        fwrite(au->data, 1, au->len, annexb);
    }
}

/* Rewrite every slice's frame_num and POC to the values it already carries.
 * The bytes must come back identical: the whole retarget path runs — emulation
 * prevention stripped and restored, every ue(v) walked to find the offsets, the
 * patch applied — so a mistake anywhere in it shows up here, across every slice
 * in the capture rather than the handful a unit test can embed. */
static void check_retarget(const SalvageAu *au) {
    static SalvageSps sps;
    static SalvagePps pps;
    uint8_t rbsp[65536], out[65536];

    for (size_t i = 0; i < au->nal_count; i++) {
        const SalvageNal *n = &au->nals[i];
        const size_t bl = n->len - 4;
        if (bl > sizeof rbsp) {
            continue;
        }
        const size_t rl =
            rbsp_unescape(au->data + n->offset + 4, bl, rbsp, sizeof rbsp);
        if (rl == 0) {
            continue;
        }
        if (n->type == 7) {
            salvage_sps_h264(rbsp, rl, &sps);
            continue;
        }
        if (n->type == 8) {
            salvage_pps_h264(rbsp, rl, &pps);
            continue;
        }
        if (!n->is_slice || !n->complete) {
            continue;
        }
        SalvageSliceHeader h;
        if (!salvage_slice_h264(rbsp, rl, &sps, &pps, &h)) {
            continue;
        }
        bits_patch(rbsp, h.frame_num_off, h.frame_num_bits, h.frame_num);
        bits_patch(rbsp, h.poc_off, h.poc_bits, h.poc_lsb);
        const size_t ol = rbsp_escape(rbsp, rl, out, sizeof out);
        retarget_checked++;
        if (ol != bl || memcmp(out, au->data + n->offset + 4, bl) != 0) {
            retarget_failed++;
        }
    }
}

static void on_pic(const uint8_t *pic, size_t len, bool repaired, void *ctx) {
    (void)ctx;
    pics_out++;
    pics_repaired += repaired;
    if (annexb != NULL) {
        fwrite(pic, 1, len, annexb);
    }
}

static void on_out(const uint8_t *pkt, size_t len, bool recovered, void *ctx) {
    (void)ctx;
    if (depay != NULL) {
        salvage_depay_input(depay, pkt, len, recovered);
    }
    if (!recovered) {
        return; /* received packets are trivially correct */
    }
    const uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    const Orig *o = &originals[seq];
    if (o->data == NULL) {
        unknown++;
        return;
    }
    was_recovered[seq] = true;
    if (o->len == len && memcmp(o->data, pkt, len) == 0) {
        verified++;
    } else {
        wrong++;
        if (wrong <= 5) {
            fprintf(stderr,
                    "  seq %u rebuilt %zu bytes, original %u — MISMATCH\n",
                    seq, len, o->len);
        }
    }
}

/* --- pcap ------------------------------------------------------------- */

typedef void (*PktCb)(const uint8_t *udp_payload, size_t len, void *ctx);

static int read_pcap(const char *path, PktCb cb, void *ctx) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    uint8_t gh[24];
    if (fread(gh, 1, sizeof gh, f) != sizeof gh) {
        fclose(f);
        return -1;
    }
    const uint32_t magic = (uint32_t)gh[0] | ((uint32_t)gh[1] << 8) |
                           ((uint32_t)gh[2] << 16) | ((uint32_t)gh[3] << 24);
    const bool le = (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d);
    uint32_t linktype;
    memcpy(&linktype, gh + 20, 4);
    if (!le) {
        linktype = __builtin_bswap32(linktype);
    }
    /* Linux "any" cooked v2 / v1, else Ethernet. */
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
        if ((ip[0] >> 4) != 4 || ip[9] != 17) { /* IPv4 / UDP */
            continue;
        }
        const size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
        const uint8_t *udp = ip + ihl;
        if ((size_t)(buf + caplen - udp) < 8) {
            continue;
        }
        /* Honour the UDP length: frames under 60 bytes are zero-padded by
         * Ethernet, and treating that padding as payload looks exactly like a
         * sender that got its lengths wrong. */
        size_t ulen = (size_t)((udp[4] << 8) | udp[5]);
        const size_t avail = (size_t)(buf + caplen - udp);
        if (ulen < 8 || ulen > avail) {
            ulen = avail;
        }
        cb(udp + 8, ulen - 8, ctx);
    }
    fclose(f);
    return 0;
}

/* --- replay ------------------------------------------------------------ */

typedef struct {
    SalvageFec *fec;
    uint8_t media_pt, fec_pt;
    unsigned drop_every;   /* drop 1 media packet in N, 0 = none */
    bool no_fec;           /* bypass recovery, to isolate its contribution */
    unsigned drop_first;   /* drop slice 0 of every Nth picture, 0 = none */
    bool h265;
    unsigned pic_index;
    bool in_first_slice;
    uint64_t first_slice_dropped;
    unsigned burst;        /* consecutive packets per drop event */
    uint64_t media_seen, dropped;
    unsigned burst_left;
} Replay;

/* Enough of RFC 6184/7798 to recognise where a picture's first slice starts
 * and ends, so the loss can be aimed at it instead of scattered. Losing slice 0
 * is the case worth testing: it is the one the decoder handles worst. */
static void classify(Replay *r, const uint8_t *pay, size_t len) {
    bool starts = false, first = false;
    if (r->h265) {
        if (len < 3) {
            return;
        }
        const uint8_t t = (uint8_t)((pay[0] >> 1) & 0x3fu);
        if (t == 49) {
            if ((pay[2] & 0x80u) == 0) {
                return; /* continuation: whatever the start said still holds */
            }
            starts = true;
            first = (pay[2] & 0x3fu) <= 31 && len > 3 && (pay[3] & 0x80u);
        } else if (t <= 47) {
            starts = true;
            first = t <= 31 && (pay[2] & 0x80u);
        }
    } else {
        if (len < 2) {
            return;
        }
        const uint8_t t = (uint8_t)(pay[0] & 0x1fu);
        if (t == 28) {
            if ((pay[1] & 0x80u) == 0) {
                return;
            }
            starts = true;
            const uint8_t orig = (uint8_t)(pay[1] & 0x1fu);
            first = (orig == 1 || orig == 5) && len > 2 && (pay[2] & 0x80u);
        } else if (t >= 1 && t <= 23) {
            starts = true;
            first = (t == 1 || t == 5) && (pay[1] & 0x80u);
        }
    }
    if (starts) {
        r->in_first_slice = first;
        if (first) {
            r->pic_index++;
        }
    }
}

static void on_pkt(const uint8_t *p, size_t len, void *ctx) {
    Replay *r = ctx;
    if (len < RTP_HDR || (p[0] >> 6) != 2) {
        return;
    }
    const uint8_t pt = (uint8_t)(p[1] & 0x7f);

    if (pt == r->media_pt) {
        const uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
        if (originals[seq].data == NULL) {
            originals[seq].data = malloc(len);
            memcpy(originals[seq].data, p, len);
            originals[seq].len = (uint16_t)len;
        }
        r->media_seen++;
        classify(r, p + RTP_HDR, len - RTP_HDR);

        if (r->drop_first && r->in_first_slice &&
            (r->pic_index % r->drop_first) == 0) {
            r->dropped++;
            r->first_slice_dropped++;
            was_dropped[seq] = true;
            return;
        }
        if (r->burst_left > 0) {
            r->burst_left--;
            r->dropped++;
            was_dropped[seq] = true;
            return;
        }
        if (r->drop_every && (r->media_seen % r->drop_every) == 0) {
            r->burst_left = r->burst > 0 ? r->burst - 1 : 0;
            r->dropped++;
            was_dropped[seq] = true;
            return;
        }
    } else if (pt != r->fec_pt) {
        return;
    }

    if (r->no_fec) {
        if (pt == r->media_pt) {
            on_out(p, len, false, NULL);
        }
        return;
    }
    salvage_fec_input(r->fec, p, len);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    SalvageCodec codec = SALVAGE_H264;
    bool salvage_off = false;
    SalvagePolicy policy = {
        .truncated = SALVAGE_TRUNCATED_FORWARD,
        .synthesise_first_slice = true,
    };
    Replay r = {.media_pt = 96, .fec_pt = 101, .burst = 1};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--media-pt") && i + 1 < argc) {
            r.media_pt = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fec-pt") && i + 1 < argc) {
            r.fec_pt = (uint8_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--drop") && i + 1 < argc) {
            r.drop_every = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--burst") && i + 1 < argc) {
            r.burst = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-fec")) {
            r.no_fec = true;
        } else if (!strcmp(argv[i], "--drop-first-slice") && i + 1 < argc) {
            r.drop_first = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--verify-retarget")) {
            verify_retarget = true;
        } else if (!strcmp(argv[i], "--no-salvage")) {
            salvage_off = true;
        } else if (!strcmp(argv[i], "--drop-truncated")) {
            policy.truncated = SALVAGE_TRUNCATED_DROP;
        } else if (!strcmp(argv[i], "--no-synth")) {
            policy.synthesise_first_slice = false;
        } else if (!strcmp(argv[i], "--h265")) {
            codec = SALVAGE_H265;
            r.h265 = true;
        } else if (!strcmp(argv[i], "--annexb") && i + 1 < argc) {
            annexb = fopen(argv[++i], "wb");
            if (annexb == NULL) {
                perror(argv[i]);
                return 1;
            }
        } else {
            path = argv[i];
        }
    }
    if (path == NULL) {
        fprintf(stderr,
                "usage: %s [--media-pt N] [--fec-pt N] [--drop N] [--burst N]"
                " [--no-fec] [--h265] [--annexb FILE]\n"
                "       [--drop-first-slice N] [--no-salvage] [--no-synth]"
                " [--drop-truncated] [--verify-retarget] capture.pcap\n",
                argv[0]);
        return 2;
    }

    if (!salvage_off) {
        stage = salvage_new(codec, policy, 0, on_pic, NULL);
        if (stage == NULL) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
    }
    depay = salvage_depay_new(codec, 0, on_au, NULL);
    r.fec = salvage_fec_new(r.media_pt, r.fec_pt, 512, on_out, NULL);
    if (r.fec == NULL || depay == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (read_pcap(path, on_pkt, &r) == -1) {
        return 1;
    }
    salvage_fec_flush(r.fec);
    salvage_depay_flush(depay);

    SalvageFecStats s;
    salvage_fec_stats(r.fec, &s);

    if (r.drop_every) {
        printf("dropped 1 media packet in %u", r.drop_every);
        if (r.burst > 1) {
            printf(", in bursts of %u", r.burst);
        }
        printf("\n");
    }
    printf("media in        : %" PRIu64 "\n", s.media_in);
    printf("repair in       : %" PRIu64 "\n", s.repair_in);
    printf("dropped by us   : %" PRIu64 "\n", r.dropped);
    printf("recovered       : %" PRIu64 "\n", s.recovered);
    printf("still missing   : %" PRIu64 "\n", s.unrecovered);
    printf("released        : %" PRIu64 "\n", s.out);
    printf("\nrecovered and byte-exact vs original : %" PRIu64 "\n", verified);
    printf("recovered but WRONG                  : %" PRIu64 "\n", wrong);
    if (unknown) {
        printf("recovered, no original to check      : %" PRIu64 "\n", unknown);
    }

    {
        unsigned shown = 0;
        printf("\nunrecovered dropped seqs:");
        for (int i = 0; i < MAX_SEQ && shown < 12; i++) {
            if (was_dropped[i] && !was_recovered[i]) {
                printf(" %d", i);
                shown++;
            }
        }
        printf("\n");
    }

    printf("\naccess units    : %" PRIu64 " (%" PRIu64 " intact, %" PRIu64
           " damaged)\n",
           au_total, au_complete, au_total - au_complete);
    printf("NAL units       : %" PRIu64 " (%" PRIu64 " truncated, %" PRIu64
           " unusable)\n",
           au_nals, au_partial_nals, au_dropped_nals);
    printf("packet gaps seen by the depacketiser : %" PRIu64 "\n",
           au_lost_packets);
    printf("pictures with no first slice : %" PRIu64 "\n", au_no_first_slice);
    if (r.first_slice_dropped) {
        printf("first-slice packets dropped  : %" PRIu64 "\n",
               r.first_slice_dropped);
    }
    if (retarget_checked) {
        printf("\nidentity retarget: %" PRIu64 " slices checked, %" PRIu64
               " changed (want 0)\n",
               retarget_checked, retarget_failed);
    }
    if (stage != NULL) {
        SalvageStats sv;
        salvage_stats(stage, &sv);
        printf("\npictures out    : %" PRIu64 " (%" PRIu64 " repaired)\n",
               pics_out, pics_repaired);
        printf("first slice synthesised : %" PRIu64 "\n",
               sv.first_slice_synthesised);
        printf("first slice unrepaired  : %" PRIu64 "\n",
               sv.first_slice_unrepaired);
        printf("truncated slices        : %" PRIu64 " forwarded, %" PRIu64
               " dropped\n",
               sv.truncated_forwarded, sv.truncated_dropped);
    }

    if (r.dropped) {
        printf("\nrecovery rate: %.1f%% of dropped packets\n",
               100.0 * (double)s.recovered / (double)r.dropped);
    }

    salvage_fec_free(r.fec);
    salvage_depay_free(depay);
    salvage_free(stage);
    if (annexb != NULL) {
        fclose(annexb);
    }
    for (int i = 0; i < MAX_SEQ; i++) {
        free(originals[i].data);
    }
    return (wrong > 0 || retarget_failed > 0) ? 1 : 0;
}
