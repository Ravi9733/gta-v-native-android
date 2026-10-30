/* Optional N4 pixel diagnostic. Included after camera observer, US snapshots
   and shader types. No guest writes, substitutions, frame generation or waits
   for GPU completion. All object state below belongs to the graphics owner;
   control/finished are the only cross-thread publications. */
#if GTA_SOURCE_PROBE_ENABLE
static _Atomic unsigned g_sp_control,g_sp_cancel,g_sp_finished;
static _Atomic uint64_t g_sp_generation;
static pthread_mutex_t g_sp_request_mu=PTHREAD_MUTEX_INITIALIZER;
static NSURL *g_sp_root;
static uint64_t g_sp_frame,g_sp_pass,g_sp_last_ds_pass,g_sp_final_pass;
static uint64_t g_sp_snapshot_ds_store_pass;
static int g_sp_camera_was_on,g_sp_error;
static GtaSourceProbe *g_sp_capture;
static MTL4RenderPassDescriptor *g_sp_pd;
static id<MTLTexture>g_sp_main_ds,g_sp_last_ds,g_sp_scene,g_sp_final;
static unsigned g_sp_cs,g_sp_ds,g_sp_ss,g_sp_last_ds_stored,g_sp_final_stored;
static gta_source_selection g_sp_selection;
static gta_camera g_sp_camera;
static gta_camera_context g_sp_camera_context;
static gta_source_draw g_sp_draw;
static id<MTL4CommandBuffer>g_sp_options_cb;
static TotkCommitOptions *g_sp_options;
static unsigned g_sp_options_roles;
static _Atomic unsigned g_sp_mismatch_roles_settled;
static id<MTL4CommandAllocator>g_sp_final_allocator;
static id<MTL4CommandBuffer>g_sp_final_cb;
static _Atomic unsigned g_sp_final_done;
static dispatch_queue_t gta_sp_prepare_queue(void){
    static dispatch_queue_t q;static dispatch_once_t once;
    dispatch_once(&once,^{q=dispatch_queue_create("gta.source-probe.prepare",DISPATCH_QUEUE_SERIAL);});return q;
}

static gta_source_image gta_sp_image(id<MTLTexture>t,NSUInteger level,NSUInteger slice){
    if(!t)return(gta_source_image){0};
    return(gta_source_image){t.gpuResourceID._impl,(uint32_t)t.width,(uint32_t)t.height,(uint32_t)t.pixelFormat,
        (uint32_t)t.textureType,(uint32_t)t.sampleCount,(uint32_t)level,(uint32_t)slice};
}
static id gta_sp_source_lease(id<MTLTexture>t){
    /* Core admits private, non-view native resources. Retain Metal backing
       owners too; source heap alias/reuse stays ordered after these GPU reads. */
    if(!t||t.parentTexture||t.storageMode!=MTLStorageModePrivate)return nil;
    NSMutableArray *a=[NSMutableArray arrayWithObject:t];if(t.heap)[a addObject:t.heap];if(t.buffer)[a addObject:t.buffer];return a;
}
static void gta_sp_restore_camera(void){if(!g_sp_camera_was_on)mtl_gta_camera_enable(0);}
static void gta_sp_abandon(int code){
    if(atomic_load(&g_sp_control)!=3)return;
    g_sp_error=code;gta_sp_restore_camera();atomic_store(&g_sp_control,4);
    uint64_t frame=g_sp_frame;dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        fprintf(stderr,"[gta-source-probe] abandoned frame=%llu reason=%d (original rendering continues)\n",(unsigned long long)frame,code);
    });
}
static void gta_sp_retire_if_done(void){
    if(atomic_load(&g_sp_control)!=4||g_sp_options)return;
    if(g_sp_final_cb&&!atomic_load_explicit(&g_sp_final_done,memory_order_acquire))return;
    unsigned roles=(unsigned)__builtin_popcount((unsigned)g_sp_capture.encodedRoleMask);
    if(g_sp_capture.feedbackRoleCount+atomic_load_explicit(&g_sp_mismatch_roles_settled,memory_order_acquire)!=roles)return;
    if(!g_sp_error&&!atomic_load_explicit(&g_sp_finished,memory_order_acquire))return;
    g_sp_capture=nil;g_sp_pd=nil;g_sp_main_ds=nil;g_sp_last_ds=nil;g_sp_scene=nil;g_sp_final=nil;
    g_sp_final_cb=nil;g_sp_final_allocator=nil;g_sp_root=nil;atomic_store(&g_sp_control,0);
}
static void gta_sp_request(int value){
    if(value==0){pthread_mutex_lock(&g_sp_request_mu);atomic_store(&g_sp_cancel,1);atomic_fetch_add(&g_sp_generation,1);
        unsigned c=atomic_load(&g_sp_control);if(c==1||c==2)atomic_store(&g_sp_control,0);pthread_mutex_unlock(&g_sp_request_mu);return;}
    if(value!=1||nogpu())return;pthread_mutex_lock(&g_sp_request_mu);
    if(atomic_load(&g_sp_control)!=0){pthread_mutex_unlock(&g_sp_request_mu);return;}
    atomic_store(&g_sp_control,1);uint64_t generation=atomic_fetch_add(&g_sp_generation,1)+1;atomic_store(&g_sp_cancel,0);
    pthread_mutex_unlock(&g_sp_request_mu);
    dispatch_async(gta_sp_prepare_queue(),^{@autoreleasepool{
        NSString *docs=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
        NSURL *root=docs?[NSURL fileURLWithPath:[docs stringByAppendingPathComponent:@"gta-source-probe"]]:nil;
        NSError *error=nil;BOOL ok=root&&[[NSFileManager defaultManager]createDirectoryAtURL:root withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700}error:&error];
        /* Generation check and publication are one short host-only section;
           cancellation/rearm cannot let an obsolete worker claim a new request. */
        pthread_mutex_lock(&g_sp_request_mu);
        if(atomic_load(&g_sp_generation)!=generation||atomic_load(&g_sp_control)!=1){pthread_mutex_unlock(&g_sp_request_mu);return;}
        if(!ok){atomic_store(&g_sp_control,0);pthread_mutex_unlock(&g_sp_request_mu);fprintf(stderr,"[gta-source-probe] output preparation failed\n");return;}
        g_sp_root=root;atomic_store_explicit(&g_sp_control,2,memory_order_release);pthread_mutex_unlock(&g_sp_request_mu);
    }});
}
static int gta_sp_active(void){
    return atomic_load_explicit(&g_sp_control,memory_order_acquire)==3&&
        !atomic_load(&g_sp_cancel)&&atomic_load(&g_cur_frame)==g_sp_frame;
}
static TotkCommitOptions *gta_sp_capture_options(void){
    if(!on_sub_thread()||!g_cb)return nil;
    if(g_sp_options_cb&&g_sp_options_cb!=g_cb){gta_sp_abandon(10);return nil;}
    if(!g_sp_options){g_sp_options=[TotkCommitOptions new];g_sp_options_cb=g_cb;g_sp_options_roles=0;}return g_sp_options;
}
static TotkCommitOptions *gta_sp_take_options(id<MTL4CommandBuffer>cb){
    unsigned control=atomic_load_explicit(&g_sp_control,memory_order_relaxed);
    if(control!=3&&control!=4)return[TotkCommitOptions new];
    if(!on_sub_thread())return[TotkCommitOptions new];
    return g_sp_options_cb==cb&&g_sp_options?g_sp_options:[TotkCommitOptions new];
}
static void gta_sp_commit_check(id<MTL4CommandBuffer>cb,TotkCommitOptions *options){
    unsigned control=atomic_load_explicit(&g_sp_control,memory_order_relaxed);
    if(control!=3&&control!=4)return;
    /* q4_commit also belongs to presenter/keepalive threads. Never inspect
       graphics-owned ObjC strong fields on those callers. */
    if(!on_sub_thread())return;
    if(g_sp_options_cb!=cb)return;
    if(options!=g_sp_options){
        /* Fail closed for publication, keep pending resources alive through the
           ACTUAL commit callback. Do not invoke uncommitted fake callbacks. */
        TotkCommitOptions *hold=g_sp_options;
        unsigned settledRoles=(unsigned)__builtin_popcount(g_sp_options_roles);
        [options addFeedbackHandler:^(id<MTL4CommitFeedback>f){(void)f;(void)hold->handlers;
            atomic_fetch_add_explicit(&g_sp_mismatch_roles_settled,settledRoles,memory_order_release);
        }];gta_sp_abandon(11);
    }
    g_sp_options=nil;g_sp_options_cb=nil;g_sp_options_roles=0;
}
static void gta_sp_pass_note(MTL4RenderPassDescriptor *pd){
    if(!gta_sp_active())return;
    g_sp_pd=pd;g_sp_pass=g_pass.serial;g_sp_cs=(unsigned)pd.colorAttachments[0].storeAction;
    g_sp_ds=(unsigned)pd.depthAttachment.storeAction;g_sp_ss=(unsigned)pd.stencilAttachment.storeAction;
    memset(&g_sp_draw,0,sizeof g_sp_draw);
}
static void gta_sp_store_note(int depth,NSUInteger index,MTLStoreAction action){
    if(!gta_sp_active()||g_sp_pass!=g_pass.serial)return;
    if(depth==1)g_sp_ds=(unsigned)action;else if(depth==2)g_sp_ss=(unsigned)action;else if(!index)g_sp_cs=(unsigned)action;
}
static void gta_sp_draw_note(NvProg *pr,MTLViewport vp,MTLScissorRect sc,uint32_t modifiers){
    if(!gta_sp_active()||!pr||!pr->sh[1]||g_sp_pass!=g_pass.serial)return;
    g_sp_draw=(gta_source_draw){.frame=g_sp_frame,.pass=g_sp_pass,.draw=g_stat_draw,.ps=pr->sh[1]->key,
      .color=gta_sp_image(g_sp_pd.colorAttachments[0].texture,g_sp_pd.colorAttachments[0].level,g_sp_pd.colorAttachments[0].slice),
      .color_count=(uint32_t)R.rt.n,.modifiers=modifiers,.blend_mask=S.color.blend_en,.channel_mask=S.chmask.mask,
      .depth_test=S.ds.dt,.depth_write=S.ds.dw,.viewport={vp.originX,vp.originY,vp.width,vp.height,vp.znear,vp.zfar},
      .scissor={(uint32_t)sc.x,(uint32_t)sc.y,(uint32_t)sc.width,(uint32_t)sc.height}};
}
static void gta_sp_bound_stage(NvShader *sh,int stage,const uint64_t *slots,size_t words){
    if(!gta_sp_active()||stage!=1||!sh||!slots||!g_sp_selection.frame||!gta_source_role(sh->key))return;
    if(g_sp_draw.frame!=g_sp_frame||g_sp_draw.pass!=g_pass.serial||!g_sp_draw.ps||strcmp(g_sp_draw.ps,sh->key)){
        gta_sp_abandon(12);return;}
    for(int i=0;i<sh->rf.nb;i++){const nvn_rbind *b=&sh->rf.b[i];if(b->kind!=NVN_RB_TEX||b->reg>=16)continue;
        if(b->aid>=words){gta_sp_abandon(13);return;}g_sp_draw.texture[b->reg]=slots[b->aid];g_sp_draw.texture_mask|=1u<<b->reg;}
    gta_source_event event=gta_source_selection_draw(&g_sp_selection,&g_sp_draw);
    if(g_sp_selection.invalid){gta_sp_abandon(14);return;}
    if(event==GTA_SOURCE_LENS_CANDIDATE)g_sp_final=g_sp_pd.colorAttachments[0].texture;
}
static BOOL gta_sp_copy(GtaSourceProbeRole role,id<MTLTexture>texture,uint64_t pass){
    if(nogpu())return NO;
    TotkCommitOptions *options=gta_sp_capture_options();id lease=gta_sp_source_lease(texture);
    BOOL copied=options&&lease&&[g_sp_capture captureRole:role texture:texture sourceLease:lease frame:g_sp_frame pass:pass
        producerStore:YES closedEncoders:YES commandBuffer:g_cb queue:g_q4 commitOptions:options];
    if(copied)g_sp_options_roles|=1u<<role;return copied;
}
static void gta_sp_pass_end(void){
    if(!gta_sp_active()||g_sp_pass!=g_pass.serial||!g_sp_pd)return;
    /* Hook runs only after end-render was queued and g_enc became nil. Direct
       copy is inserted only for selected events, after draining CPU encode FIFO. */
    id<MTLTexture>depth=g_sp_pd.depthAttachment.texture;
    if(depth){g_sp_last_ds=depth;g_sp_last_ds_pass=g_sp_pass;
        g_sp_last_ds_stored=g_sp_ds==1&&g_sp_ss==1&&g_sp_pd.stencilAttachment.texture==depth;}
    if(!g_sp_selection.frame){
        gta_camera camera;gta_camera_context context;
        if(mtl_gta_camera_copy(g_sp_frame,&camera,&context)&&context.pass==g_sp_pass&&depth&&
           context.depth_rid==depth.gpuResourceID._impl&&g_sp_last_ds_stored){
            unsigned nativeColors=0;for(unsigned i=0;i<8;i++)if(g_sp_pd.colorAttachments[i].texture)nativeColors++;
            if(nativeColors!=context.color_count){gta_sp_abandon(25);return;}
            if(!gta_source_selection_begin(&g_sp_selection,&context)){gta_sp_abandon(15);return;}
            g_sp_camera=camera;g_sp_camera_context=context;g_sp_main_ds=depth;
            blit_close();rs_sync();if(!gta_sp_copy(GtaProbeEarlyOpaqueDS,depth,g_sp_pass))gta_sp_abandon(16);
        }
        return;
    }
    gta_source_event event=gta_source_selection_pass_end(&g_sp_selection,g_sp_frame,g_sp_pass,g_sp_cs!=4,g_sp_cs);
    if(g_sp_selection.invalid){gta_sp_abandon(17);return;}
    if(event==GTA_SOURCE_SCENE_CANDIDATE){
        id<MTLTexture>scene=g_sp_pd.colorAttachments[0].texture;
        if(!g_sp_last_ds_stored||g_sp_last_ds!=g_sp_main_ds||g_sp_last_ds_pass<g_sp_camera_context.pass||
           scene.gpuResourceID._impl!=g_sp_selection.scene.rid){gta_sp_abandon(18);return;}
        blit_close();rs_sync();
        if(!gta_sp_copy(GtaProbeLatestDS,g_sp_main_ds,g_sp_pass)||!gta_sp_copy(GtaProbePreLensColor,scene,g_sp_pass)){gta_sp_abandon(19);return;}
        g_sp_scene=scene;g_sp_snapshot_ds_store_pass=g_sp_last_ds_pass;
    }
    if(g_sp_final&&g_sp_pd.colorAttachments[0].texture==g_sp_final){g_sp_final_pass=g_sp_pass;g_sp_final_stored=g_sp_cs==1;}
}
static void gta_sp_present_note(const tex_t *logical,int discard){
    if(atomic_load_explicit(&g_sp_control,memory_order_relaxed)<2)return;
    if(!on_sub_thread())return;
    gta_sp_retire_if_done();uint64_t frame=atomic_load(&g_cur_frame);unsigned control=atomic_load_explicit(&g_sp_control,memory_order_acquire);
    if(control==3){
        if(nogpu()){gta_sp_abandon(26);return;}
        if(atomic_load(&g_sp_cancel)||frame!=g_sp_frame||discard||!logical||g_cb||g_enc||g_blit){gta_sp_abandon(20);return;}
        id<MTLTexture>actual=tex_mtl(logical);gta_source_event event=gta_source_selection_present(&g_sp_selection,frame,gta_sp_image(actual,0,0));
        if(event!=GTA_SOURCE_FINAL_CANDIDATE||!g_sp_scene||g_sp_final!=actual||!g_sp_final_stored||!g_sp_final_pass){gta_sp_abandon(21);return;}
        g_sp_final_allocator=[g_dev newCommandAllocator];g_sp_final_cb=[g_dev newCommandBuffer];
        if(!g_sp_final_allocator||!g_sp_final_cb){gta_sp_abandon(22);return;}
        [g_sp_final_cb beginCommandBufferWithAllocator:g_sp_final_allocator];TotkCommitOptions *options=[TotkCommitOptions new];
        [options addFeedbackHandler:^(id<MTL4CommitFeedback>f){(void)f;atomic_store_explicit(&g_sp_final_done,1,memory_order_release);}];
        BOOL ok=[g_sp_capture captureRole:GtaProbeFinalColor texture:actual sourceLease:gta_sp_source_lease(actual)frame:frame pass:g_sp_final_pass
          producerStore:YES closedEncoders:YES commandBuffer:g_sp_final_cb queue:g_q4 commitOptions:options];
        [g_sp_final_cb endCommandBuffer];q4_commit(g_sp_final_cb,options);
        if(!ok){gta_sp_abandon(23);return;}
        NSMutableArray *vp=[NSMutableArray array],*inv=[NSMutableArray array];for(unsigned i=0;i<16;i++){[vp addObject:@(g_sp_camera.vp[i])];[inv addObject:@(g_sp_camera.view_inverse[i])];}
        NSDictionary *metadata=@{@"cameraVP":vp,@"cameraViewInverse":inv,@"cameraReversed":@(g_sp_camera.reversed),
          @"cameraNear":@(g_sp_camera.near_plane),@"cameraFar":@(g_sp_camera.far_plane),@"opaquePass":@(g_sp_camera_context.pass),
          @"sceneRID":@(g_sp_selection.scene.rid),@"finalRID":@(g_sp_selection.final.rid),@"latestDSLastStorePass":@(g_sp_snapshot_ds_store_pass),
          @"registryGeneration":@(g_reg_gen),@"ownedGeneration":@(g_owned_gen),@"serialGeneration":@(g_ser_gen),
          @"native252Conversion":@NO,@"earlyReadbackIsFloat32DepthPlane":@YES,@"frameGenerationEnabled":@NO};
        if(![g_sp_capture sealWithMetadata:metadata]){gta_sp_abandon(24);return;}
        gta_sp_restore_camera();atomic_store(&g_sp_control,4);return;
    }
    if(control!=2||atomic_load(&g_sp_cancel)||frame==UINT64_MAX)return;
    if(nogpu()){atomic_store(&g_sp_control,0);return;}
    pthread_mutex_lock(&g_sp_request_mu);
    if(atomic_load(&g_sp_control)!=2||atomic_load(&g_sp_cancel)){pthread_mutex_unlock(&g_sp_request_mu);return;}
    NSURL *root=g_sp_root;if(!root){atomic_store(&g_sp_control,0);pthread_mutex_unlock(&g_sp_request_mu);return;}
    atomic_store_explicit(&g_sp_control,3,memory_order_release);pthread_mutex_unlock(&g_sp_request_mu);
    g_sp_frame=frame+1;g_sp_error=0;g_sp_camera_was_on=gta_camera_observer_on();if(!g_sp_camera_was_on)mtl_gta_camera_enable(1);
    memset(&g_sp_selection,0,sizeof g_sp_selection);g_sp_pd=nil;g_sp_main_ds=nil;g_sp_last_ds=nil;g_sp_scene=nil;g_sp_final=nil;
    g_sp_last_ds_stored=g_sp_final_stored=0;g_sp_final_pass=g_sp_last_ds_pass=g_sp_snapshot_ds_store_pass=0;
    atomic_store(&g_sp_finished,0);atomic_store(&g_sp_final_done,0);
    atomic_store(&g_sp_mismatch_roles_settled,0);
    g_sp_capture=[[GtaSourceProbe alloc]initWithQueue:g_q4 frame:g_sp_frame outputRoot:root completion:^(NSDictionary *result){
        fprintf(stderr,"[gta-source-probe] result frame=%llu files=%d manifest=%d dir=%s\n",[result[@"frame"]unsignedLongLongValue],
          [result[@"filesOK"]boolValue],[result[@"manifestWritten"]boolValue],[result[@"directory"]UTF8String]);
        atomic_store_explicit(&g_sp_finished,1,memory_order_release);
    }];
    if(!g_sp_capture){gta_sp_restore_camera();atomic_store(&g_sp_control,0);return;}
    atomic_store_explicit(&g_sp_control,3,memory_order_release);
}
#endif
