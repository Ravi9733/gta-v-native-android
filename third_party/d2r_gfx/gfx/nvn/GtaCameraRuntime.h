#ifndef GTA_CAMERA_RUNTIME_H
#define GTA_CAMERA_RUNTIME_H
#include "GtaCamera.h"
/* Setter is thread-safe and default OFF. Copy is graphics-owner-thread ONLY.
   Output is camera/pass provenance, never a verified full-frame FX descriptor. */
void mtl_gta_camera_enable(int enabled);
int mtl_gta_camera_copy(uint64_t frame,gta_camera *camera,gta_camera_context *context);
#endif
