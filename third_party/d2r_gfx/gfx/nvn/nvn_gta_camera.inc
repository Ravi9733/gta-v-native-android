/* Include after immutable uniform snapshots and NvShader reflection. No resource
   allocations, waiting, per-frame logging, texture reads/copies or game writes. */
static _Atomic uint64_t g_gc_mode; /* low bit enabled; every setter advances epoch */
static uint64_t g_gc_seen_mode,g_gc_frame=UINT64_MAX,g_gc_pass_frame;
static gta_camera_proof g_gc_proof;
static gta_camera_context g_gc_pass,g_gc_draw;
static unsigned g_gc_scans,g_gc_program_samples[3];
static int g_gc_draw_valid;
void mtl_gta_camera_enable(int enabled) {
    if(enabled!=0&&enabled!=1)return;
    uint64_t prior=atomic_load_explicit(&g_gc_mode,memory_order_relaxed),next;
    do {next=((prior+2)&~UINT64_C(1))|(uint64_t)enabled;}
    while(!atomic_compare_exchange_weak_explicit(&g_gc_mode,&prior,next,memory_order_release,memory_order_relaxed));
}
static int gta_camera_observer_on(void) {return (int)(atomic_load_explicit(&g_gc_mode,memory_order_acquire)&1);}
static void gta_camera_pass_note(MTL4RenderPassDescriptor *pd) {
    if(!gta_camera_observer_on())return;
    memset(&g_gc_pass,0,sizeof g_gc_pass);g_gc_pass_frame=atomic_load(&g_cur_frame);
    g_gc_pass.frame=g_gc_pass_frame;g_gc_pass.pass=g_pass.serial;g_gc_pass.color_count=(uint32_t)R.rt.n;
    MTLRenderPassDepthAttachmentDescriptor *a=pd.depthAttachment;id<MTLTexture> t=a.texture;
    if(t&&t.textureType==MTLTextureType2D&&t.storageMode!=MTLStorageModeMemoryless){
        g_gc_pass.depth_rid=t.gpuResourceID._impl;g_gc_pass.depth_format=(uint32_t)t.pixelFormat;
        g_gc_pass.width=(uint32_t)t.width;g_gc_pass.height=(uint32_t)t.height;g_gc_pass.depth_samples=(uint32_t)t.sampleCount;
        g_gc_pass.depth_level=(uint32_t)a.level;g_gc_pass.depth_slice=(uint32_t)a.slice;
    }
}
static void gta_camera_draw_note(NvProg *pr,MTLViewport viewport,uint32_t modifiers) {
    uint64_t mode=atomic_load_explicit(&g_gc_mode,memory_order_acquire);if(!(mode&1))return;
    uint64_t frame=atomic_load(&g_cur_frame);
    if(g_gc_seen_mode!=mode||g_gc_frame!=frame){
        memset(&g_gc_proof,0,sizeof g_gc_proof);g_gc_scans=0;memset(g_gc_program_samples,0,sizeof g_gc_program_samples);
        g_gc_frame=frame;g_gc_seen_mode=mode;
    }
    g_gc_draw_valid=0;
    if(!pr||!pr->sh[0]||!gta_camera_program(pr->sh[0]->key)||g_gc_pass_frame!=frame||g_gc_pass.pass!=g_pass.serial)return;
    g_gc_draw=g_gc_pass;
    g_gc_draw.viewport[0]=viewport.originX;g_gc_draw.viewport[1]=viewport.originY;
    g_gc_draw.viewport[2]=viewport.width;g_gc_draw.viewport[3]=viewport.height;g_gc_draw.viewport[4]=viewport.znear;g_gc_draw.viewport[5]=viewport.zfar;
    g_gc_draw.depth_test=S.ds.dt;g_gc_draw.depth_write=S.ds.dw;g_gc_draw.depth_function=S.ds.df;
    g_gc_draw.clip_near_zero=!totk_depth_near_minus_w();g_gc_draw.modifiers=modifiers;
    g_gc_draw_valid=gta_camera_context_ok(&g_gc_draw);
}
static void gta_camera_bound_stage(NvShader *sh,int stage,const uint64_t *slots,size_t words) {
    if(!gta_camera_observer_on()||stage!=0||!g_gc_draw_valid||!sh||!slots||g_gc_proof.invalid||
       g_gc_proof.samples>=GTA_CAMERA_SAMPLES||g_gc_scans>=64)return;
    int program=gta_camera_program(sh->key);if(!program||g_gc_program_samples[program-1]>=2)return;
    if(g_gc_seen_mode!=atomic_load_explicit(&g_gc_mode,memory_order_acquire)||g_gc_draw.frame!=atomic_load(&g_cur_frame))return;
    for(int i=0;i<sh->rf.nb;i++){
        const nvn_rbind *b=&sh->rf.b[i];if(b->kind!=NVN_RB_CBUF||b->reg!=1||b->aid>=words)continue;
        g_gc_scans++;
        const p_bind_buf *u=&S.ubo[0][1];if(!u->addr||u->size!=256)return;
        int at=usnap_first(u->addr);const usnap_t *c=at>=0&&at<g_nusnap?&g_usnap[at]:NULL;
        // Exact US_READY snapshot and exact GPU argument address; no original
        // guest/pool fallback and no wait/materialization of pending GPU writes.
        if(!c||atomic_load_explicit(&c->st,memory_order_acquire)!=US_READY||!c->cpu||c->lo>u->addr||u->addr>=c->hi||
           u->size>c->hi-u->addr||c->gpu>UINT64_MAX-(u->addr-c->lo)||c->gpu+(u->addr-c->lo)!=slots[b->aid])return;
        const uint8_t *bytes=c->cpu+(u->addr-c->lo);
        uint64_t world=UINT64_C(1469598103934665603);for(unsigned j=0;j<64;j++)world=(world^bytes[j])*UINT64_C(1099511628211);
        for(unsigned j=0;j<g_gc_proof.samples;j++)if(g_gc_proof.world_hashes[j]==world)return;
        if(gta_camera_add(&g_gc_proof,&g_gc_draw,sh->key,bytes,256))g_gc_program_samples[program-1]++;
        return;
    }
}
int mtl_gta_camera_copy(uint64_t frame,gta_camera *camera,gta_camera_context *context) {
    uint64_t mode=atomic_load_explicit(&g_gc_mode,memory_order_acquire);
    if(!(mode&1)||mode!=g_gc_seen_mode||frame!=g_gc_frame||frame!=atomic_load(&g_cur_frame)||
       !gta_camera_proven(&g_gc_proof)||!camera||!context)return 0;
    *camera=g_gc_proof.camera;*context=g_gc_proof.context;return 1;
}
