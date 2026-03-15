#include "automation_engine.h"
#include "automation_node.h"

#include "event_bus.h"
#include "state.h"

static automation_node_t nodes[16];
static int node_count = 0;

static void automation_event_handler(const endap_event_t *ev)
{
    if(ev->type != EVENT_INPUT_CHANGE)
        return;

    for(int i=0;i<node_count;i++)
    {
        automation_node_t *n = &nodes[i];

        if(n->input != ev->source)
            continue;

        if(ev->data > n->threshold)
            state_set_int(n->output,1);
        else
            state_set_int(n->output,0);
    }
}

void automation_engine_init(void)
{
    event_bus_subscribe(
        EVENT_INPUT_CHANGE,
        automation_event_handler);
}
