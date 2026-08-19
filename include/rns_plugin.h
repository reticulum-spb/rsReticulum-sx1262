#ifndef RNS_PLUGIN_H
#define RNS_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#define RNS_PLUGIN_ABI_MAJOR UINT32_C(1)
#define RNS_PLUGIN_ABI_MINOR UINT32_C(0)
#define RNS_PLUGIN_OK ((int32_t)0)
#define RNS_PLUGIN_ERROR ((int32_t)-1)
#define RNS_LOG_ERROR ((int32_t)1)
#define RNS_LOG_WARN ((int32_t)2)
#define RNS_LOG_INFO ((int32_t)3)
#define RNS_LOG_DEBUG ((int32_t)4)
#define RNS_LOG_TRACE ((int32_t)5)
#define RNS_RX_METADATA_RSSI (UINT32_C(1) << 0)
#define RNS_RX_METADATA_SNR (UINT32_C(1) << 1)

typedef int32_t rns_plugin_result_t;
typedef int32_t rns_log_level_t;
typedef struct rns_plugin rns_plugin_t;

typedef struct rns_rx_metadata {
    uint32_t valid_fields;
    int16_t rssi_dbm;
    int16_t snr_db;
} rns_rx_metadata_t;

typedef struct rns_host_api {
    uint32_t abi_major, abi_minor, struct_size, reserved0;
    void *host_context;
    void (*log)(void *, rns_log_level_t, const uint8_t *, size_t);
    void (*set_bitrate)(void *, uint64_t);
    void (*set_online)(void *, uint8_t);
    void (*rx_packet)(void *, const uint8_t *, size_t,
                      const rns_rx_metadata_t *, size_t);
} rns_host_api_t;

#define RNS_HOST_API_V1_0_SIZE \
    (offsetof(rns_host_api_t, rx_packet) + sizeof(((rns_host_api_t *)0)->rx_packet))

typedef struct rns_string { const uint8_t *data; size_t len; } rns_string_t;
#define RNS_STRING_LITERAL(v) { (const uint8_t *)(v), sizeof(v) - 1 }
typedef struct rns_plugin_info {
    rns_string_t name, version, description;
} rns_plugin_info_t;

typedef struct rns_plugin_api {
    uint32_t abi_major, abi_minor, struct_size, reserved0;
    const rns_plugin_info_t *info;
    size_t info_size;
    rns_plugin_result_t (*create)(const rns_host_api_t *, const uint8_t *,
                                  size_t, rns_plugin_t **);
    rns_plugin_result_t (*send)(rns_plugin_t *, const uint8_t *, size_t);
    void (*destroy)(rns_plugin_t *);
} rns_plugin_api_t;

#define RNS_PLUGIN_EXPORT __attribute__((visibility("default")))
RNS_PLUGIN_EXPORT const rns_plugin_api_t *rns_plugin_get_api(void);
#endif
