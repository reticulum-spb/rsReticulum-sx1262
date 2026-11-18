#define _GNU_SOURCE
#include "sx126x.h"
#include "sx1262_uapi.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A record-oriented fake device exercises the real worker, locks and waits. */
static int sockets[2];
static atomic_int online = 1, reported = 1, received, late, submitted;
static int completion_status, completion_frames = -1, submit_error;
static bool automatic = true;
static struct sx1262_tx_batch last_batch;
static struct sx1262_config applied;

int __wrap_open(const char *path, int flags, ...);
int __wrap_ioctl(int fd, unsigned long request, ...);

int __wrap_open(const char *path, int flags, ...) {
    assert(strcmp(path, "/dev/mock-lora") == 0);
    assert((flags & (O_NONBLOCK | O_CLOEXEC)) == (O_NONBLOCK | O_CLOEXEC));
    return dup(sockets[0]);
}

static void emit(struct sx1262_event event) {
    ssize_t result = send(sockets[1], &event, sizeof(event), 0);
    if (result < 0) perror("fake device send");
    assert(result == sizeof(event));
}

int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    void *data;
    (void)fd;
    va_start(args, request);
    data = va_arg(args, void *);
    va_end(args);
    if (request == SX1262_IOC_GET_CONFIG) {
        struct sx1262_config *c = data;
        memset(c, 0, sizeof(*c));
        c->version = SX1262_ABI_VERSION;
        c->implicit_header = c->invert_iq = 1; /* Must not leak into new config. */
    } else if (request == SX1262_IOC_SET_CONFIG) {
        applied = *(struct sx1262_config *)data;
    } else if (request == SX1262_IOC_GET_STATS) {
        struct sx1262_stats *s = data;
        memset(s, 0, sizeof(*s));
        s->version = SX1262_ABI_VERSION;
        s->online = atomic_load(&online);
    } else if (request == SX1262_IOC_TX_BATCH) {
        if (submit_error) { errno = submit_error; return -1; }
        last_batch = *(struct sx1262_tx_batch *)data;
        atomic_fetch_add(&submitted, 1);
        if (automatic) emit((struct sx1262_event){
            .version = SX1262_ABI_VERSION, .type = SX1262_EVENT_TX,
            .cookie = last_batch.cookie, .status = completion_status,
            .frames_sent = completion_frames < 0 ? last_batch.count : (unsigned int)completion_frames
        });
    } else {
        assert(0);
    }
    return 0;
}

static void rx(void *context, const uint8_t *data, size_t len, int16_t rssi, int16_t snr) {
    (void)context;
    assert(len == 3 && data[0] == 0xa0 && data[1] == 42);
    assert(rssi == -101 && snr == -3);
    atomic_fetch_add(&received, 1);
}
static void log_message(void *context, int level, const char *message) {
    (void)context; (void)level;
    if (strstr(message, "late TX result")) atomic_fetch_add(&late, 1);
}
static void set_online(void *context, bool value) {
    (void)context;
    atomic_store(&reported, value);
}
static void wait_value(atomic_int *value, int expected) {
    for (int i = 0; i < 200 && atomic_load(value) != expected; ++i) usleep(10000);
    assert(atomic_load(value) == expected);
}

int main(void) {
    plugin_config_t c = {
        .device = "/dev/mock-lora", .frequency = 868000000, .bandwidth = 125000,
        .spreading_factor = 7, .coding_rate = 5, .tx_power = 22,
        .tx_preamble_symbols = 25, .rx_preamble_symbols = 65, .sync_word = 0x1424,
        .irq_watchdog_seconds = 60, .hard_reset_after = 2, .tx_timeout_seconds = 1
    };
    uint8_t packet[500];
    memset(packet, 42, sizeof(packet));
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets) == 0);
    sx126x_t *r = sx126x_open(&c, rx, log_message, set_online, NULL);
    assert(applied.rx_preamble_symbols == 65 && applied.tx_preamble_symbols == 25);
    assert(r && applied.ldro == 1 && applied.crc == 1);
    assert(!applied.implicit_header && !applied.invert_iq && !applied.implicit_length);
    assert(applied.silence_timeout_ms == 60000 && applied.sync_word == 0x1424);
    assert(sx126x_bitrate(r) == 5468 && sx126x_start(r));
    emit((struct sx1262_event){ .version = SX1262_ABI_VERSION, .type = SX1262_EVENT_RX,
        .length = 3, .data = {0xa0, 42, 42}, .rssi_half_dbm = -203, .snr_quarter_db = -10 });
    wait_value(&received, 1);
    assert(sx126x_send(r, packet, sizeof(packet), 0xa0));
    assert(last_batch.count == 2 && last_batch.frames[0].length == 255);
    assert(last_batch.frames[1].length == 247 && last_batch.frames[0].data[0] == 0xa1);
    assert(last_batch.frames[1].data[0] == 0xa1);
    assert(!memcmp(last_batch.frames[0].data + 1, packet, 254));
    assert(!memcmp(last_batch.frames[1].data + 1, packet + 254, 246));
    completion_status = -ETIMEDOUT;
    completion_frames = 1;
    assert(!sx126x_send(r, packet, sizeof(packet), 0xb0));
    completion_status = 0; /* Even status=0 cannot acknowledge a partial batch. */
    assert(!sx126x_send(r, packet, sizeof(packet), 0xc0));
    completion_frames = -1;
    submit_error = EAGAIN;
    int before = atomic_load(&submitted);
    assert(!sx126x_send(r, packet, 1, 0xd0));
    assert(atomic_load(&submitted) == before);
    submit_error = 0;
    automatic = false;
    assert(!sx126x_send(r, packet, 1, 0xe0)); /* Real timed wait expires. */
    wait_value(&reported, 0);
    before = atomic_load(&submitted);
    assert(!sx126x_send(r, packet, 1, 0xf0));
    assert(atomic_load(&submitted) == before);
    emit((struct sx1262_event){ .version = SX1262_ABI_VERSION, .type = SX1262_EVENT_TX,
        .cookie = last_batch.cookie + 1, .frames_sent = 1 });
    usleep(50000);
    assert(!sx126x_send(r, packet, 1, 0xf0)); /* Wrong cookie must not unlock TX. */
    emit((struct sx1262_event){ .version = SX1262_ABI_VERSION, .type = SX1262_EVENT_TX,
        .cookie = last_batch.cookie, .frames_sent = 1 });
    wait_value(&late, 1);
    wait_value(&reported, 1);
    automatic = true;
    assert(sx126x_send(r, packet, 1, 0xf0));
    atomic_store(&online, 0);
    wait_value(&reported, 0);
    atomic_store(&online, 1);
    wait_value(&reported, 1);
    close(sockets[1]); /* Removal wakes the event worker and marks it failed. */
    wait_value(&reported, 0);
    assert(!sx126x_send(r, packet, 1, 0));
    sx126x_close(r);
    close(sockets[0]);
    puts("Linux backend batching, metadata, errors, late TX and recovery tests passed");
    return 0;
}
