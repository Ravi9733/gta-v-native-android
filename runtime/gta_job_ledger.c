#include <TargetConditionals.h>
#if (TARGET_OS_IPHONE && defined(GTA_JOB_LEDGER_ENABLE) && GTA_JOB_LEDGER_ENABLE) || defined(GTA_JOB_LEDGER_TESTING)
#include "gta_job_ledger.h"
#include <stdatomic.h>
#include <string.h>

/* Single bounded try-lock makes multiwriter publication and manual snapshots
 * coherent. It is deliberately NOT called lock-free: contention drops a trace
 * event immediately. No guest/core/allocator/clock/IO/notification calls occur
 * while held. The only affected state belongs to this diagnostic recorder. */
static atomic_flag busy = ATOMIC_FLAG_INIT;
static atomic_int enabled;
static atomic_uint_fast64_t attempted, dropped_busy;
static gta_job_record records[GTA_JOB_LEDGER_CAPACITY];
static uint64_t recorded, overwritten;

void gta_job_ledger_enable(int value) {
    atomic_store_explicit(&enabled, !!value, memory_order_release);
}
int gta_job_ledger_enabled(void) {
    return atomic_load_explicit(&enabled, memory_order_acquire);
}
int gta_job_ledger_record(const gta_job_record *record) {
    if (!atomic_load_explicit(&enabled, memory_order_acquire)) return 0;
    if (!record || record->event < GTA_JOB_COUNTED_SUBMIT ||
        record->event > GTA_JOB_DEP_RELEASE) return -2;
    atomic_fetch_add_explicit(&attempted, 1, memory_order_relaxed);
    if (atomic_flag_test_and_set_explicit(&busy, memory_order_acquire)) {
        atomic_fetch_add_explicit(&dropped_busy, 1, memory_order_relaxed);
        return -1;
    }
    /* Recheck under lock to avoid recording after a disable observed here.
     * Administrative reset separately requires externally quiesced writers. */
    if (!atomic_load_explicit(&enabled, memory_order_acquire)) {
        atomic_flag_clear_explicit(&busy, memory_order_release);
        return 0;
    }
    gta_job_record value = *record;
    value.sequence = ++recorded;
    records[(recorded - 1) % GTA_JOB_LEDGER_CAPACITY] = value;
    if (recorded > GTA_JOB_LEDGER_CAPACITY) overwritten++;
    atomic_flag_clear_explicit(&busy, memory_order_release);
    return 1;
}
int gta_job_ledger_snapshot(gta_job_record *out, size_t capacity,
                            gta_job_ledger_meta *metadata) {
    if (!metadata) return -2;
    if (atomic_flag_test_and_set_explicit(&busy, memory_order_acquire)) return -1;
    size_t n = recorded < GTA_JOB_LEDGER_CAPACITY ? (size_t)recorded : GTA_JOB_LEDGER_CAPACITY;
    if (capacity < n || (n && !out)) {
        atomic_flag_clear_explicit(&busy, memory_order_release);
        return -2;
    }
    uint64_t first = recorded - n;
    for (size_t i = 0; i < n; ++i)
        out[i] = records[(first + i) % GTA_JOB_LEDGER_CAPACITY];
    *metadata = (gta_job_ledger_meta){
        .attempted = atomic_load_explicit(&attempted, memory_order_relaxed),
        .recorded = recorded,
        .dropped_busy = atomic_load_explicit(&dropped_busy, memory_order_relaxed),
        .overwritten = overwritten,
        .first_sequence = n ? first + 1 : 0,
        .last_sequence = recorded, .count = n,
        .enabled = atomic_load_explicit(&enabled, memory_order_acquire)
    };
    atomic_flag_clear_explicit(&busy, memory_order_release);
    return 1;
}
int gta_job_ledger_reset(void) {
    if (atomic_load_explicit(&enabled, memory_order_acquire)) return -2;
    if (atomic_flag_test_and_set_explicit(&busy, memory_order_acquire)) return -1;
    if (atomic_load_explicit(&enabled, memory_order_acquire)) {
        atomic_flag_clear_explicit(&busy, memory_order_release);
        return -2;
    }
    recorded = overwritten = 0;
    atomic_store_explicit(&attempted, 0, memory_order_relaxed);
    atomic_store_explicit(&dropped_busy, 0, memory_order_relaxed);
    memset(records, 0, sizeof records);
    atomic_flag_clear_explicit(&busy, memory_order_release);
    return 1;
}

#endif
