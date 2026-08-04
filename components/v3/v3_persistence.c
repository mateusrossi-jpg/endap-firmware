#include "include/v3_persistence.h"
#include "include/v3_catalog.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "endap_nvs.h"


static v3_persistence_journal_t s_journal = { .dirty_mask = {0} };
static nvs_handle_t s_nvs_handle = 0;
static const char* NVS_NAMESPACE = "v3_cat";

static void get_nvs_key(resource_id_t id, char out_key[16]) {
    char buf[16];
    int pos = 14;
    buf[15] = '\0';
    uint64_t val = id;
    if (val == 0) {
        buf[pos--] = '0';
    } else {
        while (val > 0 && pos >= 0) {
            uint64_t rem = val % 36;
            buf[pos--] = (rem < 10) ? ('0' + rem) : ('A' + (rem - 10));
            val /= 36;
        }
    }
    strcpy(out_key, &buf[pos + 1]);
}

void v3_persistence_mark_dirty(size_t idx) {
    if (idx >= V3_CATALOG_MAX_RESOURCES) return;
    s_journal.dirty_mask[idx / 32] |= (1U << (idx % 32));
}

size_t v3_persistence_flush_dirty_records(void) {
    if (s_nvs_handle == 0) {
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle) != ESP_OK) {
            return 0;
        }
    }
    
    resource_backend_t *cat = v3_catalog_get_default_backend();
    size_t write_count = 0;
    
    for (int i = 0; i < 32; i++) {
        while (s_journal.dirty_mask[i] != 0) {
            int bit_idx = __builtin_ffs(s_journal.dirty_mask[i]) - 1;
            size_t catalog_idx = (size_t)(i * 32 + bit_idx);
            
            const resource_record_t *rec = cat->get_by_index(cat, catalog_idx);
            if (rec != NULL) {
                char key_buf[16];
                get_nvs_key(rec->id, key_buf);
                if (nvs_set_blob(s_nvs_handle, key_buf, rec, sizeof(resource_record_t)) == ESP_OK) {
                    write_count++;
                }
            }
            
            s_journal.dirty_mask[i] &= ~(1U << bit_idx);
        }
    }
    
    if (write_count > 0) {
        endap_nvs_commit(s_nvs_handle);
    }
    
    return write_count;
}

bool v3_persistence_restore_catalog(void) {
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &s_nvs_handle) != ESP_OK) {
        return false;
    }
    
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NULL, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    resource_backend_t *cat = v3_catalog_get_default_backend();
    
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        
        resource_record_t rec;
        size_t required_size = sizeof(rec);
        if (nvs_get_blob(s_nvs_handle, info.key, &rec, &required_size) == ESP_OK) {
            cat->insert(cat, &rec);
        }
        
        err = nvs_entry_next(&it);
    }
    
    if (it != NULL) {
        nvs_release_iterator(it);
    }
    return true;
}
