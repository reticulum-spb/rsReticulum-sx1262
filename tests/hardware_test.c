/* Manual test only: TX modes emit real RF. Never registered with CTest. */
#define _POSIX_C_SOURCE 200809L
#include "rns_plugin.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_int online, went_offline, received;
static void log_message(void *ctx, rns_log_level_t level, const uint8_t *data, size_t len) {
    (void)ctx;
    printf("LOG %d %.*s\n", level, (int)len, (const char *)data);
}
static void bitrate(void *ctx, uint64_t value) {
    (void)ctx;
    printf("BITRATE %llu\n", (unsigned long long)value);
}
static void state(void *ctx, uint8_t value) {
    (void)ctx;
    if (!value) atomic_store(&went_offline, 1);
    atomic_store(&online, value);
    printf("ONLINE %u\n", value);
}
static void rx(void *ctx, const uint8_t *data, size_t len,
               const rns_rx_metadata_t *metadata, size_t metadata_size) {
    (void)ctx; (void)data;
    printf("RX bytes=%zu", len);
    if (metadata && metadata_size >= sizeof(*metadata))
        printf(" rssi=%d snr=%d", metadata->rssi_dbm, metadata->snr_db);
    puts("");
    atomic_fetch_add(&received, 1);
}
static void pause_ms(long ms) {
    struct timespec delay = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000 };
    nanosleep(&delay, NULL);
}
int main(int argc, char **argv) {
    uint8_t yaml[16384], packet[500];
    const rns_plugin_api_t *(*get_api)(void);
    rns_plugin_t *plugin = NULL;
    rns_host_api_t host = {
        .abi_major = RNS_PLUGIN_ABI_MAJOR, .abi_minor = RNS_PLUGIN_ABI_MINOR,
        .struct_size = sizeof(host), .log = log_message, .set_bitrate = bitrate,
        .set_online = state, .rx_packet = rx
    };
    int result = EXIT_SUCCESS;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 4 || (strcmp(argv[3], "rx") && strcmp(argv[3], "tx") && strcmp(argv[3], "late"))) {
        fprintf(stderr, "Usage: %s LIBRARY CONFIG_YAML rx|tx|late\n"
                "tx/late emit RF; late needs tx_timeout_seconds: 1 and a PHY\n"
                "where a 500-byte packet takes more than one second.\n", argv[0]);
        return EXIT_FAILURE;
    }
    FILE *file = fopen(argv[2], "rb");
    if (!file) { perror("config"); return EXIT_FAILURE; }
    size_t length = fread(yaml, 1, sizeof(yaml), file);
    bool invalid = ferror(file) || length == sizeof(yaml);
    fclose(file);
    if (invalid) return EXIT_FAILURE;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return EXIT_FAILURE; }
    *(void **)(&get_api) = dlsym(library, "rns_plugin_get_api");
    if (!get_api) return EXIT_FAILURE;
    const rns_plugin_api_t *api = get_api();
    if (!api || api->abi_major != RNS_PLUGIN_ABI_MAJOR ||
        api->create(&host, yaml, length, &plugin) != RNS_PLUGIN_OK || !plugin)
        return EXIT_FAILURE;
    if (!atomic_load(&online)) result = EXIT_FAILURE;
    for (size_t i = 0; i < sizeof(packet); ++i) packet[i] = (uint8_t)i;
    if (!strcmp(argv[3], "tx")) {
        const size_t sizes[] = {1, 254, 255, 500};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            int status = api->send(plugin, packet, sizes[i]);
            printf("TX bytes=%zu status=%d\n", sizes[i], status);
            if (status != RNS_PLUGIN_OK) { result = EXIT_FAILURE; break; }
        }
    } else if (!strcmp(argv[3], "late")) {
        int first = api->send(plugin, packet, sizeof(packet));
        int second = api->send(plugin, packet, 1);
        printf("TIMEOUT status=%d SECOND_SEND status=%d\n", first, second);
        if (first == RNS_PLUGIN_OK || second == RNS_PLUGIN_OK) result = EXIT_FAILURE;
        for (int i = 0; i < 300; ++i) {
            if (atomic_load(&went_offline) && atomic_load(&online)) break;
            pause_ms(100);
        }
        if (!atomic_load(&went_offline) || !atomic_load(&online)) result = EXIT_FAILURE;
        int last = api->send(plugin, packet, 1);
        printf("AFTER_LATE status=%d\n", last);
        if (last != RNS_PLUGIN_OK) result = EXIT_FAILURE;
    } else {
        pause_ms(5000);
        if (!atomic_load(&online)) result = EXIT_FAILURE;
    }
    api->destroy(plugin);
    printf("RESULT %s rx_packets=%d\n", result ? "FAIL" : "PASS", atomic_load(&received));
    /* Keep the library resident, as required by the normal host lifecycle. */
    return result;
}
