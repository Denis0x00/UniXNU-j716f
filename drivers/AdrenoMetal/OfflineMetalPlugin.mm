// SPDX-License-Identifier: MIT
// Explicit offline bundle factory. No normal discovery publication or install.
#include "OfflineMetalFrontend.mm"
@interface _MTLDevice : NSObject
@end
@interface NativeOfflineMetalPlugin : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
@implementation NativeOfflineMetalPlugin
- (id)initWithAcceleratorPort:(unsigned)port {
 self=[super init];if(!self)return nil;
 if(!port)return nil;
 void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);if(!iokit)return nil;
 auto conforms=(int (*)(unsigned,const char *))dlsym(iokit,"IOObjectConformsTo");
 auto registry=(int (*)(unsigned,uint64_t *))dlsym(iokit,"IORegistryEntryGetRegistryEntryID");
 auto matching=(void *(*)(const char *))dlsym(iokit,"IOServiceMatching");
 auto services=(int (*)(unsigned,void *,unsigned *))dlsym(iokit,"IOServiceGetMatchingServices");
 auto next=(unsigned (*)(unsigned))dlsym(iokit,"IOIteratorNext");auto release=(int (*)(unsigned))dlsym(iokit,"IOObjectRelease");
 if(!conforms || !registry || !matching || !services || !next || !release || !conforms(port,"NativeOfflineGPU"))return nil;
 uint64_t provided=0,expected=0;unsigned iterator=0;
 if(registry(port,&provided) || !provided || services(0,matching("NativeOfflineGPU"),&iterator) || !iterator)return nil;
 unsigned service=next(iterator),extra=next(iterator);
 bool valid=service && !extra && !registry(service,&expected) && expected==provided;
 if(extra)release(extra);if(service)release(service);release(iterator);if(!valid)return nil;
 // The explicit frontend must select precisely the provided real service.
 // No original method, class or registry result is replaced.
 id device=offline_metal_frontend_open_service("NativeOfflineGPU");if(!device)return nil;
 NSMutableDictionary *metadata=[deviceInfo mutableCopy];metadata[@"backendRegistryID"]=metadata[@"registryID"];
 metadata[@"registryID"]=@(provided);deviceInfo=metadata;
 self=device;return self;
}
@end
