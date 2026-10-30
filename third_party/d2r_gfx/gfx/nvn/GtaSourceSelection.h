#ifndef GTA_SOURCE_SELECTION_H
#define GTA_SOURCE_SELECTION_H
/* Memory-only candidate selection for one armed GTA source probe. Keys identify
   the pinned shaders; actual bound RID edges, native layouts and final Store are
   mandatory. This NEVER supplies MetalFX verified bits or proves pixel roles,
   complete scene depth, HUD replay, motion or timing. Source copy/readback is an
   independent opt-in consumer. No hard-coded RIDs, pass or draw numbers. */
#include "GtaCamera.h"

typedef struct {uint64_t rid;uint32_t width,height,format,type,samples,level,slice;} gta_source_image;
typedef struct {
    uint64_t frame,pass,draw;const char *ps;
    gta_source_image color;uint32_t color_count,modifiers,blend_mask,channel_mask,depth_test,depth_write;
    double viewport[6];uint32_t scissor[4];uint64_t texture[16];uint32_t texture_mask;
} gta_source_draw;
typedef enum {GTA_SOURCE_NONE=0,GTA_SOURCE_SCENE_CANDIDATE=1,GTA_SOURCE_LENS_CANDIDATE=2,GTA_SOURCE_FINAL_CANDIDATE=3} gta_source_event;
typedef struct {
    uint64_t frame,opaque_pass,opaque_depth_rid,last_draw,pending_pass;
    uint32_t width,height,phase,invalid;
    gta_source_image composite,edge,weights,scene,final;
} gta_source_selection;

static inline int gta_source_key(const char *key,const char *base){return key&&!strncmp(key,base,64)&&key[64]=='-';}
static inline unsigned gta_source_role(const char *key){
    static const char*const keys[]={
      "c309074ebf781cfef59218a152db14e3933dd5b67b6d8aecab97e190c88cc4c3",
      "55e15c5310ecad9bfb0818834dc63320680bfbbed65b79be290e3c01f6d6403f",
      "c2b571bca72f3b08073a2946793e9dd60ab79717c6381facab8c0987ce3c61b5",
      "9a27db71ee0178f23abe150f1516dfd729533e0453889af77e2e6c991d3622f1",
      "cfb2a6d3399fcd483a9102ca177e3437b152c659f9ae5d69eef8141373429c37"};
    for(unsigned i=0;i<5;i++)if(gta_source_key(key,keys[i]))return i+1;return 0;
}
/* c must come from successful mtl_gta_camera_copy, not a shape-only heuristic. */
static inline int gta_source_selection_begin(gta_source_selection *s,const gta_camera_context *c){
    if(!s)return 0;memset(s,0,sizeof *s);
    if(!gta_camera_context_ok(c)){s->invalid=1;return 0;}
    s->frame=c->frame;s->opaque_pass=c->pass;s->opaque_depth_rid=c->depth_rid;s->width=c->width;s->height=c->height;return 1;
}
static inline int gta_source_full_draw(const gta_source_selection *s,const gta_source_draw *d,unsigned format){
    return d->frame==s->frame&&d->pass>s->opaque_pass&&d->draw&&d->color_count==1&&!d->modifiers&&!d->blend_mask&&
      d->channel_mask==UINT32_MAX&&!d->depth_test&&!d->depth_write&&d->color.rid&&d->color.rid!=s->opaque_depth_rid&&
      d->color.width==s->width&&d->color.height==s->height&&d->color.format==format&&d->color.type==2&&d->color.samples==1&&
      !d->color.level&&!d->color.slice&&d->viewport[0]==0&&d->viewport[1]==0&&d->viewport[2]==s->width&&d->viewport[3]==s->height&&
      d->viewport[4]==0&&d->viewport[5]==1&&d->scissor[0]==0&&d->scissor[1]==0&&d->scissor[2]==s->width&&d->scissor[3]==s->height;
}
static inline int gta_source_tex(const gta_source_draw *d,unsigned reg,uint64_t rid){return (d->texture_mask&(1u<<reg))&&d->texture[reg]==rid;}
/* Feed the actually resolved fragment bindings after they were bound. Merely
   seeing a shader key is insufficient. Selected scene copy happens only at its
   subsequent native pass END, naturally before Lens starts. */
static inline gta_source_event gta_source_selection_draw(gta_source_selection *s,const gta_source_draw *d){
    if(!s||!d||s->invalid||!s->frame)return GTA_SOURCE_NONE;
    unsigned role=gta_source_role(d->ps);if(!role)return GTA_SOURCE_NONE;
    unsigned expected=(s->phase/2)+1;
    if(s->pending_pass||role!=expected||role>5||d->draw<=s->last_draw||
       !gta_source_full_draw(s,d,role==2?30:role==5?70:80))goto invalid;
    if(role==1){if(!gta_source_tex(d,0,s->opaque_depth_rid))goto invalid;s->composite=d->color;}
    if(role==2){if(!gta_source_tex(d,0,s->composite.rid)||d->color.rid==s->composite.rid)goto invalid;s->edge=d->color;}
    if(role==3){if(!gta_source_tex(d,1,s->edge.rid)||d->color.rid==s->edge.rid)goto invalid;s->weights=d->color;}
    if(role==4){
        if(!gta_source_tex(d,2,s->weights.rid)||!gta_source_tex(d,0,s->composite.rid)||
           d->color.rid==s->weights.rid||d->color.rid==s->composite.rid)goto invalid;
        s->scene=d->color;
    }
    if(role==5){if(!gta_source_tex(d,0,s->scene.rid)||d->color.rid==s->scene.rid)goto invalid;s->final=d->color;}
    s->last_draw=d->draw;s->pending_pass=d->pass;s->phase=role*2-1;
    return role==5?GTA_SOURCE_LENS_CANDIDATE:GTA_SOURCE_NONE;
invalid:s->invalid=1;return GTA_SOURCE_NONE;
}
/* actual Store1 comes from the encoder's final store decision, not the original
   descriptor's Unknown4. Unknown/DontCare rejects this probe selection. */
static inline gta_source_event gta_source_selection_pass_end(gta_source_selection *s,uint64_t frame,uint64_t pass,unsigned store_known,unsigned store){
    if(!s||s->invalid||!s->pending_pass||pass!=s->pending_pass)return GTA_SOURCE_NONE;
    if(frame!=s->frame||!store_known||store!=1){s->invalid=1;return GTA_SOURCE_NONE;}
    s->pending_pass=0;s->phase++;return s->phase==8?GTA_SOURCE_SCENE_CANDIDATE:GTA_SOURCE_NONE;
}
static inline gta_source_event gta_source_selection_present(gta_source_selection *s,uint64_t frame,gta_source_image actual){
    if(!s||s->invalid||s->phase!=10||s->pending_pass)return GTA_SOURCE_NONE;
    if(frame!=s->frame||actual.rid!=s->final.rid||actual.width!=s->width||actual.height!=s->height||actual.format!=70||
       actual.type!=2||actual.samples!=1||actual.level||actual.slice){s->invalid=1;return GTA_SOURCE_NONE;}
    s->phase=11;return GTA_SOURCE_FINAL_CANDIDATE;
}
#endif
