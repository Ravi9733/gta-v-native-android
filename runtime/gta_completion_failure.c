#include "gta_completion_failure.h"
#include <stdatomic.h>
#include <pthread.h>
#include <limits.h>
extern int runtime_core_away(void);
__attribute__((weak,noinline)) void runtime_gta_overflow_notice(int code) { (void)code; }
static _Atomic int g_first_fault;
static pthread_mutex_t g_pause_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pause_condition=PTHREAD_COND_INITIALIZER;
int runtime_gta_overflow_fault_code(void) { return atomic_load_explicit(&g_first_fault,memory_order_acquire); }
_Noreturn void runtime_gta_overflow_failure(int code) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,0);
    if(!code)code=INT_MIN; /* an invalid failure call is itself a failure, never the no-fault sentinel */
    int expected=0;
    if(atomic_compare_exchange_strong_explicit(&g_first_fault,&expected,code,memory_order_acq_rel,memory_order_acquire))
        runtime_gta_overflow_notice(code); /* nonblocking, optional native observer */
    (void)runtime_core_away(); /* no overflow/queue lock may be held on entry; never take the core back */
    pthread_mutex_lock(&g_pause_mutex);
    for(;;)pthread_cond_wait(&g_pause_condition,&g_pause_mutex); /* parked through spurious wakes, never resumes game */
}
