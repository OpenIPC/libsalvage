/* Unit tests for slice-header surgery and the salvage stage.
 *
 * The load-bearing test is the identity retarget: rewriting a slice's
 * frame_num and POC to the values it already has must give back the original
 * bytes exactly. It exercises the whole path — emulation prevention removed and
 * restored, every ue(v) walked to find the offsets, the patch applied — and any
 * mistake in any of them shows up as a byte difference. A repair that produces
 * plausible-but-wrong bytes is the failure mode worth fearing, because it
 * reaches the picture silently.
 */

#include <salvage/salvage.h>

#include "../src/bits.h"
#include "../src/retarget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "streamdata.h"

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

/* --- bitstream primitives ----------------------------------------------- */

static void test_rbsp(void) {
    printf("  emulation prevention: round trip, both directions\n");

    /* 00 00 01 in the payload must become 00 00 03 01 on the wire. */
    const uint8_t rbsp[] = {0x65, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0xff};
    uint8_t nal[32], back[32];
    const size_t n = rbsp_escape(rbsp, sizeof rbsp, nal, sizeof nal);
    CHECK(n == sizeof rbsp + 2, "escaped to %zu bytes, want %zu", n,
          sizeof rbsp + 2);
    const uint8_t want[] = {0x65, 0x00, 0x00, 0x03, 0x01,
                            0x00, 0x00, 0x03, 0x02, 0xff};
    CHECK(n == sizeof want && memcmp(nal, want, n) == 0, "wrong escaping");

    const size_t m = rbsp_unescape(nal, n, back, sizeof back);
    CHECK(m == sizeof rbsp && memcmp(back, rbsp, m) == 0,
          "round trip lost data (%zu bytes back, want %zu)", m, sizeof rbsp);

    /* A real slice must survive the same round trip untouched. */
    uint8_t r2[256], n2[256];
    const size_t a =
        rbsp_unescape(SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, r2, sizeof r2);
    const size_t b = rbsp_escape(r2, a, n2, sizeof n2);
    CHECK(b == sizeof SLICE_FIRST_NAL &&
              memcmp(n2, SLICE_FIRST_NAL, b) == 0,
          "real slice changed under a round trip");
}

static void test_bit_patch(void) {
    printf("  bit patching: unaligned fields, neighbours untouched\n");
    /* Eight bits straddling a byte boundary: 0xa5 written at bit 4 lands as
     * 1010 in the low nibble of byte 0 and 0101 in the high nibble of byte 1,
     * and must set as well as clear. */
    uint8_t buf[4] = {0xff, 0x00, 0xff, 0x00};
    bits_patch(buf, 4, 8, 0xa5);
    CHECK(buf[0] == 0xfa && buf[1] == 0x50,
          "patch across a byte boundary wrote %02x %02x, want fa 50", buf[0],
          buf[1]);

    /* Five bits at bit 20 — four in byte 2, one in byte 3 — and the bits on
     * either side must not move. */
    buf[2] = 0xff;
    buf[3] = 0xff;
    bits_patch(buf, 20, 5, 0x0a); /* 01010 */
    CHECK(buf[2] == 0xf5 && buf[3] == 0x7f,
          "patch wrote %02x %02x, want f5 7f", buf[2], buf[3]);

    /* A full-width value must round trip through a reader. */
    uint8_t w[4] = {0};
    bits_patch(w, 3, 16, 0xbeef);
    BitReader r;
    br_init(&r, w, sizeof w);
    br_u(&r, 3);
    CHECK(br_u(&r, 16) == 0xbeef, "16-bit patch did not read back");
}

/* --- parameter sets ------------------------------------------------------ */

static SalvageSps sps;
static SalvagePps pps;

static void test_parameter_sets(void) {
    printf("  SPS/PPS from a real camera: fields as the stream declares them\n");
    uint8_t r[256];
    size_t n = rbsp_unescape(SPS, sizeof SPS, r, sizeof r);
    CHECK(salvage_sps_h264(r, n, &sps), "SPS rejected");
    if (sps.valid) {
        CHECK(sps.pic_order_cnt_type == 0, "pic_order_cnt_type %u, want 0",
              sps.pic_order_cnt_type);
        CHECK(sps.frame_mbs_only, "frame_mbs_only_flag clear");
        CHECK(!sps.separate_colour_plane, "separate_colour_plane_flag set");
        CHECK(sps.log2_max_frame_num >= 4 && sps.log2_max_frame_num <= 16,
              "log2_max_frame_num %u out of range", sps.log2_max_frame_num);
        CHECK(sps.log2_max_poc_lsb >= 4 && sps.log2_max_poc_lsb <= 16,
              "log2_max_poc_lsb %u out of range", sps.log2_max_poc_lsb);
    }
    n = rbsp_unescape(PPS, sizeof PPS, r, sizeof r);
    CHECK(salvage_pps_h264(r, n, &pps), "PPS rejected");
}

static void test_slice_headers(void) {
    printf("  slice headers: first and later slices told apart\n");
    uint8_t r[256];
    SalvageSliceHeader h;

    size_t n =
        rbsp_unescape(SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, r, sizeof r);
    CHECK(salvage_slice_h264(r, n, &sps, &pps, &h), "first slice rejected");
    CHECK(h.first_mb == 0, "first_mb_in_slice %u, want 0", h.first_mb);
    CHECK(h.has_frame_num && h.has_poc, "frame_num/POC not located");
    const uint32_t fn = h.frame_num, poc = h.poc_lsb;

    n = rbsp_unescape(SLICE_LATER_NAL, sizeof SLICE_LATER_NAL, r, sizeof r);
    CHECK(salvage_slice_h264(r, n, &sps, &pps, &h), "later slice rejected");
    CHECK(h.first_mb != 0, "later slice claims first_mb_in_slice == 0");
    /* Both slices come from the capture but not necessarily the same picture,
     * so their frame_num may differ; what must hold is that the field was
     * found at a sane offset in both. */
    CHECK(h.frame_num_bits == sps.log2_max_frame_num,
          "frame_num width %u, want %u", h.frame_num_bits,
          sps.log2_max_frame_num);
    (void)fn;
    (void)poc;
}

/* --- the load-bearing test ---------------------------------------------- */

static void retarget_identity(const uint8_t *nal, size_t len, const char *what) {
    uint8_t r[512], out[512];
    const size_t n = rbsp_unescape(nal, len, r, sizeof r);
    SalvageSliceHeader h;
    if (!salvage_slice_h264(r, n, &sps, &pps, &h)) {
        CHECK(false, "%s: header not parsed", what);
        return;
    }
    bits_patch(r, h.frame_num_off, h.frame_num_bits, h.frame_num);
    bits_patch(r, h.poc_off, h.poc_bits, h.poc_lsb);
    const size_t m = rbsp_escape(r, n, out, sizeof out);
    CHECK(m == len && memcmp(out, nal, len) == 0,
          "%s: identity retarget changed the slice (%zu bytes out, want %zu)",
          what, m, len);
}

static void test_identity_retarget(void) {
    printf("  identity retarget: rewriting a slice to its own values is a "
           "no-op\n");
    retarget_identity(SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, "first slice");
    retarget_identity(SLICE_LATER_NAL, sizeof SLICE_LATER_NAL, "later slice");
}

static void test_retarget_changes_only_the_fields(void) {
    printf("  retarget: reads back the new values, everything else intact\n");
    uint8_t r[512], out[512], again[512];
    const size_t n = rbsp_unescape(
        SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, r, sizeof r);
    SalvageSliceHeader h;
    if (!salvage_slice_h264(r, n, &sps, &pps, &h)) {
        CHECK(false, "header not parsed");
        return;
    }
    const uint32_t new_fn = (h.frame_num + 3) & ((1u << h.frame_num_bits) - 1);
    const uint32_t new_poc = (h.poc_lsb + 6) & ((1u << h.poc_bits) - 1);
    bits_patch(r, h.frame_num_off, h.frame_num_bits, new_fn);
    bits_patch(r, h.poc_off, h.poc_bits, new_poc);
    const size_t m = rbsp_escape(r, n, out, sizeof out);

    const size_t k = rbsp_unescape(out, m, again, sizeof again);
    SalvageSliceHeader h2;
    CHECK(salvage_slice_h264(again, k, &sps, &pps, &h2),
          "retargeted slice no longer parses");
    CHECK(h2.frame_num == new_fn, "frame_num read back %u, want %u",
          h2.frame_num, new_fn);
    CHECK(h2.poc_lsb == new_poc, "POC read back %u, want %u", h2.poc_lsb,
          new_poc);
    CHECK(h2.first_mb == h.first_mb && h2.slice_type == h.slice_type,
          "fields before the patch moved");
    /* Fixed-width fields, so nothing downstream may shift. */
    CHECK(k == n, "RBSP length changed: %zu, want %zu", k, n);
}

static void test_refuses_unsupported(void) {
    printf("  unsupported stream shapes: refused, not guessed at\n");
    uint8_t r[512];
    const size_t n = rbsp_unescape(
        SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, r, sizeof r);
    SalvageSliceHeader h;

    SalvageSps bad = sps;
    bad.pic_order_cnt_type = 1; /* no POC lsb in the header at all */
    CHECK(!salvage_slice_h264(r, n, &bad, &pps, &h),
          "accepted pic_order_cnt_type=1");

    bad = sps;
    bad.frame_mbs_only = false; /* field_pic_flag appears before the POC */
    CHECK(!salvage_slice_h264(r, n, &bad, &pps, &h), "accepted interlaced");

    bad = sps;
    bad.separate_colour_plane = true; /* colour_plane_id shifts everything */
    CHECK(!salvage_slice_h264(r, n, &bad, &pps, &h),
          "accepted separate_colour_plane_flag=1");

    const SalvagePps nopps = {0};
    CHECK(!salvage_slice_h264(r, n, &sps, &nopps, &h),
          "parsed a slice without a PPS");

    /* A slice cut to nothing must be refused rather than read past the end. */
    CHECK(!salvage_slice_h264(r, 1, &sps, &pps, &h), "accepted a 1-byte slice");
}

/* --- the stage ----------------------------------------------------------- */

typedef struct {
    uint8_t data[65536];
    size_t len;
    bool repaired;
    unsigned count;
} Caught;

static void on_pic(const uint8_t *p, size_t len, bool repaired, void *ctx) {
    Caught *c = ctx;
    if (len <= sizeof c->data) {
        memcpy(c->data, p, len);
        c->len = len;
    }
    c->repaired = repaired;
    c->count++;
}

/* Build an access unit out of whole NALs, as the depacketiser would. */
typedef struct {
    uint8_t data[65536];
    size_t len;
    SalvageNal nals[16];
    size_t n;
} AuBuild;

static void au_add(
    AuBuild *a, const uint8_t *nal, size_t len, bool complete) {
    SalvageNal *s = &a->nals[a->n++];
    memset(s, 0, sizeof *s);
    s->offset = a->len;
    a->data[a->len++] = 0;
    a->data[a->len++] = 0;
    a->data[a->len++] = 0;
    a->data[a->len++] = 1;
    memcpy(a->data + a->len, nal, len);
    a->len += len;
    s->len = 4 + len;
    s->type = (uint8_t)(nal[0] & 0x1fu);
    s->is_slice = s->type == 1 || s->type == 5;
    s->first_slice = s->is_slice && (nal[1] & 0x80u) != 0;
    s->complete = complete;
}

static SalvageAu au_of(const AuBuild *a, bool complete) {
    SalvageAu au = {
        .data = a->data,
        .len = a->len,
        .ts = 9000,
        .nals = a->nals,
        .nal_count = a->n,
        .complete = complete,
    };
    for (size_t i = 0; i < a->n; i++) {
        if (a->nals[i].is_slice && a->nals[i].first_slice) {
            au.has_first_slice = true;
        }
    }
    return au;
}

static void test_stage_transparent(void) {
    printf("  intact picture: passed through byte for byte\n");
    Caught c = {0};
    const SalvagePolicy pol = {SALVAGE_TRUNCATED_FORWARD, true};
    SalvageStage *st = salvage_new(SALVAGE_H264, pol, 0, on_pic, &c);

    AuBuild a = {0};
    au_add(&a, SPS, sizeof SPS, true);
    au_add(&a, PPS, sizeof PPS, true);
    au_add(&a, SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, true);
    au_add(&a, SLICE_LATER_NAL, sizeof SLICE_LATER_NAL, true);
    const SalvageAu au = au_of(&a, true);
    salvage_input(st, &au);

    CHECK(c.count == 1, "%u pictures out, want 1", c.count);
    CHECK(!c.repaired, "intact picture reported as repaired");
    CHECK(c.len == a.len && memcmp(c.data, a.data, a.len) == 0,
          "picture altered: %zu bytes out, want %zu", c.len, a.len);
    salvage_free(st);
}

static void test_stage_synthesises(void) {
    printf("  lost first slice: stand-in built and retargeted to this "
           "picture\n");
    Caught c = {0};
    const SalvagePolicy pol = {SALVAGE_TRUNCATED_FORWARD, true};
    SalvageStage *st = salvage_new(SALVAGE_H264, pol, 0, on_pic, &c);

    /* Picture 1 arrives whole, so its first slice becomes the stand-in. */
    AuBuild a = {0};
    au_add(&a, SPS, sizeof SPS, true);
    au_add(&a, PPS, sizeof PPS, true);
    au_add(&a, SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, true);
    SalvageAu au = au_of(&a, true);
    salvage_input(st, &au);

    /* Picture 2 loses its first slice; only a later slice survives. */
    AuBuild b = {0};
    au_add(&b, SLICE_LATER_NAL, sizeof SLICE_LATER_NAL, true);
    au = au_of(&b, false);
    salvage_input(st, &au);

    CHECK(c.count == 2, "%u pictures out, want 2", c.count);
    CHECK(c.repaired, "picture missing its first slice not repaired");
    CHECK(c.len > b.len, "nothing was added to the picture");

    SalvageStats s;
    salvage_stats(st, &s);
    CHECK(s.first_slice_synthesised == 1, "%llu synthesised, want 1",
          (unsigned long long)s.first_slice_synthesised);

    /* The stand-in must lead, and must claim to belong to this picture — that
     * is the entire point of retargeting it. */
    CHECK(c.data[4] == SLICE_FIRST_NAL[0],
          "synthesised slice is not first in the picture");
    uint8_t r[512];
    SalvageSliceHeader want, got;
    size_t n = rbsp_unescape(
        SLICE_LATER_NAL, sizeof SLICE_LATER_NAL, r, sizeof r);
    salvage_slice_h264(r, n, &sps, &pps, &want);

    size_t end = 4;
    while (end + 3 < c.len &&
           !(c.data[end] == 0 && c.data[end + 1] == 0 && c.data[end + 2] == 0 &&
             c.data[end + 3] == 1)) {
        end++;
    }
    n = rbsp_unescape(c.data + 4, end - 4, r, sizeof r);
    if (salvage_slice_h264(r, n, &sps, &pps, &got)) {
        CHECK(got.first_mb == 0, "stand-in is not a first slice");
        CHECK(got.frame_num == want.frame_num,
              "stand-in frame_num %u, want %u (the surviving slice's)",
              got.frame_num, want.frame_num);
        CHECK(got.poc_lsb == want.poc_lsb, "stand-in POC %u, want %u",
              got.poc_lsb, want.poc_lsb);
    } else {
        CHECK(false, "synthesised slice does not parse");
    }
    salvage_free(st);
}

static void test_stage_no_template(void) {
    printf("  lost first slice with nothing to copy: nothing invented\n");
    Caught c = {0};
    const SalvagePolicy pol = {SALVAGE_TRUNCATED_FORWARD, true};
    SalvageStage *st = salvage_new(SALVAGE_H264, pol, 0, on_pic, &c);

    AuBuild a = {0};
    au_add(&a, SPS, sizeof SPS, true);
    au_add(&a, PPS, sizeof PPS, true);
    au_add(&a, SLICE_LATER_NAL, sizeof SLICE_LATER_NAL, true);
    const SalvageAu au = au_of(&a, false);
    salvage_input(st, &au);

    CHECK(c.count == 1 && !c.repaired, "repaired with no stand-in available");
    CHECK(c.len == a.len && memcmp(c.data, a.data, a.len) == 0,
          "picture altered when no repair was possible");
    SalvageStats s;
    salvage_stats(st, &s);
    CHECK(s.first_slice_unrepaired == 1, "%llu unrepaired, want 1",
          (unsigned long long)s.first_slice_unrepaired);
    salvage_free(st);
}

static void test_truncated_policy(void) {
    printf("  truncated slice: forwarded or dropped as the policy says\n");
    for (int drop = 0; drop < 2; drop++) {
        Caught c = {0};
        const SalvagePolicy pol = {
            drop ? SALVAGE_TRUNCATED_DROP : SALVAGE_TRUNCATED_FORWARD, false};
        SalvageStage *st = salvage_new(SALVAGE_H264, pol, 0, on_pic, &c);

        AuBuild a = {0};
        au_add(&a, SLICE_FIRST_NAL, sizeof SLICE_FIRST_NAL, true);
        au_add(&a, SLICE_LATER_NAL, sizeof SLICE_LATER_NAL, false);
        const SalvageAu au = au_of(&a, false);
        salvage_input(st, &au);

        SalvageStats s;
        salvage_stats(st, &s);
        if (drop) {
            CHECK(s.truncated_dropped == 1 && s.truncated_forwarded == 0,
                  "drop policy forwarded a truncated slice");
            CHECK(c.len == 4 + sizeof SLICE_FIRST_NAL,
                  "dropped policy emitted %zu bytes, want %zu", c.len,
                  4 + sizeof SLICE_FIRST_NAL);
        } else {
            CHECK(s.truncated_forwarded == 1 && s.truncated_dropped == 0,
                  "forward policy dropped a truncated slice");
            CHECK(c.len == a.len, "forward policy emitted %zu bytes, want %zu",
                  c.len, a.len);
        }
        salvage_free(st);
    }
}

static void test_stage_garbage(void) {
    printf("  nonsense access units: no crash, nothing invented\n");
    Caught c = {0};
    const SalvagePolicy pol = {SALVAGE_TRUNCATED_FORWARD, true};
    SalvageStage *st = salvage_new(SALVAGE_H264, pol, 0, on_pic, &c);

    uint8_t junk[64];
    memset(junk, 0xa5, sizeof junk);
    junk[0] = 0x41; /* looks like a slice, contains noise */
    AuBuild a = {0};
    au_add(&a, junk, sizeof junk, true);
    const SalvageAu au = au_of(&a, false);
    salvage_input(st, &au);

    SalvageStats s;
    salvage_stats(st, &s);
    CHECK(s.first_slice_synthesised == 0, "invented a slice from noise");
    salvage_free(st);
}

int main(void) {
    printf("slice salvage\n");
    test_rbsp();
    test_bit_patch();
    test_parameter_sets();
    test_slice_headers();
    test_identity_retarget();
    test_retarget_changes_only_the_fields();
    test_refuses_unsupported();
    test_stage_transparent();
    test_stage_synthesises();
    test_stage_no_template();
    test_truncated_policy();
    test_stage_garbage();

    if (failures > 0) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
