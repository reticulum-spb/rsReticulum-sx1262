#ifndef RNS_SX1262_DRIVER_H
#define RNS_SX1262_DRIVER_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sx126x sx126x_t;

typedef void (*sx126x_rx_fn)(void *, const uint8_t *, size_t, int16_t, int16_t);
typedef void (*sx126x_log_fn)(void *, int, const char *);

sx126x_t *sx126x_open(const plugin_config_t *, sx126x_rx_fn, sx126x_log_fn, void *);
bool      sx126x_start(sx126x_t *);
bool      sx126x_send(sx126x_t *, const uint8_t *, size_t, uint32_t);
uint32_t  sx126x_airtime_ms(const sx126x_t *, size_t);
uint64_t  sx126x_bitrate(const sx126x_t *);
void      sx126x_close(sx126x_t *);

#endif
