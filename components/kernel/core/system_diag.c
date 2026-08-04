#include "system_diag.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <inttypes.h>

#define TAG "SYS_DIAG"
#define SYS_DIAG_NVS_NAMESPACE "system_diag"
#define SYS_DIAG_NVS_BOOT_KEY  "boot_count"

static uint32_t g_boot_count = 0;
static esp_reset_reason_t g_reset_reason = ESP_RST_UNKNOWN;
static bool g_diag_initialized = false;

const char *system_diag_get_reset_reason_str(void)
{
    switch (g_reset_reason)
    {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXTERNAL_PIN";
        case ESP_RST_SW:        return "SW_RESET";
        case ESP_RST_PANIC:     return "EXCEPTION_PANIC";
        case ESP_RST_INT_WDT:   return "INTERRUPT_WATCHDOG";
        case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
        case ESP_RST_WDT:       return "OTHER_WATCHDOG";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

void system_diag_init(void)
{
    if (g_diag_initialized)
        return;

    g_reset_reason = esp_reset_reason();

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SYS_DIAG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK)
    {
        uint32_t count = 0;
        err = nvs_get_u32(nvs, SYS_DIAG_NVS_BOOT_KEY, &count);
        if (err != ESP_OK)
        {
            count = 0;
        }

        count++;
        g_boot_count = count;

        nvs_set_u32(nvs, SYS_DIAG_NVS_BOOT_KEY, count);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    else
    {
        g_boot_count = 1;
        ESP_LOGE(TAG, "Falha ao abrir NVS namespace '%s': %s", SYS_DIAG_NVS_NAMESPACE, esp_err_to_name(err));
    }

    g_diag_initialized = true;

    ESP_LOGI(TAG, "Boot #%" PRIu32 " | Motivo do Reset: %s (%d)",
             g_boot_count, system_diag_get_reset_reason_str(), (int)g_reset_reason);
}

uint32_t system_diag_get_boot_count(void)
{
    if (!g_diag_initialized)
    {
        ESP_LOGW(TAG, "system_diag_get_boot_count chamado antes de system_diag_init — inicializando tardiamente");
        system_diag_init();
    }
    return g_boot_count;
}

esp_reset_reason_t system_diag_get_reset_reason(void)
{
    return g_reset_reason;
}
