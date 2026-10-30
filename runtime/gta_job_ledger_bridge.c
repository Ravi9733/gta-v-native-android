#include <TargetConditionals.h>
#if (TARGET_OS_IPHONE && defined(GTA_JOB_LEDGER_ENABLE) && GTA_JOB_LEDGER_ENABLE) || defined(GTA_JOB_LEDGER_TESTING)
#include "gta_job_ledger_bridge.h"
#include "gta_job_ledger.h"
#include "gta_job_ledger_sites.h"
#include <mach/mach_time.h>
#include <string.h>

extern uint32_t runtime_thread_current_handle(void);
static uintptr_t bound_base; /* Bound once on loader thread before any guest. */
int gta_job_ledger_bind(void *main_base, const char *verified_sha) {
    if (!main_base || ((uintptr_t)main_base & 0x3fff) || !verified_sha ||
        strcmp(verified_sha,GTA_JOB_LEDGER_IMAGE_SHA256)) return 0;
    if (bound_base && bound_base!=(uintptr_t)main_base) return 0;
    bound_base=(uintptr_t)main_base;return 1;
}
int gta_job_ledger_gate(uint64_t ret, const uint64_t *regs) {
    if (!bound_base || !regs || ret<bound_base) return 0;
    uint64_t offset=ret-bound_base;uint32_t event,site;
    if (offset==GTA_JOB_COUNTED_SUBMIT_RET) {
        event=GTA_JOB_COUNTED_SUBMIT;site=GTA_JOB_COUNTED_SUBMIT_SITE;
    } else if (offset==GTA_JOB_EXECUTE_RET) {
        event=GTA_JOB_BEGIN;site=GTA_JOB_EXECUTE_SITE;
    } else if (offset==GTA_JOB_OUTER_DECREMENT_RET) {
        event=GTA_JOB_OUTER_DECREMENT;site=GTA_JOB_OUTER_DECREMENT_SITE;
    } else return 0;
    if (!gta_job_ledger_enabled()) return 1;
    gta_job_record r={.ticks=mach_absolute_time(),.event=event,.site=site,
                     .thread=runtime_thread_current_handle()};
    if (event==GTA_JOB_COUNTED_SUBMIT) {
        r.outer=regs[0];r.job=regs[1];r.dependency=regs[7];
        r.count_after=(uint32_t)regs[8];r.count_before=r.count_after-1;
    } else if (event==GTA_JOB_BEGIN) {
        r.job=regs[20];r.callback=regs[8];
    } else {
        r.outer=regs[19];r.job=regs[21];r.count_before=(uint32_t)regs[8];
        r.count_after=(uint32_t)regs[9];
    }
    (void)gta_job_ledger_record(&r);
    return 1;
}

#endif
