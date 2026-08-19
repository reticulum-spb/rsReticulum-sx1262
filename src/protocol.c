#include "protocol.h"
#include <string.h>
#define FLAG_SPLIT UINT8_C(0x01)
#define SEQ_UNSET  UINT8_C(0xff)

size_t rns_fragment(const uint8_t *data, size_t len, uint8_t sequence, rns_frame_fn emit, void *context) {
    uint8_t frame[RNS_AIR_PAYLOAD_MAX];
    size_t  first = len > RNS_AIR_DATA_MAX ? RNS_AIR_DATA_MAX : len;
    if (!data || !emit || len == 0 || len > RNS_PROTOCOL_MTU)
        return 0;
    frame[0] = (uint8_t) (sequence & UINT8_C(0xf0));
    if (len > first)
        frame[0] |= FLAG_SPLIT;
    memcpy(frame + 1, data, first);
    emit(context, frame, first + 1);
    if (len > first) {
        frame[0] = (uint8_t) ((sequence & UINT8_C(0xf0)) | FLAG_SPLIT);
        memcpy(frame + 1, data + first, len - first);
        emit(context, frame, len - first + 1);
        return 2;
    }
    return 1;
}

bool rns_reassemble(rns_reassembly_t *state, const uint8_t *frame, size_t len, const uint8_t **packet, size_t *packet_len) {
    bool    split;
    uint8_t sequence;
    size_t  payload;
    if (!state || !frame || len < 2 || len > RNS_AIR_PAYLOAD_MAX || !packet || !packet_len)
        return false;
    *packet = NULL;
    *packet_len = 0;
    split = (frame[0] & FLAG_SPLIT) != 0;
    sequence = frame[0] >> 4;
    payload = len - 1;
    if (!split) {
        memcpy(state->data, frame + 1, payload);
        state->length = payload;
        state->sequence = SEQ_UNSET;
    } else if (state->sequence == SEQ_UNSET || state->sequence != sequence) {
        memcpy(state->data, frame + 1, payload);
        state->length = payload;
        state->sequence = sequence;
        return false;
    } else {
        if (state->length + payload > sizeof(state->data)) {
            state->sequence = SEQ_UNSET;
            state->length = 0;
            return false;
        }
        memcpy(state->data + state->length, frame + 1, payload);
        state->length += payload;
        state->sequence = SEQ_UNSET;
    }
    *packet = state->data;
    *packet_len = state->length;
    return true;
}
