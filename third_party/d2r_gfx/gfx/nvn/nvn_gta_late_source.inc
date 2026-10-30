/* Metadata ONLY; included after registration, NvShader, arenas, S and g_pass. */
static __unsafe_unretained id g_ls_encoder,g_ls_pso;
static id g_ls_tables[2];
static uint64_t g_ls_addresses[2][31];static ls_record g_ls_context;
static int g_ls_tail,g_ls_program_valid,g_ls_builtin_context;static uint64_t g_ls_sequence;static unsigned g_ls_conversion,g_ls_fan_vertices,g_ls_fan_capacity;
static id<MTLTexture>g_ls_textures[64];static unsigned g_ls_texture_count;
static int gta_ls_on(void){return atomic_load_explicit(&g_ls_enabled,memory_order_relaxed)&&on_sub_thread();}
static int gta_ls_active(void){return gta_ls_on()&&atomic_load_explicit(&g_ls_phase,memory_order_relaxed)==1&&g_ls_tail&&atomic_load(&g_cur_frame)==g_ls_capture.frame;}
static void gta_ls_check_table(unsigned stage){id table=stage==0?g_vt:g_ft;if(g_ls_tables[stage]!=table){memset(g_ls_addresses[stage],0,sizeof g_ls_addresses[stage]);g_ls_tables[stage]=table;}}
static void gta_ls_encoder_on(id e){if(g_ls_encoder!=e){g_ls_encoder=e;g_ls_pso=nil;memset(g_ls_addresses,0,sizeof g_ls_addresses);}}
static void gta_ls_encoder_begin(id e){if(gta_ls_on()){g_ls_encoder=nil;gta_ls_encoder_on(e);}}
static void gta_ls_pipeline(id e,id p){if(!gta_ls_on())return;gta_ls_encoder_on(e);g_ls_pso=p;}
static void gta_ls_address(id e,unsigned stage,NSUInteger slot,uint64_t a){if(!gta_ls_on())return;gta_ls_encoder_on(e);if(stage<2&&slot<31){gta_ls_check_table(stage);g_ls_addresses[stage][slot]=g_ls_tables[stage]?a:0;}}
static void gta_ls_program(id object){
 if(!gta_ls_on()||atomic_load(&g_ls_phase)!=1||atomic_load(&g_cur_frame)!=g_ls_capture.frame)return;NvProg*p=object;
 if(p&&p->sh[1]&&gta_map_late_key(p->sh[1]->key))g_ls_tail=1;
 if(!g_ls_tail)return;g_ls_program_valid=1;g_ls_builtin_context=0;memset(&g_ls_context,0,sizeof g_ls_context);g_ls_context.frame=atomic_load(&g_cur_frame);g_ls_context.submission=g_sub_serial;g_ls_context.pass=g_pass.serial;g_ls_context.draw=g_stat_draw;
 if(p&&p->sh[0])memcpy(g_ls_context.vertex_key,p->sh[0]->key,LS_KEY);if(p&&p->sh[1])memcpy(g_ls_context.fragment_key,p->sh[1]->key,LS_KEY);
}
static void gta_ls_builtin(void){if(gta_ls_active()){g_ls_program_valid=0;g_ls_builtin_context=1;}}
static void gta_ls_refresh(void){
 uint64_t f=atomic_load(&g_cur_frame);if(g_ls_context.frame!=f||g_ls_context.pass!=g_pass.serial||g_ls_context.draw!=g_stat_draw)g_ls_program_valid=0;
 if(!g_ls_program_valid){memset(g_ls_context.vertex_key,0,LS_KEY);memset(g_ls_context.fragment_key,0,LS_KEY);}
 g_ls_context.flags=g_ls_program_valid?0:(g_ls_builtin_context?1u<<17:1u<<16);
 g_ls_context.frame=f;g_ls_context.submission=g_sub_serial;g_ls_context.pass=g_pass.serial;g_ls_context.draw=g_stat_draw;g_ls_context.sequence=g_ls_sequence+1;g_ls_context.pso=(uint64_t)(uintptr_t)(__bridge void*)g_ls_pso;
}
static id<MTLBuffer>gta_ls_find_buffer(uint64_t address,ls_buffer*meta){
 if(!meta)return nil;if(!address){meta->flags|=4;return nil;}id<MTLBuffer>found=nil;meta->address=address;meta->registry_generation=atomic_load(&g_reg_gen);meta->pool_generation=atomic_load(&g_pool_gen);
 if(pthread_mutex_trylock(&g_mu)==0){unsigned n=(unsigned)g_pools.n;if(n>4096){n=4096;meta->flags|=1;}
  for(unsigned i=0;i<n;i++){const pidx_ent*p=&g_pools.e[i];id<MTLBuffer>b=(__bridge id<MTLBuffer>)p->buf;uint64_t length=p->hi>=p->base?p->hi-p->base:0;
   if(address>=p->gpu&&address-p->gpu<length){found=b;meta->origin=LS_POOL;meta->pool_lo=p->lo;meta->pool_hi=p->hi;meta->pool_base=p->base;meta->pool_key=(uint64_t)(uintptr_t)p->key;break;}}
  pthread_mutex_unlock(&g_mu);
 }else meta->flags|=2;
 if(!found){unsigned n=0;for(NvArena*a in g_sub_arenas){if(++n>4096){meta->flags|=1;break;}if(address>=a->gpu&&address-a->gpu<a->buf.length){found=a->buf;meta->origin=LS_ARENA;break;}}}
 if(!found&&pthread_mutex_trylock(&g_res_mu)==0){unsigned n=0;for(id object in g_res_objects){if(++n>4096){meta->flags|=1;break;}if([object conformsToProtocol:@protocol(MTLBuffer)]){id<MTLBuffer>b=object;uint64_t gpu=b.gpuAddress;if(gpu&&address>=gpu&&address-gpu<b.length){found=b;meta->origin=LS_RESIDENT;break;}}}pthread_mutex_unlock(&g_res_mu);}
 if(found&&meta->origin==LS_ARENA){
  for(int i=0;i<g_nusnap&&i<4096;i++){const usnap_t*u=&g_usnap[i];uint64_t n=u->hi>=u->lo?u->hi-u->lo:0;if(u->gpu&&address>=u->gpu&&address-u->gpu<n){meta->flags|=atomic_load_explicit(&u->st,memory_order_acquire)==US_READY?8:16;break;}}
  for(int i=0;i<g_nshadow&&i<4096;i++)if(address>=g_shadow[i].gpu&&address-g_shadow[i].gpu<g_shadow[i].size){meta->flags|=32;break;}
 }
 if(found){meta->object=(uint64_t)(uintptr_t)(__bridge void*)found;meta->gpu_base=found.gpuAddress;meta->length=found.length;meta->offset=address-found.gpuAddress;meta->storage=(uint32_t)found.storageMode;meta->options=(uint32_t)found.resourceOptions;meta->heap=(uint64_t)(uintptr_t)(__bridge void*)found.heap;meta->heap_offset=found.heapOffset;if(found.storageMode==MTLStorageModeShared)meta->cpu_base=(uint64_t)(uintptr_t)found.contents;}
 else meta->flags|=4;return found;
}
static void gta_ls_buffer_note(uint64_t a,unsigned slot,unsigned role,unsigned stage){
 ls_buffer b={.slot=slot,.role=role};id<MTLBuffer>hold=gta_ls_find_buffer(a,&b);if((b.flags&4)&&g_ls_capture.unavailable!=UINT32_MAX)g_ls_capture.unavailable++;(void)hold;
 if(role==1&&slot>=16&&slot<32){b.declared_address=S.vbuf[slot-16].address;b.declared_length=S.vbuf[slot-16].size;} /* current declaration, NOT matching PSO/layout proof */
 if(role==5&&stage<2&&slot<16){b.declared_address=S.ubo[stage][slot].addr;b.declared_length=S.ubo[stage][slot].size;}
 (void)ls_append(&g_ls_capture,&g_ls_context,LS_BUFFER,stage,slot,b.flags,&b,sizeof b);
}
static id<MTLTexture>gta_ls_find_texture(int stage,unsigned reg,uint64_t rid,unsigned*origin){
 if(!rid||stage<0||stage>=6||reg>=128)return nil;
 for(unsigned i=0;i<g_ls_texture_count;i++)if(g_ls_textures[i].gpuResourceID._impl==rid){*origin=4;return g_ls_textures[i];}
 if(g_ls_texture_count==64)return nil;id<MTLTexture>found=nil;
 if(g_rmu_depth)registry_lock();else if(os_unfair_lock_trylock(&g_rmu))g_rmu_depth=1;else return nil;
 unsigned tid=(unsigned)(S.tex[stage][reg]&UINT64_C(0xfffff));if(tid&&tid<(unsigned)g_ntreg&&g_treg[tid].ok&&g_treg[tid].view){id<MTLTexture>current=(__bridge id<MTLTexture>)g_treg[tid].view;
  if(current.gpuResourceID._impl==rid){found=current;*origin=1;}else{id<MTLTexture>v=[g_type_views objectForKey:current];if(v.gpuResourceID._impl==rid){found=v;*origin=2;}}}
 if(!found&&pthread_mutex_trylock(&g_res_mu)==0){unsigned n=0;for(id object in g_res_objects){if(++n>4096)break;if([object conformsToProtocol:@protocol(MTLTexture)]){id<MTLTexture>t=object;if(t.gpuResourceID._impl==rid){found=t;*origin=3;break;}}}pthread_mutex_unlock(&g_res_mu);}
 registry_unlock();if(found)g_ls_textures[g_ls_texture_count++]=found;return found;
}
static void gta_ls_texture_note(int stage,unsigned reg,uint64_t rid){
 /* Dedup metadata records only; per-bind exact RID remains in LS_ARGUMENTS. */
 for(uint32_t i=0;i<g_ls_capture.count;i++){const ls_record*r=&g_ls_capture.records[i];if(r->kind==LS_TEXTURE&&r->bytes==sizeof(ls_texture)){ls_texture b;memcpy(&b,r->payload,sizeof b);if(b.rid==rid)return;}}
 ls_texture b;memset(&b,0,sizeof b);b.rid=rid;unsigned origin=0;id<MTLTexture>t=gta_ls_find_texture(stage,reg,rid,&origin);b.origin=origin;
 if(t){b.object=(uint64_t)(uintptr_t)(__bridge void*)t;b.parent_rid=t.parentTexture.gpuResourceID._impl;b.parent_object=(uint64_t)(uintptr_t)(__bridge void*)t.parentTexture;
 b.heap=(uint64_t)(uintptr_t)(__bridge void*)t.heap;b.heap_offset=t.heapOffset;b.buffer=(uint64_t)(uintptr_t)(__bridge void*)t.buffer;b.buffer_offset=t.bufferOffset;b.buffer_bytes_per_row=t.bufferBytesPerRow;b.iosurface=(uint64_t)(uintptr_t)t.iosurface;
 b.width=(uint32_t)t.width;b.height=(uint32_t)t.height;b.depth=(uint32_t)t.depth;b.format=(uint32_t)t.pixelFormat;b.type=(uint32_t)t.textureType;b.samples=(uint32_t)t.sampleCount;b.mips=(uint32_t)t.mipmapLevelCount;b.array_length=(uint32_t)t.arrayLength;b.parent_level=(uint32_t)t.parentRelativeLevel;b.parent_slice=(uint32_t)t.parentRelativeSlice;
 b.allocated_size=t.allocatedSize;b.usage=t.usage;b.resource_options=t.resourceOptions;b.storage=(uint32_t)t.storageMode;b.cpu_cache=(uint32_t)t.cpuCacheMode;b.hazard=(uint32_t)t.hazardTrackingMode;b.shareable=t.isShareable;b.framebuffer_only=t.framebufferOnly;b.sparse=t.isSparse;b.iosurface_plane=(uint32_t)t.iosurfacePlane;
 MTLTextureSwizzleChannels s=t.swizzle;b.swizzle[0]=s.red;b.swizzle[1]=s.green;b.swizzle[2]=s.blue;b.swizzle[3]=s.alpha;
 }else{b.flags=1;if(g_ls_capture.unavailable!=UINT32_MAX)g_ls_capture.unavailable++;}
 (void)ls_append(&g_ls_capture,&g_ls_context,LS_TEXTURE,(unsigned)stage,reg,b.flags,&b,sizeof b);
}
static void gta_ls_bound(id e,id object,int nvn_stage,int metal_vertex,id<MTLBuffer>ab,uint64_t gpu,size_t off,NSUInteger index){
 if(!gta_ls_active()||nvn_stage==5)return;gta_ls_encoder_on(e);gta_ls_refresh();NvShader*sh=object;unsigned stage=metal_vertex?0:1;gta_ls_check_table(stage);ls_arguments body={.address=index<31?g_ls_addresses[stage][index]:0,.provided_address=gpu,.buffer=(uint64_t)(uintptr_t)(__bridge void*)ab,.offset=off,.available_length=ab.length>=off?ab.length-off:0,.native_table=(uint64_t)(uintptr_t)(__bridge void*)g_ls_tables[stage],.stage=stage,.index=(uint32_t)index};
 NSUInteger bytes=sh?(sh->arg_len?:sh->ae.encodedLength):0;
 /* These are renderer-owned CPU-constructed AB metadata words, never raw guest,
    UBO/VB/index/texture contents. Validate ACTUAL bound native table address. */
 if(!sh||!sh->rf.route_b||sh->rf.nb<0||sh->rf.nb>64||!bytes||bytes>512||bytes%8||(off&7)||index>=31||g_ls_addresses[stage][index]!=gpu||!ab||ab.storageMode!=MTLStorageModeShared||!ab.contents||off>ab.length||bytes>ab.length-off||ab.gpuAddress>UINT64_MAX-off||gpu!=ab.gpuAddress+off){body.flags=1;g_ls_capture.unavailable++;}
 else{const uint64_t*words=(const uint64_t*)((const uint8_t*)ab.contents+off);for(int i=0;i<sh->rf.nb;i++){const nvn_rbind*b=&sh->rf.b[i];if(b->aid>=bytes/8){body.flags|=2;continue;}body.bindings[body.count++]=(ls_binding){words[b->aid],b->reg,b->aid,b->off,b->kind,b->rw};}}
 (void)ls_append(&g_ls_capture,&g_ls_context,LS_ARGUMENTS,stage,(unsigned)index,body.flags,&body,offsetof(ls_arguments,bindings)+body.count*sizeof(ls_binding));
 if(!body.flags)for(unsigned i=0;i<body.count;i++){const ls_binding*b=&body.bindings[i];if(b->kind==NVN_RB_TEX)gta_ls_texture_note(nvn_stage,b->reg,b->value);else if(b->kind==NVN_RB_CBUF)gta_ls_buffer_note(b->value,b->reg,5,stage);}
}
static void gta_ls_fan(unsigned vertices,unsigned capacity){if(gta_ls_active()){g_ls_conversion=LS_STATIC_ARRAY_FAN;g_ls_fan_vertices=vertices;g_ls_fan_capacity=capacity;}}
static void gta_ls_native_draw(id e,ls_draw d){
 if(!gta_ls_active())return;gta_ls_encoder_on(e);gta_ls_refresh();gta_ls_check_table(0);gta_ls_check_table(1);g_ls_context.sequence=++g_ls_sequence;g_ls_context.pso=(uint64_t)(uintptr_t)(__bridge void*)g_ls_pso;
 d.conversion=g_ls_conversion;d.fan_vertices=g_ls_fan_vertices;d.fan_capacity=g_ls_fan_capacity;g_ls_conversion=g_ls_fan_vertices=g_ls_fan_capacity=0;
 (void)ls_append(&g_ls_capture,&g_ls_context,LS_DRAW,0,0,rs_using(e)?1:0,&d,sizeof d);
 ls_layout layout={0};int known=gta_ls_layout(g_ls_pso,&layout);if(!known)g_ls_capture.unavailable++;
 (void)ls_append(&g_ls_capture,&g_ls_context,LS_LAYOUT,0,0,known?0:1,&layout,offsetof(ls_layout,attributes)+layout.count*sizeof(ls_attribute));
 uint32_t mask=0;if(known)for(unsigned i=0;i<layout.count;i++)mask|=1u<<layout.attributes[i].buffer_index;
 else for(unsigned i=16;i<31;i++)if(g_ls_addresses[0][i])mask|=1u<<i;
 for(unsigned i=0;i<31;i++)if(mask&(1u<<i))gta_ls_buffer_note(g_ls_addresses[0][i],i,1,0);
 if(d.kind==2||d.kind==3)gta_ls_buffer_note(d.index_address,0,2,0);if(d.kind==3)gta_ls_buffer_note(d.indirect_address,0,3,0);g_ls_program_valid=0;g_ls_builtin_context=0;
}
static int gta_ls_export_dir(const char*docs,const ls_capture*c,char*name,size_t cap){
 size_t bytes=ls_export_size(c);if(!docs||!bytes||!name||!cap)return 0;int root=open(docs,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)return 0;struct stat st;int ok=!fstat(root,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==getuid();
 if(ok&&mkdirat(root,"gta-late-source-metadata",0700)<0&&errno!=EEXIST)ok=0;int dir=ok?openat(root,"gta-late-source-metadata",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;close(root);if(dir<0)return 0;
 if(fstat(dir,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()){close(dir);return 0;}char temp[144]={0};int n=snprintf(name,cap,"late-%d-%llu.bin",getpid(),(unsigned long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW));if(n<=0||(size_t)n>=cap||snprintf(temp,sizeof temp,".%s.tmp",name)>=(int)sizeof temp){close(dir);return 0;}
 int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0){close(dir);return 0;}
 ok=gta_map_write_fd(fd,c,bytes);if(close(fd))ok=0;
 /* Atomic publication of COMPLETE file without replacement/following links. */
 if(ok&&linkat(dir,temp,dir,name,0))ok=0;if(unlinkat(dir,temp,0))ok=0;close(dir);return ok;
}
static void gta_ls_export_async(void){
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool{
 NSString*docs=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;char name[128]={0};int ok=gta_ls_export_dir(docs.fileSystemRepresentation,&g_ls_capture,name,sizeof name);
 fprintf(stderr,"[gta-late-source] frame=%llu records=%u truncated=%u unavailable=%u layout_bytes=%llu layout_missed=%llu export=%s %s bytes=%zu (metadata only)\n",(unsigned long long)g_ls_capture.frame,g_ls_capture.count,g_ls_capture.truncated,g_ls_capture.unavailable,(unsigned long long)g_ls_capture.layout_bytes,(unsigned long long)g_ls_capture.layout_missed,ok?"ok":"failed",name,ls_export_size(&g_ls_capture));
 }});
}
static void gta_ls_present(void){
 if(!gta_ls_on())return;uint64_t frame=atomic_load(&g_cur_frame);int phase=atomic_load(&g_ls_phase);
 if(phase==0&&atomic_load(&g_ls_request)){if(frame==UINT64_MAX)return;ls_init_capture(&g_ls_capture,frame+1);g_ls_tail=0;g_ls_sequence=0;atomic_store_explicit(&g_ls_phase,1,memory_order_release);}
 else if(phase==1&&frame>=g_ls_capture.frame){g_ls_capture.layout_bytes=atomic_load(&g_ls_layout_used);g_ls_capture.layout_missed=atomic_load(&g_ls_layout_missed);g_ls_capture.sealed=1;atomic_store_explicit(&g_ls_phase,2,memory_order_release);for(unsigned i=0;i<g_ls_texture_count;i++)g_ls_textures[i]=nil;g_ls_texture_count=0;g_ls_tables[0]=g_ls_tables[1]=nil;gta_ls_export_async();}
}
