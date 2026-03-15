#ifndef STATE_EVENTS_H
#define STATE_EVENTS_H

#include <stdint.h>

/*
============================================================
 State Module Events
------------------------------------------------------------
 Events emitted by state manager
============================================================
*/

#define EVENT_STATE_CHANGED  50

typedef struct
{
    uint16_t id;
    int32_t value;
} state_event_payload_t;

#endif
