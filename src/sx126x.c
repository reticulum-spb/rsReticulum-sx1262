#include "sx126x.h"
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define IRQ_TX_DONE UINT16_C(0x0001)
#define IRQ_RX_DONE UINT16_C(0x0002)
#define IRQ_PREAMBLE UINT16_C(0x0004)
#define IRQ_HEADER_VALID UINT16_C(0x0010)
#define IRQ_HEADER_ERR UINT16_C(0x0020)
#define IRQ_CRC_ERR UINT16_C(0x0040)
#define IRQ_TIMEOUT UINT16_C(0x0200)
#define IRQ_MASK (IRQ_TX_DONE|IRQ_RX_DONE|IRQ_PREAMBLE|IRQ_HEADER_VALID|IRQ_HEADER_ERR|IRQ_CRC_ERR|IRQ_TIMEOUT)
#define REG_SYNC_WORD UINT16_C(0x0740)
#define REG_OCP UINT16_C(0x08e7)

enum radio_state { RADIO_IDLE, RADIO_RX, RADIO_TX };
struct gpio_handle { struct gpiod_chip *chip; struct gpiod_line *line; };
struct sx126x {
    int spi_fd;
    struct gpio_handle cs, rst, busy, dio1, rx_en, tx_en;
    pthread_t irq_thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool mutex_ready, condition_ready, thread_started, tx_done, tx_failed;
    atomic_bool stopping;
    bool medium_busy;
    enum radio_state state;
    uint32_t frequency, bandwidth, preamble;
    uint16_t sync_word;
    uint8_t sf, cr, power, bw_code, tcxo_code;
    uint64_t bitrate;
    sx126x_rx_fn receive;
    sx126x_log_fn log;
    void *context;
};

static void log_message(sx126x_t *radio, int level, const char *format, ...) {
    char message[256]; va_list args;
    if (!radio->log) return;
    va_start(args, format); vsnprintf(message, sizeof(message), format, args); va_end(args);
    radio->log(radio->context, level, message);
}
static struct timespec deadline_ms(uint32_t milliseconds) {
    struct timespec value; clock_gettime(CLOCK_REALTIME, &value);
    value.tv_sec += milliseconds / 1000; value.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (value.tv_nsec >= 1000000000L) { value.tv_sec++; value.tv_nsec -= 1000000000L; }
    return value;
}
static void sleep_us(long microseconds) {
    struct timespec delay={.tv_sec=microseconds/1000000L,
                           .tv_nsec=(microseconds%1000000L)*1000L};
    while(nanosleep(&delay,&delay)<0&&errno==EINTR) {}
}
static bool gpio_open(struct gpio_handle *handle, const gpio_config_t *config,
                      const char *consumer, bool output, bool events) {
    if (!config) return true;
    handle->chip = gpiod_chip_open_by_number(config->port);
    if (!handle->chip) return false;
    handle->line = gpiod_chip_get_line(handle->chip, config->pin);
    if (!handle->line) return false;
    if (events) return gpiod_line_request_both_edges_events(handle->line, consumer) == 0;
    if (output) return gpiod_line_request_output(handle->line, consumer, 0) == 0;
    return gpiod_line_request_input(handle->line, consumer) == 0;
}
static void gpio_close(struct gpio_handle *handle) {
    if (handle->line) gpiod_line_release(handle->line);
    if (handle->chip) gpiod_chip_close(handle->chip);
    handle->line = NULL; handle->chip = NULL;
}
static bool spi_transfer(sx126x_t *radio, struct spi_ioc_transfer *transfers, size_t count, size_t expected) {
    int result;
    if (gpiod_line_set_value(radio->cs.line, 0) < 0) return false;
    result = ioctl(radio->spi_fd, SPI_IOC_MESSAGE(count), transfers);
    if (gpiod_line_set_value(radio->cs.line, 1) < 0) return false;
    return result == (int)expected;
}
static bool write_bytes(sx126x_t *radio, const uint8_t *data, size_t len) {
    struct spi_ioc_transfer transfer = {.tx_buf=(uintptr_t)data,.len=(uint32_t)len,.speed_hz=1000000,.bits_per_word=8};
    return spi_transfer(radio, &transfer, 1, len);
}
static bool command_read(sx126x_t *radio, const uint8_t *command, size_t command_len,
                         uint8_t *result, size_t result_len) {
    struct spi_ioc_transfer transfers[2] = {
        {.tx_buf=(uintptr_t)command,.len=(uint32_t)command_len,.speed_hz=1000000,.bits_per_word=8},
        {.rx_buf=(uintptr_t)result,.len=(uint32_t)result_len,.speed_hz=1000000,.bits_per_word=8}};
    return spi_transfer(radio, transfers, 2, command_len + result_len);
}
static bool wait_busy(sx126x_t *radio, uint32_t timeout_ms) {
    for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        int value = gpiod_line_get_value(radio->busy.line);
        if (value == 0) return true;
        if (value < 0) return false;
        sleep_us(1000);
    }
    log_message(radio, 1, "SX126x BUSY timeout"); return false;
}
static bool command(sx126x_t *radio, const uint8_t *data, size_t len) {
    return wait_busy(radio, 150) && write_bytes(radio, data, len);
}
static bool write_register(sx126x_t *radio, uint16_t address, const uint8_t *data, size_t len) {
    uint8_t buffer[32];
    if (len + 3 > sizeof(buffer)) return false;
    buffer[0]=0x0d; buffer[1]=(uint8_t)(address>>8); buffer[2]=(uint8_t)address;
    memcpy(buffer+3,data,len); return command(radio,buffer,len+3);
}
static bool set_antenna(sx126x_t *radio, enum radio_state state) {
    if (radio->rx_en.line && gpiod_line_set_value(radio->rx_en.line, 0) < 0) return false;
    if (radio->tx_en.line && gpiod_line_set_value(radio->tx_en.line, 0) < 0) return false;
    sleep_us(100);
    if (state == RADIO_RX && radio->rx_en.line && gpiod_line_set_value(radio->rx_en.line, 1) < 0) return false;
    if (state == RADIO_TX && radio->tx_en.line && gpiod_line_set_value(radio->tx_en.line, 1) < 0) return false;
    radio->state = state; return true;
}
static bool packet_params(sx126x_t *radio, uint8_t payload) {
    uint8_t data[]={0x8c,(uint8_t)(radio->preamble>>8),(uint8_t)radio->preamble,0,payload,1,0,0,0,0};
    return command(radio,data,sizeof(data));
}
static bool configure_irq(sx126x_t *radio) {
    uint8_t clear[]={0x02,0x03,0xff};
    uint8_t setup[]={0x08,(uint8_t)(IRQ_MASK>>8),(uint8_t)IRQ_MASK,
                     (uint8_t)(IRQ_MASK>>8),(uint8_t)IRQ_MASK,0,0,0,0};
    return command(radio,clear,sizeof(clear)) && command(radio,setup,sizeof(setup));
}
static bool enter_rx(sx126x_t *radio) {
    uint8_t receive[]={0x82,0xff,0xff,0xff};
    return packet_params(radio,255) && configure_irq(radio) && set_antenna(radio,RADIO_RX) && command(radio,receive,sizeof(receive));
}
static uint16_t irq_status(sx126x_t *radio) {
    uint8_t command_data[]={0x12}; uint8_t response[3]={0};
    if (!command_read(radio,command_data,sizeof(command_data),response,sizeof(response))) return 0;
    return (uint16_t)((uint16_t)response[1]<<8)|response[2];
}
static void clear_irq(sx126x_t *radio, uint16_t status) {
    uint8_t data[]={0x02,(uint8_t)(status>>8),(uint8_t)status}; (void)command(radio,data,sizeof(data));
}
static void receive_packet(sx126x_t *radio) {
    uint8_t status_command[]={0x13}, status[3]={0}, read_command[2], response[256];
    uint8_t packet_command[]={0x14}, packet_status[4]={0};
    if (!command_read(radio,status_command,sizeof(status_command),status,sizeof(status)) || status[1]<2) return;
    read_command[0]=0x1e; read_command[1]=status[2];
    if (!command_read(radio,read_command,sizeof(read_command),response,(size_t)status[1]+1)) return;
    if (!command_read(radio,packet_command,sizeof(packet_command),packet_status,sizeof(packet_status))) return;
    if (radio->receive) radio->receive(radio->context,response+1,status[1],
        (int16_t)(-(int)packet_status[1]/2), (int16_t)lround((double)(int8_t)packet_status[2]/4.0));
}
static void *irq_worker(void *opaque) {
    sx126x_t *radio=opaque;
    while (!atomic_load_explicit(&radio->stopping,memory_order_acquire)) {
        struct timespec timeout={.tv_sec=0,.tv_nsec=100000000L};
        int ready=gpiod_line_event_wait(radio->dio1.line,&timeout);
        if (ready<=0) continue;
        struct gpiod_line_event event;
        if (gpiod_line_event_read(radio->dio1.line,&event)<0 || event.event_type!=GPIOD_LINE_EVENT_RISING_EDGE) continue;
        pthread_mutex_lock(&radio->mutex);
        if (!atomic_load_explicit(&radio->stopping,memory_order_acquire)) {
            uint16_t status=irq_status(radio);
            if (status&(IRQ_PREAMBLE|IRQ_HEADER_VALID)) radio->medium_busy=true;
            if (status&(IRQ_HEADER_ERR|IRQ_RX_DONE|IRQ_CRC_ERR|IRQ_TIMEOUT)) {
                radio->medium_busy=false;
                pthread_cond_broadcast(&radio->condition);
            }
            if ((status&IRQ_RX_DONE) && !(status&IRQ_CRC_ERR)) receive_packet(radio);
            if (status&IRQ_TX_DONE) { radio->tx_done=true; pthread_cond_broadcast(&radio->condition); }
            if (status&(IRQ_TIMEOUT|IRQ_HEADER_ERR)) { radio->tx_failed=radio->state==RADIO_TX; pthread_cond_broadcast(&radio->condition); }
            clear_irq(radio,status);
            if (radio->state!=RADIO_TX) (void)enter_rx(radio);
        }
        pthread_mutex_unlock(&radio->mutex);
    }
    return NULL;
}
static bool map_bandwidth(uint32_t bandwidth, uint8_t *code) {
    static const struct {uint32_t hz;uint8_t code;} map[]={{7800,0},{10400,8},{15600,1},{20800,9},{31250,2},{41700,10},{62500,3},{125000,4},{250000,5},{500000,6}};
    for(size_t i=0;i<sizeof(map)/sizeof(map[0]);++i) {
        if(map[i].hz==bandwidth){*code=map[i].code;return true;}
    }
    return false;
}
static bool map_tcxo(double voltage, uint8_t *code) {
    static const struct {double voltage;uint8_t code;} map[]={{1.6,0},{1.7,1},{1.8,2},{2.2,3},{2.4,4},{2.7,5},{3.0,6},{3.3,7}};
    for(size_t i=0;i<sizeof(map)/sizeof(map[0]);++i) {
        if(fabs(map[i].voltage-voltage)<0.01){*code=map[i].code;return true;}
    }
    return false;
}

sx126x_t *sx126x_open(const plugin_config_t *config, sx126x_rx_fn receive,
                      sx126x_log_fn log, void *context) {
    sx126x_t *radio=calloc(1,sizeof(*radio));
    if(!radio)return NULL;
    atomic_init(&radio->stopping,false);
    radio->spi_fd=-1; radio->frequency=config->frequency; radio->bandwidth=config->bandwidth;
    radio->preamble=config->preamble_symbols; radio->sync_word=config->sync_word; radio->sf=config->spreading_factor;
    radio->cr=config->coding_rate; radio->power=config->tx_power; radio->receive=receive; radio->log=log; radio->context=context;
    if(!map_bandwidth(config->bandwidth,&radio->bw_code)||!map_tcxo(config->tcxo_voltage,&radio->tcxo_code)) goto fail;
    radio->bitrate=(uint64_t)((double)radio->sf*(4.0/radio->cr)/((double)(UINT32_C(1)<<radio->sf)/((double)radio->bandwidth/1000.0))*1000.0);
    radio->spi_fd=open(config->spi,O_RDWR|O_CLOEXEC); if(radio->spi_fd<0)goto fail;
    if(!gpio_open(&radio->cs,&config->cs,"sx1262-cs",true,false)||!gpio_open(&radio->rst,&config->rst,"sx1262-rst",true,false)||
       !gpio_open(&radio->busy,&config->busy,"sx1262-busy",false,false)||!gpio_open(&radio->dio1,&config->dio1,"sx1262-dio1",false,true)||
       !gpio_open(&radio->rx_en,config->rx_en,"sx1262-rx-en",true,false)||!gpio_open(&radio->tx_en,config->tx_en,"sx1262-tx-en",true,false))goto fail;
    if(pthread_mutex_init(&radio->mutex,NULL)!=0)goto fail;
    radio->mutex_ready=true;
    if(pthread_cond_init(&radio->condition,NULL)!=0)goto fail;
    radio->condition_ready=true;
    return radio;
fail: sx126x_close(radio); return NULL;
}

bool sx126x_start(sx126x_t *radio) {
    uint8_t standby[]={0x80,0}, packet_type[]={0x8a,1}, base[]={0x8f,0,0};
    uint8_t tcxo[]={0x97,radio->tcxo_code,0,5,0x60};
    uint64_t frf=(uint64_t)radio->frequency*UINT64_C(33554432)/UINT64_C(32000000);
    uint8_t frequency[]={0x86,(uint8_t)(frf>>24),(uint8_t)(frf>>16),(uint8_t)(frf>>8),(uint8_t)frf};
    uint8_t modulation[]={0x8b,radio->sf,radio->bw_code,(uint8_t)(radio->cr-4),0,0,0,0,0};
    uint8_t sync[]={ (uint8_t)(radio->sync_word>>8),(uint8_t)radio->sync_word };
    uint8_t pa[]={0x95,4,7,0,1}, ocp[]={radio->power>22?0x38:0x18};
    uint8_t regulator[]={0x96,radio->power>22?1:0x11};
    uint8_t tx_params[]={0x8e,(uint8_t)(radio->power-17),4};
    gpiod_line_set_value(radio->rst.line,0); sleep_us(10000);
    gpiod_line_set_value(radio->rst.line,1); sleep_us(10000);
    if(!command(radio,standby,sizeof(standby))||!command(radio,packet_type,sizeof(packet_type))||!command(radio,base,sizeof(base))||
       !command(radio,tcxo,sizeof(tcxo))||!command(radio,frequency,sizeof(frequency))||!command(radio,modulation,sizeof(modulation))||
       !write_register(radio,REG_SYNC_WORD,sync,sizeof(sync))||!command(radio,pa,sizeof(pa))||!write_register(radio,REG_OCP,ocp,sizeof(ocp))||
       !command(radio,regulator,sizeof(regulator))||!command(radio,tx_params,sizeof(tx_params))||!enter_rx(radio))return false;
    if(pthread_create(&radio->irq_thread,NULL,irq_worker,radio)!=0)return false;
    radio->thread_started=true;
    return true;
}

bool sx126x_send(sx126x_t *radio,const uint8_t *data,size_t len,uint32_t timeout_ms) {
    uint8_t buffer[257], transmit[]={0x83,0,0,0}; bool success=false;
    if(!radio||!data||len<2||len>255)return false;
    pthread_mutex_lock(&radio->mutex);
    struct timespec medium_deadline=deadline_ms(timeout_ms);
    while(radio->medium_busy&&!atomic_load_explicit(&radio->stopping,memory_order_acquire))
        if(pthread_cond_timedwait(&radio->condition,&radio->mutex,&medium_deadline)==ETIMEDOUT)goto done;
    buffer[0]=0x0e;buffer[1]=0;memcpy(buffer+2,data,len);radio->tx_done=false;radio->tx_failed=false;
    if(!command(radio,buffer,len+2)||!packet_params(radio,(uint8_t)len)||!configure_irq(radio)||!set_antenna(radio,RADIO_TX)||!command(radio,transmit,sizeof(transmit)))goto done;
    struct timespec deadline=deadline_ms(timeout_ms);
    while(!radio->tx_done&&!radio->tx_failed&&!atomic_load_explicit(&radio->stopping,memory_order_acquire))
        if(pthread_cond_timedwait(&radio->condition,&radio->mutex,&deadline)==ETIMEDOUT)break;
    success=radio->tx_done&&!radio->tx_failed&&!atomic_load_explicit(&radio->stopping,memory_order_acquire);
    if(!enter_rx(radio))success=false;
done: pthread_mutex_unlock(&radio->mutex); return success;
}
uint32_t sx126x_airtime_ms(const sx126x_t *radio,size_t len) {
    double symbol=pow(2.0,radio->sf)/radio->bandwidth*1000.0;
    double bits=8.0*len-4.0*radio->sf+44.0;
    double payload=ceil(bits/(4.0*radio->sf))*radio->cr+8.0;
    return (uint32_t)ceil((radio->preamble+4.25+payload)*symbol);
}
uint64_t sx126x_bitrate(const sx126x_t *radio){return radio?radio->bitrate:0;}
void sx126x_close(sx126x_t *radio) {
    if(!radio)return;
    atomic_store_explicit(&radio->stopping,true,memory_order_release);
    if(radio->condition_ready)pthread_cond_broadcast(&radio->condition);
    if(radio->thread_started)pthread_join(radio->irq_thread,NULL);
    if(radio->mutex_ready){pthread_mutex_lock(&radio->mutex);(void)set_antenna(radio,RADIO_IDLE);pthread_mutex_unlock(&radio->mutex);}
    gpio_close(&radio->tx_en);gpio_close(&radio->rx_en);gpio_close(&radio->dio1);gpio_close(&radio->busy);gpio_close(&radio->rst);gpio_close(&radio->cs);
    if(radio->spi_fd>=0)close(radio->spi_fd);
    if(radio->condition_ready)pthread_cond_destroy(&radio->condition);
    if(radio->mutex_ready)pthread_mutex_destroy(&radio->mutex);
    free(radio);
}
