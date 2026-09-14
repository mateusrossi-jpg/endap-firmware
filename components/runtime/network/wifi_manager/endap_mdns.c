#include "endap_mdns.h"
#include "device_profile.h"
#include "node_identity.h"
#include "mdns.h"
#include "esp_log.h"
#include <stdio.h>

#define ENDAP_MDNS_HOSTNAME_MAX 32

static const char *TAG = "endap_mdns";
static bool mdns_initialized = false;

esp_err_t endap_mdns_init(void)
{
    if (mdns_initialized)
        return ESP_OK;

    esp_err_t err = mdns_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao inicializar mDNS: %s", esp_err_to_name(err));
        return err;
    }

    node_identity_init();
    uint32_t my_id = node_identity_get();

    /*
     * Hostname mDNS derivado do papel do nó, sem hardcode de identidade:
     *  - Gateway (perfil GATEWAY) -> "endap" (descoberta padrão do Studio)
     *  - Demais nós             -> "endap-field-<8 hex do Node ID>" (único por nó)
     *
     * Isso garante que N nós de campo na mesma rede nunca colidam de hostname
     * e que o fluxo de provisionamento funcione independente do hardware.
     */
    const node_profile_desc_t *profile = device_profile_get_current();
    const bool is_gateway = (profile && profile->type == NODE_PROFILE_GATEWAY);

    char hostname[ENDAP_MDNS_HOSTNAME_MAX];
    char instance_name[48];

    if (is_gateway)
    {
        snprintf(hostname, sizeof(hostname), "endap");
        snprintf(instance_name, sizeof(instance_name), "ENDAP Gateway");
    }
    else
    {
        snprintf(hostname, sizeof(hostname), "endap-field-%08lX", (unsigned long)my_id);
        snprintf(instance_name, sizeof(instance_name), "ENDAP Field Node %lu", (unsigned long)my_id);
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao definir hostname mDNS (%s): %s", hostname, esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set(instance_name);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao definir nome da instância mDNS: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_service_add("ENDAP Dashboard", "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao adicionar serviço mDNS: %s", esp_err_to_name(err));
        return err;
    }

    char id_txt[16];
    snprintf(id_txt, sizeof(id_txt), "%lu", (unsigned long)my_id);
    mdns_service_txt_item_set("_http", "_tcp", "node_id", id_txt);
    mdns_service_txt_item_set("_http", "_tcp", "role",
                              is_gateway ? "gateway" : "field");

    mdns_initialized = true;
    ESP_LOGI(TAG, "mDNS inicializado com sucesso (%s.local)", hostname);
    return ESP_OK;
}