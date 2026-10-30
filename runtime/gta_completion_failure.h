#pragma once
/* Failure never returns a completed item, clears pending, retries the queue or resumes the guest. */
#ifdef __cplusplus
extern "C" {
#endif
_Noreturn void runtime_gta_overflow_failure(int code);
int runtime_gta_overflow_fault_code(void); /* 0 before failure; first nonzero code thereafter. */
#ifdef __cplusplus
}
#endif
#ifdef __OBJC__
#import <Foundation/Foundation.h>
@class UIWindow;
/* Initialize on main before any guest thread; attach on main after the scene creates its window. */
void gta_completion_failure_initialize(NSString *documentsDirectory);
void gta_completion_failure_attach_window(UIWindow *window);
#endif
