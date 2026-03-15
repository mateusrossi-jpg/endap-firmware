#include "latency_histogram.h"

#include <inttypes.h>
#include "esp_log.h"

#define TAG "RS485_LATENCY"

static uint32_t bucket_lt100 = 0;
static uint32_t bucket_100_200 = 0;
static uint32_t bucket_200_500 = 0;
static uint32_t bucket_gt500 = 0;

static uint32_t sample_count = 0;

void latency_histogram_init(void)
{
    bucket_lt100 = 0;
    bucket_100_200 = 0;
    bucket_200_500 = 0;
    bucket_gt500 = 0;

    sample_count = 0;
}

void latency_histogram_record(uint32_t latency_us)
{
    sample_count++;

    if(latency_us < 100)
        bucket_lt100++;
    else if(latency_us < 200)
        bucket_100_200++;
    else if(latency_us < 500)
        bucket_200_500++;
    else
        bucket_gt500++;
}

void latency_histogram_log(void)
{
    if(sample_count == 0)
        return;

    uint32_t p_lt100 = (bucket_lt100 * 100) / sample_count;
    uint32_t p_100_200 = (bucket_100_200 * 100) / sample_count;
    uint32_t p_200_500 = (bucket_200_500 * 100) / sample_count;
    uint32_t p_gt500 = (bucket_gt500 * 100) / sample_count;

    ESP_LOGI(TAG,
        "<100us=%" PRIu32 "%% 100-200us=%" PRIu32 "%% 200-500us=%" PRIu32 "%% >500us=%" PRIu32 "%%",
        p_lt100,
        p_100_200,
        p_200_500,
        p_gt500
    );
}
