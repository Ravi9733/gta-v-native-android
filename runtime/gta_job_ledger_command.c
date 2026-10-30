#include <TargetConditionals.h>
#if (TARGET_OS_IPHONE && defined(GTA_JOB_LEDGER_ENABLE) && GTA_JOB_LEDGER_ENABLE) || defined(GTA_JOB_LEDGER_TESTING)
#include "gta_job_ledger_command.h"
#include "gta_job_ledger.h"
#include "gta_job_ledger_sites.h"
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <string.h>

/* Native input/background thread only. Never put this buffer on a guest stack,
 * allocate it on the render path, or emit a record from inside a gate. */
static gta_job_record snapshot[GTA_JOB_LEDGER_CAPACITY];
static atomic_flag dumping=ATOMIC_FLAG_INIT;
int gta_job_ledger_dump(FILE *out) {
    if (!out) return -2;
    if (atomic_flag_test_and_set_explicit(&dumping,memory_order_acquire)) return -1;
    gta_job_ledger_meta m;
    int result=gta_job_ledger_snapshot(snapshot,GTA_JOB_LEDGER_CAPACITY,&m);
    if (result==1) {
        mach_timebase_info_data_t tb={0}; (void)mach_timebase_info(&tb);
        fprintf(out,"[gta-job-ledger] snapshot schema=1 image=%s enabled=%d count=%zu attempted=%llu recorded=%llu dropped_busy=%llu overwritten=%llu first=%llu last=%llu ticks_numer=%u ticks_denom=%u\n",
           GTA_JOB_LEDGER_IMAGE_SHA256,m.enabled,m.count,(unsigned long long)m.attempted,
           (unsigned long long)m.recorded,(unsigned long long)m.dropped_busy,
           (unsigned long long)m.overwritten,(unsigned long long)m.first_sequence,
           (unsigned long long)m.last_sequence,tb.numer,tb.denom);
        for (size_t i=0;i<m.count;i++) {
            const gta_job_record *r=&snapshot[i];
            fprintf(out,"[gta-job-ledger] event seq=%llu ticks=%llu type=%u thread=%#x site=%#x outer=%#llx job=%#llx dependency=%#llx callback=%#llx aux=%#llx count=%#x:%#x link=%#x:%#x flags=%#x\n",
              (unsigned long long)r->sequence,(unsigned long long)r->ticks,r->event,r->thread,r->site,
              (unsigned long long)r->outer,(unsigned long long)r->job,
              (unsigned long long)r->dependency,(unsigned long long)r->callback,
              (unsigned long long)r->auxiliary,r->count_before,r->count_after,
              r->link_before,r->link_after,r->flags);
        }
        if(ferror(out))result=-3;
    }
    atomic_flag_clear_explicit(&dumping,memory_order_release);
    return result;
}
static int exact(const char *line,const char *command) {
    size_t n=strlen(command);
    return !strncmp(line,command,n)&&(!line[n]||!strcmp(line+n,"\n")||!strcmp(line+n,"\r\n"));
}
int gta_job_ledger_command(const char *line) {
    if (!line || strncmp(line,"jobledger",9) || (line[9]&&line[9]!=' '&&line[9]!='\r'&&line[9]!='\n'))return 0;
    if (exact(line,"jobledger arm")) {
        gta_job_ledger_enable(1);fprintf(stderr,"[gta-job-ledger] armed manually; bounded history, no queue changes\n");
    } else if (exact(line,"jobledger off")) {
        gta_job_ledger_enable(0);fprintf(stderr,"[gta-job-ledger] recording off; history retained\n");
    } else if (exact(line,"jobledger dump")) {
        int result=gta_job_ledger_dump(stderr);
        if(result!=1)fprintf(stderr,"[gta-job-ledger] snapshot unavailable code=%d; retry manually\n",result);
    } else fprintf(stderr,"[gta-job-ledger] expected jobledger arm|off|dump; no change\n");
    return 1;
}
#endif
