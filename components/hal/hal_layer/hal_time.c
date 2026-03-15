#include "hal_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

void hal_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint64_t hal_millis(void)
{
    return esp_timer_get_time() / 1000ULL;
}
