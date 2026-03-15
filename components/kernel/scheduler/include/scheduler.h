#ifndef ENDAP_SCHEDULER_H
#define ENDAP_SCHEDULER_H

void scheduler_run_io(void);
void scheduler_run_fieldbus(void);
void scheduler_run_automation(void);
void scheduler_run_events(void);
void scheduler_run_diagnostics(void);

#endif
