#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>

void logger_init(void);
void logger_log(uint16_t event_id, int32_t value);
void logger_process(void);

#endif
