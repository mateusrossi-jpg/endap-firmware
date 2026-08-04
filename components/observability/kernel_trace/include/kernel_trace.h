#ifndef KERNEL_TRACE_H
#define KERNEL_TRACE_H

#include <stdint.h>

void kernel_trace_init(void);
void kernel_trace_cycle_start(uint64_t now);
void kernel_trace_cycle_end(uint64_t now);
void kernel_trace_process(void);

#include "esp_log.h"

#define TRACE_NVS_BEGIN() ESP_LOGW("NVS_TRACE", "NVS_SAVE_BEGIN")
#define TRACE_NVS_END()   ESP_LOGW("NVS_TRACE", "NVS_SAVE_END")

#define TRACE_HTTP_WRITE_BEGIN() ESP_LOGW("HTTP_TRACE", "HTTP_WRITE_BEGIN")
#define TRACE_HTTP_WRITE_END()   ESP_LOGW("HTTP_TRACE", "HTTP_WRITE_END")

#endif
