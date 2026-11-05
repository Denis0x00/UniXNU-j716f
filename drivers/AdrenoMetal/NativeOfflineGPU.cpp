// SPDX-License-Identifier: MIT
// Explicit offline GPU broker. No normal Metal/IOAccelerator publication.
// The kernel, not request bytes, selects each userclient's host namespace.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <sys/proc.h>
#include "NativeVirtioConsoleTransport.h"
#include "OfflineMetalSessionWire.h"
#include "OfflineMetalQueueWire.h"
#include "OfflineMetalTextureWire.h"
#include "OfflineMetalDepthStencilWire.h"
#include "OfflineMetalSharedEventWire.h"
#include <kern/task.h>
extern "C" {
volatile uint32_t native_gpu_broker_stage,native_gpu_broker_clients,native_gpu_broker_active,native_gpu_broker_closed;
volatile uint32_t native_gpu_broker_requests,native_gpu_broker_failures,native_gpu_broker_refusals;
volatile int native_gpu_broker_status=-1;
volatile uint64_t native_gpu_broker_reply_bytes;
volatile uint32_t native_gpu_broker_cpu_context_count;
volatile uint32_t native_gpu_shared_exports,native_gpu_shared_imports,native_gpu_shared_drops,native_gpu_shared_refusals,native_gpu_shared_tokens_live,native_gpu_shared_ports;
volatile uint64_t native_gpu_broker_cpu_contexts[8][8];
}
class NativeOfflineGPUClient;
class NativeGPUSharedEventToken;
class NativeOfflineGPU : public IOService {
    OSDeclareDefaultStructors(NativeOfflineGPU)
    friend class NativeOfflineGPUClient;
    friend class NativeGPUSharedEventToken;
    IOService *transport=nullptr;
    IOLock *gate=nullptr;
    bool online=false;
    uint32_t nextControlRequest=1;
    IOReturn readExact(void *bytes,size_t size) {
        auto p=static_cast<unsigned char *>(bytes);
        while(size) {
            size_t count=size>4096?4096:size;
            IOReturn status=native_console_receive(transport,this,p,&count);
            if(status!=kIOReturnSuccess)return status;
            if(!count || count>size)return kIOReturnIOError;
            p+=count;size-=count;
        }
        return kIOReturnSuccess;
    }
    // gate covers the complete actual request/reply, including every byte
    // beyond a VirtIO descriptor. Userclients retain separate reply buffers.
    IOReturn exchange(uint64_t session,const OMWireHeader &request,const void *input,
                      unsigned char **reply,size_t *length) {
        *reply=nullptr;*length=0;if(!online)return kIOReturnNotReady;
        OMSessionWireHeader header={request,session};header.wire.version=OFFLINE_METAL_SESSION_VERSION;
        IOReturn status=native_console_send(transport,this,&header,sizeof(header));
        if(status==kIOReturnSuccess && request.bytes)status=native_console_send(transport,this,input,request.bytes);
        OMSessionWireHeader response={};OMResult result={};
        if(status==kIOReturnSuccess)status=readExact(&response,sizeof(response));
        if(status==kIOReturnSuccess && (response.wire.magic!=OM_WIRE_MAGIC || response.wire.version!=OFFLINE_METAL_SESSION_VERSION ||
            response.wire.operation!=(request.operation|OM_WIRE_RESPONSE) || response.wire.request!=request.request ||
            response.session!=session || response.wire.bytes<sizeof(result) ||
            response.wire.bytes-sizeof(result)>OFFLINE_METAL_MAX_REPLY))status=kIOReturnIOError;
        if(status==kIOReturnSuccess)status=readExact(&result,sizeof(result));
        if(status==kIOReturnSuccess && result.reserved)status=kIOReturnIOError;
        unsigned char *buffer=nullptr;size_t size=sizeof(OMWireHeader)+response.wire.bytes;
        if(status==kIOReturnSuccess) {
            buffer=static_cast<unsigned char *>(IOMallocData(size));
            if(!buffer)status=kIOReturnNoMemory;
        }
        if(status==kIOReturnSuccess) {
            response.wire.version=OFFLINE_METAL_VERSION;
            memcpy(buffer,&response.wire,sizeof(OMWireHeader));memcpy(buffer+sizeof(OMWireHeader),&result,sizeof(result));
            status=readExact(buffer+sizeof(OMWireHeader)+sizeof(result),response.wire.bytes-sizeof(result));
        }
        native_gpu_broker_status=status;
        if(status!=kIOReturnSuccess) {
            if(buffer)IOFreeData(buffer,size);
            // A partial stream cannot be reused for another client's reply.
            online=false;native_gpu_broker_failures=native_gpu_broker_failures+1;return status;
        }
        native_gpu_broker_requests=native_gpu_broker_requests+1;
        native_gpu_broker_reply_bytes=native_gpu_broker_reply_bytes+size;
        if(result.error)native_gpu_broker_refusals=native_gpu_broker_refusals+1;
        *reply=buffer;*length=size;return kIOReturnSuccess;
    }
    IOReturn createSession(task_t owner,uint64_t *session) {
        *session=0;IOLockLock(gate);
        uint64_t tcr=0,ttbr0=0,ttbr1=0,sctlr=0;
        __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
        __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
        __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        bool native4k=((tcr>>14)&3)==0 && ((tcr>>30)&3)==2 && (sctlr&1) && ttbr0 && ttbr1;
        unsigned index=native_gpu_broker_cpu_context_count;
        native_gpu_broker_cpu_context_count=index+1;
        if(index<8) {
            uint64_t values[8]={(uint64_t)proc_selfpid(),0,tcr,ttbr0,ttbr1,sctlr,(uintptr_t)owner,(uint64_t)native4k};
            for(unsigned n=0;n<8;n++)native_gpu_broker_cpu_contexts[index][n]=values[n];
        }
        if(!native4k) {
            native_gpu_broker_status=kIOReturnUnsupported;native_gpu_broker_failures=native_gpu_broker_failures+1;
            IOLockUnlock(gate);return kIOReturnUnsupported;
        }
        OMWireHeader request={OM_WIRE_MAGIC,OFFLINE_METAL_VERSION,OM_SESSION_OPEN,0,1};
        unsigned char *reply=nullptr;size_t length=0;IOReturn status=exchange(0,request,nullptr,&reply,&length);
        if(status==kIOReturnSuccess) {
            OMResult result={};memcpy(&result,reply+sizeof(OMWireHeader),sizeof(result));
            if(result.error || !result.handle || length!=sizeof(OMWireHeader)+sizeof(result))status=kIOReturnNotOpen;
            else {
                *session=result.handle;native_gpu_broker_clients=native_gpu_broker_clients+1;native_gpu_broker_active=native_gpu_broker_active+1;
                if(index<8)native_gpu_broker_cpu_contexts[index][1]=result.handle;
            }
        }
        if(reply)IOFreeData(reply,length);IOLockUnlock(gate);return status;
    }
    void closeSession(uint64_t session,uint32_t number) {
        IOLockLock(gate);OMWireHeader request={OM_WIRE_MAGIC,OFFLINE_METAL_VERSION,OM_SESSION_CLOSE,0,number};
        unsigned char *reply=nullptr;size_t length=0;IOReturn status=exchange(session,request,nullptr,&reply,&length);
        if(status==kIOReturnSuccess) {
            OMResult result={};memcpy(&result,reply+sizeof(OMWireHeader),sizeof(result));
            if(!result.error){native_gpu_broker_closed=native_gpu_broker_closed+1;native_gpu_broker_active=native_gpu_broker_active-1;}
        }
        if(reply)IOFreeData(reply,length);IOLockUnlock(gate);
    }
    void dropSharedExport(uint64_t exported) {
        if(!exported)return;IOLockLock(gate);
        OMWireHeader header={OM_WIRE_MAGIC,OFFLINE_METAL_VERSION,OM_SHARED_EXPORT_DROP,sizeof(OMHandle),nextControlRequest};OMHandle input={exported};
        unsigned char *reply=nullptr;size_t length=0;IOReturn status=exchange(0,header,&input,&reply,&length);
        if(status==kIOReturnSuccess){nextControlRequest++;OMResult result={};memcpy(&result,reply+sizeof(OMWireHeader),sizeof(result));if(!result.error)__atomic_fetch_add(&native_gpu_shared_drops,1,__ATOMIC_RELAXED);}
        if(reply)IOFreeData(reply,length);IOLockUnlock(gate);
    }
public:
    bool start(IOService *provider) override {
        if(!IOService::start(provider))return false;
        gate=IOLockAlloc();if(!gate){IOService::stop(provider);return false;}
        if(!provider->open(this)){IOLockFree(gate);gate=nullptr;IOService::stop(provider);return false;}
        transport=provider;online=true;native_gpu_broker_stage=2;return true;
    }
    void stop(IOService *provider) override {
        if(gate){IOLockLock(gate);online=false;if(transport)transport->close(this);transport=nullptr;IOLockUnlock(gate);}
        IOService::stop(provider);
    }
    void free() override {
        if(transport){transport->close(this);transport=nullptr;}
        if(gate){IOLockFree(gate);gate=nullptr;}IOService::free();
    }
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
};
// A real kernel OSObject carried exclusively by an IKOT_IOKIT_IDENT Mach
// right. No user-supplied integer can stand in for this object's authority.
class NativeGPUSharedEventToken : public OSObject {
    OSDeclareDefaultStructors(NativeGPUSharedEventToken)
    friend class NativeOfflineGPUClient;
    NativeOfflineGPU *controller=nullptr;
    uint64_t exported=0;
    bool initWithExport(NativeOfflineGPU *owner,uint64_t identifier) {
        if(!owner || !identifier || !OSObject::init())return false;
        controller=owner;controller->retain();exported=identifier;
        __atomic_fetch_add(&native_gpu_shared_tokens_live,1,__ATOMIC_RELAXED);return true;
    }
    void free() override {
        if(controller){uint64_t identifier=exported;exported=0;controller->dropSharedExport(identifier);controller->release();controller=nullptr;__atomic_fetch_sub(&native_gpu_shared_tokens_live,1,__ATOMIC_RELAXED);}
        OSObject::free();
    }
};
class NativeOfflineGPUClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeOfflineGPUClient)
    NativeOfflineGPU *controller=nullptr;
    task_t owner=nullptr; // The native IOUserClient base owns this task reference.
    IOLock *gate=nullptr;
    uint64_t session=0;
    uint32_t nextRequest=1,nextHostRequest=1;
    unsigned char *reply=nullptr;size_t replyLength=0,replyOffset=0;
    void clearReply() {if(reply)IOFreeData(reply,replyLength);reply=nullptr;replyLength=replyOffset=0;}
    void closeContext() {
        if(gate)IOLockLock(gate);
        clearReply();if(session && controller)controller->closeSession(session,nextHostRequest);
        session=0;controller=nullptr;if(gate)IOLockUnlock(gate);
    }
    static IOReturn sendAction(OSObject *target,void *,IOExternalMethodArguments *args) {
        auto self=OSDynamicCast(NativeOfflineGPUClient,target);if(!self || !self->gate)return kIOReturnNotOpen;
        if(args->structureInputDescriptor)return kIOReturnUnsupported;
        if(!args->structureInput || args->structureInputSize<sizeof(OMWireHeader) || args->structureInputSize>4096)return kIOReturnBadArgument;
        OMWireHeader header={};memcpy(&header,args->structureInput,sizeof(header));
        IOLockLock(self->gate);IOReturn status=kIOReturnNotOpen;
        if(self->session && self->controller) {
            if(self->replyOffset!=self->replyLength)status=kIOReturnBusy;
            else if(header.magic!=OM_WIRE_MAGIC || header.version!=OFFLINE_METAL_VERSION || header.request!=self->nextRequest ||
                header.bytes+sizeof(header)!=args->structureInputSize || header.bytes>4080 ||
                header.operation<OM_DEVICE || header.operation>OM_SHARED_LABEL_SET || (header.operation==OM_FINISH && header.bytes))status=kIOReturnBadArgument;
            else {
                self->clearReply();uint16_t operation=header.operation;if(operation==OM_FINISH)header.operation=OM_SESSION_CLOSE;
                IOLockLock(self->controller->gate);
                OMWireHeader hostHeader=header;hostHeader.request=self->nextHostRequest;
                status=self->controller->exchange(self->session,hostHeader,static_cast<const unsigned char *>(args->structureInput)+sizeof(header),&self->reply,&self->replyLength);
                if(status==kIOReturnSuccess) {
                    self->nextRequest++;self->nextHostRequest++;
                    // Preserve the real namespace-close result in the client's
                    // own framing; no host-wide Finish is forwarded.
                    header.operation=operation|OM_WIRE_RESPONSE;header.bytes=(uint32_t)(self->replyLength-sizeof(header));
                    memcpy(self->reply,&header,sizeof(header));OMResult result={};memcpy(&result,self->reply+sizeof(header),sizeof(result));
                    if(operation==OM_FINISH && !result.error) {
                        self->session=0;native_gpu_broker_closed=native_gpu_broker_closed+1;native_gpu_broker_active=native_gpu_broker_active-1;
                    }
                }
                IOLockUnlock(self->controller->gate);
            }
        }
        IOLockUnlock(self->gate);return status;
    }
    static IOReturn receiveAction(OSObject *target,void *,IOExternalMethodArguments *args) {
        auto self=OSDynamicCast(NativeOfflineGPUClient,target);if(!self || !self->gate)return kIOReturnNotOpen;
        if(args->structureOutputDescriptor)return kIOReturnUnsupported;
        if(!args->structureOutput || !args->structureOutputSize || args->structureOutputSize>4096)return kIOReturnBadArgument;
        IOLockLock(self->gate);size_t available=self->replyLength-self->replyOffset;
        size_t count=args->structureOutputSize;if(count>available)count=available;
        if(count){memcpy(args->structureOutput,self->reply+self->replyOffset,count);self->replyOffset+=count;}
        args->structureOutputSize=(uint32_t)count;IOLockUnlock(self->gate);return count?kIOReturnSuccess:kIOReturnNotReady;
    }
    IOReturn sharedExchange(uint16_t operation,uint64_t identifier,unsigned char **output,size_t *length) {
        *output=nullptr;*length=0;
        if(!session || !controller)return kIOReturnNotOpen;
        if(replyOffset!=replyLength)return kIOReturnBusy;
        OMWireHeader header={OM_WIRE_MAGIC,OFFLINE_METAL_VERSION,operation,sizeof(OMHandle),nextHostRequest};OMHandle input={identifier};
        IOLockLock(controller->gate);IOReturn status=controller->exchange(session,header,&input,output,length);
        if(status==kIOReturnSuccess)nextHostRequest++;IOLockUnlock(controller->gate);return status;
    }
    static IOReturn exportSharedAction(OSObject *target,void *,IOExternalMethodArguments *args) {
        auto self=OSDynamicCast(NativeOfflineGPUClient,target);if(!self || !self->gate)return kIOReturnNotOpen;
        if(current_task()!=self->owner || args->structureOutputDescriptor || !args->structureOutput || !args->structureOutputSize || args->structureOutputSize>4096)return kIOReturnBadArgument;
        args->scalarOutput[0]=0;IOLockLock(self->gate);unsigned char *bytes=nullptr;size_t length=0;
        IOReturn status=self->sharedExchange(OM_SHARED_EXPORT,args->scalarInput[0],&bytes,&length);OMResult result={};
        if(status==kIOReturnSuccess) {
            memcpy(&result,bytes+sizeof(OMWireHeader),sizeof(result));size_t prefix=sizeof(OMWireHeader)+sizeof(result);
            if(result.error || !result.handle)status=kIOReturnBadArgument;
            else {
                __atomic_fetch_add(&native_gpu_shared_exports,1,__ATOMIC_RELAXED);
                size_t count=length-prefix;
                if(!count || count>args->structureOutputSize){self->controller->dropSharedExport(result.handle);status=kIOReturnNoSpace;}
                else {
                    auto token=new NativeGPUSharedEventToken;
                    if(!token || !token->initWithExport(self->controller,result.handle)) {
                        OSSafeReleaseNULL(token);self->controller->dropSharedExport(result.handle);status=kIOReturnNoMemory;
                    } else {
                        // Kernel MACH_PORT_NULL is an ipc_port pointer, while
                        // this API returns a name in the owner's namespace.
                        mach_port_name_t name=0;status=IOUserClient::copyPortNameForObjectInTask(self->owner,token,&name);
                        if(status==kIOReturnSuccess && name && name!=MACH_PORT_DEAD) {
                            args->scalarOutput[0]=name;memcpy(args->structureOutput,bytes+prefix,count);args->structureOutputSize=(uint32_t)count;
                            __atomic_fetch_add(&native_gpu_shared_ports,1,__ATOMIC_RELAXED);
                        } else if(status==kIOReturnSuccess)status=kIOReturnNoResources;
                        token->release();
                    }
                }
            }
        }
        if(bytes)IOFreeData(bytes,length);IOLockUnlock(self->gate);
        if(status!=kIOReturnSuccess)__atomic_fetch_add(&native_gpu_shared_refusals,1,__ATOMIC_RELAXED);return status;
    }
    static IOReturn importSharedAction(OSObject *target,void *,IOExternalMethodArguments *args) {
        auto self=OSDynamicCast(NativeOfflineGPUClient,target);if(!self || !self->gate)return kIOReturnNotOpen;
        if(current_task()!=self->owner || args->scalarInput[0]>UINT32_MAX || args->structureOutputDescriptor || !args->structureOutput || !args->structureOutputSize || args->structureOutputSize>4096)return kIOReturnBadArgument;
        args->scalarOutput[0]=0;OSObject *object=nullptr;IOReturn status=IOUserClient::copyObjectForPortNameInTask(self->owner,(mach_port_name_t)args->scalarInput[0],&object);
        auto token=OSDynamicCast(NativeGPUSharedEventToken,object);unsigned char *bytes=nullptr;size_t length=0;IOLockLock(self->gate);
        if(status!=kIOReturnSuccess || !token || token->controller!=self->controller || !token->exported)status=kIOReturnBadArgument;
        else {
            status=self->sharedExchange(OM_SHARED_IMPORT,token->exported,&bytes,&length);
            if(status==kIOReturnSuccess) {
                OMResult result={};memcpy(&result,bytes+sizeof(OMWireHeader),sizeof(result));size_t prefix=sizeof(OMWireHeader)+sizeof(result),count=length-prefix;
                if(result.error || !result.handle)status=kIOReturnBadArgument;
                else if(!count || count>args->structureOutputSize) {
                    // Retire the just-created independent event handle through
                    // the same real session without consuming a user sequence.
                    unsigned char *released=nullptr;size_t releasedLength=0;
                    (void)self->sharedExchange(OM_RELEASE,result.handle,&released,&releasedLength);if(released)IOFreeData(released,releasedLength);status=kIOReturnNoSpace;
                } else {args->scalarOutput[0]=result.handle;memcpy(args->structureOutput,bytes+prefix,count);args->structureOutputSize=(uint32_t)count;__atomic_fetch_add(&native_gpu_shared_imports,1,__ATOMIC_RELAXED);}
            }
        }
        if(bytes)IOFreeData(bytes,length);OSSafeReleaseNULL(object);IOLockUnlock(self->gate);
        if(status!=kIOReturnSuccess)__atomic_fetch_add(&native_gpu_shared_refusals,1,__ATOMIC_RELAXED);return status;
    }
public:
    bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties) override {
        if(type || clientHasPrivilege(security,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess ||
            !IOUserClient::initWithTask(task,security,type,properties))return false;
        owner=task;gate=IOLockAlloc();return gate!=nullptr;
    }
    bool start(IOService *provider) override {
        controller=OSDynamicCast(NativeOfflineGPU,provider);
        if(!controller || !IOUserClient::start(provider))return false;
        if(controller->createSession(owner,&session)!=kIOReturnSuccess){controller=nullptr;IOUserClient::stop(provider);return false;}
        return true;
    }
    void stop(IOService *provider) override {closeContext();IOUserClient::stop(provider);}
    void free() override {closeContext();if(gate){IOLockFree(gate);gate=nullptr;}IOUserClient::free();}
    IOReturn clientClose() override {terminate();return kIOReturnSuccess;}
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,IOExternalMethodDispatch *,OSObject *,void *) override {
        static const IOExternalMethodDispatch methods[]={{sendAction,0,kIOUCVariableStructureSize,0,0},{receiveAction,0,0,0,kIOUCVariableStructureSize},
            {exportSharedAction,1,0,1,kIOUCVariableStructureSize},{importSharedAction,1,0,1,kIOUCVariableStructureSize}};
        if(selector>=4)return kIOReturnUnsupported;
        return IOUserClient::externalMethod(selector,args,const_cast<IOExternalMethodDispatch *>(methods+selector),this,nullptr);
    }
};
OSDefineMetaClassAndStructors(NativeOfflineGPU,IOService)
OSDefineMetaClassAndStructors(NativeOfflineGPUClient,IOUserClient)
OSDefineMetaClassAndStructors(NativeGPUSharedEventToken,OSObject)
IOReturn NativeOfflineGPU::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out) {
    if(!out)return kIOReturnBadArgument;*out=nullptr;
    auto client=new NativeOfflineGPUClient;if(!client)return kIOReturnNoMemory;
    bool attached=false;
    if(!client->initWithTask(task,security,type,properties) || !(attached=client->attach(this)) || !client->start(this)) {
        if(attached)client->detach(this);client->release();return kIOReturnNotOpen;
    }
    *out=client;return kIOReturnSuccess;
}
extern "C" IOService *native_offline_gpu_create(void) {return new NativeOfflineGPU;}
