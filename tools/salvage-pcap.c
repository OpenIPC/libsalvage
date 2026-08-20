/* salvage-pcap — replay a captured RTP session through the FlexFEC receiver,
 * dropping packets on the way in, and check that what comes out is what went
 * in.
 *
 * The capture holds the originals, so every recovered packet can be compared
 * byte for byte against the packet it is supposed to be. That is the only
 * check worth making: a decoder that produces plausible bytes is worse than
 * one that produces none, because the damage reaches the picture silently.
 */

#include <salvage/flexfec.h>

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

static void on_out(const uint8_t *pkt, size_t len, bool recovered, void *ctx) {
    (void)ctx;
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
    unsigned burst;        /* consecutive packets per drop event */
    uint64_t media_seen, dropped;
    unsigned burst_left;
} Replay;

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

    salvage_fec_input(r->fec, p, len);
}

int main(int argc, char **argv) {
    const char *path = NULL;
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
        } else {
            path = argv[i];
        }
    }
    if (path == NULL) {
        fprintf(stderr,
                "usage: %s [--media-pt N] [--fec-pt N] [--drop N] [--burst N]"
                " capture.pcap\n",
                argv[0]);
        return 2;
    }

    r.fec = salvage_fec_new(r.media_pt, r.fec_pt, 512, on_out, NULL);
    if (r.fec == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (read_pcap(path, on_pkt, &r) == -1) {
        return 1;
    }
    salvage_fec_flush(r.fec);

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

    if (r.dropped) {
        printf("\nrecovery rate: %.1f%% of dropped packets\n",
               100.0 * (double)s.recovered / (double)r.dropped);
    }

    salvage_fec_free(r.fec);
    for (int i = 0; i < MAX_SEQ; i++) {
        free(originals[i].data);
    }
    return wrong > 0 ? 1 : 0;
}
