#include "protocol.h"
//#include "eventbus.h"

#define EVENT_PROTOCOL_FRAME  60

void protocol_init(void)
{
}

void protocol_process_frame(uint8_t *data, uint16_t len)
{
    (void)len;
//    eventbus_publish(EVENT_PROTOCOL_FRAME, data);
}
