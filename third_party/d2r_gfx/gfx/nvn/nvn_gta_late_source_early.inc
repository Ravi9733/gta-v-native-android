/* No resource contents/GPU work. Enabled once at startup, before native PSOs. */
static _Atomic int g_ls_enabled,g_ls_request,g_ls_phase;
static _Atomic uint64_t g_ls_layout_used,g_ls_layout_missed;
static _Atomic unsigned g_ls_layout_count;static _Atomic uint64_t g_ls_layout_slots[65536];
static _Alignas(8) uint8_t g_ls_layout_blob[LS_LAYOUT_BUDGET];
static ls_capture g_ls_capture;static char g_ls_association_key;
static void gta_ls_init(void){
 static dispatch_once_t once;dispatch_once(&once,^{
 const char*e=getenv("GTA_LATE_SOURCE_CAPTURE");if(!e||strcmp(e,"1"))return;
 memset(g_ls_layout_blob,0,sizeof g_ls_layout_blob);memset(&g_ls_capture,0,sizeof g_ls_capture);
 atomic_store_explicit(&g_ls_enabled,1,memory_order_release);
 });
}
static void gta_ls_request(void){if(atomic_load_explicit(&g_ls_enabled,memory_order_acquire)){int no=0;(void)atomic_compare_exchange_strong(&g_ls_request,&no,1);}}
static void gta_ls_pso_created(id object,MTLRenderPipelineDescriptor*d){
 if(!atomic_load_explicit(&g_ls_enabled,memory_order_acquire)||!object||!d)return;
 ls_layout layout={0};MTLVertexDescriptor*v=d.vertexDescriptor;
 if(v)for(unsigned i=0;i<31;i++){MTLVertexAttributeDescriptor*a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;if(a.bufferIndex>=31){layout.flags|=1;continue;}
  MTLVertexBufferLayoutDescriptor*l=v.layouts[a.bufferIndex];layout.attributes[layout.count++]=(ls_attribute){a.offset,l.stride,l.stepRate,i,(uint32_t)a.format,(uint32_t)a.bufferIndex,(uint32_t)l.stepFunction};}
 unsigned slot=atomic_load_explicit(&g_ls_layout_count,memory_order_relaxed);
 do{if(slot>=65536){atomic_fetch_add(&g_ls_layout_missed,1);return;}}while(!atomic_compare_exchange_weak_explicit(&g_ls_layout_count,&slot,slot+1,memory_order_relaxed,memory_order_relaxed));
 size_t bytes=offsetof(ls_layout,attributes)+(size_t)layout.count*sizeof(ls_attribute);uint64_t old=atomic_load_explicit(&g_ls_layout_used,memory_order_relaxed);
 do{if(old>LS_LAYOUT_BUDGET||bytes>LS_LAYOUT_BUDGET-old){atomic_fetch_add(&g_ls_layout_missed,1);return;}}
 while(!atomic_compare_exchange_weak_explicit(&g_ls_layout_used,&old,old+bytes,memory_order_acq_rel,memory_order_relaxed));
 memcpy(g_ls_layout_blob+old,&layout,bytes);
 atomic_store_explicit(&g_ls_layout_slots[slot],old+1,memory_order_release);
 /* Association follows the exact native object's lifetime: pointer reuse cannot
    resurrect an old descriptor. It stores only an offset into preallocated data. */
 objc_setAssociatedObject(object,&g_ls_association_key,@(slot+1),OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static int gta_ls_layout(id object,ls_layout*out){
 if(!object||!out)return 0;NSNumber*at=objc_getAssociatedObject(object,&g_ls_association_key);if(!at)return 0;uint64_t slot=at.unsignedLongLongValue-1;if(slot>=65536)return 0;uint64_t published=atomic_load_explicit(&g_ls_layout_slots[slot],memory_order_acquire);if(!published)return 0;uint64_t off=published-1;
 uint64_t used=atomic_load_explicit(&g_ls_layout_used,memory_order_relaxed);if(off>used||used-off<8||off>LS_LAYOUT_BUDGET-8)return 0;
 uint32_t n;memcpy(&n,g_ls_layout_blob+off,4);size_t bytes=8+(size_t)n*sizeof(ls_attribute);if(n>31||bytes>used-off||bytes>LS_LAYOUT_BUDGET-off)return 0;
 memset(out,0,sizeof *out);memcpy(out,g_ls_layout_blob+off,bytes);return 1;
}
