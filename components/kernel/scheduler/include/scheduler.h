#ifndef ENDAP_SCHEDULER_H
#define ENDAP_SCHEDULER_H

/* IO */
void scheduler_run_io(void);
void scheduler_run_io_apply(void);

/* Fieldbus */
void scheduler_run_fieldbus(void);

/* Automation */
void scheduler_run_automation(void);

/* Events */
void scheduler_run_events(void);

/* Diagnostics */
void scheduler_run_diagnostics(void);

#endif
