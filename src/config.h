#ifndef RNS_SX1262_CONFIG_H
#define RNS_SX1262_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t port;
    uint8_t pin;
} gpio_config_t;

typedef struct {
    char          *spi;
    gpio_config_t  cs, rst, busy, dio1;
    gpio_config_t *rx_en, *tx_en;
    uint32_t       frequency, bandwidth, preamble_symbols;
    uint8_t        spreading_factor, coding_rate, tx_power;
    uint16_t       sync_word;
    double         tcxo_voltage;
    uint32_t       irq_watchdog_seconds;
    uint8_t        hard_reset_after;
} plugin_config_t;

bool config_parse(const uint8_t *, size_t, plugin_config_t **, char *, size_t);
void config_free(plugin_config_t *);

#endif
