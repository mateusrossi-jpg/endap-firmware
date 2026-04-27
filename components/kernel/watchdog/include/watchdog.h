#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* ============================================================
   API
============================================================ */

void watchdog_init(void);

void watchdog_register(uint8_t id, uint32_t timeout_ms);

void watchdog_feed(uint8_t id);

void watchdog_check(void);

#endif
