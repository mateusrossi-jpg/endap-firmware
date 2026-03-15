#ifndef ENDAP_CONTROL_LOOP_H
#define ENDAP_CONTROL_LOOP_H

#include <stdint.h>
#include "control_loop_metrics.h"

/* ============================================================
   CONTROL LOOP
============================================================ */

/* start deterministic control loop */
void control_loop_start(void);

/* get runtime metrics */
void control_loop_get_metrics(control_loop_metrics_t *m);

#endif
