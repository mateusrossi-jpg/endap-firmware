#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ladder_engine_init(void);

esp_err_t ladder_engine_load_program(const uint8_t *buffer, size_t length);

void ladder_engine_run(void);

#ifdef __cplusplus
}
#endif
