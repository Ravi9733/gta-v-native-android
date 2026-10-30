/* Triangle fans: Metal has no fan primitive. The assembly order is nxnative's
   (0, previous, next), including restart-before-baseVertex semantics.
   Indexed data is ALWAYS assembled on the GPU: shader/copy writes may still be
   queued even when no CPU-side pending-copy record describes them. */
typedef struct { __unsafe_unretained id<MTLBuffer> buffer; size_t offset; uint64_t indices, args; NSUInteger length; } fan_result;
static id<MTLBuffer> g_fan_ib;
static uint32_t g_fan_vertices;
static uint64_t g_fan_arrays, g_fan_indexed, g_fan_restarts, g_fan_empty, g_fan_fail, g_fan_indirect_skip;
static void fan_note(const char *path, int count, int bytes, int instances, int base_vertex, int base_instance) {
    static unsigned logged[5];
    unsigned kind = !strcmp(path,"arrays") ? 0 : !strcmp(path,"indexed-gpu") ? (bytes==1 ? 1 : bytes==2 ? 2 : 3) : 4;
    if (logged[kind] < 3) { logged[kind]++; MLOG("[fan] %s count=%d index_bytes=%d instances=%d base_vertex=%d base_instance=%d restart=%u/%#x", path, count, bytes, instances, base_vertex, base_instance, S.restart.on, S.restart.index); }
}
static void fan_report(void) {
    MLOG("[fan] arrays=%llu indexed_gpu=%llu restart=%llu empty=%llu failed=%llu indirect_skipped=%llu", (unsigned long long)g_fan_arrays, (unsigned long long)g_fan_indexed, (unsigned long long)g_fan_restarts, (unsigned long long)g_fan_empty, (unsigned long long)g_fan_fail, (unsigned long long)g_fan_indirect_skip);
}
static int fan_arrays(id<MTL4RenderCommandEncoder> en, const p_draw *d) {
    if (d->count < 3 || d->instances <= 0) { g_fan_empty++; return 1; }
    uint64_t count = nvn_fan_index_count((uint32_t)d->count), length = count * 4;
    if (length > g_dev.maxBufferLength || count > UINT32_MAX) { g_fan_fail++; RED_ONCE(126, "triangle fan arrays exceed the device index-buffer limit (vertices %d)", d->count); return 0; }
    if ((uint32_t)d->count > g_fan_vertices) {
        uint32_t vertices = MAX((uint32_t)d->count, 1024u);
        uint64_t nbytes = nvn_fan_index_count(vertices) * 4;
        id<MTLBuffer> next = [g_dev newBufferWithLength:(NSUInteger)nbytes options:MTLResourceStorageModeShared];
        if (!next) { g_fan_fail++; RED_ONCE(126, "triangle fan shared index buffer allocation failed"); return 0; }
        nvn_fan_fill(next.contents, vertices); res_add(next);
        if (g_fan_ib) defer_obj_release(g_fan_ib);
        g_fan_ib = next; g_fan_vertices = vertices;
    }
    fan_note("arrays", d->count, 0, d->instances, d->first, d->base_instance);
    gta_ls_fan((unsigned)d->count,g_fan_vertices);
    enc_r_draw_indexed(en, MTLPrimitiveTypeTriangle, (NSUInteger)count, MTLIndexTypeUInt32, g_fan_ib.gpuAddress, g_fan_ib.length, d->instances, d->first, d->base_instance);
    g_fan_arrays++; return 1;
}
/* count, source byte width/offset, restart enable/value, instance count, signed
   base vertex bits and base instance. Addresses bound to the table are aligned. */
static const char k_fan_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct fan_job { uint count, bytes, shift, restart, marker, instances, base_vertex, base_instance; };\n"
"static uint fan_read(device const uchar *s, uint at, uint bytes) { uint v=0; for(uint b=0;b<bytes;b++) v |= uint(s[ulong(at)*bytes+b]) << (8*b); return v; }\n"
"kernel void fan_expand(device const uchar *src [[buffer(0)]], device uint *dst [[buffer(1)]], device uint *args [[buffer(2)]], constant fan_job &j [[buffer(3)]], uint tid [[thread_position_in_grid]]) {\n"
" device const uchar *s=src+j.shift;\n"
" if (!j.restart) { uint tris=j.count-2; if(tid==0) { args[0]=tris*3;args[1]=j.instances;args[2]=0;args[3]=j.base_vertex;args[4]=j.base_instance; }\n"
"  if(tid<tris) { dst[ulong(tid)*3]=fan_read(s,0,j.bytes);dst[ulong(tid)*3+1]=fan_read(s,tid+1,j.bytes);dst[ulong(tid)*3+2]=fan_read(s,tid+2,j.bytes); } return; }\n"
" if(tid) return; uint have=0,first=0,previous=0,used=0;\n"
" for(uint i=0;i<j.count;i++) { uint x=fan_read(s,i,j.bytes); if(x==j.marker) {have=0;continue;}\n"
"  if(!have) first=x; else if(have>=2) {dst[used++]=first;dst[used++]=previous;dst[used++]=x;} previous=x;if(have<2)have++; }\n"
" args[0]=used;args[1]=j.instances;args[2]=0;args[3]=j.base_vertex;args[4]=j.base_instance; }\n";
static int fan_prepare_indexed(const p_draw_idx *d, fan_result *out) {
    memset(out, 0, sizeof *out);
    if (d->count < 3 || d->instances <= 0) { g_fan_empty++; return 1; }
    if (d->itype < 0 || d->itype > 2) { g_fan_fail++; RED_ONCE(127, "invalid indexed triangle fan type/count"); return 0; }
    unsigned bytes = 1u << d->itype;
    uint64_t count = nvn_fan_index_count((uint32_t)d->count), length = count * 4;
    if (count > UINT32_MAX || length > g_dev.maxBufferLength - 256) { g_fan_fail++; RED_ONCE(127, "indexed triangle fan exceeds the device index-buffer limit"); return 0; }
    void *mb; uint64_t mo, gpu;
    if (!mtl_gpu_addr(d->ibuf, (uint64_t)d->count * bytes, &mb, &mo, &gpu)) { g_fan_fail++; RED_ONCE(127, "triangle fan index range is outside memory pools"); return 0; }
    static id<MTLComputePipelineState> pipeline; static id<MTL4ArgumentTable> table; static int failed;
    if (failed) { g_fan_fail++; return 0; }
    if (!pipeline) {
        NSError *error = nil; id<MTLLibrary> library = [g_dev newLibraryWithSource:@(k_fan_msl) options:nil error:&error];
        id<MTLFunction> function = library ? [library newFunctionWithName:@"fan_expand"] : nil;
        pipeline = function ? [g_dev newComputePipelineStateWithFunction:function error:&error] : nil;
        MTL4ArgumentTableDescriptor *desc = [MTL4ArgumentTableDescriptor new]; desc.maxBufferBindCount = 4;
        table = pipeline ? [g_dev newArgumentTableWithDescriptor:desc error:&error] : nil;
        if (!pipeline || !table) { failed = 1; g_fan_fail++; MLOG("[RED] triangle fan compute conversion unavailable: %s", error.localizedDescription.UTF8String); return 0; }
    }
    /* The existing arena is only 4 MiB; large fans need a dedicated allocation.
       Both paths are retained/resident until this submission's fence completes. */
    id<MTLBuffer> buffer; size_t offset = 0;
    NSUInteger arg_offset = ((NSUInteger)length + 255) & ~(NSUInteger)255, size = arg_offset + 256;
    if (size <= (4u << 20)) offset = arena_alloc(size, &buffer);
    else { buffer = [g_dev newBufferWithLength:size options:MTLResourceStorageModePrivate]; if (buffer) { res_add(buffer); defer_obj_release(buffer); } }
    if (!buffer) { g_fan_fail++; RED_ONCE(127, "triangle fan output allocation failed"); return 0; }
    id<MTLBuffer> params; size_t po = arena_alloc(32, &params);
    if (!params || !params.contents) { g_fan_fail++; RED_ONCE(127, "triangle fan parameter allocation failed"); return 0; }
    uint32_t *j = (uint32_t *)((uint8_t *)params.contents + po);
    uint64_t aligned = gpu & ~15ull;
    j[0]=(uint32_t)d->count;j[1]=bytes;j[2]=(uint32_t)(gpu-aligned);j[3]=S.restart.on;j[4]=S.restart.index;
    j[5]=(uint32_t)d->instances;j[6]=(uint32_t)d->base_vertex;j[7]=(uint32_t)d->base_instance;
    out->buffer=buffer;out->offset=offset;out->indices=buffer.gpuAddress+offset;out->args=out->indices+arg_offset;out->length=(NSUInteger)length;
    fan_note("indexed-gpu",d->count,(int)bytes,d->instances,d->base_vertex,d->base_instance);
    rp_end();
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); [ce setArgumentTable:table];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [table setAddress:aligned atIndex:0];[table setAddress:out->indices atIndex:1];[table setAddress:out->args atIndex:2];[table setAddress:params.gpuAddress+po atIndex:3];
    enc_c_pso(ce,pipeline); enc_c_dispatch(ce,MTLSizeMake(j[3] ? 1 : ((NSUInteger)d->count-2+63)/64,1,1),MTLSizeMake(j[3] ? 1 : 64,1,1));
    enc_c_end(ce);args_dirty_all();g_fan_indexed++;g_fan_restarts += j[3] != 0;
    return 1;
}
