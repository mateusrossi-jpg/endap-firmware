#pragma once

#include <stdint.h>

#define EVENT_BUS_MAX_EVENTS   32
#define EVENT_BUS_MAX_HANDLERS 8
#define EVENT_TYPE_MAX         16

typedef enum
{
    EVENT_INPUT_CHANGE = 1,
    EVENT_FIELD_BUS_FRAME,
    EVENT_AUTOMATION_TRIGGER

} endap_event_type_t;

typedef struct
{
    endap_event_type_t type;
    uint16_t source;
    int32_t data;

} endap_event_t;

typedef void (*event_handler_t)(const endap_event_t *ev);

/* ============================================================
   API
============================================================ */

void event_bus_publish(endap_event_t event);

void event_bus_dispatch(void);

void event_bus_subscribe(
    endap_event_type_t type,
    event_handler_t handler
);
