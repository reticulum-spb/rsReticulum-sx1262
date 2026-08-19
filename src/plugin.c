#include "rns_plugin.h"
#include "config.h"
#include "protocol.h"
#include "config_schema.h"
#include "sx126x.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rns_plugin {
    const rns_host_api_t *host;
    sx126x_t             *radio;
    rns_reassembly_t      reassembly;
    unsigned int          random_state;
    atomic_bool           committed;
};

static void host_log(const rns_host_api_t *host, rns_log_level_t level, const char *message) {
    if (host && host->log && message)
        host->log(host->host_context, level, (const uint8_t *) message, strlen(message));
}

static void radio_log(void *opaque, int level, const char *message) {
    rns_plugin_t *plugin = opaque;
    host_log(plugin->host, (rns_log_level_t) level, message);
}

static void radio_online(void *opaque, bool online) {
    rns_plugin_t *plugin = opaque;

    plugin->reassembly.sequence = UINT8_C(0xff);
    plugin->reassembly.length = 0;

    if (atomic_load_explicit(&plugin->committed, memory_order_acquire))
        plugin->host->set_online(plugin->host->host_context, online ? 1 : 0);
}

static void radio_receive(void *opaque, const uint8_t *frame, size_t frame_len, int16_t rssi, int16_t snr) {
    rns_plugin_t  *plugin = opaque;
    const uint8_t *packet;
    size_t         packet_len;

    rns_rx_metadata_t metadata = {
        .valid_fields = RNS_RX_METADATA_RSSI | RNS_RX_METADATA_SNR,
        .rssi_dbm = rssi,
        .snr_db = snr
    };

    if (!atomic_load_explicit(&plugin->committed, memory_order_acquire))
        return;

    if (rns_reassemble(&plugin->reassembly, frame, frame_len, &packet, &packet_len))
        plugin->host->rx_packet(plugin->host->host_context, packet, packet_len, &metadata, sizeof(metadata));
}

typedef struct {
    rns_plugin_t *plugin;
    bool          success;
} tx_context_t;

static void transmit_frame(void *opaque, const uint8_t *frame, size_t frame_len) {
    tx_context_t *tx = opaque;
    uint32_t      timeout;

    if (!tx->success)
        return;

    timeout = sx126x_airtime_ms(tx->plugin->radio, frame_len) * 2 + 500;
    tx->success = sx126x_send(tx->plugin->radio, frame, frame_len, timeout);
}

static rns_plugin_result_t plugin_create(const rns_host_api_t *host, const uint8_t *yaml, size_t yaml_len, rns_plugin_t **out) {
    plugin_config_t *config = NULL;
    rns_plugin_t    *plugin = NULL;
    char             error[256];

    if (!host || !out)
        return RNS_PLUGIN_ERROR;

    *out = NULL;

    if (host->abi_major != RNS_PLUGIN_ABI_MAJOR || host->struct_size < RNS_HOST_API_V1_0_SIZE ||
        !host->log || !host->set_bitrate || !host->set_online || !host->rx_packet)
        return RNS_PLUGIN_ERROR;

    if (!config_parse(yaml, yaml_len, &config, error, sizeof(error))) {
        host_log(host, RNS_LOG_ERROR, error);

        return RNS_PLUGIN_ERROR;
    }

    plugin = calloc(1, sizeof(*plugin));

    if (!plugin) {
        host_log(host, RNS_LOG_ERROR, "sx1262: cannot allocate instance");
        config_free(config);

        return RNS_PLUGIN_ERROR;
    }

    plugin->host = host;
    plugin->reassembly.sequence = UINT8_C(0xff);
    plugin->random_state = (unsigned int) (uintptr_t) plugin;
    atomic_init(&plugin->committed, false);
    plugin->radio = sx126x_open(config, radio_receive, radio_log, radio_online, plugin);
    config_free(config);

    if (!plugin->radio) {
        host_log(host, RNS_LOG_ERROR, "sx1262: cannot open SPI or GPIO resources");
        free(plugin);
        return RNS_PLUGIN_ERROR;
    }

    if (!sx126x_start(plugin->radio)) {
        host_log(host, RNS_LOG_ERROR, "sx1262: radio initialization failed");
        sx126x_close(plugin->radio);
        free(plugin);
        return RNS_PLUGIN_ERROR;
    }

    host->set_bitrate(host->host_context, sx126x_bitrate(plugin->radio));
    host->set_online(host->host_context, 1);
    atomic_store_explicit(&plugin->committed, true, memory_order_release);
    *out = plugin;
    host_log(host, RNS_LOG_INFO, "sx1262: instance created and radio online");

    return RNS_PLUGIN_OK;
}

static rns_plugin_result_t plugin_send(rns_plugin_t *plugin, const uint8_t *data, size_t len) {
    tx_context_t tx = { .plugin = plugin, .success = true };
    uint8_t      sequence;

    if (!plugin || !data || len == 0 || len > RNS_PROTOCOL_MTU)
        return RNS_PLUGIN_ERROR;

    sequence = (uint8_t) (rand_r(&plugin->random_state) & 0xf0);

    if (rns_fragment(data, len, sequence, transmit_frame, &tx) == 0 || !tx.success) {
        host_log(plugin->host, RNS_LOG_ERROR, "sx1262: physical transmission failed or timed out");
        return RNS_PLUGIN_ERROR;
    }

    return RNS_PLUGIN_OK;
}

static void plugin_destroy(rns_plugin_t *plugin) {
    if (!plugin)
        return;

    atomic_store_explicit(&plugin->committed, false, memory_order_release);
    plugin->host->set_online(plugin->host->host_context, 0);
    sx126x_close(plugin->radio);
    host_log(plugin->host, RNS_LOG_INFO, "sx1262: instance destroyed");
    free(plugin);
}

static const rns_plugin_info_t INFO = {
    .name = RNS_STRING_LITERAL("SX1262 RNode"),
    .version = RNS_STRING_LITERAL("0.1.0"),
    .description = RNS_STRING_LITERAL("Direct SPI SX1262 interface with RNode-compatible LoRa framing."),
    .config_schema_json = { (const uint8_t *) RNS_PLUGIN_CONFIG_SCHEMA, sizeof(RNS_PLUGIN_CONFIG_SCHEMA) - 1 }
};

static const rns_plugin_api_t API = {
    .abi_major = RNS_PLUGIN_ABI_MAJOR,
    .abi_minor = RNS_PLUGIN_ABI_MINOR,
    .struct_size = sizeof(rns_plugin_api_t),
    .info = &INFO,
    .info_size = sizeof(INFO),
    .create = plugin_create,
    .send = plugin_send,
    .destroy = plugin_destroy
};

RNS_PLUGIN_EXPORT const rns_plugin_api_t *rns_plugin_get_api(void) {
    return &API;
}
