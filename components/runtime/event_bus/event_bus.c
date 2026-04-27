#include "event_bus.h"
#include "esp_attr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <string.h>

#define EVENT_MASK (EVENT_BUS_MAX_EVENTS - 1)

/* ============================================================
   EVENT QUEUE (RING BUFFER)
============================================================ */

static endap_event_t event_queue[EVENT_BUS_MAX_EVENTS];

static volatile uint8_t head = 0;
static volatile uint8_t tail = 0;
static portMUX_TYPE event_bus_lock = portMUX_INITIALIZER_UNLOCKED;

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
   INIT
============================================================ */

void event_bus_init(void)
{
    head = 0;
    tail = 0;

    memset(routes, 0, sizeof(routes));
}

/* ============================================================
   PUBLISH EVENT
============================================================ */

void IRAM_ATTR event_bus_publish(endap_event_t event)
{
    portENTER_CRITICAL(&event_bus_lock);

    uint8_t next = (head + 1) & EVENT_MASK;

    /* queue full → drop */
    if (next == tail)
    {
        portEXIT_CRITICAL(&event_bus_lock);
        return;
    }

    event_queue[head] = event;
    head = next;

    portEXIT_CRITICAL(&event_bus_lock);
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
   DISPATCH EVENTS (BUDGETED)
============================================================ */

void IRAM_ATTR event_bus_dispatch_budgeted(uint8_t max_events)
{
    if (max_events == 0)
        return;

    uint8_t processed = 0;

    while (processed < max_events)
    {
        endap_event_t ev;
        uint8_t type;

        portENTER_CRITICAL(&event_bus_lock);

        if (tail == head)
        {
            portEXIT_CRITICAL(&event_bus_lock);
            break;
        }

        ev = event_queue[tail];
        tail = (tail + 1) & EVENT_MASK;

        portEXIT_CRITICAL(&event_bus_lock);

        type = ev.type;

        if (type < EVENT_TYPE_MAX)
        {
            event_route_t *r = &routes[type];
            uint8_t count = r->count;

            for (uint8_t h = 0; h < count; h++)
            {
                event_handler_t handler = r->handlers[h];

                if (handler)
                    handler(&ev);
            }
        }

        processed++;
    }
}

/* ============================================================
   DISPATCH EVENTS (LEGACY / FULL DRAIN)
============================================================ */

void IRAM_ATTR event_bus_dispatch(void)
{
    event_bus_dispatch_budgeted(EVENT_BUS_MAX_EVENTS - 1);
}
