#include "endap_onboarding.h"
#include "device_profile.h"
#include "endap_nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include <string.h>

#define TAG "ONBOARDING"
#define ONBOARDING_NVS_NAMESPACE "onboarding"
#define ONBOARDING_NVS_KEY "ob_cfg_v1"

static endap_onboarding_config_t g_onboarding_cfg = {
    .version = ENDAP_ONBOARDING_CONFIG_VERSION,
    .state = ENDAP_ONBOARDING_STATE_PENDING,
    .profile = NODE_PROFILE_FIELD,
    .reserved0 = 0,
    .adopted_by_gateway_id = 0,
    .adopted_timestamp = 0,
    .node_name = "ENDAP-Field-Node",
};

static bool g_onboarding_initialized = false;

static uint32_t get_system_time_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static esp_err_t save_config_to_nvs(const endap_onboarding_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ONBOARDING_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao abrir NVS namespace '%s': %s", ONBOARDING_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs, ONBOARDING_NVS_KEY, cfg, sizeof(endap_onboarding_config_t));
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t load_config_from_nvs(endap_onboarding_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ONBOARDING_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK)
    {
        return err;
    }

    size_t required_size = sizeof(endap_onboarding_config_t);
    err = nvs_get_blob(nvs, ONBOARDING_NVS_KEY, cfg, &required_size);
    nvs_close(nvs);

    if (err == ESP_OK && cfg->version != ENDAP_ONBOARDING_CONFIG_VERSION)
    {
        ESP_LOGW(TAG, "Versao invalida da config na NVS (%d != %d)", cfg->version, ENDAP_ONBOARDING_CONFIG_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    return err;
}

esp_err_t endap_onboarding_init(void)
{
    if (g_onboarding_initialized)
    {
        return ESP_OK;
    }

    endap_onboarding_config_t loaded_cfg;
    esp_err_t err = load_config_from_nvs(&loaded_cfg);

    if (err == ESP_OK)
    {
        g_onboarding_cfg = loaded_cfg;
        ESP_LOGI(TAG, "Configuracao de onboarding restaurada da NVS: state=%s, profile=%d, gw=%" PRIu32 ", name='%s'",
                 endap_onboarding_state_name((endap_onboarding_state_t)g_onboarding_cfg.state),
                 g_onboarding_cfg.profile,
                 g_onboarding_cfg.adopted_by_gateway_id,
                 g_onboarding_cfg.node_name);
    }
    else
    {
        ESP_LOGI(TAG, "Nenhuma configuracao valida de onboarding na NVS. Inicializando padrao.");

        const node_profile_desc_t *curr_prof = device_profile_get_current();
        if (curr_prof && curr_prof->type == NODE_PROFILE_GATEWAY)
        {
            // Regra 7: Gateway sempre sobe como ACTIVE
            g_onboarding_cfg.state = ENDAP_ONBOARDING_STATE_ACTIVE;
            g_onboarding_cfg.profile = NODE_PROFILE_GATEWAY;
            strncpy(g_onboarding_cfg.node_name, "ENDAP-Gateway", ENDAP_NODE_NAME_MAX - 1);
            save_config_to_nvs(&g_onboarding_cfg);
            ESP_LOGI(TAG, "Perfil Gateway detectado: forçando onboarding ACTIVE");
        }
        else
        {
            g_onboarding_cfg.state = ENDAP_ONBOARDING_STATE_PENDING;
            g_onboarding_cfg.profile = curr_prof ? curr_prof->type : NODE_PROFILE_FIELD;
        }
    }

    g_onboarding_initialized = true;
    return ESP_OK;
}

endap_onboarding_state_t endap_onboarding_get_state(void)
{
    if (!g_onboarding_initialized)
    {
        endap_onboarding_init();
    }
    return (endap_onboarding_state_t)g_onboarding_cfg.state;
}

bool endap_onboarding_is_pending(void)
{
    endap_onboarding_state_t st = endap_onboarding_get_state();
    return (st == ENDAP_ONBOARDING_STATE_PENDING || st == ENDAP_ONBOARDING_STATE_DISCOVERED);
}

esp_err_t endap_onboarding_get_config(endap_onboarding_config_t *out_cfg)
{
    if (!out_cfg)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_onboarding_initialized)
    {
        endap_onboarding_init();
    }

    *out_cfg = g_onboarding_cfg;
    return ESP_OK;
}

esp_err_t endap_onboarding_claim(node_profile_t profile, uint32_t gateway_id, const char *node_name)
{
    if (!device_profile_is_valid(profile))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_onboarding_initialized)
    {
        endap_onboarding_init();
    }

    // Regra 5: IDEMPOTÊNCIA
    // Se o perfil, gateway_id e estado já forem os mesmos, apenas garante sincronia sem reescrever NVS à toa.
    if (g_onboarding_cfg.state == ENDAP_ONBOARDING_STATE_ACTIVE &&
        g_onboarding_cfg.profile == (uint8_t)profile &&
        g_onboarding_cfg.adopted_by_gateway_id == gateway_id)
    {
        if (node_name && strcmp(g_onboarding_cfg.node_name, node_name) == 0)
        {
            ESP_LOGD(TAG, "endap_onboarding_claim: Chamada idempotente. Sem alteracoes necessarias.");
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Executando claim do no: profile=%d, gateway_id=%" PRIu32 ", name='%s'",
             profile, gateway_id, node_name ? node_name : "default");

    // 1. Aplica o template no Device Profile
    esp_err_t err = device_profile_apply_template(profile);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao aplicar template de perfil %d: %s", profile, esp_err_to_name(err));
        return err;
    }

    // 2. Atualiza a struct de configuração local
    g_onboarding_cfg.version = ENDAP_ONBOARDING_CONFIG_VERSION;
    g_onboarding_cfg.state = ENDAP_ONBOARDING_STATE_ACTIVE;
    g_onboarding_cfg.profile = (uint8_t)profile;
    g_onboarding_cfg.adopted_by_gateway_id = gateway_id;
    g_onboarding_cfg.adopted_timestamp = get_system_time_sec();

    if (node_name && strlen(node_name) > 0)
    {
        strncpy(g_onboarding_cfg.node_name, node_name, ENDAP_NODE_NAME_MAX - 1);
        g_onboarding_cfg.node_name[ENDAP_NODE_NAME_MAX - 1] = '\0';
    }

    // 3. Persiste no NVS (namespace "onboarding")
    err = save_config_to_nvs(&g_onboarding_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Falha ao salvar onboarding no NVS: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Claim concluido com sucesso. No ativado como %s.", endap_onboarding_state_name(ENDAP_ONBOARDING_STATE_ACTIVE));
    return ESP_OK;
}

esp_err_t endap_onboarding_reset(void)
{
    ESP_LOGW(TAG, "Executando reset do onboarding...");

    // Regra 6: Limpa as chaves NVS do onboarding
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ONBOARDING_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK)
    {
        nvs_erase_key(nvs, ONBOARDING_NVS_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    // Restaura o estado em memória para PENDING
    g_onboarding_cfg.state = ENDAP_ONBOARDING_STATE_PENDING;
    g_onboarding_cfg.adopted_by_gateway_id = 0;
    g_onboarding_cfg.adopted_timestamp = 0;
    strncpy(g_onboarding_cfg.node_name, "ENDAP-Field-Node", ENDAP_NODE_NAME_MAX - 1);

    ESP_LOGI(TAG, "Reset concluido. Estado restaurado para PENDING.");
    return ESP_OK;
}

esp_err_t endap_factory_reset_full(void)
{
    ESP_LOGW(TAG, "Iniciando Factory Reset Completo do dispositivo...");

    // 1. Limpa NVS de Onboarding
    endap_onboarding_reset();

    // 2. Restaura o perfil de nó para FIELD (default seguro)
    device_profile_set_current(NODE_PROFILE_FIELD);
    device_profile_apply_template(NODE_PROFILE_FIELD);

    // 3. Limpa NVS de dev_profile (net_cfg_v2 e node_profile)
    nvs_handle_t nvs_dp;
    if (nvs_open("dev_profile", NVS_READWRITE, &nvs_dp) == ESP_OK)
    {
        nvs_erase_all(nvs_dp);
        nvs_commit(nvs_dp);
        nvs_close(nvs_dp);
    }

    ESP_LOGI(TAG, "Factory Reset Completo realizado com sucesso. O no voltou ao estado PENDING virgem.");
    return ESP_OK;
}

const char *endap_onboarding_state_name(endap_onboarding_state_t state)
{
    switch (state)
    {
        case ENDAP_ONBOARDING_STATE_PENDING:
            return "PENDING";
        case ENDAP_ONBOARDING_STATE_DISCOVERED:
            return "DISCOVERED";
        case ENDAP_ONBOARDING_STATE_CLAIMED:
            return "CLAIMED";
        case ENDAP_ONBOARDING_STATE_ACTIVE:
            return "ACTIVE";
        default:
            return "UNKNOWN";
    }
}
