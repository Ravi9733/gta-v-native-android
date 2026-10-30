/* Triangle-fan assembly adapted from nxnative beea17cf, nvn_primitive.h and
   fan_ib_make. Copyright (c) 2026 seedds, MIT; see LICENSE.nxnative. */
#pragma once
#include <stdint.h>
#include <stddef.h>
static inline uint64_t nvn_fan_index_count(uint32_t vertices) {
    return vertices < 3 ? 0 : ((uint64_t)vertices - 2) * 3;
}
static inline void nvn_fan_fill(uint32_t *out, uint32_t vertices) {
    if (vertices < 3) return;
    for (uint32_t i = 0; i < vertices - 2; i++) {
        out[3 * (size_t)i] = 0; out[3 * (size_t)i + 1] = i + 1; out[3 * (size_t)i + 2] = i + 2;
    }
}
