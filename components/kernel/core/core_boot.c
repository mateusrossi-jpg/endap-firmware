#include "core_supervisor.h"

/* ============================================================
   SYSTEM
============================================================ */

#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "nvs_flash.h"

/* ============================================================
   HAL
============================================================ */

//#include "hal_gpio.h"

/* ============================================================
   FIELD BUS
============================================================ */

#include "rs485.h"
#include "rs485_master.h"
#include "rs485_engine.h"
#include "fieldbus.h"
#include "bus_health_monitor.h"

/* ============================================================
   CORE DATA
============================================================ */

#include "state.h"
#include "snapshot.h"

/* ============================================================
   CONTROL ENGINE
============================================================ */

#include "control_kernel.h"
#include "control_loop.h"
#include "io_image.h"

/* ============================================================
   SERVICES
============================================================ */

#include "scheduler.h"
#include "runtime_monitor.h"

/* ============================================================
   SAFETY
============================================================ */

#include "watchdog.h"
#include "watchdog_ids.h"

#define TAG "CORE_SUP"
#define SELF_NODE_ID 1

/* ============================================================
   INFRA INIT
============================================================ */

static void core_init_infrastructure(void)
{
    ESP_LOGI(TAG,"Init Infrastructure");

   // hal_gpio_init();

    rs485_init(&(rs485_config_t)
    {
        .uart_num = UART_NUM_2,
        .tx_pin   = 25,
        .rx_pin   = 26,
        .de_pin   = 27,
        .baudrate = 115200
    });
}

/* ============================================================
   DOMAIN INIT
============================================================ */

static void core_init_domain(void)
{
    ESP_LOGI(TAG,"Init Domain");

    state_init();

    fieldbus_init();

    control_kernel_init();
}

/* ============================================================
   SERVICES INIT
============================================================ */

static void core_init_services(void)
{
    ESP_LOGI(TAG,"Init Services");
     
    /* Runtime services */

    runtime_monitor_init();
    
    /* Bus health monitor */
    
    bus_health_init();

    /* Safety */

    watchdog_init();

    watchdog_register(WD_CONTROL_LOOP, 10);

    /* Snapshot system */

    snapshot_init();
}

/* ============================================================
   CORE START
============================================================ */

void core_start(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG,"Boot ENDAP");

    core_init_infrastructure();

    core_init_domain();

    core_init_services();
    
    ESP_LOGI(TAG,"Starting Control Loop");

    control_loop_start();
}
