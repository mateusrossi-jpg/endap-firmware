#include "io_binding.h"

#include "device_profile.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

#define TAG "IO_BINDING"
#define IO_BINDING_NAMESPACE "io_binding"
#define IO_BINDING_KEY_INPUTS "input_v1"
#define IO_BINDING_KEY_OUTPUTS "output_v1"
#define IO_BINDING_MAGIC_INPUTS 0x4942494EUL
#define IO_BINDING_MAGIC_OUTPUTS 0x49424F55UL
#define IO_BINDING_VERSION 4U
#define IO_BINDING_VERSION_V3 3U
#define IO_BINDING_VERSION_V2 2U
#define IO_BINDING_VERSION_V1 1U

typedef struct
{
    uint16_t id;
    int16_t gpio;
    int16_t backend_instance;
    int16_t endpoint_index;
    uint8_t backend;
    uint8_t channel_class;
    uint16_t reserved0;
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
} io_binding_input_entry_t;

typedef struct
{
    uint16_t id;
    int16_t gpio;
    int16_t backend_instance;
    int16_t endpoint_index;
    uint8_t backend;
    uint8_t channel_class;
    uint16_t reserved0;
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
} io_binding_output_entry_t;

typedef struct
{
    uint16_t id;
    int16_t gpio;
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
} io_binding_input_entry_v2_t;

typedef struct
{
    uint16_t id;
    int16_t gpio;
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
} io_binding_output_entry_v2_t;

typedef struct
{
    uint16_t id;
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
} io_binding_input_entry_v1_t;

typedef struct
{
    uint16_t id;
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
} io_binding_output_entry_v1_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    io_binding_input_entry_t inputs[IO_BINDING_MAX_INPUTS];
    uint32_t crc;
} io_binding_input_blob_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    io_binding_output_entry_t outputs[IO_BINDING_MAX_OUTPUTS];
    uint32_t crc;
} io_binding_output_blob_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    io_binding_input_entry_v2_t inputs[IO_BINDING_MAX_INPUTS];
    uint32_t crc;
} io_binding_input_blob_v2_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    io_binding_output_entry_v2_t outputs[IO_BINDING_MAX_OUTPUTS];
    uint32_t crc;
} io_binding_output_blob_v2_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    io_binding_input_entry_v1_t inputs[IO_BINDING_MAX_INPUTS];
    uint32_t crc;
} io_binding_input_blob_v1_t;

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    io_binding_output_entry_v1_t outputs[IO_BINDING_MAX_OUTPUTS];
    uint32_t crc;
} io_binding_output_blob_v1_t;

static const io_binding_backend_view_t binding_backends[] =
{
    {
        .backend = DEVICE_CHANNEL_BACKEND_GPIO,
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .implemented_now = true,
        .selectable_now = true,
        .expansion_path = false,
        .backend_code = "gpio",
        .label = "GPIO nativo",
        .notes = "Backend atual para canais digitais locais do nó.",
    },
    {
        .backend = DEVICE_CHANNEL_BACKEND_MCP23X17,
        .channel_class = DEVICE_CHANNEL_CLASS_DIGITAL_INPUT,
        .implemented_now = true,
        .selectable_now = true,
        .expansion_path = true,
        .backend_code = "mcp23x17",
        .label = "MCP23x17",
        .notes = "Backend digital expandido já configurável no binding; integração elétrica/runtime segue como próxima etapa.",
    },
    {
        .backend = DEVICE_CHANNEL_BACKEND_ADC_NATIVE,
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .implemented_now = false,
        .selectable_now = false,
        .expansion_path = true,
        .backend_code = "adc-native",
        .label = "ADC nativo",
        .notes = "Entradas analógicas nativas do ESP32 previstas para inventário futuro.",
    },
    {
        .backend = DEVICE_CHANNEL_BACKEND_ADC_EXTERNAL,
        .channel_class = DEVICE_CHANNEL_CLASS_ANALOG_INPUT,
        .implemented_now = false,
        .selectable_now = false,
        .expansion_path = true,
        .backend_code = "adc-external",
        .label = "ADC externo",
        .notes = "Expansão analógica dedicada, como ADS1115, prevista para próximos passos.",
    },
};

static io_binding_input_entry_t input_entries[IO_BINDING_MAX_INPUTS];
static int input_entry_count = 0;
static io_binding_output_entry_t output_entries[IO_BINDING_MAX_OUTPUTS];
static int output_entry_count = 0;
static bool gpio_restart_required = false;

static void io_binding_persist_inputs(void);
static void io_binding_persist_outputs(void);

static uint32_t io_binding_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }

    return ~crc;
}

static void io_binding_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0U)
        return;

    if (!src)
        src = "";

    strncpy(dst, src, dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}

const char *io_binding_backend_code(device_channel_backend_t backend)
{
    switch (backend)
    {
        case DEVICE_CHANNEL_BACKEND_GPIO:
            return "gpio";
        case DEVICE_CHANNEL_BACKEND_MCP23X17:
            return "mcp23x17";
        case DEVICE_CHANNEL_BACKEND_ADC_NATIVE:
            return "adc-native";
        case DEVICE_CHANNEL_BACKEND_ADC_EXTERNAL:
            return "adc-external";
        default:
            return "unknown";
    }
}

const char *io_binding_backend_label(device_channel_backend_t backend)
{
    switch (backend)
    {
        case DEVICE_CHANNEL_BACKEND_GPIO:
            return "GPIO nativo";
        case DEVICE_CHANNEL_BACKEND_MCP23X17:
            return "MCP23x17";
        case DEVICE_CHANNEL_BACKEND_ADC_NATIVE:
            return "ADC nativo";
        case DEVICE_CHANNEL_BACKEND_ADC_EXTERNAL:
            return "ADC externo";
        default:
            return "Desconhecido";
    }
}


static bool io_binding_backend_is_gpio(device_channel_backend_t backend)
{
    return backend == DEVICE_CHANNEL_BACKEND_GPIO;
}

static bool io_binding_backend_is_mcp(device_channel_backend_t backend)
{
    return backend == DEVICE_CHANNEL_BACKEND_MCP23X17;
}

static bool io_binding_backend_allows_input(device_channel_backend_t backend)
{
    const device_expansion_capabilities_t *expansion = device_profile_expansion_capabilities();

    switch (backend)
    {
        case DEVICE_CHANNEL_BACKEND_GPIO:
            return true;
        case DEVICE_CHANNEL_BACKEND_MCP23X17:
            return expansion && expansion->supports_mcp23x17;
        default:
            return false;
    }
}

static bool io_binding_backend_allows_output(device_channel_backend_t backend)
{
    const device_expansion_capabilities_t *expansion = device_profile_expansion_capabilities();

    switch (backend)
    {
        case DEVICE_CHANNEL_BACKEND_GPIO:
            return true;
        case DEVICE_CHANNEL_BACKEND_MCP23X17:
            return expansion && expansion->supports_mcp23x17;
        default:
            return false;
    }
}

bool io_binding_backend_selectable_now(device_channel_backend_t backend, bool for_input)
{
    return for_input
        ? io_binding_backend_allows_input(backend)
        : io_binding_backend_allows_output(backend);
}

static const char *io_binding_mcp_endpoint_label(int endpoint_index)
{
    static const char *labels[16] =
    {
        "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
        "B0", "B1", "B2", "B3", "B4", "B5", "B6", "B7",
    };

    if (endpoint_index < 0 || endpoint_index >= 16)
        return "?";

    return labels[endpoint_index];
}

static bool io_binding_mcp_address_valid(int backend_instance, int endpoint_index)
{
    const device_expansion_capabilities_t *expansion = device_profile_expansion_capabilities();
    int instance_count = expansion ? (int)expansion->recommended_mcp_instances : 0;
    int channel_count = expansion ? (int)expansion->channels_per_mcp : 0;

    if (!expansion || !expansion->supports_mcp23x17)
        return false;

    if (instance_count <= 0)
        instance_count = 1;

    if (channel_count <= 0)
        channel_count = 16;

    return backend_instance >= 0 && backend_instance < instance_count &&
           endpoint_index >= 0 && endpoint_index < channel_count;
}

bool io_binding_backend_address_valid(device_channel_backend_t backend,
                                      bool for_input,
                                      int gpio,
                                      int backend_instance,
                                      int endpoint_index)
{
    switch (backend)
    {
        case DEVICE_CHANNEL_BACKEND_GPIO:
            return for_input
                ? device_profile_input_gpio_allowed((gpio_num_t)gpio)
                : device_profile_output_gpio_allowed((gpio_num_t)gpio);
        case DEVICE_CHANNEL_BACKEND_MCP23X17:
            return io_binding_backend_selectable_now(backend, for_input) &&
                   io_binding_mcp_address_valid(backend_instance, endpoint_index);
        default:
            return false;
    }
}


static void io_binding_channel_address_text(device_channel_backend_t backend,
                                            int backend_instance,
                                            int endpoint_index,
                                            gpio_num_t gpio,
                                            char *dst,
                                            size_t dst_size)
{
    if (!dst || dst_size == 0U)
        return;

    dst[0] = '\0';

    switch (backend)
    {
        case DEVICE_CHANNEL_BACKEND_GPIO:
            snprintf(dst, dst_size, "GPIO%d", (int)gpio);
            break;
        case DEVICE_CHANNEL_BACKEND_MCP23X17:
            snprintf(dst, dst_size, "MCP%d:%s", backend_instance, io_binding_mcp_endpoint_label(endpoint_index));
            break;
        case DEVICE_CHANNEL_BACKEND_ADC_NATIVE:
            snprintf(dst, dst_size, "ADC%d", endpoint_index);
            break;
        case DEVICE_CHANNEL_BACKEND_ADC_EXTERNAL:
            snprintf(dst, dst_size, "ADCX%d:%d", backend_instance, endpoint_index);
            break;
        default:
            snprintf(dst, dst_size, "N/A");
            break;
    }
}

int io_binding_backend_count(void)
{
    return (int)(sizeof(binding_backends) / sizeof(binding_backends[0]));
}

const io_binding_backend_view_t *io_binding_backend_at(int index)
{
    if (index < 0 || index >= io_binding_backend_count())
        return NULL;

    return &binding_backends[index];
}

static const char *io_binding_default_input_role_for(const device_input_profile_t *profile)
{
    (void)profile;
    return "Input";
}

static const char *io_binding_default_output_role_for(const device_output_profile_t *profile)
{
    (void)profile;
    return "Actuator";
}

static io_binding_input_entry_t *io_binding_find_input_entry(uint16_t id)
{
    for (int i = 0; i < input_entry_count; i++)
    {
        if (input_entries[i].id == id)
            return &input_entries[i];
    }

    return NULL;
}

static io_binding_output_entry_t *io_binding_find_output_entry(uint16_t id)
{
    for (int i = 0; i < output_entry_count; i++)
    {
        if (output_entries[i].id == id)
            return &output_entries[i];
    }

    return NULL;
}

static bool io_binding_input_is_protected(uint16_t id)
{
    return device_profile_is_default_input_id(id);
}

static bool io_binding_output_is_protected(uint16_t id)
{
    return device_profile_is_default_output_id(id);
}

static bool io_binding_address_in_use_by_input(device_channel_backend_t backend,
                                                int backend_instance,
                                                int endpoint_index,
                                                uint16_t ignore_id)
{
    for (int i = 0; i < input_entry_count; i++)
    {
        if (input_entries[i].id == ignore_id)
            continue;

        if ((device_channel_backend_t)input_entries[i].backend == backend &&
            input_entries[i].backend_instance == backend_instance &&
            input_entries[i].endpoint_index == endpoint_index)
        {
            return true;
        }
    }

    return false;
}

static bool io_binding_address_in_use_by_output(device_channel_backend_t backend,
                                                 int backend_instance,
                                                 int endpoint_index,
                                                 uint16_t ignore_id)
{
    for (int i = 0; i < output_entry_count; i++)
    {
        if (output_entries[i].id == ignore_id)
            continue;

        if ((device_channel_backend_t)output_entries[i].backend == backend &&
            output_entries[i].backend_instance == backend_instance &&
            output_entries[i].endpoint_index == endpoint_index)
        {
            return true;
        }
    }

    return false;
}

static bool io_binding_gpio_in_use_by_input(gpio_num_t gpio, uint16_t ignore_id)
{
    return io_binding_address_in_use_by_input(DEVICE_CHANNEL_BACKEND_GPIO, 0, (int)gpio, ignore_id);
}

static bool io_binding_gpio_in_use_by_output(gpio_num_t gpio, uint16_t ignore_id)
{
    return io_binding_address_in_use_by_output(DEVICE_CHANNEL_BACKEND_GPIO, 0, (int)gpio, ignore_id);
}

static io_binding_result_t io_binding_validate_input_address(uint16_t id,
                                                             device_channel_backend_t backend,
                                                             gpio_num_t gpio,
                                                             int backend_instance,
                                                             int endpoint_index)
{
    if (!io_binding_backend_selectable_now(backend, true))
        return io_binding_backend_is_mcp(backend)
            ? IO_BINDING_RESULT_UNSUPPORTED_BACKEND
            : IO_BINDING_RESULT_INVALID_BACKEND;

    if (!io_binding_backend_address_valid(backend, true, (int)gpio, backend_instance, endpoint_index))
        return io_binding_backend_is_gpio(backend)
            ? IO_BINDING_RESULT_INVALID_GPIO
            : IO_BINDING_RESULT_INVALID_ENDPOINT;

    if (io_binding_address_in_use_by_input(backend, backend_instance, endpoint_index, id) ||
        io_binding_address_in_use_by_output(backend, backend_instance, endpoint_index, 0U))
    {
        return io_binding_backend_is_gpio(backend)
            ? IO_BINDING_RESULT_GPIO_CONFLICT
            : IO_BINDING_RESULT_ADDRESS_CONFLICT;
    }

    return IO_BINDING_RESULT_OK;
}

static io_binding_result_t io_binding_validate_output_address(uint16_t id,
                                                              device_channel_backend_t backend,
                                                              gpio_num_t gpio,
                                                              int backend_instance,
                                                              int endpoint_index)
{
    if (!io_binding_backend_selectable_now(backend, false))
        return io_binding_backend_is_mcp(backend)
            ? IO_BINDING_RESULT_UNSUPPORTED_BACKEND
            : IO_BINDING_RESULT_INVALID_BACKEND;

    if (!io_binding_backend_address_valid(backend, false, (int)gpio, backend_instance, endpoint_index))
        return io_binding_backend_is_gpio(backend)
            ? IO_BINDING_RESULT_INVALID_GPIO
            : IO_BINDING_RESULT_INVALID_ENDPOINT;

    if (io_binding_address_in_use_by_output(backend, backend_instance, endpoint_index, id) ||
        io_binding_address_in_use_by_input(backend, backend_instance, endpoint_index, 0U))
    {
        return io_binding_backend_is_gpio(backend)
            ? IO_BINDING_RESULT_GPIO_CONFLICT
            : IO_BINDING_RESULT_ADDRESS_CONFLICT;
    }

    return IO_BINDING_RESULT_OK;
}

static bool io_binding_input_blob_has_duplicate(const io_binding_input_entry_t *entries, uint16_t count, uint16_t index)
{
    for (uint16_t i = 0; i < count; i++)
    {
        if (i != index && entries[i].id == entries[index].id)
            return true;

        if (i != index &&
            entries[i].backend == entries[index].backend &&
            entries[i].backend_instance == entries[index].backend_instance &&
            entries[i].endpoint_index == entries[index].endpoint_index)
        {
            return true;
        }
    }

    return false;
}

static bool io_binding_output_blob_has_duplicate(const io_binding_output_entry_t *entries, uint16_t count, uint16_t index)
{
    for (uint16_t i = 0; i < count; i++)
    {
        if (i != index && entries[i].id == entries[index].id)
            return true;

        if (i != index &&
            entries[i].backend == entries[index].backend &&
            entries[i].backend_instance == entries[index].backend_instance &&
            entries[i].endpoint_index == entries[index].endpoint_index)
        {
            return true;
        }
    }

    return false;
}

static void io_binding_prepare_input_entry(io_binding_input_entry_t *entry,
                                           uint16_t id,
                                           const char *name,
                                           const char *role,
                                           device_channel_backend_t backend,
                                           gpio_num_t gpio,
                                           int backend_instance,
                                           int endpoint_index,
                                           const device_input_profile_t *profile)
{
    if (!entry)
        return;

    memset(entry, 0, sizeof(*entry));
    entry->id = id;
    entry->gpio = (int16_t)gpio;
    entry->backend = (uint8_t)backend;
    entry->channel_class = (uint8_t)DEVICE_CHANNEL_CLASS_DIGITAL_INPUT;
    entry->backend_instance = (int16_t)backend_instance;
    entry->endpoint_index = (int16_t)endpoint_index;
    io_binding_copy_text(entry->name, sizeof(entry->name), (name && name[0]) ? name : (profile ? profile->name : "Entrada"));
    io_binding_copy_text(entry->role, sizeof(entry->role), (role && role[0]) ? role : io_binding_default_input_role_for(profile));
}

static void io_binding_prepare_output_entry(io_binding_output_entry_t *entry,
                                            uint16_t id,
                                            const char *name,
                                            const char *role,
                                            device_channel_backend_t backend,
                                            gpio_num_t gpio,
                                            int backend_instance,
                                            int endpoint_index,
                                            const device_output_profile_t *profile)
{
    if (!entry)
        return;

    memset(entry, 0, sizeof(*entry));
    entry->id = id;
    entry->gpio = (int16_t)gpio;
    entry->backend = (uint8_t)backend;
    entry->channel_class = (uint8_t)DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT;
    entry->backend_instance = (int16_t)backend_instance;
    entry->endpoint_index = (int16_t)endpoint_index;
    io_binding_copy_text(entry->name, sizeof(entry->name), (name && name[0]) ? name : (profile ? profile->name : "Saída"));
    io_binding_copy_text(entry->role, sizeof(entry->role), (role && role[0]) ? role : io_binding_default_output_role_for(profile));
}

static void io_binding_resolve_cross_conflicts_after_load(void)
{
    bool changed = false;

    for (int i = 0; i < output_entry_count; i++)
    {
        const device_output_profile_t *profile = device_profile_find_output(output_entries[i].id);

        if (!profile || output_entries[i].backend != DEVICE_CHANNEL_BACKEND_GPIO)
            continue;

        if (io_binding_gpio_in_use_by_input((gpio_num_t)output_entries[i].gpio, 0U))
        {
            output_entries[i].gpio = (int16_t)profile->gpio;
            output_entries[i].endpoint_index = (int16_t)profile->gpio;
            changed = true;
        }
    }

    for (int i = 0; i < input_entry_count; i++)
    {
        const device_input_profile_t *profile = device_profile_find_input(input_entries[i].id);

        if (!profile || input_entries[i].backend != DEVICE_CHANNEL_BACKEND_GPIO)
            continue;

        if (io_binding_gpio_in_use_by_output((gpio_num_t)input_entries[i].gpio, 0U))
        {
            input_entries[i].gpio = (int16_t)profile->gpio;
            input_entries[i].endpoint_index = (int16_t)profile->gpio;
            changed = true;
        }
    }

    if (!changed)
        return;

    ESP_LOGW(TAG, "Conflitos cruzados de GPIO detectados no restore; revertendo canais conflitantes para o padrao");
    io_binding_persist_inputs();
    io_binding_persist_outputs();
}

static void io_binding_describe_input(const device_input_profile_t *profile,
                                      gpio_num_t gpio,
                                      device_channel_backend_t backend,
                                      char *dst,
                                      size_t dst_size)
{
    if (!dst || dst_size == 0U)
        return;

    if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17)
    {
        snprintf(dst, dst_size, "MCP23x17 digital expandido • entrada lógica remota/local de expansor");
        return;
    }

    if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17)
    {
        snprintf(dst, dst_size, "MCP23x17 digital expandido • saída lógica via expansor");
        return;
    }

    if (backend != DEVICE_CHANNEL_BACKEND_GPIO)
    {
        snprintf(dst, dst_size, "Backend %s previsto para expansão futura", io_binding_backend_label(backend));
        return;
    }

    if (profile && gpio == profile->gpio && profile->description && profile->description[0])
    {
        io_binding_copy_text(dst, dst_size, profile->description);
        return;
    }

    if (device_profile_gpio_is_input_only(gpio))
    {
        snprintf(dst,
                 dst_size,
                 "GPIO %d • entrada apenas • debounce %u • resistor externo recomendado",
                 (int)gpio,
                 profile ? profile->debounce_samples : 0U);
        return;
    }

    snprintf(dst,
             dst_size,
             "GPIO %d • %s • debounce %u",
             (int)gpio,
             (profile && profile->active_low) ? "active-low" : "active-high",
             profile ? profile->debounce_samples : 0U);
}

static void io_binding_describe_output(const device_output_profile_t *profile,
                                       gpio_num_t gpio,
                                       device_channel_backend_t backend,
                                       char *dst,
                                       size_t dst_size)
{
    if (!dst || dst_size == 0U)
        return;

    if (backend != DEVICE_CHANNEL_BACKEND_GPIO)
    {
        snprintf(dst, dst_size, "Backend %s previsto para expansão futura", io_binding_backend_label(backend));
        return;
    }

    if (profile && gpio == profile->gpio && profile->description && profile->description[0])
    {
        io_binding_copy_text(dst, dst_size, profile->description);
        return;
    }

    snprintf(dst,
             dst_size,
             "GPIO %d • saida local • %s",
             (int)gpio,
             (profile && profile->active_low) ? "active-low" : "active-high");
}

static void io_binding_apply_input_defaults(void)
{
    int count = device_profile_default_input_count();

    input_entry_count = 0;

    for (int i = 0; i < count && i < IO_BINDING_MAX_INPUTS; i++)
    {
        const device_input_profile_t *profile = device_profile_input_at(i);

        if (!profile)
            continue;

        io_binding_prepare_input_entry(&input_entries[input_entry_count],
                                       profile->id,
                                       profile->name,
                                       io_binding_default_input_role_for(profile),
                                       DEVICE_CHANNEL_BACKEND_GPIO,
                                       profile->gpio,
                                       0,
                                       (int)profile->gpio,
                                       profile);
        input_entry_count++;
    }
}

static void io_binding_apply_output_defaults(void)
{
    int count = device_profile_default_output_count();

    output_entry_count = 0;

    for (int i = 0; i < count && i < IO_BINDING_MAX_OUTPUTS; i++)
    {
        const device_output_profile_t *profile = device_profile_output_at(i);

        if (!profile)
            continue;

        io_binding_prepare_output_entry(&output_entries[output_entry_count],
                                        profile->id,
                                        profile->name,
                                        io_binding_default_output_role_for(profile),
                                        DEVICE_CHANNEL_BACKEND_GPIO,
                                        profile->gpio,
                                        0,
                                        (int)profile->gpio,
                                        profile);
        output_entry_count++;
    }
}

static void io_binding_persist_inputs(void)
{
    io_binding_input_blob_t blob = {0};
    nvs_handle_t nvs;

    blob.magic = IO_BINDING_MAGIC_INPUTS;
    blob.version = IO_BINDING_VERSION;
    blob.count = (uint16_t)input_entry_count;

    if (input_entry_count > 0)
        memcpy(blob.inputs, input_entries, sizeof(blob.inputs[0]) * (size_t)input_entry_count);

    blob.crc = io_binding_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

    if (nvs_open(IO_BINDING_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao abrir NVS para persistir bindings de input");
        return;
    }

    if (nvs_set_blob(nvs, IO_BINDING_KEY_INPUTS, &blob, sizeof(blob)) == ESP_OK &&
        nvs_commit(nvs) == ESP_OK)
    {
        ESP_LOGI(TAG, "Bindings de input persistidos (%d canal(is))", input_entry_count);
    }
    else
    {
        ESP_LOGE(TAG, "Falha ao gravar bindings de input");
    }

    nvs_close(nvs);
}

static void io_binding_persist_outputs(void)
{
    io_binding_output_blob_t blob = {0};
    nvs_handle_t nvs;

    blob.magic = IO_BINDING_MAGIC_OUTPUTS;
    blob.version = IO_BINDING_VERSION;
    blob.count = (uint16_t)output_entry_count;

    if (output_entry_count > 0)
        memcpy(blob.outputs, output_entries, sizeof(blob.outputs[0]) * (size_t)output_entry_count);

    blob.crc = io_binding_crc32((const uint8_t *)&blob, sizeof(blob) - sizeof(blob.crc));

    if (nvs_open(IO_BINDING_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao abrir NVS para persistir bindings de output");
        return;
    }

    if (nvs_set_blob(nvs, IO_BINDING_KEY_OUTPUTS, &blob, sizeof(blob)) == ESP_OK &&
        nvs_commit(nvs) == ESP_OK)
    {
        ESP_LOGI(TAG, "Bindings de output persistidos (%d canal(is))", output_entry_count);
    }
    else
    {
        ESP_LOGE(TAG, "Falha ao gravar bindings de output");
    }

    nvs_close(nvs);
}

static bool io_binding_load_inputs_v3(const io_binding_input_blob_t *blob)
{
    if (!blob ||
        blob->magic != IO_BINDING_MAGIC_INPUTS ||
        (blob->version != IO_BINDING_VERSION &&
         blob->version != IO_BINDING_VERSION_V3) ||
        blob->count > IO_BINDING_MAX_INPUTS)
    {
        return false;
    }

    for (uint16_t i = 0; i < blob->count; i++)
    {
        if (!device_profile_is_valid_input(blob->inputs[i].id) ||
            !io_binding_backend_address_valid((device_channel_backend_t)blob->inputs[i].backend, true, blob->inputs[i].gpio, blob->inputs[i].backend_instance, blob->inputs[i].endpoint_index) ||
            io_binding_input_blob_has_duplicate(blob->inputs, blob->count, i))
        {
            ESP_LOGW(TAG, "Binding de input v3 invalido para id %u", blob->inputs[i].id);
            return false;
        }
    }

    memcpy(input_entries, blob->inputs, sizeof(blob->inputs[0]) * blob->count);
    input_entry_count = (int)blob->count;
    ESP_LOGI(TAG, "Bindings de input restaurados (%d canal(is))", input_entry_count);
    return true;
}

static bool io_binding_load_inputs_v2(const io_binding_input_blob_v2_t *blob)
{
    input_entry_count = 0;

    if (!blob ||
        blob->magic != IO_BINDING_MAGIC_INPUTS ||
        blob->version != IO_BINDING_VERSION_V2 ||
        blob->count > IO_BINDING_MAX_INPUTS)
    {
        return false;
    }

    for (uint16_t i = 0; i < blob->count && input_entry_count < IO_BINDING_MAX_INPUTS; i++)
    {
        const device_input_profile_t *profile = device_profile_find_input(blob->inputs[i].id);

        if (!profile ||
            !device_profile_input_gpio_allowed((gpio_num_t)blob->inputs[i].gpio))
        {
            ESP_LOGW(TAG, "Binding de input v2 invalido para id %u", blob->inputs[i].id);
            return false;
        }

        io_binding_prepare_input_entry(&input_entries[input_entry_count],
                                       blob->inputs[i].id,
                                       blob->inputs[i].name,
                                       blob->inputs[i].role,
                                       DEVICE_CHANNEL_BACKEND_GPIO,
                                       (gpio_num_t)blob->inputs[i].gpio,
                                       0,
                                       (int)blob->inputs[i].gpio,
                                       profile);
        input_entry_count++;
    }

    ESP_LOGI(TAG, "Bindings de input v2 migrados para backend-aware (%d canal(is))", input_entry_count);
    io_binding_persist_inputs();
    return true;
}

static bool io_binding_load_inputs_v1(const io_binding_input_blob_v1_t *blob)
{
    input_entry_count = 0;

    if (!blob ||
        blob->magic != IO_BINDING_MAGIC_INPUTS ||
        blob->version != IO_BINDING_VERSION_V1 ||
        blob->count > IO_BINDING_MAX_INPUTS)
    {
        return false;
    }

    for (uint16_t i = 0; i < blob->count && input_entry_count < IO_BINDING_MAX_INPUTS; i++)
    {
        const device_input_profile_t *profile = device_profile_find_input(blob->inputs[i].id);

        if (!profile)
        {
            ESP_LOGW(TAG, "Binding de input v1 invalido para id %u", blob->inputs[i].id);
            return false;
        }

        io_binding_prepare_input_entry(&input_entries[input_entry_count],
                                       blob->inputs[i].id,
                                       blob->inputs[i].name,
                                       blob->inputs[i].role,
                                       DEVICE_CHANNEL_BACKEND_GPIO,
                                       profile->gpio,
                                       0,
                                       (int)profile->gpio,
                                       profile);
        input_entry_count++;
    }

    ESP_LOGI(TAG, "Bindings de input v1 migrados (%d canal(is))", input_entry_count);
    io_binding_persist_inputs();
    return true;
}

static bool io_binding_load_outputs_v3(const io_binding_output_blob_t *blob)
{
    if (!blob ||
        blob->magic != IO_BINDING_MAGIC_OUTPUTS ||
        (blob->version != IO_BINDING_VERSION &&
         blob->version != IO_BINDING_VERSION_V3) ||
        blob->count > IO_BINDING_MAX_OUTPUTS)
    {
        return false;
    }

    for (uint16_t i = 0; i < blob->count; i++)
    {
        if (!device_profile_is_valid_output(blob->outputs[i].id) ||
            !io_binding_backend_address_valid((device_channel_backend_t)blob->outputs[i].backend, false, blob->outputs[i].gpio, blob->outputs[i].backend_instance, blob->outputs[i].endpoint_index) ||
            io_binding_output_blob_has_duplicate(blob->outputs, blob->count, i))
        {
            ESP_LOGW(TAG, "Binding de output v3 invalido para id %u", blob->outputs[i].id);
            return false;
        }
    }

    memcpy(output_entries, blob->outputs, sizeof(blob->outputs[0]) * blob->count);
    output_entry_count = (int)blob->count;
    ESP_LOGI(TAG, "Bindings de output restaurados (%d canal(is))", output_entry_count);
    return true;
}

static bool io_binding_load_outputs_v2(const io_binding_output_blob_v2_t *blob)
{
    output_entry_count = 0;

    if (!blob ||
        blob->magic != IO_BINDING_MAGIC_OUTPUTS ||
        blob->version != IO_BINDING_VERSION_V2 ||
        blob->count > IO_BINDING_MAX_OUTPUTS)
    {
        return false;
    }

    for (uint16_t i = 0; i < blob->count && output_entry_count < IO_BINDING_MAX_OUTPUTS; i++)
    {
        const device_output_profile_t *profile = device_profile_find_output(blob->outputs[i].id);

        if (!profile ||
            !device_profile_output_gpio_allowed((gpio_num_t)blob->outputs[i].gpio))
        {
            ESP_LOGW(TAG, "Binding de output v2 invalido para id %u", blob->outputs[i].id);
            return false;
        }

        io_binding_prepare_output_entry(&output_entries[output_entry_count],
                                        blob->outputs[i].id,
                                        blob->outputs[i].name,
                                        blob->outputs[i].role,
                                        DEVICE_CHANNEL_BACKEND_GPIO,
                                        (gpio_num_t)blob->outputs[i].gpio,
                                        0,
                                        (int)blob->outputs[i].gpio,
                                        profile);
        output_entry_count++;
    }

    ESP_LOGI(TAG, "Bindings de output v2 migrados para backend-aware (%d canal(is))", output_entry_count);
    io_binding_persist_outputs();
    return true;
}

static bool io_binding_load_outputs_v1(const io_binding_output_blob_v1_t *blob)
{
    output_entry_count = 0;

    if (!blob ||
        blob->magic != IO_BINDING_MAGIC_OUTPUTS ||
        blob->version != IO_BINDING_VERSION_V1 ||
        blob->count > IO_BINDING_MAX_OUTPUTS)
    {
        return false;
    }

    for (uint16_t i = 0; i < blob->count && output_entry_count < IO_BINDING_MAX_OUTPUTS; i++)
    {
        const device_output_profile_t *profile = device_profile_find_output(blob->outputs[i].id);

        if (!profile)
        {
            ESP_LOGW(TAG, "Binding de output v1 invalido para id %u", blob->outputs[i].id);
            return false;
        }

        io_binding_prepare_output_entry(&output_entries[output_entry_count],
                                        blob->outputs[i].id,
                                        blob->outputs[i].name,
                                        blob->outputs[i].role,
                                        DEVICE_CHANNEL_BACKEND_GPIO,
                                        profile->gpio,
                                        0,
                                        (int)profile->gpio,
                                        profile);
        output_entry_count++;
    }

    ESP_LOGI(TAG, "Bindings de output v1 migrados (%d canal(is))", output_entry_count);
    io_binding_persist_outputs();
    return true;
}

static void io_binding_load_inputs(void)
{
    uint8_t raw[sizeof(io_binding_input_blob_t)] = {0};
    nvs_handle_t nvs;
    size_t len = sizeof(raw);
    uint32_t crc;

    if (nvs_open(IO_BINDING_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_blob(nvs, IO_BINDING_KEY_INPUTS, raw, &len) != ESP_OK)
    {
        nvs_close(nvs);
        return;
    }

    nvs_close(nvs);

    if (len == sizeof(io_binding_input_blob_t))
    {
        io_binding_input_blob_t *blob = (io_binding_input_blob_t *)raw;
        crc = io_binding_crc32((const uint8_t *)blob, sizeof(*blob) - sizeof(blob->crc));
        if (blob->crc == crc && io_binding_load_inputs_v3(blob))
            return;
    }

    if (len == sizeof(io_binding_input_blob_v2_t))
    {
        io_binding_input_blob_v2_t *blob = (io_binding_input_blob_v2_t *)raw;
        crc = io_binding_crc32((const uint8_t *)blob, sizeof(*blob) - sizeof(blob->crc));
        if (blob->crc == crc && io_binding_load_inputs_v2(blob))
            return;
    }

    if (len == sizeof(io_binding_input_blob_v1_t))
    {
        io_binding_input_blob_v1_t *blob = (io_binding_input_blob_v1_t *)raw;
        crc = io_binding_crc32((const uint8_t *)blob, sizeof(*blob) - sizeof(blob->crc));
        if (blob->crc == crc && io_binding_load_inputs_v1(blob))
            return;
    }

    ESP_LOGW(TAG, "Blob de input binding invalido");
}

static void io_binding_load_outputs(void)
{
    uint8_t raw[sizeof(io_binding_output_blob_t)] = {0};
    nvs_handle_t nvs;
    size_t len = sizeof(raw);
    uint32_t crc;

    if (nvs_open(IO_BINDING_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;

    if (nvs_get_blob(nvs, IO_BINDING_KEY_OUTPUTS, raw, &len) != ESP_OK)
    {
        nvs_close(nvs);
        return;
    }

    nvs_close(nvs);

    if (len == sizeof(io_binding_output_blob_t))
    {
        io_binding_output_blob_t *blob = (io_binding_output_blob_t *)raw;
        crc = io_binding_crc32((const uint8_t *)blob, sizeof(*blob) - sizeof(blob->crc));
        if (blob->crc == crc && io_binding_load_outputs_v3(blob))
            return;
    }

    if (len == sizeof(io_binding_output_blob_v2_t))
    {
        io_binding_output_blob_v2_t *blob = (io_binding_output_blob_v2_t *)raw;
        crc = io_binding_crc32((const uint8_t *)blob, sizeof(*blob) - sizeof(blob->crc));
        if (blob->crc == crc && io_binding_load_outputs_v2(blob))
            return;
    }

    if (len == sizeof(io_binding_output_blob_v1_t))
    {
        io_binding_output_blob_v1_t *blob = (io_binding_output_blob_v1_t *)raw;
        crc = io_binding_crc32((const uint8_t *)blob, sizeof(*blob) - sizeof(blob->crc));
        if (blob->crc == crc && io_binding_load_outputs_v1(blob))
            return;
    }

    ESP_LOGW(TAG, "Blob de output binding invalido");
}

bool io_binding_has_input(uint16_t id)
{
    return io_binding_find_input_entry(id) != NULL;
}

int io_binding_available_inputs(uint16_t *out_ids, int max_ids)
{
    int total = 0;
    int written = 0;

    for (int i = 0; i < device_profile_input_count(); i++)
    {
        const device_input_profile_t *profile = device_profile_input_at(i);
        if (!profile || io_binding_has_input(profile->id))
            continue;
        if (out_ids && written < max_ids)
            out_ids[written++] = profile->id;
        total++;
    }

    return total;
}

io_binding_result_t io_binding_add_input(uint16_t id, const char *name, const char *role, int gpio)
{
    return io_binding_add_input_ex(id,
                                   name,
                                   role,
                                   DEVICE_CHANNEL_BACKEND_GPIO,
                                   gpio,
                                   0,
                                   gpio);
}

io_binding_result_t io_binding_add_input_ex(uint16_t id,
                                            const char *name,
                                            const char *role,
                                            device_channel_backend_t backend,
                                            int gpio,
                                            int backend_instance,
                                            int endpoint_index)
{
    const device_input_profile_t *profile = device_profile_find_input(id);
    gpio_num_t resolved_gpio = GPIO_NUM_NC;
    io_binding_result_t addr_result;

    if (!profile)
        return IO_BINDING_RESULT_NOT_FOUND;
    if (io_binding_has_input(id))
        return IO_BINDING_RESULT_ALREADY_ACTIVE;
    if (input_entry_count >= IO_BINDING_MAX_INPUTS)
        return IO_BINDING_RESULT_LIMIT_REACHED;

    if (backend == DEVICE_CHANNEL_BACKEND_GPIO)
    {
        if (gpio < 0)
            return IO_BINDING_RESULT_INVALID_GPIO;
        resolved_gpio = (gpio_num_t)gpio;
        backend_instance = 0;
        endpoint_index = gpio;
    }
    else if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17)
    {
        resolved_gpio = GPIO_NUM_NC;
        gpio = -1;
    }
    else
    {
        return IO_BINDING_RESULT_INVALID_BACKEND;
    }

    addr_result = io_binding_validate_input_address(id, backend, resolved_gpio, backend_instance, endpoint_index);
    if (addr_result != IO_BINDING_RESULT_OK)
        return addr_result;

    io_binding_prepare_input_entry(&input_entries[input_entry_count],
                                   id,
                                   name,
                                   role,
                                   backend,
                                   resolved_gpio,
                                   backend_instance,
                                   endpoint_index,
                                   profile);
    input_entry_count++;
    gpio_restart_required = true;
    io_binding_persist_inputs();
    return IO_BINDING_RESULT_OK;
}

io_binding_result_t io_binding_remove_input(uint16_t id)
{
    if (io_binding_input_is_protected(id))
        return IO_BINDING_RESULT_PROTECTED;

    for (int i = 0; i < input_entry_count; i++)
    {
        if (input_entries[i].id == id)
        {
            if (i < input_entry_count - 1)
                memmove(&input_entries[i], &input_entries[i + 1], sizeof(input_entries[0]) * (size_t)(input_entry_count - i - 1));
            input_entry_count--;
            gpio_restart_required = true;
            io_binding_persist_inputs();
            return IO_BINDING_RESULT_OK;
        }
    }

    return IO_BINDING_RESULT_NOT_FOUND;
}

bool io_binding_has_output(uint16_t id)
{
    return io_binding_find_output_entry(id) != NULL;
}

int io_binding_available_outputs(uint16_t *out_ids, int max_ids)
{
    int total = 0;
    int written = 0;

    for (int i = 0; i < device_profile_output_count(); i++)
    {
        const device_output_profile_t *profile = device_profile_output_at(i);
        if (!profile || io_binding_has_output(profile->id))
            continue;
        if (out_ids && written < max_ids)
            out_ids[written++] = profile->id;
        total++;
    }

    return total;
}

io_binding_result_t io_binding_add_output(uint16_t id, const char *name, const char *role, int gpio)
{
    return io_binding_add_output_ex(id,
                                    name,
                                    role,
                                    DEVICE_CHANNEL_BACKEND_GPIO,
                                    gpio,
                                    0,
                                    gpio);
}

io_binding_result_t io_binding_add_output_ex(uint16_t id,
                                             const char *name,
                                             const char *role,
                                             device_channel_backend_t backend,
                                             int gpio,
                                             int backend_instance,
                                             int endpoint_index)
{
    const device_output_profile_t *profile = device_profile_find_output(id);
    gpio_num_t resolved_gpio = GPIO_NUM_NC;
    io_binding_result_t addr_result;

    if (!profile)
        return IO_BINDING_RESULT_NOT_FOUND;
    if (io_binding_has_output(id))
        return IO_BINDING_RESULT_ALREADY_ACTIVE;
    if (output_entry_count >= IO_BINDING_MAX_OUTPUTS)
        return IO_BINDING_RESULT_LIMIT_REACHED;

    if (backend == DEVICE_CHANNEL_BACKEND_GPIO)
    {
        if (gpio < 0)
            return IO_BINDING_RESULT_INVALID_GPIO;
        resolved_gpio = (gpio_num_t)gpio;
        backend_instance = 0;
        endpoint_index = gpio;
    }
    else if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17)
    {
        resolved_gpio = GPIO_NUM_NC;
        gpio = -1;
    }
    else
    {
        return IO_BINDING_RESULT_INVALID_BACKEND;
    }

    addr_result = io_binding_validate_output_address(id, backend, resolved_gpio, backend_instance, endpoint_index);
    if (addr_result != IO_BINDING_RESULT_OK)
        return addr_result;

    io_binding_prepare_output_entry(&output_entries[output_entry_count],
                                    id,
                                    name,
                                    role,
                                    backend,
                                    resolved_gpio,
                                    backend_instance,
                                    endpoint_index,
                                    profile);
    output_entry_count++;
    gpio_restart_required = true;
    io_binding_persist_outputs();
    return IO_BINDING_RESULT_OK;
}

io_binding_result_t io_binding_remove_output(uint16_t id)
{
    if (io_binding_output_is_protected(id))
        return IO_BINDING_RESULT_PROTECTED;

    for (int i = 0; i < output_entry_count; i++)
    {
        if (output_entries[i].id == id)
        {
            if (i < output_entry_count - 1)
                memmove(&output_entries[i], &output_entries[i + 1], sizeof(output_entries[0]) * (size_t)(output_entry_count - i - 1));
            output_entry_count--;
            gpio_restart_required = true;
            io_binding_persist_outputs();
            return IO_BINDING_RESULT_OK;
        }
    }

    return IO_BINDING_RESULT_NOT_FOUND;
}

void io_binding_init(void)
{
    gpio_restart_required = false;
    io_binding_apply_input_defaults();
    io_binding_apply_output_defaults();
    io_binding_load_inputs();
    io_binding_load_outputs();
    io_binding_resolve_cross_conflicts_after_load();
}

bool io_binding_get_input(uint16_t id, io_binding_input_view_t *out)
{
    io_binding_input_entry_t *entry = io_binding_find_input_entry(id);
    const device_input_profile_t *profile = device_profile_find_input(id);

    if (!entry || !profile || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->id = id;
    out->gpio = entry->gpio;
    out->default_gpio = profile->gpio;
    out->backend_instance = entry->backend_instance;
    out->endpoint_index = entry->endpoint_index;
    out->backend = (device_channel_backend_t)entry->backend;
    out->channel_class = (device_channel_class_t)entry->channel_class;
    out->configurable_now = ((device_channel_backend_t)entry->backend == DEVICE_CHANNEL_BACKEND_GPIO ||
                             (device_channel_backend_t)entry->backend == DEVICE_CHANNEL_BACKEND_MCP23X17);
    out->expansion_path = ((device_channel_backend_t)entry->backend != DEVICE_CHANNEL_BACKEND_GPIO);
    io_binding_copy_text(out->backend_name, sizeof(out->backend_name), io_binding_backend_label((device_channel_backend_t)entry->backend));
    io_binding_channel_address_text((device_channel_backend_t)entry->backend, entry->backend_instance, entry->endpoint_index, (gpio_num_t)entry->gpio, out->address, sizeof(out->address));
    io_binding_copy_text(out->name, sizeof(out->name), entry->name);
    io_binding_copy_text(out->role, sizeof(out->role), entry->role);
    io_binding_describe_input(profile, (gpio_num_t)entry->gpio, (device_channel_backend_t)entry->backend, out->description, sizeof(out->description));
    return true;
}

int io_binding_export_inputs(io_binding_input_view_t *out, int max_inputs)
{
    int count = (input_entry_count < max_inputs) ? input_entry_count : max_inputs;

    if (!out || max_inputs <= 0)
        return input_entry_count;

    for (int i = 0; i < count; i++)
        io_binding_get_input(input_entries[i].id, &out[i]);

    return count;
}

io_binding_result_t io_binding_set_input(uint16_t id, const char *name, const char *role, int gpio)
{
    return io_binding_set_input_ex(id,
                                   name,
                                   role,
                                   DEVICE_CHANNEL_BACKEND_GPIO,
                                   gpio,
                                   0,
                                   gpio);
}

io_binding_result_t io_binding_set_input_ex(uint16_t id,
                                            const char *name,
                                            const char *role,
                                            device_channel_backend_t backend,
                                            int gpio,
                                            int backend_instance,
                                            int endpoint_index)
{
    io_binding_input_entry_t *entry = io_binding_find_input_entry(id);
    const device_input_profile_t *profile = device_profile_find_input(id);
    gpio_num_t resolved_gpio = GPIO_NUM_NC;
    io_binding_result_t addr_result;

    if (!entry || !profile)
        return IO_BINDING_RESULT_NOT_FOUND;

    if (backend == DEVICE_CHANNEL_BACKEND_GPIO)
    {
        resolved_gpio = (gpio >= 0) ? (gpio_num_t)gpio : (gpio_num_t)entry->gpio;
        backend_instance = 0;
        endpoint_index = (int)resolved_gpio;
    }
    else if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17)
    {
        resolved_gpio = GPIO_NUM_NC;
    }
    else
    {
        return IO_BINDING_RESULT_INVALID_BACKEND;
    }

    if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && backend_instance < 0)
        backend_instance = entry->backend_instance;
    if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && endpoint_index < 0)
        endpoint_index = entry->endpoint_index;

    addr_result = io_binding_validate_input_address(id, backend, resolved_gpio, backend_instance, endpoint_index);
    if (addr_result != IO_BINDING_RESULT_OK)
        return addr_result;

    if (entry->gpio != (int16_t)resolved_gpio ||
        entry->backend != (uint8_t)backend ||
        entry->backend_instance != (int16_t)backend_instance ||
        entry->endpoint_index != (int16_t)endpoint_index)
    {
        gpio_restart_required = true;
    }

    entry->gpio = (int16_t)resolved_gpio;
    entry->backend = (uint8_t)backend;
    entry->channel_class = (uint8_t)DEVICE_CHANNEL_CLASS_DIGITAL_INPUT;
    entry->backend_instance = (int16_t)backend_instance;
    entry->endpoint_index = (int16_t)endpoint_index;
    io_binding_copy_text(entry->name, sizeof(entry->name), (name && name[0]) ? name : profile->name);
    io_binding_copy_text(entry->role, sizeof(entry->role), (role && role[0]) ? role : io_binding_default_input_role_for(profile));
    io_binding_persist_inputs();
    return IO_BINDING_RESULT_OK;
}

io_binding_result_t io_binding_reset_input(uint16_t id)
{
    const device_input_profile_t *profile = device_profile_find_input(id);

    if (!profile)
        return IO_BINDING_RESULT_NOT_FOUND;

    return io_binding_set_input(id, profile->name, io_binding_default_input_role_for(profile), profile->gpio);
}

bool io_binding_get_output(uint16_t id, io_binding_output_view_t *out)
{
    io_binding_output_entry_t *entry = io_binding_find_output_entry(id);
    const device_output_profile_t *profile = device_profile_find_output(id);

    if (!entry || !profile || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->id = id;
    out->gpio = entry->gpio;
    out->default_gpio = profile->gpio;
    out->backend_instance = entry->backend_instance;
    out->endpoint_index = entry->endpoint_index;
    out->backend = (device_channel_backend_t)entry->backend;
    out->channel_class = (device_channel_class_t)entry->channel_class;
    out->configurable_now = ((device_channel_backend_t)entry->backend == DEVICE_CHANNEL_BACKEND_GPIO ||
                             (device_channel_backend_t)entry->backend == DEVICE_CHANNEL_BACKEND_MCP23X17);
    out->expansion_path = ((device_channel_backend_t)entry->backend != DEVICE_CHANNEL_BACKEND_GPIO);
    io_binding_copy_text(out->backend_name, sizeof(out->backend_name), io_binding_backend_label((device_channel_backend_t)entry->backend));
    io_binding_channel_address_text((device_channel_backend_t)entry->backend, entry->backend_instance, entry->endpoint_index, (gpio_num_t)entry->gpio, out->address, sizeof(out->address));
    io_binding_copy_text(out->name, sizeof(out->name), entry->name);
    io_binding_copy_text(out->role, sizeof(out->role), entry->role);
    io_binding_describe_output(profile, (gpio_num_t)entry->gpio, (device_channel_backend_t)entry->backend, out->description, sizeof(out->description));
    return true;
}

int io_binding_export_outputs(io_binding_output_view_t *out, int max_outputs)
{
    int count = (output_entry_count < max_outputs) ? output_entry_count : max_outputs;

    if (!out || max_outputs <= 0)
        return output_entry_count;

    for (int i = 0; i < count; i++)
        io_binding_get_output(output_entries[i].id, &out[i]);

    return count;
}

io_binding_result_t io_binding_set_output(uint16_t id, const char *name, const char *role, int gpio)
{
    return io_binding_set_output_ex(id,
                                    name,
                                    role,
                                    DEVICE_CHANNEL_BACKEND_GPIO,
                                    gpio,
                                    0,
                                    gpio);
}

io_binding_result_t io_binding_set_output_ex(uint16_t id,
                                             const char *name,
                                             const char *role,
                                             device_channel_backend_t backend,
                                             int gpio,
                                             int backend_instance,
                                             int endpoint_index)
{
    io_binding_output_entry_t *entry = io_binding_find_output_entry(id);
    const device_output_profile_t *profile = device_profile_find_output(id);
    gpio_num_t resolved_gpio = GPIO_NUM_NC;
    io_binding_result_t addr_result;

    if (!entry || !profile)
        return IO_BINDING_RESULT_NOT_FOUND;

    if (backend == DEVICE_CHANNEL_BACKEND_GPIO)
    {
        resolved_gpio = (gpio >= 0) ? (gpio_num_t)gpio : (gpio_num_t)entry->gpio;
        backend_instance = 0;
        endpoint_index = (int)resolved_gpio;
    }
    else if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17)
    {
        resolved_gpio = GPIO_NUM_NC;
    }
    else
    {
        return IO_BINDING_RESULT_INVALID_BACKEND;
    }

    if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && backend_instance < 0)
        backend_instance = entry->backend_instance;
    if (backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && endpoint_index < 0)
        endpoint_index = entry->endpoint_index;

    addr_result = io_binding_validate_output_address(id, backend, resolved_gpio, backend_instance, endpoint_index);
    if (addr_result != IO_BINDING_RESULT_OK)
        return addr_result;

    if (entry->gpio != (int16_t)resolved_gpio ||
        entry->backend != (uint8_t)backend ||
        entry->backend_instance != (int16_t)backend_instance ||
        entry->endpoint_index != (int16_t)endpoint_index)
    {
        gpio_restart_required = true;
    }

    entry->gpio = (int16_t)resolved_gpio;
    entry->backend = (uint8_t)backend;
    entry->channel_class = (uint8_t)DEVICE_CHANNEL_CLASS_DIGITAL_OUTPUT;
    entry->backend_instance = (int16_t)backend_instance;
    entry->endpoint_index = (int16_t)endpoint_index;
    io_binding_copy_text(entry->name, sizeof(entry->name), (name && name[0]) ? name : profile->name);
    io_binding_copy_text(entry->role, sizeof(entry->role), (role && role[0]) ? role : io_binding_default_output_role_for(profile));
    io_binding_persist_outputs();
    return IO_BINDING_RESULT_OK;
}

io_binding_result_t io_binding_reset_output(uint16_t id)
{
    const device_output_profile_t *profile = device_profile_find_output(id);

    if (!profile)
        return IO_BINDING_RESULT_NOT_FOUND;

    return io_binding_set_output(id, profile->name, io_binding_default_output_role_for(profile), profile->gpio);
}



int io_binding_mcp_instance_count(void)
{
    const device_expansion_capabilities_t *expansion = device_profile_expansion_capabilities();
    int count = expansion ? (int)expansion->recommended_mcp_instances : 0;

    if (!expansion || !expansion->supports_mcp23x17)
        return 0;

    if (count <= 0)
        count = 1;

    return count;
}

int io_binding_export_mcp_instances(io_binding_mcp_instance_view_t *out, int max_instances)
{
    int total = io_binding_mcp_instance_count();
    int exported = 0;

    if (!out || max_instances <= 0)
        return total;

    for (int instance = 0; instance < total && exported < max_instances; instance++)
    {
        io_binding_mcp_instance_view_t *view = &out[exported++];
        int active_inputs = 0;
        int active_outputs = 0;

        memset(view, 0, sizeof(*view));
        view->instance = instance;
        view->channel_capacity = 16;
        view->configurable_now = io_binding_backend_selectable_now(DEVICE_CHANNEL_BACKEND_MCP23X17, true);
        view->runtime_contract_ready = true;
        view->hardware_runtime_ready = false;
        snprintf(view->label, sizeof(view->label), "MCP%d", instance);
        io_binding_copy_text(view->notes, sizeof(view->notes), "Mapa de instância pronto para integração no runtime; aplicação elétrica do MCP segue como próxima etapa.");

        for (int i = 0; i < input_entry_count; i++)
        {
            if ((device_channel_backend_t)input_entries[i].backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && input_entries[i].backend_instance == instance)
                active_inputs++;
        }

        for (int i = 0; i < output_entry_count; i++)
        {
            if ((device_channel_backend_t)output_entries[i].backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && output_entries[i].backend_instance == instance)
                active_outputs++;
        }

        view->active_inputs = active_inputs;
        view->active_outputs = active_outputs;
        view->active_total = active_inputs + active_outputs;
    }

    return total;
}

int io_binding_export_mcp_endpoints(int instance, io_binding_mcp_endpoint_view_t *out, int max_endpoints)
{
    const device_expansion_capabilities_t *expansion = device_profile_expansion_capabilities();
    int total = expansion ? (int)expansion->channels_per_mcp : 16;
    int exported = 0;

    if (total <= 0)
        total = 16;

    if (!io_binding_mcp_address_valid(instance, 0))
        return 0;

    if (!out || max_endpoints <= 0)
        return total;

    for (int endpoint = 0; endpoint < total && exported < max_endpoints; endpoint++)
    {
        io_binding_mcp_endpoint_view_t *view = &out[exported++];
        io_binding_input_entry_t *input_entry = NULL;
        io_binding_output_entry_t *output_entry = NULL;

        memset(view, 0, sizeof(*view));
        view->instance = instance;
        view->endpoint_index = endpoint;
        view->configurable_now = io_binding_backend_selectable_now(DEVICE_CHANNEL_BACKEND_MCP23X17, true);
        view->runtime_contract_ready = true;
        view->hardware_runtime_ready = false;
        io_binding_copy_text(view->endpoint_label, sizeof(view->endpoint_label), io_binding_mcp_endpoint_label(endpoint));
        io_binding_channel_address_text(DEVICE_CHANNEL_BACKEND_MCP23X17, instance, endpoint, GPIO_NUM_NC, view->address, sizeof(view->address));

        for (int i = 0; i < input_entry_count; i++)
        {
            if ((device_channel_backend_t)input_entries[i].backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && input_entries[i].backend_instance == instance && input_entries[i].endpoint_index == endpoint)
            {
                input_entry = &input_entries[i];
                break;
            }
        }

        for (int i = 0; i < output_entry_count; i++)
        {
            if ((device_channel_backend_t)output_entries[i].backend == DEVICE_CHANNEL_BACKEND_MCP23X17 && output_entries[i].backend_instance == instance && output_entries[i].endpoint_index == endpoint)
            {
                output_entry = &output_entries[i];
                break;
            }
        }

        view->input_bound = (input_entry != NULL);
        view->output_bound = (output_entry != NULL);
        view->occupied = view->input_bound || view->output_bound;

        if (view->input_bound)
        {
            view->bound_id = input_entry->id;
            io_binding_copy_text(view->direction, sizeof(view->direction), "input");
            io_binding_copy_text(view->name, sizeof(view->name), input_entry->name);
            io_binding_copy_text(view->role, sizeof(view->role), input_entry->role);
        }
        else if (view->output_bound)
        {
            view->bound_id = output_entry->id;
            io_binding_copy_text(view->direction, sizeof(view->direction), "output");
            io_binding_copy_text(view->name, sizeof(view->name), output_entry->name);
            io_binding_copy_text(view->role, sizeof(view->role), output_entry->role);
        }
        else
        {
            io_binding_copy_text(view->direction, sizeof(view->direction), "free");
        }
    }

    return total;
}
bool io_binding_gpio_restart_required(void)
{
    return gpio_restart_required;
}
