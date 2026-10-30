#pragma once
#include <stdint.h>
/* Loader-only, before guest starts, after actual embedded SHA/map checks.
 * verified_sha must describe the mapped guest image, not a guessed constant.
 * This does not replace existing loader image/permission/overflow binding. */
int gta_job_ledger_bind(void *main_base, const char *verified_sha);
/* Call before ordinary gate lookup. Exact known tags return1, unknown0.
 * Saved guest registers are read-only; queue state is never touched. */
int gta_job_ledger_gate(uint64_t ret, const uint64_t *regs);
