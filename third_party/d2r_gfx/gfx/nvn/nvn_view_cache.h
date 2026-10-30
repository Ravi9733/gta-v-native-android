// Cross-ID sampled texture-view cache, adapted from nxnative beea17cfdbd8fe6112de9d1c191c0488da7970bf.
// Copyright (c) 2026 seedds; MIT, see LICENSE.nxnative (and existing component LICENSE).
// Caller holds the texture registry lock. Own only derived views; never alter residency of their base.
// Unlike upstream's live-object registry, this renderer remembers finalized serials until every descriptor/recording
// releases them. Cache insertion must check that history, and finalize/pool deletion must drop this extra owner.
#pragma once
#ifndef NVN_VC_MAX
#define NVN_VC_MAX 1024u
#endif
#define NVN_VC_WAYS 4u
#ifndef NVN_VC_CALLOC
#define NVN_VC_CALLOC calloc
#endif
_Static_assert(NVN_VC_MAX >= 1 && NVN_VC_MAX <= 1024, "view-cache sets must fit the fixed hash storage");
typedef struct { uint32_t pf, l0, nl, s0, ns; uint8_t tt, sw[4], pad[3]; } vc_key;
typedef struct { const void *pool, *base; unsigned next; vc_key k[NVN_VC_WAYS]; void *view[NVN_VC_WAYS]; } vc_set;
// Fixed hash backing at < 1/2 load: optional cache insertion never reaches nvn_hm_grow's fatal OOM path.
static uint64_t g_vc_keys[2048]; static void *g_vc_vals[2048];
static nvn_hmap g_vc = {.keys=(uint8_t *)g_vc_keys,.vals=g_vc_vals,.klen=8,.cap=2048};
static uint32_t g_vc_hand;
static uint64_t g_vc_hit, g_vc_new, g_vc_invalidated, g_vc_evicted;
static int vc_on(void) { static int on=-1; if(on<0) { const char *e=getenv("GTA_SAMPLE_VIEW_CACHE"); on=!e||strcmp(e,"0"); } return on; }
static void vc_set_clear(vc_set *s) {
    for (unsigned i=0; i<NVN_VC_WAYS; i++) if (s->view[i]) {
        defer_obj_unref((__bridge id)s->view[i]); CFRelease(s->view[i]); s->view[i]=NULL;
    }
    s->base=NULL; s->next=0;
}
static void vc_set_free(void *p) { vc_set_clear(p); free(p); }
static void vc_drop_serial_locked(uint64_t ser) { vc_set *s=nvn_hm_del(&g_vc,&ser); if(s)vc_set_free(s); }
static id<MTLTexture> vc_get(uint64_t ser, const vc_key *k, id<MTLTexture> base) {
    if(!vc_on())return nil;
    vc_set *s=nvn_hm_get(&g_vc,&ser); if(!s)return nil;
    if(s->base!=(__bridge void *)base) { vc_set_clear(s); g_vc_invalidated++; return nil; }
    for(unsigned i=0;i<NVN_VC_WAYS;i++) if(s->view[i]&&!memcmp(&s->k[i],k,sizeof *k)) {
        id<MTLTexture> v=(__bridge id<MTLTexture>)s->view[i];
        if(v.parentTexture==base) {g_vc_hit++;return v;}
        vc_set_clear(s);g_vc_invalidated++;return nil;
    }
    return nil;
}
static void vc_put(uint64_t ser,const pool_t *pool_object,const vc_key *k,id<MTLTexture> base,id<MTLTexture> v) {
    if(!vc_on()||!ser||!v||v==base||v.parentTexture!=base||serial_is_dead(ser))return;
    const void *pool=pool_object?pool_object->mem:NULL; // mtl_pool_del receives backing memory, not the pool_t address.
    vc_set *s=nvn_hm_get(&g_vc,&ser);
    if(!s) {
        // Evict one bounded set, not all entries at once. There are at most 4,096 additional view references.
        if(g_vc.n>=NVN_VC_MAX) {
            while(!g_vc.vals[g_vc_hand%g_vc.cap])g_vc_hand++;
            uint64_t old;memcpy(&old,g_vc.keys+(size_t)(g_vc_hand++%g_vc.cap)*8,8);
            vc_drop_serial_locked(old);g_vc_evicted++;
        }
        s=NVN_VC_CALLOC(1,sizeof *s);if(!s)return;
        s->pool=pool;nvn_hm_put(&g_vc,&ser,s);
    }
    if(s->base!=(__bridge void *)base||s->pool!=pool) {vc_set_clear(s);s->pool=pool;}
    s->base=(__bridge void *)base;
    unsigned i=s->next++%NVN_VC_WAYS;
    if(s->view[i]) {defer_obj_unref((__bridge id)s->view[i]);CFRelease(s->view[i]);}
    s->k[i]=*k;s->view[i]=(void *)CFBridgingRetain(v);g_vc_new++;
}
static void vc_drop_pool(const void *pool) {
    registry_lock();
    for(uint32_t i=0;i<g_vc.cap;) {
        vc_set *s=g_vc.vals[i];
        if(s&&s->pool==pool) {uint64_t ser;memcpy(&ser,g_vc.keys+(size_t)i*8,8);vc_drop_serial_locked(ser);}
        else i++;
    }
    registry_unlock();
}
// g_rmu and g_smu held. A replaced/evicted upload must not be kept alive by an unused cross-ID cache entry.
static void vc_sweep_replaced_locked(void) {
    for(uint32_t i=0;i<g_vc.cap;) {
        vc_set *s=g_vc.vals[i];if(!s){i++;continue;}
        uint64_t ser;memcpy(&ser,g_vc.keys+(size_t)i*8,8);
        __unsafe_unretained id o=hm_get_id(&g_by_serial,&ser);
        id<MTLTexture> current=[o isKindOfClass:[NvPkg class]]?((NvPkg *)o)->tex:([o conformsToProtocol:@protocol(MTLTexture)]?o:nil);
        if(s->base!=(__bridge void *)current) {vc_drop_serial_locked(ser);g_vc_invalidated++;}
        else i++;
    }
}
