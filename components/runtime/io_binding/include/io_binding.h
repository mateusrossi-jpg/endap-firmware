#pragma once

#include "device_profile.h"

#include <stdbool.h>
#include <stdint.h>

#define IO_BINDING_NAME_LEN 24
#define IO_BINDING_ROLE_LEN 24
#define IO_BINDING_DESC_LEN 128
#define IO_BINDING_ADDR_LEN 32
#define IO_BINDING_BACKEND_LABEL_LEN 24
#define IO_BINDING_MAX_INPUTS 16
#define IO_BINDING_MAX_OUTPUTS 16

typedef enum
{
    IO_BINDING_RESULT_OK = 0,
    IO_BINDING_RESULT_NOT_FOUND,
    IO_BINDING_RESULT_INVALID_GPIO,
    IO_BINDING_RESULT_GPIO_CONFLICT,
    IO_BINDING_RESULT_ALREADY_ACTIVE,
    IO_BINDING_RESULT_LIMIT_REACHED,
    IO_BINDING_RESULT_PROTECTED,
    IO_BINDING_RESULT_INVALID_BACKEND,
    IO_BINDING_RESULT_UNSUPPORTED_BACKEND,
    IO_BINDING_RESULT_INVALID_ENDPOINT,
    IO_BINDING_RESULT_ADDRESS_CONFLICT,
} io_binding_result_t;

typedef struct
{
    device_channel_backend_t backend;
    device_channel_class_t channel_class;
    bool implemented_now;
    bool selectable_now;
    bool expansion_path;
    char backend_code[IO_BINDING_BACKEND_LABEL_LEN];
    char label[IO_BINDING_BACKEND_LABEL_LEN];
    char notes[IO_BINDING_DESC_LEN];
} io_binding_backend_view_t;

typedef struct
{
    uint16_t id;
    int gpio;
    int default_gpio;
    int backend_instance;
    int endpoint_index;
    device_channel_backend_t backend;
    device_channel_class_t channel_class;
    bool configurable_now;
    bool expansion_path;
    char backend_name[IO_BINDING_BACKEND_LABEL_LEN];
    char address[IO_BINDING_ADDR_LEN];
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
    char description[IO_BINDING_DESC_LEN];
} io_binding_input_view_t;

typedef struct
{
    uint16_t id;
    int gpio;
    int default_gpio;
    int backend_instance;
    int endpoint_index;
    device_channel_backend_t backend;
    device_channel_class_t channel_class;
    bool configurable_now;
    bool expansion_path;
    char backend_name[IO_BINDING_BACKEND_LABEL_LEN];
    char address[IO_BINDING_ADDR_LEN];
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
    char description[IO_BINDING_DESC_LEN];
} io_binding_output_view_t;

typedef struct
{
    int instance;
    int channel_capacity;
    int active_inputs;
    int active_outputs;
    int active_total;
    bool configurable_now;
    bool runtime_contract_ready;
    bool hardware_runtime_ready;
    char label[IO_BINDING_BACKEND_LABEL_LEN];
    char notes[IO_BINDING_DESC_LEN];
} io_binding_mcp_instance_view_t;

typedef struct
{
    int instance;
    int endpoint_index;
    bool occupied;
    bool configurable_now;
    bool runtime_contract_ready;
    bool hardware_runtime_ready;
    bool input_bound;
    bool output_bound;
    uint16_t bound_id;
    char endpoint_label[8];
    char address[IO_BINDING_ADDR_LEN];
    char direction[16];
    char name[IO_BINDING_NAME_LEN];
    char role[IO_BINDING_ROLE_LEN];
} io_binding_mcp_endpoint_view_t;

void io_binding_init(void);

bool io_binding_get_input(uint16_t id, io_binding_input_view_t *out);
bool io_binding_has_input(uint16_t id);
int io_binding_available_inputs(uint16_t *out_ids, int max_ids);
io_binding_result_t io_binding_add_input(uint16_t id, const char *name, const char *role, int gpio);
io_binding_result_t io_binding_add_input_ex(uint16_t id,
                                            const char *name,
                                            const char *role,
                                            device_channel_backend_t backend,
                                            int gpio,
                                            int backend_instance,
                                            int endpoint_index);
io_binding_result_t io_binding_remove_input(uint16_t id);
int io_binding_export_inputs(io_binding_input_view_t *out, int max_inputs);
io_binding_result_t io_binding_set_input(uint16_t id, const char *name, const char *role, int gpio);
io_binding_result_t io_binding_set_input_ex(uint16_t id,
                                            const char *name,
                                            const char *role,
                                            device_channel_backend_t backend,
                                            int gpio,
                                            int backend_instance,
                                            int endpoint_index);
io_binding_result_t io_binding_reset_input(uint16_t id);

bool io_binding_get_output(uint16_t id, io_binding_output_view_t *out);
bool io_binding_has_output(uint16_t id);
int io_binding_available_outputs(uint16_t *out_ids, int max_ids);
io_binding_result_t io_binding_add_output(uint16_t id, const char *name, const char *role, int gpio);
io_binding_result_t io_binding_add_output_ex(uint16_t id,
                                             const char *name,
                                             const char *role,
                                             device_channel_backend_t backend,
                                             int gpio,
                                             int backend_instance,
                                             int endpoint_index);
io_binding_result_t io_binding_remove_output(uint16_t id);
int io_binding_export_outputs(io_binding_output_view_t *out, int max_outputs);
io_binding_result_t io_binding_set_output(uint16_t id, const char *name, const char *role, int gpio);
io_binding_result_t io_binding_set_output_ex(uint16_t id,
                                             const char *name,
                                             const char *role,
                                             device_channel_backend_t backend,
                                             int gpio,
                                             int backend_instance,
                                             int endpoint_index);
io_binding_result_t io_binding_reset_output(uint16_t id);

int io_binding_backend_count(void);
const io_binding_backend_view_t *io_binding_backend_at(int index);
const char *io_binding_backend_code(device_channel_backend_t backend);
const char *io_binding_backend_label(device_channel_backend_t backend);
bool io_binding_backend_selectable_now(device_channel_backend_t backend, bool for_input);
bool io_binding_backend_address_valid(device_channel_backend_t backend,
                                      bool for_input,
                                      int gpio,
                                      int backend_instance,
                                      int endpoint_index);

int io_binding_mcp_instance_count(void);
int io_binding_export_mcp_instances(io_binding_mcp_instance_view_t *out, int max_instances);
int io_binding_export_mcp_endpoints(int instance, io_binding_mcp_endpoint_view_t *out, int max_endpoints);

bool io_binding_gpio_restart_required(void);
