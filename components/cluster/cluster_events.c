#include "cluster_events.h"
#include <stddef.h>   // 🔥 CORREÇÃO

#define MAX_SUBSCRIBERS 8

static cluster_event_handler_t subscribers[MAX_SUBSCRIBERS] = {0};

/* ============================================================
   SUBSCRIBE
============================================================ */

void cluster_events_subscribe(cluster_event_handler_t handler)
{
    for (int i = 0; i < MAX_SUBSCRIBERS; i++)
    {
        if (subscribers[i] == NULL)
        {
            subscribers[i] = handler;
            return;
        }
    }
}

/* ============================================================
   PUBLISH
============================================================ */

void cluster_publish_event(cluster_event_t *evt)
{
    for (int i = 0; i < MAX_SUBSCRIBERS; i++)
    {
        if (subscribers[i] != NULL)
        {
            subscribers[i](evt);
        }
    }
}
