#include "event_bus.h"
#include "esp_attr.h"

#define EVENT_MASK (EVENT_BUS_MAX_EVENTS - 1)

/* ============================================================
   EVENT QUEUE (RING BUFFER)
============================================================ */

static endap_event_t event_queue[EVENT_BUS_MAX_EVENTS];

static volatile uint8_t head = 0;
static volatile uint8_t tail = 0;

/* ============================================================
   ROUTING TABLE
============================================================ */

typedef struct
{
    event_handler_t handlers[EVENT_BUS_MAX_HANDLERS];
    uint8_t count;

} event_route_t;

static event_route_t routes[EVENT_TYPE_MAX];

/* ============================================================
   PUBLISH EVENT
============================================================ */

void IRAM_ATTR event_bus_publish(endap_event_t event)
{
    uint8_t next = (head + 1) & EVENT_MASK;

    /* queue full → drop */

    if (next == tail)
        return;

    event_queue[head] = event;

    head = next;
}

/* ============================================================
   SUBSCRIBE
============================================================ */

void event_bus_subscribe(
    endap_event_type_t type,
    event_handler_t handler)
{
    if (type >= EVENT_TYPE_MAX)
        return;

    event_route_t *r = &routes[type];

    if (r->count >= EVENT_BUS_MAX_HANDLERS)
        return;

    r->handlers[r->count++] = handler;
}

/* ============================================================
   DISPATCH EVENTS
============================================================ */

void IRAM_ATTR event_bus_dispatch(void)
{
    while (tail != head)
    {
        endap_event_t *ev = &event_queue[tail];

        uint8_t type = ev->type;

        if (type < EVENT_TYPE_MAX)
        {
            event_route_t *r = &routes[type];

            uint8_t count = r->count;

            for (uint8_t h = 0; h < count; h++)
            {
                event_handler_t handler = r->handlers[h];

                if (handler)
                    handler(ev);
            }
        }

        tail = (tail + 1) & EVENT_MASK;
    }
}
