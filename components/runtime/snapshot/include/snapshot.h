#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    bool (*read)(void *buffer, uint32_t len);
    bool (*write)(const void *buffer, uint32_t len);

} snapshot_storage_if_t;

/* init */

void snapshot_init(void);

/* runtime */

void snapshot_process(void);
void snapshot_force_save(void);

/* restore */

void snapshot_restore(const uint8_t *buffer, uint16_t len);

/* export */

uint16_t snapshot_export(uint8_t *buffer, uint16_t max_len);

/* crc */

uint32_t snapshot_get_crc(void);

/* storage backend */

void snapshot_set_storage(snapshot_storage_if_t *iface);

/* SNAP dirty */

void snapshot_mark_dirty(void);
    

#endif
