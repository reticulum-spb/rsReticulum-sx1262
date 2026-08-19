#include "config.h"
#include <assert.h>
#include <string.h>
int main(void) {
    static const uint8_t valid[] =
        "spi: /dev/spidev0.0\ncs: {port: 0, pin: 1}\nrst: {port: 0, pin: 2}\n"
        "busy: {port: 0, pin: 3}\ndio1: {port: 0, pin: 4}\nfrequency: 868000000\n"
        "bandwidth: 125000\nspreading_factor: 7\ncoding_rate: 5\ntx_power: 22\n";
    static const uint8_t unknown[] =
        "spi: x\ncs: {port: 0, pin: 1}\nrst: {port: 0, pin: 2}\nbusy: {port: 0, pin: 3}\n"
        "dio1: {port: 0, pin: 4}\nfrequency: 868000000\nbandwidth: 125000\n"
        "spreading_factor: 7\ncoding_rate: 5\ntx_power: 22\nunknown: true\n";
    plugin_config_t *config=NULL; char error[256];
    assert(config_parse(valid,sizeof(valid)-1,&config,error,sizeof(error)));
    assert(config && config->preamble_symbols==25 && config->sync_word==0x1424);
    assert(config->rx_en==NULL && config->tx_en==NULL);
    config_free(config); config=NULL;
    assert(!config_parse(unknown,sizeof(unknown)-1,&config,error,sizeof(error)));
    assert(config==NULL && strstr(error,"invalid configuration")!=NULL);
    return 0;
}
