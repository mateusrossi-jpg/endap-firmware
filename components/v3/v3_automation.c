#include "include/v3_automation.h"
#include "include/v3_catalog.h"
#include <esp_attr.h>

bool v3_automation_write_desired(resource_id_t target_id, uint32_t desired_state) {
    resource_backend_t *cat = v3_catalog_get_default_backend();
    return cat->update_desired(cat, target_id, desired_state);
}

bool IRAM_ATTR v3_automation_update_reported(resource_id_t target_id, uint32_t reported_state) {
    resource_backend_t *cat = v3_catalog_get_default_backend();
    return cat->update_reported(cat, target_id, reported_state);
}
