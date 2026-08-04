#pragma once

#include "device_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tabela de templates imutáveis de perfil de nó (indexada por node_profile_t).
 */
extern const node_profile_desc_t * const node_profile_templates[NODE_PROFILE_MAX];

#ifdef __cplusplus
}
#endif
