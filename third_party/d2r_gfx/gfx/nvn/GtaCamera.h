#ifndef GTA_CAMERA_H
#define GTA_CAMERA_H
/* Bounded GTA camera extraction from immutable, actually bound scene cb1 bytes.
   This verifies only camera consistency within one main opaque pass; it does NOT
   verify a complete scene/HUD/depth texture or enable interpolation. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

enum { GTA_CAMERA_SAMPLES=6 };
typedef struct {
    double vp[16],projection[16],view_inverse[16],world[16],position[3];
    double near_plane,far_plane,fov_y,aspect;
    uint64_t world_hash;
    int reversed;
} gta_camera;
typedef struct {
    uint64_t frame,pass,depth_rid;
    double viewport[6];
    uint32_t width,height,color_count,depth_format,depth_samples;
    uint32_t depth_test,depth_write,depth_function,clip_near_zero,modifiers;
    uint32_t depth_level,depth_slice;
} gta_camera_context;
typedef struct {
    gta_camera camera;
    gta_camera_context context;
    uint64_t world_hashes[GTA_CAMERA_SAMPLES];
    unsigned samples,distinct_worlds,program_mask,invalid;
    double max_pixel_disagreement;
} gta_camera_proof;

static inline int gta_camera_program(const char *key) {
    static const char *const programs[3]={
        "eaa00c1500caad7f6dd3af3fb6021523cf796f9ba44c69d17cb34e26b6f49204",
        "50d316cebe715be637a7b2a33e70b938eb073fe7498df5b88644e4e337bdbc8b",
        "efe33349019151d25d802746fa69c6d690dc250d88094e55539b1cd6d8251d00"};
    if(!key)return 0;
    for(unsigned i=0;i<3;i++)if(!strncmp(key,programs[i],64)&&key[64]=='-')return (int)i+1;
    return 0;
}
static inline void gta_camera_mul(const double a[16],const double b[16],double out[16]) {
    double r[16];for(unsigned c=0;c<4;c++)for(unsigned row=0;row<4;row++){
        double v=0;for(unsigned k=0;k<4;k++)v+=a[k*4+row]*b[c*4+k];r[c*4+row]=v;}
    memcpy(out,r,sizeof r);
}
static inline int gta_camera_inverse(const double m[16],double out[16]) {
    double a[4][8]={{0}};
    for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++){
        if(!isfinite(m[c*4+r]))return 0;a[r][c]=m[c*4+r];a[r][c+4]=r==c;}
    for(unsigned c=0;c<4;c++){
        unsigned p=c;for(unsigned r=c+1;r<4;r++)if(fabs(a[r][c])>fabs(a[p][c]))p=r;
        if(fabs(a[p][c])<1e-10)return 0;
        for(unsigned j=0;j<8;j++){double t=a[p][j];a[p][j]=a[c][j];a[c][j]=t;}
        double z=a[c][c];for(unsigned j=0;j<8;j++)a[c][j]/=z;
        for(unsigned r=0;r<4;r++)if(r!=c){double z2=a[r][c];for(unsigned j=0;j<8;j++)a[r][j]-=z2*a[c][j];}
    }
    for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++)out[c*4+r]=a[r][4+c];return 1;
}
static inline int gta_camera_affine(const double m[16]) {
    return fabs(m[3])<2e-4&&fabs(m[7])<2e-4&&fabs(m[11])<2e-4&&fabs(m[15]-1)<2e-4;
}
static inline int gta_camera_derive(const void *bytes,size_t size,gta_camera *out) {
    if(!bytes||size!=256||!out)return 0;
    float f[64];memcpy(f,bytes,sizeof f);for(unsigned i=0;i<64;i++)if(!isfinite(f[i]))return 0;
    gta_camera c={0};double wvp[16],inverse[16];
    for(unsigned i=0;i<16;i++){c.world[i]=f[i];wvp[i]=f[32+i];c.view_inverse[i]=f[48+i];}
    if(!gta_camera_affine(c.world)||!gta_camera_affine(c.view_inverse))return 0;
    for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++){
        double dot=0;for(unsigned k=0;k<3;k++)dot+=c.view_inverse[a*4+k]*c.view_inverse[b*4+k];
        if(fabs(dot-(a==b))>.005)return 0;
    }
    if(!gta_camera_inverse(c.world,inverse))return 0;
    gta_camera_mul(wvp,inverse,c.vp);gta_camera_mul(c.vp,c.view_inverse,c.projection);
    double scale=fabs(c.projection[11]);if(scale<1e-8)return 0;
    for(unsigned i=0;i<16;i++)c.projection[i]/=scale;
    const unsigned zeros[]={1,3,4,7,2,6,12,13,15};
    for(unsigned i=0;i<sizeof zeros/sizeof zeros[0];i++)if(fabs(c.projection[zeros[i]])>.003)return 0;
    if(fabs(c.projection[0])<1e-5||fabs(c.projection[5])<1e-5)return 0;
    // Off-centre projection can be real jitter/asymmetry. Initial camera-only
    // adapter rejects it instead of manufacturing de-jittered matrices/history.
    if(fabs(c.projection[8])>1e-5||fabs(c.projection[9])>1e-5)return 0;
    double forward=c.projection[11]>0?1:-1,planes[2];
    for(unsigned z=0;z<2;z++){
        double den=c.projection[10]-z*c.projection[11];if(fabs(den)<1e-10)return 0;
        planes[z]=(z*c.projection[15]-c.projection[14])/den*forward;
        if(!isfinite(planes[z])||planes[z]<=0)return 0;
    }
    c.near_plane=fmin(planes[0],planes[1]);c.far_plane=fmax(planes[0],planes[1]);
    if(c.near_plane<.005||c.far_plane<=c.near_plane||c.far_plane>1e7)return 0;
    c.reversed=planes[1]<planes[0];c.fov_y=2*atan(1/fabs(c.projection[5]))*180/3.14159265358979323846;
    c.aspect=fabs(c.projection[5]/c.projection[0]);if(c.fov_y<=1||c.fov_y>=179||c.aspect<.2||c.aspect>5)return 0;
    for(unsigned i=0;i<3;i++)c.position[i]=c.view_inverse[12+i];
    const uint8_t *p=bytes;c.world_hash=UINT64_C(1469598103934665603);
    for(unsigned i=0;i<64;i++)c.world_hash=(c.world_hash^p[i])*UINT64_C(1099511628211);
    *out=c;return 1;
}
static inline int gta_camera_context_ok(const gta_camera_context *c) {
    if(!c||!c->frame||!c->pass||!c->depth_rid||!c->width||!c->height||c->width>8192||c->height>8192||
       c->color_count<3||c->color_count>4||c->depth_format!=260||c->depth_samples!=1||c->depth_level||c->depth_slice||
       !c->depth_test||!c->depth_write||(c->depth_function!=5&&c->depth_function!=7)||!c->clip_near_zero||c->modifiers)return 0;
    return c->viewport[0]==0&&c->viewport[1]==0&&c->viewport[2]==c->width&&c->viewport[3]==c->height&&c->viewport[4]==0&&c->viewport[5]==1;
}
static inline double gta_camera_agreement(const gta_camera *a,const gta_camera *b,unsigned w,unsigned h) {
    double worst=0;
    const double distances[]={.3,1,10,100,600};
    for(unsigned d=0;d<5;d++)for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y++){
        double v[4]={x*.5*distances[d],y*.5*distances[d],-distances[d],1},world[4]={0},pa[4]={0},pb[4]={0};
        for(unsigned r=0;r<4;r++)for(unsigned k=0;k<4;k++)world[r]+=a->view_inverse[k*4+r]*v[k];
        for(unsigned r=0;r<4;r++)for(unsigned k=0;k<4;k++){pa[r]+=a->vp[k*4+r]*world[k];pb[r]+=b->vp[k*4+r]*world[k];}
        if(!isfinite(pa[3])||!isfinite(pb[3])||pa[3]<1e-8||pb[3]<1e-8)return INFINITY;
        double e=hypot((pa[0]/pa[3]-pb[0]/pb[3])*.5*w,(pa[1]/pa[3]-pb[1]/pb[3])*.5*h);
        if(!isfinite(e))return INFINITY;worst=fmax(worst,e);
    }
    return worst;
}
static inline int gta_camera_add(gta_camera_proof *p,const gta_camera_context *context,const char *key,const void *bytes,size_t size) {
    if(!p||!context)return 0;
    int program=gta_camera_program(key);if(!program||!gta_camera_context_ok(context))return 0;
    if(p->samples>=GTA_CAMERA_SAMPLES||p->invalid)return 0;
    gta_camera c;
    if(!gta_camera_derive(bytes,size,&c)||fabs(c.aspect-(double)context->width/context->height)>.002*c.aspect){p->invalid=1;return 0;}
    if(!p->samples){p->camera=c;p->context=*context;}
    else {
        if(context->frame!=p->context.frame||context->pass!=p->context.pass||context->depth_rid!=p->context.depth_rid||
           context->width!=p->context.width||context->height!=p->context.height||c.reversed!=p->camera.reversed){p->invalid=1;return 0;}
        double e=gta_camera_agreement(&p->camera,&c,context->width,context->height);
        p->max_pixel_disagreement=fmax(p->max_pixel_disagreement,e);
        if(e>.05||fabs(c.fov_y-p->camera.fov_y)>.005||fabs(c.near_plane-p->camera.near_plane)>.0005||
           fabs(c.far_plane-p->camera.far_plane)>.001*p->camera.far_plane){p->invalid=1;return 0;}
    }
    int distinct=1;for(unsigned i=0;i<p->samples;i++)if(p->world_hashes[i]==c.world_hash)distinct=0;
    p->world_hashes[p->samples++]=c.world_hash;p->distinct_worlds+=(unsigned)distinct;p->program_mask|=1u<<(program-1);return 1;
}
static inline int gta_camera_proven(const gta_camera_proof *p) {
    return p&&!p->invalid&&p->samples>=3&&p->distinct_worlds>=3&&p->program_mask&&(p->program_mask&(p->program_mask-1));
}
#endif
