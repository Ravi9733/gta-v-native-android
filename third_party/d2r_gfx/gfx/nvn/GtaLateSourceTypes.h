#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
enum{LS_ABI=1,LS_RECORDS=1024,LS_PAYLOAD=1280,LS_KEY=80,LS_LAYOUT_BUDGET=8*1024*1024};
enum{LS_DRAW=1,LS_LAYOUT=2,LS_BUFFER=3,LS_ARGUMENTS=4,LS_TEXTURE=5};
enum{LS_UNKNOWN=0,LS_POOL=1,LS_ARENA=2,LS_RESIDENT=3,LS_STATIC_ARRAY_FAN=4};
typedef struct{uint64_t offset,stride,step_rate;uint32_t location,format,buffer_index,step_function;} ls_attribute;
typedef struct{uint32_t count,flags;ls_attribute attributes[31];} ls_layout;
typedef struct{uint64_t first,count,instances,base_instance,index_address,index_length,indirect_address;int64_t base_vertex;uint32_t kind,primitive,index_type,conversion,fan_vertices,fan_capacity;} ls_draw;
typedef struct{
 uint64_t object,gpu_base,length,address,offset,declared_address,declared_length,pool_lo,pool_hi,pool_base,pool_key,pool_generation,registry_generation,heap,heap_offset,cpu_base;
 uint32_t slot,role,origin,storage,options,flags; /* role1VB/2index/3indirect/4AB/5CBUF */
} ls_buffer;
typedef struct{uint64_t value;uint16_t reg,aid,off;uint8_t kind,rw;} ls_binding;
typedef struct{uint64_t address,provided_address,buffer,offset,available_length,native_table;uint32_t stage,index,count,flags;ls_binding bindings[64];} ls_arguments;
typedef struct{
 uint64_t rid,object,parent_rid,parent_object,heap,heap_offset,buffer,buffer_offset,buffer_bytes_per_row,iosurface,allocated_size,usage,resource_options;
 uint32_t width,height,depth,format,type,samples,mips,array_length,parent_level,parent_slice,storage,cpu_cache,hazard,shareable,framebuffer_only,sparse,swizzle[4],iosurface_plane,origin,flags;
} ls_texture;
typedef struct{uint64_t frame,submission,pass,draw,sequence,pso;uint32_t kind,stage,slot,flags,bytes,reserved;char vertex_key[LS_KEY],fragment_key[LS_KEY];uint8_t payload[LS_PAYLOAD];} ls_record;
typedef struct{uint64_t magic;uint32_t abi,endian,record_size,count,truncated,unavailable,sealed,startup_enabled;uint64_t frame,layout_bytes,layout_missed;ls_record records[LS_RECORDS];} ls_capture;
static inline void ls_init_capture(ls_capture*c,uint64_t frame){memset(c,0,sizeof *c);c->magic=UINT64_C(0x315243534c415447);c->abi=LS_ABI;c->endian=0x01020304;c->record_size=sizeof(ls_record);c->startup_enabled=1;c->frame=frame;}
static inline ls_record*ls_append(ls_capture*c,const ls_record*context,unsigned kind,unsigned stage,unsigned slot,unsigned flags,const void*bytes,size_t n){
 if(!c||c->sealed||!context||context->frame!=c->frame||kind<LS_DRAW||kind>LS_TEXTURE||n>LS_PAYLOAD||(n&&!bytes)){if(c&&c->unavailable!=UINT32_MAX)c->unavailable++;return NULL;}
 if(c->count==LS_RECORDS){if(c->truncated!=UINT32_MAX)c->truncated++;return NULL;}
 ls_record*r=&c->records[c->count++];*r=*context;r->kind=kind;r->stage=stage;r->slot=slot;r->flags=context->flags|flags;r->bytes=(uint32_t)n;r->vertex_key[79]=r->fragment_key[79]=0;memset(r->payload,0,sizeof r->payload);if(n)memcpy(r->payload,bytes,n);return r;
}
static inline size_t ls_export_size(const ls_capture*c){return c&&c->magic==UINT64_C(0x315243534c415447)&&c->abi==LS_ABI&&c->endian==0x01020304&&c->record_size==sizeof(ls_record)&&c->sealed&&c->count<=LS_RECORDS?offsetof(ls_capture,records)+(size_t)c->count*sizeof(ls_record):0;}
_Static_assert(sizeof(ls_layout)<=LS_PAYLOAD,"layout cap");
_Static_assert(sizeof(ls_arguments)<=LS_PAYLOAD,"argument cap");
_Static_assert(sizeof(ls_attribute)==40,"attribute ABI");
_Static_assert(sizeof(ls_binding)==16,"binding ABI");
#ifdef __OBJC__
static void gta_ls_init(void),gta_ls_request(void),gta_ls_present(void);
static void gta_ls_encoder_begin(id),gta_ls_pipeline(id,id),gta_ls_address(id,unsigned,NSUInteger,uint64_t),gta_ls_native_draw(id,ls_draw);
static void gta_ls_pso_created(id,MTLRenderPipelineDescriptor*);
static void gta_ls_program(id),gta_ls_bound(id,id,int,int,id<MTLBuffer>,uint64_t,size_t,NSUInteger);
static void gta_ls_fan(unsigned,unsigned),gta_ls_builtin(void);
#endif
_Static_assert(sizeof(ls_draw)==88,"native draw ABI");
_Static_assert(sizeof(ls_buffer)==152,"native buffer ABI");
_Static_assert(sizeof(ls_arguments)==1088,"native arguments ABI");
_Static_assert(sizeof(ls_texture)==200,"native texture ABI");
_Static_assert(sizeof(ls_record)==1512,"late source record ABI");
_Static_assert(offsetof(ls_capture,records)==64,"late source header ABI");
