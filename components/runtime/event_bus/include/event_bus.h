#pragma once

#include <stdint.h>

#define EVENT_BUS_MAX_EVENTS   64
#define EVENT_BUS_MAX_HANDLERS 8

/* ============================================================
   EVENT TYPES
============================================================ */

typedef enum
{
    EVENT_INPUT_CHANGE,
    EVENT_STATE_CHANGE,
    EVENT_TIMER,
    EVENT_TYPE_MAX

} endap_event_type_t;

/* ============================================================
   EVENT STRUCT
============================================================ */

typedef struct
{
    uint8_t type;
    uint16_t source;
    int32_t data;

} endap_event_t;

/* ============================================================
   HANDLER
============================================================ */

typedef void (*event_handler_t)(const endap_event_t *ev);

/* ============================================================
   API
============================================================ */

void event_bus_init(void);

void event_bus_publish(endap_event_t event);

/* compatibilidade / comportamento legado */
void event_bus_dispatch(void);

/* nova API com budget explícito */
void event_bus_dispatch_budgeted(uint8_t max_events);

void event_bus_subscribe(
    endap_event_type_t type,
    event_handler_t handler);
