#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/*
============================================================
 Protocol Engine Interface
============================================================
*/

void protocol_init(void);

void protocol_process_frame(uint8_t *data, uint16_t len);

#endif
