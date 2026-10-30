/* nn::ro (ldr:ro) host replacements for the GTA V homebrew port.
   The guest's own ldr:ro registry is never initialised: the public nn::ro API is answered
   by the host against the loader's already-mapped modules. The game's subsdk2/subsdk3
   (video/audio decoders) resolve through these tables.
   All entry points run on the host stack via runtime_hle_wrap_named. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static Module *ro_module_for(uintptr_t image) {
    for (unsigned i = 0; i < runtime_module_count(); i++) {
        Module *m = runtime_module_at(i);
        if (image >= (uintptr_t)m->base && image < (uintptr_t)m->base + m->file_size) return m;
    }
    return NULL;
}

uint64_t ro_initialize(void) { fprintf(stderr, "[totk-ro] Initialize\n"); return 0; }
uint64_t ro_initialize_port(const char *name) { fprintf(stderr, "[totk-ro] InitializeWithPortName(%s)\n", name ? name : "(null)"); return 0; }
uint64_t ro_initialize_runtime(void) { fprintf(stderr, "[totk-ro] InitializeForRuntime\n"); return 0; }
uint64_t ro_finalize(void) { return 0; }
uint64_t ro_bind_entry(void) { fprintf(stderr, "[totk-ro] BindEntry\n"); return 0; }

uint64_t ro_get_buffer_size(uint64_t *out, const void *image) {
    Module *m = ro_module_for((uintptr_t)image);
    fprintf(stderr, "[totk-ro] GetBufferSize(%p) -> %s\n", image, m ? m->name : "unknown");
    if (out) *out = m ? m->file_size : 0;
    return 0;
}

uint64_t ro_load_module(void *out_module, const void *image, void *bss, uint64_t size, uint32_t flag) {
    Module *m = ro_module_for((uintptr_t)image);
    fprintf(stderr, "[totk-ro] LoadModule(image=%p size=%llu bss=%p flag=%u) -> %s\n",
            image, (unsigned long long)size, bss, flag, m ? m->name : "UNKNOWN");
    if (!m) return 0xE0010001;   /* ro::ResultInvalidNroAddress-ish */
    *(void **)out_module = m;    /* cookie: host Module pointer */
    return 0;
}

uint64_t ro_load_module_b(void *out_module, const void *image, void *bss, uint64_t size, uint32_t flag, uint8_t bind) {
    (void)bind;
    return ro_load_module(out_module, image, bss, size, flag);
}

uint64_t ro_unload_module(void *module_cookie) { fprintf(stderr, "[totk-ro] UnloadModule(%p)\n", module_cookie); return 0; }

uint64_t ro_lookup_module_symbol(uint64_t *out, const void *module_cookie, const char *name) {
    Module *m = (Module *)module_cookie;
    if (!m) { fprintf(stderr, "[totk-ro] LookupModuleSymbol(%s): no module cookie\n", name ? name : "(null)"); return 0xE0010001; }
    for (unsigned i = 0; i < m->nsym; i++) {
        const Symbol *s = &m->symbols[i];
        if (s->defined && !strcmp(s->name, name)) {
            uint64_t v = s->value < m->key_off ? (uintptr_t)m->base + s->value : 0;
            fprintf(stderr, "[totk-ro] LookupModuleSymbol(%s, %s) -> %p\n", m->name, name, (void *)(uintptr_t)v);
            if (out) *out = v;
            return 0;
        }
    }
    fprintf(stderr, "[totk-ro] LookupModuleSymbol(%s, %s): NOT FOUND\n", m->name, name);
    return 0xE0010001;
}

uint64_t ro_lookup_symbol(uint64_t *out, const char *name) {
    for (unsigned i = 0; i < runtime_module_count(); i++) {
        Module *m = runtime_module_at(i);
        for (unsigned j = 0; j < m->nsym; j++) {
            const Symbol *s = &m->symbols[j];
            if (s->defined && !strcmp(s->name, name)) {
                uint64_t v = s->value < m->key_off ? (uintptr_t)m->base + s->value : 0;
                fprintf(stderr, "[totk-ro] LookupSymbol(%s) -> %p (%s)\n", name, (void *)(uintptr_t)v, m->name);
                if (out) *out = v;
                return 0;
            }
        }
    }
    fprintf(stderr, "[totk-ro] LookupSymbol(%s): NOT FOUND\n", name);
    return 0xE0010001;
}

uint64_t ro_lookup_next_symbol(uint64_t *out, const char *name) { return ro_lookup_symbol(out, name); }

uint64_t ro_protect_relro(const void *a, const void *b, const void *c, const void *d, const void *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
uint64_t ro_register_module_info(void *info, const void *nro) { (void)info; (void)nro; fprintf(stderr, "[totk-ro] RegisterModuleInfo(%p, %p)\n", info, nro); return 0; }
uint64_t ro_register_module_info_j(void *info, const void *nro, uint32_t j) { (void)info; (void)nro; (void)j; fprintf(stderr, "[totk-ro] RegisterModuleInfo(%p, %p, %u)\n", info, nro, j); return 0; }
uint64_t ro_unregister_module_info(void *info) { (void)info; return 0; }

void *runtime_ro_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2ro10InitializeEv", ro_initialize},
        {"_ZN2nn2ro22InitializeWithPortNameEPKc", ro_initialize_port},
        {"_ZN2nn2ro20InitializeForRuntimeEv", ro_initialize_runtime},
        {"_ZN2nn2ro8FinalizeEv", ro_finalize},
        {"_ZN2nn2ro9BindEntryEv", ro_bind_entry},
        {"_ZN2nn2ro13GetBufferSizeEPmPKv", ro_get_buffer_size},
        {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ro_load_module},
        {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib", ro_load_module_b},
        {"_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ro_unload_module},
        {"_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", ro_lookup_module_symbol},
        {"_ZN2nn2ro12LookupSymbolEPmPKc", ro_lookup_symbol},
        {"_ZN2nn2ro16LookupNextSymbolEPmPKc", ro_lookup_next_symbol},
        {"_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_", ro_protect_relro},
        {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ro_register_module_info},
        {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKvj", ro_register_module_info_j},
        {"_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ro_unregister_module_info},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
