#include "io_image.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define IO_MAX 256

/* ======================================================
LOCK
====================================================== */

static portMUX_TYPE io_lock = portMUX_INITIALIZER_UNLOCKED;

/* ======================================================
DOUBLE BUFFERS
====================================================== */

static int32_t input_a[IO_MAX];
static int32_t input_b[IO_MAX];

static int32_t output_a[IO_MAX];
static int32_t output_b[IO_MAX];

static int32_t *input_read;
static int32_t *input_write;

static int32_t *output_read;
static int32_t *output_write;

/* ======================================================
INIT
====================================================== */

void io_image_init(void)
{
    memset(input_a,0,sizeof(input_a));
    memset(input_b,0,sizeof(input_b));

    memset(output_a,0,sizeof(output_a));
    memset(output_b,0,sizeof(output_b));

    input_read = input_a;
    input_write = input_b;

    output_read = output_a;
    output_write = output_b;
}

/* ======================================================
INPUT
====================================================== */

void io_image_set_input(uint16_t id, int32_t value)
{
    if(id >= IO_MAX)
        return;

    input_write[id] = value;
}

int32_t io_image_get_input(uint16_t id)
{
    if(id >= IO_MAX)
        return 0;

    return input_read[id];
}

/* ======================================================
OUTPUT
====================================================== */

void io_image_set_output(uint16_t id, int32_t value)
{
    if(id >= IO_MAX)
        return;

    output_write[id] = value;
}

int32_t io_image_get_output(uint16_t id)
{
    if(id >= IO_MAX)
        return 0;

    return output_read[id];
}

/* ======================================================
INPUT SWAP
====================================================== */

void io_image_scan_inputs(void)
{
    portENTER_CRITICAL(&io_lock);

    int32_t *tmp = input_read;
    input_read = input_write;
    input_write = tmp;

    portEXIT_CRITICAL(&io_lock);
}

/* ======================================================
OUTPUT SWAP
====================================================== */

void io_image_apply_outputs(void)
{
    portENTER_CRITICAL(&io_lock);

    int32_t *tmp = output_read;
    output_read = output_write;
    output_write = tmp;

    portEXIT_CRITICAL(&io_lock);
}
