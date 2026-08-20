/* Unit tests for the FlexFEC receiver.
 *
 * The repair packets here are built from RFC 8627 directly rather than by
 * calling any shared helper, so an encoder and a decoder that agree on the
 * same misreading of the spec cannot both pass.
 */

#include <salvage/flexfec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTP_HDR 12
#define MEDIA_PT 96
#define FEC_PT 101
#define SSRC 0x89abcdefu /* top bit set: catches sign-extension in TS/SSRC */

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

/* --- packet construction ---------------------------------------------- */

typedef struct {
    uint8_t data[1500];
    size_t len;
} Pkt;

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static Pkt make_media(uint16_t seq, uint32_t ts, bool marker, size_t plen) {
    Pkt p = {0};
    p.data[0] = 0x80; /* V=2 */
    p.data[1] = (uint8_t)((marker ? 0x80u : 0u) | MEDIA_PT);
    put16(p.data + 2, seq);
    put32(p.data + 4, ts);
    put32(p.data + 8, SSRC);
    for (size_t i = 0; i < plen; i++) {
        /* Content varies with the sequence number so a packet rebuilt from the
         * wrong group cannot accidentally compare equal. */
        p.data[RTP_HDR + i] = (uint8_t)(seq * 7u + i * 31u + 1u);
    }
    p.len = RTP_HDR + plen;
    return p;
}

/* RFC 8627 §6.3.3 flexible mask form, R=0 F=0. */
static Pkt make_repair(
    const Pkt *const *members, size_t n, uint16_t sn_base, uint16_t seq) {
    uint8_t bits = 0, pt = 0;
    uint16_t length = 0;
    uint32_t ts = 0;
    uint8_t body[1500] = {0};
    size_t body_len = 0;
    uint16_t max_off = 0;

    for (size_t i = 0; i < n; i++) {
        const uint8_t *m = members[i]->data;
        const uint16_t mseq = (uint16_t)((m[2] << 8) | m[3]);
        const uint16_t off = (uint16_t)(mseq - sn_base);
        const size_t plen = members[i]->len - RTP_HDR;
        if (off > max_off) {
            max_off = off;
        }
        bits ^= (uint8_t)((m[0] & 0x3fu) | ((m[1] & 0x80u) >> 1));
        pt ^= (uint8_t)(m[1] & 0x7fu);
        length ^= (uint16_t)plen;
        ts ^= ((uint32_t)m[4] << 24) | ((uint32_t)m[5] << 16) |
              ((uint32_t)m[6] << 8) | (uint32_t)m[7];
        for (size_t b = 0; b < plen; b++) {
            body[b] ^= m[RTP_HDR + b];
        }
        if (plen > body_len) {
            body_len = plen;
        }
    }

    Pkt p = {0};
    p.data[0] = 0x80;
    p.data[1] = FEC_PT; /* M=0 on repair packets */
    put16(p.data + 2, seq);
    put32(p.data + 4, 0);
    put32(p.data + 8, SSRC + 1u); /* repair flow has its own SSRC */

    uint8_t *f = p.data + RTP_HDR;
    f[0] = (uint8_t)(bits & 0x3fu); /* R=0, F=0 */
    f[1] = (uint8_t)(((bits & 0x40u) << 1) | (pt & 0x7fu));
    put16(f + 2, length);
    put32(f + 4, ts);
    put16(f + 8, sn_base);

    size_t off = 10;
    if (max_off < 15) {
        uint16_t w = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *m = members[i]->data;
            const uint16_t o =
                (uint16_t)((uint16_t)((m[2] << 8) | m[3]) - sn_base);
            w |= (uint16_t)(1u << (14 - o));
        }
        put16(f + off, w);
        off += 2;
    } else {
        /* k=1 in the first word chains a second, 31-bit word (offsets 15-45). */
        uint16_t w1 = 0x8000u;
        uint32_t w2 = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *m = members[i]->data;
            const uint16_t o =
                (uint16_t)((uint16_t)((m[2] << 8) | m[3]) - sn_base);
            if (o < 15) {
                w1 |= (uint16_t)(1u << (14 - o));
            } else {
                w2 |= (uint32_t)1u << (30 - (o - 15));
            }
        }
        put16(f + off, w1);
        off += 2;
        put32(f + off, w2);
        off += 4;
    }

    memcpy(f + off, body, body_len);
    p.len = RTP_HDR + off + body_len;
    return p;
}

/* --- output capture ---------------------------------------------------- */

typedef struct {
    uint16_t seq[512];
    bool recovered[512];
    Pkt pkt[512];
    size_t n;
} Out;

static void on_out(const uint8_t *p, size_t len, bool recovered, void *ctx) {
    Out *o = ctx;
    if (o->n >= 512) {
        return;
    }
    o->seq[o->n] = (uint16_t)((p[2] << 8) | p[3]);
    o->recovered[o->n] = recovered;
    memcpy(o->pkt[o->n].data, p, len);
    o->pkt[o->n].len = len;
    o->n++;
}

static const Pkt *find_out(const Out *o, uint16_t seq) {
    for (size_t i = 0; i < o->n; i++) {
        if (o->seq[i] == seq) {
            return &o->pkt[i];
        }
    }
    return NULL;
}

static bool same(const Pkt *a, const Pkt *b) {
    return a != NULL && b != NULL && a->len == b->len &&
           memcmp(a->data, b->data, a->len) == 0;
}

/* --- tests -------------------------------------------------------------- */

static void test_passthrough(void) {
    printf("  no loss: everything released once, in order, unmarked\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 8, on_out, &o);

    Pkt m[6];
    for (int i = 0; i < 6; i++) {
        m[i] = make_media((uint16_t)(100 + i), 9000, i == 5, 40);
        salvage_fec_input(f, m[i].data, m[i].len);
    }
    salvage_fec_flush(f);

    CHECK(o.n == 6, "released %zu, want 6", o.n);
    for (size_t i = 0; i < o.n; i++) {
        CHECK(o.seq[i] == 100 + i, "out[%zu] seq %u, want %zu", i, o.seq[i],
              100 + i);
        CHECK(!o.recovered[i], "seq %u wrongly flagged recovered", o.seq[i]);
        CHECK(same(&o.pkt[i], &m[i]), "seq %u altered in transit", o.seq[i]);
    }

    SalvageFecStats s;
    salvage_fec_stats(f, &s);
    CHECK(s.recovered == 0 && s.unrecovered == 0, "recovered %llu lost %llu",
          (unsigned long long)s.recovered, (unsigned long long)s.unrecovered);
    salvage_fec_free(f);
}

static void test_single_loss(void) {
    printf("  one loss in a group: rebuilt byte-exact\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 16, on_out, &o);

    /* Deliberately mixed lengths and marker bits: length and M are recovery
     * fields, and a receiver that ignores them still rebuilds the payload. */
    Pkt a = make_media(200, 0xdeadbeefu, false, 40);
    Pkt b = make_media(201, 0xdeadbeefu, true, 73);
    const Pkt *g[] = {&a, &b};
    Pkt rep = make_repair(g, 2, 200, 7000);

    salvage_fec_input(f, a.data, a.len); /* 201 is lost */
    salvage_fec_input(f, rep.data, rep.len);
    salvage_fec_flush(f);

    const Pkt *got = find_out(&o, 201);
    CHECK(got != NULL, "seq 201 never released");
    CHECK(same(got, &b), "seq 201 rebuilt wrong (%zu bytes, want %zu)",
          got ? got->len : 0, b.len);
    salvage_fec_free(f);
}

static void test_newest_packet_lost(void) {
    /* Regression: the window used to be sized from the highest sequence number
     * that ARRIVED, so a loss at the leading edge sat exactly one slot beyond
     * reach and its repair packet was discarded as "aged out". Nothing but a
     * real capture caught this, because it needs the window to be full. */
    printf("  loss at the leading edge of a full window: still recovered\n");
    Out o = {0};
    const unsigned W = 8;
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, W, on_out, &o);

    Pkt m[9];
    for (unsigned i = 0; i < 9; i++) {
        m[i] = make_media((uint16_t)(300 + i), 9000, false, 40);
    }
    for (unsigned i = 0; i < W; i++) { /* 300..307 arrive, filling the window */
        salvage_fec_input(f, m[i].data, m[i].len);
    }

    /* 308 is lost, and nothing later ever arrives to push the window along. */
    const Pkt *g[] = {&m[7], &m[8]};
    Pkt rep = make_repair(g, 2, 307, 7100);
    salvage_fec_input(f, rep.data, rep.len);
    salvage_fec_flush(f);

    const Pkt *got = find_out(&o, 308);
    CHECK(got != NULL, "seq 308 never released");
    CHECK(same(got, &m[8]), "seq 308 rebuilt wrong");
    salvage_fec_free(f);
}

static void test_two_losses(void) {
    printf("  two losses in one group: reported lost, nothing invented\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 16, on_out, &o);

    Pkt a = make_media(400, 9000, false, 40);
    Pkt b = make_media(401, 9000, false, 40);
    Pkt c = make_media(402, 9000, false, 40);
    const Pkt *g[] = {&a, &b, &c};
    Pkt rep = make_repair(g, 3, 400, 7200);

    salvage_fec_input(f, a.data, a.len); /* 401 and 402 both lost */
    salvage_fec_input(f, rep.data, rep.len);
    salvage_fec_flush(f);

    CHECK(find_out(&o, 401) == NULL, "seq 401 invented from an ambiguous group");
    CHECK(find_out(&o, 402) == NULL, "seq 402 invented from an ambiguous group");
    salvage_fec_free(f);
}

static void test_cascade(void) {
    printf("  cascading recovery: one rebuild completes the next group\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 16, on_out, &o);

    Pkt m[4];
    for (int i = 0; i < 4; i++) {
        m[i] = make_media((uint16_t)(500 + i), 9000, false, 48);
    }
    const Pkt *ga[] = {&m[0], &m[1]}; /* {500,501} */
    const Pkt *gb[] = {&m[1], &m[2]}; /* {501,502} */
    const Pkt *gc[] = {&m[2], &m[3]}; /* {502,503} */
    Pkt ra = make_repair(ga, 2, 500, 7300);
    Pkt rb = make_repair(gb, 2, 501, 7301);
    Pkt rc = make_repair(gc, 2, 502, 7302);

    /* Only 500 survives; 501, 502 and 503 must fall out one after another. */
    salvage_fec_input(f, m[0].data, m[0].len);
    salvage_fec_input(f, rc.data, rc.len); /* two missing: must wait */
    salvage_fec_input(f, rb.data, rb.len); /* two missing: must wait */
    salvage_fec_input(f, ra.data, ra.len); /* unblocks the chain */
    salvage_fec_flush(f);

    for (int i = 1; i < 4; i++) {
        const Pkt *got = find_out(&o, (uint16_t)(500 + i));
        CHECK(got != NULL, "seq %d never released", 500 + i);
        CHECK(same(got, &m[i]), "seq %d rebuilt wrong", 500 + i);
    }
    salvage_fec_free(f);
}

static void test_wide_mask(void) {
    printf("  offset past 15: second mask word decoded\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 64, on_out, &o);

    Pkt a = make_media(600, 9000, false, 40);
    Pkt b = make_media(620, 9000, false, 40); /* offset 20 */
    const Pkt *g[] = {&a, &b};
    Pkt rep = make_repair(g, 2, 600, 7400);

    salvage_fec_input(f, a.data, a.len);
    salvage_fec_input(f, rep.data, rep.len);
    salvage_fec_flush(f);

    const Pkt *got = find_out(&o, 620);
    CHECK(got != NULL, "seq 620 never released");
    CHECK(same(got, &b), "seq 620 rebuilt wrong");
    salvage_fec_free(f);
}

static void test_reordering(void) {
    printf("  repair ahead of its members: held, then applied\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 16, on_out, &o);

    Pkt a = make_media(700, 9000, false, 40);
    Pkt b = make_media(701, 9000, false, 55);
    const Pkt *g[] = {&a, &b};
    Pkt rep = make_repair(g, 2, 700, 7500);

    salvage_fec_input(f, rep.data, rep.len); /* arrives first */
    salvage_fec_input(f, a.data, a.len);     /* 701 is lost */
    salvage_fec_flush(f);

    const Pkt *got = find_out(&o, 701);
    CHECK(got != NULL, "seq 701 never released");
    CHECK(same(got, &b), "seq 701 rebuilt wrong");
    salvage_fec_free(f);
}

static void test_duplicate_and_late(void) {
    printf("  duplicates and packets behind the window: ignored\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 4, on_out, &o);

    Pkt m[8];
    for (int i = 0; i < 8; i++) {
        m[i] = make_media((uint16_t)(800 + i), 9000, false, 40);
        salvage_fec_input(f, m[i].data, m[i].len);
    }
    /* 800 was released four packets ago; replaying it must not resurrect it
     * into a slot that now belongs to a live sequence number. */
    salvage_fec_input(f, m[0].data, m[0].len);
    salvage_fec_input(f, m[7].data, m[7].len); /* duplicate of the newest */
    salvage_fec_flush(f);

    size_t seen800 = 0, seen807 = 0;
    for (size_t i = 0; i < o.n; i++) {
        seen800 += o.seq[i] == 800;
        seen807 += o.seq[i] == 807;
    }
    CHECK(seen800 == 1, "seq 800 released %zu times, want 1", seen800);
    CHECK(seen807 == 1, "seq 807 released %zu times, want 1", seen807);
    for (size_t i = 1; i < o.n; i++) {
        CHECK((uint16_t)(o.seq[i] - o.seq[i - 1]) == 1,
              "output out of order at %zu: %u then %u", i, o.seq[i - 1],
              o.seq[i]);
    }
    salvage_fec_free(f);
}

static void test_garbage(void) {
    printf("  malformed input: rejected without crashing\n");
    Out o = {0};
    SalvageFec *f = salvage_fec_new(MEDIA_PT, FEC_PT, 16, on_out, &o);

    Pkt a = make_media(900, 9000, false, 40);
    salvage_fec_input(f, a.data, a.len);

    uint8_t junk[64];
    memset(junk, 0xff, sizeof junk);
    salvage_fec_input(f, junk, 3);           /* shorter than an RTP header */
    salvage_fec_input(f, junk, sizeof junk); /* version 3, R and F set */

    uint8_t trunc[RTP_HDR + 8];
    memset(trunc, 0, sizeof trunc);
    trunc[0] = 0x80;
    trunc[1] = FEC_PT;
    salvage_fec_input(f, trunc, sizeof trunc); /* repair header cut short */

    salvage_fec_flush(f);
    CHECK(o.n == 1 && o.seq[0] == 900, "released %zu packets, want just 900",
          o.n);
    salvage_fec_free(f);
}

int main(void) {
    printf("flexfec receiver\n");
    test_passthrough();
    test_single_loss();
    test_newest_packet_lost();
    test_two_losses();
    test_cascade();
    test_wide_mask();
    test_reordering();
    test_duplicate_and_late();
    test_garbage();

    if (failures > 0) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
