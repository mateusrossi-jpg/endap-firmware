#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_SESSION_TOKEN_LEN 64
#define AUTH_USERNAME_LEN 24
#define AUTH_MAX_ACCOUNTS 6
#define AUTH_AUDIT_MAX_ENTRIES 16
#define AUTH_AUDIT_ACTION_LEN 24
#define AUTH_AUDIT_DETAIL_LEN 80

typedef enum
{
    AUTH_ROLE_NONE = 0,
    AUTH_ROLE_ADMIN = 1,
    AUTH_ROLE_INSTALLER = 2,
    AUTH_ROLE_OPERATOR = 3,
    AUTH_ROLE_VIEWER = 4,
} auth_role_t;

typedef enum
{
    AUTH_CAP_DASHBOARD_READ = 1U << 0,
    AUTH_CAP_MANUAL_IO = 1U << 1,
    AUTH_CAP_NODE_ADMISSION = 1U << 2,
    AUTH_CAP_PROFILE_WRITE = 1U << 3,
    AUTH_CAP_TRANSPORT_WRITE = 1U << 4,
    AUTH_CAP_AUTOMATION_WRITE = 1U << 5,
    AUTH_CAP_REBOOT_RECOVERY = 1U << 6,
    AUTH_CAP_RUNTIME_DIAGNOSTICS = 1U << 7,
    AUTH_CAP_SECURITY_ADMIN = 1U << 8,
    AUTH_CAP_FAILSAFE_WRITE = 1U << 9,
} auth_capability_t;

typedef struct
{
    bool authenticated;
    bool configured;
    bool bootstrap_open;
    bool password_change_required;
    auth_role_t role;
    uint32_t capabilities;
    uint32_t session_expires_in;
    uint32_t session_timeout_seconds;
    uint32_t retry_after_seconds;
    char username[AUTH_USERNAME_LEN + 1];
} auth_status_t;

typedef struct
{
    bool enabled;
    bool bootstrap;
    bool current_session;
    auth_role_t role;
    char username[AUTH_USERNAME_LEN + 1];
} auth_account_info_t;

typedef struct
{
    uint32_t timestamp_ms;
    char action[AUTH_AUDIT_ACTION_LEN];
    char detail[AUTH_AUDIT_DETAIL_LEN];
} auth_audit_info_t;

typedef enum
{
    AUTH_LOGIN_OK = 0,
    AUTH_LOGIN_INVALID_CREDENTIALS,
    AUTH_LOGIN_RATE_LIMITED,
    AUTH_LOGIN_SESSION_CREATE_FAILED,
} auth_login_result_t;

typedef enum
{
    AUTH_CHANGE_PASSWORD_OK = 0,
    AUTH_CHANGE_PASSWORD_INVALID_CURRENT,
    AUTH_CHANGE_PASSWORD_WEAK,
    AUTH_CHANGE_PASSWORD_SAVE_FAILED,
    AUTH_CHANGE_PASSWORD_INTERNAL_ERROR,
} auth_change_password_result_t;

typedef enum
{
    AUTH_BOOTSTRAP_CREATE_OK = 0,
    AUTH_BOOTSTRAP_CREATE_INVALID_USERNAME,
    AUTH_BOOTSTRAP_CREATE_WEAK_PASSWORD,
    AUTH_BOOTSTRAP_CREATE_ALREADY_CONFIGURED,
    AUTH_BOOTSTRAP_CREATE_SAVE_FAILED,
    AUTH_BOOTSTRAP_CREATE_SESSION_CREATE_FAILED,
} auth_bootstrap_create_result_t;

typedef enum
{
    AUTH_ACCOUNT_SAVE_OK = 0,
    AUTH_ACCOUNT_SAVE_INVALID_USERNAME,
    AUTH_ACCOUNT_SAVE_INVALID_ROLE,
    AUTH_ACCOUNT_SAVE_WEAK_PASSWORD,
    AUTH_ACCOUNT_SAVE_NO_SPACE,
    AUTH_ACCOUNT_SAVE_SAVE_FAILED,
    AUTH_ACCOUNT_SAVE_FORBIDDEN,
    AUTH_ACCOUNT_SAVE_LAST_ADMIN,
} auth_account_save_result_t;

typedef enum
{
    AUTH_ACCOUNT_DELETE_OK = 0,
    AUTH_ACCOUNT_DELETE_NOT_FOUND,
    AUTH_ACCOUNT_DELETE_SAVE_FAILED,
    AUTH_ACCOUNT_DELETE_FORBIDDEN,
    AUTH_ACCOUNT_DELETE_LAST_ADMIN,
} auth_account_delete_result_t;

void auth_init(void);
bool auth_is_configured(void);
bool auth_bootstrap_open(void);
auth_login_result_t auth_login(const char *username,
                               const char *password,
                               char *out_token,
                               size_t out_size,
                               auth_status_t *out_status);
auth_bootstrap_create_result_t auth_bootstrap_create_first_admin(const char *username,
                                                                 const char *password,
                                                                 char *out_token,
                                                                 size_t out_size,
                                                                 auth_status_t *out_status);
bool auth_validate_session(const char *token, auth_status_t *out_status);
void auth_destroy_session(const char *token);
bool auth_rate_limit_check(uint32_t *out_retry_after_seconds);
auth_change_password_result_t auth_change_password(const char *token,
                                                   const char *current_password,
                                                   const char *new_password);
void auth_get_status(const char *token, auth_status_t *out_status);
uint32_t auth_session_timeout_seconds(void);
void auth_audit_log(const char *action, const char *detail);
int auth_export_audit(auth_audit_info_t *out_entries, size_t max_entries);
const char *auth_role_name(auth_role_t role);
bool auth_role_from_text(const char *text, auth_role_t *out_role);
bool auth_role_has_capability(auth_role_t role, auth_capability_t capability);
int auth_list_accounts(auth_account_info_t *out_accounts, size_t max_accounts);
auth_account_save_result_t auth_save_account(const char *current_token,
                                             const char *username,
                                             auth_role_t role,
                                             bool enabled,
                                             const char *password);
auth_account_delete_result_t auth_delete_account(const char *current_token,
                                                 const char *username);

#ifdef __cplusplus
}
#endif
