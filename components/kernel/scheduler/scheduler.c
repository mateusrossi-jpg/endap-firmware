#include "scheduler.h"
#include "esp_attr.h"

#include "io_image.h"
#include "fieldbus.h"
#include "control_kernel.h"

void IRAM_ATTR scheduler_run_io(void)
{
    io_image_scan_inputs();
    io_image_apply_outputs();
}

void IRAM_ATTR scheduler_run_fieldbus(void)
{
    fieldbus_tick();
}

void IRAM_ATTR scheduler_run_automation(void)
{
    control_kernel_task();
}

void IRAM_ATTR scheduler_run_events(void)
{
}

void IRAM_ATTR scheduler_run_diagnostics(void)
{
}
