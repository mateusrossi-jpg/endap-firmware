#include "io_command.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <string.h>

#define IO_CMD_QUEUE_SIZE 32

static io_cmd_t queue[IO_CMD_QUEUE_SIZE];
static volatile uint8_t head = 0;
static volatile uint8_t tail = 0;
static portMUX_TYPE io_command_lock = portMUX_INITIALIZER_UNLOCKED;

void io_command_init(void)
{
    head = tail = 0;
}

bool io_command_push(uint16_t id, int32_t value)
{
    bool pushed = false;

    portENTER_CRITICAL(&io_command_lock);

    uint8_t next = (head + 1) % IO_CMD_QUEUE_SIZE;

    if (next != tail)
    {
        queue[head].id = id;
        queue[head].value = value;
        head = next;
        pushed = true;
    }

    portEXIT_CRITICAL(&io_command_lock);

    return pushed;
}

bool io_command_pop(io_cmd_t *cmd)
{
    bool popped = false;

    if (!cmd)
        return false;

    portENTER_CRITICAL(&io_command_lock);

    if (tail != head)
    {
        *cmd = queue[tail];
        tail = (tail + 1) % IO_CMD_QUEUE_SIZE;
        popped = true;
    }

    portEXIT_CRITICAL(&io_command_lock);

    return popped;
}
