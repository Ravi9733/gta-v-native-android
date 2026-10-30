#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN
typedef NS_ENUM(NSUInteger,GtaSourceProbeRole){
    GtaProbeEarlyOpaqueDS=0,GtaProbeLatestDS=1,GtaProbePreLensColor=2,GtaProbeFinalColor=3
};
/* Standalone private diagnostic, default uncreated. Never selects GTA roles.
   Caller must prove admission, producer Store, closed encoders, same ordered
   queue, exact actual commit options, and backing-generation lease. No reused
   texture or source-ID inference here. A session accepts exactly one frame.
   Output root must already exist and be a trusted app-private directory.
   Allocations/encode happen only for an explicit one-frame diagnostic request.
   Completion runs on a private IO queue; no CPU GPU-wait or IO in capture. */
API_AVAILABLE(macos(26.0),ios(26.0))
@interface GtaSourceProbe : NSObject
- (nullable instancetype)initWithQueue:(id<MTL4CommandQueue>)queue
                                frame:(uint64_t)frame
                           outputRoot:(NSURL *)root
                           completion:(void (^)(NSDictionary *result))completion;
- (BOOL)captureRole:(GtaSourceProbeRole)role texture:(id<MTLTexture>)texture
         sourceLease:(id)lease frame:(uint64_t)frame pass:(uint64_t)pass
       producerStore:(BOOL)stored closedEncoders:(BOOL)closed
       commandBuffer:(id<MTL4CommandBuffer>)cb queue:(id<MTL4CommandQueue>)queue
       commitOptions:(MTL4CommitOptions *)options;
/* Seal after all four roles were encoded and final role's actual CB committed.
   No callbacks or writer until all four actual GPU feedback events arrive.
   Caller may abandon an incomplete session; pending commands retain it safely.
   Callbacks may arrive before seal; this is handled without waiting. */
- (BOOL)sealWithMetadata:(NSDictionary *)metadata;
@property(readonly) NSUInteger encodedRoleMask;
@property(readonly) unsigned feedbackRoleCount;
/* Independent owned textures become available only after successful GPU
   feedback for all roles. Order is role0..3; native early source remains260.
   Caller can convert early260->252 off-render with the reviewed primitive.
   Retaining this returned array is an independent lease; no target is reused. */
- (nullable NSArray<id<MTLTexture>> *)completedOwnedTextures;
@end
NS_ASSUME_NONNULL_END
