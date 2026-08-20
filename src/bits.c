#include "bits.h"

void br_init(BitReader *r, const uint8_t *data, size_t len) {
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->overrun = false;
}

uint32_t br_u(BitReader *r, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (r->pos >> 3 >= r->len) {
            r->overrun = true;
            return v << (n - i); /* pad with zeros, keep the field width */
        }
        v = (v << 1) | ((r->data[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}

uint32_t br_ue(BitReader *r) {
    unsigned lead = 0;
    while (br_u(r, 1) == 0) {
        if (++lead > 32 || r->overrun) {
            r->overrun = true;
            return 0;
        }
    }
    return lead ? (uint32_t)((1u << lead) - 1u + br_u(r, lead)) : 0;
}

int32_t br_se(BitReader *r) {
    const uint32_t k = br_ue(r);
    return (k & 1u) ? (int32_t)((k + 1u) / 2u) : -(int32_t)(k / 2u);
}

size_t rbsp_unescape(const uint8_t *nal, size_t len, uint8_t *out, size_t cap) {
    size_t n = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = nal[i];
        if (zeros == 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        if (n >= cap) {
            return 0;
        }
        out[n++] = b;
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return n;
}

size_t rbsp_escape(const uint8_t *rbsp, size_t len, uint8_t *out, size_t cap) {
    size_t n = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = rbsp[i];
        if (zeros == 2 && b <= 0x03) {
            if (n >= cap) {
                return 0;
            }
            out[n++] = 0x03;
            zeros = 0;
        }
        if (n >= cap) {
            return 0;
        }
        out[n++] = b;
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return n;
}

void bits_patch(uint8_t *buf, size_t off, unsigned n, uint32_t value) {
    for (unsigned i = 0; i < n; i++) {
        const size_t at = off + i;
        const uint8_t mask = (uint8_t)(1u << (7 - (at & 7)));
        if ((value >> (n - 1 - i)) & 1u) {
            buf[at >> 3] |= mask;
        } else {
            buf[at >> 3] &= (uint8_t)~mask;
        }
    }
}
