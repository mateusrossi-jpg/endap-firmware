#include "logger.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "LOGGER"
#define LOG_SIZE 128

typedef struct
{
    uint32_t timestamp;
    uint16_t id;
    int32_t value;
} log_entry_t;

static log_entry_t buffer[LOG_SIZE];
static uint16_t head = 0;


/* ================= INIT ================= */

void logger_init(void)
{
    head = 0;
}


/* ================= LOG ================= */

void logger_log(uint16_t event_id, int32_t value)
{
    buffer[head].timestamp = esp_timer_get_time()/1000;
    buffer[head].id = event_id;
    buffer[head].value = value;

    head++;
    if(head >= LOG_SIZE)
        head = 0;
}


/* ================= PROCESS ================= */

void logger_process(void)
{
    // reservado para exportação futura
}
