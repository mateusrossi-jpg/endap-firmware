#include "protocol.h"

#include <stddef.h>
#include "esp_log.h"

#define TAG "PROTOCOL"

static protocol_io_update_cb_t io_update_cb = NULL;
static protocol_output_command_cb_t output_command_cb = NULL;

void protocol_register_io_update_callback(protocol_io_update_cb_t cb)
{
    io_update_cb = cb;
}

void protocol_register_output_command_callback(protocol_output_command_cb_t cb)
{
    output_command_cb = cb;
}

void protocol_init(void)
{
    ESP_LOGI(TAG, "Protocol iniciado");
}

void protocol_process_frame(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(protocol_msg_t))
        return;

    const protocol_msg_t *msg = (const protocol_msg_t*)data;

    ESP_LOGI(TAG, "Mensagem tipo: %d", msg->type);

    switch (msg->type)
    {
        case PROTOCOL_MSG_CLUSTER_IO_UPDATE:
            if (io_update_cb)
                io_update_cb(&msg->data.io_update);
            break;

        case PROTOCOL_MSG_OUTPUT_COMMAND:
            if (output_command_cb)
                output_command_cb(&msg->data.output_command);
            break;

        default:
            break;
    }
}
