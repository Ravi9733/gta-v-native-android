/* Included after S, NvShader/NvProg, g_pass and uniform snapshot declarations.
   Only passive metadata and proven ready submit-snapshot bytes are copied. */
static gm_capture g_gm;
static _Atomic uint64_t g_gm_request;
static _Atomic int g_gm_phase; /* 0 idle, 1 capturing, 2 sealed: immutable forever */
static gm_draw g_gm_pass,g_gm_draw,g_gm_last;
static uint64_t g_gm_count_frame=UINT64_MAX;
static unsigned g_gm_ubo_n,g_gm_tex_n,g_gm_world_n,g_gm_unavail_n;
static unsigned g_gm_cbuf_n[2][16][3]; /* independent scene/postfx-sized/UI sample quotas */
static int g_gm_last_valid;
static gm_pass_meta g_gm_pass_meta;
static int g_gm_late;
static unsigned g_gm_late_ubo_n,g_gm_late_tex_n;
/* Filters arm a trace tail only; they never establish a verified main/UI role. */
static int gta_map_late_key(const char *key) {
    return key && (!strncmp(key,"c309074ebf781cfef59218a152db14e3933dd5b67b6d8aecab97e190c88cc4c3",64) ||
        !strncmp(key,"9a27db71ee0178f23abe150f1516dfd729533e0453889af77e2e6c991d3622f1",64) ||
        !strncmp(key,"cfb2a6d3399fcd483a9102ca177e3437b152c659f9ae5d69eef8141373429c37",64));
}
_Static_assert(sizeof(S.blend)==sizeof(g_gm_draw.blend),"mapping blend ABI");
static void gta_map_export_async(void);
int mtl_gta_mapping_request(uint64_t first,unsigned frames) {
    if(!frames||frames>3||first>=(UINT64_MAX>>2))return 0;
    uint64_t no=0,request=((first+1)<<2)|frames;
    return atomic_compare_exchange_strong_explicit(&g_gm_request,&no,request,memory_order_release,memory_order_relaxed);
}
const void *mtl_gta_mapping_data(size_t *bytes) {
    if(bytes)*bytes=0;
    if(atomic_load_explicit(&g_gm_phase,memory_order_acquire)!=2)return NULL;
    if(bytes)*bytes=gm_export_size(&g_gm);return &g_gm;
}
static int gta_map_on(void) {
    return atomic_load_explicit(&g_gm_phase,memory_order_acquire)==1 &&
        gm_active(&g_gm,atomic_load_explicit(&g_cur_frame,memory_order_relaxed));
}
static gm_image gta_map_image(id<MTLTexture> t,uint64_t serial,NSUInteger level,NSUInteger slice) {
    gm_image i={0};i.serial=serial;
    if(t){i.rid=t.gpuResourceID._impl;i.parent_rid=t.parentTexture.gpuResourceID._impl;
        i.width=(uint32_t)t.width;i.height=(uint32_t)t.height;i.depth=(uint32_t)t.depth;
        i.format=(uint32_t)t.pixelFormat;i.type=(uint32_t)t.textureType;i.samples=(uint32_t)t.sampleCount;
        i.level=(uint32_t)level;i.slice=(uint32_t)slice;}
    return i;
}
static gm_image gta_map_logical(const tex_t *t) {
    gm_image i={0};if(t){i.serial=t->serial;i.width=t->d.width;i.height=t->d.height;
        i.depth=t->d.depth;i.format=t->d.format;i.type=t->d.target;i.samples=t->d.samples;}
    return i; /* rid=0, format/type are NVN here; marker flags make this explicit. */
}
static void gta_map_pass_note(MTL4RenderPassDescriptor *pd) {
    if(!gta_map_on())return;
    memset(&g_gm_pass,0,sizeof g_gm_pass);memset(&g_gm_pass_meta,0,sizeof g_gm_pass_meta);
    g_gm_pass.frame=atomic_load(&g_cur_frame);g_gm_pass.submission=g_sub_serial;g_gm_pass.pass=g_pass.serial;
    for(int i=0;i<8;i++){MTLRenderPassColorAttachmentDescriptor *a=pd.colorAttachments[i];
        g_gm_pass.color[i]=gta_map_image(a.texture,i<R.rt.n&&R.rt.color[i]?R.rt.color[i]->serial:0,a.level,a.slice);
        g_gm_pass.color_load[i]=(uint8_t)a.loadAction;g_gm_pass.color_store[i]=(uint8_t)a.storeAction;
        g_gm_pass_meta.clear_color[i][0]=a.clearColor.red;g_gm_pass_meta.clear_color[i][1]=a.clearColor.green;
        g_gm_pass_meta.clear_color[i][2]=a.clearColor.blue;g_gm_pass_meta.clear_color[i][3]=a.clearColor.alpha;
        if(a.texture && a.storeAction!=MTLStoreActionUnknown)g_gm_pass_meta.color_store_known|=1u<<i;}
    g_gm_pass.depth=gta_map_image(pd.depthAttachment.texture,R.rt.depth?R.rt.depth->serial:0,pd.depthAttachment.level,pd.depthAttachment.slice);
    g_gm_pass.clear_depth=(float)pd.depthAttachment.clearDepth;g_gm_pass.color_count=(uint32_t)R.rt.n;
    g_gm_pass.depth_load_action=(uint32_t)pd.depthAttachment.loadAction;g_gm_pass.depth_store_action=(uint32_t)pd.depthAttachment.storeAction;
    g_gm_pass.layered=(uint32_t)g_pass.layered;
    g_gm_pass.pass_metadata_valid=1;
    g_gm_pass_meta.color_count=(uint32_t)R.rt.n;
    g_gm_pass_meta.depth_store_known=pd.depthAttachment.texture && pd.depthAttachment.storeAction!=MTLStoreActionUnknown;
    g_gm_pass_meta.stencil_store_known=pd.stencilAttachment.texture && pd.stencilAttachment.storeAction!=MTLStoreActionUnknown;
    g_gm_pass_meta.stencil_rid=pd.stencilAttachment.texture.gpuResourceID._impl;
    g_gm_pass_meta.stencil_load=(uint32_t)pd.stencilAttachment.loadAction;
    g_gm_pass_meta.stencil_store=(uint32_t)pd.stencilAttachment.storeAction;
    g_gm_pass_meta.clear_stencil=(uint32_t)pd.stencilAttachment.clearStencil;
    (void)gm_append(&g_gm,&g_gm_pass,GM_PASS_BEGIN,0,0,GM_NO_BYTES,0,&g_gm_pass_meta,sizeof g_gm_pass_meta);
}
static void gta_map_store_note(int depth,NSUInteger index,MTLStoreAction action) {
    if(!gta_map_on()||g_gm_pass.pass!=g_pass.serial)return;
    if(depth==1){g_gm_pass.depth_store_action=(uint32_t)action;g_gm_pass_meta.depth_store_known=action!=MTLStoreActionUnknown;}
    else if(depth==2){g_gm_pass_meta.stencil_store=(uint32_t)action;g_gm_pass_meta.stencil_store_known=action!=MTLStoreActionUnknown;}
    else if(index<8){g_gm_pass.color_store[index]=(uint8_t)action;
        if(action!=MTLStoreActionUnknown)g_gm_pass_meta.color_store_known|=1u<<index;
        else g_gm_pass_meta.color_store_known&=~(1u<<index);}
}
static void gta_map_pass_end_note(void) {
    if(!gta_map_on()||g_gm_pass.pass!=g_pass.serial)return;
    gm_draw d=g_gm_pass;d.draw=g_stat_draw;
    (void)gm_append(&g_gm,&d,GM_PASS_END,0,0,GM_NO_BYTES,0,&g_gm_pass_meta,sizeof g_gm_pass_meta);
}
static void gta_map_color_clear_note(const p_clear_color *c,MTLScissorRect sc) {
    if(!gta_map_on()||g_gm_pass.pass!=g_pass.serial)return;
    gm_color_clear body={.index=c->index,.mask=c->mask,.scissor={(uint32_t)sc.x,(uint32_t)sc.y,(uint32_t)sc.width,(uint32_t)sc.height}};
    memcpy(body.rgba,c->rgba,sizeof body.rgba);gm_draw d=g_gm_pass;d.draw=g_stat_draw;
    (void)gm_append(&g_gm,&d,GM_COLOR_CLEAR,0,0,GM_NO_BYTES,0,&body,sizeof body);
}
static void gta_map_depth_clear_note(const p_clear_ds *c,MTLScissorRect sc) {
    if(!gta_map_on()||g_gm_pass.pass!=g_pass.serial)return;
    gm_depth_clear body={.depth=c->depth,.depth_mask=c->depth_mask,.stencil=c->stencil,.stencil_mask=c->stencil_mask,
        .scissor={(uint32_t)sc.x,(uint32_t)sc.y,(uint32_t)sc.width,(uint32_t)sc.height}};
    gm_draw d=g_gm_pass;d.draw=g_stat_draw;
    (void)gm_append(&g_gm,&d,GM_DEPTH_CLEAR,0,0,GM_NO_BYTES,0,&body,sizeof body);
}
/* Exact key sets are diagnostics filters only, not a verified HUD/main-pass map. */
#include "nvn_gta_mapping_keys.inc"
static void gta_map_draw_note(NvProg *pr,MTLViewport viewport,MTLScissorRect scissor,uint32_t modifiers) {
    if(!gta_map_on())return;
    if(g_gm_count_frame!=atomic_load(&g_cur_frame)){
        g_gm_count_frame=atomic_load(&g_cur_frame);g_gm_late=0;g_gm_late_ubo_n=g_gm_late_tex_n=0;
        g_gm_ubo_n=g_gm_tex_n=g_gm_world_n=g_gm_unavail_n=0;memset(g_gm_cbuf_n,0,sizeof g_gm_cbuf_n);}
    if(pr->sh[1]&&gta_map_late_key(pr->sh[1]->key))g_gm_late=1;
    g_gm_draw=g_gm_pass;g_gm_draw.frame=atomic_load(&g_cur_frame);
    if(g_gm_pass.pass!=g_pass.serial){memset(g_gm_draw.color,0,sizeof g_gm_draw.color);memset(&g_gm_draw.depth,0,sizeof g_gm_draw.depth);g_gm_draw.color_count=0;g_gm_draw.pass_metadata_valid=0;}
    g_gm_draw.submission=g_sub_serial;g_gm_draw.pass=g_pass.serial;g_gm_draw.draw=g_stat_draw;
    g_gm_draw.modifier_flags=modifiers;
    if(pr->sh[0])memcpy(g_gm_draw.vertex_key,pr->sh[0]->key,GM_KEY);
    if(pr->sh[1])memcpy(g_gm_draw.fragment_key,pr->sh[1]->key,GM_KEY);
    memcpy(g_gm_draw.blend,S.blend,sizeof g_gm_draw.blend);
    g_gm_draw.viewport[0]=(float)viewport.originX;g_gm_draw.viewport[1]=(float)viewport.originY;
    g_gm_draw.viewport[2]=(float)viewport.width;g_gm_draw.viewport[3]=(float)viewport.height;
    g_gm_draw.viewport[4]=(float)viewport.znear;g_gm_draw.viewport[5]=(float)viewport.zfar;
    g_gm_draw.depth_range[0]=S.dr[0];g_gm_draw.depth_range[1]=S.dr[1];
    g_gm_draw.depth_test=S.ds.dt;g_gm_draw.depth_write=S.ds.dw;g_gm_draw.depth_function=S.ds.df;
    g_gm_draw.stencil_test=S.ds.st;g_gm_draw.depth_clamp=(uint32_t)S.depth_clamp;
    g_gm_draw.blend_enable_mask=S.color.blend_en;g_gm_draw.channel_mask=S.chmask.mask;
    g_gm_draw.clip_near_zero=!totk_depth_near_minus_w();
    g_gm_draw.scissor[0]=(int32_t)scissor.x;g_gm_draw.scissor[1]=(int32_t)scissor.y;
    g_gm_draw.scissor[2]=(int32_t)scissor.width;g_gm_draw.scissor[3]=(int32_t)scissor.height;
    gm_draw signature=g_gm_draw;signature.draw=0;
    if(!g_gm_last_valid||memcmp(&g_gm_last,&signature,sizeof signature)){
        unsigned cls=gta_map_key_class(g_gm_draw.vertex_key)|gta_map_key_class(g_gm_draw.fragment_key);
        int priority=(cls&4)||((cls&2)&&!g_gm_draw.depth_test&&g_gm_draw.color_count<=1);
        if(g_gm.count<GM_MAX-256||priority)(void)gm_append(&g_gm,&g_gm_draw,GM_DRAW,0,0,GM_NO_BYTES,0,NULL,0);
        else if(g_gm.truncated!=UINT32_MAX)g_gm.truncated++; /* reserve late HUD/postfx/UBO/present space */
        g_gm_last=signature;g_gm_last_valid=1;
    }
}
static int gta_map_seen(uint32_t kind,uint32_t stage,uint32_t slot,uint64_t id) {
    /* Hard caps keep this diagnostic scan bounded; newest frame only. */
    for(uint32_t n=g_gm.count;n;n--){gm_record *r=&g_gm.records[n-1];
        if(r->state.frame!=g_gm_draw.frame)break;
        if(g_gm_late&&(r->state.pass!=g_gm_draw.pass||r->state.draw!=g_gm_draw.draw))continue;
        if((r->kind==kind||(kind==GM_UBO&&r->kind==GM_UNAVAILABLE))&&r->stage==stage&&r->slot==slot&&r->identity==id&&r->state.submission==g_sub_serial)return 1;}
    return 0;
}
static void gta_map_texture(NvShader *sh,int stage,int slot,id<MTLTexture> t,uint64_t serial) {
    if(!gta_map_on()||stage>1||!t)return;
    if(g_gm_late){if(g_gm_late_tex_n>=256){if(g_gm.truncated!=UINT32_MAX)g_gm.truncated++;return;}}
    else if(g_gm_tex_n>=64||!(gta_map_key_class(sh->key)&6))return;
    uint64_t rid=t.gpuResourceID._impl;if(gta_map_seen(GM_TEXTURE,stage,slot,rid))return;
    gm_image image=gta_map_image(t,serial,t.parentTexture?t.parentRelativeLevel:0,t.parentTexture?t.parentRelativeSlice:0);
    if(gm_append(&g_gm,&g_gm_draw,GM_TEXTURE,(uint32_t)stage,(uint32_t)slot,GM_NO_BYTES,rid,&image,sizeof image)){
        if(g_gm_late)g_gm_late_tex_n++;else g_gm_tex_n++;}
}
static void gta_map_bound_stage(NvShader *sh,int stage,const uint64_t *slots,size_t words) {
    if(!gta_map_on()||stage>1||!slots||!sh)return;
    unsigned cls=gta_map_key_class(sh->key);
    if(g_gm_late&&!gta_map_seen(GM_ARGUMENTS,(uint32_t)stage,0,0)){
        gm_arg_binding body[64];size_t n=0;
        for(int i=0;i<sh->rf.nb&&i<64;i++){const nvn_rbind *b=&sh->rf.b[i];
            if(b->aid>=words){if(g_gm.unavailable!=UINT32_MAX)g_gm.unavailable++;continue;}
            body[n++]=(gm_arg_binding){.value=slots[b->aid],.reg=b->reg,.aid=b->aid,.off=b->off,.kind=b->kind,.rw=b->rw};}
        if(sh->rf.nb>64){if(g_gm.truncated!=UINT32_MAX)g_gm.truncated++;}
        (void)gm_append(&g_gm,&g_gm_draw,GM_ARGUMENTS,(uint32_t)stage,0,GM_NO_BYTES,0,body,n*sizeof body[0]);
    }
    int scene=stage==0 && g_gm_draw.depth_test && g_gm_draw.color_count>=3;
    for(int i=0;i<sh->rf.nb;i++){
        const nvn_rbind *b=&sh->rf.b[i];unsigned reg=b->reg,aid=b->aid;
        if(aid>=words)continue;
        if(b->kind==NVN_RB_TEX && (g_gm_late||(cls&6)) &&
           (g_gm_late?g_gm_late_tex_n<256:g_gm_tex_n<64) && !gta_map_seen(GM_TEXTURE,stage,reg,slots[aid])){
            gm_record *r=gm_append(&g_gm,&g_gm_draw,GM_TEXTURE,stage,reg,GM_NO_BYTES,slots[aid],NULL,0);
            if(r){r->flags=1;if(g_gm_late)g_gm_late_tex_n++;else g_gm_tex_n++;} /* identity known, metadata unavailable on cached path */
        }
        if(b->kind!=NVN_RB_CBUF || reg>=16)continue;
        if(g_gm_late){if(g_gm_late_ubo_n>=256){if(g_gm.truncated!=UINT32_MAX)g_gm.truncated++;continue;}}
        else if(g_gm_ubo_n>=32 || !(reg==1||reg==2||reg==3||reg==5||reg==11))continue;
        if(!g_gm_late && !(cls&6) && !(scene&&reg==1&&g_gm_world_n<6))continue;
        const p_bind_buf *u=&S.ubo[stage][reg];
        if(!u->addr||!u->size||gta_map_seen(GM_UBO,stage,reg,slots[aid]))continue;
        if(u->size>GM_BYTES){if(g_gm_late){
            if(g_gm_unavail_n<8){gm_record *r=gm_append(&g_gm,&g_gm_draw,GM_UNAVAILABLE,stage,reg,GM_NO_BYTES,slots[aid],NULL,0);
                if(r){r->flags=2;g_gm_unavail_n++;if(g_gm.unavailable!=UINT32_MAX)g_gm.unavailable++;}}
            else if(g_gm.truncated!=UINT32_MAX)g_gm.truncated++;}continue;}
        unsigned bucket=(cls&4)?2:u->size>=1488?1:0;
        unsigned limit=reg==1&&stage==0&&bucket==0?6:2;
        if(!g_gm_late&&g_gm_cbuf_n[stage][reg][bucket]>=limit)continue;
        /* Non-waiting authoritative lookup. Never read original pool/guest bytes,
           never materialize pending GPU writes, never change snapshot state. */
        int at=usnap_first(u->addr);const usnap_t *c=at<g_nusnap?&g_usnap[at]:NULL;
        int ready=c && atomic_load_explicit(&c->st,memory_order_acquire)==US_READY && c->cpu &&
            c->lo<=u->addr && u->addr<c->hi && u->size<=c->hi-u->addr &&
            c->gpu<=UINT64_MAX-(u->addr-c->lo) && c->gpu+(u->addr-c->lo)==slots[aid];
        if(!ready&&g_gm_unavail_n>=8){if(g_gm.truncated!=UINT32_MAX)g_gm.truncated++;continue;}
        gm_record *r=gm_append(&g_gm,&g_gm_draw,ready?GM_UBO:GM_UNAVAILABLE,stage,reg,
            ready?GM_READY_SNAPSHOT:GM_NO_BYTES,slots[aid],ready?c->cpu+(u->addr-c->lo):NULL,ready?(size_t)u->size:0);
        if(r){if(ready){if(g_gm_late)g_gm_late_ubo_n++;else g_gm_ubo_n++;g_gm_cbuf_n[stage][reg][bucket]++;if(scene&&reg==1)g_gm_world_n++;}else{g_gm.unavailable++;g_gm_unavail_n++;}}
    }
}
static void gta_map_present_note(const tex_t *t,double period,int discard,const int crop[4]) {
    uint64_t f=atomic_load(&g_cur_frame);
    if(atomic_load_explicit(&g_gm_phase,memory_order_relaxed)==1){
        if(gm_active(&g_gm,f)){
            gm_draw d={0};d.frame=f;d.submission=g_sub_serial;d.draw=g_stat_draw;d.pass=g_pass.serial;
            d.color[0]=gta_map_logical(t);
            gm_present p={.requested_period=period,.monotonic_ns=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
            if(crop)memcpy(p.crop,crop,sizeof p.crop);
            gm_record *r=gm_append(&g_gm,&d,GM_PRESENT,0,0,GM_NO_BYTES,t?t->serial:0,&p,sizeof p);
            if(r)r->flags=2u|(discard?4u:0u); /* logical NVN descriptor, requested period is not measured dt */
        }
        if(f>=g_gm.first_frame && f-g_gm.first_frame>=g_gm.frame_count-1){
            g_gm.sealed=1;atomic_store_explicit(&g_gm_phase,2,memory_order_release);gta_map_export_async();
        }
    }else if(!atomic_load_explicit(&g_gm_phase,memory_order_relaxed)){
        uint64_t request=atomic_load_explicit(&g_gm_request,memory_order_acquire);
        if(request){uint64_t first=(request>>2)-1;unsigned frames=(unsigned)(request&3);
            int clamped=!first||first<=f;if(clamped){if(f==UINT64_MAX)return;first=f+1;}
            gm_init(&g_gm,first,frames);g_gm.request_clamped=(uint32_t)clamped;
            int cap=-1;
            if(@available(macOS 26.0,iOS 26.0,*)){
                if(g_dev&&[MTLFXFrameInterpolatorDescriptor respondsToSelector:@selector(supportsMetal4FX:)])
                    cap=[MTLFXFrameInterpolatorDescriptor supportsMetal4FX:g_dev]?1:0;
            }
            g_gm.metalfx_capability=(uint64_t)(cap+1);
            fprintf(stderr,"[gta-mfx-map] Metal4FX frame interpolation support=%d device=%s (capability only; effect disabled)\n",cap,g_dev?g_dev.name.UTF8String:"unavailable");
            atomic_store_explicit(&g_gm_phase,1,memory_order_release);
        }
    }
}
/* Export is off the graphics path, once, after release-publication of immutable
   payload. Exclusive own file, no retry allocations, no overwrite/follow links. */
static int gta_map_write_fd(int fd,const void *bytes,size_t n) {
    const uint8_t *p=bytes;while(n){ssize_t k=write(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return 0;p+=k;n-=(size_t)k;}return 1;
}
static int gta_map_export_dir(const char *docs,const gm_capture *capture,char *name,size_t name_cap) {
    size_t n=gm_export_size(capture);if(!docs||!n)return 0;
    int dd=open(docs,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dd<0)return 0;
    struct stat st;int ok=!fstat(dd,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==getuid();
    if(ok && mkdirat(dd,"gta-metalfx-mapping",0700)<0 && errno!=EEXIST)ok=0;
    int dir=ok?openat(dd,"gta-metalfx-mapping",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;close(dd);
    if(dir<0)return 0;
    if(fstat(dir,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()){close(dir);return 0;}
    int len=snprintf(name,name_cap,"capture-%d-%llu.bin",(int)getpid(),(unsigned long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    if(len<=0||(size_t)len>=name_cap){close(dir);return 0;}
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);close(dir);if(fd<0)return 0;
    ok=gta_map_write_fd(fd,capture,n);if(close(fd))ok=0;return ok;
}
static void gta_map_export_async(void) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool{
        NSString *docs=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
        char name[128]={0};int ok=gta_map_export_dir(docs.fileSystemRepresentation,&g_gm,name,sizeof name);
        fprintf(stderr,"[gta-mfx-map] sealed records=%u truncated=%u unavailable=%u bytes=%zu export=%s %s\n",
            g_gm.count,g_gm.truncated,g_gm.unavailable,gm_export_size(&g_gm),ok?"ok":"failed",name);
    }});
}
