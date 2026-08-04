#include "include/v3_events.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

static v3_event_t s_queue[V3_EVENTS_QUEUE_SIZE];
static size_t s_head = 0;
static size_t s_tail = 0;
static size_t s_count = 0;
static uint32_t s_drop_count = 0;

static portMUX_TYPE s_event_spinlock = portMUX_INITIALIZER_UNLOCKED;

bool v3_events_post(resource_id_t id, v3_event_type_t type) {
    uint32_t timestamp = (uint32_t)xTaskGetTickCount();
    
    portENTER_CRITICAL(&s_event_spinlock);
    
    if (s_count == V3_EVENTS_QUEUE_SIZE) {
        // Drop Oldest
        s_tail = (s_tail + 1) & V3_EVENTS_MASK;
        s_drop_count++;
    } else {
        s_count++;
    }
    
    v3_event_t *event = &s_queue[s_head];
    event->resource_id = id;
    event->timestamp_epoch = timestamp;
    event->type = (uint8_t)type;
    memset(event->padding, 0, sizeof(event->padding));
    
    s_head = (s_head + 1) & V3_EVENTS_MASK;
    
    portEXIT_CRITICAL(&s_event_spinlock);
    return true;
}

size_t v3_events_poll(v3_event_t *out_batch, size_t max_count) {
    if (!out_batch || max_count == 0) return 0;
    
    portENTER_CRITICAL(&s_event_spinlock);
    
    size_t to_read = (s_count < max_count) ? s_count : max_count;
    
    for (size_t i = 0; i < to_read; i++) {
        out_batch[i] = s_queue[s_tail];
        s_tail = (s_tail + 1) & V3_EVENTS_MASK;
    }
    
    s_count -= to_read;
    
    portEXIT_CRITICAL(&s_event_spinlock);
    return to_read;
}

uint32_t v3_events_get_drop_count(void) {
    // Atomic read of uint32_t is generally safe, 
    // but protected here to ensure consistency if extended.
    portENTER_CRITICAL(&s_event_spinlock);
    uint32_t drop_count = s_drop_count;
    portEXIT_CRITICAL(&s_event_spinlock);
    return drop_count;
}
