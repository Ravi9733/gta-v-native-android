/* Private diagnostic candidate. No guest memory access or queue modifications. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define GTA_JOB_LEDGER_CAPACITY 8192u
enum gta_job_event {
    GTA_JOB_COUNTED_SUBMIT = 1, GTA_JOB_LINK, GTA_JOB_DEFER,
    GTA_JOB_PUBLISH, GTA_JOB_CLAIM, GTA_JOB_BEGIN, GTA_JOB_END,
    GTA_JOB_OUTER_DECREMENT, GTA_JOB_WAIT_SNAPSHOT, GTA_JOB_DEP_RELEASE
};
/* Inputs are copied values from an exact, verified hook context. Zero means
 * unavailable; neither the recorder nor snapshot dereferences guest pointers.
 * ticks is supplied by the caller's existing monotonic clock, not read here. */
typedef struct {
    uint64_t sequence, ticks, outer, job, dependency, auxiliary, callback;
    uint32_t thread, event, site, count_before, count_after;
    uint32_t link_before, link_after, flags;
} gta_job_record;
typedef struct {
    uint64_t attempted, recorded, dropped_busy, overwritten;
    uint64_t first_sequence, last_sequence;
    size_t count;
    int enabled;
} gta_job_ledger_meta;

/* Disabled by default. Enable/disable never resets diagnostic history. */
void gta_job_ledger_enable(int enabled);
int gta_job_ledger_enabled(void);
/* Returns 1 recorded, 0 disabled, -1 busy/drop, -2 invalid event. No waiting. */
int gta_job_ledger_record(const gta_job_record *record);
/* Manual/background-only: copies oldest->newest while writers try-drop.
 * Returns 1 success, -1 busy, -2 capacity insufficient/invalid pointer.
 * Caller owns output storage and any export I/O. No partial/torn records. */
int gta_job_ledger_snapshot(gta_job_record *out, size_t capacity,
                            gta_job_ledger_meta *metadata);
/* Disabled + externally quiesced writers only (startup/test administration).
 * Do not call while a guest/render thread may be inside record(). Runtime
 * enable/disable and snapshot need no reset. Returns 1, -1 busy, -2 enabled. */
int gta_job_ledger_reset(void);
