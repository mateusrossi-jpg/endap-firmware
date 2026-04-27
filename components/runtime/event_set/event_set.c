#include "event_set.h"
#include "state.h"
#include "esp_attr.h"

/* ============================================================
   CONFIG
============================================================ */

#define EVENT_QUEUE_SIZE 32

/* ============================================================
   STRUCT
============================================================ */

typedef struct
{
    uint16_t id;
    int32_t value;
} event_set_t;

/* ============================================================
   QUEUE
============================================================ */

static event_set_t queue[EVENT_QUEUE_SIZE];

static volatile uint16_t head = 0;
static volatile uint16_t tail = 0;

/* ============================================================
   ENQUEUE (HTTP)
============================================================ */

bool event_enqueue_set(uint16_t id, int32_t value)
{
    uint16_t next = (head + 1) % EVENT_QUEUE_SIZE;

    if (next == tail)
        return false;

    queue[head].id = id;
    queue[head].value = value;

    head = next;

    return true;
}

/* ============================================================
   PROCESS (CICLO)
============================================================ */

void IRAM_ATTR event_process_set_queue(void)
{
    while (tail != head)
    {
        event_set_t ev = queue[tail];

        tail = (tail + 1) % EVENT_QUEUE_SIZE;

        state_set_int(ev.id, ev.value);
    }
}
