#include "http_server.h"
#include "cluster_manager.h"
#include "node_registry.h"
#include "endap_onboarding.h"
#include "device_profile.h"
#include "cJSON.h"

esp_err_t cluster_nodes_handler(httpd_req_t *req)
{
    if (!http_server_validate_authorization(req))
        return ESP_OK;

    http_set_private_json_headers(req);

    node_registry_entry_t entries[NODE_REGISTRY_MAX_NODES];
    int count = node_registry_export(entries, NODE_REGISTRY_MAX_NODES);

    cJSON *root = cJSON_CreateArray();

    for (int i = 0; i < count; i++)
    {
        if (entries[i].node_id == 0) continue;

        cJSON *node = cJSON_CreateObject();
        cJSON_AddNumberToObject(node, "node_id", entries[i].node_id);

        // Mapeamento Oficial:
        // - registry_state: estado interno do nó na tabela do Gateway (DISCOVERED, ADOPTED, CONFIGURED, ACTIVE).
        // - onboarding_state: estado equivalente de adoção exposto para o Dashboard.
        const char *reg_state_str = node_registry_state_name((node_registry_state_t)entries[i].registry_state);
        cJSON_AddStringToObject(node, "registry_state", reg_state_str);
        cJSON_AddStringToObject(node, "onboarding_state", reg_state_str);
        cJSON_AddStringToObject(node, "cluster_state", node_registry_cluster_state_name(entries[i].cluster_state));
        cJSON_AddStringToObject(node, "profile", entries[i].profile[0] ? entries[i].profile : "unconfigured");
        cJSON_AddStringToObject(node, "template", entries[i].template_name[0] ? entries[i].template_name : "default");
        cJSON_AddNumberToObject(node, "last_seen_ms", entries[i].age_ms);
        cJSON_AddBoolToObject(node, "operational", node_registry_is_operational(entries[i].node_id));

        cJSON_AddItemToArray(root, node);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

