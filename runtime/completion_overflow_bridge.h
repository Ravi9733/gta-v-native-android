#ifndef GTA_COMPLETION_OVERFLOW_BRIDGE_H
#define GTA_COMPLETION_OVERFLOW_BRIDGE_H
#include "completion_overflow.h"
/* Binding must occur after the existing iOS loader verifies the embedded image.
 * These signatures also permit private offline fault injection. */
int runtime_gta_overflow_bind(unsigned char *,const char *,size_t,
                              GtaOverflowAlloc,GtaOverflowFree,void *);
int runtime_gta_overflow_gate(uint64_t,uint64_t *);
int runtime_gta_overflow_shutdown(void);
/* Quiescent offline verification only. Not a live concurrent diagnostics view. */
const GtaCompletionOverflow *runtime_gta_overflow_state(void);
/* REQUIRED integration policy: never returns to the original FULL continuation
 * on OOM/capacity/invalid state. The host must show/report a
 * diagnostic and stop safely, or explicitly terminate; silent continuation
 * would clear pending and lose a completed task. No queue-capacity retry/wait
 * is permitted; a terminal native fault park after yielding the core is allowed. */
_Noreturn void runtime_gta_overflow_failure(int);
#endif
