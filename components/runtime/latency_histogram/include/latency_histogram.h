#pragma once

#include <stdint.h>

void latency_histogram_init(void);

void latency_histogram_record(uint32_t latency_us);

void latency_histogram_log(void);
