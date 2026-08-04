#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the self-tests for the PVE component.
 * @return true if all tests passed, false otherwise.
 */
bool pve_run_self_tests(void);

#ifdef __cplusplus
}
#endif
