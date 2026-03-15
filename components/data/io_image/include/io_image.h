#ifndef IO_IMAGE_H
#define IO_IMAGE_H

#include <stdint.h>

void io_image_init(void);

void io_image_set_input(uint16_t id, int32_t value);
int32_t io_image_get_input(uint16_t id);

void io_image_set_output(uint16_t id, int32_t value);
int32_t io_image_get_output(uint16_t id);

void io_image_scan_inputs(void);
void io_image_apply_outputs(void);

#endif
