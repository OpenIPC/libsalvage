#ifndef SALVAGE_BITS_H
#define SALVAGE_BITS_H

/* Just enough bitstream handling to find and patch a slice header field.
 *
 * The reader saturates rather than reading past the end: a truncated slice is
 * the normal case here, not an error, and a parser that walks off the buffer
 * turns a damaged picture into a crash.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t len;      /* bytes */
    size_t pos;      /* bit offset */
    bool overrun;    /* set once a read went past the end */
} BitReader;

void br_init(BitReader *r, const uint8_t *data, size_t len);
uint32_t br_u(BitReader *r, unsigned n);
uint32_t br_ue(BitReader *r);
int32_t br_se(BitReader *r);

/* Emulation prevention. Both return the number of bytes written, or 0 if the
 * output would not fit. */
size_t rbsp_unescape(const uint8_t *nal, size_t len, uint8_t *out, size_t cap);
size_t rbsp_escape(const uint8_t *rbsp, size_t len, uint8_t *out, size_t cap);

/* Overwrite `n` bits at bit offset `off`, most significant bit first. */
void bits_patch(uint8_t *buf, size_t off, unsigned n, uint32_t value);

#endif /* SALVAGE_BITS_H */
