#ifndef ENDAP_NVS_H
#define ENDAP_NVS_H

#include "nvs.h"
#include "kernel_trace.h"

static inline esp_err_t endap_nvs_commit(nvs_handle_t handle) {
    TRACE_NVS_BEGIN();
    esp_err_t err = nvs_commit(handle);
    TRACE_NVS_END();
    return err;
}

#endif
