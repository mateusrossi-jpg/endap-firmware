#pragma once
#include "esp_log.h"

#define LOGI(tag, msg, ...) ESP_LOGI(tag, msg, ##__VA_ARGS__)
#define LOGW(tag, msg, ...) ESP_LOGW(tag, msg, ##__VA_ARGS__)
#define LOGE(tag, msg, ...) ESP_LOGE(tag, msg, ##__VA_ARGS__)
#define LOGD(tag, msg, ...) ESP_LOGD(tag, msg, ##__VA_ARGS__)
