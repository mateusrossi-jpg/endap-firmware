#include "include/v3_api.h"
#include "include/v3_catalog.h"
#include <stdio.h>
#include <string.h>

resource_id_t v3_api_stream_catalog_ndjson(v3_stream_sink_fn sink, void *sink_ctx, const v3_api_cursor_t *cursor) {
    if (!sink || !cursor) return 0;
    
    resource_backend_t *cat = v3_catalog_get_default_backend();
    size_t total = cat->size(cat);
    size_t start_index = 0;

    if (cursor->after_id != 0) {
        // Binary search to find after_id
        int low = 0;
        int high = (int)total - 1;
        int found_idx = -1;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            const resource_record_t *rec = cat->get_by_index(cat, mid);
            if (rec->id == cursor->after_id) {
                found_idx = mid;
                break;
            }
            if (rec->id < cursor->after_id) low = mid + 1;
            else high = mid - 1;
        }
        if (found_idx != -1) start_index = found_idx + 1;
        else start_index = 0; // Not found, start from beginning?
    }

    char stream_buf[V3_API_STREAM_BUFFER_SIZE];
    size_t buf_pos = 0;
    resource_id_t last_id = 0;

    for (size_t i = start_index; i < total && i < start_index + cursor->limit; i++) {
        const resource_record_t *rec = cat->get_by_index(cat, i);
        
        char line[128]; // Enough for the NDJSON line
        int len = snprintf(line, sizeof(line), "{\"id\":\"%llu\",\"k\":\"%llu\",\"s\":%u,\"t\":%lu,\"d\":%lu,\"r\":%lu}\n",
                           (unsigned long long)rec->id, (unsigned long long)rec->key.raw,
                           (unsigned int)rec->status, (unsigned long)rec->last_seen_tick,
                           (unsigned long)rec->desired_state, (unsigned long)rec->reported_state);
        
        if (len < 0) continue; 
        
        if (buf_pos + (size_t)len >= V3_API_STREAM_BUFFER_SIZE) {
            sink(sink_ctx, (uint8_t*)stream_buf, buf_pos);
            buf_pos = 0;
        }
        
        memcpy(&stream_buf[buf_pos], line, (size_t)len);
        buf_pos += (size_t)len;
        last_id = rec->id;
    }

    if (buf_pos > 0) {
        sink(sink_ctx, (uint8_t*)stream_buf, buf_pos);
    }

    return last_id;
}
