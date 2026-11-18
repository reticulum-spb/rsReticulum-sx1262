/* Linux sx1262 ABI client. Hardware recovery belongs to the kernel driver. */
#define _GNU_SOURCE
#include "sx126x.h"
#include "sx1262_uapi.h"
#include "protocol.h"
#include "rns_plugin.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct sx126x {
    int fd, wake[2];
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool started, stopping, failed, pending, expired, success;
    uint64_t cookie, bitrate;
    uint32_t expected, timeout_seconds;
    sx126x_rx_fn receive;
    sx126x_log_fn log;
    sx126x_online_fn online;
    void *context;
};

static void log_error(sx126x_t *r, const char *operation) {
    char message[256];
    snprintf(message, sizeof(message), "sx1262: %s: %s", operation, strerror(errno));
    if (r->log) r->log(r->context, RNS_LOG_ERROR, message);
}

static void handle_event(sx126x_t *r, const struct sx1262_event *e) {
    if (e->type == SX1262_EVENT_RX) {
        if (e->length >= 2 && e->length <= SX1262_MAX_PAYLOAD && r->receive)
            r->receive(r->context, e->data, e->length,
                       (int16_t)(e->rssi_half_dbm / 2),
                       (int16_t)lround(e->snr_quarter_db / 4.0));
    } else if (e->type == SX1262_EVENT_TX) {
        char message[192];
        bool late = false, matched;
        pthread_mutex_lock(&r->mutex);
        matched = r->pending && e->cookie == r->cookie;
        if (matched) {
            late = r->expired;
            r->success = e->status == 0 && e->frames_sent == r->expected;
            r->pending = false;
            pthread_cond_broadcast(&r->condition);
        }
        pthread_mutex_unlock(&r->mutex);
        if (r->log && (late || !matched || e->status)) {
            snprintf(message, sizeof(message),
                     "sx1262: %sTX result cookie=%" PRIu64 " status=%d frames_sent=%u",
                     !matched ? "unmatched " : late ? "late " : "",
                     (uint64_t)e->cookie, e->status, e->frames_sent);
            r->log(r->context, RNS_LOG_WARN, message);
        }
    }
}

static void *event_worker(void *opaque) {
    sx126x_t *r = opaque;
    bool reported = true, healthy = true;
    for (;;) {
        /* Suppress the level-triggered POLLERR while offline. The wake pipe
         * still stops us promptly; GET_STATS probes recovery every 100 ms. */
        struct pollfd fds[2] = {
            { .fd = healthy ? r->fd : -1, .events = POLLIN },
            { .fd = r->wake[0], .events = POLLIN }
        };
        struct sx1262_stats stats;
        bool stopping, failed, online;
        int result = poll(fds, 2, 100);
        pthread_mutex_lock(&r->mutex);
        stopping = r->stopping;
        pthread_mutex_unlock(&r->mutex);
        if (stopping) break;
        if (result < 0 && errno == EINTR) continue;
        failed = result < 0 || (fds[0].revents & POLLNVAL);
        /* Drain even offline, including completions already queued on remove.
         * Bound each pass so RX traffic cannot starve shutdown/status checks. */
        for (unsigned int i = 0; !failed && i < 160; ++i) {
            struct sx1262_event event;
            ssize_t size = read(r->fd, &event, sizeof(event));
            if (size < 0 && errno == EINTR) continue;
            if (size < 0 && errno == EAGAIN) break;
            if (size != sizeof(event) || event.version != SX1262_ABI_VERSION) {
                if (size >= 0) errno = EPROTO;
                log_error(r, "read event");
                failed = true;
                break;
            }
            handle_event(r, &event);
        }
        if (!failed && ioctl(r->fd, SX1262_IOC_GET_STATS, &stats) < 0) {
            if (errno == EINTR) continue;
            log_error(r, "GET_STATS");
            failed = true;
        }
        if (!failed && stats.version != SX1262_ABI_VERSION) failed = true;
        healthy = !failed && stats.online;
        pthread_mutex_lock(&r->mutex);
        r->failed = failed;
        online = healthy && !(r->pending && r->expired);
        if (failed) pthread_cond_broadcast(&r->condition);
        pthread_mutex_unlock(&r->mutex);
        if (online != reported) {
            if (r->online) r->online(r->context, online);
            reported = online;
        }
        if (failed) break;
    }
    return NULL;
}

sx126x_t *sx126x_open(const plugin_config_t *c, sx126x_rx_fn receive,
                     sx126x_log_fn log, sx126x_online_fn online, void *context) {
    sx126x_t *r = calloc(1, sizeof(*r));
    struct sx1262_config config;
    pthread_condattr_t attr;
    if (!r) return NULL;
    r->fd = r->wake[0] = r->wake[1] = -1;
    r->receive = receive; r->log = log; r->online = online; r->context = context;
    r->timeout_seconds = c->tx_timeout_seconds;
    r->bitrate = (uint64_t)c->spreading_factor * 4 * c->bandwidth /
                 ((UINT64_C(1) << c->spreading_factor) * c->coding_rate);
    if (pthread_mutex_init(&r->mutex, NULL)) { free(r); return NULL; }
    if (pthread_condattr_init(&attr)) goto mutex_fail;
    int result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!result) result = pthread_cond_init(&r->condition, &attr);
    pthread_condattr_destroy(&attr);
    if (result) goto mutex_fail;
    r->fd = open(c->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (r->fd < 0) { log_error(r, "open device"); goto fail; }
    if (ioctl(r->fd, SX1262_IOC_GET_CONFIG, &config) < 0) {
        log_error(r, "GET_CONFIG"); goto fail;
    }
    if (config.version != SX1262_ABI_VERSION) {
        errno = EPROTO; log_error(r, "driver ABI"); goto fail;
    }
    /* Do not inherit PHY settings from a previous opener. */
    config = (struct sx1262_config) {
        .version = SX1262_ABI_VERSION,
        .frequency_hz = c->frequency, .bandwidth_hz = c->bandwidth,
        .tx_power_dbm = c->tx_power, .tx_preamble_symbols = c->tx_preamble_symbols,
        .rx_preamble_symbols = c->rx_preamble_symbols,
        .silence_timeout_ms = c->irq_watchdog_seconds * 1000,
        .sync_word = c->sync_word, .spreading_factor = c->spreading_factor,
        .coding_rate = c->coding_rate, .crc = 1, .ldro = 1,
        .hard_reset_after = c->hard_reset_after
    };
    if (ioctl(r->fd, SX1262_IOC_SET_CONFIG, &config) < 0) {
        log_error(r, "SET_CONFIG"); goto fail;
    }
    if (pipe2(r->wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        log_error(r, "wake pipe"); goto fail;
    }
    return r;
fail:
    sx126x_close(r);
    return NULL;
mutex_fail:
    pthread_mutex_destroy(&r->mutex);
    free(r);
    return NULL;
}

bool sx126x_start(sx126x_t *r) {
    if (pthread_create(&r->thread, NULL, event_worker, r)) return false;
    r->started = true;
    return true;
}

static void append_frame(void *context, const uint8_t *data, size_t len) {
    struct sx1262_tx_batch *batch = context;
    struct sx1262_frame *frame = &batch->frames[batch->count++];
    frame->length = (uint16_t)len;
    memcpy(frame->data, data, len);
}

bool sx126x_send(sx126x_t *r, const uint8_t *data, size_t len, uint8_t sequence) {
    struct sx1262_tx_batch batch = { .version = SX1262_ABI_VERSION };
    struct timespec deadline;
    bool success = false;
    if (!r || !rns_fragment(data, len, sequence, append_frame, &batch)) return false;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += r->timeout_seconds;
    pthread_mutex_lock(&r->mutex);
    if (r->pending || r->failed || r->stopping) goto out;
    batch.cookie = ++r->cookie;
    r->expected = batch.count;
    r->success = r->expired = false;
    /* Nonblocking submission: EAGAIN is a definite rejection, never retry an
     * accepted batch. The lock prevents completion racing the pending flag. */
    if (ioctl(r->fd, SX1262_IOC_TX_BATCH, &batch) < 0) {
        log_error(r, "TX_BATCH"); goto out;
    }
    r->pending = true;
    while (r->pending && !r->failed && !r->stopping) {
        int result = pthread_cond_timedwait(&r->condition, &r->mutex, &deadline);
        if (result && r->pending) {
            r->expired = true;
            if (r->log) r->log(r->context, RNS_LOG_WARN,
                "sx1262: TX wait expired; batch remains pending, new TX blocked until completion");
            break;
        }
    }
    success = !r->pending && r->success && !r->failed && !r->stopping;
out:
    pthread_mutex_unlock(&r->mutex);
    return success;
}

uint64_t sx126x_bitrate(const sx126x_t *r) { return r ? r->bitrate : 0; }

void sx126x_close(sx126x_t *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mutex);
    r->stopping = true;
    pthread_cond_broadcast(&r->condition);
    pthread_mutex_unlock(&r->mutex);
    if (r->started) {
        char byte = 0;
        (void)write(r->wake[1], &byte, 1);
        pthread_join(r->thread, NULL);
    }
    if (r->fd >= 0) close(r->fd);
    if (r->wake[0] >= 0) close(r->wake[0]);
    if (r->wake[1] >= 0) close(r->wake[1]);
    pthread_cond_destroy(&r->condition);
    pthread_mutex_destroy(&r->mutex);
    free(r);
}
