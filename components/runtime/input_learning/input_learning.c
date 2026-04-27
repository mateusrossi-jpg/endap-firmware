#include "input_learning.h"

#include "device_profile.h"
#include "event_bus.h"
#include "io_binding.h"

#include <string.h>

static volatile bool learn_armed = false;
static volatile bool learn_found = false;
static volatile uint16_t learned_input_id = 0;
static char learned_input_name[IO_BINDING_NAME_LEN];

static void input_learning_event_handler(const endap_event_t *ev)
{
    if (!ev || ev->type != EVENT_STATE_CHANGE)
        return;

    if (!learn_armed)
        return;

    if (ev->data == 0)
        return;

    if (!device_profile_is_valid_input(ev->source))
        return;

    learned_input_id = ev->source;
    learn_found = true;
    learn_armed = false;
}

void input_learning_init(void)
{
    learn_armed = false;
    learn_found = false;
    learned_input_id = 0;
    learned_input_name[0] = '\0';

    event_bus_subscribe(EVENT_STATE_CHANGE, input_learning_event_handler);
}

void input_learning_arm(void)
{
    learned_input_id = 0;
    learn_found = false;
    learn_armed = true;
    learned_input_name[0] = '\0';
}

void input_learning_cancel(void)
{
    learn_armed = false;
    learned_input_name[0] = '\0';
}

void input_learning_clear(void)
{
    learn_armed = false;
    learn_found = false;
    learned_input_id = 0;
    learned_input_name[0] = '\0';
}

void input_learning_get_snapshot(input_learning_snapshot_t *out)
{
    const device_input_profile_t *input;
    io_binding_input_view_t binding = {0};

    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    out->armed = learn_armed;
    out->found = learn_found;
    out->input_id = learned_input_id;

    input = device_profile_find_input(learned_input_id);

    if (input)
    {
        if (io_binding_get_input(learned_input_id, &binding))
        {
            strncpy(learned_input_name, binding.name, sizeof(learned_input_name) - 1U);
            learned_input_name[sizeof(learned_input_name) - 1U] = '\0';
            out->input_name = learned_input_name;
        }
        else
        {
            out->input_name = input->name;
        }

        out->input_description = input->description;
    }
}
