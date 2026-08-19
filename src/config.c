#include "config.h"
#include <cyaml/cyaml.h>
#include <stdio.h>

static const cyaml_schema_field_t gpio_fields[] = {
    CYAML_FIELD_UINT("port", CYAML_FLAG_DEFAULT, gpio_config_t, port),
    CYAML_FIELD_UINT("pin", CYAML_FLAG_DEFAULT, gpio_config_t, pin),
    CYAML_FIELD_END
};
static const cyaml_schema_field_t config_fields[] = {
    CYAML_FIELD_STRING_PTR("spi", CYAML_FLAG_POINTER, plugin_config_t, spi, 1, 4096),
    CYAML_FIELD_MAPPING("cs", CYAML_FLAG_DEFAULT, plugin_config_t, cs, gpio_fields),
    CYAML_FIELD_MAPPING("rst", CYAML_FLAG_DEFAULT, plugin_config_t, rst, gpio_fields),
    CYAML_FIELD_MAPPING("busy", CYAML_FLAG_DEFAULT, plugin_config_t, busy, gpio_fields),
    CYAML_FIELD_MAPPING("dio1", CYAML_FLAG_DEFAULT, plugin_config_t, dio1, gpio_fields),
    CYAML_FIELD_MAPPING_PTR("rx_en", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, plugin_config_t, rx_en, gpio_fields),
    CYAML_FIELD_MAPPING_PTR("tx_en", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, plugin_config_t, tx_en, gpio_fields),
    CYAML_FIELD_UINT("frequency", CYAML_FLAG_DEFAULT, plugin_config_t, frequency),
    CYAML_FIELD_UINT("bandwidth", CYAML_FLAG_DEFAULT, plugin_config_t, bandwidth),
    CYAML_FIELD_UINT("spreading_factor", CYAML_FLAG_DEFAULT, plugin_config_t, spreading_factor),
    CYAML_FIELD_UINT("coding_rate", CYAML_FLAG_DEFAULT, plugin_config_t, coding_rate),
    CYAML_FIELD_UINT("tx_power", CYAML_FLAG_DEFAULT, plugin_config_t, tx_power),
    CYAML_FIELD_UINT("preamble_symbols", CYAML_FLAG_OPTIONAL, plugin_config_t, preamble_symbols),
    CYAML_FIELD_UINT("sync_word", CYAML_FLAG_OPTIONAL, plugin_config_t, sync_word),
    CYAML_FIELD_FLOAT("tcxo_voltage", CYAML_FLAG_OPTIONAL, plugin_config_t, tcxo_voltage),
    CYAML_FIELD_END
};
static const cyaml_schema_value_t config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, plugin_config_t, config_fields)
};
static const cyaml_config_t yaml_config = {
    .log_fn = cyaml_log,
    .mem_fn = cyaml_mem,
    .log_level = CYAML_LOG_WARNING,
    .flags = 0,
};

static bool bandwidth_valid(uint32_t value) {
    static const uint32_t values[] = { 7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000 };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (value == values[i])
            return true;
    return false;
}

bool config_parse(const uint8_t *data, size_t len, plugin_config_t **out, char *error, size_t error_size) {
    cyaml_err_t result;
    if (!out || !error || error_size == 0 || (!data && len))
        return false;
    *out = NULL;
    result = cyaml_load_data(data ? data : (const uint8_t *) "{}", data ? len : 2, &yaml_config, &config_schema, (cyaml_data_t **) out, NULL);
    if (result != CYAML_OK) {
        snprintf(error, error_size, "invalid configuration: %s", cyaml_strerror(result));
        return false;
    }
    if (!*out || !(*out)->spi || (*out)->frequency < 150000000 || (*out)->frequency > 960000000 ||
        !bandwidth_valid((*out)->bandwidth) || (*out)->spreading_factor < 5 ||
        (*out)->spreading_factor > 12 || (*out)->coding_rate < 4 || (*out)->coding_rate > 8 ||
        (*out)->tx_power < 8 || (*out)->tx_power > 30) {
        snprintf(error, error_size, "missing or out-of-range radio configuration");
        config_free(*out);
        *out = NULL;
        return false;
    }
    if ((*out)->preamble_symbols == 0)
        (*out)->preamble_symbols = 25;
    if ((*out)->sync_word == 0)
        (*out)->sync_word = UINT16_C(0x1424);
    if ((*out)->tcxo_voltage == 0.0)
        (*out)->tcxo_voltage = 1.8;
    if ((*out)->preamble_symbols > UINT16_MAX || (*out)->tcxo_voltage < 1.6 || (*out)->tcxo_voltage > 3.3) {
        snprintf(error, error_size, "preamble_symbols or tcxo_voltage is out of range");
        config_free(*out);
        *out = NULL;
        return false;
    }
    return true;
}

void config_free(plugin_config_t *config) {
    if (config)
        cyaml_free(&yaml_config, &config_schema, (cyaml_data_t *) config, 0);
}
