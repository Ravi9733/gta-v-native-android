#import "GtaSourceProbe.h"
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <CommonCrypto/CommonDigest.h>

/* Fixed limits refer to application-owned image/buffer bytes, not opaque driver
   PSO/descriptor allocations. No pipeline compilation is performed here. Early
   opaque data is copied as native DS260 and its exact float32 depth plane; the
   reviewed DS260->D32 helper may convert this OWNED source later, off render. */
enum{ProbeRoleCount=4,ProbeMaxW=1920,ProbeMaxH=1080};
static const NSUInteger ProbeBudget=96*1024*1024;
@interface GtaSourceProbeSlot:NSObject
@property(strong)id<MTLTexture> source,owned;
@property(strong)id sourceLease;
@property(strong)id<MTLBuffer> depthOrColor,stencil;
@property(strong)id<MTLResidencySet> residency;
@property NSUInteger row,stencilRow;
@property uint64_t pass;
@end
@implementation GtaSourceProbeSlot @end

@implementation GtaSourceProbe {
    id<MTL4CommandQueue> _queue;
    NSURL *_root;
    uint64_t _frame;
    GtaSourceProbeSlot *_slots[ProbeRoleCount];
    NSUInteger _mask,_attemptMask,_bytes,_width,_height;
    NSDictionary *_metadata;
    void(^_completion)(NSDictionary *);
    _Atomic unsigned _done;
    _Atomic bool _sealed,_error,_queued;
}
static dispatch_queue_t probeIO(void){
    static dispatch_queue_t q;static dispatch_once_t once;
    dispatch_once(&once,^{q=dispatch_queue_create("gta.private.source-probe.io",DISPATCH_QUEUE_SERIAL);});return q;
}
static NSString *probeHash(const void *p,NSUInteger n){
    unsigned char h[CC_SHA256_DIGEST_LENGTH];CC_SHA256(p,(CC_LONG)n,h);
    char s[65];for(unsigned i=0;i<32;i++)snprintf(s+i*2,3,"%02x",h[i]);return @(s);
}
static BOOL writeExclusive(int dir,const char *name,const void *bytes,NSUInteger length){
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return NO;
    const uint8_t *p=bytes;NSUInteger left=length;BOOL ok=YES;
    while(left){ssize_t n=write(fd,p,left);if(n<0&&errno==EINTR)continue;if(n<=0){ok=NO;break;}p+=n;left-=(NSUInteger)n;}
    if(close(fd))ok=NO;return ok;
}
- (instancetype)initWithQueue:(id<MTL4CommandQueue>)queue frame:(uint64_t)frame outputRoot:(NSURL *)root completion:(void(^)(NSDictionary *))completion {
    if(!queue||!frame||!root.isFileURL||!root.path.isAbsolutePath||!completion)return nil;
    if(!(self=[super init]))return nil;_queue=queue;_frame=frame;_root=root;_completion=[completion copy];return self;
}
- (NSUInteger)encodedRoleMask{return _mask;}
- (unsigned)feedbackRoleCount{return atomic_load_explicit(&_done,memory_order_acquire);}
- (NSArray<id<MTLTexture>> *)completedOwnedTextures {
    if(!atomic_load_explicit(&_sealed,memory_order_acquire)||atomic_load_explicit(&_done,memory_order_acquire)!=4||
       atomic_load_explicit(&_error,memory_order_acquire))return nil;
    return @[_slots[0].owned,_slots[1].owned,_slots[2].owned,_slots[3].owned];
}
- (BOOL)captureRole:(GtaSourceProbeRole)role texture:(id<MTLTexture>)t sourceLease:(id)lease frame:(uint64_t)frame pass:(uint64_t)pass producerStore:(BOOL)stored closedEncoders:(BOOL)closed commandBuffer:(id<MTL4CommandBuffer>)cb queue:(id<MTL4CommandQueue>)queue commitOptions:(MTL4CommitOptions *)options {
    /* Graphics-owner-thread only. The caller owns role/metadata/command order. */
    if(role>=ProbeRoleCount||!t||!lease||!cb||!options||queue!=_queue||t.device!=_queue.device||cb.device!=t.device||
       frame!=_frame||!pass||!stored||!closed||atomic_load(&_sealed)||(_attemptMask&(1u<<role)))return NO;
    if((role==GtaProbeEarlyOpaqueDS&&_mask)||(role!=GtaProbeEarlyOpaqueDS&&!(_mask&1))||
       (role==GtaProbeFinalColor&&_mask!=7))return NO;
    if(role!=GtaProbeEarlyOpaqueDS&&pass<=_slots[GtaProbeEarlyOpaqueDS].pass)return NO;
    if((role==GtaProbeLatestDS&&_slots[GtaProbePreLensColor]&&pass!=_slots[GtaProbePreLensColor].pass)||
       (role==GtaProbePreLensColor&&_slots[GtaProbeLatestDS]&&pass!=_slots[GtaProbeLatestDS].pass)||
       (role==GtaProbeFinalColor&&pass<=_slots[GtaProbeLatestDS].pass))return NO;
    MTLPixelFormat format=role<=GtaProbeLatestDS?MTLPixelFormatDepth32Float_Stencil8:role==GtaProbePreLensColor?MTLPixelFormatBGRA8Unorm:MTLPixelFormatRGBA8Unorm;
    if(t.pixelFormat!=format||t.textureType!=MTLTextureType2D||t.sampleCount!=1||t.depth!=1||t.arrayLength!=1||t.mipmapLevelCount!=1||
       !t.width||!t.height||t.width>ProbeMaxW||t.height>ProbeMaxH||t.framebufferOnly||t.storageMode!=MTLStorageModePrivate||t.parentTexture||
       (_width&&(_width!=t.width||_height!=t.height)))return NO;
    NSUInteger row=(t.width*4+255)&~(NSUInteger)255,stencilRow=(t.width+255)&~(NSUInteger)255;
    NSUInteger extra=t.width*t.height*(role<=GtaProbeLatestDS?8:4)+row*t.height+(role==GtaProbeLatestDS?stencilRow*t.height:0);
    if(extra>ProbeBudget-_bytes)return NO;
    /* A resource/encoder failure abandons this role for the session. Retrying
       with the same options cannot accumulate unaccounted residency leases. */
    _attemptMask|=1u<<role;_bytes+=extra;
    GtaSourceProbeSlot *s=[GtaSourceProbeSlot new];s.source=t;s.sourceLease=lease;s.pass=pass;s.row=row;s.stencilRow=stencilRow;
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:t.width height:t.height mipmapped:NO];
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    s.owned=[t.device newTextureWithDescriptor:td];s.depthOrColor=[t.device newBufferWithLength:row*t.height options:MTLResourceStorageModeShared];
    if(role==GtaProbeLatestDS)s.stencil=[t.device newBufferWithLength:stencilRow*t.height options:MTLResourceStorageModeShared];
    if(!s.owned||!s.depthOrColor||(role==GtaProbeLatestDS&&!s.stencil))return NO;
    memset(s.depthOrColor.contents,0,s.depthOrColor.length);if(s.stencil)memset(s.stencil.contents,0,s.stencil.length);
    MTLResidencySetDescriptor *rd=[MTLResidencySetDescriptor new];rd.initialCapacity=5;NSError *error=nil;
    s.residency=[t.device newResidencySetWithDescriptor:rd error:&error];if(!s.residency)return NO;
    for(id<MTLAllocation>a in @[t,s.owned,s.depthOrColor])[s.residency addAllocation:a];
    if(s.stencil)[s.residency addAllocation:s.stencil];[s.residency commit];[cb useResidencySet:s.residency];
    id<MTL4ComputeCommandEncoder>en=[cb computeCommandEncoder];
    if(!en){[options addFeedbackHandler:^(id<MTL4CommitFeedback>f){(void)f;(void)s.sourceLease;}];return NO;}
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en copyFromTexture:t toTexture:s.owned];
    /* These two commands both READ original t; there is no intervening writer.
       Readback is not read from the pending clone, avoiding an intra-encoder
       copy-write -> copy-read dependency. Owned clone is independently retained. */
    if(role<=GtaProbeLatestDS){
        [en copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:s.depthOrColor destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:0 options:MTLBlitOptionDepthFromDepthStencil];
        if(s.stencil)[en copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:s.stencil destinationOffset:0 destinationBytesPerRow:stencilRow destinationBytesPerImage:0 options:MTLBlitOptionStencilFromDepthStencil];
    }else [en copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:s.depthOrColor destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:0];
    [en barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];[en endEncoding];
    _slots[role]=s;_width=t.width;_height=t.height;_mask|=1u<<role;
    GtaSourceProbe *leaseSelf=self;
    [options addFeedbackHandler:^(id<MTL4CommitFeedback>f){
        if(!f||f.error)atomic_store_explicit(&leaseSelf->_error,true,memory_order_release);
        atomic_fetch_add_explicit(&leaseSelf->_done,1,memory_order_release);[leaseSelf tryWrite];
    }];return YES;
}
- (BOOL)sealWithMetadata:(NSDictionary *)metadata {
    if(_mask!=15||atomic_load(&_sealed)||!metadata||![NSJSONSerialization isValidJSONObject:metadata])return NO;
    NSData *d=[NSJSONSerialization dataWithJSONObject:metadata options:0 error:NULL];if(!d||d.length>16384)return NO;
    _metadata=[NSJSONSerialization JSONObjectWithData:d options:0 error:NULL];
    if(!_metadata)return NO;atomic_store_explicit(&_sealed,true,memory_order_release);[self tryWrite];return YES;
}
- (void)tryWrite {
    if(!atomic_load_explicit(&_sealed,memory_order_acquire)||atomic_load_explicit(&_done,memory_order_acquire)!=4)return;
    bool no=false;if(!atomic_compare_exchange_strong_explicit(&_queued,&no,true,memory_order_acq_rel,memory_order_relaxed))return;
    dispatch_async(probeIO(),^{@autoreleasepool{[self writeResult];}});
}
- (void)writeResult {
    NSMutableDictionary *result=[@{ @"schema":@1,@"frame":@(_frame),@"width":@(_width),@"height":@(_height),
      @"applicationOwnedLogicalBytes":@(_bytes),@"GPUError":@(atomic_load_explicit(&_error,memory_order_acquire)),
      @"metadata":_metadata,@"verifiedForInterpolation":@NO,@"earlyDepthTextureFormat":@260,
      @"earlyDepthReadbackFormat":@252,@"note":@"Depth readback is the exact float32 plane; native252 conversion is a later off-render step." } mutableCopy];
    NSString *dir=[NSString stringWithFormat:@"probe-%llu-%@",(unsigned long long)_frame,NSUUID.UUID.UUIDString];
    int root=open(_root.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW),child=-1;BOOL ok=root>=0;
    if(ok)ok=mkdirat(root,dir.fileSystemRepresentation,0700)==0;
    if(ok){child=openat(root,dir.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);ok=child>=0;}
    NSMutableArray *planes=[NSMutableArray array];
    const char *names[4]={"early-depth-f32.raw","latest-depth-f32.raw","pre-lens-bgra8.raw","final-rgba8.raw"};
    if(ok&&!atomic_load(&_error))for(NSUInteger role=0;role<4;role++){
        GtaSourceProbeSlot *s=_slots[role];
        for(unsigned plane=0;plane<(s.stencil?2u:1u);plane++){
            id<MTLBuffer>b=plane?s.stencil:s.depthOrColor;NSUInteger row=plane?s.stencilRow:s.row,length=row*_height;
            const char *name=plane?"latest-stencil-u8.raw":names[role];
            BOOL written=writeExclusive(child,name,b.contents,length);ok=ok&&written;
            [planes addObject:@{@"file":@(name),@"role":@(role),@"plane":@(plane),@"rowBytes":@(row),@"length":@(length),
              @"SHA256":probeHash(b.contents,length),@"pass":@(s.pass),@"sourceRID":@(s.source.gpuResourceID._impl),
              @"ownedRID":@(s.owned.gpuResourceID._impl),@"nativeFormat":@(s.source.pixelFormat),@"written":@(written)}];
        }
    }
    result[@"planes"]=planes;result[@"directory"]=dir;result[@"filesOK"]=@(ok&&!atomic_load(&_error));
    if(child>=0){NSData *manifest=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:NULL];
        BOOL manifestOK=manifest&&writeExclusive(child,"manifest.json",manifest.bytes,manifest.length);result[@"manifestWritten"]=@(manifestOK);close(child);
    }else result[@"manifestWritten"]=@NO;
    if(root>=0)close(root);_completion(result);
}
@end
