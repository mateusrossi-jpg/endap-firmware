#include "auth.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "mbedtls/sha256.h"
#include "nvs.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TAG "AUTH"

#define AUTH_NAMESPACE "security_auth"
#define AUTH_CONFIG_KEY "config"
#define AUTH_CONFIG_VERSION 2U
#define AUTH_LEGACY_CONFIG_VERSION 1U
#define AUTH_HASH_LEN 32U
#define AUTH_SALT_LEN 16U
#define AUTH_PASSWORD_MIN_LEN 8U
#define AUTH_SESSION_TIMEOUT_SECONDS_DEFAULT 1800U
#define AUTH_RATE_LIMIT_THRESHOLD 5U
#define AUTH_RATE_LIMIT_BASE_SECONDS 5U
#define AUTH_RATE_LIMIT_MAX_SECONDS 60U
#define AUTH_AUDIT_CAPACITY AUTH_AUDIT_MAX_ENTRIES
#define AUTH_MAX_SESSIONS 4U
#define AUTH_ACCOUNT_FLAG_BOOTSTRAP 0x01U

typedef struct
{
    uint32_t version;
    uint8_t configured;
    uint8_t reserved[3];
    uint8_t salt[AUTH_SALT_LEN];
    uint8_t hash[AUTH_HASH_LEN];
} auth_config_blob_v1_t;

typedef struct
{
    uint8_t in_use;
    uint8_t enabled;
    uint8_t role;
    uint8_t flags;
    char username[AUTH_USERNAME_LEN + 1];
    uint8_t salt[AUTH_SALT_LEN];
    uint8_t hash[AUTH_HASH_LEN];
} auth_account_record_t;

typedef struct
{
    uint32_t version;
    uint8_t configured;
    uint8_t reserved[3];
    auth_account_record_t accounts[AUTH_MAX_ACCOUNTS];
} auth_config_blob_t;

typedef struct
{
    bool active;
    char token[AUTH_SESSION_TOKEN_LEN + 1];
    uint64_t expires_at_ms;
    uint64_t issued_at_ms;
    int account_index;
} auth_session_state_t;

typedef struct
{
    uint32_t timestamp_ms;
    char action[AUTH_AUDIT_ACTION_LEN];
    char detail[AUTH_AUDIT_DETAIL_LEN];
} auth_audit_entry_t;

static auth_config_blob_t auth_config;
static auth_session_state_t auth_sessions[AUTH_MAX_SESSIONS];
static auth_audit_entry_t auth_audit_ring[AUTH_AUDIT_CAPACITY];
static uint8_t auth_audit_index = 0U;
static uint8_t auth_device_mac[6] = {0};
static uint32_t auth_session_timeout_seconds_value = AUTH_SESSION_TIMEOUT_SECONDS_DEFAULT;
static uint32_t auth_failed_attempts = 0U;
static uint64_t auth_block_until_ms = 0U;
static bool auth_initialized = false;

static uint64_t auth_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static uint32_t auth_role_capability_mask(auth_role_t role)
{
    switch (role)
    {
        case AUTH_ROLE_ADMIN:
            return AUTH_CAP_DASHBOARD_READ |
                   AUTH_CAP_MANUAL_IO |
                   AUTH_CAP_NODE_ADMISSION |
                   AUTH_CAP_PROFILE_WRITE |
                   AUTH_CAP_TRANSPORT_WRITE |
                   AUTH_CAP_AUTOMATION_WRITE |
                   AUTH_CAP_REBOOT_RECOVERY |
                   AUTH_CAP_RUNTIME_DIAGNOSTICS |
                   AUTH_CAP_SECURITY_ADMIN |
                   AUTH_CAP_FAILSAFE_WRITE;

        case AUTH_ROLE_INSTALLER:
            return AUTH_CAP_DASHBOARD_READ |
                   AUTH_CAP_MANUAL_IO |
                   AUTH_CAP_NODE_ADMISSION |
                   AUTH_CAP_PROFILE_WRITE |
                   AUTH_CAP_TRANSPORT_WRITE |
                   AUTH_CAP_AUTOMATION_WRITE |
                   AUTH_CAP_REBOOT_RECOVERY |
                   AUTH_CAP_RUNTIME_DIAGNOSTICS |
                   AUTH_CAP_FAILSAFE_WRITE;

        case AUTH_ROLE_OPERATOR:
            return AUTH_CAP_DASHBOARD_READ |
                   AUTH_CAP_MANUAL_IO;

        case AUTH_ROLE_VIEWER:
            return AUTH_CAP_DASHBOARD_READ;

        default:
            return 0U;
    }
}

const char *auth_role_name(auth_role_t role)
{
    switch (role)
    {
        case AUTH_ROLE_ADMIN:
            return "admin";
        case AUTH_ROLE_INSTALLER:
            return "integrator";
        case AUTH_ROLE_OPERATOR:
            return "operator";
        case AUTH_ROLE_VIEWER:
            return "viewer";
        default:
            return "";
    }
}

bool auth_role_from_text(const char *text, auth_role_t *out_role)
{
    if (!text || !out_role)
        return false;

    if (strcmp(text, "admin") == 0)
    {
        *out_role = AUTH_ROLE_ADMIN;
        return true;
    }

    if (strcmp(text, "integrator") == 0 ||
        strcmp(text, "installer") == 0)
    {
        *out_role = AUTH_ROLE_INSTALLER;
        return true;
    }

    if (strcmp(text, "operator") == 0)
    {
        *out_role = AUTH_ROLE_OPERATOR;
        return true;
    }

    if (strcmp(text, "viewer") == 0)
    {
        *out_role = AUTH_ROLE_VIEWER;
        return true;
    }

    return false;
}

bool auth_role_has_capability(auth_role_t role, auth_capability_t capability)
{
    return (auth_role_capability_mask(role) & (uint32_t)capability) != 0U;
}

static bool auth_constant_time_equals(const uint8_t *left, const uint8_t *right, size_t len)
{
    uint8_t diff = 0U;

    if (!left || !right)
        return false;

    for (size_t i = 0; i < len; i++)
        diff |= (uint8_t)(left[i] ^ right[i]);

    return diff == 0U;
}

static void auth_hash_password(const char *password,
                               const uint8_t *salt,
                               uint8_t out_hash[AUTH_HASH_LEN])
{
    mbedtls_sha256_context ctx;

    if (!password || !salt || !out_hash)
        return;

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, AUTH_SALT_LEN);
    mbedtls_sha256_update(&ctx, auth_device_mac, sizeof(auth_device_mac));
    mbedtls_sha256_update(&ctx, (const unsigned char *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, out_hash);
    mbedtls_sha256_free(&ctx);
}

static void auth_generate_salt(uint8_t salt[AUTH_SALT_LEN])
{
    esp_fill_random(salt, AUTH_SALT_LEN);
}

static void auth_generate_token(char out_token[AUTH_SESSION_TOKEN_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[AUTH_SESSION_TOKEN_LEN / 2U];

    esp_fill_random(raw, sizeof(raw));

    for (size_t i = 0; i < sizeof(raw); i++)
    {
        out_token[i * 2U] = hex[(raw[i] >> 4) & 0x0FU];
        out_token[(i * 2U) + 1U] = hex[raw[i] & 0x0FU];
    }

    out_token[AUTH_SESSION_TOKEN_LEN] = '\0';
}

static bool auth_password_is_strong(const char *password)
{
    if (!password || password[0] == '\0')
        return false;

    return strlen(password) >= AUTH_PASSWORD_MIN_LEN;
}

static bool auth_username_normalize(const char *input,
                                    char *out_username,
                                    size_t out_size,
                                    bool allow_default_admin)
{
    const char *start = input ? input : "";
    size_t len = 0U;

    if (!out_username || out_size == 0U)
        return false;

    while (*start != '\0' && isspace((unsigned char)*start))
        start++;

    len = strlen(start);
    while (len > 0U && isspace((unsigned char)start[len - 1U]))
        len--;

    if (len == 0U)
    {
        if (!allow_default_admin || out_size < sizeof("admin"))
            return false;

        snprintf(out_username, out_size, "admin");
        return true;
    }

    if (len > AUTH_USERNAME_LEN || len >= out_size)
        return false;

    for (size_t i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)start[i];
        char normalized = (char)tolower(ch);

        if (!isalnum(ch) && normalized != '.' && normalized != '-' && normalized != '_')
            return false;

        out_username[i] = normalized;
    }

    out_username[len] = '\0';
    return true;
}

static bool auth_account_in_use(const auth_account_record_t *account)
{
    return account && account->in_use != 0U && account->username[0] != '\0';
}

static bool auth_has_any_account(void)
{
    for (size_t i = 0; i < AUTH_MAX_ACCOUNTS; i++)
    {
        if (auth_account_in_use(&auth_config.accounts[i]))
            return true;
    }

    return false;
}

static bool auth_account_enabled(const auth_account_record_t *account)
{
    return auth_account_in_use(account) && account->enabled != 0U;
}

static bool auth_account_bootstrap(const auth_account_record_t *account)
{
    return auth_account_in_use(account) && (account->flags & AUTH_ACCOUNT_FLAG_BOOTSTRAP) != 0U;
}

static void auth_account_clear(auth_account_record_t *account)
{
    if (!account)
        return;

    memset(account, 0, sizeof(*account));
}

static int auth_find_account_index(const char *normalized_username)
{
    if (!normalized_username || normalized_username[0] == '\0')
        return -1;

    for (size_t i = 0; i < AUTH_MAX_ACCOUNTS; i++)
    {
        const auth_account_record_t *account = &auth_config.accounts[i];

        if (auth_account_in_use(account) &&
            strcmp(account->username, normalized_username) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static int auth_find_free_account_index(void)
{
    for (size_t i = 0; i < AUTH_MAX_ACCOUNTS; i++)
    {
        if (!auth_account_in_use(&auth_config.accounts[i]))
            return (int)i;
    }

    return -1;
}

static int auth_count_enabled_admins_excluding(int excluded_index)
{
    int count = 0;

    for (size_t i = 0; i < AUTH_MAX_ACCOUNTS; i++)
    {
        const auth_account_record_t *account = &auth_config.accounts[i];

        if ((int)i == excluded_index)
            continue;

        if (auth_account_enabled(account) &&
            account->role == (uint8_t)AUTH_ROLE_ADMIN)
        {
            count++;
        }
    }

    return count;
}

static bool auth_account_password_matches(const auth_account_record_t *account,
                                          const char *password)
{
    uint8_t hash[AUTH_HASH_LEN];

    if (!auth_account_enabled(account) || !password || password[0] == '\0')
        return false;

    auth_hash_password(password, account->salt, hash);
    return auth_constant_time_equals(hash, account->hash, sizeof(hash));
}

static void auth_reset_sessions(void)
{
    memset(auth_sessions, 0, sizeof(auth_sessions));
}

static void auth_prune_expired_sessions(void)
{
    uint64_t now_ms = auth_now_ms();

    for (size_t i = 0; i < AUTH_MAX_SESSIONS; i++)
    {
        if (auth_sessions[i].active &&
            auth_sessions[i].expires_at_ms <= now_ms)
        {
            memset(&auth_sessions[i], 0, sizeof(auth_sessions[i]));
        }
    }
}

static int auth_find_session_index(const char *token)
{
    auth_prune_expired_sessions();

    if (!token || token[0] == '\0')
        return -1;

    for (size_t i = 0; i < AUTH_MAX_SESSIONS; i++)
    {
        if (auth_sessions[i].active &&
            strcmp(auth_sessions[i].token, token) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

static int auth_session_account_index_from_token(const char *token)
{
    int session_index = auth_find_session_index(token);

    if (session_index < 0)
        return -1;

    return auth_sessions[session_index].account_index;
}

static void auth_invalidate_sessions_for_account(int account_index)
{
    for (size_t i = 0; i < AUTH_MAX_SESSIONS; i++)
    {
        if (auth_sessions[i].active &&
            auth_sessions[i].account_index == account_index)
        {
            memset(&auth_sessions[i], 0, sizeof(auth_sessions[i]));
        }
    }
}

static bool auth_save_config(void)
{
    nvs_handle_t nvs;

    if (nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return false;

    if (nvs_set_blob(nvs, AUTH_CONFIG_KEY, &auth_config, sizeof(auth_config)) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK)
    {
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    return true;
}

static void auth_seed_open_bootstrap(void)
{
    memset(&auth_config, 0, sizeof(auth_config));
    auth_config.version = AUTH_CONFIG_VERSION;
    auth_config.configured = 0U;
}

static bool auth_bootstrap_open_state(void)
{
    return auth_config.configured == 0U && !auth_has_any_account();
}

static bool auth_sanitize_loaded_config(bool *out_open_bootstrap_migrated)
{
    bool changed = false;

    if (out_open_bootstrap_migrated)
        *out_open_bootstrap_migrated = false;

    if (auth_config.version != AUTH_CONFIG_VERSION)
    {
        auth_seed_open_bootstrap();
        return true;
    }

    if (auth_config.configured == 0U)
    {
        if (auth_has_any_account())
        {
            auth_seed_open_bootstrap();
            changed = true;
            if (out_open_bootstrap_migrated)
                *out_open_bootstrap_migrated = true;
        }

        return changed;
    }

    if (!auth_has_any_account())
    {
        auth_seed_open_bootstrap();
        return true;
    }

    return false;
}

static bool auth_migrate_legacy_config(const auth_config_blob_v1_t *legacy)
{
    auth_account_record_t *account;

    if (!legacy || legacy->version != AUTH_LEGACY_CONFIG_VERSION)
        return false;

    auth_seed_open_bootstrap();

    if (legacy->configured)
    {
        auth_config.configured = 1U;
        account = &auth_config.accounts[0];
        account->in_use = 1U;
        account->enabled = 1U;
        account->role = (uint8_t)AUTH_ROLE_ADMIN;
        account->flags = 0U;
        snprintf(account->username, sizeof(account->username), "admin");
        memcpy(account->salt, legacy->salt, sizeof(account->salt));
        memcpy(account->hash, legacy->hash, sizeof(account->hash));
    }

    if (!auth_save_config())
        ESP_LOGW(TAG, "Falha ao persistir migracao de auth v1 para contas locais");

    if (!legacy->configured)
    {
        ESP_LOGW(TAG,
                 "Auth legado com senha bootstrap foi convertido para bootstrap local aberto. Crie a primeira conta admin pela dashboard.");
        auth_audit_log("bootstrap_open_migrated", "legacy bootstrap convertido para criacao local da primeira conta");
    }

    return true;
}

static bool auth_load_config(void)
{
    nvs_handle_t nvs;
    size_t len = 0U;
    esp_err_t err;

    if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    err = nvs_get_blob(nvs, AUTH_CONFIG_KEY, NULL, &len);
    if (err != ESP_OK || len == 0U)
    {
        nvs_close(nvs);
        return false;
    }

    if (len == sizeof(auth_config_blob_t))
    {
        auth_config_blob_t loaded = {0};
        bool open_bootstrap_migrated = false;

        if (nvs_get_blob(nvs, AUTH_CONFIG_KEY, &loaded, &len) != ESP_OK ||
            loaded.version != AUTH_CONFIG_VERSION)
        {
            nvs_close(nvs);
            return false;
        }

        auth_config = loaded;
        nvs_close(nvs);

        if (auth_sanitize_loaded_config(&open_bootstrap_migrated))
        {
            if (!auth_save_config())
                ESP_LOGW(TAG, "Falha ao persistir saneamento da configuracao de auth");

            if (open_bootstrap_migrated)
            {
                ESP_LOGW(TAG,
                         "Auth local com bootstrap legado foi convertido para bootstrap aberto. Crie a primeira conta admin pela dashboard.");
                auth_audit_log("bootstrap_open_migrated", "bootstrap antigo convertido para conta inicial pela dashboard");
            }
        }

        return true;
    }

    if (len == sizeof(auth_config_blob_v1_t))
    {
        auth_config_blob_v1_t legacy = {0};

        if (nvs_get_blob(nvs, AUTH_CONFIG_KEY, &legacy, &len) != ESP_OK)
        {
            nvs_close(nvs);
            return false;
        }

        nvs_close(nvs);
        return auth_migrate_legacy_config(&legacy);
    }

    nvs_close(nvs);
    return false;
}

static void auth_fill_status(auth_status_t *out_status,
                             bool authenticated,
                             int account_index,
                             uint64_t expires_at_ms)
{
    uint64_t now_ms = auth_now_ms();
    const auth_account_record_t *account = NULL;

    if (!out_status)
        return;

    memset(out_status, 0, sizeof(*out_status));
    out_status->authenticated = authenticated;
    out_status->configured = (auth_config.configured != 0U);
    out_status->bootstrap_open = auth_bootstrap_open_state();
    out_status->session_timeout_seconds = auth_session_timeout_seconds_value;

    if (auth_block_until_ms > now_ms)
        out_status->retry_after_seconds = (uint32_t)(((auth_block_until_ms - now_ms) + 999ULL) / 1000ULL);

    if (!authenticated || account_index < 0 || account_index >= (int)AUTH_MAX_ACCOUNTS)
        return;

    account = &auth_config.accounts[account_index];
    if (!auth_account_enabled(account))
        return;

    out_status->authenticated = true;
    out_status->role = (auth_role_t)account->role;
    out_status->capabilities = auth_role_capability_mask(out_status->role);
    out_status->password_change_required = auth_account_bootstrap(account);
    snprintf(out_status->username, sizeof(out_status->username), "%s", account->username);

    if (expires_at_ms > now_ms)
        out_status->session_expires_in = (uint32_t)((expires_at_ms - now_ms) / 1000ULL);
}

static bool auth_audit_is_warning_action(const char *action)
{
    if (!action || action[0] == '\0')
        return false;

    return strcmp(action, "login_failure") == 0 ||
           strcmp(action, "password_change_invalid_current") == 0 ||
           strcmp(action, "password_change_weak") == 0 ||
           strcmp(action, "password_change_save_failed") == 0 ||
           strcmp(action, "account_save_failed") == 0 ||
           strcmp(action, "account_delete_failed") == 0;
}

void auth_audit_log(const char *action, const char *detail)
{
    auth_audit_entry_t *entry = &auth_audit_ring[auth_audit_index % AUTH_AUDIT_CAPACITY];

    memset(entry, 0, sizeof(*entry));
    entry->timestamp_ms = (uint32_t)(auth_now_ms() & 0xFFFFFFFFU);
    snprintf(entry->action, sizeof(entry->action), "%s", action ? action : "event");
    snprintf(entry->detail, sizeof(entry->detail), "%s", detail ? detail : "");
    auth_audit_index = (uint8_t)((auth_audit_index + 1U) % AUTH_AUDIT_CAPACITY);

    if (auth_audit_is_warning_action(entry->action))
    {
        ESP_LOGW(TAG,
                 "audit action=%s detail=%s ts_ms=%" PRIu32,
                 entry->action,
                 entry->detail,
                 entry->timestamp_ms);
    }
    else
    {
        ESP_LOGI(TAG,
                 "audit action=%s detail=%s ts_ms=%" PRIu32,
                 entry->action,
                 entry->detail,
                 entry->timestamp_ms);
    }
}

int auth_export_audit(auth_audit_info_t *out_entries, size_t max_entries)
{
    int count = 0;
    int exported = 0;
    uint8_t start = 0U;

    auth_init();

    if (!out_entries || max_entries == 0U)
        return 0;

    memset(out_entries, 0, sizeof(auth_audit_info_t) * max_entries);

    for (size_t i = 0; i < AUTH_AUDIT_CAPACITY; i++)
    {
        if (auth_audit_ring[i].action[0] != '\0')
            count++;
    }

    if (count == 0)
        return 0;

    start = (count == (int)AUTH_AUDIT_CAPACITY) ? auth_audit_index : 0U;
    for (int i = 0; i < count && (size_t)exported < max_entries; i++)
    {
        uint8_t index = (uint8_t)((start + (uint8_t)i) % AUTH_AUDIT_CAPACITY);
        const auth_audit_entry_t *entry = &auth_audit_ring[index];
        auth_audit_info_t *out = &out_entries[exported];

        if (entry->action[0] == '\0')
            continue;

        out->timestamp_ms = entry->timestamp_ms;
        snprintf(out->action, sizeof(out->action), "%s", entry->action);
        snprintf(out->detail, sizeof(out->detail), "%s", entry->detail);
        exported++;
    }

    return exported;
}

void auth_init(void)
{
    if (auth_initialized)
        return;

    memset(&auth_config, 0, sizeof(auth_config));
    auth_reset_sessions();
    esp_read_mac(auth_device_mac, ESP_MAC_WIFI_STA);

    if (!auth_load_config())
    {
        auth_seed_open_bootstrap();

        if (!auth_save_config())
            ESP_LOGE(TAG, "Falha ao persistir configuracao inicial de autenticacao");

        ESP_LOGW(TAG,
                 "Primeiro boot de seguranca em bootstrap local aberto. Crie a primeira conta admin pela dashboard deste dispositivo.");
        auth_audit_log("bootstrap_open", "primeira conta admin aguardando criacao local");
    }

    auth_initialized = true;
}

bool auth_is_configured(void)
{
    auth_init();
    return auth_config.configured != 0U;
}

bool auth_bootstrap_open(void)
{
    auth_init();
    return auth_bootstrap_open_state();
}

bool auth_rate_limit_check(uint32_t *out_retry_after_seconds)
{
    uint64_t now_ms = auth_now_ms();

    auth_init();

    if (out_retry_after_seconds)
        *out_retry_after_seconds = 0U;

    if (auth_block_until_ms > now_ms)
    {
        if (out_retry_after_seconds)
            *out_retry_after_seconds = (uint32_t)(((auth_block_until_ms - now_ms) + 999ULL) / 1000ULL);
        return false;
    }

    return true;
}

static void auth_record_login_success(const char *username, bool bootstrap_session)
{
    char detail[64];

    auth_failed_attempts = 0U;
    auth_block_until_ms = 0U;

    snprintf(detail,
             sizeof(detail),
             "user=%s%s",
             username && username[0] ? username : "unknown",
             bootstrap_session ? " bootstrap" : "");
    auth_audit_log("login_success", detail);
}

static void auth_record_login_failure(const char *username)
{
    uint32_t block_seconds = 0U;
    char detail[80];

    auth_failed_attempts++;
    if (auth_failed_attempts >= AUTH_RATE_LIMIT_THRESHOLD)
    {
        uint32_t step = auth_failed_attempts - AUTH_RATE_LIMIT_THRESHOLD + 1U;
        block_seconds = AUTH_RATE_LIMIT_BASE_SECONDS * step;
        if (block_seconds > AUTH_RATE_LIMIT_MAX_SECONDS)
            block_seconds = AUTH_RATE_LIMIT_MAX_SECONDS;
        auth_block_until_ms = auth_now_ms() + ((uint64_t)block_seconds * 1000ULL);
    }

    snprintf(detail,
             sizeof(detail),
             "user=%s %s",
             username && username[0] ? username : "unknown",
             block_seconds ? "rate-limited" : "invalid-credentials");
    auth_audit_log("login_failure", detail);
}

static bool auth_create_session_for_account(int account_index,
                                            char *out_token,
                                            size_t out_size,
                                            uint32_t *out_expires_in)
{
    auth_session_state_t *target = NULL;
    uint64_t now_ms = auth_now_ms();
    uint64_t oldest_issued = UINT64_MAX;

    auth_prune_expired_sessions();

    for (size_t i = 0; i < AUTH_MAX_SESSIONS; i++)
    {
        if (!auth_sessions[i].active)
        {
            target = &auth_sessions[i];
            break;
        }

        if (auth_sessions[i].issued_at_ms < oldest_issued)
        {
            oldest_issued = auth_sessions[i].issued_at_ms;
            target = &auth_sessions[i];
        }
    }

    if (!target || !out_token || out_size < (AUTH_SESSION_TOKEN_LEN + 1U))
        return false;

    memset(target, 0, sizeof(*target));
    auth_generate_token(target->token);
    target->active = true;
    target->account_index = account_index;
    target->issued_at_ms = now_ms;
    target->expires_at_ms = now_ms + ((uint64_t)auth_session_timeout_seconds_value * 1000ULL);

    snprintf(out_token, out_size, "%s", target->token);
    if (out_expires_in)
        *out_expires_in = auth_session_timeout_seconds_value;

    return true;
}

auth_login_result_t auth_login(const char *username,
                               const char *password,
                               char *out_token,
                               size_t out_size,
                               auth_status_t *out_status)
{
    char normalized_username[AUTH_USERNAME_LEN + 1] = {0};
    uint32_t retry_after = 0U;
    int account_index = -1;
    uint32_t expires_in = 0U;

    auth_init();

    if (auth_bootstrap_open_state())
    {
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_LOGIN_INVALID_CREDENTIALS;
    }

    if (!auth_username_normalize(username, normalized_username, sizeof(normalized_username), true) ||
        !password || password[0] == '\0')
    {
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_LOGIN_INVALID_CREDENTIALS;
    }

    if (!auth_rate_limit_check(&retry_after))
    {
        auth_fill_status(out_status, false, -1, 0U);
        if (out_status)
            out_status->retry_after_seconds = retry_after;
        return AUTH_LOGIN_RATE_LIMITED;
    }

    account_index = auth_find_account_index(normalized_username);
    if (account_index < 0 ||
        !auth_account_password_matches(&auth_config.accounts[account_index], password))
    {
        auth_record_login_failure(normalized_username);
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_LOGIN_INVALID_CREDENTIALS;
    }

    if (!auth_create_session_for_account(account_index, out_token, out_size, &expires_in))
    {
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_LOGIN_SESSION_CREATE_FAILED;
    }

    auth_record_login_success(normalized_username,
                              auth_account_bootstrap(&auth_config.accounts[account_index]));
    auth_get_status(out_token, out_status);
    return AUTH_LOGIN_OK;
}

auth_bootstrap_create_result_t auth_bootstrap_create_first_admin(const char *username,
                                                                 const char *password,
                                                                 char *out_token,
                                                                 size_t out_size,
                                                                 auth_status_t *out_status)
{
    auth_config_blob_t previous_config;
    char normalized_username[AUTH_USERNAME_LEN + 1] = {0};
    auth_account_record_t *account = NULL;
    char audit_detail[96];

    auth_init();

    if (!auth_bootstrap_open_state())
    {
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_BOOTSTRAP_CREATE_ALREADY_CONFIGURED;
    }

    if (!auth_username_normalize(username, normalized_username, sizeof(normalized_username), true))
    {
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_BOOTSTRAP_CREATE_INVALID_USERNAME;
    }

    if (!auth_password_is_strong(password))
    {
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_BOOTSTRAP_CREATE_WEAK_PASSWORD;
    }

    previous_config = auth_config;
    account = &auth_config.accounts[0];
    auth_account_clear(account);
    account->in_use = 1U;
    account->enabled = 1U;
    account->role = (uint8_t)AUTH_ROLE_ADMIN;
    account->flags = 0U;
    snprintf(account->username, sizeof(account->username), "%s", normalized_username);
    auth_generate_salt(account->salt);
    auth_hash_password(password, account->salt, account->hash);
    auth_config.configured = 1U;

    if (!auth_save_config())
    {
        auth_config = previous_config;
        auth_audit_log("bootstrap_admin_create_failed", normalized_username);
        auth_fill_status(out_status, false, -1, 0U);
        return AUTH_BOOTSTRAP_CREATE_SAVE_FAILED;
    }

    if (!auth_create_session_for_account(0, out_token, out_size, NULL))
    {
        auth_get_status(NULL, out_status);
        return AUTH_BOOTSTRAP_CREATE_SESSION_CREATE_FAILED;
    }

    auth_record_login_success(normalized_username, false);
    snprintf(audit_detail,
             sizeof(audit_detail),
             "user=%s first-admin-created",
             normalized_username);
    auth_audit_log("bootstrap_admin_created", audit_detail);
    auth_get_status(out_token, out_status);
    return AUTH_BOOTSTRAP_CREATE_OK;
}

bool auth_validate_session(const char *token, auth_status_t *out_status)
{
    int session_index;
    int account_index;

    auth_init();
    session_index = auth_find_session_index(token);
    if (session_index < 0)
    {
        auth_fill_status(out_status, false, -1, 0U);
        return false;
    }

    account_index = auth_sessions[session_index].account_index;
    if (account_index < 0 ||
        account_index >= (int)AUTH_MAX_ACCOUNTS ||
        !auth_account_enabled(&auth_config.accounts[account_index]))
    {
        memset(&auth_sessions[session_index], 0, sizeof(auth_sessions[session_index]));
        auth_fill_status(out_status, false, -1, 0U);
        return false;
    }

    auth_fill_status(out_status,
                     true,
                     account_index,
                     auth_sessions[session_index].expires_at_ms);
    return true;
}

void auth_get_status(const char *token, auth_status_t *out_status)
{
    (void)auth_validate_session(token, out_status);
}

void auth_destroy_session(const char *token)
{
    int session_index;

    auth_init();

    if (!token || token[0] == '\0')
    {
        auth_reset_sessions();
        return;
    }

    session_index = auth_find_session_index(token);
    if (session_index >= 0)
        memset(&auth_sessions[session_index], 0, sizeof(auth_sessions[session_index]));
}

auth_change_password_result_t auth_change_password(const char *token,
                                                   const char *current_password,
                                                   const char *new_password)
{
    auth_config_blob_t previous_config;
    int session_index;
    int account_index;
    auth_account_record_t *account;
    uint8_t hash[AUTH_HASH_LEN];
    bool bootstrap_flow = false;
    char audit_detail[64];

    auth_init();

    session_index = auth_find_session_index(token);
    if (session_index < 0)
        return AUTH_CHANGE_PASSWORD_INTERNAL_ERROR;

    account_index = auth_sessions[session_index].account_index;
    if (account_index < 0 || account_index >= (int)AUTH_MAX_ACCOUNTS)
        return AUTH_CHANGE_PASSWORD_INTERNAL_ERROR;

    account = &auth_config.accounts[account_index];
    if (!auth_account_enabled(account))
        return AUTH_CHANGE_PASSWORD_INTERNAL_ERROR;

    bootstrap_flow = auth_account_bootstrap(account);
    if (!bootstrap_flow)
    {
        if (!current_password || current_password[0] == '\0' ||
            !auth_account_password_matches(account, current_password))
        {
            auth_audit_log("password_change_invalid_current", account->username);
            return AUTH_CHANGE_PASSWORD_INVALID_CURRENT;
        }
    }

    if (!auth_password_is_strong(new_password))
    {
        auth_audit_log("password_change_weak", account->username);
        return AUTH_CHANGE_PASSWORD_WEAK;
    }

    previous_config = auth_config;
    auth_generate_salt(account->salt);
    auth_hash_password(new_password, account->salt, hash);
    memcpy(account->hash, hash, sizeof(account->hash));
    account->flags &= (uint8_t)(~AUTH_ACCOUNT_FLAG_BOOTSTRAP);
    auth_config.configured = 1U;

    if (!auth_save_config())
    {
        auth_config = previous_config;
        auth_audit_log("password_change_save_failed", account->username);
        return AUTH_CHANGE_PASSWORD_SAVE_FAILED;
    }

    auth_sessions[session_index].expires_at_ms = auth_now_ms() +
                                                 ((uint64_t)auth_session_timeout_seconds_value * 1000ULL);
    snprintf(audit_detail,
             sizeof(audit_detail),
             "user=%s %s",
             account->username,
             bootstrap_flow ? "bootstrap-completed" : "password-updated");
    auth_audit_log("password_changed", audit_detail);
    return AUTH_CHANGE_PASSWORD_OK;
}

int auth_list_accounts(auth_account_info_t *out_accounts, size_t max_accounts)
{
    int count = 0;

    auth_init();
    auth_prune_expired_sessions();

    if (!out_accounts || max_accounts == 0U)
        return 0;

    memset(out_accounts, 0, sizeof(auth_account_info_t) * max_accounts);

    for (size_t i = 0; i < AUTH_MAX_ACCOUNTS && (size_t)count < max_accounts; i++)
    {
        auth_account_info_t *info;
        const auth_account_record_t *account = &auth_config.accounts[i];

        if (!auth_account_in_use(account))
            continue;

        info = &out_accounts[count++];
        info->enabled = account->enabled != 0U;
        info->bootstrap = auth_account_bootstrap(account);
        info->role = (auth_role_t)account->role;
        snprintf(info->username, sizeof(info->username), "%s", account->username);

        for (size_t session_index = 0; session_index < AUTH_MAX_SESSIONS; session_index++)
        {
            if (auth_sessions[session_index].active &&
                auth_sessions[session_index].account_index == (int)i)
            {
                info->current_session = true;
                break;
            }
        }
    }

    return count;
}

auth_account_save_result_t auth_save_account(const char *current_token,
                                             const char *username,
                                             auth_role_t role,
                                             bool enabled,
                                             const char *password)
{
    auth_config_blob_t previous_config;
    char normalized_username[AUTH_USERNAME_LEN + 1] = {0};
    int current_account_index;
    int existing_index;
    bool password_present = password && password[0] != '\0';
    auth_account_record_t *account = NULL;
    char audit_detail[96];

    auth_init();

    if (!auth_username_normalize(username, normalized_username, sizeof(normalized_username), false))
        return AUTH_ACCOUNT_SAVE_INVALID_USERNAME;

    if (role != AUTH_ROLE_ADMIN &&
        role != AUTH_ROLE_INSTALLER &&
        role != AUTH_ROLE_OPERATOR &&
        role != AUTH_ROLE_VIEWER)
    {
        return AUTH_ACCOUNT_SAVE_INVALID_ROLE;
    }

    current_account_index = auth_session_account_index_from_token(current_token);
    if (current_account_index < 0 ||
        current_account_index >= (int)AUTH_MAX_ACCOUNTS ||
        !auth_account_enabled(&auth_config.accounts[current_account_index]) ||
        auth_config.accounts[current_account_index].role != (uint8_t)AUTH_ROLE_ADMIN)
    {
        return AUTH_ACCOUNT_SAVE_FORBIDDEN;
    }

    existing_index = auth_find_account_index(normalized_username);
    if (existing_index >= 0)
    {
        account = &auth_config.accounts[existing_index];

        if (existing_index == current_account_index)
            return AUTH_ACCOUNT_SAVE_FORBIDDEN;

        if ((!enabled || role != AUTH_ROLE_ADMIN) &&
            account->role == (uint8_t)AUTH_ROLE_ADMIN &&
            auth_count_enabled_admins_excluding(existing_index) == 0)
        {
            return AUTH_ACCOUNT_SAVE_LAST_ADMIN;
        }

        previous_config = auth_config;
        account->enabled = enabled ? 1U : 0U;
        account->role = (uint8_t)role;

        if (password_present)
        {
            uint8_t hash[AUTH_HASH_LEN];

            if (!auth_password_is_strong(password))
                return AUTH_ACCOUNT_SAVE_WEAK_PASSWORD;

            auth_generate_salt(account->salt);
            auth_hash_password(password, account->salt, hash);
            memcpy(account->hash, hash, sizeof(account->hash));
            account->flags &= (uint8_t)(~AUTH_ACCOUNT_FLAG_BOOTSTRAP);
            auth_invalidate_sessions_for_account(existing_index);
        }

        if (!auth_save_config())
        {
            auth_config = previous_config;
            auth_audit_log("account_save_failed", normalized_username);
            return AUTH_ACCOUNT_SAVE_SAVE_FAILED;
        }

        snprintf(audit_detail,
                 sizeof(audit_detail),
                 "user=%s role=%s enabled=%u updated",
                 normalized_username,
                 auth_role_name(role),
                 enabled ? 1U : 0U);
        auth_audit_log("account_saved", audit_detail);
        return AUTH_ACCOUNT_SAVE_OK;
    }

    if (!password_present)
        return AUTH_ACCOUNT_SAVE_WEAK_PASSWORD;

    if (!auth_password_is_strong(password))
        return AUTH_ACCOUNT_SAVE_WEAK_PASSWORD;

    existing_index = auth_find_free_account_index();
    if (existing_index < 0)
        return AUTH_ACCOUNT_SAVE_NO_SPACE;

    previous_config = auth_config;
    account = &auth_config.accounts[existing_index];
    auth_account_clear(account);
    account->in_use = 1U;
    account->enabled = enabled ? 1U : 0U;
    account->role = (uint8_t)role;
    snprintf(account->username, sizeof(account->username), "%s", normalized_username);
    auth_generate_salt(account->salt);
    auth_hash_password(password, account->salt, account->hash);

    if (!auth_save_config())
    {
        auth_config = previous_config;
        auth_audit_log("account_save_failed", normalized_username);
        return AUTH_ACCOUNT_SAVE_SAVE_FAILED;
    }

    snprintf(audit_detail,
             sizeof(audit_detail),
             "user=%s role=%s enabled=%u created",
             normalized_username,
             auth_role_name(role),
             enabled ? 1U : 0U);
    auth_audit_log("account_saved", audit_detail);
    return AUTH_ACCOUNT_SAVE_OK;
}

auth_account_delete_result_t auth_delete_account(const char *current_token,
                                                 const char *username)
{
    auth_config_blob_t previous_config;
    char normalized_username[AUTH_USERNAME_LEN + 1] = {0};
    int current_account_index;
    int account_index;
    char audit_detail[64];

    auth_init();

    if (!auth_username_normalize(username, normalized_username, sizeof(normalized_username), false))
        return AUTH_ACCOUNT_DELETE_NOT_FOUND;

    current_account_index = auth_session_account_index_from_token(current_token);
    if (current_account_index < 0 ||
        current_account_index >= (int)AUTH_MAX_ACCOUNTS ||
        !auth_account_enabled(&auth_config.accounts[current_account_index]) ||
        auth_config.accounts[current_account_index].role != (uint8_t)AUTH_ROLE_ADMIN)
    {
        return AUTH_ACCOUNT_DELETE_FORBIDDEN;
    }

    account_index = auth_find_account_index(normalized_username);
    if (account_index < 0)
        return AUTH_ACCOUNT_DELETE_NOT_FOUND;

    if (account_index == current_account_index)
        return AUTH_ACCOUNT_DELETE_FORBIDDEN;

    if (auth_config.accounts[account_index].role == (uint8_t)AUTH_ROLE_ADMIN &&
        auth_count_enabled_admins_excluding(account_index) == 0)
    {
        return AUTH_ACCOUNT_DELETE_LAST_ADMIN;
    }

    previous_config = auth_config;
    auth_account_clear(&auth_config.accounts[account_index]);
    auth_invalidate_sessions_for_account(account_index);

    if (!auth_save_config())
    {
        auth_config = previous_config;
        auth_audit_log("account_delete_failed", normalized_username);
        return AUTH_ACCOUNT_DELETE_SAVE_FAILED;
    }

    snprintf(audit_detail, sizeof(audit_detail), "user=%s deleted", normalized_username);
    auth_audit_log("account_deleted", audit_detail);
    return AUTH_ACCOUNT_DELETE_OK;
}

uint32_t auth_session_timeout_seconds(void)
{
    return auth_session_timeout_seconds_value;
}
