#ifndef RNS_SX1262_PROTOCOL_H
#define RNS_SX1262_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RNS_AIR_PAYLOAD_MAX 255u
#define RNS_AIR_DATA_MAX    254u
#define RNS_PROTOCOL_MTU    500u
typedef struct {
    uint8_t sequence;
    uint8_t data[RNS_PROTOCOL_MTU];
    size_t  length;
} rns_reassembly_t;
typedef void (*rns_frame_fn)(void *, const uint8_t *, size_t);
size_t rns_fragment(const uint8_t *, size_t, uint8_t, rns_frame_fn, void *);
bool   rns_reassemble(rns_reassembly_t *, const uint8_t *, size_t, const uint8_t **, size_t *);
#endif
