#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PARSER_MAX_FRAME 128

void parser_init(void);

bool parser_process_byte(uint8_t byte);

bool parser_frame_available(void);

int parser_get_frame(uint8_t *buf, int max_len);
