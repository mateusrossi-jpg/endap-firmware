#include "device_manager.h"
#include "device_graph.h"
#include "state.h"
#include "esp_log.h"
#include <stddef.h>
#include "feedback.h"

#define TAG "DEV_MGR"
#define MAX_DEVICES 64

static device_t *devices[MAX_DEVICES];


/* ================= INIT ================= */

void device_manager_init(void)
{
    for(int i=0;i<MAX_DEVICES;i++)
        devices[i] = NULL;
}


/* ================= REGISTER ================= */

bool device_register(device_t *dev)
{
    if(!dev)
        return false;

    if(dev->id >= MAX_DEVICES)
        return false;

    if(devices[dev->id] != NULL)
        return false;

    devices[dev->id] = dev;

/* registra no graph determinístico */

device_graph_register_device(
    dev->id,
    dev->node_id,
    dev->type
);

ESP_LOGI(TAG,"Registrado device id=%d",dev->id);

return true;
}


/* ================= WRITE ================= */

bool device_write(uint16_t id,int32_t value)
{
    if(id >= MAX_DEVICES)
        return false;

    device_t *d = devices[id];
    if(!d || !d->write)
        return false;

    d->write(id,value);

    state_set_int(d->state_id,value);
    feedback_emit(id,value);

    ESP_LOGD(TAG,"WRITE id=%d value=%ld",id,value);

    return true;
}


/* ================= READ ================= */

bool device_read(uint16_t id,int32_t *out)
{
    if(id >= MAX_DEVICES || !out)
        return false;

    device_t *d = devices[id];
    if(!d || !d->read)
        return false;

    *out = d->read(id);
    return true;
}


/* ================= LOOP ================= */

void device_manager_process(void)
{
    // reservado para lógica futura
}
