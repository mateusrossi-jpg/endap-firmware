#include "include/v3_catalog.h"
#include <string.h>
#include <esp_attr.h>

static resource_record_t s_pool[V3_CATALOG_MAX_RESOURCES];
static size_t s_count = 0;


static int IRAM_ATTR find_index(resource_id_t id) {
    int low = 0;
    int high = (int)s_count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (s_pool[mid].id == id) return mid;
        if (s_pool[mid].id < id) low = mid + 1;
        else high = mid - 1;
    }
    return -1;
}

static int find_insertion_index(resource_id_t id) {
    int low = 0;
    int high = (int)s_count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (s_pool[mid].id == id) return mid;
        if (s_pool[mid].id < id) low = mid + 1;
        else high = mid - 1;
    }
    return low;
}

static bool insert(struct resource_backend_interface *self, const resource_record_t *record) {
    int idx = find_insertion_index(record->id);
    
    if (idx < (int)s_count && s_pool[idx].id == record->id) {
        s_pool[idx] = *record;
        return true;
    }
    
    if (s_count >= V3_CATALOG_MAX_RESOURCES) return false;
    
    if (idx < (int)s_count) {
        memmove(&s_pool[idx + 1], &s_pool[idx], (s_count - idx) * sizeof(resource_record_t));
    }
    
    s_pool[idx] = *record;
    s_count++;
    return true;
}

static const resource_record_t* IRAM_ATTR find(struct resource_backend_interface *self, resource_id_t id) {
    int idx = find_index(id);
    return (idx == -1) ? NULL : &s_pool[idx];
}

static const resource_record_t* IRAM_ATTR get_by_index(struct resource_backend_interface *self, size_t index) {
    if (index >= s_count) return NULL;
    return &s_pool[index];
}

static bool update_desired(struct resource_backend_interface *self, resource_id_t id, uint32_t desired) {
    int idx = find_index(id);
    if (idx == -1) return false;
    s_pool[idx].desired_state = desired;
    return true;
}

static bool update_reported(struct resource_backend_interface *self, resource_id_t id, uint32_t reported) {
    int idx = find_index(id);
    if (idx == -1) return false;
    s_pool[idx].reported_state = reported;
    return true;
}

static bool remove_record(struct resource_backend_interface *self, resource_id_t id) {
    int idx = find_index(id);
    if (idx == -1) return false;
    
    if (idx < (int)s_count - 1) {
        memmove(&s_pool[idx], &s_pool[idx + 1], (s_count - idx - 1) * sizeof(resource_record_t));
    }
    s_count--;
    return true;
}

static size_t size(struct resource_backend_interface *self) {
    return s_count;
}

static bool snapshot(struct resource_backend_interface *self, resource_record_t *out_array, size_t *in_out_count) {
    if (!out_array || !in_out_count) return false;
    size_t to_copy = (*in_out_count < s_count) ? *in_out_count : s_count;
    memcpy(out_array, s_pool, to_copy * sizeof(resource_record_t));
    *in_out_count = to_copy;
    return true;
}

static resource_backend_t s_default_backend = {
    .insert = insert,
    .find = find,
    .get_by_index = get_by_index,
    .update_desired = update_desired,
    .update_reported = update_reported,
    .remove = remove_record,
    .size = size,
    .snapshot = snapshot
};

resource_backend_t* v3_catalog_get_default_backend(void) {
    return &s_default_backend;
}
