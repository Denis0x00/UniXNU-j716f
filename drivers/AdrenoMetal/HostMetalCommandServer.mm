// SPDX-License-Identifier: MIT
// Actual host Metal objects and commands. Guest requests select operations;
// no pre-scripted draw, completion or texture content is returned.
#import "HostMetalCommandServer.h"
#import <CommonCrypto/CommonDigest.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include "OfflineMetalWire.h"
#include "OfflineMetalFunctionWire.h"
#include "OfflineMetalQueueWire.h"
#include "OfflineMetalTextureWire.h"
#include "OfflineMetalDepthStencilWire.h"
#include "OfflineMetalVertexWire.h"
#include "OfflineMetalFenceWire.h"
#include "OfflineMetalCommandWire.h"
#include "OfflineMetalEventWire.h"
#include "OfflineMetalSharedEventWire.h"
#include "OfflineMetalSessionWire.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>
static bool exact(int fd,void *bytes,size_t size,bool sending) {
 unsigned char *p=(unsigned char *)bytes;
 while(size){ssize_t n=sending?write(fd,p,size):read(fd,p,size);if(n<0 && errno==EINTR)continue;if(n<=0)return false;p+=n;size-=n;}return true;
}
static NSString *hashBytes(const void *bytes,size_t size) {
 if(size>UINT32_MAX)return nil;unsigned char hash[32];CC_SHA256(bytes,(CC_LONG)size,hash);
 NSMutableString *s=[NSMutableString string];for(unsigned i=0;i<32;i++)[s appendFormat:@"%02x",hash[i]];return s;
}
static NSData *jsonBytes(id value) {return [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];}
NSDictionary *ServeOfflineMetalCommands(const char *path,id<MTLDevice> device,NSArray<id<MTLLibrary>> *libraries,NSArray *libraryRows,unsigned expectedSessions) {
 if(expectedSessions>128)return nil;
 if(strlen(path)>=sizeof(((sockaddr_un *)nullptr)->sun_path))return nil;
 NSMutableDictionary<NSNumber *,NSDictionary *> *fenceProducers=[NSMutableDictionary dictionary],*eventProducers=[NSMutableDictionary dictionary];
 NSMutableArray *eventsCreated=[NSMutableArray array],*eventOperations=[NSMutableArray array],*sharedValues=[NSMutableArray array],*sharedLabels=[NSMutableArray array],*notificationJournal=[NSMutableArray array],*exportJournal=[NSMutableArray array];
 NSMutableDictionary<NSNumber *,NSDictionary *> *sharedExports=[NSMutableDictionary dictionary];
 NSMutableDictionary<NSNumber *,NSMutableDictionary *> *sharedStates=[NSMutableDictionary dictionary];
 NSMutableDictionary<NSNumber *,NSMutableDictionary *> *notifications=[NSMutableDictionary dictionary];
 NSLock *notificationLock=[NSLock new];MTLSharedEventListener *sharedListener=[[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_get_global_queue(QOS_CLASS_DEFAULT,0)];
 uint64_t nextExport=1,nextNotification=1;uint32_t controlRequest=1;
 NSMutableArray *fencesCreated=[NSMutableArray array],*fenceOperations=[NSMutableArray array],*textureHazards=[NSMutableArray array];
 int listener=socket(AF_UNIX,SOCK_STREAM,0);if(listener<0)return nil;
 sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,path);
 if(bind(listener,(sockaddr *)&address,sizeof(address)) || listen(listener,1)){close(listener);return nil;}
 fprintf(stderr,"OFFLINE_METAL_READY\n");fflush(stderr);
 int fd=accept(listener,nullptr,nullptr);close(listener);if(fd<0)return nil;
 timeval timeout={90,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
 NSMutableDictionary<NSNumber *,id> *objects=[NSMutableDictionary dictionary];uint64_t next=1;
 NSMutableDictionary<NSNumber *,NSMutableDictionary *> *sessions=[NSMutableDictionary dictionary];
 NSMutableDictionary<NSNumber *,NSNumber *> *sessionRequests=[NSMutableDictionary dictionary];
 uint64_t nextSession=1;unsigned openedSessions=0,closedSessions=0;
 NSMutableArray *completed=[NSMutableArray array],*readbacks=[NSMutableArray array],*operations=[NSMutableArray array],*uploads=[NSMutableArray array];
 NSMutableArray *bufferWrites=[NSMutableArray array],*bufferReads=[NSMutableArray array],*bufferCopies=[NSMutableArray array];
 NSMutableArray *queuePriorities=[NSMutableArray array];
 NSMutableArray *enqueued=[NSMutableArray array];
 NSMutableDictionary<NSNumber *,NSDictionary *> *pendingEnqueues=[NSMutableDictionary dictionary];
 NSMutableArray *textureCopies=[NSMutableArray array];
 NSMutableArray *depthStates=[NSMutableArray array],*stencilReferences=[NSMutableArray array],*attachmentPasses=[NSMutableArray array];
 NSMutableDictionary<NSNumber *,NSDictionary *> *openEncoders=[NSMutableDictionary dictionary];NSMutableArray *abortedEncoders=[NSMutableArray array];
 NSMutableArray *specializations=[NSMutableArray array];
 NSMutableDictionary<NSNumber *,NSDictionary *> *asyncCommands=[NSMutableDictionary dictionary];
 NSMutableArray *submitted=[NSMutableArray array],*commandObservations=[NSMutableArray array],*failedCommands=[NSMutableArray array];
 NSMutableSet<NSNumber *> *recordedCompletions=[NSMutableSet set],*recordedFailures=[NSMutableSet set];
 auto observeCommand=[&](uint64_t handle,id<MTLCommandBuffer> command,NSString *reason)->OMCommandResult {
  OMCommandResult value={(uint64_t)command.status,0,0};
  if(value.status==MTLCommandBufferStatusCompleted) {
   value.start_ns=(uint64_t)(command.GPUStartTime*1e9);value.end_ns=(uint64_t)(command.GPUEndTime*1e9);
   if(value.start_ns && value.end_ns>value.start_ns && ![recordedCompletions containsObject:@(handle)]) {
    [recordedCompletions addObject:@(handle)];[completed addObject:@{@"command":@(handle),@"status":@(value.status),@"gpu_start_ns":@(value.start_ns),@"gpu_end_ns":@(value.end_ns),@"observed_by":reason}];
   }
  } else if(value.status==MTLCommandBufferStatusError && ![recordedFailures containsObject:@(handle)]) {
   [recordedFailures addObject:@(handle)];[failedCommands addObject:@{@"command":@(handle),@"status":@(value.status),@"actual_error":command.error.localizedDescription?:@""}];
  }
  return value;
 };
 auto finishSubmitted=[&](uint64_t selected,bool all,NSString *reason) {
  for(NSNumber *handle in asyncCommands.allKeys) {
   NSDictionary *row=asyncCommands[handle];if(!all && [row[@"session"] unsignedLongLongValue]!=selected)continue;
   id<MTLCommandBuffer> command=row[@"command"];[command waitUntilCompleted];
   observeCommand(handle.unsignedLongLongValue,command,reason);[asyncCommands removeObjectForKey:handle];
  }
 };
 auto abortEncoders=[&](uint64_t selected,bool all,NSString *reason) {
  for(NSNumber *handle in openEncoders.allKeys) {
   NSDictionary *row=openEncoders[handle];if(!all && [row[@"session"] unsignedLongLongValue]!=selected)continue;
   id<MTLCommandEncoder> encoder=row[@"encoder"];id<MTLCommandBuffer> command=row[@"command_object"];
   uint64_t status=command.status;[encoder endEncoding];
   [abortedEncoders addObject:@{@"encoder":handle,@"command":row[@"command_handle"],@"session":row[@"session"],
       @"actual_command_status":@(status),@"reason":reason,@"encoding_ended":@YES,@"gpu_submission_created_by_cleanup":@NO}];
   [openEncoders removeObjectForKey:handle];
  }
 };
 auto hasOpenEncoder=[&](uint64_t command)->bool {
  for(NSDictionary *row in openEncoders.allValues)if([row[@"command_handle"] unsignedLongLongValue]==command)return true;return false;
 };
 auto cancelNotifications=[&](uint64_t selected,bool all) {
  [notificationLock lock];
  for(NSNumber *key in notifications.allKeys) {
   NSMutableDictionary *record=notifications[key];if(!all && [record[@"session"] unsignedLongLongValue]!=selected)continue;
   record[@"cancelled"]=@YES;[notificationJournal addObject:@{@"notification":key,@"session":record[@"session"],@"event":record[@"event"],@"requested":record[@"requested"],@"actually_fired":record[@"ready"],@"actual_value":record[@"value"]}];[notifications removeObjectForKey:key];
  }
  [notificationLock unlock];
 };
 NSMutableDictionary *renderExtents=[NSMutableDictionary dictionary];NSMutableArray *blendPipelines=[NSMutableArray array],*textureBindings=[NSMutableArray array],*vertexPipelines=[NSMutableArray array];
 uint32_t bootBytes=0;bool finished=false,healthy=true;
 auto save=[&](id object)->uint64_t {if(!object)return 0;uint64_t key=next++;objects[@(key)]=object;return key;};
 for(uint32_t request=1;request<=8192 && healthy && !finished;request++) {
  @autoreleasepool {
   OMWireHeader header={};
   if(request==1) {
    uint32_t prefix=0,count=0;
    do {unsigned char byte=0;if(!exact(fd,&byte,1,false)){healthy=false;break;}
        prefix=(prefix>>8)|((uint32_t)byte<<24);count++;if(count>65536){healthy=false;break;}
    } while(count<4 || prefix!=OM_WIRE_MAGIC);
    if(!healthy)break;bootBytes=count-4;header.magic=prefix;
    if(!exact(fd,(unsigned char *)&header+4,sizeof(header)-4,false)){healthy=false;break;}
   } else if(!exact(fd,&header,sizeof(header),false)){healthy=false;break;}
   if(header.magic!=OM_WIRE_MAGIC || header.version!=(expectedSessions?OFFLINE_METAL_SESSION_VERSION:OFFLINE_METAL_VERSION)
       || (!expectedSessions && header.request!=request) || header.bytes>4080){healthy=false;break;}
   uint64_t session=0;bool validSession=true;
   if(expectedSessions) {
    if(!exact(fd,&session,sizeof(session),false)){healthy=false;break;}
    if(header.operation==OM_SESSION_OPEN)validSession=!session && header.request==1 && !header.bytes;
    else if(!session && header.operation==OM_SHARED_EXPORT_DROP){validSession=header.request==controlRequest;if(validSession)controlRequest++;}
    else {
     objects=sessions[@(session)];NSNumber *expected=sessionRequests[@(session)];
     validSession=objects && expected && header.request==expected.unsignedIntValue;
     if(validSession)sessionRequests[@(session)]=@(header.request+1);
    }
   }
   std::vector<unsigned char> input(header.bytes);
   if(!input.empty() && !exact(fd,input.data(),input.size(),false)){healthy=false;break;}
   OMResult result={};NSData *body=nil;NSError *error=nil;
   auto get=[&](uint64_t handle,Protocol *protocol)->id {id object=objects[@(handle)];return object && [object conformsToProtocol:protocol]?object:nil;};
   auto getEncoder=[&](uint64_t handle,Protocol *protocol)->id {return openEncoders[@(handle)]?get(handle,protocol):nil;};
#define REQUEST(Type) if(input.size()!=sizeof(Type)){result.error=-2;break;} Type q={};memcpy(&q,input.data(),sizeof(q))
   if(!validSession)result.error=-2;
   else @try {
    switch(header.operation) {
     case OM_SESSION_OPEN:
      if(!expectedSessions || openedSessions>=expectedSessions){result.error=-2;break;}
      result.handle=nextSession++;sessions[@(result.handle)]=[NSMutableDictionary dictionary];
      sessionRequests[@(result.handle)]=@1;openedSessions++;break;
     case OM_SESSION_CLOSE:
      if(!expectedSessions || !input.empty()){result.error=-2;break;}
      // Remove only this real namespace's independent references. Command
      // buffers retain their actual GPU dependencies according to Metal.
      abortEncoders(session,false,@"session-close");
      finishSubmitted(session,false,@"session-close");
      cancelNotifications(session,false);
      for(NSNumber *handle in objects.allKeys){[renderExtents removeObjectForKey:handle];[fenceProducers removeObjectForKey:handle];[eventProducers removeObjectForKey:handle];NSMutableDictionary *shared=sharedStates[handle];if([shared[@"producer"][@"session"] unsignedLongLongValue]==session)[shared removeObjectForKey:@"producer"];[sharedStates removeObjectForKey:handle];}
      for(NSNumber *handle in pendingEnqueues.allKeys)if([pendingEnqueues[handle][@"session"] unsignedLongLongValue]==session)[pendingEnqueues removeObjectForKey:handle];
      [objects removeAllObjects];[sessions removeObjectForKey:@(session)];[sessionRequests removeObjectForKey:@(session)];
      closedSessions++;finished=openedSessions==expectedSessions && !sessions.count && !sharedExports.count;break;
     case OM_DEVICE:
      if(!input.empty()){result.error=-2;break;}
      body=jsonBytes(@{@"name":device.name,@"registryID":@(device.registryID),@"maxBufferLength":@(MIN(device.maxBufferLength,(NSUInteger)OFFLINE_METAL_MAX_BUFFER)),
          @"lowPower":@(device.lowPower),@"removable":@(device.removable),@"headless":@(device.headless),
          @"maxThreads":@[@(device.maxThreadsPerThreadgroup.width),@(device.maxThreadsPerThreadgroup.height),@(device.maxThreadsPerThreadgroup.depth)]});break;
     case OM_QUEUE:
      if(!input.empty()){result.error=-2;break;}result.handle=save([device newCommandQueue]);if(!result.handle)result.error=-3;break;
     case OM_QUEUE_PRIORITY_GET:case OM_QUEUE_PRIORITY_SET: {
      REQUEST(OMQueuePriority);id<MTLCommandQueue> queue=get(q.queue,@protocol(MTLCommandQueue));
      if(!queue || q.background>1 || (header.operation==OM_QUEUE_PRIORITY_GET && q.priority)){result.error=-2;break;}
      bool writing=header.operation==OM_QUEUE_PRIORITY_SET;
      if(writing && (q.priority>5 || (!q.background && q.priority==3))){result.error=-4;break;}
      SEL getter=sel_registerName(q.background?"getBackgroundGPUPriority":"getGPUPriority");
      SEL setter=sel_registerName(q.background?"setBackgroundGPUPriority:":"setGPUPriority:");
      Method readMethod=class_getInstanceMethod(object_getClass(queue),getter),writeMethod=class_getInstanceMethod(object_getClass(queue),setter);
      if(!readMethod || strcmp(method_getTypeEncoding(readMethod),"Q16@0:8") ||
          (writing && (!writeMethod || strcmp(method_getTypeEncoding(writeMethod),"B24@0:8Q16")))){result.error=-4;break;}
      uint64_t before=((NSUInteger(*)(id,SEL))objc_msgSend)(queue,getter),value=before;
      if(writing)value=((BOOL(*)(id,SEL,NSUInteger))objc_msgSend)(queue,setter,q.priority)?1:0;
      uint64_t after=((NSUInteger(*)(id,SEL))objc_msgSend)(queue,getter);
      body=[NSData dataWithBytes:&value length:sizeof(value)];
      [queuePriorities addObject:@{@"queue":@(q.queue),@"session":@(session),@"background":@(q.background),@"writing":@(writing),
          @"requested":@(q.priority),@"before":@(before),@"after":@(after),@"result":@(value)}];break;
     }
     case OM_LIBRARY: {
      REQUEST(OMLibrary);NSMutableString *hash=[NSMutableString string];for(unsigned i=0;i<32;i++)[hash appendFormat:@"%02x",q.sha256[i]];
      NSUInteger i=0;for(;i<libraryRows.count;i++)if([libraryRows[i][@"input_sha256"] isEqualToString:hash])break;
      if(i==libraryRows.count){result.error=-4;break;}
      id<MTLLibrary> library=libraries[i];result.handle=save(library);body=jsonBytes(library.functionNames);break;
     }
     case OM_FUNCTION: {
      OMFunction q={};OMSpecializedFunction extended={};bool specialized=input.size()!=sizeof(q);
      if(!specialized)memcpy(&q,input.data(),sizeof(q));
      else {
       if(input.size()<sizeof(extended)){result.error=-2;break;}memcpy(&extended,input.data(),sizeof(extended));q=extended.function;
       if(extended.count>OFFLINE_METAL_MAX_CONSTANTS || input.size()!=sizeof(extended)+extended.count*sizeof(OMFunctionConstant)){result.error=-2;break;}
      }
      id<MTLLibrary> library=get(q.library,@protocol(MTLLibrary));
      if(!library || !memchr(q.name,0,sizeof(q.name))){result.error=-2;break;}
      NSString *name=[NSString stringWithUTF8String:q.name];if(!name.length){result.error=-2;break;}
      id<MTLFunction> function=nil;
      if(specialized) {
       MTLFunctionConstantValues *values=[MTLFunctionConstantValues new];NSMutableArray *rows=[NSMutableArray array];NSMutableSet *keys=[NSMutableSet set];
       for(uint64_t i=0;i<extended.count;i++) {
        OMFunctionConstant row={};memcpy(&row,input.data()+sizeof(extended)+i*sizeof(row),sizeof(row));
        if(!row.bytes || row.bytes!=OMFunctionConstantBytes(row.type) || row.named>1 || row.index>65535 ||
            (row.named && (!memchr(row.name,0,sizeof(row.name)) || row.index)) || (!row.named && row.name[0])){result.error=-2;break;}
        NSString *constantName=row.named?[NSString stringWithUTF8String:row.name]:nil;
        NSString *key=row.named?[NSString stringWithFormat:@"name:%@",constantName]:[NSString stringWithFormat:@"index:%llu",row.index];
        if((row.named && !constantName.length) || [keys containsObject:key]){result.error=-2;break;}[keys addObject:key];
        if(row.named)[values setConstantValue:row.data type:(MTLDataType)row.type withName:constantName];
        else [values setConstantValue:row.data type:(MTLDataType)row.type atIndex:row.index];
        NSMutableString *hex=[NSMutableString string];for(uint64_t j=0;j<row.bytes;j++)[hex appendFormat:@"%02x",row.data[j]];
        [rows addObject:@{@"type":@(row.type),@"index":@(row.index),@"name":constantName?:@"",@"bytes":@(row.bytes),@"data_hex":hex}];
       }
       if(result.error)break;
       function=[library newFunctionWithName:name constantValues:values error:&error];
       [specializations addObject:@{@"session":@(session),@"library":@(q.library),@"name":name,@"constants":rows,
           @"actual_success":@(function!=nil),@"actual_error":error.localizedDescription?:@""}];
      } else function=[library newFunctionWithName:name];
      result.handle=save(function);if(!result.handle){result.error=-4;break;}
      body=jsonBytes(@{@"name":function.name,@"type":@(function.functionType)});break;
     }
     case OM_TEXTURE:case OM_TEXTURE_HAZARD: {
      OMTexture q={};uint64_t hazard=0;
      if(header.operation==OM_TEXTURE){if(input.size()!=sizeof(q)){result.error=-2;break;}memcpy(&q,input.data(),sizeof(q));}
      else {if(input.size()!=sizeof(OMHazardTexture)){result.error=-2;break;}OMHazardTexture ext={};memcpy(&ext,input.data(),sizeof(ext));q=ext.texture;hazard=ext.hazard;}
      if(hazard>MTLHazardTrackingModeTracked){result.error=-2;break;}
      bool attachment=q.format==MTLPixelFormatStencil8 || q.format==MTLPixelFormatDepth32Float || q.format==MTLPixelFormatDepth32Float_Stencil8;
      if(q.type!=MTLTextureType2D || (!attachment && q.format!=MTLPixelFormatR32Float && q.format!=MTLPixelFormatRG16Float && q.format!=MTLPixelFormatBGRA8Unorm && q.format!=MTLPixelFormatRGBA8Unorm) || !q.width || !q.height || q.width>2048 || q.height>2048
          || q.depth!=1 || q.levels!=1 || q.samples!=1 || q.array!=1 || (q.storage!=MTLStorageModeShared && q.storage!=MTLStorageModePrivate) || q.cache!=MTLCPUCacheModeDefaultCache){result.error=-4;break;}
      if(attachment && (q.storage!=MTLStorageModePrivate || q.usage!=MTLTextureUsageRenderTarget)){result.error=-4;break;}
      MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)q.format width:q.width height:q.height mipmapped:NO];
      d.usage=q.usage;d.storageMode=(MTLStorageMode)q.storage;d.hazardTrackingMode=(MTLHazardTrackingMode)hazard;
      id<MTLTexture> texture=[device newTextureWithDescriptor:d];if(!texture || (hazard && texture.hazardTrackingMode!=hazard)){result.error=-4;break;}
      result.handle=save(texture);
      if(header.operation==OM_TEXTURE_HAZARD){uint64_t actual=texture.hazardTrackingMode;body=[NSData dataWithBytes:&actual length:sizeof(actual)];[textureHazards addObject:@{@"texture":@(result.handle),@"session":@(session),@"requested":@(hazard),@"actual":@(actual)}];}
      break;
     }
     case OM_PIPELINE:case OM_PIPELINE_BLEND:case OM_PIPELINE_ATTACHMENTS: {
      OMPipeline q={};OMBlend blends[8]={};uint64_t depth=0,stencil=0;OMVertexPipeline vertexInput={};bool hasVertexInput=false;
      if(header.operation==OM_PIPELINE) {if(input.size()!=sizeof(q)){result.error=-2;break;}memcpy(&q,input.data(),sizeof(q));}
      else if(header.operation==OM_PIPELINE_BLEND){if(input.size()!=sizeof(OMBlendPipeline)){result.error=-2;break;}OMBlendPipeline ext={};memcpy(&ext,input.data(),sizeof(ext));q=ext.pipeline;memcpy(blends,ext.colors,sizeof(blends));}
      else {
       if(input.size()==sizeof(vertexInput)) {memcpy(&vertexInput,input.data(),sizeof(vertexInput));if(!OMVertexInputsValid(vertexInput)){result.error=-2;break;}hasVertexInput=true;}
       else if(input.size()!=sizeof(OMAttachmentPipeline)){result.error=-2;break;}
       OMAttachmentPipeline ext={};memcpy(&ext,input.data(),sizeof(ext));q=ext.colors.pipeline;memcpy(blends,ext.colors.colors,sizeof(blends));depth=ext.depthFormat;stencil=ext.stencilFormat;
      }
      id<MTLFunction> vertex=get(q.vertex,@protocol(MTLFunction)),fragment=get(q.fragment,@protocol(MTLFunction));
      if(!vertex || !fragment || q.samples!=1 || (depth && depth!=MTLPixelFormatDepth32Float && depth!=MTLPixelFormatDepth32Float_Stencil8) ||
          (stencil && stencil!=MTLPixelFormatStencil8 && stencil!=MTLPixelFormatDepth32Float_Stencil8)){result.error=-2;break;}
      MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=vertex;d.fragmentFunction=fragment;d.rasterSampleCount=q.samples;
      d.depthAttachmentPixelFormat=(MTLPixelFormat)depth;d.stencilAttachmentPixelFormat=(MTLPixelFormat)stencil;
      NSMutableArray *actualAttributes=[NSMutableArray array],*actualLayouts=[NSMutableArray array];
      if(hasVertexInput) {
       MTLVertexDescriptor *v=[MTLVertexDescriptor vertexDescriptor];bool used[31]={};
       for(unsigned i=0;i<31;i++) {
        const auto &a=vertexInput.attributes[i];if(!a.format)continue;
        v.attributes[i].format=(MTLVertexFormat)a.format;v.attributes[i].offset=a.offset;v.attributes[i].bufferIndex=a.buffer;used[a.buffer]=true;
        [actualAttributes addObject:@{@"index":@(i),@"format":@(v.attributes[i].format),@"offset":@(v.attributes[i].offset),@"buffer":@(v.attributes[i].bufferIndex)}];
       }
       for(unsigned i=0;i<31;i++)if(used[i]) {
        const auto &l=vertexInput.layouts[i];v.layouts[i].stride=l.stride;v.layouts[i].stepFunction=(MTLVertexStepFunction)l.step;v.layouts[i].stepRate=l.rate;
        [actualLayouts addObject:@{@"index":@(i),@"stride":@(v.layouts[i].stride),@"step":@(v.layouts[i].stepFunction),@"rate":@(v.layouts[i].stepRate)}];
       }
       d.vertexDescriptor=v;
      }
      for(unsigned i=0;i<8;i++) {
       d.colorAttachments[i].pixelFormat=(MTLPixelFormat)q.formats[i];d.colorAttachments[i].writeMask=(MTLColorWriteMask)q.masks[i];
       if(header.operation!=OM_PIPELINE) {
        const OMBlend &b=blends[i];if(b.enabled>1 || b.sourceRGB>MTLBlendFactorOneMinusSource1Alpha || b.destinationRGB>MTLBlendFactorOneMinusSource1Alpha ||
           b.sourceAlpha>MTLBlendFactorOneMinusSource1Alpha || b.destinationAlpha>MTLBlendFactorOneMinusSource1Alpha || b.rgbOperation>MTLBlendOperationMax || b.alphaOperation>MTLBlendOperationMax){result.error=-2;break;}
        MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];a.blendingEnabled=b.enabled;
        a.sourceRGBBlendFactor=(MTLBlendFactor)b.sourceRGB;a.destinationRGBBlendFactor=(MTLBlendFactor)b.destinationRGB;a.rgbBlendOperation=(MTLBlendOperation)b.rgbOperation;
        a.sourceAlphaBlendFactor=(MTLBlendFactor)b.sourceAlpha;a.destinationAlphaBlendFactor=(MTLBlendFactor)b.destinationAlpha;a.alphaBlendOperation=(MTLBlendOperation)b.alphaOperation;
       }
      }
      if(result.error)break;result.handle=save([device newRenderPipelineStateWithDescriptor:d error:&error]);if(!result.handle)result.error=-3;
      if(result.handle && hasVertexInput)[vertexPipelines addObject:@{@"pipeline":@(result.handle),@"session":@(session),@"vertex":vertex.name,@"fragment":fragment.name,@"attributes":actualAttributes,@"layouts":actualLayouts}];
      if(result.handle && header.operation==OM_PIPELINE_BLEND)[blendPipelines addObject:@{@"pipeline":@(result.handle),@"vertex":vertex.name,@"fragment":fragment.name}];break;
     }
     case OM_FENCE:
      if(!input.empty()){result.error=-2;break;}
      result.handle=save([device newFence]);if(!result.handle){result.error=-4;break;}
      [fencesCreated addObject:@{@"fence":@(result.handle),@"session":@(session)}];break;
     case OM_RENDER_FENCE_UPDATE:case OM_RENDER_FENCE_WAIT:case OM_BLIT_FENCE_UPDATE:case OM_BLIT_FENCE_WAIT: {
      REQUEST(OMFenceDependency);
      bool render=header.operation==OM_RENDER_FENCE_UPDATE || header.operation==OM_RENDER_FENCE_WAIT;
      bool update=header.operation==OM_RENDER_FENCE_UPDATE || header.operation==OM_BLIT_FENCE_UPDATE;
      id encoder=getEncoder(q.encoder,render?@protocol(MTLRenderCommandEncoder):@protocol(MTLBlitCommandEncoder));
      id<MTLFence> fence=get(q.fence,@protocol(MTLFence));NSDictionary *active=openEncoders[@(q.encoder)];
      if(!encoder || !fence || !active || (render?(!q.stages || (q.stages&~3ULL)):q.stages!=0)){result.error=-2;break;}
      id<MTLCommandBuffer> command=active[@"command_object"];NSDictionary *producer=fenceProducers[@(q.fence)];
      if(!update) {
       // Same-queue producers must be actually committed. Cross-queue fences
       // stay limited to completed producers after a retained real pending-
       // producer readback failure. Use actual GPU events for overlap across
       // queues; CPU encoding metadata never represents a signaled fence.
       id<MTLCommandBuffer> source=producer[@"command"];MTLCommandBufferStatus actual=source.status;
       if(!producer || [producer[@"encoder"] unsignedLongLongValue]==q.encoder || openEncoders[producer[@"encoder"]] ||
          (source!=command && (actual<MTLCommandBufferStatusCommitted || actual>MTLCommandBufferStatusCompleted ||
             (source.commandQueue!=command.commandQueue && actual!=MTLCommandBufferStatusCompleted)))){result.error=-4;break;}
      }
      if(render) {
       if(update)[(id<MTLRenderCommandEncoder>)encoder updateFence:fence afterStages:(MTLRenderStages)q.stages];
       else [(id<MTLRenderCommandEncoder>)encoder waitForFence:fence beforeStages:(MTLRenderStages)q.stages];
      } else {
       if(update)[(id<MTLBlitCommandEncoder>)encoder updateFence:fence];else [(id<MTLBlitCommandEncoder>)encoder waitForFence:fence];
      }
      [fenceOperations addObject:@{@"fence":@(q.fence),@"encoder":@(q.encoder),@"session":@(session),@"command":active[@"command_handle"],@"stages":@(q.stages),@"render":@(render),@"update":@(update)}];
      if(update)fenceProducers[@(q.fence)]=@{@"encoder":@(q.encoder),@"command":command};break;
     }
     case OM_SHARED_EVENT: {
      if(!input.empty()){result.error=-2;break;}id<MTLSharedEvent> event=[device newSharedEvent];if(!event){result.error=-4;break;}
      if(event.device && event.device.registryID!=device.registryID){result.error=-4;break;}
      result.handle=save(event);sharedStates[@(result.handle)]=[NSMutableDictionary dictionary];[eventsCreated addObject:@{@"event":@(result.handle),@"session":@(session),@"shared":@YES,@"device_is_nil":@(event.device==nil)}];
      body=jsonBytes(@{@"device_is_nil":@(event.device==nil),@"device_is_owner":@(event.device && event.device.registryID==device.registryID),@"signaled_value":@(event.signaledValue),@"label":event.label?:[NSNull null]});break;
     }
     case OM_SHARED_VALUE_GET: {
      REQUEST(OMHandle);id<MTLSharedEvent> event=get(q.handle,@protocol(MTLSharedEvent));if(!event){result.error=-2;break;}
      uint64_t value=event.signaledValue;body=[NSData dataWithBytes:&value length:sizeof(value)];
      [sharedValues addObject:@{@"event":@(q.handle),@"session":@(session),@"value":@(value),@"cpu_set":@NO}];break;
     }
     case OM_SHARED_VALUE_SET: {
      REQUEST(OMSharedEventValue);id<MTLSharedEvent> event=get(q.event,@protocol(MTLSharedEvent));if(!event || q.value<event.signaledValue){result.error=-2;break;}
      event.signaledValue=q.value;uint64_t actual=event.signaledValue;body=[NSData dataWithBytes:&actual length:sizeof(actual)];
      [sharedValues addObject:@{@"event":@(q.event),@"session":@(session),@"value":@(actual),@"requested":@(q.value),@"cpu_set":@YES}];break;
     }
     case OM_SHARED_LABEL_SET: {
      if(input.size()<sizeof(OMSharedLabel)){result.error=-2;break;}OMSharedLabel q={};memcpy(&q,input.data(),sizeof(q));id<MTLSharedEvent> event=get(q.event,@protocol(MTLSharedEvent));
      if(!event || q.present>1 || q.bytes>512 || input.size()!=sizeof(q)+q.bytes || (!q.present && q.bytes)){result.error=-2;break;}
      NSString *label=q.present?[[NSString alloc] initWithBytes:input.data()+sizeof(q) length:q.bytes encoding:NSUTF8StringEncoding]:nil;
      if(q.present && !label){result.error=-2;break;}event.label=label;body=jsonBytes(@{@"label":event.label?:[NSNull null]});
      [sharedLabels addObject:@{@"event":@(q.event),@"session":@(session),@"actual_label":event.label?:[NSNull null]}];break;
     }
     case OM_SHARED_NOTIFY: {
      REQUEST(OMSharedEventValue);id<MTLSharedEvent> event=get(q.event,@protocol(MTLSharedEvent));if(!event || !sharedListener){result.error=-2;break;}
      uint64_t identifier=nextNotification++;NSMutableDictionary *record=[@{@"session":@(session),@"event":@(q.event),@"requested":@(q.value),@"ready":@NO,@"value":@0,@"cancelled":@NO,@"identity_match":@NO} mutableCopy];
      [notificationLock lock];notifications[@(identifier)]=record;[notificationLock unlock];__weak id<MTLSharedEvent> expected=event;
      [event notifyListener:sharedListener atValue:q.value block:^(id<MTLSharedEvent> actual,uint64_t value) {
       [notificationLock lock];if(![record[@"cancelled"] boolValue]){record[@"ready"]=@YES;record[@"value"]=@(value);record[@"identity_match"]=@(actual==expected);}[notificationLock unlock];
      }];result.handle=identifier;break;
     }
     case OM_SHARED_NOTIFICATION_POLL: {
      REQUEST(OMHandle);[notificationLock lock];NSMutableDictionary *record=notifications[@(q.handle)];
      if(!record || [record[@"session"] unsignedLongLongValue]!=session){result.error=-2;[notificationLock unlock];break;}
      OMSharedNotification value={};value.ready=[record[@"ready"] boolValue];
      if(value.ready){if(![record[@"identity_match"] boolValue] || [record[@"value"] unsignedLongLongValue]<[record[@"requested"] unsignedLongLongValue])result.error=-3;else {value.event=[record[@"event"] unsignedLongLongValue];value.value=[record[@"value"] unsignedLongLongValue];}}
      [notificationLock unlock];if(!result.error)body=[NSData dataWithBytes:&value length:sizeof(value)];break;
     }
     case OM_SHARED_EXPORT: {
      REQUEST(OMHandle);id<MTLSharedEvent> event=get(q.handle,@protocol(MTLSharedEvent));if(!expectedSessions || !event || !sharedStates[@(q.handle)]){result.error=-2;break;}
      MTLSharedEventHandle *handle=[event newSharedEventHandle];if(!handle){result.error=-4;break;}result.handle=nextExport++;sharedExports[@(result.handle)]=@{@"handle":handle,@"state":sharedStates[@(q.handle)]};
      Method trace=class_getInstanceMethod(object_getClass(handle),sel_registerName("labelTraceID"));BOOL supported=trace && !strcmp(method_getTypeEncoding(trace),"Q16@0:8");
      uint64_t traceID=supported?((uint64_t (*)(id,SEL))objc_msgSend)(handle,sel_registerName("labelTraceID")):0;
      body=jsonBytes(@{@"label":handle.label?:[NSNull null],@"trace_id_supported":@(supported),@"trace_id":supported?@(traceID):[NSNull null]});
      [exportJournal addObject:@{@"operation":@"export",@"export":@(result.handle),@"event":@(q.handle),@"session":@(session)}];break;
     }
     case OM_SHARED_IMPORT: {
      REQUEST(OMHandle);NSDictionary *exported=sharedExports[@(q.handle)];MTLSharedEventHandle *handle=exported[@"handle"];if(!session || !handle){result.error=-2;break;}
      id<MTLSharedEvent> event=[device newSharedEventWithHandle:handle];if(!event || (event.device && event.device.registryID!=device.registryID)){result.error=-4;break;}result.handle=save(event);sharedStates[@(result.handle)]=exported[@"state"];
      body=jsonBytes(@{@"device_is_nil":@(event.device==nil),@"device_is_owner":@(event.device && event.device.registryID==device.registryID),@"signaled_value":@(event.signaledValue),@"label":event.label?:[NSNull null]});
      [eventsCreated addObject:@{@"event":@(result.handle),@"session":@(session),@"shared":@YES,@"imported":@YES,@"device_is_nil":@(event.device==nil)}];
      [exportJournal addObject:@{@"operation":@"import",@"export":@(q.handle),@"event":@(result.handle),@"session":@(session)}];break;
     }
     case OM_SHARED_EXPORT_DROP: {
      REQUEST(OMHandle);if(!expectedSessions || session || !sharedExports[@(q.handle)]){result.error=-2;break;}[sharedExports removeObjectForKey:@(q.handle)];
      [exportJournal addObject:@{@"operation":@"drop",@"export":@(q.handle),@"session":@0}];finished=openedSessions==expectedSessions && !sessions.count && !sharedExports.count;break;
     }
     case OM_EVENT:
      if(!input.empty()){result.error=-2;break;}
      result.handle=save([device newEvent]);if(!result.handle){result.error=-4;break;}
      [eventsCreated addObject:@{@"event":@(result.handle),@"session":@(session)}];break;
     case OM_EVENT_SIGNAL:case OM_EVENT_WAIT: {
      REQUEST(OMEventDependency);id<MTLCommandBuffer> command=get(q.command,@protocol(MTLCommandBuffer));id<MTLEvent> event=get(q.event,@protocol(MTLEvent));
      if(!command || !event || !q.value || command.status>MTLCommandBufferStatusEnqueued || hasOpenEncoder(q.command)){result.error=-2;break;}
      NSMutableDictionary *shared=sharedStates[@(q.event)];NSDictionary *producer=shared?shared[@"producer"]:eventProducers[@(q.event)];bool signal=header.operation==OM_EVENT_SIGNAL;
      uint64_t previous=[producer[@"value"] unsignedLongLongValue],actualValue=shared?[(id<MTLSharedEvent>)event signaledValue]:0;
      uint64_t actualProducer=0;
      if(signal) {
       if(q.value<=previous || (shared && q.value<=actualValue)){result.error=-2;break;}
       [command encodeSignalEvent:event value:q.value];NSDictionary *record=@{@"value":@(q.value),@"command":command,@"session":@(session)};if(shared)shared[@"producer"]=record;else eventProducers[@(q.event)]=record;
      } else {
       id<MTLCommandBuffer> source=producer[@"command"];actualProducer=source.status;
       if(!(shared && actualValue>=q.value) && (!producer || q.value>previous || source==command || actualProducer<MTLCommandBufferStatusCommitted || actualProducer>MTLCommandBufferStatusCompleted)){result.error=-4;break;}
       [command encodeWaitForEvent:event value:q.value];
      }
      [eventOperations addObject:@{@"event":@(q.event),@"command":@(q.command),@"session":@(session),@"value":@(q.value),@"signal":@(signal),@"shared":@(shared!=nil),@"actual_signaled_value":@(actualValue),@"actual_producer_status":@(actualProducer)}];break;
     }
     case OM_COMMAND: {
      REQUEST(OMHandle);id<MTLCommandQueue> queue=get(q.handle,@protocol(MTLCommandQueue));
      if(!queue){result.error=-2;break;}result.handle=save([queue commandBuffer]);if(!result.handle)result.error=-3;break;
     }
     case OM_COMMAND_ENQUEUE: {
      REQUEST(OMHandle);id<MTLCommandBuffer> command=get(q.handle,@protocol(MTLCommandBuffer));
      if(!command || command.status!=MTLCommandBufferStatusNotEnqueued){result.error=-2;break;}
      // This synchronous transport currently supports one pending reservation
      // per real queue. Refuse unsupported ordering before blocking the server
      // behind a command whose predecessor cannot yet receive its commit.
      for(NSDictionary *row in pendingEnqueues.allValues)if([row[@"command"] commandQueue]==command.commandQueue){result.error=-4;break;}
      if(result.error)break;[command enqueue];uint64_t status=command.status;
      if(status!=MTLCommandBufferStatusEnqueued){result.error=-3;break;}
      pendingEnqueues[@(q.handle)]=@{@"command":command,@"session":@(session)};
      body=[NSData dataWithBytes:&status length:sizeof(status)];
      [enqueued addObject:@{@"command":@(q.handle),@"session":@(session),@"status":@(status),@"operation_request":@(request)}];break;
     }
     case OM_RENDER:case OM_RENDER_ATTACHMENTS: {
      OMRender q={};OMDepthAttachment depth={};OMStencilAttachment stencil={};
      if(header.operation==OM_RENDER){if(input.size()!=sizeof(q)){result.error=-2;break;}memcpy(&q,input.data(),sizeof(q));}
      else {if(input.size()!=sizeof(OMAttachmentRender)){result.error=-2;break;}OMAttachmentRender ext={};memcpy(&ext,input.data(),sizeof(ext));q=ext.colors;depth=ext.depth;stencil=ext.stencil;}
      id<MTLCommandBuffer> command=get(q.command,@protocol(MTLCommandBuffer));if(!command || command.status>MTLCommandBufferStatusEnqueued || hasOpenEncoder(q.command)){result.error=-2;break;}
      MTLRenderPassDescriptor *d=[MTLRenderPassDescriptor renderPassDescriptor];NSUInteger width=0,height=0;
      for(unsigned i=0;i<8;i++)if(q.colors[i].texture) {
       id<MTLTexture> texture=get(q.colors[i].texture,@protocol(MTLTexture));if(!texture || q.colors[i].load>MTLLoadActionClear || q.colors[i].store>MTLStoreActionStore || (width && (width!=texture.width || height!=texture.height))){result.error=-2;break;}
       width=texture.width;height=texture.height;
       d.colorAttachments[i].texture=texture;d.colorAttachments[i].loadAction=(MTLLoadAction)q.colors[i].load;d.colorAttachments[i].storeAction=(MTLStoreAction)q.colors[i].store;
       const double *v=q.colors[i].clear;d.colorAttachments[i].clearColor=MTLClearColorMake(v[0],v[1],v[2],v[3]);
      }
      if(depth.texture) {
       id<MTLTexture> texture=get(depth.texture,@protocol(MTLTexture));
       if(!texture || (texture.pixelFormat!=MTLPixelFormatDepth32Float && texture.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8) ||
           depth.load>MTLLoadActionClear || depth.store>MTLStoreActionStore || !std::isfinite(depth.clear) || depth.clear<0 || depth.clear>1 ||
           (width && (width!=texture.width || height!=texture.height))){result.error=-2;break;}
       width=texture.width;height=texture.height;d.depthAttachment.texture=texture;d.depthAttachment.loadAction=(MTLLoadAction)depth.load;
       d.depthAttachment.storeAction=(MTLStoreAction)depth.store;d.depthAttachment.clearDepth=depth.clear;
      }
      if(stencil.texture) {
       id<MTLTexture> texture=get(stencil.texture,@protocol(MTLTexture));
       if(!texture || (texture.pixelFormat!=MTLPixelFormatStencil8 && texture.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8) ||
           stencil.load>MTLLoadActionClear || stencil.store>MTLStoreActionStore || stencil.clear>255 ||
           (width && (width!=texture.width || height!=texture.height))){result.error=-2;break;}
       width=texture.width;height=texture.height;d.stencilAttachment.texture=texture;d.stencilAttachment.loadAction=(MTLLoadAction)stencil.load;
       d.stencilAttachment.storeAction=(MTLStoreAction)stencil.store;d.stencilAttachment.clearStencil=(uint32_t)stencil.clear;
      }
      if(!width || !height){result.error=-2;break;}
      if(result.error)break;result.handle=save([command renderCommandEncoderWithDescriptor:d]);if(!result.handle)result.error=-3;
      else {
       renderExtents[@(result.handle)]=@[@(width),@(height)];
       openEncoders[@(result.handle)]=@{@"encoder":objects[@(result.handle)],@"command_object":command,@"command_handle":@(q.command),@"session":@(session)};
       if(depth.texture || stencil.texture)[attachmentPasses addObject:@{@"command":@(q.command),@"encoder":@(result.handle),@"session":@(session),
           @"depth":@(depth.texture),@"stencil":@(stencil.texture),@"depth_clear":@(depth.clear),@"stencil_clear":@(stencil.clear),
           @"depth_load":@(depth.load),@"stencil_load":@(stencil.load),@"width":@(width),@"height":@(height)}];
      }break;
     }
     case OM_DEPTH_STENCIL_STATE: {
      REQUEST(OMDepthStencil);
      auto validFace=[](const OMStencilFace &f)->bool {
       if(f.enabled>1)return false;
       if(!f.enabled)return !(f.compare|f.failure|f.depthFailure|f.pass|f.readMask|f.writeMask);
       return f.compare<=MTLCompareFunctionAlways && f.failure<=MTLStencilOperationDecrementWrap &&
           f.depthFailure<=MTLStencilOperationDecrementWrap && f.pass<=MTLStencilOperationDecrementWrap && f.readMask<=UINT32_MAX && f.writeMask<=UINT32_MAX;
      };
      if(q.compare>MTLCompareFunctionAlways || q.write>1 || !validFace(q.front) || !validFace(q.back)){result.error=-2;break;}
      auto face=[](const OMStencilFace &f)->MTLStencilDescriptor * {
       if(!f.enabled)return nil;MTLStencilDescriptor *s=[MTLStencilDescriptor new];s.stencilCompareFunction=(MTLCompareFunction)f.compare;
       s.stencilFailureOperation=(MTLStencilOperation)f.failure;s.depthFailureOperation=(MTLStencilOperation)f.depthFailure;s.depthStencilPassOperation=(MTLStencilOperation)f.pass;
       s.readMask=(uint32_t)f.readMask;s.writeMask=(uint32_t)f.writeMask;return s;
      };
      MTLDepthStencilDescriptor *d=[MTLDepthStencilDescriptor new];d.depthCompareFunction=(MTLCompareFunction)q.compare;d.depthWriteEnabled=q.write;
      d.frontFaceStencil=face(q.front);d.backFaceStencil=face(q.back);result.handle=save([device newDepthStencilStateWithDescriptor:d]);
      if(!result.handle){result.error=-3;break;}
      [depthStates addObject:@{@"state":@(result.handle),@"session":@(session),@"depth_compare":@(q.compare),@"depth_write":@(q.write),
          @"front_compare":@(q.front.compare),@"back_compare":@(q.back.compare),@"front_write_mask":@(q.front.writeMask),@"back_write_mask":@(q.back.writeMask)}];break;
     }
     case OM_SET_DEPTH_STENCIL: {
      REQUEST(OMSetDepthStencil);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));id<MTLDepthStencilState> state=q.state?get(q.state,@protocol(MTLDepthStencilState)):nil;
      if(!encoder || (q.state && !state)){result.error=-2;break;}[encoder setDepthStencilState:state];break;
     }
     case OM_STENCIL_REFERENCE: {
      REQUEST(OMStencilReference);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));
      if(!encoder || q.front>UINT32_MAX || q.back>UINT32_MAX){result.error=-2;break;}
      [encoder setStencilFrontReferenceValue:(uint32_t)q.front backReferenceValue:(uint32_t)q.back];
      [stencilReferences addObject:@{@"encoder":@(q.encoder),@"session":@(session),@"front":@(q.front),@"back":@(q.back)}];break;
     }
     case OM_SET_PIPELINE: {
      REQUEST(OMSetPipeline);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));id<MTLRenderPipelineState> pipeline=get(q.pipeline,@protocol(MTLRenderPipelineState));
      if(!encoder || !pipeline){result.error=-2;break;}[encoder setRenderPipelineState:pipeline];break;
     }
     case OM_SET_CULL: {
      REQUEST(OMSetCull);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));if(!encoder || q.mode>MTLCullModeBack){result.error=-2;break;}[encoder setCullMode:(MTLCullMode)q.mode];break;
     }
     case OM_DRAW: {
      REQUEST(OMDraw);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));
      if(!encoder || q.primitive>MTLPrimitiveTypeTriangleStrip || !q.count || q.count>65536 || q.start>65536 || !q.instances || q.instances>1024){result.error=-2;break;}
      [encoder drawPrimitives:(MTLPrimitiveType)q.primitive vertexStart:q.start vertexCount:q.count instanceCount:q.instances];break;
     }
     case OM_END: {
      REQUEST(OMHandle);id<MTLCommandEncoder> encoder=get(q.handle,@protocol(MTLCommandEncoder));if(!encoder || !openEncoders[@(q.handle)]){result.error=-2;break;}[encoder endEncoding];[openEncoders removeObjectForKey:@(q.handle)];break;
     }
     case OM_COMMIT:case OM_COMMAND_SUBMIT: {
      REQUEST(OMHandle);id<MTLCommandBuffer> command=get(q.handle,@protocol(MTLCommandBuffer));if(!command || hasOpenEncoder(q.handle)){result.error=-2;break;}
      if(command.status>MTLCommandBufferStatusEnqueued){result.error=-2;break;}
      for(NSNumber *handle in pendingEnqueues)if(handle.unsignedLongLongValue!=q.handle && [pendingEnqueues[handle][@"command"] commandQueue]==command.commandQueue){result.error=-4;break;}
      if(result.error)break;[command commit];[pendingEnqueues removeObjectForKey:@(q.handle)];
      if(header.operation==OM_COMMIT)[command waitUntilCompleted];
      else asyncCommands[@(q.handle)]=@{@"session":@(session),@"command":command};
      OMCommandResult value=observeCommand(q.handle,command,header.operation==OM_COMMIT?@"blocking-commit":@"nonblocking-submit");error=command.error;
      if(value.status<MTLCommandBufferStatusCommitted || value.status>MTLCommandBufferStatusCompleted || error ||
          (value.status==MTLCommandBufferStatusCompleted && (!value.start_ns || value.end_ns<=value.start_ns))){result.error=-3;break;}
      if(header.operation==OM_COMMAND_SUBMIT)[submitted addObject:@{@"command":@(q.handle),@"session":@(session),@"actual_status":@(value.status),@"wait_until_completed_called":@NO}];
      body=[NSData dataWithBytes:&value length:sizeof(value)];break;
     }
     case OM_COMMAND_STATUS: {
      REQUEST(OMHandle);id<MTLCommandBuffer> command=get(q.handle,@protocol(MTLCommandBuffer));
      if(!command || command.status<MTLCommandBufferStatusCommitted){result.error=-2;break;}
      OMCommandResult value=observeCommand(q.handle,command,@"status-query");
      [commandObservations addObject:@{@"command":@(q.handle),@"session":@(session),@"actual_status":@(value.status),@"gpu_start_ns":@(value.start_ns),@"gpu_end_ns":@(value.end_ns)}];
      error=command.error;
      if(value.status>MTLCommandBufferStatusCompleted || error || (value.status==MTLCommandBufferStatusCompleted && (!value.start_ns || value.end_ns<=value.start_ns))){result.error=-3;break;}
      body=[NSData dataWithBytes:&value length:sizeof(value)];break;
     }
     case OM_READ_TEXTURE: {
      REQUEST(OMReadTexture);id<MTLTexture> texture=get(q.texture,@protocol(MTLTexture));
      if(!texture || texture.storageMode!=MTLStorageModeShared || q.level || !q.width || !q.height || q.x>texture.width || q.width>texture.width-q.x
          || q.y>texture.height || q.height>texture.height-q.y || (texture.pixelFormat!=MTLPixelFormatR32Float && texture.pixelFormat!=MTLPixelFormatRG16Float && texture.pixelFormat!=MTLPixelFormatBGRA8Unorm && texture.pixelFormat!=MTLPixelFormatRGBA8Unorm)
          || q.width>OFFLINE_METAL_MAX_REPLY/4/q.height){result.error=-2;break;}
      NSMutableData *pixels=[NSMutableData dataWithLength:q.width*q.height*4];
      [texture getBytes:pixels.mutableBytes bytesPerRow:q.width*4 fromRegion:MTLRegionMake2D(q.x,q.y,q.width,q.height) mipmapLevel:0];body=pixels;
      [readbacks addObject:@{@"texture":@(q.texture),@"bytes":@(pixels.length),@"sha256":hashBytes(pixels.bytes,pixels.length)}];break;
     }
     case OM_WRITE_TEXTURE: {
      if(input.size()<sizeof(OMWriteTexture)){result.error=-2;break;}OMWriteTexture q={};memcpy(&q,input.data(),sizeof(q));
      id<MTLTexture> texture=get(q.texture,@protocol(MTLTexture));
      if(!texture || texture.storageMode!=MTLStorageModeShared || q.level || !q.width || !q.height || q.x>texture.width || q.width>texture.width-q.x
          || q.y>texture.height || q.height>texture.height-q.y || (texture.pixelFormat!=MTLPixelFormatR32Float && texture.pixelFormat!=MTLPixelFormatRG16Float && texture.pixelFormat!=MTLPixelFormatBGRA8Unorm && texture.pixelFormat!=MTLPixelFormatRGBA8Unorm)
          || q.width>SIZE_MAX/4 || q.stride!=q.width*4 || q.height>(input.size()-sizeof(q))/q.stride
          || q.height*q.stride!=input.size()-sizeof(q)){result.error=-2;break;}
      const void *pixels=input.data()+sizeof(q);
      [texture replaceRegion:MTLRegionMake2D(q.x,q.y,q.width,q.height) mipmapLevel:0 withBytes:pixels bytesPerRow:q.stride];
      [uploads addObject:@{@"texture":@(q.texture),@"x":@(q.x),@"y":@(q.y),@"width":@(q.width),@"height":@(q.height),
          @"bytes":@(q.stride*q.height),@"sha256":hashBytes(pixels,q.stride*q.height)}];break;
     }
     case OM_BUFFER: {
      REQUEST(OMBuffer);
      if(!q.bytes || q.bytes>OFFLINE_METAL_MAX_BUFFER || q.bytes>device.maxBufferLength || q.options){result.error=-4;break;}
      result.handle=save([device newBufferWithLength:q.bytes options:MTLResourceStorageModeShared]);if(!result.handle)result.error=-3;break;
     }
     case OM_WRITE_BUFFER: {
      if(input.size()<sizeof(OMBufferRegion)){result.error=-2;break;}OMBufferRegion q={};memcpy(&q,input.data(),sizeof(q));
      id<MTLBuffer> buffer=get(q.buffer,@protocol(MTLBuffer));
      if(!buffer || !q.bytes || q.offset>buffer.length || q.bytes>buffer.length-q.offset || q.bytes!=input.size()-sizeof(q)){result.error=-2;break;}
      memcpy((unsigned char *)buffer.contents+q.offset,input.data()+sizeof(q),q.bytes);
      [bufferWrites addObject:@{@"buffer":@(q.buffer),@"offset":@(q.offset),@"bytes":@(q.bytes),@"sha256":hashBytes(input.data()+sizeof(q),q.bytes)}];break;
     }
     case OM_READ_BUFFER: {
      REQUEST(OMBufferRegion);id<MTLBuffer> buffer=get(q.buffer,@protocol(MTLBuffer));
      if(!buffer || !q.bytes || q.bytes>OFFLINE_METAL_MAX_REPLY || q.offset>buffer.length || q.bytes>buffer.length-q.offset){result.error=-2;break;}
      body=[NSData dataWithBytes:(unsigned char *)buffer.contents+q.offset length:q.bytes];
      [bufferReads addObject:@{@"buffer":@(q.buffer),@"offset":@(q.offset),@"bytes":@(q.bytes),@"sha256":hashBytes(body.bytes,body.length)}];break;
     }
     case OM_VERTEX_BUFFER:case OM_FRAGMENT_BUFFER: {
      REQUEST(OMSetBuffer);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));
      id<MTLBuffer> buffer=q.buffer?get(q.buffer,@protocol(MTLBuffer)):nil;
      if(!encoder || q.index>=31 || (q.buffer && !buffer) || (!buffer && q.offset) || (buffer && q.offset>=buffer.length)){result.error=-2;break;}
      if(header.operation==OM_VERTEX_BUFFER)[encoder setVertexBuffer:buffer offset:q.offset atIndex:q.index];
      else [encoder setFragmentBuffer:buffer offset:q.offset atIndex:q.index];break;
     }
     case OM_DRAW_INDEXED: {
      REQUEST(OMDrawIndexed);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));id<MTLBuffer> buffer=get(q.buffer,@protocol(MTLBuffer));
      uint64_t stride=q.type==MTLIndexTypeUInt16?2:4;
      if(!encoder || !buffer || q.primitive>MTLPrimitiveTypeTriangleStrip || q.type>MTLIndexTypeUInt32 || !q.count ||
         q.count>65536 || !q.instances || q.instances>1024 || q.offset%stride || q.offset>buffer.length || q.count>(buffer.length-q.offset)/stride){result.error=-2;break;}
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)q.primitive indexCount:q.count indexType:(MTLIndexType)q.type
          indexBuffer:buffer indexBufferOffset:q.offset instanceCount:q.instances baseVertex:q.baseVertex baseInstance:q.baseInstance];break;
     }
     case OM_BLIT: {
      REQUEST(OMHandle);id<MTLCommandBuffer> command=get(q.handle,@protocol(MTLCommandBuffer));if(!command || command.status>MTLCommandBufferStatusEnqueued || hasOpenEncoder(q.handle)){result.error=-2;break;}
      result.handle=save([command blitCommandEncoder]);if(!result.handle)result.error=-3;
      else openEncoders[@(result.handle)]=@{@"encoder":objects[@(result.handle)],@"command_object":command,@"command_handle":@(q.handle),@"session":@(session)};break;
     }
     case OM_COPY_BUFFER: {
      REQUEST(OMCopyBuffer);id<MTLBlitCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLBlitCommandEncoder));
      id<MTLBuffer> source=get(q.source,@protocol(MTLBuffer)),destination=get(q.destination,@protocol(MTLBuffer));
      if(!encoder || !source || !destination || !q.bytes || ((q.sourceOffset|q.destinationOffset|q.bytes)&3) || q.sourceOffset>source.length || q.bytes>source.length-q.sourceOffset ||
          q.destinationOffset>destination.length || q.bytes>destination.length-q.destinationOffset ||
          (source==destination && q.sourceOffset<q.destinationOffset+q.bytes && q.destinationOffset<q.sourceOffset+q.bytes)){result.error=-2;break;}
      [encoder copyFromBuffer:source sourceOffset:q.sourceOffset toBuffer:destination destinationOffset:q.destinationOffset size:q.bytes];
      [bufferCopies addObject:@{@"source":@(q.source),@"source_offset":@(q.sourceOffset),@"destination":@(q.destination),@"destination_offset":@(q.destinationOffset),@"bytes":@(q.bytes)}];break;
     }
     case OM_COPY_TEXTURE: {
      REQUEST(OMCopyTexture);id<MTLBlitCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLBlitCommandEncoder));
      id<MTLTexture> source=get(q.source,@protocol(MTLTexture)),destination=get(q.destination,@protocol(MTLTexture));
      if(!encoder || !source || !destination || source==destination || source.pixelFormat!=destination.pixelFormat ||
          source.textureType!=MTLTextureType2D || destination.textureType!=MTLTextureType2D ||
          q.sourceSlice || q.destinationSlice || q.sourceLevel || q.destinationLevel || q.sourceOrigin[2] || q.destinationOrigin[2] || q.sourceSize[2]!=1 ||
          !q.sourceSize[0] || !q.sourceSize[1] || q.sourceOrigin[0]>source.width || q.sourceSize[0]>source.width-q.sourceOrigin[0] ||
          q.sourceOrigin[1]>source.height || q.sourceSize[1]>source.height-q.sourceOrigin[1] ||
          q.destinationOrigin[0]>destination.width || q.sourceSize[0]>destination.width-q.destinationOrigin[0] ||
          q.destinationOrigin[1]>destination.height || q.sourceSize[1]>destination.height-q.destinationOrigin[1]){result.error=-2;break;}
      if(source.pixelFormat!=MTLPixelFormatR32Float && source.pixelFormat!=MTLPixelFormatRG16Float && source.pixelFormat!=MTLPixelFormatRGBA8Unorm && source.pixelFormat!=MTLPixelFormatBGRA8Unorm){result.error=-4;break;}
      [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(q.sourceOrigin[0],q.sourceOrigin[1],0)
          sourceSize:MTLSizeMake(q.sourceSize[0],q.sourceSize[1],1) toTexture:destination destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(q.destinationOrigin[0],q.destinationOrigin[1],0)];
      [textureCopies addObject:@{@"source":@(q.source),@"destination":@(q.destination),@"session":@(session),@"format":@(source.pixelFormat),
          @"source_storage":@(source.storageMode),@"destination_storage":@(destination.storageMode),@"width":@(q.sourceSize[0]),@"height":@(q.sourceSize[1])}];break;
     }
     case OM_VERTEX_TEXTURE:case OM_FRAGMENT_TEXTURE: {
      REQUEST(OMSetTexture);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));id<MTLTexture> texture=q.texture?get(q.texture,@protocol(MTLTexture)):nil;
      if(!encoder || q.index>=128 || (q.texture && !texture)){result.error=-2;break;}
      if(header.operation==OM_VERTEX_TEXTURE)[encoder setVertexTexture:texture atIndex:q.index];else [encoder setFragmentTexture:texture atIndex:q.index];
      [textureBindings addObject:@{@"encoder":@(q.encoder),@"texture":@(q.texture),@"index":@(q.index),@"vertex":@(header.operation==OM_VERTEX_TEXTURE)}];break;
     }
     case OM_SCISSOR: {
      REQUEST(OMScissor);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));NSArray *extent=renderExtents[@(q.encoder)];
      if(!encoder || !extent || !q.width || !q.height || q.x>[extent[0] unsignedLongLongValue] || q.y>[extent[1] unsignedLongLongValue] ||
         q.width>[extent[0] unsignedLongLongValue]-q.x || q.height>[extent[1] unsignedLongLongValue]-q.y){result.error=-2;break;}
      [encoder setScissorRect:(MTLScissorRect){(NSUInteger)q.x,(NSUInteger)q.y,(NSUInteger)q.width,(NSUInteger)q.height}];break;
     }
     case OM_VIEWPORT: {
      REQUEST(OMViewport);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));
      if(!encoder || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.width) || !std::isfinite(q.height) ||
         !std::isfinite(q.nearDepth) || !std::isfinite(q.farDepth) || q.width<=0 || q.height<=0 || q.nearDepth<0 || q.farDepth>1 || q.nearDepth>q.farDepth){result.error=-2;break;}
      [encoder setViewport:(MTLViewport){q.x,q.y,q.width,q.height,q.nearDepth,q.farDepth}];break;
     }
     case OM_BLEND_COLOR: {
      REQUEST(OMBlendColor);id<MTLRenderCommandEncoder> encoder=getEncoder(q.encoder,@protocol(MTLRenderCommandEncoder));
      if(!encoder || !std::isfinite(q.red) || !std::isfinite(q.green) || !std::isfinite(q.blue) || !std::isfinite(q.alpha)){result.error=-2;break;}
      [encoder setBlendColorRed:q.red green:q.green blue:q.blue alpha:q.alpha];break;
     }
     case OM_RELEASE: {REQUEST(OMHandle);if(!objects[@(q.handle)]){result.error=-2;break;}[objects removeObjectForKey:@(q.handle)];[renderExtents removeObjectForKey:@(q.handle)];[fenceProducers removeObjectForKey:@(q.handle)];[eventProducers removeObjectForKey:@(q.handle)];[sharedStates removeObjectForKey:@(q.handle)];break;}
     case OM_FINISH:if(expectedSessions || !input.empty()){result.error=-2;break;}finishSubmitted(0,true,@"finish");finished=true;break;
     default:result.error=-4;break;
    }
   } @catch(NSException *exception) {result.error=-5;body=[exception.reason dataUsingEncoding:NSUTF8StringEncoding];}
#undef REQUEST
   if(error)body=[error.localizedDescription dataUsingEncoding:NSUTF8StringEncoding];
   if(body.length>OFFLINE_METAL_MAX_REPLY){result.error=-2;body=nil;}
   if(result.error) {
    NSString *reason=[[NSString alloc] initWithData:body?:[NSData data] encoding:NSUTF8StringEncoding];
    fprintf(stderr,"OFFLINE_METAL_ERROR request=%u operation=%u error=%d %s\n",request,header.operation,result.error,
            reason?[[reason substringToIndex:MIN(reason.length,(NSUInteger)512)] UTF8String]:"");fflush(stderr);
   }
   [operations addObject:@{@"request":@(request),@"client_request":@(header.request),@"session":@(session),@"operation":@(header.operation),@"input_bytes":@(input.size()),@"error":@(result.error),@"handle":@(result.handle),@"result_bytes":@(body.length)}];
   header.operation|=OM_WIRE_RESPONSE;header.bytes=(uint32_t)(sizeof(result)+body.length);
   healthy=exact(fd,&header,sizeof(header),true) && (!expectedSessions || exact(fd,&session,sizeof(session),true))
       && exact(fd,&result,sizeof(result),true) && (!body.length || exact(fd,(void *)body.bytes,body.length,true));
  }
 }
 close(fd);unlink(path);
 // End abandoned encoding before releasing its real objects. Never commit
 // interrupted buffers or invent a GPU completion to make teardown succeed.
 abortEncoders(0,true,healthy?@"unclosed-at-finish":@"transport-lost");
 finishSubmitted(0,true,healthy?@"finish":@"transport-lost");
 cancelNotifications(0,true);
 return @{@"host_device":device.name,@"operations":operations,@"completed_commands":completed,@"texture_readbacks":readbacks,@"texture_uploads":uploads,
     @"transport_healthy":@(healthy),@"session_finished":@(finished),@"enqueued_commands":enqueued,@"pending_enqueues":@(pendingEnqueues.count),
     @"aborted_encoders":abortedEncoders,@"open_encoders":@(openEncoders.count),
     @"shared_event_values":sharedValues,@"shared_event_labels":sharedLabels,@"shared_event_notifications":notificationJournal,@"shared_event_exports":exportJournal,@"live_shared_exports":@(sharedExports.count),
     @"events_created":eventsCreated,@"event_operations":eventOperations,
     @"submitted_commands":submitted,@"command_observations":commandObservations,@"failed_commands":failedCommands,@"pending_submissions":@(asyncCommands.count),
     @"function_specializations":specializations,@"fences_created":fencesCreated,@"fence_operations":fenceOperations,@"texture_hazard_modes":textureHazards,
     @"buffer_uploads":bufferWrites,@"buffer_readbacks":bufferReads,@"buffer_copies":bufferCopies,@"queue_priorities":queuePriorities,@"texture_copies":textureCopies,
     @"depth_stencil_states":depthStates,@"stencil_references":stencilReferences,@"attachment_passes":attachmentPasses,
     @"blend_pipelines":blendPipelines,@"texture_bindings":textureBindings,@"vertex_pipelines":vertexPipelines,
     @"boot_console_prefix_bytes":@(bootBytes),@"isolated_sessions":@(expectedSessions!=0),@"opened_sessions":@(openedSessions),@"closed_sessions":@(closedSessions),
     @"cpu_pixel_renderer":@NO,@"guest_system_device_discovery":@NO,
     @"adreno_execution":@NO,@"original_desktop":@NO};
}
