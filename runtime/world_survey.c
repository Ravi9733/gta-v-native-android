#include "world_survey.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The TotK actor survey stays disabled. These bounded, read-only diagnostics are reached only through
   the explicitly enabled TOTK_INPUT_FILE command stream. */
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2] = {0, -1};   /* read by hle_nvn.c only while a survey is pending, which never happens here */
static _Atomic(Module *) mainModule;
void runtime_survey_init(Module *main_module) { atomic_store_explicit(&mainModule, main_module, memory_order_release); }
static Module *active_main_module(void) {
    Module *m = atomic_load_explicit(&mainModule, memory_order_acquire);
    /* This port has no actor-survey init caller. Discover the active image only when diagnostics ask for it. */
    if (!m || !m->base) for (unsigned i = 0; i < runtime_module_count(); i++) {
        Module *candidate = runtime_module_at(i);
        if (candidate && candidate->base && candidate->name && !strcmp(candidate->name, "main")) {
            runtime_survey_init(candidate); m = candidate; break;
        }
    }
    return m && m->base ? m : NULL;
}

static const char *skip_space(const char *p) { while (isspace((unsigned char)*p)) p++; return p; }
static int command_is(const char *line, const char *name) {
    size_t n = strlen(name);
    return !strncmp(line, name, n) && (!line[n] || isspace((unsigned char)line[n]));
}
static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int peek_command(const char *p) {
    uintptr_t address = 0; unsigned count = 0;
    p = skip_space(p);
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) goto usage;
    p += 2;
    if (hex_digit((unsigned char)*p) < 0) goto usage;
    for (int digit; (digit = hex_digit((unsigned char)*p)) >= 0; p++) {
        if (address > (UINTPTR_MAX - (unsigned)digit) / 16) goto usage;
        address = address * 16 + (unsigned)digit;
    }
    if (!isspace((unsigned char)*p)) goto usage;
    p = skip_space(p);
    if (*p < '0' || *p > '9') goto usage;
    while (*p >= '0' && *p <= '9') {
        count = count * 10 + (unsigned)(*p++ - '0');
        if (count > 512) goto usage;
    }
    if (*skip_space(p) || !count || address > UINTPTR_MAX - (count - 1)) goto usage;
    /* Input-file constructors can run before the loader installs its fault handler; modules are mapped afterwards. */
    if (!active_main_module()) { fprintf(stderr, "[totk-peek] main image not ready; read skipped\n"); return 1; }
    if (runtime_fault_guard) { fprintf(stderr, "[totk-peek] fault guard already active; read skipped\n"); return 1; }
    unsigned char bytes[512]; sigjmp_buf guard;
    if (sigsetjmp(guard, 1)) {
        runtime_fault_guard = NULL;
        fprintf(stderr, "[totk-peek] unreadable address=%#llx bytes=%u\n", (unsigned long long)address, count);
        return 1;
    }
    runtime_fault_guard = &guard;
    atomic_signal_fence(memory_order_seq_cst);
    memcpy(bytes, (const void *)address, count);
    atomic_signal_fence(memory_order_seq_cst);
    runtime_fault_guard = NULL;
    for (unsigned i = 0; i < count; i += 16) {
        fprintf(stderr, "[totk-peek] %016llx:", (unsigned long long)(address + i));
        for (unsigned j = i; j < count && j < i + 16; j++) fprintf(stderr, " %02x", bytes[j]);
        fputc('\n', stderr);
    }
    return 1;
usage:
    fprintf(stderr, "[totk-peek] usage: peek <0xaddress> <1..512 decimal bytes>\n");
    return 1;
}
int runtime_survey_command(const char *line) {
    if (command_is(line, "peek")) return peek_command(line + 4);
    if (!command_is(line, "gta-base")) return 0;
    if (*skip_space(line + 8)) { fprintf(stderr, "[gta-base] usage: gta-base\n"); return 1; }
    Module *m = active_main_module();
    if (!m) { fprintf(stderr, "[gta-base] main image not ready\n"); return 1; }
    fprintf(stderr, "[gta-base] base=%#llx text_end=%#llx ro_off=%#llx data_off=%#llx size=%#llx\n",
            (unsigned long long)(uintptr_t)m->base, (unsigned long long)m->text_end,
            (unsigned long long)m->ro_off, (unsigned long long)m->data_off, (unsigned long long)m->size);
    return 1;
}
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) {}
