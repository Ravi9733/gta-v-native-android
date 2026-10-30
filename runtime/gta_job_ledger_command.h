#pragma once
#include <stdio.h>
/* Native diagnostic input thread only; no automatic arm or live reset. */
int gta_job_ledger_command(const char *line);
int gta_job_ledger_dump(FILE *out);
