// SPDX-License-Identifier: MIT
// Explicit offline native Metal API frontend. Limited supported operations;
// never installed or advertised as a complete system/hardware GPU driver.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <CoreVideo/CVPixelBuffer.h>
#import <CommonCrypto/CommonDigest.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include "OfflineMetalWire.h"
#include "OfflineMetalFunctionWire.h"
#include "OfflineMetalQueueWire.h"
#include "OfflineMetalTextureWire.h"
#include "OfflineMetalDepthStencilWire.h"
#include "OfflineMetalVertexWire.h"
#include "OfflineMetalFenceWire.h"
#include "OfflineMetalSharedEventWire.h"
#include <pthread.h>
#include <atomic>
#include <unistd.h>
#include <time.h>
using IOMethod=int (*)(unsigned,unsigned,const uint64_t *,unsigned,const void *,size_t,uint64_t *,unsigned *,void *,size_t *);
static IOMethod ioMethod;
static unsigned connection;
static uint32_t sequence;
static NSRecursiveLock *transaction;
static NSDictionary *deviceInfo;
static id remoteDevice;
static std::atomic<bool> live{false},asyncEnabled{false},closing{false};
static NSLock *pendingLock;
static NSMutableSet *pendingCommands;
static NSMutableSet *pendingNotifications;
static NSException *notificationFailure;
static uint64_t notificationsRegistered,notificationsDelivered,notificationsCancelled,notificationsFailed;
static void fail(NSString *reason) { @throw [NSException exceptionWithName:@"OfflineMetalTransport" reason:reason userInfo:nil]; }
static void receiveBytes(void *bytes,size_t size) {
 unsigned char *p=(unsigned char *)bytes;
 while(size){size_t count=MIN(size,(size_t)4096);int status=ioMethod(connection,1,nullptr,0,nullptr,0,nullptr,nullptr,p,&count);
  if(status || !count || count>size)fail(@"Actual console receive failed");p+=count;size-=count;}
}
static NSData *call(uint16_t operation,const void *input,size_t size,OMResult *result) {
 [transaction lock];
 @try {
  if(!live || size>4080)fail(@"Invalid live transport request");
  OMWireHeader header={OM_WIRE_MAGIC,OFFLINE_METAL_VERSION,operation,(uint32_t)size,sequence+1};
  unsigned char bytes[4096];memcpy(bytes,&header,sizeof(header));if(size)memcpy(bytes+sizeof(header),input,size);
  int sendStatus=ioMethod(connection,0,nullptr,0,bytes,sizeof(header)+size,nullptr,nullptr,nullptr,nullptr);
  if(sendStatus)fail([NSString stringWithFormat:@"Actual transport send failed: %08x",(unsigned)sendStatus]);
  sequence++;
  OMWireHeader reply={};receiveBytes(&reply,sizeof(reply));
  if(reply.magic!=OM_WIRE_MAGIC || reply.version!=OFFLINE_METAL_VERSION || reply.operation!=(operation|OM_WIRE_RESPONSE)
     || reply.request!=sequence || reply.bytes<sizeof(*result) || reply.bytes-sizeof(*result)>OFFLINE_METAL_MAX_REPLY)fail(@"Invalid actual Metal reply");
  receiveBytes(result,sizeof(*result));if(result->reserved)fail(@"Unknown reply flags");
  NSMutableData *body=[NSMutableData dataWithLength:reply.bytes-sizeof(*result)];if(body.length)receiveBytes(body.mutableBytes,body.length);
  return body;
 } @finally {[transaction unlock];}
}
static NSData *checked(uint16_t operation,const void *input,size_t size,OMResult *result) {
 NSData *body=call(operation,input,size,result);if(result->error)fail([[NSString alloc] initWithData:body encoding:NSUTF8StringEncoding]?:@"Unsupported or failed actual Metal operation");return body;
}
static NSError *operationError(OMResult result,NSData *body) {
 NSString *why=[[NSString alloc] initWithData:body encoding:NSUTF8StringEncoding]?:@"Actual offline backend refused this operation";
 return [NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:result.error userInfo:@{NSLocalizedDescriptionKey:why}];
}
@interface OMObject : NSObject
@property(readonly) id<MTLDevice> device;
@property(nonatomic) uint64_t handle;
@property(atomic,copy) NSString *label;
@end
@implementation OMObject
- (id<MTLDevice>)device {return remoteDevice;}
- (void)dealloc {
 // Actual host Metal objects retain resources required by their recorded
 // command buffers. Drop exactly this proxy's independent table reference.
 // Session teardown releases any remaining references after real commits.
 [transaction lock];
 @try {if(live && !closing && _handle){OMHandle request={_handle};OMResult result={};checked(OM_RELEASE,&request,sizeof(request),&result);}}
 @finally {[transaction unlock];}
}
@end
@interface OMFenceObject : OMObject<MTLFence>
@end
@implementation OMFenceObject
@end
@interface OMEventObject : OMObject<MTLEvent>
@end
@implementation OMEventObject
@end
// These selectors are read by the unchanged target handle constructor. The
// eventPort is a real Mach capability created by the native kernel broker.
@interface MTLSharedEventHandle (OMOriginalSharedHandle)
- (instancetype)initWithSharedEvent:(id)event;
- (mach_port_t)eventPort;
- (uint64_t)labelTraceID;
@end
static NSDictionary *sharedMetadata(NSData *body) {
 NSDictionary *info=[NSJSONSerialization JSONObjectWithData:body options:0 error:nil];
 if(![info isKindOfClass:[NSDictionary class]] || ![info[@"device_is_nil"] isKindOfClass:[NSNumber class]] ||
    ![info[@"device_is_owner"] isKindOfClass:[NSNumber class]] ||
    ([info[@"device_is_nil"] boolValue]==[info[@"device_is_owner"] boolValue]) ||
    ![info[@"signaled_value"] isKindOfClass:[NSNumber class]] ||
    (info[@"label"]!=[NSNull null] && ![info[@"label"] isKindOfClass:[NSString class]]))fail(@"Invalid actual shared-event metadata");
 return info;
}
enum {OMNotificationPending,OMNotificationQueued,OMNotificationRunning,OMNotificationFinished,OMNotificationCancelled};
@interface OMSharedNotificationState : NSObject
@property(nonatomic) uint64_t handle,value;
@property(nonatomic,strong) id<MTLSharedEvent> event;
@property(nonatomic,strong) MTLSharedEventListener *listener;
@property(nonatomic,strong) dispatch_queue_t queue;
@property(nonatomic,strong) dispatch_group_t callbackGroup;
@property(nonatomic,copy) MTLSharedEventNotificationBlock block;
// All state transitions and real callback thread identities use pendingLock.
@property(nonatomic) unsigned phase;
@property(nonatomic) pthread_t callbackThread;
- (void)start;
@end
@implementation OMSharedNotificationState
- (void)start {
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool {
  @try {
   struct timespec start={},now={};if(clock_gettime(CLOCK_MONOTONIC,&start))fail(@"Actual listener clock unavailable");
   for(;;) {
    [pendingLock lock];BOOL cancelled=self.phase==OMNotificationCancelled;[pendingLock unlock];if(cancelled)return;
    OMSharedNotification reply={};
    [transaction lock];@try {
     if(!live || closing)return;
     OMHandle input={self.handle};OMResult result={};NSData *body=checked(OM_SHARED_NOTIFICATION_POLL,&input,sizeof(input),&result);
     if(body.length!=sizeof(reply))fail(@"Invalid actual listener reply");memcpy(&reply,body.bytes,sizeof(reply));
    } @finally {[transaction unlock];}
    if(reply.ready) {
     if(reply.ready!=1 || reply.event!=[(OMObject *)self.event handle] || reply.value<self.value)fail(@"Actual listener event/value mismatch");
     [pendingLock lock];if(closing || self.phase==OMNotificationCancelled){[pendingLock unlock];return;}self.phase=OMNotificationQueued;[pendingLock unlock];
     uint64_t actualValue=reply.value;
     // Only the real host Metal listener's ready record reaches this queue.
     // The queue is read from the unchanged original target listener object.
     dispatch_async(self.queue,^{@autoreleasepool {
      [pendingLock lock];if(closing || self.phase==OMNotificationCancelled){[pendingLock unlock];return;}
      self.phase=OMNotificationRunning;self.callbackThread=pthread_self();dispatch_group_enter(self.callbackGroup);[pendingLock unlock];
      @try {self.block(self.event,actualValue);}
      @catch(NSException *exception){[pendingLock lock];if(!notificationFailure)notificationFailure=exception;notificationsFailed++;[pendingLock unlock];}
      @finally {[pendingLock lock];self.callbackThread=nullptr;self.phase=OMNotificationFinished;notificationsDelivered++;[pendingNotifications removeObject:self];[pendingLock unlock];dispatch_group_leave(self.callbackGroup);}
     }});return;
    }
    if(clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec-start.tv_sec>=60)fail(@"Bounded actual listener observation timed out");usleep(1000);
   }
  } @catch(NSException *exception) {
   [pendingLock lock];if(self.phase!=OMNotificationCancelled){if(!notificationFailure)notificationFailure=exception;notificationsFailed++;self.phase=OMNotificationFinished;[pendingNotifications removeObject:self];}[pendingLock unlock];
  }
 }});
}
@end
@interface OMSharedEventObject : OMEventObject<MTLSharedEvent> {
 mach_port_t _exportPort;
 uint64_t _traceID;
}
@property(nonatomic) BOOL deviceIsNil;
- (mach_port_t)eventPort;
- (uint64_t)labelTraceID;
@end
@implementation OMSharedEventObject
- (id<MTLDevice>)device {return self.deviceIsNil?nil:remoteDevice;}
- (void)dealloc {if(_exportPort)mach_port_deallocate(mach_task_self(),_exportPort);}
- (uint64_t)signaledValue {
 OMHandle input={self.handle};OMResult result={};NSData *body=checked(OM_SHARED_VALUE_GET,&input,sizeof(input),&result);
 if(body.length!=8)fail(@"Invalid actual event counter");uint64_t value=0;memcpy(&value,body.bytes,8);return value;
}
- (void)setSignaledValue:(uint64_t)value {
 OMSharedEventValue input={self.handle,value};OMResult result={};NSData *body=checked(OM_SHARED_VALUE_SET,&input,sizeof(input),&result);
 if(body.length!=8)fail(@"Missing actual event counter write result");
}
- (void)setLabel:(NSString *)label {
 if(label && ![label isKindOfClass:[NSString class]])fail(@"Invalid shared-event label");
 NSData *text=[label dataUsingEncoding:NSUTF8StringEncoding];if(text.length>512)fail(@"Excessive shared-event label");
 OMSharedLabel input={self.handle,(uint64_t)(label!=nil),text.length};NSMutableData *data=[NSMutableData dataWithBytes:&input length:sizeof(input)];if(text)[data appendData:text];
 [transaction lock];@try {
  OMResult result={};NSData *body=checked(OM_SHARED_LABEL_SET,data.bytes,data.length,&result);NSDictionary *info=[NSJSONSerialization JSONObjectWithData:body options:0 error:nil];
  id actual=info[@"label"];if(actual!=[NSNull null] && ![actual isKindOfClass:[NSString class]])fail(@"Missing actual label result");
  [super setLabel:actual==[NSNull null]?nil:actual];
  if(_exportPort){mach_port_deallocate(mach_task_self(),_exportPort);_exportPort=0;_traceID=0;}
 } @finally {[transaction unlock];}
}
- (mach_port_t)eventPort {
 [transaction lock];@try {
  if(!live || closing)fail(@"Shared export on a closing context");if(_exportPort)return _exportPort;
  uint64_t output=0,input=self.handle;unsigned count=1;unsigned char bytes[4096];size_t size=sizeof(bytes);
  int status=ioMethod(connection,2,&input,1,nullptr,0,&output,&count,bytes,&size);
  if(status || count!=1 || !output || output>UINT32_MAX || output==MACH_PORT_DEAD)fail(@"Native shared-event capability export refused");
  NSDictionary *info=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:bytes length:size] options:0 error:nil];
  if(![info[@"trace_id_supported"] boolValue] || ![info[@"trace_id"] isKindOfClass:[NSNumber class]]){mach_port_deallocate(mach_task_self(),(mach_port_t)output);fail(@"Unqualified actual shared-event trace ABI");}
  _exportPort=(mach_port_t)output;_traceID=[info[@"trace_id"] unsignedLongLongValue];return _exportPort;
 } @finally {[transaction unlock];}
}
- (uint64_t)labelTraceID {(void)self.eventPort;return _traceID;}
- (MTLSharedEventHandle *)newSharedEventHandle {
 Class cls=NSClassFromString(@"MTLSharedEventHandle");if(!cls || class_getInstanceSize(cls)!=16)fail(@"Wrong original shared-handle ABI");
 return [[cls alloc] initWithSharedEvent:self];
}
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
 Class cls=NSClassFromString(@"MTLSharedEventListenerInternal");Method getter=class_getInstanceMethod(cls,@selector(dispatchQueue));
 if(!listener || !block || object_getClass(listener)!=cls || class_getInstanceSize(cls)!=24 || !getter || strcmp(method_getTypeEncoding(getter),"@16@0:8"))fail(@"Unqualified original shared listener ABI");
 dispatch_queue_t queue=listener.dispatchQueue;if(!queue)fail(@"Missing actual original listener queue");
 [transaction lock];@try {
  if(!live || closing)fail(@"Listener registration on a closing context");OMSharedEventValue input={self.handle,value};OMResult result={};checked(OM_SHARED_NOTIFY,&input,sizeof(input),&result);if(!result.handle)fail(@"Missing actual listener registration");
  OMSharedNotificationState *state=[OMSharedNotificationState new];state.handle=result.handle;state.value=value;state.event=self;state.listener=listener;state.queue=queue;state.block=block;state.callbackGroup=dispatch_group_create();
  [pendingLock lock];if(closing){[pendingLock unlock];fail(@"Listener registration raced context finish");}[pendingNotifications addObject:state];notificationsRegistered++;[pendingLock unlock];[state start];
 } @finally {[transaction unlock];}
}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {
 struct timespec start={},now={};if(clock_gettime(CLOCK_MONOTONIC,&start))fail(@"Actual monotonic clock unavailable");
 for(;;){if(self.signaledValue>=value)return YES;if(clock_gettime(CLOCK_MONOTONIC,&now))fail(@"Actual monotonic clock unavailable");
  uint64_t elapsed=(uint64_t)(now.tv_sec-start.tv_sec)*1000;
  if(now.tv_nsec>=start.tv_nsec)elapsed+=(now.tv_nsec-start.tv_nsec)/1000000;
  else elapsed-=(start.tv_nsec-now.tv_nsec+999999)/1000000;
  if(elapsed>=milliseconds)return NO;usleep(1000);}
}
@end
@interface OMFunctionObject : OMObject
@property(nonatomic,strong) NSString *name;
@property(nonatomic) MTLFunctionType functionType;
@end
@implementation OMFunctionObject
@end
@interface MTLFunctionConstantValues (OMOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol OMOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end
static bool methodABI(id object,SEL selector,const char *encoding) {
 Method method=class_getInstanceMethod(object_getClass(object),selector);
 return method && !strcmp(method_getTypeEncoding(method),encoding);
}
static NSError *constantError(NSString *reason) {
 return [NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:-4 userInfo:@{NSLocalizedDescriptionKey:reason}];
}
@interface OMLibraryObject : OMObject
@property(nonatomic,strong) NSArray<NSString *> *functionNames;
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error;
@end
@implementation OMLibraryObject
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
 if(!name || [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>=128)return nil;
 OMFunction request={};request.library=self.handle;strcpy(request.name,name.UTF8String);
 OMResult result={};NSData *body=call(OM_FUNCTION,&request,sizeof(request),&result);if(result.error)return nil;
 NSDictionary *info=[NSJSONSerialization JSONObjectWithData:body options:0 error:nil];if(![info isKindOfClass:[NSDictionary class]])fail(@"Actual function metadata missing");
 OMFunctionObject *object=[OMFunctionObject new];object.handle=result.handle;object.name=info[@"name"];object.functionType=(MTLFunctionType)[info[@"type"] unsignedIntegerValue];
 return (id<MTLFunction>)object;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 auto refuse=[&](NSString *why)->id<MTLFunction> {if(error)*error=constantError(why);return nil;};
 if(!name.length || [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>=128 ||
     object_getClass(values)!=NSClassFromString(@"MTLFunctionConstantValuesInternal") ||
     !methodABI(values,@selector(newIndexedConstantArray),"@16@0:8") || !methodABI(values,@selector(newNamedConstantArray),"@16@0:8"))
  return refuse(@"Unqualified actual function-constant object or name");
 // Read through the original target's real accessors. Never replace setters,
 // interpret opaque storage, or substitute unspecialized shader functions.
 MTLFunctionConstantValues *snapshot=[values copy];
 NSArray *indexed=[snapshot newIndexedConstantArray],*named=[snapshot newNamedConstantArray];
 if(![indexed isKindOfClass:[NSArray class]] || ![named isKindOfClass:[NSArray class]] || indexed.count+named.count>OFFLINE_METAL_MAX_CONSTANTS)
  return refuse(@"Unqualified or excessive original constants");
 OMSpecializedFunction request={};request.function.library=self.handle;strcpy(request.function.name,name.UTF8String);request.count=indexed.count+named.count;
 NSMutableData *packet=[NSMutableData dataWithBytes:&request length:sizeof(request)];
 for(unsigned group=0;group<2;group++)for(id value in (group?named:indexed)) {
  if(object_getClass(value)!=NSClassFromString(group?@"MTLNamedConstantValue":@"MTLIndexedConstantValue") ||
      !methodABI(value,@selector(dataType),"Q16@0:8") || !methodABI(value,@selector(data),"^v16@0:8") ||
      !methodABI(value,group?@selector(name):@selector(index),group?"@16@0:8":"Q16@0:8"))
   return refuse(@"Unqualified actual constant descriptor ABI");
  id<OMOriginalConstantValue> descriptor=value;
  OMFunctionConstant row={};row.type=[descriptor dataType];row.bytes=OMFunctionConstantBytes(row.type);row.named=group;
  const void *data=[descriptor data];if(!row.bytes || !data)return refuse(@"Unsupported original constant type or missing actual value");
  if(group) {
   NSString *key=[descriptor name];if(![key isKindOfClass:[NSString class]] || !key.length || [key lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>=128){
    return refuse(@"Invalid actual named constant");}strcpy(row.name,key.UTF8String);
  } else {row.index=[descriptor index];if(row.index>65535)return refuse(@"Original constant index exceeds qualified range");}
  memcpy(row.data,data,row.bytes);[packet appendBytes:&row length:sizeof(row)];
 }
 OMResult result={};NSData *body=call(OM_FUNCTION,packet.bytes,packet.length,&result);
 if(result.error){if(error)*error=operationError(result,body);return nil;}
 NSDictionary *info=[NSJSONSerialization JSONObjectWithData:body options:0 error:nil];if(![info isKindOfClass:[NSDictionary class]])fail(@"Actual specialized function metadata missing");
 OMFunctionObject *object=[OMFunctionObject new];object.handle=result.handle;object.name=info[@"name"];object.functionType=(MTLFunctionType)[info[@"type"] unsignedIntegerValue];
 if(error)*error=nil;return (id<MTLFunction>)object;
}
@end
@interface OMTextureObject : OMObject
@property(nonatomic) OMTexture descriptor;
@property(nonatomic) MTLHazardTrackingMode actualHazardMode;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) uint32_t surfaceSeed;
@property(nonatomic) BOOL surfaceSeedValid;
- (void)uploadSurface;
- (void)downloadSurface;
- (void)getBytes:(void *)pixels bytesPerRow:(NSUInteger)stride fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level;
@end
@implementation OMTextureObject
- (void)dealloc {if(_surface)CFRelease(_surface);}
- (IOSurfaceRef)iosurface {return self.surface;}
- (NSUInteger)iosurfacePlane {return 0;}
- (void)uploadSurface {
 @synchronized(self) {
 if(!self.surface)return;uint32_t seed=0;
 if(IOSurfaceLock(self.surface,kIOSurfaceLockReadOnly,&seed))fail(@"Actual IOSurface read lock failed");
 @try {
  if(self.surfaceSeedValid && seed==self.surfaceSeed)return;
  const unsigned char *base=(const unsigned char *)IOSurfaceGetBaseAddress(self.surface);size_t row=IOSurfaceGetBytesPerRow(self.surface);
  if(!base)fail(@"Missing actual IOSurface mapping");
  for(uint64_t y=0;y<self.descriptor.height;) {
   uint64_t width=MIN(self.descriptor.width,(4080-sizeof(OMWriteTexture))/4);
   uint64_t height=MIN(self.descriptor.height-y,(4080-sizeof(OMWriteTexture))/(width*4));
   for(uint64_t x=0;x<self.descriptor.width;x+=width) {
    uint64_t part=MIN(width,self.descriptor.width-x);OMWriteTexture request={self.handle,x,y,part,height,0,part*4};
    unsigned char packet[4080];memcpy(packet,&request,sizeof(request));
    for(uint64_t j=0;j<height;j++)memcpy(packet+sizeof(request)+j*request.stride,base+(y+j)*row+x*4,request.stride);
    OMResult result={};checked(OM_WRITE_TEXTURE,packet,sizeof(request)+request.stride*height,&result);
   }
   y+=height;
  }
  self.surfaceSeed=seed;self.surfaceSeedValid=YES;
 } @finally {if(IOSurfaceUnlock(self.surface,kIOSurfaceLockReadOnly,nullptr))fail(@"Actual IOSurface read unlock failed");}
 }
}
- (void)downloadSurface {
 @synchronized(self) {
 if(!self.surface)return;if(IOSurfaceLock(self.surface,0,nullptr))fail(@"Actual IOSurface write lock failed");
 @try {
  void *base=IOSurfaceGetBaseAddress(self.surface);if(!base)fail(@"Missing actual IOSurface write mapping");
  [self getBytes:base bytesPerRow:IOSurfaceGetBytesPerRow(self.surface) fromRegion:MTLRegionMake2D(0,0,self.descriptor.width,self.descriptor.height) mipmapLevel:0];
 } @finally {if(IOSurfaceUnlock(self.surface,0,nullptr))fail(@"Actual IOSurface write unlock failed");}
 self.surfaceSeed=IOSurfaceGetSeed(self.surface);self.surfaceSeedValid=YES;
 }
}
- (NSUInteger)width {return self.descriptor.width;}
- (NSUInteger)height {return self.descriptor.height;}
- (NSUInteger)depth {return self.descriptor.depth;}
- (NSUInteger)mipmapLevelCount {return self.descriptor.levels;}
- (NSUInteger)sampleCount {return self.descriptor.samples;}
- (NSUInteger)arrayLength {return self.descriptor.array;}
- (MTLTextureType)textureType {return (MTLTextureType)self.descriptor.type;}
- (MTLPixelFormat)pixelFormat {return (MTLPixelFormat)self.descriptor.format;}
- (MTLTextureUsage)usage {return self.descriptor.usage;}
- (MTLHazardTrackingMode)hazardTrackingMode {return self.actualHazardMode;}
- (MTLResourceOptions)resourceOptions {return (self.descriptor.storage<<MTLResourceStorageModeShift)|(self.descriptor.cache<<MTLResourceCPUCacheModeShift)|((uint64_t)self.actualHazardMode<<MTLResourceHazardTrackingModeShift);}
- (MTLStorageMode)storageMode {return (MTLStorageMode)self.descriptor.storage;}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)self.descriptor.cache;}
- (void)getBytes:(void *)pixels bytesPerRow:(NSUInteger)stride fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level {
 if(!pixels || region.origin.z || region.size.depth!=1 || !region.size.width || !region.size.height || region.size.width>SIZE_MAX/4
    || stride<region.size.width*4 || stride>SIZE_MAX/region.size.height)fail(@"Unsupported texture readback layout");
 OMReadTexture request={self.handle,region.origin.x,region.origin.y,region.size.width,region.size.height,level};OMResult result={};
 NSData *body=checked(OM_READ_TEXTURE,&request,sizeof(request),&result);
 if(region.size.width>SIZE_MAX/4/region.size.height || body.length!=region.size.width*region.size.height*4)fail(@"Actual pixel extent differs");
 for(NSUInteger row=0;row<region.size.height;row++)memcpy((unsigned char *)pixels+row*stride,(const unsigned char *)body.bytes+row*region.size.width*4,region.size.width*4);
}
@end
@interface OMPipelineObject : OMObject
@end
@implementation OMPipelineObject
@end
@interface OMDepthStateObject : OMObject
@end
@implementation OMDepthStateObject
@end
@interface OMBufferObject : OMObject
@property(nonatomic,strong) NSMutableData *data;
- (void)upload;
- (void)download;
@end
@implementation OMBufferObject
- (NSUInteger)length {return self.data.length;}
- (void *)contents {return self.data.mutableBytes;}
- (MTLStorageMode)storageMode {return MTLStorageModeShared;}
- (MTLCPUCacheMode)cpuCacheMode {return MTLCPUCacheModeDefaultCache;}
- (MTLResourceOptions)resourceOptions {return MTLResourceStorageModeShared;}
- (MTLHazardTrackingMode)hazardTrackingMode {return MTLHazardTrackingModeDefault;}
- (void)didModifyRange:(NSRange)range {if(range.location>self.data.length || range.length>self.data.length-range.location)fail(@"Invalid native buffer modification range");}
- (void)upload {
 for(NSUInteger offset=0;offset<self.data.length;) {
  NSUInteger bytes=MIN(self.data.length-offset,(NSUInteger)(4080-sizeof(OMBufferRegion)));
  OMBufferRegion request={self.handle,offset,bytes};unsigned char packet[4080];memcpy(packet,&request,sizeof(request));
  memcpy(packet+sizeof(request),(const unsigned char *)self.data.bytes+offset,bytes);OMResult result={};
  checked(OM_WRITE_BUFFER,packet,sizeof(request)+bytes,&result);offset+=bytes;
 }
}
- (void)download {
 for(NSUInteger offset=0;offset<self.data.length;) {
  NSUInteger bytes=MIN(self.data.length-offset,(NSUInteger)OFFLINE_METAL_MAX_REPLY);OMBufferRegion request={self.handle,offset,bytes};OMResult result={};
  NSData *body=checked(OM_READ_BUFFER,&request,sizeof(request),&result);if(body.length!=bytes)fail(@"Incomplete actual GPU buffer readback");
  memcpy((unsigned char *)self.data.mutableBytes+offset,body.bytes,bytes);offset+=bytes;
 }
}
@end
@protocol OMTextureDrawable <MTLDrawable>
- (id<MTLTexture>)texture;
@end
@interface OMCommandObject : OMObject
@property(nonatomic,strong) id<MTLCommandQueue> commandQueue;
@property(atomic) OMCommandResult actualResult;
@property(nonatomic) BOOL useAsync;
@property(atomic) BOOL submissionStarted;
@property(atomic) BOOL resourcesVisible;
@property(atomic) pthread_t callbackThread;
@property(atomic,strong) NSException *asynchronousFailure;
@property(atomic,strong) NSException *deliveryFailure;
- (void)commitAsync;
@property(nonatomic) MTLCommandBufferStatus actualEnqueueStatus;
@property(nonatomic,strong) NSMutableArray *completedHandlers;
@property(nonatomic,strong) NSMutableArray *scheduledHandlers;
@property(atomic,strong) dispatch_group_t callbackGroup;
@property(atomic,strong) dispatch_group_t scheduledGroup;
@property(atomic,strong) dispatch_group_t visibilityGroup;
@property(atomic) BOOL scheduledObserved;
@property(atomic) MTLCommandBufferStatus actualScheduledStatus;
@property(nonatomic,strong) NSMutableArray<OMTextureObject *> *surfaceTargets;
@property(nonatomic,strong) NSMutableArray<OMBufferObject *> *buffers;
@property(nonatomic,strong) NSMutableArray<OMTextureObject *> *textures;
@property(nonatomic,strong) NSMutableArray<id<MTLDrawable>> *presentations;
@property(nonatomic,strong) NSMutableArray<OMFenceObject *> *fences;
@property(nonatomic,strong) NSMutableArray<OMEventObject *> *events;
- (void)trackFence:(id<MTLFence>)fence;
- (void)trackBuffer:(OMBufferObject *)buffer;
- (void)trackTexture:(OMTextureObject *)texture;
- (void)trackSurfaceTarget:(OMTextureObject *)texture;
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor;
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;
- (void)commit;
- (void)waitUntilCompleted;
@end
@interface OMEncoderObject : OMObject
@property(nonatomic,strong) OMCommandObject *command;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,OMBufferObject *> *vertexBuffers;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,OMBufferObject *> *fragmentBuffers;
@end
@implementation OMEncoderObject
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages {
 if(![(id)fence isKindOfClass:[OMFenceObject class]] || fence.device!=remoteDevice)fail(@"Foreign render fence");
 OMFenceDependency request={self.handle,[(OMObject *)fence handle],(uint64_t)stages};OMResult result={};checked(OM_RENDER_FENCE_UPDATE,&request,sizeof(request),&result);[self.command trackFence:fence];
}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages {
 if(![(id)fence isKindOfClass:[OMFenceObject class]] || fence.device!=remoteDevice)fail(@"Foreign render fence");
 OMFenceDependency request={self.handle,[(OMObject *)fence handle],(uint64_t)stages};OMResult result={};checked(OM_RENDER_FENCE_WAIT,&request,sizeof(request),&result);[self.command trackFence:fence];
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state {
 if(state && ![(id)state isKindOfClass:[OMDepthStateObject class]])fail(@"Foreign depth/stencil state");
 OMSetDepthStencil request={self.handle,state?[(OMObject *)state handle]:0};OMResult result={};checked(OM_SET_DEPTH_STENCIL,&request,sizeof(request),&result);
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back {
 OMStencilReference request={self.handle,front,back};OMResult result={};checked(OM_STENCIL_REFERENCE,&request,sizeof(request),&result);
}
- (void)setStencilReferenceValue:(uint32_t)value {[self setStencilFrontReferenceValue:value backReferenceValue:value];}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline {
 if(![(id)pipeline isKindOfClass:[OMPipelineObject class]])fail(@"Foreign pipeline object");
 OMSetPipeline request={self.handle,[(OMObject *)pipeline handle]};OMResult result={};checked(OM_SET_PIPELINE,&request,sizeof(request),&result);
}
- (void)setCullMode:(MTLCullMode)mode {OMSetCull request={self.handle,(uint64_t)mode};OMResult result={};checked(OM_SET_CULL,&request,sizeof(request),&result);}
- (void)setViewport:(MTLViewport)viewport {
 OMViewport request={self.handle,viewport.originX,viewport.originY,viewport.width,viewport.height,viewport.znear,viewport.zfar};OMResult result={};checked(OM_VIEWPORT,&request,sizeof(request),&result);
}
- (void)setScissorRect:(MTLScissorRect)rect {OMScissor request={self.handle,rect.x,rect.y,rect.width,rect.height};OMResult result={};checked(OM_SCISSOR,&request,sizeof(request),&result);}
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha {
 OMBlendColor request={self.handle,red,green,blue,alpha};OMResult result={};checked(OM_BLEND_COLOR,&request,sizeof(request),&result);
}
- (void)setTexture:(id<MTLTexture>)texture index:(NSUInteger)index vertex:(BOOL)vertex {
 if(texture && ![(id)texture isKindOfClass:[OMTextureObject class]])fail(@"Foreign texture object");
 OMSetTexture request={self.handle,texture?[(OMObject *)texture handle]:0,index};OMResult result={};
 checked(vertex?OM_VERTEX_TEXTURE:OM_FRAGMENT_TEXTURE,&request,sizeof(request),&result);
 if(texture)[self.command trackTexture:(OMTextureObject *)texture];
}
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {[self setTexture:texture index:index vertex:YES];}
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {[self setTexture:texture index:index vertex:NO];}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count {
 OMDraw request={self.handle,(uint64_t)type,start,count,1};OMResult result={};checked(OM_DRAW,&request,sizeof(request),&result);
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index vertex:(BOOL)vertex {
 if(index>=31 || (buffer && ![(id)buffer isKindOfClass:[OMBufferObject class]]))fail(@"Foreign buffer object or invalid binding index");
 OMSetBuffer request={self.handle,buffer?[(OMObject *)buffer handle]:0,offset,index};OMResult result={};
 checked(vertex?OM_VERTEX_BUFFER:OM_FRAGMENT_BUFFER,&request,sizeof(request),&result);
 if(vertex && !self.vertexBuffers)self.vertexBuffers=[NSMutableDictionary dictionary];
 if(!vertex && !self.fragmentBuffers)self.fragmentBuffers=[NSMutableDictionary dictionary];
 NSMutableDictionary *bindings=vertex?self.vertexBuffers:self.fragmentBuffers;
 if(buffer)bindings[@(index)]=buffer;else [bindings removeObjectForKey:@(index)];
 if(buffer)[self.command trackBuffer:(OMBufferObject *)buffer];
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {[self setBuffer:buffer offset:offset index:index vertex:YES];}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {[self setBuffer:buffer offset:offset index:index vertex:NO];}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
 OMBufferObject *buffer=self.vertexBuffers[@(index)];if(!buffer)fail(@"No actual vertex buffer bound at this index");
 [self setVertexBuffer:(id<MTLBuffer>)buffer offset:offset atIndex:index];
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
 OMBufferObject *buffer=self.fragmentBuffers[@(index)];if(!buffer)fail(@"No actual fragment buffer bound at this index");
 [self setFragmentBuffer:(id<MTLBuffer>)buffer offset:offset atIndex:index];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
 id<MTLBuffer> buffer=[remoteDevice newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
 if(!buffer)fail(@"Invalid vertex-byte allocation");[self setVertexBuffer:buffer offset:0 atIndex:index];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
 id<MTLBuffer> buffer=[remoteDevice newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
 if(!buffer)fail(@"Invalid fragment-byte allocation");[self setFragmentBuffer:buffer offset:0 atIndex:index];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)buffer indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance {
 if(![(id)buffer isKindOfClass:[OMBufferObject class]])fail(@"Foreign index buffer");
 OMDrawIndexed request={self.handle,(uint64_t)type,count,(uint64_t)indexType,[(OMObject *)buffer handle],offset,instances,baseVertex,baseInstance};OMResult result={};
 checked(OM_DRAW_INDEXED,&request,sizeof(request),&result);[self.command trackBuffer:(OMBufferObject *)buffer];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)buffer indexBufferOffset:(NSUInteger)offset {
 [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:buffer indexBufferOffset:offset instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)buffer indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances {
 [self drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:buffer indexBufferOffset:offset instanceCount:instances baseVertex:0 baseInstance:0];
}
- (void)endEncoding {OMHandle request={self.handle};OMResult result={};checked(OM_END,&request,sizeof(request),&result);}
@end
@interface OMBlitEncoderObject : OMEncoderObject
@end
@implementation OMBlitEncoderObject
- (void)updateFence:(id<MTLFence>)fence {
 if(![(id)fence isKindOfClass:[OMFenceObject class]] || fence.device!=remoteDevice)fail(@"Foreign blit fence");
 OMFenceDependency request={self.handle,[(OMObject *)fence handle],0};OMResult result={};checked(OM_BLIT_FENCE_UPDATE,&request,sizeof(request),&result);[self.command trackFence:fence];
}
- (void)waitForFence:(id<MTLFence>)fence {
 if(![(id)fence isKindOfClass:[OMFenceObject class]] || fence.device!=remoteDevice)fail(@"Foreign blit fence");
 OMFenceDependency request={self.handle,[(OMObject *)fence handle],0};OMResult result={};checked(OM_BLIT_FENCE_WAIT,&request,sizeof(request),&result);[self.command trackFence:fence];
}
- (void)copyFromTexture:(id<MTLTexture>)source sourceSlice:(NSUInteger)sourceSlice sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin sourceSize:(MTLSize)sourceSize
    toTexture:(id<MTLTexture>)destination destinationSlice:(NSUInteger)destinationSlice destinationLevel:(NSUInteger)destinationLevel destinationOrigin:(MTLOrigin)destinationOrigin {
 if(![(id)source isKindOfClass:[OMTextureObject class]] || ![(id)destination isKindOfClass:[OMTextureObject class]])fail(@"Foreign blit texture");
 OMCopyTexture request={self.handle,[(OMObject *)source handle],[(OMObject *)destination handle],sourceSlice,sourceLevel,
  {sourceOrigin.x,sourceOrigin.y,sourceOrigin.z},{sourceSize.width,sourceSize.height,sourceSize.depth},destinationSlice,destinationLevel,
  {destinationOrigin.x,destinationOrigin.y,destinationOrigin.z}};
 OMResult result={};checked(OM_COPY_TEXTURE,&request,sizeof(request),&result);
 [self.command trackTexture:(OMTextureObject *)source];[self.command trackSurfaceTarget:(OMTextureObject *)destination];
}
- (void)copyFromTexture:(id<MTLTexture>)source toTexture:(id<MTLTexture>)destination {
 [self copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(source.width,source.height,source.depth)
     toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
}
- (void)copyFromBuffer:(id<MTLBuffer>)source sourceOffset:(NSUInteger)sourceOffset toBuffer:(id<MTLBuffer>)destination destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size {
 if(![(id)source isKindOfClass:[OMBufferObject class]] || ![(id)destination isKindOfClass:[OMBufferObject class]])fail(@"Foreign blit buffer");
 OMCopyBuffer request={self.handle,[(OMObject *)source handle],sourceOffset,[(OMObject *)destination handle],destinationOffset,size};OMResult result={};
 checked(OM_COPY_BUFFER,&request,sizeof(request),&result);
 [self.command trackBuffer:(OMBufferObject *)source];[self.command trackBuffer:(OMBufferObject *)destination];
}
@end
@implementation OMCommandObject
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {
 if(self.submissionStarted || self.actualResult.status || ![(id)event isKindOfClass:[OMEventObject class]])fail(@"Unqualified event or submitted command");
 OMEventDependency request={self.handle,[(OMObject *)event handle],value};OMResult result={};checked(OM_EVENT_SIGNAL,&request,sizeof(request),&result);
 if(!self.events)self.events=[NSMutableArray array];if([self.events indexOfObjectIdenticalTo:event]==NSNotFound)[self.events addObject:(OMEventObject *)event];
}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {
 if(self.submissionStarted || self.actualResult.status || ![(id)event isKindOfClass:[OMEventObject class]])fail(@"Unqualified event or submitted command");
 OMEventDependency request={self.handle,[(OMObject *)event handle],value};OMResult result={};checked(OM_EVENT_WAIT,&request,sizeof(request),&result);
 if(!self.events)self.events=[NSMutableArray array];if([self.events indexOfObjectIdenticalTo:event]==NSNotFound)[self.events addObject:(OMEventObject *)event];
}
- (void)commitAsync {
 if(self.submissionStarted || self.actualResult.status)fail(@"Command already submitted");
 dispatch_group_t group=dispatch_group_create();dispatch_group_enter(group);self.callbackGroup=group;
 self.scheduledGroup=dispatch_group_create();self.visibilityGroup=dispatch_group_create();dispatch_group_enter(self.scheduledGroup);dispatch_group_enter(self.visibilityGroup);
 [pendingLock lock];
 if(closing || !live){[pendingLock unlock];dispatch_group_leave(self.scheduledGroup);dispatch_group_leave(self.visibilityGroup);dispatch_group_leave(group);fail(@"Context is closing");}
 self.submissionStarted=YES;[pendingCommands addObject:self];[pendingLock unlock];
 @try {
  for(OMBufferObject *buffer in self.buffers)[buffer upload];
  for(OMTextureObject *texture in self.textures)[texture uploadSurface];
  OMHandle request={self.handle};OMResult result={};NSData *body=checked(OM_COMMAND_SUBMIT,&request,sizeof(request),&result);
  if(body.length!=sizeof(OMCommandResult))fail(@"Missing actual asynchronous submission");OMCommandResult initial={};memcpy(&initial,body.bytes,sizeof(initial));
  if(initial.status<MTLCommandBufferStatusCommitted || initial.status>MTLCommandBufferStatusCompleted ||
     (initial.status<MTLCommandBufferStatusCompleted && (initial.start_ns || initial.end_ns)))fail(@"Invalid actual submission state");
  self.actualResult=initial;if(initial.status>=MTLCommandBufferStatusScheduled){self.actualScheduledStatus=(MTLCommandBufferStatus)initial.status;self.scheduledObserved=YES;dispatch_group_leave(self.scheduledGroup);}
  NSArray *scheduled=[self.scheduledHandlers copy],*completed=[self.completedHandlers copy],*presentations=[self.presentations copy];
  self.scheduledHandlers=nil;self.completedHandlers=nil;self.presentations=nil;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{@autoreleasepool {
   @try {
    OMCommandResult value=initial;unsigned polls=0;
    while(value.status!=MTLCommandBufferStatusCompleted) {
     if(++polls>4096)fail(@"Actual GPU completion observation exceeded its bound");
     OMResult observed={};NSData *reply=checked(OM_COMMAND_STATUS,&request,sizeof(request),&observed);
     if(reply.length!=sizeof(value))fail(@"Missing actual GPU status");memcpy(&value,reply.bytes,sizeof(value));
     if(value.status<MTLCommandBufferStatusCommitted || value.status>MTLCommandBufferStatusCompleted ||
         (value.status<MTLCommandBufferStatusCompleted && (value.start_ns || value.end_ns)))fail(@"Invalid actual GPU status");
     if(value.status>=MTLCommandBufferStatusScheduled && !self.scheduledObserved){self.actualScheduledStatus=(MTLCommandBufferStatus)value.status;self.scheduledObserved=YES;dispatch_group_leave(self.scheduledGroup);}
     if(value.status!=MTLCommandBufferStatusCompleted){self.actualResult=value;usleep(1000);}
    }
    if(!value.start_ns || value.end_ns<=value.start_ns)fail(@"Missing actual completed GPU timing");
    for(OMTextureObject *texture in self.surfaceTargets)[texture downloadSurface];
    for(OMBufferObject *buffer in self.buffers)[buffer download];
    self.actualResult=value;self.resourcesVisible=YES;
    @try {for(id<MTLDrawable> drawable in presentations)[drawable present];}
    @catch(NSException *exception){self.deliveryFailure=exception;}
   } @catch(NSException *exception){self.asynchronousFailure=exception;}
   if(!self.scheduledObserved){self.scheduledObserved=YES;dispatch_group_leave(self.scheduledGroup);}
   dispatch_group_leave(self.visibilityGroup);
   // Callbacks are deliberately late: real GPU completion and CPU visibility
   // precede success delivery. A transport/GPU failure has a real error and
   // never delivers a fabricated successful command or display timestamp.
   self.callbackThread=pthread_self();
   @try {
    if(self.resourcesVisible)for(MTLCommandBufferHandler handler in scheduled)handler((id<MTLCommandBuffer>)self);
    for(MTLCommandBufferHandler handler in completed)handler((id<MTLCommandBuffer>)self);
   } @catch(NSException *exception){self.deliveryFailure=exception;}
   @finally {
    self.callbackThread=nullptr;[pendingLock lock];[pendingCommands removeObject:self];[pendingLock unlock];dispatch_group_leave(group);
   }
  }});
 } @catch(NSException *exception) {
  self.asynchronousFailure=exception;if(!self.scheduledObserved){self.scheduledObserved=YES;dispatch_group_leave(self.scheduledGroup);}dispatch_group_leave(self.visibilityGroup);[pendingLock lock];[pendingCommands removeObject:self];[pendingLock unlock];dispatch_group_leave(group);@throw;
 }
}
- (void)trackFence:(id<MTLFence>)fence {
 if(!self.fences)self.fences=[NSMutableArray array];if([self.fences indexOfObjectIdenticalTo:fence]==NSNotFound)[self.fences addObject:(OMFenceObject *)fence];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable {
 if(self.submissionStarted || self.actualResult.status || !drawable || ![(id)drawable conformsToProtocol:@protocol(MTLDrawable)] ||
     !methodABI(drawable,@selector(texture),"@16@0:8") || !methodABI(drawable,@selector(present),"v16@0:8"))fail(@"Unqualified drawable or already completed command");
 id<MTLTexture> texture=[(id<OMTextureDrawable>)drawable texture];
 if(![(id)texture isKindOfClass:[OMTextureObject class]] || texture.device!=remoteDevice || !texture.iosurface)
  fail(@"Drawable requires this device's actual IOSurface texture");
 if(!self.presentations)self.presentations=[NSMutableArray array];
 if(self.presentations.count>=8 || [self.presentations indexOfObjectIdenticalTo:drawable]!=NSNotFound)fail(@"Duplicate or excessive actual drawable presentation");
 [self trackSurfaceTarget:(OMTextureObject *)texture];[self.presentations addObject:drawable];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
 if(!handler || self.submissionStarted || self.actualResult.status)fail(@"Completion handler requires a valid uncommitted command");
 if(!self.completedHandlers)self.completedHandlers=[NSMutableArray array];[self.completedHandlers addObject:[handler copy]];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler {
 if(!handler || self.submissionStarted || self.actualResult.status)fail(@"Scheduled handler requires a valid uncommitted command");
 if(!self.scheduledHandlers)self.scheduledHandlers=[NSMutableArray array];[self.scheduledHandlers addObject:[handler copy]];
}
- (void)enqueue {
 if(self.submissionStarted || self.actualResult.status || self.actualEnqueueStatus)fail(@"Command already enqueued or submitted");
 OMHandle request={self.handle};OMResult result={};NSData *body=checked(OM_COMMAND_ENQUEUE,&request,sizeof(request),&result);
 if(body.length!=sizeof(uint64_t))fail(@"Missing actual enqueue status");uint64_t status=0;memcpy(&status,body.bytes,sizeof(status));
 if(status!=MTLCommandBufferStatusEnqueued)fail(@"Actual GPU command was not enqueued");self.actualEnqueueStatus=(MTLCommandBufferStatus)status;
}
- (void)trackBuffer:(OMBufferObject *)buffer {
 if(!self.buffers)self.buffers=[NSMutableArray array];if(![self.buffers containsObject:buffer])[self.buffers addObject:buffer];
}
- (void)trackTexture:(OMTextureObject *)texture {
 if(!self.textures)self.textures=[NSMutableArray array];if(![self.textures containsObject:texture])[self.textures addObject:texture];
}
- (void)trackSurfaceTarget:(OMTextureObject *)texture {
 [self trackTexture:texture];
 if(texture.surface){if(!self.surfaceTargets)self.surfaceTargets=[NSMutableArray array];if(![self.surfaceTargets containsObject:texture])[self.surfaceTargets addObject:texture];}
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor {
 if(!descriptor)fail(@"Missing render pass descriptor");
 OMAttachmentRender extended={};OMRender &request=extended.colors;request.command=self.handle;
 for(unsigned i=0;i<8;i++) {
  MTLRenderPassColorAttachmentDescriptor *a=descriptor.colorAttachments[i];if(!a.texture)continue;
  if(![(id)a.texture isKindOfClass:[OMTextureObject class]] || a.level || a.slice || a.depthPlane || a.resolveTexture)fail(@"Unsupported or foreign render attachment");
  request.colors[i].texture=[(OMObject *)a.texture handle];request.colors[i].load=a.loadAction;request.colors[i].store=a.storeAction;
  OMTextureObject *texture=(OMTextureObject *)a.texture;
  [self trackSurfaceTarget:texture];
  if(texture.surface)[texture uploadSurface];
  MTLClearColor color=a.clearColor;request.colors[i].clear[0]=color.red;request.colors[i].clear[1]=color.green;request.colors[i].clear[2]=color.blue;request.colors[i].clear[3]=color.alpha;
 }
 MTLRenderPassDepthAttachmentDescriptor *depth=descriptor.depthAttachment;
 if(depth.texture) {
  if(![(id)depth.texture isKindOfClass:[OMTextureObject class]] || depth.level || depth.slice || depth.depthPlane || depth.resolveTexture)fail(@"Unsupported or foreign depth attachment");
  extended.depth={[(OMObject *)depth.texture handle],(uint64_t)depth.loadAction,(uint64_t)depth.storeAction,depth.clearDepth};[self trackTexture:(OMTextureObject *)depth.texture];
 }
 MTLRenderPassStencilAttachmentDescriptor *stencil=descriptor.stencilAttachment;
 if(stencil.texture) {
  if(![(id)stencil.texture isKindOfClass:[OMTextureObject class]] || stencil.level || stencil.slice || stencil.depthPlane || stencil.resolveTexture)fail(@"Unsupported or foreign stencil attachment");
  extended.stencil={[(OMObject *)stencil.texture handle],(uint64_t)stencil.loadAction,(uint64_t)stencil.storeAction,stencil.clearStencil};[self trackTexture:(OMTextureObject *)stencil.texture];
 }
 bool attachments=depth.texture || stencil.texture;
 OMResult result={};checked(attachments?OM_RENDER_ATTACHMENTS:OM_RENDER,attachments?(const void *)&extended:(const void *)&request,attachments?sizeof(extended):sizeof(request),&result);
 OMEncoderObject *object=[OMEncoderObject new];object.handle=result.handle;object.command=self;return (id<MTLRenderCommandEncoder>)object;
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
 OMHandle request={self.handle};OMResult result={};checked(OM_BLIT,&request,sizeof(request),&result);
 OMBlitEncoderObject *object=[OMBlitEncoderObject new];object.handle=result.handle;object.command=self;return (id<MTLBlitCommandEncoder>)object;
}
- (void)commit {
 if(self.useAsync){[self commitAsync];return;}
 if(self.actualResult.status)fail(@"Command already submitted");
 for(OMBufferObject *buffer in self.buffers)[buffer upload];
 for(OMTextureObject *texture in self.textures)[texture uploadSurface];
 OMHandle request={self.handle};OMResult result={};NSData *body=checked(OM_COMMIT,&request,sizeof(request),&result);
 if(body.length!=sizeof(OMCommandResult))fail(@"Missing actual command result");OMCommandResult value={};memcpy(&value,body.bytes,sizeof(value));
 if(value.status!=MTLCommandBufferStatusCompleted || !value.start_ns || value.end_ns<=value.start_ns)fail(@"Actual GPU command did not complete");
 for(OMTextureObject *texture in self.surfaceTargets)[texture downloadSurface];
 for(OMBufferObject *buffer in self.buffers)[buffer download];
 self.actualResult=value;self.resourcesVisible=YES;
 // Present the real original drawable only after actual GPU completion and
 // visibility in its original IOSurface. Its real framework owns presentation
 // scheduling and timestamps; GPU completion is never a fabricated display
 // completion. Preserve actual GPU status/callbacks even if presentation fails.
 NSArray<id<MTLDrawable>> *presentations=self.presentations;self.presentations=nil;
 NSException *presentationFailure=nil;
 @try {for(id<MTLDrawable> drawable in presentations)[drawable present];}
 @catch(NSException *exception){presentationFailure=exception;}
 // This transport is synchronous: submission, execution and surface/buffer
 // readback have all actually finished. Deliver each registered block exactly
 // once on a real dispatch worker, with the real completed command. Completion
 // waits also wait for that delivery, without invoking callbacks under the
 // original renderer's encoding/commit call stack.
 // Scheduled callbacks are deliberately late here; no asynchronous timing or
 // fabricated Scheduled/Completed state is advertised.
 NSArray *scheduled=self.scheduledHandlers,*completed=self.completedHandlers;
 self.scheduledHandlers=nil;self.completedHandlers=nil;
 dispatch_group_t group=nil;
 if(scheduled.count || completed.count){group=dispatch_group_create();dispatch_group_enter(group);self.callbackGroup=group;}
 if(group)dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{
  self.callbackThread=pthread_self();
  @try {
   for(MTLCommandBufferHandler handler in scheduled)handler((id<MTLCommandBuffer>)self);
   for(MTLCommandBufferHandler handler in completed)handler((id<MTLCommandBuffer>)self);
  } @finally {self.callbackThread=nullptr;dispatch_group_leave(group);}
 });
 if(presentationFailure)@throw presentationFailure;
}
- (void)waitUntilScheduled {
 if(self.useAsync) {
  if(!self.submissionStarted || !self.scheduledGroup)fail(@"Command has not actually been submitted");
  if(dispatch_group_wait(self.scheduledGroup,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC)))fail(@"Actual scheduling observation timed out");
  if(self.asynchronousFailure)@throw self.asynchronousFailure;
  if(self.actualScheduledStatus<MTLCommandBufferStatusScheduled)fail(@"Actual GPU scheduling has not been observed");return;
 }
 if(self.actualResult.status!=MTLCommandBufferStatusCompleted)fail(@"Command has not actually been submitted and completed");
}
- (BOOL)commitAndWaitUntilSubmitted {
 // Original B16@0:8 ABI. Async success reports actual accepted submission,
 // never completion or callback delivery while the original caller holds locks.
 [self commit];if(self.useAsync)return !self.asynchronousFailure && self.actualResult.status>=MTLCommandBufferStatusCommitted && self.actualResult.status<=MTLCommandBufferStatusCompleted;
 [self waitUntilScheduled];return self.actualResult.status==MTLCommandBufferStatusCompleted;
}
- (void)waitUntilCompleted {
 if(self.useAsync) {
  if(!self.submissionStarted || !self.callbackGroup)fail(@"Command has not actually been submitted");
  bool ownCallback=self.callbackThread && pthread_equal(self.callbackThread,pthread_self());
  if(!ownCallback && dispatch_group_wait(self.callbackGroup,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC)))fail(@"Actual asynchronous completion/visibility/callback wait timed out");
  if(self.asynchronousFailure)@throw self.asynchronousFailure;if(self.deliveryFailure)@throw self.deliveryFailure;
  if(!self.resourcesVisible || self.actualResult.status!=MTLCommandBufferStatusCompleted)fail(@"Actual GPU resources are not completed and visible");return;
 }
 if(self.actualResult.status!=MTLCommandBufferStatusCompleted)fail(@"Command has not actually completed");
 if(self.callbackGroup && !(self.callbackThread && pthread_equal(self.callbackThread,pthread_self())) && dispatch_group_wait(self.callbackGroup,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC)))fail(@"Actual completion-handler delivery timed out");
}
- (MTLCommandBufferStatus)status {
 if(self.asynchronousFailure)return MTLCommandBufferStatusError;
 OMCommandResult value=self.actualResult;
 if(self.useAsync && value.status==MTLCommandBufferStatusCompleted && !self.resourcesVisible){if(dispatch_group_wait(self.visibilityGroup,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC)))fail(@"Actual resource visibility timed out");if(self.asynchronousFailure)return MTLCommandBufferStatusError;value=self.actualResult;}
 return value.status?(MTLCommandBufferStatus)value.status:self.actualEnqueueStatus;
}
- (NSError *)error {
 if(self.asynchronousFailure)return [NSError errorWithDomain:@"org.blu-sharky.offline-metal.async" code:-1 userInfo:@{NSLocalizedDescriptionKey:self.asynchronousFailure.reason?:@"Actual asynchronous transport/GPU failure"}];
 return nil;
}
- (CFTimeInterval)GPUStartTime {return self.actualResult.start_ns/1e9;}
- (CFTimeInterval)GPUEndTime {return self.actualResult.end_ns/1e9;}
@end
@interface OMQueueObject : OMObject
@end
@implementation OMQueueObject
- (NSUInteger)readPriority:(BOOL)background {
 OMQueuePriority request={self.handle,(uint64_t)background,0};OMResult result={};NSData *body=checked(OM_QUEUE_PRIORITY_GET,&request,sizeof(request),&result);
 if(body.length!=sizeof(uint64_t))fail(@"Missing actual queue priority");uint64_t value=0;memcpy(&value,body.bytes,sizeof(value));return value;
}
- (BOOL)writePriority:(NSUInteger)priority background:(BOOL)background {
 // Exact original MTLIOAccelCommandQueue validation: 0..5, except 3 in foreground.
 if(priority>5 || (!background && priority==3))return NO;
 OMQueuePriority request={self.handle,(uint64_t)background,priority};OMResult result={};NSData *body=call(OM_QUEUE_PRIORITY_SET,&request,sizeof(request),&result);
 if(result.error)return NO;if(body.length!=sizeof(uint64_t))fail(@"Missing actual queue priority-change result");uint64_t value=0;memcpy(&value,body.bytes,sizeof(value));
 if(value>1)fail(@"Invalid actual priority-change result");return value!=0;
}
- (NSUInteger)getGPUPriority {return [self readPriority:NO];}
- (NSUInteger)getBackgroundGPUPriority {return [self readPriority:YES];}
- (BOOL)setGPUPriority:(NSUInteger)priority {return [self writePriority:priority background:NO];}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {return [self writePriority:priority background:YES];}
- (id<MTLCommandBuffer>)commandBuffer {
 OMHandle request={self.handle};OMResult result={};checked(OM_COMMAND,&request,sizeof(request),&result);
 OMCommandObject *object=[OMCommandObject new];object.handle=result.handle;object.commandQueue=(id<MTLCommandQueue>)self;object.useAsync=asyncEnabled;return (id<MTLCommandBuffer>)object;
}
@end
// This model contributes only own implementations to a dynamically allocated
// real _MTLDevice subclass. No original method implementation is overwritten.
@interface OMDeviceMethods : NSObject
@end
@implementation OMDeviceMethods
- (NSString *)name {return deviceInfo[@"name"];}
- (uint64_t)registryID {return [deviceInfo[@"registryID"] unsignedLongLongValue];}
- (NSUInteger)maxBufferLength {return [deviceInfo[@"maxBufferLength"] unsignedIntegerValue];}
- (BOOL)isLowPower {return [deviceInfo[@"lowPower"] boolValue];}
- (BOOL)isRemovable {return [deviceInfo[@"removable"] boolValue];}
- (BOOL)isHeadless {return [deviceInfo[@"headless"] boolValue];}
// This partial offline device does not implement any complete GPU family or
// feature set. The original base implementation needs Apple capability tables
// which this independent device neither owns nor populates.
- (BOOL)supportsFamily:(MTLGPUFamily)family {(void)family;return NO;}
- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet {(void)featureSet;return NO;}
- (id<MTLEvent>)newEvent {
 OMResult result={};call(OM_EVENT,nullptr,0,&result);if(result.error)return nil;
 if(!result.handle)fail(@"Missing actual GPU event identity");OMEventObject *object=[OMEventObject new];object.handle=result.handle;return (id<MTLEvent>)object;
}
- (id<MTLSharedEvent>)newSharedEvent {
 OMResult result={};NSData *body=call(OM_SHARED_EVENT,nullptr,0,&result);if(result.error)return nil;
 if(!result.handle)fail(@"Missing actual shared-event identity");NSDictionary *info=sharedMetadata(body);
 OMSharedEventObject *object=[OMSharedEventObject new];object.handle=result.handle;object.deviceIsNil=[info[@"device_is_nil"] boolValue];
 // Initialize without writing metadata back to the actual event.
 ((void (*)(id,SEL,id))class_getMethodImplementation([OMObject class],@selector(setLabel:)))(object,@selector(setLabel:),info[@"label"]==[NSNull null]?nil:info[@"label"]);
 return object;
}
- (id<MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle *)handle {
 Class cls=NSClassFromString(@"MTLSharedEventHandle");if(!handle || object_getClass(handle)!=cls || class_getInstanceSize(cls)!=16)return nil;
 mach_port_t port=handle.eventPort;if(!MACH_PORT_VALID(port))return nil;
 [transaction lock];@try {
  if(!live || closing)return nil;uint64_t input=port,output=0;unsigned count=1;unsigned char bytes[4096];size_t size=sizeof(bytes);
  int status=ioMethod(connection,3,&input,1,nullptr,0,&output,&count,bytes,&size);if(status)return nil;
  if(count!=1 || !output)fail(@"Missing actual imported shared event");NSDictionary *info=sharedMetadata([NSData dataWithBytes:bytes length:size]);
  OMSharedEventObject *object=[OMSharedEventObject new];object.handle=output;object.deviceIsNil=[info[@"device_is_nil"] boolValue];
  ((void (*)(id,SEL,id))class_getMethodImplementation([OMObject class],@selector(setLabel:)))(object,@selector(setLabel:),info[@"label"]==[NSNull null]?nil:info[@"label"]);
  return object;
 } @finally {[transaction unlock];}
}
- (id<MTLFence>)newFence {
 OMResult result={};call(OM_FENCE,nullptr,0,&result);if(result.error)return nil;
 if(!result.handle)fail(@"Missing actual GPU fence identity");OMFenceObject *object=[OMFenceObject new];object.handle=result.handle;return (id<MTLFence>)object;
}
- (id<MTLCommandQueue>)newCommandQueue {
 OMResult result={};checked(OM_QUEUE,nullptr,0,&result);OMQueueObject *object=[OMQueueObject new];object.handle=result.handle;return (id<MTLCommandQueue>)object;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!length || length>MIN((NSUInteger)OFFLINE_METAL_MAX_BUFFER,self.maxBufferLength) || options)return nil;
 OMBuffer request={length,(uint64_t)options};OMResult result={};call(OM_BUFFER,&request,sizeof(request),&result);if(result.error)return nil;
 OMBufferObject *object=[OMBufferObject new];object.handle=result.handle;object.data=[NSMutableData dataWithLength:length];return (id<MTLBuffer>)object;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!bytes)return nil;OMBufferObject *object=(OMBufferObject *)[self newBufferWithLength:length options:options];if(!object)return nil;
 memcpy(object.data.mutableBytes,bytes,length);return (id<MTLBuffer>)object;
}
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error {
 // This optional original directory contains Apple-family pipeline entries.
 // No backend qualification exists for that private archive format. Refuse
 // explicitly so the unchanged caller can take its real AIR-library path,
 // rather than invoking the Apple base implementation with absent internals.
 (void)path;
 if(error){*error=[NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:-4
     userInfo:@{NSLocalizedDescriptionKey:@"Apple-family pipeline libraries are not qualified for this independent device; use portable AIR input"}];}
 return nil;
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
 NSError *failure=nil;NSData *bytes=path?[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:&failure]:nil;
 if(!bytes){if(error)*error=failure?:[NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:-2 userInfo:@{NSLocalizedDescriptionKey:@"A readable actual library file is required"}];return nil;}
 dispatch_data_t data=dispatch_data_create(bytes.bytes,bytes.length,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)bytes.length;});
 return [(id<MTLDevice>)self newLibraryWithData:data error:error];
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=[NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:-2 userInfo:@{NSLocalizedDescriptionKey:@"An actual local library file URL is required"}];return nil;}
 return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
 NSURL *url=[bundle URLForResource:@"default" withExtension:@"metallib"];
 if(!url){if(error)*error=[NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:-2 userInfo:@{NSLocalizedDescriptionKey:@"The actual bundle has no default Metal library"}];return nil;}
 return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 const void *bytes=nullptr;size_t size=0;dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);(void)mapped;
 if(!bytes || !size || size>64*1024*1024){if(error)*error=[NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:-2 userInfo:nil];return nil;}
 OMLibrary request={};CC_SHA256(bytes,(CC_LONG)size,request.sha256);OMResult result={};NSData *body=call(OM_LIBRARY,&request,sizeof(request),&result);
 if(result.error){if(error)*error=operationError(result,body);return nil;}
 NSArray *names=[NSJSONSerialization JSONObjectWithData:body options:0 error:nil];if(![names isKindOfClass:[NSArray class]])fail(@"Missing actual library metadata");
 OMLibraryObject *object=[OMLibraryObject new];object.handle=result.handle;object.functionNames=names;if(error)*error=nil;return (id<MTLLibrary>)object;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
 if(!d || d.hazardTrackingMode>MTLHazardTrackingModeTracked || !d.allowGPUOptimizedContents)return nil;
 OMTexture request={(uint64_t)d.textureType,(uint64_t)d.pixelFormat,d.width,d.height,d.depth,d.mipmapLevelCount,d.sampleCount,d.arrayLength,(uint64_t)d.usage,(uint64_t)d.storageMode,(uint64_t)d.cpuCacheMode};
 OMHazardTexture extended={request,(uint64_t)d.hazardTrackingMode};OMResult result={};NSData *body=call(OM_TEXTURE_HAZARD,&extended,sizeof(extended),&result);if(result.error)return nil;
 if(body.length!=sizeof(uint64_t))fail(@"Missing actual texture hazard mode");uint64_t actual=0;memcpy(&actual,body.bytes,sizeof(actual));
 if(actual>MTLHazardTrackingModeTracked || (extended.hazard && actual!=extended.hazard))fail(@"Actual texture hazard mode differs");
 OMTextureObject *object=[OMTextureObject new];object.handle=result.handle;object.descriptor=request;object.actualHazardMode=(MTLHazardTrackingMode)actual;return (id<MTLTexture>)object;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
 if(!d)return nil;
 auto face=[](MTLStencilDescriptor *s)->OMStencilFace {
  if(!s)return {};return {1,(uint64_t)s.stencilCompareFunction,(uint64_t)s.stencilFailureOperation,(uint64_t)s.depthFailureOperation,
      (uint64_t)s.depthStencilPassOperation,s.readMask,s.writeMask};
 };
 OMDepthStencil request={(uint64_t)d.depthCompareFunction,(uint64_t)d.depthWriteEnabled,face(d.frontFaceStencil),face(d.backFaceStencil)};
 OMResult result={};call(OM_DEPTH_STENCIL_STATE,&request,sizeof(request),&result);if(result.error)return nil;
 OMDepthStateObject *object=[OMDepthStateObject new];object.handle=result.handle;return (id<MTLDepthStencilState>)object;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
 if(!surface || CFGetTypeID(surface)!=IOSurfaceGetTypeID() || !d || plane || IOSurfaceGetPlaneCount(surface) || d.storageMode!=MTLStorageModeShared)return nil;
 bool format=((d.pixelFormat==MTLPixelFormatR32Float && IOSurfaceGetPixelFormat(surface)==kCVPixelFormatType_OneComponent32Float)
                   || (d.pixelFormat==MTLPixelFormatRGBA8Unorm && IOSurfaceGetPixelFormat(surface)==kCVPixelFormatType_32RGBA)
                   || (d.pixelFormat==MTLPixelFormatBGRA8Unorm && IOSurfaceGetPixelFormat(surface)==kCVPixelFormatType_32BGRA));
 if(!format
     || !d.width || !d.height || d.width!=IOSurfaceGetWidth(surface) || d.height!=IOSurfaceGetHeight(surface)
     || d.width>SIZE_MAX/4 || IOSurfaceGetBytesPerElement(surface)!=4 || IOSurfaceGetBytesPerRow(surface)<d.width*4
     || IOSurfaceGetBytesPerRow(surface)>SIZE_MAX/d.height || IOSurfaceGetAllocSize(surface)<IOSurfaceGetBytesPerRow(surface)*d.height)return nil;
 OMTextureObject *object=(OMTextureObject *)[self newTextureWithDescriptor:d];if(!object)return nil;
 object.surface=(IOSurfaceRef)CFRetain(surface);[object uploadSurface];return (id<MTLTexture>)object;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 static_assert(MTLVertexFormatFloat4==31 && MTLVertexFormatHalf4==27 && MTLVertexStepFunctionPerInstance==2,"Actual target SDK vertex enum ABI");
 bool vertexInputs=false;OMVertexPipeline vertexPipeline={};
 // Read real original descriptors. Inactive attributes/layouts carry zeroes;
 // static stepping is transported without modifying any original descriptor.
 if(d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
  MTLVertexAttributeDescriptor *a=d.vertexDescriptor.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
  vertexInputs=true;vertexPipeline.attributes[i]={(uint64_t)a.format,a.offset,a.bufferIndex};
  if(a.bufferIndex<31) {
   MTLVertexBufferLayoutDescriptor *l=d.vertexDescriptor.layouts[a.bufferIndex];
   vertexPipeline.layouts[a.bufferIndex]={l.stride,(uint64_t)l.stepFunction,l.stepRate};
  }
 }
 if(![(id)d.vertexFunction isKindOfClass:[OMFunctionObject class]] || ![(id)d.fragmentFunction isKindOfClass:[OMFunctionObject class]]
     || (vertexInputs && !OMVertexInputsValid(vertexPipeline)) || !d.rasterizationEnabled || d.alphaToCoverageEnabled || d.alphaToOneEnabled) {
  if(error)*error=[NSError errorWithDomain:@"org.blu-sharky.offline-metal" code:-4 userInfo:nil];return nil;
 }
 OMAttachmentPipeline attachments={};OMBlendPipeline &extended=attachments.colors;OMPipeline &request=extended.pipeline;bool blending=false;
 attachments.depthFormat=d.depthAttachmentPixelFormat;attachments.stencilFormat=d.stencilAttachmentPixelFormat;
 request.vertex=[(OMObject *)d.vertexFunction handle];request.fragment=[(OMObject *)d.fragmentFunction handle];request.samples=d.rasterSampleCount;
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];request.formats[i]=a.pixelFormat;request.masks[i]=a.writeMask;
  blending|=a.blendingEnabled;extended.colors[i]={(uint64_t)a.blendingEnabled,(uint64_t)a.sourceRGBBlendFactor,(uint64_t)a.destinationRGBBlendFactor,
      (uint64_t)a.rgbBlendOperation,(uint64_t)a.sourceAlphaBlendFactor,(uint64_t)a.destinationAlphaBlendFactor,(uint64_t)a.alphaBlendOperation};
 }
 bool attached=attachments.depthFormat || attachments.stencilFormat;
 vertexPipeline.pipeline=attachments;
 OMResult result={};NSData *body=call((vertexInputs || attached)?OM_PIPELINE_ATTACHMENTS:blending?OM_PIPELINE_BLEND:OM_PIPELINE,
     vertexInputs?(const void *)&vertexPipeline:attached?(const void *)&attachments:blending?(const void *)&extended:(const void *)&request,
     vertexInputs?sizeof(vertexPipeline):attached?sizeof(attachments):blending?sizeof(extended):sizeof(request),&result);if(result.error){if(error)*error=operationError(result,body);return nil;}
 OMPipelineObject *object=[OMPipelineObject new];object.handle=result.handle;return (id<MTLRenderPipelineState>)object;
}
@end
extern "C" id<MTLDevice> offline_metal_frontend_open(void);
extern "C" id<MTLDevice> offline_metal_frontend_open_service(const char *);
extern "C" void offline_metal_frontend_finish(void);
id<MTLDevice> offline_metal_frontend_open(void) {
 return offline_metal_frontend_open_service("NativeVirtioConsole");
}
id<MTLDevice> offline_metal_frontend_open_service(const char *serviceName) {
 if(!serviceName || (strcmp(serviceName,"NativeVirtioConsole") && strcmp(serviceName,"NativeOfflineGPU")))return nil;
 if(live)return nil;void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);if(!iokit)return nil;
 auto matching=(void *(*)(const char *))dlsym(iokit,"IOServiceMatching");auto services=(int (*)(unsigned,void *,unsigned *))dlsym(iokit,"IOServiceGetMatchingServices");
 auto next=(unsigned (*)(unsigned))dlsym(iokit,"IOIteratorNext");auto release=(int (*)(unsigned))dlsym(iokit,"IOObjectRelease");auto open=(int (*)(unsigned,unsigned,unsigned,unsigned *))dlsym(iokit,"IOServiceOpen");
 ioMethod=(IOMethod)dlsym(iokit,"IOConnectCallMethod");if(!matching || !services || !next || !release || !open || !ioMethod)return nil;
 unsigned iterator=0;if(services(0,matching(serviceName),&iterator) || !iterator)return nil;unsigned service=next(iterator);
 unsigned extra=next(iterator);int openStatus=-1;
 if(service && !extra)openStatus=open(service,mach_task_self(),0,&connection);
 if(extra)release(extra);if(service)release(service);release(iterator);if(openStatus)return nil;
 transaction=[NSRecursiveLock new];pendingLock=[NSLock new];pendingCommands=[NSMutableSet set];pendingNotifications=[NSMutableSet set];notificationFailure=nil;asyncEnabled=false;closing=false;live=true;OMResult result={};NSData *body=checked(OM_DEVICE,nullptr,0,&result);
 deviceInfo=[NSJSONSerialization JSONObjectWithData:body options:0 error:nil];if(![deviceInfo isKindOfClass:[NSDictionary class]])fail(@"Actual device metadata missing");
 Class base=objc_getClass("_MTLDevice");if(!base || class_getInstanceSize(base)!=536)fail(@"Wrong original Metal base ABI");
 Class cls=objc_allocateClassPair(base,"NativeOfflineMetalDevice",0);if(!cls)fail(@"Own device class allocation failed");
 unsigned count=0;Method *methods=class_copyMethodList([OMDeviceMethods class],&count);
 for(unsigned i=0;i<count;i++)if(!class_addMethod(cls,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i])))fail(@"Own method installation failed");
 free(methods);objc_registerClassPair(cls);remoteDevice=[(id)cls new];return remoteDevice;
}
extern "C" BOOL offline_metal_frontend_enable_async(void) {
 if(!live)return NO;[pendingLock lock];BOOL allowed=!closing && (asyncEnabled || !pendingCommands.count);if(allowed)asyncEnabled=true;[pendingLock unlock];return allowed;
}
// Explicit diagnostic control: even a guessed real host export identifier may
// not be submitted through the public raw endpoint. Do not consume a sequence
// on a kernel framing refusal; the subsequent real operation proves usability.
extern "C" int offline_metal_frontend_raw_shared_import_control(uint64_t identifier) {
 [transaction lock];@try {
  if(!live || closing)return -1;
  struct {OMWireHeader header;OMHandle input;} request={{OM_WIRE_MAGIC,OFFLINE_METAL_VERSION,OM_SHARED_IMPORT,sizeof(OMHandle),sequence+1},{identifier}};
  int status=ioMethod(connection,0,nullptr,0,&request,sizeof(request),nullptr,nullptr,nullptr,nullptr);
  if(!status)fail(@"Kernel unexpectedly accepted an unmediated shared identifier");return status;
 } @finally {[transaction unlock];}
}
extern "C" int offline_metal_frontend_stale_shared_port_control(mach_port_t port) {
 [transaction lock];@try {
  if(!live || closing)return -1;uint64_t input=port,output=0;unsigned count=1;unsigned char bytes[4096];size_t size=sizeof(bytes);
  int status=ioMethod(connection,3,&input,1,nullptr,0,&output,&count,bytes,&size);
  if(!status)fail(@"Kernel unexpectedly accepted a stale shared-event port");return status;
 } @finally {[transaction unlock];}
}
void offline_metal_frontend_finish(void) {
 [pendingLock lock];
 for(OMCommandObject *command in pendingCommands)if(command.callbackThread && pthread_equal(command.callbackThread,pthread_self())){[pendingLock unlock];fail(@"Finish from an active command callback is unsupported");}
 for(OMSharedNotificationState *state in pendingNotifications)if(state.callbackThread && pthread_equal(state.callbackThread,pthread_self())){[pendingLock unlock];fail(@"Finish from an active event callback is unsupported");}
 closing=true;NSArray *commands=pendingCommands.allObjects,*notifications=pendingNotifications.allObjects;
 for(OMSharedNotificationState *state in notifications)if(state.phase!=OMNotificationRunning){state.phase=OMNotificationCancelled;notificationsCancelled++;[pendingNotifications removeObject:state];}
 [pendingLock unlock];
 NSException *failure=nil;for(OMCommandObject *command in commands)@try {[command waitUntilCompleted];}@catch(NSException *exception){if(!failure)failure=exception;}
 for(OMSharedNotificationState *state in notifications)dispatch_group_wait(state.callbackGroup,DISPATCH_TIME_FOREVER);
 [pendingLock lock];if(!failure)failure=notificationFailure;[pendingLock unlock];
 [transaction lock];
 @try {OMResult result={};checked(OM_FINISH,nullptr,0,&result);live=false;} @finally {[transaction unlock];}
 auto close=(int (*)(unsigned))dlsym(RTLD_DEFAULT,"IOServiceClose");if(!close || close(connection))fail(@"Actual userclient close failed");
 if(failure)@throw failure;
}
extern "C" void offline_metal_frontend_notification_counts(uint64_t *values) {
 if(!values)return;[pendingLock lock];values[0]=notificationsRegistered;values[1]=notificationsDelivered;values[2]=notificationsCancelled;values[3]=notificationsFailed;
 values[4]=pendingNotifications.count;values[5]=values[6]=values[7]=0;
 for(OMSharedNotificationState *state in pendingNotifications){if(state.phase==OMNotificationQueued)values[5]++;if(state.phase==OMNotificationRunning)values[6]++;if(state.phase==OMNotificationPending)values[7]++;}
 [pendingLock unlock];
}
