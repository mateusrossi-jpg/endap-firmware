#include "parser.h"

#define START_BYTE '<'
#define END_BYTE   '>'

static uint8_t frame[PARSER_MAX_FRAME];
static uint16_t index = 0;

static bool receiving = false;
static bool frame_ready = false;



void parser_init(void)
{
    index = 0;
    receiving = false;
    frame_ready = false;
}



bool parser_process_byte(uint8_t b)
{
    if(b == START_BYTE)
    {
        receiving = true;
        index = 0;
        return false;
    }

    if(!receiving)
        return false;

    if(b == END_BYTE)
    {
        frame_ready = true;
        receiving = false;
        return true;
    }

    if(index < PARSER_MAX_FRAME)
    {
        frame[index++] = b;
    }

    return false;
}



bool parser_frame_available(void)
{
    return frame_ready;
}



int parser_get_frame(uint8_t *buf, int max_len)
{
    if(!frame_ready)
        return 0;

    int len = index;

    if(len > max_len)
        len = max_len;

    for(int i=0;i<len;i++)
        buf[i] = frame[i];

    frame_ready = false;
    index = 0;

    return len;
}
