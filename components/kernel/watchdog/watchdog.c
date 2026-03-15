#include "watchdog.h"

#include "esp_log.h"
#include "esp_system.h"

#define TAG "WATCHDOG"
#define MAX_WD 16

typedef struct
{
    uint32_t timeout_cycles;
    uint32_t last_feed;
    bool active;

} wd_entry_t;

static wd_entry_t table[MAX_WD];
static uint32_t global_cycle = 0;

void watchdog_tick(void)
{
    global_cycle++;
}

void watchdog_init(void)
{
    for(int i=0;i<MAX_WD;i++)
        table[i].active=false;

    ESP_LOGI(TAG,"Watchdog initialized");
}

void watchdog_register(uint8_t id,uint32_t timeout_cycles)
{
    if(id>=MAX_WD) return;

    table[id].timeout_cycles=timeout_cycles;
    table[id].last_feed=global_cycle;
    table[id].active=true;

    ESP_LOGI(TAG,"Registered WD %u",id);
}

void IRAM_ATTR watchdog_feed(uint8_t id)
{
    if(id>=MAX_WD) return;
    if(!table[id].active) return;

    table[id].last_feed=global_cycle;
}

void watchdog_check(void)
{
    for(int i=0;i<MAX_WD;i++)
    {
        if(!table[i].active) continue;

        if(global_cycle - table[i].last_feed > table[i].timeout_cycles)
        {
            ESP_LOGE(TAG,"WATCHDOG TIMEOUT task=%d",i);
            esp_restart();
        }
    }
}
