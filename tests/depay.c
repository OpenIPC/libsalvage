/* Unit tests for the RTP depacketiser.
 *
 * Most of these are about damage, because that is the part an ordinary
 * depacketiser does not have to get right. The rules being pinned down:
 * a NAL cut short by loss is delivered and marked, a NAL whose head was lost
 * is not delivered at all, and fragments either side of a hole are never
 * spliced together.
 */

#include <salvage/depay.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTP_HDR 12

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("    FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* first_mb_in_slice is ue(v): 0 codes as a single 1 bit, so any byte with the
 * top bit set opens the picture's first slice. A larger value has at least
 * eight leading zeros, hence 0x00. */
#define SLICE_FIRST 0x88
#define SLICE_LATER 0x00

/* --- captured output ---------------------------------------------------- */

typedef struct {
    uint8_t data[65536];
    size_t len;
    uint32_t ts;
    SalvageNal nals[64];
    size_t nal_count;
    bool complete, has_first_slice;
    unsigned lost, dropped;
} Au;

typedef struct {
    Au au[32];
    size_t n;
} Caught;

static void on_au(const SalvageAu *au, void *ctx) {
    Caught *c = ctx;
    if (c->n >= 32 || au->len > 65536 || au->nal_count > 64) {
        return;
    }
    Au *d = &c->au[c->n++];
    memcpy(d->data, au->data, au->len);
    d->len = au->len;
    d->ts = au->ts;
    memcpy(d->nals, au->nals, au->nal_count * sizeof *au->nals);
    d->nal_count = au->nal_count;
    d->complete = au->complete;
    d->has_first_slice = au->has_first_slice;
    d->lost = au->lost_packets;
    d->dropped = au->dropped_nals;
}

/* --- packet construction ------------------------------------------------ */

static uint8_t pktbuf[2048];

static const uint8_t *rtp(
    uint16_t seq, uint32_t ts, bool marker, const uint8_t *payload, size_t plen,
    size_t *out_len) {
    pktbuf[0] = 0x80;
    pktbuf[1] = (uint8_t)((marker ? 0x80u : 0u) | 96u);
    pktbuf[2] = (uint8_t)(seq >> 8);
    pktbuf[3] = (uint8_t)seq;
    pktbuf[4] = (uint8_t)(ts >> 24);
    pktbuf[5] = (uint8_t)(ts >> 16);
    pktbuf[6] = (uint8_t)(ts >> 8);
    pktbuf[7] = (uint8_t)ts;
    memset(pktbuf + 8, 0x11, 4); /* SSRC */
    memcpy(pktbuf + RTP_HDR, payload, plen);
    *out_len = RTP_HDR + plen;
    return pktbuf;
}

static void feed(
    SalvageDepay *d, uint16_t seq, uint32_t ts, bool marker,
    const uint8_t *payload, size_t plen) {
    size_t len;
    const uint8_t *p = rtp(seq, ts, marker, payload, plen, &len);
    salvage_depay_input(d, p, len, false);
}

/* --- tests -------------------------------------------------------------- */

static void test_fu_reassembly(void) {
    printf("  FU-A reassembly: one NAL, Annex-B, header rebuilt\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    /* NAL type 5 (IDR), nri 3, split into three fragments. */
    const uint8_t f1[] = {0x7c, 0x85, SLICE_FIRST, 0xaa, 0xbb};
    const uint8_t f2[] = {0x7c, 0x05, 0xcc, 0xdd};
    const uint8_t f3[] = {0x7c, 0x45, 0xee};
    feed(d, 1, 1000, false, f1, sizeof f1);
    feed(d, 2, 1000, false, f2, sizeof f2);
    feed(d, 3, 1000, false, f3, sizeof f3);
    salvage_depay_flush(d);

    CHECK(c.n == 1, "%zu access units, want 1", c.n);
    if (c.n == 1) {
        const Au *a = &c.au[0];
        CHECK(a->nal_count == 1, "%zu NALs, want 1", a->nal_count);
        CHECK(a->complete, "AU flagged damaged");
        CHECK(a->has_first_slice, "first slice not detected");
        const uint8_t want[] = {0,    0,    0,    1,    0x65, SLICE_FIRST,
                                0xaa, 0xbb, 0xcc, 0xdd, 0xee};
        CHECK(a->len == sizeof want && memcmp(a->data, want, a->len) == 0,
              "reassembled %zu bytes, want %zu", a->len, sizeof want);
        CHECK(a->nals[0].type == 5, "type %u, want 5", a->nals[0].type);
        CHECK(a->nals[0].complete, "NAL flagged truncated");
    }
    salvage_depay_free(d);
}

static void test_slices_are_one_picture(void) {
    printf("  per-NAL marker bits: slices still form one picture\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    /* The majestic/smolrtsp sender sets the marker at the end of every NAL,
     * not every access unit. A receiver that believes it turns each slice into
     * its own picture, which is how "hardware decoders cannot do slices" gets
     * diagnosed when the real fault is in the sender. */
    for (int i = 0; i < 8; i++) {
        const uint8_t s[] = {0x41, i == 0 ? SLICE_FIRST : SLICE_LATER,
                             (uint8_t)i};
        feed(d, (uint16_t)(10 + i), 2000, true /* marker on every slice */, s,
             sizeof s);
    }
    salvage_depay_flush(d);

    CHECK(c.n == 1, "%zu access units, want 1 picture of 8 slices", c.n);
    if (c.n == 1) {
        CHECK(c.au[0].nal_count == 8, "%zu slices, want 8", c.au[0].nal_count);
        CHECK(c.au[0].has_first_slice, "first slice not detected");
        unsigned firsts = 0;
        for (size_t i = 0; i < c.au[0].nal_count; i++) {
            firsts += c.au[0].nals[i].first_slice;
        }
        CHECK(firsts == 1, "%u slices claim to be first, want 1", firsts);
    }
    salvage_depay_free(d);
}

static void test_honest_marker(void) {
    printf("  honest marker bits: earn trust, then end the picture\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    const uint8_t s[] = {0x41, SLICE_FIRST, 0x00};
    for (int i = 0; i < 5; i++) { /* one slice per picture, marker on each */
        feed(d, (uint16_t)(20 + i), (uint32_t)(3000 + i * 100), true, s,
             sizeof s);
    }
    /* Two honest markers are needed before the marker is believed, so the
     * first two pictures are emitted a packet late and the rest on the
     * marker. Either way the count and the timestamps must be right. */
    salvage_depay_flush(d);
    CHECK(c.n == 5, "%zu access units, want 5", c.n);
    for (size_t i = 0; i < c.n && i < 5; i++) {
        CHECK(c.au[i].ts == 3000 + i * 100, "AU %zu ts %u, want %zu", i,
              c.au[i].ts, 3000 + i * 100);
    }
    salvage_depay_free(d);
}

static void test_truncated_fu(void) {
    printf("  loss inside a fragmented NAL: truncated, never spliced\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    const uint8_t f1[] = {0x7c, 0x81, SLICE_FIRST, 0xaa};
    const uint8_t f3[] = {0x7c, 0x41, 0xee}; /* fragment 2 was lost */
    feed(d, 30, 4000, false, f1, sizeof f1);
    feed(d, 32, 4000, false, f3, sizeof f3);
    salvage_depay_flush(d);

    CHECK(c.n == 1, "%zu access units, want 1", c.n);
    if (c.n == 1) {
        const Au *a = &c.au[0];
        CHECK(a->nal_count == 1, "%zu NALs, want 1", a->nal_count);
        CHECK(!a->complete, "AU with a hole reported intact");
        CHECK(a->lost == 1, "lost %u packets, want 1", a->lost);
        CHECK(!a->nals[0].complete, "truncated NAL reported complete");
        /* 0xee must NOT be present: it followed the hole, and appending it
         * would put bytes next to each other that never were. */
        const uint8_t want[] = {0, 0, 0, 1, 0x61, SLICE_FIRST, 0xaa};
        CHECK(a->len == sizeof want && memcmp(a->data, want, a->len) == 0,
              "kept %zu bytes, want %zu — data after the hole was spliced in",
              a->len, sizeof want);
    }
    salvage_depay_free(d);
}

static void test_lost_fu_head(void) {
    printf("  loss of a fragmented NAL's head: whole NAL discarded\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    /* A good slice, then one whose leading fragment never arrives. Without the
     * head there is no slice header, so the payload cannot be placed in the
     * picture and is worth nothing to a decoder. */
    const uint8_t good[] = {0x41, SLICE_FIRST, 0x01};
    const uint8_t mid[] = {0x7c, 0x01, 0xaa};
    const uint8_t end[] = {0x7c, 0x41, 0xbb};
    feed(d, 40, 5000, false, good, sizeof good);
    feed(d, 42, 5000, false, mid, sizeof mid); /* 41 was the start fragment */
    feed(d, 43, 5000, false, end, sizeof end);
    salvage_depay_flush(d);

    CHECK(c.n == 1, "%zu access units, want 1", c.n);
    if (c.n == 1) {
        const Au *a = &c.au[0];
        CHECK(a->nal_count == 1, "%zu NALs, want 1 (headless one dropped)",
              a->nal_count);
        CHECK(a->dropped == 1, "dropped %u NALs, want 1", a->dropped);
        CHECK(!a->complete, "AU missing a NAL reported intact");
        const uint8_t want[] = {0, 0, 0, 1, 0x41, SLICE_FIRST, 0x01};
        CHECK(a->len == sizeof want && memcmp(a->data, want, a->len) == 0,
              "emitted %zu bytes, want %zu", a->len, sizeof want);
    }
    salvage_depay_free(d);
}

static void test_stap(void) {
    printf("  STAP-A: aggregated NALs split out\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    /* SPS and PPS bundled into one packet, then a slice. */
    const uint8_t stap[] = {0x78, 0x00, 0x03, 0x67, 0x42, 0x00,
                            0x00, 0x02, 0x68, 0xce};
    const uint8_t slice[] = {0x65, SLICE_FIRST, 0x01};
    feed(d, 50, 6000, false, stap, sizeof stap);
    feed(d, 51, 6000, false, slice, sizeof slice);
    salvage_depay_flush(d);

    CHECK(c.n == 1, "%zu access units, want 1", c.n);
    if (c.n == 1) {
        const Au *a = &c.au[0];
        CHECK(a->nal_count == 3, "%zu NALs, want 3", a->nal_count);
        if (a->nal_count == 3) {
            CHECK(a->nals[0].type == 7 && a->nals[1].type == 8 &&
                      a->nals[2].type == 5,
                  "types %u/%u/%u, want 7/8/5", a->nals[0].type,
                  a->nals[1].type, a->nals[2].type);
            CHECK(!a->nals[0].is_slice && a->nals[2].is_slice,
                  "parameter sets and slices confused");
        }
        const uint8_t want[] = {0, 0,    0,    1,    0x67, 0x42, 0x00, 0,
                                0, 0,    1,    0x68, 0xce, 0,    0,    0,
                                1, 0x65, SLICE_FIRST, 0x01};
        CHECK(a->len == sizeof want && memcmp(a->data, want, a->len) == 0,
              "emitted %zu bytes, want %zu", a->len, sizeof want);
    }
    salvage_depay_free(d);
}

static void test_h265_fu(void) {
    printf("  H.265 FU: two-byte NAL header rebuilt\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H265, 0, on_au, &c);

    /* Payload header type 49, layer 0, tid 1; fragmented unit type 19 (IDR_W_RADL). */
    const uint8_t f1[] = {0x62, 0x01, 0x93, SLICE_FIRST, 0xaa};
    const uint8_t f2[] = {0x62, 0x01, 0x53, 0xbb};
    feed(d, 60, 7000, false, f1, sizeof f1);
    feed(d, 61, 7000, false, f2, sizeof f2);
    salvage_depay_flush(d);

    CHECK(c.n == 1, "%zu access units, want 1", c.n);
    if (c.n == 1) {
        const Au *a = &c.au[0];
        CHECK(a->nal_count == 1, "%zu NALs, want 1", a->nal_count);
        CHECK(a->nals[0].type == 19, "type %u, want 19", a->nals[0].type);
        CHECK(a->nals[0].is_slice, "IDR_W_RADL not treated as a slice");
        CHECK(a->has_first_slice, "first slice segment not detected");
        /* 19 << 1 = 0x26, keeping the forbidden bit and layer-id MSB from the
         * payload header. */
        const uint8_t want[] = {0, 0, 0, 1, 0x26, 0x01, SLICE_FIRST, 0xaa, 0xbb};
        CHECK(a->len == sizeof want && memcmp(a->data, want, a->len) == 0,
              "reassembled %zu bytes, want %zu", a->len, sizeof want);
    }
    salvage_depay_free(d);
}

static void test_header_extension(void) {
    printf("  CSRC list and header extension: skipped, not decoded as video\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    uint8_t p[64];
    memset(p, 0, sizeof p);
    p[0] = 0x80 | 0x10 | 1; /* V=2, X=1, CC=1 */
    p[1] = 96;
    p[3] = 70;              /* seq */
    p[7] = 90;              /* ts */
    memset(p + 8, 0x11, 4); /* SSRC */
    memset(p + 12, 0x22, 4);/* one CSRC */
    p[16] = 0xbe;
    p[17] = 0xde;
    p[18] = 0x00;
    p[19] = 0x01; /* one extension word */
    memset(p + 20, 0x33, 4);
    p[24] = 0x41;
    p[25] = SLICE_FIRST;
    p[26] = 0x07;
    salvage_depay_input(d, p, 27, false);
    salvage_depay_flush(d);

    CHECK(c.n == 1, "%zu access units, want 1", c.n);
    if (c.n == 1) {
        const uint8_t want[] = {0, 0, 0, 1, 0x41, SLICE_FIRST, 0x07};
        CHECK(c.au[0].len == sizeof want &&
                  memcmp(c.au[0].data, want, c.au[0].len) == 0,
              "emitted %zu bytes, want %zu — extension bytes leaked into the "
              "bitstream",
              c.au[0].len, sizeof want);
    }
    salvage_depay_free(d);
}

static void test_garbage(void) {
    printf("  malformed input: rejected without crashing\n");
    Caught c = {0};
    SalvageDepay *d = salvage_depay_new(SALVAGE_H264, 0, on_au, &c);

    uint8_t junk[32];
    memset(junk, 0xff, sizeof junk);
    salvage_depay_input(d, junk, 4, false);           /* too short */
    salvage_depay_input(d, junk, sizeof junk, false); /* version 3 */

    uint8_t p[RTP_HDR + 1];
    memset(p, 0, sizeof p);
    p[0] = 0x80;
    p[1] = 96;
    p[RTP_HDR] = 0x7c; /* FU-A indicator with no FU header behind it */
    salvage_depay_input(d, p, sizeof p, false);

    salvage_depay_flush(d);
    CHECK(c.n == 0, "%zu access units from garbage, want 0", c.n);
    salvage_depay_free(d);
}

int main(void) {
    printf("rtp depacketiser\n");
    test_fu_reassembly();
    test_slices_are_one_picture();
    test_honest_marker();
    test_truncated_fu();
    test_lost_fu_head();
    test_stap();
    test_h265_fu();
    test_header_extension();
    test_garbage();

    if (failures > 0) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
