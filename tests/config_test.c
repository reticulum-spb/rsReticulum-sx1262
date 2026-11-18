#include "config.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    static const uint8_t valid[] =
        "device: /dev/lora0\nfrequency: 868000000\n"
        "bandwidth: 125000\nspreading_factor: 7\ncoding_rate: 5\ntx_power: 22\n"
        "irq_watchdog_seconds: 12\nhard_reset_after: 3\n";
    static const uint8_t unknown[] =
        "device: /dev/lora0\nfrequency: 868000000\nbandwidth: 125000\n"
        "spreading_factor: 7\ncoding_rate: 5\ntx_power: 22\nspi: x\n";
    plugin_config_t *config = NULL;
    char             error[256];
    assert(config_parse(valid, sizeof(valid) - 1, &config, error, sizeof(error)));
    assert(config && config->tx_preamble_symbols == 25 && config->rx_preamble_symbols == 65 && config->sync_word == 0x1424);
    assert(config->irq_watchdog_seconds == 12 && config->hard_reset_after == 3);
    assert(config->tx_timeout_seconds == 120);
    config_free(config);
    config = NULL;
    assert(!config_parse(unknown, sizeof(unknown) - 1, &config, error, sizeof(error)));
    assert(config == NULL && strstr(error, "invalid configuration") != NULL);
    struct { int sf, cr, power, preamble, timeout; bool valid; } cases[] = {
        {7, 5, -9, 25, 1, true}, {7, 8, 30, 4, 86400, true},
        {7, 4, 22, 25, 120, false}, {7, 9, 22, 25, 120, false},
        {7, 5, -10, 25, 120, false}, {7, 5, 31, 25, 120, false},
        {5, 5, 22, 4, 120, false}, {6, 5, 22, 12, 120, true},
        {7, 5, 22, 25, 86401, false}, {7, 5, 22, 65536, 120, false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char yaml[512];
        int len = snprintf(yaml, sizeof(yaml),
            "device: /dev/lora0\nfrequency: 868000000\nbandwidth: 125000\n"
            "spreading_factor: %d\ncoding_rate: %d\ntx_power: %d\n"
            "tx_preamble_symbols: %d\ntx_timeout_seconds: %d\n",
            cases[i].sf, cases[i].cr, cases[i].power, cases[i].preamble, cases[i].timeout);
        config = NULL;
        assert(config_parse((const uint8_t *)yaml, (size_t)len, &config, error, sizeof(error)) == cases[i].valid);
        config_free(config);
    }
    const char *extra[] = {
        "rx_preamble_symbols: 1000\ntx_preamble_symbols: 18\n",
        "rx_preamble_symbols: 3\n", "rx_preamble_symbols: 65536\n",
        "preamble_symbols: 25\n"
    };
    for (size_t i = 0; i < sizeof(extra)/sizeof(extra[0]); ++i) {
        char yaml[512];
        int len = snprintf(yaml, sizeof(yaml), "%s%s", valid, extra[i]);
        config = NULL;
        assert(config_parse((const uint8_t *)yaml, (size_t)len, &config, error, sizeof(error)) == (i == 0));
        if (config) assert(config->rx_preamble_symbols == 1000 && config->tx_preamble_symbols == 18);
        config_free(config);
    }
    return 0;
}
