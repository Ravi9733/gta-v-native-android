#ifndef GTA_SPIRV_CROSS_H
#define GTA_SPIRV_CROSS_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define GTA_SPVC_EXPORT __attribute__((visibility("default")))
#else
#define GTA_SPVC_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = success; -1 = failure. mode 0 = production MSL flags, mode 1 = raw
 * SPIRV-Cross --reflect JSON (not the service's .res.json / _b.json schema).
 * words must name nwords readable native-endian uint32_t values. Maximum input
 * is 16 MiB and ID bound is limited to 1,048,576. out/error must be distinct,
 * nonnull addresses. On success *out is malloc-owned UTF-8/NUL, *error is NULL.
 * On failure *out is NULL and *error is malloc-owned, unless allocation failed.
 * No exception crosses the ABI. Each call owns a separate compiler instance.
 */
GTA_SPVC_EXPORT int gta_spirv_cross(const uint32_t *words, size_t nwords,
                                   int mode, char **out, char **error);
GTA_SPVC_EXPORT void gta_spirv_cross_free(void *memory);

#ifdef __cplusplus
}
#endif
#endif
