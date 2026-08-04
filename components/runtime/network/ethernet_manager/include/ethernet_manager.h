#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t static_ip_enabled;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
    char dns_sec[16];
} eth_ip_config_t;

void ethernet_manager_init(void);
bool ethernet_manager_is_ready(void);
void ethernet_manager_get_ip_config(eth_ip_config_t *cfg);
void ethernet_manager_set_ip_config(const eth_ip_config_t *cfg);

