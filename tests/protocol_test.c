#include "protocol.h"
#include <assert.h>
#include <string.h>
typedef struct { uint8_t frames[2][255]; size_t lengths[2]; size_t count; } capture_t;
static void capture(void *opaque, const uint8_t *data, size_t len) {
    capture_t *c = opaque; memcpy(c->frames[c->count], data, len); c->lengths[c->count++] = len;
}
int main(void) {
    uint8_t input[500]; const uint8_t *output; size_t output_len; capture_t c = {0};
    rns_reassembly_t rx = {.sequence = 0xff};
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)i;
    assert(rns_fragment(input, sizeof(input), 0xa0, capture, &c) == 2);
    assert(c.lengths[0] == 255 && c.lengths[1] == 247);
    assert(!rns_reassemble(&rx, c.frames[0], c.lengths[0], &output, &output_len));
    assert(rns_reassemble(&rx, c.frames[1], c.lengths[1], &output, &output_len));
    assert(output_len == sizeof(input) && memcmp(input, output, sizeof(input)) == 0);
    return 0;
}
