#pragma once
#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Call once before the guest starts. Only our fixed diagnostics subdirectory is used. */
void gta_unsupported_shaders_initialize(NSString *documentsDirectory);
/* Snapshot: {serial: NSNumber, stages: [{stage: NSNumber 0..5, base: 64 lowercase hex}]}.
   Requires a known unsupported stage (2, 3 or 4). No shader bytes or addresses are saved. */
void runtime_shader_unsupported_program(NSDictionary *snapshot);
/* Immutable snapshot of a previous launch's unacknowledged batch; never current-run discoveries. */
NSDictionary *gta_unsupported_shaders_startup_notice(void);
void gta_unsupported_shaders_acknowledge(NSString *batch);
#ifdef __cplusplus
}
#endif
