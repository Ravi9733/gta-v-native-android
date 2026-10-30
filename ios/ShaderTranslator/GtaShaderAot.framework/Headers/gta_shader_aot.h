#ifndef GTA_SHADER_AOT_H
#define GTA_SHADER_AOT_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*gta_cross_fn)(const uint32_t*, size_t, int, char**, char**);
typedef void (*gta_cross_free_fn)(void*);
int gta_shader_set_cross_backend(gta_cross_fn, gta_cross_free_fn);
int gta_shader_translate_json(const uint8_t *request, size_t request_length,
                             const uint8_t *code, size_t code_length, int debug_mode,
                             uint8_t **owned_output, size_t *output_length);
void gta_shader_free(void*);
#ifdef __cplusplus
}
#endif
#endif
