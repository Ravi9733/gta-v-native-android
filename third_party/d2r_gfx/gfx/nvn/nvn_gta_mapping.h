#ifndef NVN_GTA_MAPPING_H
#define NVN_GTA_MAPPING_H
/* Diagnostic only. No MetalFX activation, resource pixel reads or game writes.
   One bounded capture per process, disabled until explicitly requested.
   All mutation of the payload is on serialized graphics replay/present paths.
   The immutable sealed payload can be exported off that path. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
enum { GM_VERSION=3, GM_MAX=2048, GM_BYTES=1536, GM_KEY=80 };
enum { GM_DRAW=1, GM_UBO=2, GM_TEXTURE=3, GM_PRESENT=4, GM_UNAVAILABLE=5, GM_PASS_END=6, GM_PASS_BEGIN=7, GM_COLOR_CLEAR=8, GM_DEPTH_CLEAR=9, GM_ARGUMENTS=10 };
enum { GM_NO_BYTES=0, GM_READY_SNAPSHOT=1 };
typedef struct {
    uint64_t serial, rid, parent_rid;
    uint32_t width,height,depth,format,type,samples,level,slice;
} gm_image;
typedef struct {
    uint64_t frame,submission,pass,draw;
    char vertex_key[GM_KEY],fragment_key[GM_KEY];
    gm_image color[8],depth;
    uint64_t blend[8]; /* exact nvn_obj.h blend_st value, pinned by renderer ABI */
    float viewport[6],depth_range[2],clear_depth;
    uint32_t depth_test,depth_write,depth_function,color_count;
    uint32_t blend_enable_mask,channel_mask,clip_near_zero,layered;
    uint32_t depth_load_action,depth_store_action,stencil_test,depth_clamp;
    uint32_t pass_metadata_valid,modifier_flags;
    uint8_t color_load[8],color_store[8];
    int32_t scissor[4];
} gm_draw;
typedef struct {
    gm_draw state;
    uint64_t identity;
    uint32_t kind,stage,slot,origin,byte_count,flags;
    uint8_t bytes[GM_BYTES];
} gm_record;
typedef struct {
    uint64_t magic;
    uint32_t version,endian,record_size,count,truncated,unavailable;
    uint64_t first_frame,frame_count;
    uint32_t sealed,request_clamped;
    uint64_t metalfx_capability; /* 0 not queried/unknown, 1 unsupported, 2 supported */
    gm_record records[GM_MAX];
} gm_capture;
/* The same-size v3 record adds explicit pass packets. Store masks attest only
   actions actually sent to the encoder, not predictions at pass begin. */
typedef struct {
    double clear_color[8][4];
    uint32_t color_count,color_store_known,depth_store_known,stencil_store_known;
    uint32_t stencil_load,stencil_store,clear_stencil,reserved;
    uint64_t stencil_rid;
} gm_pass_meta;
typedef struct {uint64_t value;uint16_t reg,aid,off;uint8_t kind,rw;} gm_arg_binding;
typedef struct {int32_t index,mask;float rgba[4];uint32_t scissor[4];} gm_color_clear;
typedef struct {float depth;int32_t depth_mask,stencil,stencil_mask;uint32_t scissor[4];} gm_depth_clear;
typedef struct {double requested_period;uint64_t monotonic_ns;int32_t crop[4];} gm_present;
static inline void gm_init(gm_capture *c,uint64_t first,unsigned frames) {
    memset(c,0,sizeof(*c));c->magic=UINT64_C(0x3350414d58464d47);
    c->version=GM_VERSION;c->endian=UINT32_C(0x01020304);c->record_size=(uint32_t)sizeof(gm_record);
    c->first_frame=first;c->frame_count=frames>3?3:frames;
}
static inline int gm_active(const gm_capture *c,uint64_t frame) {
    return c && c->frame_count && !c->sealed && frame>=c->first_frame && frame-c->first_frame<c->frame_count;
}
static inline gm_record *gm_append(gm_capture *c,const gm_draw *d,uint32_t kind,
    uint32_t stage,uint32_t slot,uint32_t origin,uint64_t identity,const void *bytes,size_t n) {
    if(!d||!gm_active(c,d->frame)||kind<GM_DRAW||kind>GM_ARGUMENTS)return NULL;
    if(n>GM_BYTES || (n&&!bytes) || ((kind==GM_UBO||kind==GM_UNAVAILABLE)&&(stage>1||slot>=16)) ||
        (kind==GM_UBO && (origin!=GM_READY_SNAPSHOT || !n))) {
        if(c->unavailable!=UINT32_MAX)c->unavailable++;return NULL;
    }
    if(c->count>=GM_MAX){if(c->truncated!=UINT32_MAX)c->truncated++;return NULL;}
    gm_record *r=&c->records[c->count++];memset(r,0,sizeof(*r));r->state=*d;
    r->state.vertex_key[GM_KEY-1]=0;r->state.fragment_key[GM_KEY-1]=0;
    r->kind=kind;r->stage=stage;r->slot=slot;r->origin=origin;r->identity=identity;r->byte_count=(uint32_t)n;
    if(n)memcpy(r->bytes,bytes,n);return r;
}
static inline size_t gm_export_size(const gm_capture *c) {
    return c && c->magic==UINT64_C(0x3350414d58464d47) && c->version==GM_VERSION &&
        c->endian==UINT32_C(0x01020304) && c->record_size==sizeof(gm_record) && c->sealed && c->count<=GM_MAX ?
        offsetof(gm_capture,records)+(size_t)c->count*sizeof(gm_record):0;
}
_Static_assert(sizeof(gm_pass_meta)==296,"mapping pass packet ABI");
_Static_assert(sizeof(gm_arg_binding)==16,"mapping binding ABI");
_Static_assert(sizeof(gm_color_clear)==40,"mapping color clear ABI");
_Static_assert(sizeof(gm_depth_clear)==32,"mapping depth clear ABI");
_Static_assert(sizeof(gm_image)==56,"mapping image ABI");
_Static_assert(sizeof(gm_draw)==888,"mapping draw ABI");
_Static_assert(sizeof(gm_record)==2456,"mapping record ABI");
_Static_assert(offsetof(gm_capture,records)==64,"mapping header ABI");
/* Public diagnostic request: first=0 starts at next complete presented frame;
   frames=1..3; one request per launch. Thread-safe; no renderer resources touched. */
int mtl_gta_mapping_request(uint64_t first,unsigned frames);
/* Returns immutable data ONLY after sealing; caller does not own/free it. */
const void *mtl_gta_mapping_data(size_t *bytes);
#endif
