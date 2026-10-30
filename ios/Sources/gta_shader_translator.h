#pragma once
#include <TargetConditionals.h>

#if TARGET_OS_IPHONE || defined(GTA_SHADER_TRANSLATOR_HOST_TEST)
#import <Foundation/Foundation.h>
#ifdef __cplusplus
extern "C" {
#endif

// Availability is true only after framework exports and the native backend
// have been registered. The framework remains loaded for the process lifetime.
int gta_ios_shader_available(void);
NSDictionary *gta_ios_shader_translate(NSDictionary *request, NSString *cacheDir);

#ifdef __cplusplus
}
#endif
#endif
