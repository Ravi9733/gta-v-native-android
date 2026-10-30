#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || defined(GTA_COMPLETION_OVERFLOW_TESTING)
#include "completion_overflow_bridge.h"
#include "gta_completion_failure.h"
#include "completion_overflow_sites.h"
#include <string.h>

extern uint32_t runtime_thread_current_handle(void);
static GtaCompletionOverflow state;
static uintptr_t module_base;
static int bound;

int runtime_gta_overflow_bind(unsigned char *base,const char *image_sha,size_t cap,
                             GtaOverflowAlloc alloc,GtaOverflowFree dealloc,void *context) {
 if(bound||!base||!image_sha||strcmp(image_sha,GTA_OVERFLOW_IMAGE_SHA256))return GTA_OVERFLOW_INVALID;
 int result=gta_overflow_init(&state,(GtaCompletionRing *)(base+0x3de0aa0),cap,alloc,dealloc,context);
 if(result==GTA_OVERFLOW_OK){module_base=(uintptr_t)base;bound=1;}
 return result;
}
int runtime_gta_overflow_gate(uint64_t ret,uint64_t *regs) {
 if(!bound||!regs)return 0;
 uint64_t off=ret-module_base;
 if(off==GTA_OVERFLOW_FULL_RET) {
  int fault=runtime_gta_overflow_fault_code();if(fault)runtime_gta_overflow_failure(fault);
  int result=gta_overflow_append_completed(&state,(uintptr_t)regs[8]);
  if(result!=GTA_OVERFLOW_OK)runtime_gta_overflow_failure(result);
  return 1;
 }
 if(off==GTA_OVERFLOW_POP_FAST_RET||off==GTA_OVERFLOW_POP_WAITING_RET||
    off==GTA_OVERFLOW_POP_SIGNALED_RET) {
  int fault=runtime_gta_overflow_fault_code();if(fault)runtime_gta_overflow_failure(fault);
  uint32_t owner=runtime_thread_current_handle();
  uintptr_t payload=0;int result=gta_overflow_try_pop(&state,owner,&payload);
  if(result!=GTA_OVERFLOW_ITEM&&result!=GTA_OVERFLOW_EMPTY)runtime_gta_overflow_failure(result);
  regs[19]=payload;return 1;
 }
 return 0;
}
int runtime_gta_overflow_shutdown(void) {
 if(!bound)return GTA_OVERFLOW_INVALID;
 int result=gta_overflow_destroy(&state);
 if(result==GTA_OVERFLOW_OK){module_base=0;bound=0;}
 return result;
}
const GtaCompletionOverflow *runtime_gta_overflow_state(void){return bound?&state:NULL;}

#endif
