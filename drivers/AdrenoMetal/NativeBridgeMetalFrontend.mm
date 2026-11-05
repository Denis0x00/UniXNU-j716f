// SPDX-License-Identifier: MIT
// Native Metal API frontend over the bridge (bridge/NativeMetalObjects.h): the metal2vulkan runtime on Turnip, on the Adreno 740 through
// NativeAdreno. Same shape as the offline frontend: the device is a dynamically allocated subclass of the target's own _MTLDevice that
// only adds this file's implementations; every other object is an own class answering the Metal protocol methods it implements.
// Every result comes from the runtime and the GPU: a refused call returns nil/NO with the runtime's reason, an encoder call the runtime
// refuses fails its command buffer (status Error, handlers run) instead of being dropped. A selector this frontend does not implement is
// reported through the missing-selector hook before the usual exception, so a hardware run names the gap.
// Shared events work within this process (no cross-process handle yet). Not yet implemented here: heaps, argument buffers, indirect commands, drawables/presentation.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#include <algorithm>
#include <dlfcn.h>
#include <string.h>
#include <atomic>
#include <map>
#include <mutex>
#include <pthread.h>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"

extern "C" void sys_dcache_flush(void *start, size_t length);

static NmbDevice *gDevice;
static id gDeviceObject;
static uint64_t gRegistryID;
static NSString *gName;
static void (*gMissing)(const char *, const char *);

// What this device has been asked and refused, for the plugin's status line (native_bridge_metal_status): commits, render/compute pipelines made,
// refusals, and the last two refusals' texts. Counters only; nothing here changes a result.
static std::atomic<uint64_t> gCommits,gCommitFailures,gPipelines,gProblems,gCompleted;
// What the render server actually asks of the GPU (int-141: 59 commits in 200 s, and a desktop that behaves like CPU compositing): render passes,
// draws, the pixels of their first colour attachment, compute dispatches, blits and the largest pass (pixels).
static std::atomic<uint64_t> gPasses,gDraws,gPassPixels,gDispatches,gBlits,gBiggestPass,gFullScreenPasses;
// The shapes of the render passes the render server opens (width, height, colour format, sample count), each counted once: the corner
// question (2026-10-24: rounded corners drawn in 4 x 4 pixel blocks in GMEM passes) needs to know whether the corners are drawn into the
// full-screen pass or into a small offscreen one, and whether that pass is multisampled.
struct NmbPassShape {uint32_t width,height,format,samples;std::atomic<uint64_t> count;};
static NmbPassShape gPassShapes[6];
static std::atomic<unsigned> gPassShapeCount;
// Where the time goes on the CPU side of the bridge: pipeline, library and function creation (count, total and slowest, in microseconds), and the
// last commits with what each held and how long the GPU took (commit to completion).
static uint64_t nowUs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
struct Timed {std::atomic<uint64_t> count{0},total{0},slowest{0};
 void add(uint64_t began){const uint64_t took=nowUs()-began;count++;total+=took;uint64_t seen=slowest.load();while(took>seen && !slowest.compare_exchange_weak(seen,took)){}}};
static Timed gTimePipeline,gTimeLibrary,gTimeFunction,gTimeEncodeToCommit;
static Timed gTimeWaitScheduled,gTimeWaitCompleted;
// What each kind of call the render server makes costs on the CPU: the draws, the binds that set them up, the pass's open and close and the
// commit (2026-10-25: a frame's draws come to about 150 us each, which nothing in the frontend does per draw, so the frame's CPU has to be
// split by class before anything is changed).
static Timed gTimeDraw,gTimeBind,gTimePassOpen,gTimePassClose,gTimeCommit;
// The draws by what they cost: the first of each render encoder (the pass's framebuffer, variant and pools are made then), and the later ones
// by whether the pipeline state changed since the draw before (A3). A draw is a few microseconds of frontend and one call into the runtime, so
// what the runtime and Turnip spend inside it is what these split apart.
static Timed gTimeDrawFirst,gTimeDrawNewState,gTimeDrawSameState;
// The encoder's state calls, by class (the A7 and A8 rows): the pipeline state, viewport, scissor, depth/stencil state, the raster states
// (cull mode, winding, fill mode), blend colour, and the rest (depth bias, line width, stencil references, visibility result). A frame's
// draws take about 3.6 ms and its passes about 1.4 ms, so a frame's CPU above those is here, in CA's own work, or in calls not yet counted.
enum {kStatePipeline,kStateViewport,kStateScissor,kStateDepthStencil,kStateRaster,kStateBlend,kStateOther,kStateCount};
static Timed gTimeState[kStateCount];
// What creating a resource costs on this thread (a texture, a buffer, a surface's import): the encoder's own calls are the A7 and A8 rows.
static Timed gTimeCreate;
// The sets that left the encoder's state as it was, by class: what the deduplication above saved (the A7 and A8 rows report both).
static std::atomic<uint64_t> gStateSkipped[kStateCount];
// The blit encoder's calls ('A0' row with the draws and binds): the scanout copy is one blit a frame of a 1800x2880 texture, and a
// texture readback would be the same kind of call.
static Timed gTimeBlit;
// Who waits for a command buffer to complete (tb128fu-047: about 41 ms once per swap, so the next frame never overlapped the GPU;
// tb128fu-049: every frame above the wait outside this plugin was the thread's start): the four return addresses above
// waitUntilCompleted, each as one letter for its image (P this plugin, Q QuartzCore, M Metal, L libdispatch, T libsystem_pthread,
// ? another) and its offset from that image's header, counted per distinct chain (rows C0..C3; P offsets resolve with the plugin's dSYM).
struct WaitSite {char image[4];uintptr_t offset[4];uint64_t count;};
static WaitSite gWaitSites[4];
static std::mutex gWaitSiteLock;
static char imageLetter(const char *path) {
 if(strstr(path,"NativeAdrenoMetal"))return 'P';
 if(strstr(path,"/QuartzCore"))return 'Q';
 if(strstr(path,"/Metal.framework"))return 'M';
 if(strstr(path,"libdispatch"))return 'L';
 if(strstr(path,"libsystem_pthread"))return 'T';
 return '?';
}
static void noteWaitSite() {
 void *frames[8];const int n=backtrace(frames,8);
 WaitSite seen{};
 for(int i=2;i<n && i<6;i++) {   // 0 is this function, 1 waitUntilCompleted
  Dl_info info{};
  if(!dladdr(frames[i],&info) || !info.dli_fname)continue;
  seen.image[i-2]=imageLetter(info.dli_fname);seen.offset[i-2]=(uintptr_t)frames[i]-(uintptr_t)info.dli_fbase;
 }
 std::lock_guard<std::mutex> lock(gWaitSiteLock);
 for(auto &site:gWaitSites) {
  if(site.count && !memcmp(site.offset,seen.offset,sizeof seen.offset)) {++site.count;return;}
  if(!site.count) {site=seen;site.count=1;return;}
 }
}
static Timed gTimeResourceWait;
static std::atomic<uint64_t> gResourceChecks,gResourceSkipped;
static Timed gTimeSurfaceFlush;
static std::atomic<uint64_t> gSurfaceFlushBytes,gSurfaceFlushDuplicates;
// What a command buffer held, and the texture and buffer work that led to it (a replaceRegion, a texture or buffer made, a surface
// imported, a buffer-to-texture copy): counted on the thread that encodes and commits, read by the H rows at its completion.
struct EncodedWork {uint32_t passes,draws,dispatches,blits;uint64_t pixels;uint32_t tex,buf;};
// Where the compositor's frames are prepared: at each commit with a render pass, the committing thread's CPU time (user and system) and
// the wall time since its previous such commit, and the core it committed from (TPIDR_EL0's CPU number; on SM6225 cores 0-3 are the
// A53s, 4-7 the A73s). A gap past 200 ms is the compositor at rest and is not counted.
static std::atomic<uint64_t> gFrameCostN,gFrameCpuUs,gFrameWallUs,gFrameCores[8];
static void startProfile();
// The render thread's long intervals between two frames (40 ms to 2 s) and the CPU it spent in them: whether a stutter at the start or end
// of an animation (60 to 130 ms, 2026-10-25, while the frames around it took 16) is the render server busy (its CPU near the interval) or
// left without an update to draw (near none). The last 8, the L rows.
struct LongFrame {uint64_t at,gapUs,cpuUs;};
static LongFrame gLongFrames[8];
static std::atomic<uint32_t> gLongFrameCount;
static std::mutex gLongFrameLock;
static void noteFrameCost() {
 static thread_local uint64_t lastCpu,lastWall;
 static std::atomic<uint32_t> renderCommits;
 if(++renderCommits==200)startProfile();   // past the first frames: the thread committing now is the compositor's render thread
 uint64_t tpidr;__asm__ volatile("mrs %0, TPIDR_EL0":"=r"(tpidr));
 const unsigned core=(unsigned)(tpidr&0xfff);
 thread_basic_info_data_t t{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;mach_port_t self=mach_thread_self();
 const bool ok=thread_info(self,THREAD_BASIC_INFO,(thread_info_t)&t,&n)==KERN_SUCCESS;
 mach_port_deallocate(mach_task_self(),self);
 if(!ok)return;
 const uint64_t cpu=(uint64_t)(t.user_time.seconds+t.system_time.seconds)*1000000+t.user_time.microseconds+t.system_time.microseconds;
 const uint64_t wall=nowUs();
 if(core<8)gFrameCores[core]++;
 if(lastWall && wall-lastWall<200000){gFrameCostN++;gFrameCpuUs+=cpu-lastCpu;gFrameWallUs+=wall-lastWall;}
 if(lastWall && wall-lastWall>=40000 && wall-lastWall<2000000) {
  std::lock_guard<std::mutex> lock(gLongFrameLock);
  gLongFrames[gLongFrameCount.fetch_add(1)%8]={wall,wall-lastWall,cpu-lastCpu};
 }
 lastCpu=cpu;lastWall=wall;
}
struct CommitRecord {uint64_t committedUs=0,callbackUs=0,ageUs=0,gpuUs=0;int64_t scheduledUs=-1;uint32_t index=0;EncodedWork work={};};
// The GPU's own time on command buffers (the runtime's gpuStartTime/gpuEndTime: its timestamp queries), all of them and those with a render
// pass, against the commit-to-completion time that also holds the queue's wait (round 067: 80-130 ms per compositor command buffer).
static std::atomic<uint64_t> gGpuTimed,gGpuUs,gGpuPassUs,gGpuUntimed;
// The GPU time of the render server's two kinds of command buffer: those with a render pass, and the scanout copy's (a blit alone, one per
// frame: 1894 of 3784 commits in the 45 s round of 2026-10-25).
static std::atomic<uint64_t> gGpuPassN,gGpuCopyN,gGpuCopyUs;
static CommitRecord gCommitRing[32];
// Where the time between two render commits goes, by how long that interval was: [0] under 11 ms, [1] to 16.7, [2] to 33, [3] to 200 ms
// (longer ones are idle). pg = the earlier render buffer's commit-to-callback, enc = this buffer's creation-to-commit, res = what is left
// (negative when the two overlapped), kpx = the earlier one's thousands of pixels, hv = how many earlier ones were over 30 Mpx.
// ---- frame trace: one record per command buffer and per wait, appended to a file by a writer thread ----
// The file is read by the kernel's diagnostic export (BootDiagExport.c lists it), so a hang or a reset still leaves everything up to then.
// 72-byte records: type 1 command buffer {idx, t0 created, t1 committed, t2 scheduled (0: unknown), t3 completion callback, a passes, b draws,
// c thousands of pixels, d blits, tid creator thread, cpu0/cpu1 its cumulative CPU microseconds at creation and at commit, tid2 committing
// thread}; type 2 waitUntilCompleted {t0 begin, t1 end, tid, a caller (low 32 bits), cpu0 the thread's CPU time at the start, cpu1 at the end};
// type 3 the same for waitUntilScheduled; type 4 a thread's name {tid, 24 characters in t0..t2}. Times are nowUs().
struct TraceRec {uint32_t type,idx;uint64_t t0,t1,t2,t3;uint32_t a,b,c,d,tid,cpu0,cpu1,tid2;};
static_assert(sizeof(TraceRec)==72,"trace record");
static TraceRec gTrace[16384];
static std::atomic<uint32_t> gTraceHead{0};
static std::atomic<bool> gTraceStarted{false};
static uint32_t threadCpuUs() {
 thread_basic_info_data_t info;mach_msg_type_number_t count=THREAD_BASIC_INFO_COUNT;
 if(thread_info(pthread_mach_thread_np(pthread_self()),THREAD_BASIC_INFO,(thread_info_t)&info,&count)!=KERN_SUCCESS)return 0;
 return (uint32_t)((uint64_t)(info.user_time.seconds+info.system_time.seconds)*1000000ull+(uint64_t)(info.user_time.microseconds+info.system_time.microseconds));
}
static uint32_t threadIdLow() {uint64_t id=0;pthread_threadid_np(NULL,&id);return (uint32_t)id;}
static void tracePush(const TraceRec &rec) {
 const uint32_t at=gTraceHead.fetch_add(1);
 gTrace[at%16384]=rec;
 gTrace[at%16384].idx=rec.type==1?rec.idx:at;
}
static void traceStartWriter();
// type 5 swap served by the scanout client: {idx, t0 offer arrived, t1 GPU copy returned, t2 swap completed, a sequence, b result}
extern "C" void NBTraceSwap(uint64_t offered,uint64_t copied,uint64_t completed,uint32_t sequence,uint32_t result) {
 TraceRec rec={};rec.type=5;rec.t0=offered;rec.t1=copied;rec.t2=completed;rec.a=sequence;rec.b=result;tracePush(rec);
 traceStartWriter();
}
static void traceNote(uint32_t tid) {
 static std::atomic<uint32_t> seen[64];
 for(unsigned i=0;i<64;i++){uint32_t v=seen[i].load();if(v==tid)return;if(!v){uint32_t zero=0;if(seen[i].compare_exchange_strong(zero,tid))break;if(seen[i].load()==tid)return;}}
 TraceRec rec={};rec.type=4;rec.tid=tid;pthread_getname_np(pthread_self(),(char *)&rec.t0,24);tracePush(rec);
}

// ---- sampler: where the render thread's CPU goes between a command buffer's creation and its commit ----
// A thread suspends the render thread every 500 us while it is inside that window and keeps the program counter plus the frame-pointer chain in a
// ring (no allocation, no locks: the suspended thread may hold any). The writer thread resolves the addresses with dladdr and rewrites cpu.profile
// every few seconds: L = samples whose leaf is that symbol, I = samples with that symbol anywhere in the chain.
static std::atomic<mach_port_t> gSampleThread{0};
static std::atomic<uint64_t> gSampleTop{0};
struct StackSample {uint64_t frames[8];};
static StackSample gSamples[8192];
static std::atomic<uint32_t> gSampleHead{0};
static std::atomic<bool> gSamplerStarted{false};
static void sampleWindowOpen() {
#ifndef NB_SAMPLER
 return;   // the sampler suspends the render thread 2000 times a second: off unless asked for
#endif
 char name[32]={};pthread_getname_np(pthread_self(),name,sizeof name);
 if(strncmp(name,"com.apple.coreanimation",23))return;
 gSampleTop.store((uint64_t)pthread_get_stackaddr_np(pthread_self()));
 gSampleThread.store(pthread_mach_thread_np(pthread_self()));
 if(!gSamplerStarted.exchange(true))std::thread([]{
  pthread_setname_np("frame-sampler");
  for(;;) {
   usleep(500);
   const mach_port_t port=gSampleThread.load();
   if(!port)continue;
   if(thread_suspend(port)!=KERN_SUCCESS)continue;
   arm_thread_state64_t state;mach_msg_type_number_t count=ARM_THREAD_STATE64_COUNT;
   StackSample sample={};
   if(thread_get_state(port,ARM_THREAD_STATE64,(thread_state_t)&state,&count)==KERN_SUCCESS) {
    const uint64_t mask=0x0000007fffffffffull,top=gSampleTop.load();
    const uint64_t sp=arm_thread_state64_get_sp(state);
    unsigned n=0;
    sample.frames[n++]=arm_thread_state64_get_pc(state)&mask;
    sample.frames[n++]=arm_thread_state64_get_lr(state)&mask;
    uint64_t fp=arm_thread_state64_get_fp(state)&mask;
    while(n<8 && fp>=sp && fp+16<=top && !(fp&15)) {
     const uint64_t *record=(const uint64_t *)fp;
     sample.frames[n++]=record[1]&mask;
     const uint64_t next=record[0]&mask;
     if(next<=fp)break;
     fp=next;
    }
   }
   thread_resume(port);
   if(sample.frames[0])gSamples[gSampleHead.fetch_add(1)%8192]=sample;
  }
 }).detach();
}
static void sampleWindowClose() {
 if(gSampleThread.load()==pthread_mach_thread_np(pthread_self()))gSampleThread.store(0);
}
static void sampleWriteProfile(uint32_t &consumed,int &fd,const char *const *paths) {
 static std::unordered_map<uint64_t,std::string> names;
 static std::unordered_map<std::string,uint32_t> leaf,inclusive;
 struct AddressCount {uint32_t leaf=0,inclusive=0;};
 static std::unordered_map<uint64_t,AddressCount> addresses;
 static uint32_t total=0,rounds=0;
 const uint32_t head=gSampleHead.load();
 if(head-consumed>8000)consumed=head-8000;
 auto resolve=[&](uint64_t address)->const std::string & {
  auto it=names.find(address);if(it!=names.end())return it->second;
  Dl_info info={};std::string name;
  if(dladdr((const void *)address,&info)){
   const char *image=info.dli_fname?strrchr(info.dli_fname,'/'):NULL;image=image?image+1:(info.dli_fname?info.dli_fname:"?");
   char text[256];
   if(info.dli_sname)snprintf(text,sizeof text,"%s`%s+%llu",image,info.dli_sname,(unsigned long long)(address-(uint64_t)info.dli_saddr));
   else snprintf(text,sizeof text,"%s`+0x%llx",image,(unsigned long long)(address-(uint64_t)info.dli_fbase));
   name=text;
  } else name="?";
  return names[address]=name;
 };
 while(consumed<head) {
  const StackSample &sample=gSamples[consumed%8192];++consumed;++total;
  leaf[resolve(sample.frames[0])]++;
  addresses[sample.frames[0]].leaf++;
  for(unsigned i=0;i<8 && sample.frames[i];i++){bool dup=false;for(unsigned j=0;j<i;j++)if(sample.frames[j]==sample.frames[i])dup=true;if(!dup)addresses[sample.frames[i]].inclusive++;}
  std::string seen[8];unsigned count=0;
  for(unsigned i=0;i<8 && sample.frames[i];i++) {
   std::string name=resolve(sample.frames[i]);
   const size_t plus=name.rfind('+');if(plus!=std::string::npos && i>0)name.resize(plus);
   bool dup=false;for(unsigned j=0;j<count;j++)if(seen[j]==name)dup=true;
   if(!dup){seen[count++]=name;inclusive[name]++;}
  }
 }
 if(++rounds%12)return;
 if(fd<0)for(int i=0;i<2 && fd<0;i++)fd=open(paths[i],O_WRONLY|O_CREAT|O_TRUNC,0644);
 if(fd<0)return;
 auto top=[&](std::unordered_map<std::string,uint32_t> &map,char tag,unsigned limit,std::string &out) {
  std::vector<std::pair<uint32_t,std::string>> rows;
  for(auto &entry:map)rows.push_back({entry.second,entry.first});
  std::sort(rows.rbegin(),rows.rend());
  for(unsigned i=0;i<rows.size() && i<limit;i++){char line[320];snprintf(line,sizeof line,"%c %u %s\n",tag,rows[i].first,rows[i].second.c_str());out+=line;}
 };
 std::string out="samples "+std::to_string(total)+"\n";
 top(leaf,'L',60,out);top(inclusive,'I',90,out);
 {   // exact addresses (image path, offset in the image) for offline symbolization, the busiest ones
  std::vector<std::pair<uint32_t,uint64_t>> rows;
  for(auto &entry:addresses)rows.push_back({entry.second.inclusive,entry.first});
  std::sort(rows.rbegin(),rows.rend());
  for(unsigned i=0;i<rows.size() && i<1500;i++){
   Dl_info info={};if(!dladdr((const void *)rows[i].second,&info) || !info.dli_fname)continue;
   char line[400];snprintf(line,sizeof line,"A %u %u %s 0x%llx\n",rows[i].first,addresses[rows[i].second].leaf,info.dli_fname,(unsigned long long)(rows[i].second-(uint64_t)info.dli_fbase));
   out+=line;
  }
 }
 ftruncate(fd,0);lseek(fd,0,SEEK_SET);
 if(write(fd,out.data(),out.size())<0)return;
}
static void traceStartWriter() {
 bool expected=false;
 if(!gTraceStarted.compare_exchange_strong(expected,true))return;
 std::thread([]{
  pthread_setname_np("frame-trace");
  int fd=-1;
  static const char *const paths[]={"/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frames.trace","/private/var/root/Library/Caches/com.apple.backboardd/m2v/frames.trace"};
  uint32_t written=0,sampled=0;int profileFd=-1;
  static const char *const profilePaths[]={"/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/cpu.profile","/private/var/root/Library/Caches/com.apple.backboardd/m2v/cpu.profile"};
  for(;;) {
   usleep(400000);
   sampleWriteProfile(sampled,profileFd,profilePaths);
   if(fd<0)for(const char *path:paths){fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0)break;}
   if(fd<0)continue;
   uint32_t head=gTraceHead.load();
   if(head<written+4)continue;
   head-=4;                                   // records still being filled stay for the next round
   if(head-written>16000)written=head-16000;   // the ring lapped the writer: the oldest are gone
   while(written<head){
    uint32_t n=head-written;const uint32_t room=16384-written%16384;if(n>room)n=room;
    if(write(fd,&gTrace[written%16384],n*sizeof(TraceRec))<0)break;
    written+=n;
   }
  }
 }).detach();
}

struct FrameAttribution {
 uint64_t prevCommitUs=0,prevCtcUs=0,prevKpx=0;
 struct Bin {uint32_t n=0,heavy=0;uint64_t sumI=0,sumPg=0,sumEnc=0,sumKpx=0;int64_t sumRes=0;} bins[4];
};
static FrameAttribution gAttribution;
// Who calls waitUntilCompleted: the caller's return address (the 36 low bits) and its caller's, with a count and the total wait.
struct WaitCaller {std::atomic<uint64_t> lr{0},lr2{0};std::atomic<uint32_t> n{0};std::atomic<uint64_t> us{0};char thread[24]={0},symbol[28]={0};};
static WaitCaller gWaitCallers[8];
static void noteWaitCaller(uint64_t lr,uint64_t lr2,uint64_t us) {
 for(unsigned i=0;i<8;i++) {
  uint64_t seen=gWaitCallers[i].lr.load();
  if(!seen){uint64_t zero=0;if(gWaitCallers[i].lr.compare_exchange_strong(zero,lr)){
   gWaitCallers[i].lr2=lr2;pthread_getname_np(pthread_self(),gWaitCallers[i].thread,sizeof gWaitCallers[i].thread);
   Dl_info info;if(dladdr((void*)(uintptr_t)lr2,&info)&&info.dli_sname)strlcpy(gWaitCallers[i].symbol,info.dli_sname,sizeof gWaitCallers[i].symbol);
  }seen=gWaitCallers[i].lr.load();}
  if(seen==lr){gWaitCallers[i].n++;gWaitCallers[i].us+=us;return;}
 }
}
static std::mutex gCommitLock;
// Whether a stall is periodic, what a heavy commit holds, and how often one is heavy: the K ring (32 commits, printed once each) could say none
// of it. Every 5 s of frames becomes one W5 row (fps, the frame intervals' classes, the frames' GPU mean and 99th, their passes and pixels), the
// five heaviest commits of the last minute the H1..H5 rows (with the time they committed, their composition, and the texture and buffer work
// that led to them), and the commit GPU times past 8, 16 and 32 ms the X row. Fed at each completion; diagnostic only.
struct HeavyCommit {uint64_t at;uint32_t gpu,ctc;uint32_t passes,draws;uint64_t kpx;uint32_t tex,buf;};
struct FrameWindow {
 uint64_t start;          // when this window's first frame committed
 uint64_t lastFrame;      // when its previous frame did
 uint32_t frames,onTime,late,slow;
 uint64_t passes,kpx;
 uint32_t gpuUs[1024];
 uint32_t gpuCount;
 uint64_t spikes[5];      // commits past 8, 16 and 32 ms of GPU, then past 16 and 32 ms of commit-to-callback
 HeavyCommit heavy[5];    // the heaviest of the last minute, by GPU time
 // The last window that closed, which the W5 row prints.
 uint32_t valid;
 uint64_t fps,mean,p99,rpX10,kpxMean;
};
static FrameWindow gWindow;
static std::mutex gWindowLock;
// The texture and buffer work since the last commit: a replaceRegion, a texture or buffer made, a surface imported, a buffer-to-texture copy.
// Thread-local: the render server makes them on the thread that commits.
static thread_local uint32_t tTextureOps,tBufferOps;
static void noteCommitWork(CommitRecord r,uint32_t gpu) {
 const uint64_t at=r.committedUs;
 std::lock_guard<std::mutex> lock(gWindowLock);
 FrameWindow &w=gWindow;
 if(!w.start)w.start=at;
 // The frames: a command buffer with a render pass, its interval since the previous one (under 200 ms: a screen at rest is neither on time
 // nor slow), its passes and pixels, and its GPU time.
 if(r.work.passes){
  ++w.frames;w.passes+=r.work.passes;w.kpx+=r.work.pixels>>10;
  if(w.lastFrame){const uint64_t gap=at>w.lastFrame?at-w.lastFrame:0;
   if(gap && gap<200000){if(gap<=16700)++w.onTime;else if(gap<=33000)++w.late;else ++w.slow;}}
  w.lastFrame=at;
  if(w.gpuCount<sizeof w.gpuUs/sizeof w.gpuUs[0])w.gpuUs[w.gpuCount++]=(uint32_t)gpu;
 }
 if(gpu>8000)++w.spikes[0];
 if(gpu>16000)++w.spikes[1];
 if(gpu>32000)++w.spikes[2];
 if(r.callbackUs>16000)++w.spikes[3];
 if(r.callbackUs>32000)++w.spikes[4];
 // The heaviest of the last minute: the lightest of the five goes where one is heavier, and one older than the minute goes first.
 if(gpu){
  int slot=-1;
  for(unsigned i=0;i<5;i++)if(w.heavy[i].at+60000000ull<at)w.heavy[i]=HeavyCommit{};
  for(unsigned i=0;i<5 && slot<0;i++)if(!w.heavy[i].at)slot=(int)i;
  if(slot<0){
   int lightest=0;
   for(unsigned i=1;i<5;i++)if(w.heavy[i].gpu<w.heavy[lightest].gpu)lightest=(int)i;
   if(gpu>w.heavy[lightest].gpu)slot=lightest;
  }
  if(slot>=0){HeavyCommit &h=w.heavy[slot];
   h.at=at;h.gpu=gpu;h.ctc=(uint32_t)r.callbackUs;h.passes=r.work.passes;h.draws=r.work.draws;
   h.kpx=r.work.pixels>>10;h.tex=r.work.tex;h.buf=r.work.buf;}
 }
 // A window publishes after 5 s of frames, and when the screen rests (a gap of 200 ms or more ends it): its frames a second over the span
 // they took, their GPU mean and 99th, their passes (times ten, a frame) and their pixels.
 const bool rested=w.lastFrame && at-w.lastFrame>=200000;
 if(at-w.start<5000000 && !rested)return;
 const uint64_t end=w.lastFrame?w.lastFrame:at;
 const uint64_t span=end>w.start?end-w.start:1;
 uint64_t gpuSum=0;
 for(uint32_t i=0;i<w.gpuCount;i++)gpuSum+=w.gpuUs[i];
 if(w.frames){
  w.valid=1;
  w.fps=(uint64_t)w.frames*1000000ull/span;
  w.mean=w.gpuCount?gpuSum/w.gpuCount:0;
  w.p99=0;
  if(w.gpuCount){
   std::sort(w.gpuUs,w.gpuUs+w.gpuCount);
   w.p99=w.gpuUs[w.gpuCount>1?((uint64_t)(w.gpuCount-1)*99)/100:0];
  }
  w.rpX10=(w.passes*10)/w.frames;
  w.kpxMean=w.kpx/w.frames;
 }
 w.start=at;w.lastFrame=0;w.frames=w.onTime=w.late=w.slow=0;w.passes=w.kpx=0;w.gpuCount=0;
}
// Command buffers committed in the last windowUs microseconds (at most the ring's 32): shader priming waits while the render server animates.
extern "C" unsigned native_bridge_metal_recent_commits(uint64_t windowUs) {
 const uint64_t now=nowUs();unsigned n=0;
 std::lock_guard<std::mutex> lock(gCommitLock);
 for(const CommitRecord &r: gCommitRing)if(r.index && r.committedUs<=now && now-r.committedUs<=windowUs)n++;
 return n;
}
static std::mutex gProblemLock;
static char gLastProblems[2][200];
// The process's first problem, kept: a client process has no telemetry table, and its first refusal is usually the cause of the rest.
static char gFirstProblem[200];
// Called once with the first problem, on the thread that hit it: a polling reader missed WidgetRenderer's, which crashed within its first second.
void (*native_bridge_metal_problem_hook)(const char *);
static void noteProblem(NSString *what) {
 gProblems++;
 std::lock_guard<std::mutex> lock(gProblemLock);
 // A device answer the runtime lacks is reported at creation in every process and is harmless: the first problem is the next one.
 if(!gFirstProblem[0] && ![what hasPrefix:@"native-metal: no device answer"]) {
  strlcpy(gFirstProblem,what.UTF8String?:"?",sizeof gFirstProblem);
  if(native_bridge_metal_problem_hook)native_bridge_metal_problem_hook(gFirstProblem);
 }
 memcpy(gLastProblems[1],gLastProblems[0],sizeof gLastProblems[0]);
 strlcpy(gLastProblems[0],what.UTF8String?:"?",sizeof gLastProblems[0]);
}
#define NBLOG(...) do{NSString *nbText=[NSString stringWithFormat:__VA_ARGS__];NSLog(@"%@",nbText);noteProblem(nbText);}while(0)
// The pixel formats textures were asked for (first eight distinct), with how many were refused: native_bridge_metal_formats.
static uint32_t gFormats[8];static unsigned gFormatCount;static std::atomic<uint64_t> gTextureRefusals;
// Textures over IOSurfaces: made, refused, and how many of the requests named a planar surface.
static std::atomic<uint64_t> gSurfaceTextures,gSurfaceRefusals,gSurfacePlanar;
// The last refused surface texture: the check that refused it (1 plane, 2 base, 3 plane range, 4 import, 5 texture), the surface's format,
// planes, requested plane, plane offset, plane row bytes and allocation, and the descriptor's size and pixel format.
struct SurfaceRefusal {uint32_t step,format,planes,plane,pixelFormat;uint64_t offset,row,allocation,width,height;};
static SurfaceRefusal gSurfaceLastRefusal;
// Shader priming's progress (PrimeShaders.h, in the plugin), for the PR row.
extern "C" volatile uint32_t native_prime_compiling,native_prime_libraries_done,native_prime_libraries_total,native_prime_made,native_prime_rate;
static void refuseSurface(uint32_t step,IOSurfaceRef surface,MTLTextureDescriptor *d,NSUInteger plane,uint64_t offset,uint64_t row) {
 gSurfaceRefusals++;
 SurfaceRefusal r={step,surface?IOSurfaceGetPixelFormat(surface):0,surface?(uint32_t)IOSurfaceGetPlaneCount(surface):0,(uint32_t)plane,
                   d?(uint32_t)d.pixelFormat:0,offset,row,surface?IOSurfaceGetAllocSize(surface):0,d?d.width:0,d?d.height:0};
 std::lock_guard<std::mutex> lock(gProblemLock);gSurfaceLastRefusal=r;
}
static void noteFormat(uint32_t format,bool made) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 if(!made)gTextureRefusals++;
 for(unsigned i=0;i<gFormatCount;i++)if(gFormats[i]==format)return;
 if(gFormatCount<8)gFormats[gFormatCount++]=format;
}
static NSError *nbError(NSString *why) {
 return [NSError errorWithDomain:@"org.blu-sharky.native-metal" code:-1 userInfo:@{NSLocalizedDescriptionKey:why?:@"refused"}];
}
static NSString *lastError() {return [NSString stringWithUTF8String:nmb_last_error()];}
// The render pipeline journal: one line per render pipeline creation (when, on which thread, how long, the outcome, both functions with
// every constant value, the attachments, blending and vertex descriptor) appended to pipelines.log in the runtime's cache directory, which
// the board log exports with the caches (CacheExport.h): every pipeline a boot made, or was refused, can be made again on the host.
static std::mutex gJournalLock;
static std::string gJournalPath;
static uint64_t gJournalBegan;
static void journal(const std::string &line) {
 if(gJournalPath.empty())return;
 std::lock_guard<std::mutex> lock(gJournalLock);
 if(FILE *f=fopen(gJournalPath.c_str(),"a")){fwrite(line.data(),1,line.size(),f);fputc('\n',f);fclose(f);}
}
// The draw census (censusDraw), on where Turnip's trace is (NMB_GPU_TRACE).
static bool gDrawCensus;
static std::mutex gCensusLock;
static std::unordered_map<std::string,uint64_t> gCensus;
static uint64_t gCensusDraws;
static std::string gCensusPath;

// The render thread's CPU profile (U row: the compositor spends about 73 ms of CPU per frame). From the 200th commit with a render pass,
// the committing thread is sampled about every millisecond: suspended, its registers read, its frame-pointer chain walked within its
// stack (nothing is allocated while it is suspended: it may hold the allocator's lock), resumed. Identical stacks are counted together.
// A sample of the thread waiting within 30 ms of a running one (between the frames of an animation, not at rest) is kept apart, with its
// link register after the program counter: a wait is a system call whose leaf wrapper keeps the return address there (U row: the thread
// runs about 37 of every 53 ms of a frame).
// Once the thread has been idle for 5 s after new samples, profile.txt in the runtime's cache directory is rewritten (through a rename,
// so the cache export never reads half a file): every address with its image, offset and dladdr's symbol, then each running stack (S)
// and each waiting stack (W) with its count.
static std::string gProfilePath;
static mach_port_t gProfiled;
static uint64_t profileStrip(uint64_t address) {return address>>47?address&0x0000000FFFFFFFFFull:address;}
static void writeProfile(const std::map<std::vector<uint64_t>,uint32_t> &stacks,const std::map<std::vector<uint64_t>,uint32_t> &waits,
                         uint64_t samples,uint64_t running,uint64_t waiting,uint64_t since,std::unordered_map<uint64_t,std::string> &names) {
 std::unordered_map<uint64_t,uint32_t> index;std::vector<uint64_t> addresses;
 for(const auto *map:{&stacks,&waits})for(const auto &entry:*map)for(uint64_t address:entry.first)
  if(index.emplace(address,(uint32_t)addresses.size()).second)addresses.push_back(address);
 const std::string temporary=gProfilePath+".new";
 FILE *f=fopen(temporary.c_str(),"w");
 if(!f)return;
 fprintf(f,"NBPROF1 samples=%llu running=%llu waiting=%llu stacks=%zu addresses=%zu since=%llu\n",(unsigned long long)samples,
         (unsigned long long)running,(unsigned long long)waiting,stacks.size(),addresses.size(),(unsigned long long)since);
 for(size_t i=0;i<addresses.size();i++) {
  const uint64_t address=addresses[i];
  auto known=names.find(address);
  if(known==names.end()) {
   Dl_info info{};char text[256];
   if(dladdr((const void *)address,&info) && info.dli_fname) {
    const char *image=strrchr(info.dli_fname,'/');image=image?image+1:info.dli_fname;
    snprintf(text,sizeof text,"%s+%llx %s",image,(unsigned long long)(address-(uint64_t)info.dli_fbase),info.dli_sname?info.dli_sname:"-");
   } else snprintf(text,sizeof text,"? -");
   known=names.emplace(address,text).first;
  }
  fprintf(f,"A %zu %llx %s\n",i,(unsigned long long)address,known->second.c_str());
 }
 for(const auto &entry:stacks) {
  fprintf(f,"S %u",entry.second);
  for(uint64_t address:entry.first)fprintf(f," %u",index[address]);
  fputc('\n',f);
 }
 for(const auto &entry:waits) {
  fprintf(f,"W %u",entry.second);
  for(uint64_t address:entry.first)fprintf(f," %u",index[address]);
  fputc('\n',f);
 }
 fclose(f);
 rename(temporary.c_str(),gProfilePath.c_str());
}
// The same profile as telemetry rows (PR, native_bridge_metal_telemetry): profile.txt sits on the tablet's Data, which no host tool reads,
// so the kernel log is the only way out. The rows give the images (a letter each, with their leaf samples), the functions by their own
// samples and by the samples they are anywhere in (dladdr's symbol start: exact for the plugin and Turnip, the closest exported symbol
// for the shared cache's images), the call sites by the samples they are in (exact return addresses, for the host's symbolizer), and the
// waits by the call site of the system call. Offsets are from each image's header; the rows go out a page at a time.
static std::mutex gProfileRowsLock;
static std::vector<std::string> gProfileRows;
static void summarizeProfile(const std::map<std::vector<uint64_t>,uint32_t> &stacks,const std::map<std::vector<uint64_t>,uint32_t> &waits,
                             uint64_t running,uint64_t waiting) {
 struct Where {char image;uint64_t offset,start;};
 std::unordered_map<uint64_t,Where> where;std::vector<std::string> images;
 auto locate=[&](uint64_t address)->Where {
  auto known=where.find(address);
  if(known!=where.end())return known->second;
  Where w{'?',address,address};
  Dl_info info{};
  if(dladdr((const void *)address,&info) && info.dli_fname) {
   const char *name=strrchr(info.dli_fname,'/');name=name?name+1:info.dli_fname;
   size_t i=std::find(images.begin(),images.end(),std::string(name))-images.begin();
   if(i==images.size())images.push_back(name);
   w.image=i<26?(char)('a'+i):i<52?(char)('A'+i-26):'?';
   w.offset=address-(uint64_t)info.dli_fbase;
   w.start=info.dli_saddr?(uint64_t)info.dli_saddr-(uint64_t)info.dli_fbase:w.offset;
  }
  where.emplace(address,w);return w;
 };
 typedef std::map<std::pair<char,uint64_t>,uint64_t> Counts;
 Counts own,in,sites,waited;std::map<char,uint64_t> leaves;
 for(const auto &entry:stacks) {
  std::set<std::pair<char,uint64_t>> seenFunctions,seenSites;
  for(size_t i=0;i<entry.first.size();i++) {
   const Where w=locate(entry.first[i]);
   // A leaf in the plugin or Turnip by its 64-byte bucket: their local symbols are stripped on the tablet, so dladdr's symbol start is
   // the closest exported function, and the host's symbolizer (with the build's dSYM) folds the buckets into functions.
   if(!i) {
    const size_t li=w.image>='a'?(size_t)(w.image-'a'):(size_t)(w.image-'A'+26);
    const bool own_code=w.image!='?' && li<images.size() &&
     (images[li].find("NativeAdrenoMetal")!=std::string::npos || images[li].find("libvulkan")!=std::string::npos);
    own[{w.image,own_code?(w.offset&~(uint64_t)63):w.start}]+=entry.second;leaves[w.image]+=entry.second;
   }
   if(seenFunctions.insert({w.image,w.start}).second)in[{w.image,w.start}]+=entry.second;
   if(i && seenSites.insert({w.image,w.offset}).second)sites[{w.image,w.offset}]+=entry.second;
  }
 }
 // Who is under the system calls and the allocator: for a sample whose leaf is in libsystem_kernel or libsystem_malloc, its first frame
 // in the plugin or in Turnip (the code that asked), keyed with the leaf's letter.
 Counts askers;
 for(const auto &entry:stacks) {
  if(entry.first.empty())continue;
  const Where leaf=locate(entry.first[0]);
  const size_t li=leaf.image>='a'?(size_t)(leaf.image-'a'):(size_t)(leaf.image-'A'+26);
  if(leaf.image=='?' || li>=images.size())continue;
  const std::string &leafName=images[li];
  if(leafName.find("libsystem_kernel")==std::string::npos && leafName.find("libsystem_malloc")==std::string::npos)continue;
  for(size_t i=1;i<entry.first.size();i++) {
   const Where w=locate(entry.first[i]);
   const size_t wi=w.image>='a'?(size_t)(w.image-'a'):(size_t)(w.image-'A'+26);
   if(w.image=='?' || wi>=images.size())continue;
   if(images[wi].find("NativeAdrenoMetal")==std::string::npos && images[wi].find("libvulkan")==std::string::npos)continue;
   askers[{w.image,w.offset}]+=entry.second;break;
  }
 }
 // What the render thread waits in, between the frames of an animation: the first frame of a waiting stack in QuartzCore, the plugin or
 // Turnip (who asked to wait), and the second one above it in QuartzCore (what for).
 Counts waiters;
 for(const auto &entry:waits) {
  unsigned found=0;
  for(size_t i=1;i<entry.first.size() && found<2;i++) {
   const Where w=locate(entry.first[i]);
   const size_t wi=w.image>='a'?(size_t)(w.image-'a'):(size_t)(w.image-'A'+26);
   if(w.image=='?' || wi>=images.size())continue;
   const std::string &name=images[wi];
   if(name.find("QuartzCore")==std::string::npos && name.find("NativeAdrenoMetal")==std::string::npos &&
      name.find("libvulkan")==std::string::npos)continue;
   waiters[{w.image,w.offset}]+=entry.second;++found;
  }
 }
 for(const auto &entry:waits)   // the program counter is in the system call's wrapper, the link register at its call site
  if(entry.first.size()>1) {const Where w=locate(entry.first[1]);waited[{w.image,w.offset}]+=entry.second;}
 std::vector<std::string> rows;
 auto pack=[&](const char *label,const Counts &counts,unsigned most) {
  std::vector<std::pair<uint64_t,std::pair<char,uint64_t>>> sorted;
  for(const auto &c:counts)sorted.push_back({c.second,c.first});
  std::sort(sorted.begin(),sorted.end(),[](const auto &a,const auto &b){return a.first>b.first;});
  std::string line;unsigned n=0,part=0;
  for(const auto &s:sorted) {
   if(n++>=most)break;
   char entry[40];snprintf(entry,sizeof entry," %c%llx=%llu",s.second.first,(unsigned long long)s.second.second,(unsigned long long)s.first);
   if(line.size()+strlen(entry)>84){rows.push_back(line);line.clear();}
   if(line.empty()){char head[12];snprintf(head,sizeof head,"PR%s%u",label,part++);line=head;}
   line+=entry;
  }
  if(!line.empty())rows.push_back(line);
 };
 {
  std::string line;char head[64];snprintf(head,sizeof head,"PRi run%llu wait%llu",(unsigned long long)running,(unsigned long long)waiting);line=head;
  for(size_t i=0;i<images.size() && i<52;i++) {
   const char letter=i<26?(char)('a'+i):(char)('A'+i-26);
   char entry[64];snprintf(entry,sizeof entry," %c=%.18s:%llu",letter,images[i].c_str(),(unsigned long long)leaves[letter]);
   if(line.size()+strlen(entry)>88){rows.push_back(line);line="PRi";}
   line+=entry;
  }
  rows.push_back(line);
 }
 pack("s",own,200);pack("f",in,8);pack("c",sites,40);pack("w",waited,8);pack("k",askers,24);pack("x",waiters,12);
 // The exported names dladdr gives for the shared cache's addresses at the top of the self, wait and waiter lists (the host has the cache's
 // local symbols only, which miss its exported functions: the system call stubs among them).
 {
  std::vector<std::pair<uint64_t,uint64_t>> named;
  for(const auto &known:where) {
   const size_t wi=known.second.image>='a'?(size_t)(known.second.image-'a'):(size_t)(known.second.image-'A'+26);
   if(known.second.image=='?' || wi>=images.size())continue;
   const std::string &name=images[wi];
   if(name.find("NativeAdrenoMetal")!=std::string::npos || name.find("libvulkan")!=std::string::npos)continue;
   uint64_t count=0;
   for(const Counts *counts:{&own,&waited,&waiters}) {
    auto hit=counts->find({known.second.image,known.second.offset});if(hit!=counts->end())count+=hit->second;
    hit=counts->find({known.second.image,known.second.start});if(hit!=counts->end())count+=hit->second;
   }
   if(count)named.push_back({count,known.first});
  }
  std::sort(named.begin(),named.end(),[](const auto &a,const auto &b){return a.first>b.first;});
  std::set<std::string> said;
  for(const auto &n:named) {
   if(said.size()>=24)break;
   Dl_info info{};
   if(!dladdr((const void *)n.second,&info) || !info.dli_sname)continue;
   const Where w=where[n.second];
   char line[100];snprintf(line,sizeof line,"PRn %c%llx=%.40s+%llx",w.image,(unsigned long long)w.offset,info.dli_sname,
                         (unsigned long long)(n.second-(uint64_t)info.dli_saddr));
   if(said.insert(line).second)rows.push_back(line);
  }
 }
 std::lock_guard<std::mutex> lock(gProfileRowsLock);
 gProfileRows.swap(rows);
}
static void *profileLoop(void *) {
 pthread_setname_np("nb-profile");
 const mach_port_t target=gProfiled;
 uintptr_t stackHigh=0,stackLow=0;
 if(pthread_t thread=pthread_from_mach_thread_np(target)) {
  stackHigh=(uintptr_t)pthread_get_stackaddr_np(thread);stackLow=stackHigh-pthread_get_stacksize_np(thread);
 }
 std::map<std::vector<uint64_t>,uint32_t> stacks,waits;std::unordered_map<uint64_t,std::string> names;
 uint64_t samples=0,running=0,waiting=0,written=0,lastRunning=0;
 const uint64_t since=nowUs();
 for(;;) {
  usleep(1000);
  thread_basic_info_data_t info{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
  if(thread_info(target,THREAD_BASIC_INFO,(thread_info_t)&info,&n)!=KERN_SUCCESS)break;   // the thread ended
  samples++;
  const bool isRunning=info.run_state==TH_STATE_RUNNING;
  if(isRunning || (lastRunning && nowUs()-lastRunning<30000)) {
   uint64_t frames[20];unsigned depth=0;
   if(thread_suspend(target)==KERN_SUCCESS) {
    arm_thread_state64_t state{};mach_msg_type_number_t count=ARM_THREAD_STATE64_COUNT;
    if(thread_get_state(target,ARM_THREAD_STATE64,(thread_state_t)&state,&count)==KERN_SUCCESS) {
     frames[depth++]=profileStrip(arm_thread_state64_get_pc(state));
     if(!isRunning)frames[depth++]=profileStrip(arm_thread_state64_get_lr(state));
     uintptr_t fp=(uintptr_t)arm_thread_state64_get_fp(state);
     while(depth<20 && fp>=stackLow && fp+16<=stackHigh && !(fp&7)) {
      const uintptr_t *record=(const uintptr_t *)fp;
      const uint64_t ret=profileStrip(record[1]);
      if(!ret)break;
      frames[depth++]=ret;
      if(record[0]<=fp)break;
      fp=record[0];
     }
    }
    thread_resume(target);
   }
   if(depth && isRunning){running++;stacks[std::vector<uint64_t>(frames,frames+depth)]++;lastRunning=nowUs();}
   else if(depth){waiting++;waits[std::vector<uint64_t>(frames,frames+depth)]++;}
  }
  if(running!=written && nowUs()-lastRunning>=5000000) {
   writeProfile(stacks,waits,samples,running,waiting,since,names);summarizeProfile(stacks,waits,running,waiting);written=running;
  }
 }
 return nullptr;
}
static void startProfile() {
 if(gProfilePath.empty())return;
 gProfiled=mach_thread_self();   // the send right stays with the sampler
 pthread_t thread;
 if(pthread_create(&thread,nullptr,profileLoop,nullptr)==0)pthread_detach(thread);
}

@interface NBObject : NSObject
@property(atomic,copy) NSString *label;
@end
@implementation NBObject
- (id<MTLDevice>)device {return gDeviceObject;}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing(class_getName(object_getClass(self)),sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing %s %s",class_getName(object_getClass(self)),sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
@end

// ---- resources ----
// The wait takes a strong snapshot of already-committed buffers; new commits require the
// caller's usual resource synchronization. Storage dependency checks are in the runtime.
static NSMutableSet *gInFlight;
static void waitForResource(id resource);
// Resource SPI the runtime has no state for: no protected content (Metal's default options), and no owner transfer
// (memory attribution is the kernel driver's, by the importing task), refused as not supported.
#define NB_RESOURCE_SPI \
- (uint64_t)protectionOptions {return 0;} \
- (id<MTLHeap>)heap {return _parentHeap;} \
- (BOOL)isAliasable {return NO;} \
- (int)setOwnerWithIdentity:(unsigned)identity {(void)identity;return 46; /* KERN_NOT_SUPPORTED */} \
- (void)waitUntilComplete {waitForResource(self);}
@interface NBBuffer : NBObject
@property(nonatomic) NmbBuffer *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) void *wrapped;
@property(nonatomic,strong) id<MTLHeap> parentHeap;   // the heap that made the buffer, nil otherwise
@property(nonatomic) NSUInteger heapOffset;
@end
@interface NBTexture : NBObject
@property(nonatomic) NmbTexture *handle;
@property(nonatomic) NmbTextureDescriptor descriptor;
@property(nonatomic,strong) NBBuffer *buffer;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NSUInteger offset,rowBytes;
@property(nonatomic,strong) id<MTLTexture> parent;
@property(nonatomic,strong) id<MTLHeap> parentHeap;
@property(nonatomic) NSUInteger heapOffset;
@end
// A no-copy import keeps its pages until the runtime frees it, which can be after the buffer object goes (a command buffer still using
// it). The owner's release (Metal's deallocator, an IOSurface's mapping) waits for that, as Metal's does: run earlier, the owner could
// unmap or reuse pages the GPU still reads or writes.
static std::atomic<uint64_t> gNoCopyAll,gNoCopyMade,gNoCopyReleased,gSurfaceImportsMade,gSurfaceImportsReleased;
// The bytes the CPU puts into a texture or takes out of one (replaceRegion and getBytes): a full-screen 1800x2880 readback is 20 MB and
// stalls on the GPU that wrote it, which no other row would show, and a frame's CPU has to be accounted for before anything is changed.
static Timed gTimeRegion;
static std::atomic<uint64_t> gRegionBytes;
// What the scanout copy costs, as the plugin's scanout service last reported it (gpupresent::Cost, native_bridge_metal_present_cost): frames,
// then import, texture, encode, submit and wait in nanoseconds, the copy form each frame took, and the textures made and reused (the A4 row).
static std::atomic<uint64_t> gPresent[6],gPresentForms[4],gPresentTextures[2],gPresentGeo[6];
// The runtime's copy census and buffer-texture census (nmb_copy_census, seven words and five), read for the A5 and A6 rows.
static std::atomic<uint64_t> gCopy[7],gBufTex[5];
// The runtime's own phasing of a draw, a pass's close and a commit (nmb_phase_timings, fourteen words): where inside those three calls the
// time goes, which the frontend's own timing of them (A0, A1) cannot tell.
static std::atomic<uint64_t> gPhase[14];
static void readCopyCensus() {
 uint64_t census[12]={};
 if(nmb_copy_census(census)) {
  for(unsigned i=0;i<7;i++)gCopy[i].store(census[i]);
  for(unsigned i=0;i<5;i++)gBufTex[i].store(census[7+i]);
 }
 uint64_t phases[14]={};
 if(nmb_phase_timings(phases))for(unsigned i=0;i<14;i++)gPhase[i].store(phases[i]);
}
static void releaseBlock(void *context,void *pointer,uint64_t length) {
 gNoCopyReleased++;
 void (^deallocator)(void *,NSUInteger)=(__bridge_transfer void (^)(void *,NSUInteger))context;deallocator(pointer,(NSUInteger)length);
}
static void releaseSurface(void *context,void *,uint64_t) {gSurfaceImportsReleased++;CFRelease((IOSurfaceRef)context);}
@implementation NBBuffer
- (void)dealloc {
 nmb_buffer_release(_handle);
}
- (NSUInteger)length {return nmb_buffer_length(_handle);}
- (void *)contents {return nmb_buffer_contents(_handle);}
- (uint64_t)gpuAddress {return nmb_buffer_gpu_address(_handle);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (void)didModifyRange:(NSRange)range {(void)range;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_buffer_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: buffer setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (NSUInteger)allocatedSize {return nmb_buffer_length(_handle);}
- (IOSurfaceRef)iosurface {return nullptr;}   // a buffer is never the backing of a surface here (surfaces back textures)
NB_RESOURCE_SPI
@end
// Reuse only an import still held by a texture/view or pending command. Weak
// values never extend a GPU allocation's lifetime. The strong buffer snapshot
// and its retained surface prevent object/address reuse during a hit. A new
// mapping, allocation identity or size needs a separate import; ordinary
// caller-owned no-copy buffers do not enter this cache.
static NSMapTable<NSValue *,NBBuffer *> *gSurfaceImports;
static std::mutex gSurfaceImportLock;
static NBBuffer *surfaceBuffer(IOSurfaceRef surface,void *base,size_t bytes) {
 struct Key {uint64_t identity,address,bytes;};
 const Key value={IOSurfaceGetID(surface),(uintptr_t)base,bytes};
 NSValue *key=value.identity?[NSValue valueWithBytes:&value objCType:@encode(Key)]:nil;
 std::lock_guard<std::mutex> lock(gSurfaceImportLock);
 if(!gSurfaceImports)gSurfaceImports=[NSMapTable strongToWeakObjectsMapTable];
 NBBuffer *buffer=key?[gSurfaceImports objectForKey:key]:nil;
 if(buffer)return buffer;
 // The surface keeps its CPU mapping (the imported pages) until the import is freed.
 NmbBuffer *handle=nmb_buffer_new_no_copy_releasing(gDevice,base,bytes,MTLResourceStorageModeShared,releaseSurface,(void *)CFRetain(surface));
 if(!handle){CFRelease(surface);NBLOG(@"native-metal: surface import refused: %@",lastError());return nil;}
 gSurfaceImportsMade++;buffer=[NBBuffer new];buffer.handle=handle;buffer.options=MTLResourceStorageModeShared;
 if(key) {
  // Bound metadata without retaining old frames. Evicting an association is
  // harmless: its owners still retain the actual import until their own end.
  if(gSurfaceImports.count>=256) {
   for(NSValue *old in gSurfaceImports.keyEnumerator.allObjects)
    if(![gSurfaceImports objectForKey:old])[gSurfaceImports removeObjectForKey:old];
   if(gSurfaceImports.count>=256)[gSurfaceImports removeObjectForKey:gSurfaceImports.keyEnumerator.nextObject];
  }
  [gSurfaceImports setObject:buffer forKey:key];
 }
 return buffer;
}
static bool textureDescriptor(MTLTextureDescriptor *d,NmbTextureDescriptor *out) {
 if(!d)return false;
 *out={(uint64_t)d.textureType,(uint64_t)d.pixelFormat,d.width,d.height,d.depth,d.mipmapLevelCount,d.sampleCount,d.arrayLength,(uint64_t)d.usage,(uint64_t)d.resourceOptions};
 return true;
}
@implementation NBTexture
- (void)dealloc {nmb_texture_release(_handle);if(_surface)CFRelease(_surface);}
- (MTLTextureType)textureType {return (MTLTextureType)_descriptor.type;}
- (MTLPixelFormat)pixelFormat {return (MTLPixelFormat)_descriptor.format;}
- (NSUInteger)width {return _descriptor.width;}
- (NSUInteger)height {return _descriptor.height;}
- (NSUInteger)depth {return _descriptor.depth;}
- (NSUInteger)mipmapLevelCount {return _descriptor.levels;}
- (NSUInteger)sampleCount {return _descriptor.samples;}
- (NSUInteger)arrayLength {return _descriptor.array_length;}
- (MTLTextureUsage)usage {return (MTLTextureUsage)_descriptor.usage;}
- (MTLResourceOptions)resourceOptions {return (MTLResourceOptions)_descriptor.options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_descriptor.options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_descriptor.options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (BOOL)isFramebufferOnly {return NO;}
- (IOSurfaceRef)iosurface {return _surface;}
- (NSUInteger)iosurfacePlane {return 0;}
- (id<MTLBuffer>)buffer {return _surface?nil:(id<MTLBuffer>)_buffer;}
- (NSUInteger)bufferOffset {return _offset;}
- (NSUInteger)bufferBytesPerRow {return _rowBytes;}
- (id<MTLTexture>)parentTexture {return _parent;}
- (NSUInteger)rotation {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_texture_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: texture setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
NB_RESOURCE_SPI
- (id<MTLTexture>)view:(NmbTexture *)handle {
 if(!handle){NBLOG(@"native-metal: texture view refused: %@",lastError());return nil;}
 NBTexture *view=[NBTexture new];view.handle=handle;NmbTextureDescriptor descriptor;nmb_texture_describe(handle,&descriptor);view.descriptor=descriptor;
 view.parent=(id<MTLTexture>)self;view.buffer=_buffer;view.offset=_offset;view.rowBytes=_rowBytes;
 if(_surface)view.surface=(IOSurfaceRef)CFRetain(_surface);
 return (id<MTLTexture>)view;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {
 return [self view:nmb_texture_new_view(_handle,format,0,nullptr,nullptr,true)];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
 const uint64_t l[2]={levels.location,levels.length},s[2]={slices.location,slices.length};
 return [self view:nmb_texture_new_view(_handle,format,type,l,s,false)];
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 ++tTextureOps;
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 const uint64_t began=nowUs();
 const bool ok=nmb_texture_replace(_handle,region,level,slice,bytes,row,image,length);
 gTimeRegion.add(began);gRegionBytes+=length;
 if(!ok)NBLOG(@"native-metal: replaceRegion refused: %@",lastError());
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row {
 [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height];
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
 const uint64_t region[6]={r.origin.x,r.origin.y,r.origin.z,r.size.width,r.size.height,r.size.depth};
 const uint64_t length=(r.size.depth>1?image*(r.size.depth-1):0)+row*r.size.height;
 const uint64_t began=nowUs();
 const bool ok=nmb_texture_get_bytes(_handle,bytes,row,image,length,region,level,slice);
 gTimeRegion.add(began);gRegionBytes+=length;
 if(!ok)NBLOG(@"native-metal: getBytes refused: %@",lastError());
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level {
 [self getBytes:bytes bytesPerRow:row bytesPerImage:row*r.size.height fromRegion:r mipmapLevel:level slice:0];
}
@end
// A command can use many independent imports/views of one IOSurface. Their
// owners keep the mapping live through this phase. Maintain each exact CPU
// range once; no address identity or decision is carried into another phase
// or command, and distinct/partially overlapping ranges retain their calls.
// The surfaces a command buffer uses, their CPU-written lines cleaned to DRAM before the GPU reads them: the GPU maps these imports
// non-coherent, and sys_dcache_flush does nothing on Apple's arm64, whose caches are coherent with its GPU (2026-10-25: 50 GB "flushed" in
// 0.7 ms). A surface is cleaned the first time a command buffer here uses it and whenever its seed (IOSurface bumps it at each unlock after a
// write) differs from the one it was cleaned at; a GPU-written one keeps its seed and is not walked again. The kernel's own cleaning of
// every read-only import at every submission (about 7 MB and 1.2 ms a submission while apps opened) is off for clients of this frontend
// (GpuProbe.cpp: native_adreno_readonly_imports_owned). DC CVAC at EL0 is allowed (XNU sets SCTLR_EL1.UCI); 64 bytes is the Cortex-A55's and
// A77's line (CTR_EL0 is not readable here without SCTLR_EL1.UCT).
static std::mutex gSurfaceSeedLock;
static std::unordered_map<uint32_t,uint32_t> gSurfaceSeeds;   // IOSurfaceID: the seed it was last cleaned at
static std::atomic<uint64_t> gSurfaceCleanSkips;
static void cleanToCoherency(uintptr_t start,size_t bytes) {
 for(uintptr_t at=start&~(uintptr_t)63,end=start+bytes;at<end;at+=64)__asm__ volatile("dc cvac, %0"::"r"(at):"memory");
 __asm__ volatile("dsb sy":::"memory");
}
static void flushSurfaces(NSMutableSet *surfaces) {
 if(!surfaces.count)return;
 struct Range {uintptr_t base;size_t bytes;uint32_t id,seed;};
 std::vector<Range> ranges;
 ranges.reserve(surfaces.count);
 for(NBTexture *texture in surfaces)if(IOSurfaceRef surface=texture.surface)
  ranges.push_back({reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(surface)),IOSurfaceGetAllocSize(surface),IOSurfaceGetID(surface),
                    IOSurfaceGetSeed(surface)});
 std::sort(ranges.begin(),ranges.end(),[](const Range &a,const Range &b){return a.base<b.base;});
 const auto last=std::unique(ranges.begin(),ranges.end(),[](const Range &a,const Range &b){return a.base==b.base;});
 gSurfaceFlushDuplicates+=(uint64_t)(ranges.end()-last);
 for(auto it=ranges.begin();it!=last;++it) {
  {
   std::lock_guard<std::mutex> lock(gSurfaceSeedLock);
   auto known=gSurfaceSeeds.find(it->id);
   if(known!=gSurfaceSeeds.end() && known->second==it->seed){gSurfaceCleanSkips++;continue;}
   gSurfaceSeeds[it->id]=it->seed;
  }
  const uint64_t began=nowUs();
  cleanToCoherency(it->base,it->bytes);
  gSurfaceFlushBytes+=it->bytes;gTimeSurfaceFlush.add(began);
 }
}
static NBTexture *textureOnBuffer(NBBuffer *buffer,MTLTextureDescriptor *d,NSUInteger offset,NSUInteger row,bool surface) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_on_buffer(buffer.handle,&descriptor,offset,row,surface);
 if(!handle){NBLOG(@"native-metal: texture over buffer refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
// A texture over a two-plane surface: the runtime reads plane 1 from the surface's own layout (two-plane RGB and YCbCr formats).
static NBTexture *textureOnSurface(NBBuffer *buffer,MTLTextureDescriptor *d,IOSurfaceRef surface,NSUInteger plane,NSUInteger offset,NSUInteger row) {
 NmbTextureDescriptor descriptor;if(!buffer || !textureDescriptor(d,&descriptor))return nil;
 const uint64_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface),chroma=(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,1);
 NmbTexture *handle=nmb_texture_new_on_surface(buffer.handle,&descriptor,offset,row,IOSurfaceGetPixelFormat(surface),plane,
  IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),chroma>=base?chroma-base:0,IOSurfaceGetBytesPerRowOfPlane(surface,1));
 if(!handle){NBLOG(@"native-metal: texture over surface refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=buffer;texture.offset=offset;texture.rowBytes=row;return texture;
}
@implementation NBBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row {
 return (id<MTLTexture>)textureOnBuffer(self,d,offset,row,false);
}
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)image {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_texture_new_linear(self.handle,&descriptor,offset,row,image);
 if(!handle){NBLOG(@"native-metal: linear texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.buffer=self;texture.offset=offset;texture.rowBytes=row;return (id<MTLTexture>)texture;
}
@end

// ---- libraries, functions, states ----
@interface NBLibrary : NBObject
@property(nonatomic) NmbLibrary *handle;
@end
@interface NBFunction : NBObject
@property(nonatomic) NmbFunction *handle;
@property(nonatomic,copy) NSString *name;
@property(nonatomic) MTLFunctionType functionType;
@property(nonatomic) std::string constantsText;   // name:type:hex of each constant value it was specialized with (the journal's)
@end
@implementation NBFunction
- (void)dealloc {nmb_function_release(_handle);}
- (MTLFunctionOptions)options {return MTLFunctionOptionNone;}
@end
// The target's own accessors of MTLFunctionConstantValues (as the offline frontend reads them); their ABI is checked before any call.
@interface MTLFunctionConstantValues (NBOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol NBOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end
static bool methodABI(id object,SEL selector,const char *encoding) {
 Method method=class_getInstanceMethod(object_getClass(object),selector);
 return method && !strcmp(method_getTypeEncoding(method),encoding);
}
// Bytes of a function constant of MTLDataType `type` (scalars and 2/4-wide vectors; 3-wide vectors are not qualified).
static uint64_t constantBytes(uint64_t type) {
 switch(type) {
  case 3:case 29:case 33:return 4;
  case 4:case 30:case 34:return 8;
  case 6:case 32:case 36:return 16;
  case 16:case 37:case 41:return 2;
  case 17:case 38:case 42:return 4;
  case 19:case 40:case 44:return 8;
  case 45:case 49:case 53:return 1;
  case 46:case 50:case 54:return 2;
  case 48:case 52:case 56:return 4;
  case 81:case 85:return 8;
  case 82:case 86:return 16;
  case 84:case 88:return 32;
  default:return 0;
 }
}
static std::string quartzCoreCallers();
@implementation NBLibrary
- (void)dealloc {nmb_library_release(_handle);}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 if(!name || !values || !methodABI(values,@selector(newIndexedConstantArray),"@16@0:8") || !methodABI(values,@selector(newNamedConstantArray),"@16@0:8")) {
  if(error)*error=nbError(@"unqualified function constant values");return nil;
 }
 MTLFunctionConstantValues *snapshot=[values copy];
 NSArray *indexed=[snapshot newIndexedConstantArray],*named=[snapshot newNamedConstantArray];
 std::vector<NmbConstant> constants;
 for(unsigned group=0;group<2;group++)for(id value in (group?named:indexed)) {
  if(!methodABI(value,@selector(dataType),"Q16@0:8") || !methodABI(value,@selector(data),"^v16@0:8") ||
     !methodABI(value,group?@selector(name):@selector(index),group?"@16@0:8":"Q16@0:8")) {
   if(error)*error=nbError(@"unqualified constant value ABI");return nil;
  }
  id<NBOriginalConstantValue> v=value;
  const uint64_t bytes=constantBytes(v.dataType);
  if(!bytes || !v.data){if(error)*error=nbError([NSString stringWithFormat:@"constant type %lu is not qualified",(unsigned long)v.dataType]);return nil;}
  constants.push_back({v.dataType,group?0:v.index,group?v.name.UTF8String:nullptr,v.data,bytes});
 }
 std::string text;
 for(const NmbConstant &c:constants) {
  char head[32];if(!c.name)snprintf(head,sizeof head,"#%llu",(unsigned long long)c.index);
  text+=text.empty()?"":" ";text+=c.name?c.name:head;snprintf(head,sizeof head,":%llu:",(unsigned long long)c.type);text+=head;
  for(uint64_t i=0;i<c.length;i++){snprintf(head,sizeof head,"%02x",((const uint8_t *)c.bytes)[i]);text+=head;}
 }
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=nmb_function_new_specialized(_handle,name.UTF8String,constants.data(),(uint32_t)constants.size());gTimeFunction.add(functionBegan);
 // One S line per specialization (int-155: every QuartzCore pipeline was made on its draw, and a refused generic function would leave its
 // generic pipeline unmade without a word): the function, its constants, the time, the refusal and QuartzCore's callers.
 if(!gJournalPath.empty()) {
  char head[96];snprintf(head,sizeof head,"S t=%llu us=%llu ok=%d name=",(unsigned long long)((functionBegan-gJournalBegan)/1000),
      (unsigned long long)(nowUs()-functionBegan),handle?1:0);
  journal(head+std::string(name.UTF8String?:"?")+" k="+text+(handle?std::string():" err="+std::string(lastError().UTF8String?:"?"))+" | "+quartzCoreCallers());
 }
 if(!handle){
  noteProblem([NSString stringWithFormat:@"specialized function %@: %@",name,lastError()]);
  if(error)*error=nbError([NSString stringWithFormat:@"specialized function: %@",lastError()]);return nil;
 }
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 function.constantsText=text;if(error)*error=nil;return (id<MTLFunction>)function;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values completionHandler:(void (^)(id<MTLFunction>,NSError *))handler {
 NSError *error=nil;id<MTLFunction> function=[self newFunctionWithName:name constantValues:values error:&error];
 handler(function,error);
}
- (MTLLibraryType)type {return MTLLibraryTypeExecutable;}
// The descriptor's name and constants; its specialized name only labels Metal's function and its options ask for compile-time work the
// runtime does when the function is made.
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error {
 if(!d.name){if(error)*error=nbError(@"a function name is required");return nil;}
 if(d.constantValues)return [self newFunctionWithName:d.name constantValues:d.constantValues error:error];
 id<MTLFunction> function=[self newFunctionWithName:d.name];
 if(!function){if(error)*error=nbError([NSString stringWithFormat:@"function %@: %@",d.name,lastError()]);return nil;}
 if(error)*error=nil;return function;
}
- (NSArray<NSString *> *)functionNames {
 NSMutableArray *names=[NSMutableArray array];
 for(uint32_t i=0,n=nmb_library_function_count(_handle);i<n;i++) {
  char name[256];nmb_library_function_name(_handle,i,name,sizeof name);[names addObject:[NSString stringWithUTF8String:name]];
 }
 return names;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
 const uint64_t functionBegan=nowUs();
 NmbFunction *handle=name?nmb_function_new(_handle,name.UTF8String):nullptr;gTimeFunction.add(functionBegan);
 if(!handle)return nil;
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 return (id<MTLFunction>)function;
}
// Shader priming (PrimeShaders.h): the function with each of its function constants set to zero, or nil.
- (id<MTLFunction>)nb_newFunctionWithZeroConstants:(NSString *)name {
 NmbFunction *handle=name?nmb_function_new_zero_constants(_handle,name.UTF8String):nullptr;
 if(!handle)return nil;
 NBFunction *function=[NBFunction new];function.handle=handle;function.name=name;function.functionType=(MTLFunctionType)nmb_function_type(handle);
 return (id<MTLFunction>)function;
}
@end
@interface NBComputePipeline : NBObject
@property(nonatomic) NmbComputePipeline *handle;
@end
@implementation NBComputePipeline
- (void)dealloc {nmb_compute_pipeline_release(_handle);}
- (NSUInteger)maxTotalThreadsPerThreadgroup {return nmb_compute_pipeline_max_threads(_handle);}
- (NSUInteger)threadExecutionWidth {return nmb_compute_pipeline_execution_width(_handle);}
@end
@interface NBRenderPipeline : NBObject
@property(nonatomic) NmbRenderPipeline *handle;
@property(nonatomic,copy) NSString *censusName;   // label and fragment function, for the draw census (gDrawCensus)
@end
@implementation NBRenderPipeline
- (void)dealloc {nmb_render_pipeline_release(_handle);}
// The threadgroup size a tile dispatch of this pipeline could use: the device's limit (no tile shaders are offered).
- (NSUInteger)maxTotalThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))return 0;
 return s[0];
}
@end
@interface NBSampler : NBObject
@property(nonatomic) NmbSampler *handle;
@end
@implementation NBSampler
- (void)dealloc {nmb_sampler_release(_handle);}
- (NSUInteger)pixelFormat {return MTLPixelFormatInvalid;}   // SPI: a sampler built from a plain descriptor names no format
@end
@interface NBDepthStencil : NBObject
@property(nonatomic) NmbDepthStencil *handle;
@end
@implementation NBDepthStencil
- (void)dealloc {nmb_depth_stencil_release(_handle);}
@end

// ---- events and fences ----
@interface NBEvent : NBObject
@property(nonatomic) NmbEvent *handle;
@end
@implementation NBEvent
- (void)dealloc {nmb_event_release(_handle);}
@end
@interface NBSharedEvent : NBEvent
@end
static void notified(void *context,uint64_t value);
@implementation NBSharedEvent
- (uint64_t)signaledValue {return nmb_shared_event_value(self.handle);}
- (void)setSignaledValue:(uint64_t)value {nmb_shared_event_set_value(self.handle,value);}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {return nmb_shared_event_wait(self.handle,value,milliseconds);}
// The block runs on the listener's dispatch queue (Metal's contract); the bridge's listener thread only forwards it there.
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value block:(MTLSharedEventNotificationBlock)block {
 if(!block)return;
 dispatch_queue_t queue=listener.dispatchQueue?:dispatch_get_global_queue(QOS_CLASS_DEFAULT,0);
 NSArray *job=@[self,queue,[block copy]];
 if(!nmb_shared_event_notify(self.handle,value,notified,(__bridge_retained void *)job)) {
  (void)(__bridge_transfer NSArray *)(__bridge void *)job;
  NBLOG(@"native-metal: notifyListener refused: %@",lastError());
 }
}
// A handle for another process needs a kernel-backed event this bridge does not have yet: refused, never a handle that could not work.
- (MTLSharedEventHandle *)newSharedEventHandle {
 if(gMissing)gMissing("NBSharedEvent","newSharedEventHandle (cross-process)");
 return nil;
}
@end
static void notified(void *context,uint64_t value) {
 NSArray *job=(__bridge_transfer NSArray *)context;
 id<MTLSharedEvent> event=job[0];MTLSharedEventNotificationBlock block=job[2];
 dispatch_async(job[1],^{block(event,value);});
}
@interface NBFence : NBObject
@property(nonatomic) NmbFence *handle;
@end
@implementation NBFence
- (void)dealloc {nmb_fence_release(_handle);}
@end

// ---- command buffers and encoders ----
@interface NBQueue : NBObject
@property(nonatomic) NmbQueue *handle;
@property(atomic,strong) dispatch_queue_t completionQueue;   // SPI: where command buffers' handlers run (nil: the runtime's thread)
@end
@interface NBCommandBuffer : NBObject {
@public
 EncodedWork encodedWork;   // the command buffer's single encoder thread, not global counter deltas
 uint64_t createdUs;
 uint32_t createCpu,createTid,commitCpu,commitTid;
 BOOL earlyEligible;   // made by the render server's thread: its completed handlers may run at submission (see fireEarly)
}
@property(nonatomic) NmbCommandBuffer *handle;
@property(nonatomic,strong) NBQueue *queue;
@property(atomic,copy) NSString *encodeFailure;
@property(atomic) BOOL committed,failedLocally;
@property(nonatomic) uint64_t committedUs;
@property(nonatomic) uint32_t commitIndex;
@property(nonatomic,strong) NSMutableArray *scheduledHandlers,*completedHandlers;
@property(nonatomic,strong) NSMutableSet *surfaces;
// Runtime handles retain GPU storage, but only the Objective-C owner retains
// a no-copy deallocator, its memory owner and texture/heap ancestry.
@property(nonatomic,strong) NSMutableSet *resourceOwners;
@property(nonatomic,strong) dispatch_group_t handlers;
@property(nonatomic,strong) dispatch_group_t schedulingHandlers;
@property(nonatomic) BOOL schedulingDelivered;
- (void)refuse:(NSString *)what;
@end
@interface NBEncoder : NBObject
@property(nonatomic) NmbEncoder *handle;
@property(nonatomic,strong) NBCommandBuffer *commandBuffer;
@end
static void waitForResource(id resource) {
 const uint64_t began=nowUs();
 NSArray *pending;@synchronized(gInFlight){pending=gInFlight.allObjects;}
 for(NBCommandBuffer *commandBuffer in pending) {
  int dependency=-1;
  if([resource isKindOfClass:[NBBuffer class]])dependency=nmb_command_buffer_uses_buffer(commandBuffer.handle,[(NBBuffer *)resource handle]);
  else if([resource isKindOfClass:[NBTexture class]])dependency=nmb_command_buffer_uses_texture(commandBuffer.handle,[(NBTexture *)resource handle]);
  gResourceChecks++;
  // An unknown result retains the old conservative wait. False means no matching storage
  // (or GPU work already completed), not a fabricated completion for a pending dependency.
  if(dependency!=0)[(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];else gResourceSkipped++;
 }
 gTimeResourceWait.add(began);
}
@implementation NBEncoder
- (void)dealloc {if(_handle)nmb_encoder_discard(_handle);}
- (bool)check:(bool)ok what:(const char *)what {
 if(!ok)[_commandBuffer refuse:[NSString stringWithFormat:@"%s: %@",what,lastError()]];
 return ok;
}
- (void)endEncoding {
 NmbEncoder *handle=_handle;_handle=nullptr;
 if(!handle){[_commandBuffer refuse:@"endEncoding twice"];return;}
 const uint64_t began=nowUs();
 [self check:nmb_encoder_end(handle) what:"endEncoding"];
 gTimePassClose.add(began);
}
- (void)insertDebugSignpost:(NSString *)s {(void)s;}
- (void)pushDebugGroup:(NSString *)s {(void)s;}
- (void)popDebugGroup {}
// Stage 0 compute, 1 vertex, 2 fragment (the bridge's numbering).
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 if(buffer && ![(id)buffer isKindOfClass:[NBBuffer class]]){[_commandBuffer refuse:@"foreign buffer"];return;}
 if(buffer)[_commandBuffer.resourceOwners addObject:buffer];
 const uint64_t began=nowUs();
 [self check:nmb_encoder_set_buffer(_handle,stage,buffer?[(NBBuffer *)buffer handle]:nullptr,offset,(uint32_t)index) what:"setBuffer"];
 gTimeBind.add(began);
}
- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage {
 if(texture && ![(id)texture isKindOfClass:[NBTexture class]]){[_commandBuffer refuse:@"foreign texture"];return;}
 if(texture)[_commandBuffer.resourceOwners addObject:texture];
 if(texture && [(NBTexture *)texture surface])[_commandBuffer.surfaces addObject:texture];
 const uint64_t began=nowUs();
 [self check:nmb_encoder_set_texture(_handle,stage,texture?[(NBTexture *)texture handle]:nullptr,(uint32_t)index) what:"setTexture"];
 gTimeBind.add(began);
}
- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage {
 if(sampler && ![(id)sampler isKindOfClass:[NBSampler class]]){[_commandBuffer refuse:@"foreign sampler"];return;}
 const uint64_t began=nowUs();
 [self check:nmb_encoder_set_sampler(_handle,stage,sampler?[(NBSampler *)sampler handle]:nullptr,(uint32_t)index) what:"setSamplerState"];
 gTimeBind.add(began);
}
- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage {
 const uint64_t began=nowUs();
 [self check:nmb_encoder_set_bytes(_handle,stage,bytes,length,(uint32_t)index) what:"setBytes"];
 gTimeBind.add(began);
}
- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update {
 if(![(id)fence isKindOfClass:[NBFence class]]){[_commandBuffer refuse:@"foreign fence"];return;}
 NmbFence *handle=[(NBFence *)fence handle];
 [self check:update?nmb_encoder_update_fence(_handle,handle,stages):nmb_encoder_wait_fence(_handle,handle,stages) what:update?"updateFence":"waitForFence"];
}
- (void)updateFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:true];}
- (void)waitForFence:(id<MTLFence>)fence {[self fence:fence stages:0 update:false];}
- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage {
 [self check:nmb_encoder_set_buffer_offset(_handle,stage,offset,(uint32_t)index) what:"setBufferOffset"];
}
@end

@interface NBComputeEncoder : NBEncoder
@end
@implementation NBComputeEncoder
- (MTLDispatchType)dispatchType {return MTLDispatchTypeSerial;}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state {
 if(![(id)state isKindOfClass:[NBComputePipeline class]]){[self.commandBuffer refuse:@"foreign compute pipeline"];return;}
 [self check:nmb_compute_set_pipeline(self.handle,[(NBComputePipeline *)state handle]) what:"setComputePipelineState"];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:0];}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:0];}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:0];
}
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:0];}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:0];}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self texture:t[k] index:r.location+k stage:0];
}
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:0];}
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i {
 [self check:nmb_compute_set_threadgroup_memory(self.handle,(uint32_t)n,(uint32_t)i) what:"setThreadgroupMemoryLength"];
}
- (void)dispatch:(MTLSize)size group:(MTLSize)group threads:(bool)threads {
 gDispatches++;self.commandBuffer->encodedWork.dispatches++;
 const uint64_t s[3]={size.width,size.height,size.depth},g[3]={group.width,group.height,group.depth};
 [self check:nmb_compute_dispatch(self.handle,s,g,threads) what:"dispatch"];
}
- (void)dispatchThreads:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:true];}
- (void)dispatchThreadgroups:(MTLSize)t threadsPerThreadgroup:(MTLSize)g {[self dispatch:t group:g threads:false];}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {(void)r;(void)u;}
- (void)memoryBarrierWithScope:(MTLBarrierScope)s {(void)s;}
@end

@interface NBRenderEncoder : NBEncoder {
@public
 NSString *censusPipeline,*censusTexture;   // the draw census's pipeline and fragment texture 0 (gDrawCensus)
 uintptr_t lastPipeline,drawnPipeline;      // the pipeline set now, and the one the previous draw ran under (the A3 row)
 uint64_t draws;                            // this encoder's draws so far
 // What this encoder last pushed, so that a set which leaves its state as it is skips the runtime: the pipeline, depth/stencil state,
 // viewport and scissor sent, and the raster, stencil reference and blend colour values (Metal's defaults 0 until set).
 // (A new object's ivars are zero, which is Metal's default for each.)
 uintptr_t lastPipelinePushed,lastDepthStencil;
 BOOL lastPipelineSet,lastDepthStencilSet,lastViewportSet,lastScissorSet,rasterSet,stencilSet,blendColorSet;
 double lastViewport[6];
 uint64_t lastScissor[4];
 uint32_t rasterCull,rasterWinding,rasterFill,rasterPushed;
 uint32_t stencilFrontValue,stencilBackValue;
 float blendColor[4];
}
@end
// The draw's bucket by what it costs (A3): the first of its encoder, then the later ones by whether the pipeline state changed since the draw
// before it.
static void noteDrawBucket(NBRenderEncoder *encoder,uint64_t began) {
 Timed &bucket=!encoder->draws?gTimeDrawFirst:(encoder->drawnPipeline==encoder->lastPipeline?gTimeDrawSameState:gTimeDrawNewState);
 bucket.add(began);
 encoder->drawnPipeline=encoder->lastPipeline;
 ++encoder->draws;
}
// The draw census (with Turnip's trace, NativeTurnipTrace): draws counted by pipeline, vertex count and fragment texture 0, written to
// draws.txt beside the journal every 2000 draws, so that the trace's per-shader draw counts name their pipelines and textures.
static void censusDraw(NBRenderEncoder *encoder,NSUInteger vertices,NSUInteger instances) {
 std::string key=std::string((encoder->censusPipeline?:@"?").UTF8String)+" | v"+std::to_string(vertices)+(instances>1?"x"+std::to_string(instances):"")+
  " | t0 "+(encoder->censusTexture?:@"-").UTF8String;
 std::lock_guard<std::mutex> lock(gCensusLock);
 ++gCensus[key];
 if(++gCensusDraws%2000 || gCensusPath.empty())return;
 const std::string temporary=gCensusPath+".tmp";
 if(FILE *f=fopen(temporary.c_str(),"w")){
  for(const auto &[line,count]:gCensus)fprintf(f,"%llu %s\n",(unsigned long long)count,line.c_str());
  fclose(f);rename(temporary.c_str(),gCensusPath.c_str());
 }
}
static NSString *describeCensusTexture(id<MTLTexture> t) {
 if(![(id)t isKindOfClass:[NBTexture class]])return t?@"other":@"-";
 NBTexture *texture=(NBTexture *)t;const NmbTextureDescriptor d=texture.descriptor;
 return [NSString stringWithFormat:@"%llux%llu f%llu L%llu%s",d.width,d.height,d.format,d.levels,texture.surface?" surface":texture.buffer?" buffer":""];
}
@implementation NBRenderEncoder
// Every setter here skips the runtime when it would leave the encoder's state as it is: Metal's state is the encoder's own, a set of the same
// value changes nothing, and the runtime's setters only store (a pipeline's set recomputes the same routing of the same state in the same
// pass). The comparison is the exact value pushed (bytes, not the numbers), so a value that differs in any bit is set again.
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state {
 if(![(id)state isKindOfClass:[NBRenderPipeline class]]){[self.commandBuffer refuse:@"foreign render pipeline"];return;}
 if(gDrawCensus)censusPipeline=[(NBRenderPipeline *)state censusName];
 lastPipeline=(uintptr_t)(__bridge void *)state;
 if(lastPipelineSet && lastPipelinePushed==lastPipeline){++gStateSkipped[kStatePipeline];return;}
 lastPipelinePushed=lastPipeline;lastPipelineSet=true;
 const uint64_t began=nowUs();
 [self check:nmb_render_set_pipeline(self.handle,[(NBRenderPipeline *)state handle]) what:"setRenderPipelineState"];
 gTimeState[kStatePipeline].add(began);gTimeBind.add(began);
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:1];}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {[self buffer:b offset:o index:i stage:2];}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:1];}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {[self offset:o index:i stage:2];}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:1];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger [])o withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self buffer:b[k] offset:o[k] index:r.location+k stage:2];
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:1];}
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {[self bytes:p length:n index:i stage:2];}
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {[self texture:t index:i stage:1];}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {
 if(gDrawCensus && i==0)censusTexture=describeCensusTexture(t);
 [self texture:t index:i stage:2];
}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
 for(NSUInteger k=0;k<r.length;k++)[self setFragmentTexture:t[k] atIndex:r.location+k];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:1];}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {[self sampler:s index:i stage:2];}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state {
 if(state && ![(id)state isKindOfClass:[NBDepthStencil class]]){[self.commandBuffer refuse:@"foreign depth/stencil state"];return;}
 const uintptr_t handle=(uintptr_t)(__bridge void *)state;
 if(lastDepthStencilSet && lastDepthStencil==handle){++gStateSkipped[kStateDepthStencil];return;}
 lastDepthStencil=handle;lastDepthStencilSet=true;
 const uint64_t began=nowUs();
 [self check:nmb_render_set_depth_stencil(self.handle,state?[(NBDepthStencil *)state handle]:nullptr) what:"setDepthStencilState"];
 gTimeState[kStateDepthStencil].add(began);
}
- (void)setViewport:(MTLViewport)v {
 const double values[6]={v.originX,v.originY,v.width,v.height,v.znear,v.zfar};
 if(lastViewportSet && !memcmp(values,lastViewport,sizeof values)){++gStateSkipped[kStateViewport];return;}
 memcpy(lastViewport,values,sizeof values);lastViewportSet=true;
 const uint64_t began=nowUs();
 [self check:nmb_render_set_viewport(self.handle,values) what:"setViewport"];
 gTimeState[kStateViewport].add(began);
}
- (void)setScissorRect:(MTLScissorRect)r {
 const uint64_t values[4]={r.x,r.y,r.width,r.height};
 if(lastScissorSet && !memcmp(values,lastScissor,sizeof values)){++gStateSkipped[kStateScissor];return;}
 memcpy(lastScissor,values,sizeof values);lastScissorSet=true;
 const uint64_t began=nowUs();
 [self check:nmb_render_set_scissor(self.handle,values) what:"setScissorRect"];
 gTimeState[kStateScissor].add(began);
}
// The runtime takes cull mode, winding and fill mode together; this encoder keeps Metal's defaults (none, clockwise, fill: all 0) until each is
// set, and pushes the three again only when one of them changes.
- (void)raster {
 const uint32_t state=rasterCull|(rasterWinding<<2)|(rasterFill<<4);
 if(rasterSet && rasterPushed==state){++gStateSkipped[kStateRaster];return;}
 rasterPushed=state;rasterSet=true;
 const uint64_t began=nowUs();
 [self check:nmb_render_set_raster(self.handle,rasterCull,rasterWinding,rasterFill) what:"raster state"];
 gTimeState[kStateRaster].add(began);
}
- (void)setCullMode:(MTLCullMode)m {rasterCull=(uint32_t)m;[self raster];}
- (void)setFrontFacingWinding:(MTLWinding)w {rasterWinding=(uint32_t)w;[self raster];}
- (void)setTriangleFillMode:(MTLTriangleFillMode)f {rasterFill=(uint32_t)f;[self raster];}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
 const uint64_t began=nowUs();
 [self check:nmb_render_set_depth_bias(self.handle,bias,slope,clamp) what:"setDepthBias"];
 gTimeState[kStateOther].add(began);
}
- (void)setLineWidth:(float)width {
 const uint64_t began=nowUs();
 [self check:nmb_render_set_line_width(self.handle,width) what:"setLineWidth"];
 gTimeState[kStateOther].add(began);
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
 const uint64_t began=nowUs();
 [self check:nmb_render_set_visibility_mode(self.handle,mode,offset) what:"setVisibilityResultMode"];
 gTimeState[kStateOther].add(began);
}
- (void)setFragmentTexture:(id<MTLTexture>)t atTextureIndex:(NSUInteger)ti samplerState:(id<MTLSamplerState>)s atSamplerIndex:(NSUInteger)si {
 [self setFragmentTexture:t atIndex:ti];[self sampler:s index:si stage:2];
}
// Tile shaders and imageblock tile memory (Apple GPU families 4 and up) are not offered: supportsTileShaders answers no, and a call that
// reaches one anyway fails its command buffer.
- (NSUInteger)tileWidth {return 0;}
- (NSUInteger)tileHeight {return 0;}
- (void)setTileTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {(void)t;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {(void)p;(void)n;(void)i;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size {(void)size;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)dispatchThreadsPerTile:(MTLSize)size inRegion:(MTLRegion)region {(void)size;(void)region;[self.commandBuffer refuse:@"tile shaders are not supported"];}
- (void)setStencilReferenceValue:(uint32_t)v {[self stencilFront:v back:v];}
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b {[self stencilFront:f back:b];}
- (void)stencilFront:(uint32_t)f back:(uint32_t)b {
 if(stencilSet && stencilFrontValue==f && stencilBackValue==b){++gStateSkipped[kStateOther];return;}
 stencilFrontValue=f;stencilBackValue=b;stencilSet=true;
 const uint64_t began=nowUs();
 [self check:nmb_render_set_stencil_reference(self.handle,f,b) what:"setStencilReferenceValues"];
 gTimeState[kStateOther].add(began);
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
 const float c[4]={r,g,b,a};
 if(blendColorSet && !memcmp(c,blendColor,sizeof c)){++gStateSkipped[kStateBlend];return;}
 memcpy(blendColor,c,sizeof c);blendColorSet=true;
 const uint64_t began=nowUs();
 [self check:nmb_render_set_blend_color(self.handle,c) what:"setBlendColor"];
 gTimeState[kStateBlend].add(began);
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
 gDraws++;self.commandBuffer->encodedWork.draws++;
 if(gDrawCensus)censusDraw(self,n,i);
 const uint64_t began=nowUs();
 [self check:nmb_render_draw(self.handle,t,(uint32_t)s,(uint32_t)n,(uint32_t)i,(uint32_t)b) what:"drawPrimitives"];
 gTimeDraw.add(began);noteDrawBucket(self,began);
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
 [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:1 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i baseVertex:(NSInteger)v baseInstance:(NSUInteger)bi {
 if(![(id)b isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"foreign index buffer"];return;}
 [self.commandBuffer.resourceOwners addObject:b];
 gDraws++;self.commandBuffer->encodedWork.draws++;
 if(gDrawCensus)censusDraw(self,n,i);
 const uint64_t began=nowUs();
 [self check:nmb_render_draw_indexed(self.handle,t,(uint32_t)n,k,[(NBBuffer *)b handle],o,(uint32_t)i,(int32_t)v,(uint32_t)bi) what:"drawIndexedPrimitives"];
 gTimeDraw.add(began);noteDrawBucket(self,began);
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:i baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)k indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o {
 [self drawIndexedPrimitives:t indexCount:n indexType:k indexBuffer:b indexBufferOffset:o instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s {(void)r;(void)u;(void)s;}
- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:true];}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages {[self fence:fence stages:stages update:false];}
@end

@interface NBBlitEncoder : NBEncoder
@end
@implementation NBBlitEncoder
// The arguments are passed as a plain array: an NSArray literal raises on a nil element, and a nil resource (a refused allocation) must fail the
// command buffer like any invalid argument, not abort the host process.
#define NB_OBJECTS(...) ((__unsafe_unretained id[]){__VA_ARGS__})
- (bool)buffers:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++)if(![objects[i] isKindOfClass:[NBBuffer class]]){[self.commandBuffer refuse:@"missing or foreign blit buffer"];return false;}
 for(size_t i=0;i<count;i++)[self.commandBuffer.resourceOwners addObject:objects[i]];
 return true;
}
- (bool)textures:(__unsafe_unretained id *)objects count:(size_t)count {
 for(size_t i=0;i<count;i++) {
  id o=objects[i];
  if(![o isKindOfClass:[NBTexture class]]){[self.commandBuffer refuse:@"missing or foreign blit texture"];return false;}
  [self.commandBuffer.resourceOwners addObject:o];
  if([(NBTexture *)o surface])[self.commandBuffer.surfaces addObject:o];
 }
 return true;
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)dO size:(NSUInteger)n {
 if(![self buffers:NB_OBJECTS(s,d) count:2])return;
 const uint64_t began=nowUs();
 [self check:nmb_blit_copy_buffer(self.handle,[(NBBuffer *)s handle],so,[(NBBuffer *)d handle],dO,n) what:"copyFromBuffer:toBuffer:"];
 gTimeBlit.add(began);
}
- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v {
 if(![self buffers:NB_OBJECTS(b) count:1])return;
 const uint64_t began=nowUs();
 [self check:nmb_blit_fill_buffer(self.handle,[(NBBuffer *)b handle],r.location,r.location+r.length,v) what:"fillBuffer"];
 gTimeBlit.add(began);
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 const uint64_t began=nowUs();
 [self check:nmb_blit_copy_texture(self.handle,[(NBTexture *)s handle],ss,sl,a,size,[(NBTexture *)d handle],ds,dl,b) what:"copyFromTexture:toTexture:"];
 gTimeBlit.add(began);
}
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d {
 if(![self textures:NB_OBJECTS(s,d) count:2])return;
 const NSUInteger levels=MIN(s.mipmapLevelCount,d.mipmapLevelCount),slices=MIN(s.arrayLength,d.arrayLength);
 for(NSUInteger slice=0;slice<slices;slice++)for(NSUInteger level=0;level<levels;level++) {
  MTLSize size=MTLSizeMake(MAX(s.width>>level,(NSUInteger)1),MAX(s.height>>level,(NSUInteger)1),MAX(s.depth>>level,(NSUInteger)1));
  [self copyFromTexture:s sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toTexture:d destinationSlice:slice
       destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
 }
}
- (void)copyFromBuffer:(id<MTLBuffer>)s sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)image
            sourceSize:(MTLSize)z toTexture:(id<MTLTexture>)d destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)dO {
 if(![self buffers:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])return;
 ++tBufferOps;
 const uint64_t size[3]={z.width,z.height,z.depth},b[3]={dO.x,dO.y,dO.z};
 const uint64_t began=nowUs();
 [self check:nmb_blit_buffer_to_texture(self.handle,[(NBBuffer *)s handle],o,row,image,size,[(NBTexture *)d handle],slice,level,b) what:"copyFromBuffer:toTexture:"];
 gTimeBlit.add(began);
}
- (void)copyFromTexture:(id<MTLTexture>)s sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)z
               toBuffer:(id<MTLBuffer>)d destinationOffset:(NSUInteger)o destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)image {
 if(![self textures:NB_OBJECTS(s) count:1] || ![self buffers:NB_OBJECTS(d) count:1])return;
 const uint64_t a[3]={so.x,so.y,so.z},size[3]={z.width,z.height,z.depth};
 const uint64_t began=nowUs();
 [self check:nmb_blit_texture_to_buffer(self.handle,[(NBTexture *)s handle],slice,level,a,size,[(NBBuffer *)d handle],o,row,image) what:"copyFromTexture:toBuffer:"];
 gTimeBlit.add(began);
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)t {
 if(![self textures:NB_OBJECTS(t) count:1])return;
 const uint64_t began=nowUs();
 [self check:nmb_blit_generate_mipmaps(self.handle,[(NBTexture *)t handle]) what:"generateMipmapsForTexture"];
 gTimeBlit.add(began);
}
- (void)synchronizeResource:(id<MTLResource>)r {(void)r;}
@end

static void completed(void *context);
static void scheduled(void *context);
// Early completed handlers. The render server starts its next frame from the completed handler of the previous one, so a frame's encoding
// waited for the GPU of the frame before it (interval = GPU time + encode time). The handlers of the server's own render buffers run at submission
// instead; the swap offered by them is held back by the scanout client until the GPU finished everything committed before the offer
// (NBWaitInFlight), and waitUntilCompleted, the resource waits and the real completion work (surface flushes, release) are unchanged.
static bool earlyOn() {return false;}   // reverted: with the handlers early the blur passes read frames that were not finished (i343, i344)
static bool onRenderServerThread() {
 char name[32]={};pthread_getname_np(pthread_self(),name,sizeof name);
 return !strncmp(name,"com.apple.coreanimation",23);
}
extern "C" void NBWaitInFlight(uint64_t beforeUs);
@implementation NBCommandBuffer
- (instancetype)init {
 if((self=[super init])){_scheduledHandlers=[NSMutableArray array];_completedHandlers=[NSMutableArray array];_surfaces=[NSMutableSet set];_resourceOwners=[NSMutableSet set];_handlers=dispatch_group_create();_schedulingHandlers=dispatch_group_create();}
 if(self){createdUs=nowUs();createCpu=threadCpuUs();createTid=threadIdLow();earlyEligible=onRenderServerThread();sampleWindowOpen();}
 return self;
}
- (void)dealloc {nmb_command_buffer_release(_handle);}
- (id<MTLCommandQueue>)commandQueue {return (id<MTLCommandQueue>)_queue;}
- (BOOL)retainedReferences {return YES;}
// Protected content is not supported (the runtime's AppleOnlyCall::SetProtectionOptions takes Metal's default only): any other option fails
// the command buffer.
- (uint64_t)protectionOptions {return 0;}
- (void)setProtectionOptions:(uint64_t)options {if(options)[self refuse:[NSString stringWithFormat:@"protection options 0x%llx",options]];}
- (void)refuse:(NSString *)what {
 if(!self.encodeFailure){self.encodeFailure=what;NBLOG(@"native-metal: command buffer refused: %@",what);}
}
- (MTLCommandBufferStatus)status {
 if(self.failedLocally)return MTLCommandBufferStatusError;
 return (MTLCommandBufferStatus)nmb_command_buffer_status(_handle);
}
- (NSError *)error {
 if(self.failedLocally)return nbError(self.encodeFailure);
 if(self.status!=MTLCommandBufferStatusError)return nil;
 char text[512]={};nmb_command_buffer_error(_handle,text,sizeof text);
 return nbError([NSString stringWithUTF8String:text]);
}
// Debug groups and cache hints name work for tools and the driver's caches; they change nothing the GPU computes here.
- (void)pushDebugGroup:(NSString *)name {(void)name;}
- (void)popDebugGroup {}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
- (void)encodeCacheHintFinalize:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count {(void)tag;(void)groups;(void)count;}
// The kernel driver's own submission times are not recorded: 0 is Metal's answer where they are unavailable.
- (CFTimeInterval)kernelStartTime {return 0;}
- (CFTimeInterval)kernelEndTime {return 0;}
- (BOOL)commitAndWaitUntilSubmitted {[(id<MTLCommandBuffer>)self commit];[self waitUntilScheduled];return !self.failedLocally;}
- (CFTimeInterval)GPUStartTime {return nmb_command_buffer_gpu_time(_handle,false);}
- (CFTimeInterval)GPUEndTime {return nmb_command_buffer_gpu_time(_handle,true);}
- (id)encoder:(NBEncoder *)encoder handle:(NmbEncoder *)handle what:(NSString *)what {
 if(self.committed){[self refuse:[what stringByAppendingString:@" after commit"]];return nil;}
 /* A refused encoder is still an object: its calls fail on the null handle and the buffer completes with an error. RenderBox caches the
  * encoder's IMPs with methodForSelector:, and on nil it called address 0 (i568: WidgetRenderer_Default crashed eight launches running, then
  * hit its open cap and lost the device). */
 if(!handle){[self refuse:[NSString stringWithFormat:@"%@: %@",what,lastError()]];encoder.commandBuffer=self;return encoder;}
 encoder.handle=handle;encoder.commandBuffer=self;return encoder;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
 return [self encoder:[NBComputeEncoder new] handle:self.committed?nullptr:nmb_compute_encoder_new(_handle) what:@"computeCommandEncoder"];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
 if(type!=MTLDispatchTypeSerial){[self refuse:@"concurrent compute dispatch"];return nil;}
 return [self computeCommandEncoder];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
 gBlits++;encodedWork.blits++;
 return [self encoder:[NBBlitEncoder new] handle:self.committed?nullptr:nmb_blit_encoder_new(_handle) what:@"blitCommandEncoder"];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
 NmbRenderPassDescriptor pass={};
 {
  id<MTLTexture> first=d.colorAttachments[0].texture;
  if(first) {
   const uint64_t pixels=(uint64_t)first.width*first.height;
   gPasses++;gPassPixels+=pixels;encodedWork.passes++;encodedWork.pixels+=pixels;
   uint64_t seen=gBiggestPass.load();while(pixels>seen && !gBiggestPass.compare_exchange_weak(seen,pixels)){}
   if(pixels>=2000000)gFullScreenPasses++;
   const uint32_t w=(uint32_t)first.width,h=(uint32_t)first.height,f=(uint32_t)first.pixelFormat,s=(uint32_t)first.sampleCount;
   unsigned n=0;const unsigned have=gPassShapeCount.load();
   for(;n<have&&n<6;n++)if(gPassShapes[n].width==w&&gPassShapes[n].height==h&&gPassShapes[n].format==f&&gPassShapes[n].samples==s)break;
   if(n<6){if(n==have){gPassShapes[n].width=w;gPassShapes[n].height=h;gPassShapes[n].format=f;gPassShapes[n].samples=s;gPassShapeCount.store(n+1);}gPassShapes[n].count++;}
  }
 }
 auto attachment=[&](MTLRenderPassAttachmentDescriptor *a,NmbPassAttachment &out)->bool {
  if(!a.texture)return true;
  if(![(id)a.texture isKindOfClass:[NBTexture class]])return false;
  NBTexture *texture=(NBTexture *)a.texture;if(texture.surface)[_surfaces addObject:texture];
  [_resourceOwners addObject:texture];
  out.texture=texture.handle;out.level=a.level;out.slice=a.slice;out.depth_plane=a.depthPlane;out.load=a.loadAction;out.store=a.storeAction;
  return true;
 };
 for(unsigned i=0;i<8;i++) {
  MTLRenderPassColorAttachmentDescriptor *c=d.colorAttachments[i];
  if(!attachment(c,pass.colors[i])){[self refuse:@"foreign color attachment"];return nil;}
  pass.colors[i].clear[0]=c.clearColor.red;pass.colors[i].clear[1]=c.clearColor.green;pass.colors[i].clear[2]=c.clearColor.blue;pass.colors[i].clear[3]=c.clearColor.alpha;
 }
 if(!attachment(d.depthAttachment,pass.depth) || !attachment(d.stencilAttachment,pass.stencil)){[self refuse:@"foreign depth/stencil attachment"];return nil;}
 pass.depth.clear[0]=d.depthAttachment.clearDepth;pass.stencil.clear[0]=d.stencilAttachment.clearStencil;
 pass.width=d.renderTargetWidth;pass.height=d.renderTargetHeight;pass.array_length=d.renderTargetArrayLength;pass.samples=d.defaultRasterSampleCount;
 if(d.visibilityResultBuffer) {
  if(![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]]){[self refuse:@"foreign visibility result buffer"];return nil;}
  [_resourceOwners addObject:d.visibilityResultBuffer];
  pass.visibility=[(NBBuffer *)d.visibilityResultBuffer handle];
 }
 const uint64_t began=nowUs();
 NmbEncoder *encoder=self.committed?nullptr:nmb_render_encoder_new(_handle,&pass);
 gTimePassOpen.add(began);
 return [self encoder:[NBRenderEncoder new] handle:encoder what:@"renderCommandEncoder"];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_scheduledHandlers addObject:[block copy]];}}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {if(block)@synchronized(self){[_completedHandlers addObject:[block copy]];}}
// A drawable (RenderBox's CAMetalLayer path) is presented once the GPU has written it: this runtime's scheduling comes before completion.
- (void)presentDrawable:(id<MTLDrawable>)drawable {[self addCompletedHandler:^(id<MTLCommandBuffer>){[drawable present];}];}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time {[self addCompletedHandler:^(id<MTLCommandBuffer>){[drawable presentAtTime:time];}];}
- (void)enqueue {}
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal {
 if(![(id)event isKindOfClass:[NBEvent class]]){[self refuse:@"foreign event"];return;}
 if(self.committed){[self refuse:@"event encoded after commit"];return;}
 NmbEvent *handle=[(NBEvent *)event handle];
 if(!(signal?nmb_command_buffer_signal_event(_handle,handle,value):nmb_command_buffer_wait_event(_handle,handle,value)))
  [self refuse:[NSString stringWithFormat:@"%s: %@",signal?"encodeSignalEvent":"encodeWaitForEvent",lastError()]];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:true];}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value {[self event:event value:value signal:false];}
// The runtime has submitted the work. Deliver scheduling without waiting for GPU completion.
- (void)didSchedule {
 {
  const uint64_t took=nowUs()-self.committedUs;
  std::lock_guard<std::mutex> lock(gCommitLock);
  CommitRecord &r=gCommitRing[self.commitIndex%32];
  if(r.index==self.commitIndex)r.scheduledUs=took;
 }
 NSArray *handlers;
 @synchronized(self){
  _schedulingDelivered=YES;handlers=[_scheduledHandlers copy];[_scheduledHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in handlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_schedulingHandlers);
 };
 // Run on the runtime's scheduled-handler thread, preserving its ordering and own-wait
 // deadlock checks. The queue's completionQueue routes completed handlers only.
 run();
}
// After completion (or a refusal): invalidate the CPU's view of imported surfaces, then completed handlers, once.
- (void)finish {
 flushSurfaces(_surfaces);
 // Actual GPU completion (or a local refusal that never submitted) is the
 // release point. A retained completed command must not pin every old frame.
 [_surfaces removeAllObjects];[_resourceOwners removeAllObjects];
 @synchronized(gInFlight){[gInFlight removeObject:self];}
 NSArray *completedHandlers;
 @synchronized(self){
  // A frontend refusal or a failed submission never scheduled. Unblock its waiter without inventing a scheduled event.
  if(!_schedulingDelivered){_schedulingDelivered=YES;[_scheduledHandlers removeAllObjects];dispatch_group_leave(_schedulingHandlers);}
  completedHandlers=[_completedHandlers copy];[_completedHandlers removeAllObjects];
 }
 void (^run)(void)=^{
  for(MTLCommandBufferHandler block in completedHandlers)block((id<MTLCommandBuffer>)self);
  dispatch_group_leave(self->_handlers);
 };
 dispatch_queue_t queue=_queue.completionQueue;
 if(queue)dispatch_async(queue,run);else run();
}
- (void)commit {
 if(self.committed){NBLOG(@"native-metal: commit twice");return;}
 self.committed=YES;
 self.committedUs=nowUs();
 if(encodedWork.passes)noteFrameCost();
 commitCpu=threadCpuUs();commitTid=threadIdLow();sampleWindowClose();
 {
  std::lock_guard<std::mutex> lock(gCommitLock);
  // Assign once under the same lock as publication. A concurrent commit cannot reuse this index.
  const uint32_t index=(uint32_t)(gCommits.fetch_add(1)+1);self.commitIndex=index;
  encodedWork.tex=tTextureOps;encodedWork.buf=tBufferOps;tTextureOps=tBufferOps=0;
  CommitRecord &r=gCommitRing[index%32];
  r={};r.index=index;r.committedUs=self.committedUs;r.ageUs=self.committedUs-createdUs;r.work=encodedWork;
 }
 dispatch_group_enter(_handlers);
 dispatch_group_enter(_schedulingHandlers);
 @synchronized(gInFlight){[gInFlight addObject:self];}
 if(!self.encodeFailure) {
  // The commit as a whole: the surfaces' flush, then the submission itself (the CPU's cost per commit, which the A1 row reports).
  const uint64_t began=nowUs();
  flushSurfaces(_surfaces);
  // Completion owns this reference; the runtime orders the non-owning scheduled callback before it.
  void *context=(__bridge_retained void *)self;
  const bool submitted=nmb_command_buffer_on_completed(_handle,completed,context) &&
     nmb_command_buffer_on_scheduled(_handle,scheduled,context) && nmb_command_buffer_commit(_handle);
  gTimeCommit.add(began);
  if(submitted) {[self fireEarly];return;}
  (void)CFBridgingRelease(context);   // refused registration/commit cannot call the registered handlers
  [self refuse:[@"commit: " stringByAppendingString:lastError()]];
 }
 self.failedLocally=YES;gCommitFailures++;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{[self finish];});
}
- (void)fireEarly {
 if(!earlyEligible || encodedWork.passes==0 || !earlyOn())return;
 NSArray *handlers;
 @synchronized(self){handlers=[_completedHandlers copy];[_completedHandlers removeAllObjects];}
 if(!handlers.count)return;
 dispatch_queue_t queue=_queue.completionQueue?:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0);
 dispatch_async(queue,^{for(MTLCommandBufferHandler block in handlers)block((id<MTLCommandBuffer>)self);});
}
- (void)waitUntilCompleted {
 if(!self.committed)return;
 noteWaitSite();
 const uint64_t began=nowUs();
 if(!self.failedLocally)nmb_command_buffer_wait(_handle,0);
 const uint32_t cpuStart=threadCpuUs();
 dispatch_group_wait(_handlers,DISPATCH_TIME_FOREVER);
 {
  TraceRec rec={};rec.type=2;rec.t0=began;rec.t1=nowUs();rec.tid=threadIdLow();rec.a=(uint32_t)(uintptr_t)__builtin_return_address(0);
  rec.cpu0=cpuStart;rec.cpu1=threadCpuUs();traceNote(rec.tid);tracePush(rec);
 }
 noteWaitCaller((uint64_t)__builtin_return_address(0)&0xfffffffffull,(uint64_t)__builtin_return_address(0),nowUs()-began);
 gTimeWaitCompleted.add(began);
}
- (void)waitUntilScheduled {
 if(!self.committed)return;
 const uint64_t began=nowUs();
 if(!self.failedLocally && !nmb_command_buffer_wait_scheduled(_handle)){
  NBLOG(@"native-metal: waitUntilScheduled refused: %@",lastError());return;
 }
 const uint32_t cpuStart=threadCpuUs();
 dispatch_group_wait(_schedulingHandlers,DISPATCH_TIME_FOREVER);
 {
  TraceRec rec={};rec.type=3;rec.t0=began;rec.t1=nowUs();rec.tid=threadIdLow();rec.a=(uint32_t)(uintptr_t)__builtin_return_address(0);
  rec.cpu0=cpuStart;rec.cpu1=threadCpuUs();traceNote(rec.tid);tracePush(rec);
 }
 gTimeWaitScheduled.add(began);
}
@end
// The scanout client: before it copies an offered frame, the GPU work committed up to then must be done.
extern "C" void NBWaitInFlight(uint64_t beforeUs) {
 if(!earlyOn())return;
 NSArray *pending;@synchronized(gInFlight){pending=gInFlight.allObjects;}
 for(NBCommandBuffer *commandBuffer in pending)
  if(commandBuffer.committed && !commandBuffer.failedLocally && commandBuffer.committedUs<=beforeUs)nmb_command_buffer_wait(commandBuffer.handle,0);
}
static void scheduled(void *context) {[(__bridge NBCommandBuffer *)context didSchedule];}
// The runtime's completion: the command buffer's own reference, taken at commit, is given back here.
static void completed(void *context) {
 gCompleted++;
 NBCommandBuffer *commandBuffer=(__bridge_transfer NBCommandBuffer *)context;
 {
  const uint64_t took=nowUs()-commandBuffer.committedUs;
  gTimeEncodeToCommit.count++;gTimeEncodeToCommit.total+=took;
  uint64_t seen=gTimeEncodeToCommit.slowest.load();while(took>seen && !gTimeEncodeToCommit.slowest.compare_exchange_weak(seen,took)){}
  const double gpuStart=nmb_command_buffer_gpu_time(commandBuffer.handle,false),gpuEnd=nmb_command_buffer_gpu_time(commandBuffer.handle,true);
  const uint64_t gpu=(gpuEnd>gpuStart && gpuStart>0)?(uint64_t)((gpuEnd-gpuStart)*1e6):0;
  if(gpu){gGpuTimed++;gGpuUs+=gpu;
   if(commandBuffer->encodedWork.passes){gGpuPassUs+=gpu;gGpuPassN++;}else{gGpuCopyUs+=gpu;gGpuCopyN++;}}else gGpuUntimed++;
  CommitRecord record{};
  {
   std::lock_guard<std::mutex> lock(gCommitLock);
   CommitRecord &r=gCommitRing[commandBuffer.commitIndex%32];
   if(r.index==commandBuffer.commitIndex) {
    r.callbackUs=took;r.gpuUs=gpu;record=r;
    {
     traceStartWriter();traceNote(commandBuffer->createTid);
     if(commandBuffer->commitTid!=commandBuffer->createTid)traceNote(commandBuffer->commitTid);
     TraceRec rec={};rec.type=1;rec.idx=r.index;rec.t0=commandBuffer->createdUs;rec.t1=r.committedUs;rec.t2=r.scheduledUs>=0?r.committedUs+(uint64_t)r.scheduledUs:0;
     rec.t3=r.committedUs+took;rec.a=r.work.passes;rec.b=r.work.draws;rec.c=(uint32_t)(r.work.pixels>>10);rec.d=r.work.blits;
     rec.tid=commandBuffer->createTid;rec.cpu0=commandBuffer->createCpu;rec.cpu1=commandBuffer->commitCpu;rec.tid2=commandBuffer->commitTid;
     tracePush(rec);
    }
    if(r.work.passes>0 && r.work.pixels>0) {
     FrameAttribution &a=gAttribution;
     const uint64_t interval=a.prevCommitUs?r.committedUs-a.prevCommitUs:0;
     if(interval && interval<200000) {
      FrameAttribution::Bin &b=a.bins[interval<11000?0:interval<16700?1:interval<33000?2:3];
      b.n++;b.sumI+=interval;b.sumPg+=a.prevCtcUs;b.sumEnc+=r.ageUs;b.sumKpx+=a.prevKpx;
      b.sumRes+=(int64_t)interval-(int64_t)a.prevCtcUs-(int64_t)r.ageUs;
      if(a.prevKpx>30000)b.heavy++;
     }
     a.prevCommitUs=r.committedUs;a.prevCtcUs=took;a.prevKpx=r.work.pixels>>10;
    }
   }
  }
  if(record.index==commandBuffer.commitIndex)
   noteCommitWork(record,(uint32_t)(gpu>0xffffffffu?0xffffffffu:gpu));
 }
 [commandBuffer finish];
}

@implementation NBQueue
- (void)dealloc {nmb_queue_release(_handle);}
- (id<MTLCommandBuffer>)commandBuffer {
 NmbCommandBuffer *handle=nmb_command_buffer_new(_handle);
 if(!handle){NBLOG(@"native-metal: commandBuffer refused: %@",lastError());return nil;}
 NBCommandBuffer *commandBuffer=[NBCommandBuffer new];commandBuffer.handle=handle;commandBuffer.queue=self;return (id<MTLCommandBuffer>)commandBuffer;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {return [self commandBuffer];}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {(void)queue;}   // submission happens on the caller's thread here
// setCompletionQueue: is the property's setter.
// GPU priorities are not applied (one hardware queue): reported as not applied.
- (BOOL)setGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {(void)priority;return NO;}
// What a new Metal queue reports (MTLIOAccelCommandQueue: normal 1, background 3). RenderBox and Core Image read both before any frame.
- (NSUInteger)getGPUPriority {return 1;}
- (NSUInteger)getBackgroundGPUPriority {return 3;}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d {(void)d;return [self commandBuffer];}
@end

// ---- the device ----
template <typename T> static T failWith(NSError **error,NSString *what,T value) {
 noteProblem([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 if(error)*error=nbError([NSString stringWithFormat:@"%@: %@",what,lastError()]);
 return value;
}
// Which of QuartzCore's paths asked for a pipeline (int-153: every one was made synchronously on its render thread and no generic pipeline
// was ever asked for): its return addresses as offsets from its image header (header 0x189429000 in the 21H461 cache, so offset +
// 0x189429000 is the address in that image's disassembly), the global feature flags it ORs into every context's flags, and its
// CA_DISABLE_GENERIC_SHADERS, CA_FORCE_GENERIC_SHADERS and CA_DISABLE_PRECOMPILED_PIPELINES debug options (ca_debug_options +0x55..0x57,
// read only where dladdr places that address in QuartzCore).
static std::string quartzCoreCallers() {
 void *frames[24];const int n=backtrace(frames,24);
 std::string out="qc";char item[32];unsigned kept=0;uintptr_t header=0;
 for(int i=0;i<n && kept<8;i++) {
  Dl_info info{};
  if(!dladdr(frames[i],&info) || !info.dli_fname || !strstr(info.dli_fname,"/QuartzCore"))continue;
  header=(uintptr_t)info.dli_fbase;
  snprintf(item,sizeof item," %lx",(unsigned long)((uintptr_t)frames[i]-header));out+=item;++kept;
 }
 // The context's own flags (int-154: the draws never took the generic path, which only bit 18 of MetalContext +0x1a0 or the debug byte
 // can cause, and the debug byte was 0). MetalContext::create_pipeline_state (header +0x143000) saves its caller's x19 at fp-8, and its
 // caller MetalContext::draw keeps `this` in x19: the frame record whose return address is +0x143214 links to that frame. Read with
 // vm_read so a different stack layout reports ctx=? instead of faulting.
 if(header) {
  uintptr_t fp=(uintptr_t)__builtin_frame_address(0),ctx=0;
  for(unsigned depth=0;fp && depth<24;++depth) {
   uintptr_t record[2]={};vm_size_t got=0;
   if(vm_read_overwrite(mach_task_self(),fp,sizeof record,(vm_address_t)record,&got)!=KERN_SUCCESS || got!=sizeof record)break;
   if(record[1]-header==0x143214) {
    uintptr_t saved=0;
    if(record[0] && vm_read_overwrite(mach_task_self(),record[0]-8,sizeof saved,(vm_address_t)&saved,&got)==KERN_SUCCESS)ctx=saved;
    break;
   }
   if(record[0]<=fp)break;
   fp=record[0];
  }
  uint32_t flags[2]={};vm_size_t got=0;
  if(ctx && vm_read_overwrite(mach_task_self(),ctx+0x1a0,4,(vm_address_t)&flags[0],&got)==KERN_SUCCESS &&
     vm_read_overwrite(mach_task_self(),ctx+0xb3c,4,(vm_address_t)&flags[1],&got)==KERN_SUCCESS) {
   snprintf(item,sizeof item," ctx=%x/%x",flags[0],flags[1]);out+=item;
  } else out+=" ctx=?";
 }
 static auto global=(uint32_t (*)(void))dlsym(RTLD_DEFAULT,"CARenderGetGlobalFeatureFlags");
 snprintf(item,sizeof item," global=%x",global?global():0xffffffffu);out+=item;
 if(header) {
  const uintptr_t options=header+(0x1f491b3c5ull-0x189429000ull);
  Dl_info info{};
  if(dladdr((void *)options,&info) && info.dli_fname && strstr(info.dli_fname,"/QuartzCore") && (uintptr_t)info.dli_fbase==header) {
   const volatile uint8_t *b=(const volatile uint8_t *)options;
   snprintf(item,sizeof item," dbg=%u%u%u",b[0],b[1],b[2]);out+=item;
  } else out+=" dbg=?";
 }
 return out;
}
// What a creation cost beyond its wall time (int-156: a translation the host does in 6 ms took 665 ms on the tablet, whose single core is
// as fast): this thread's CPU time (user and system, microseconds) and the task's page faults, page-ins and context switches. A creation
// whose CPU time is near its wall time computed; one whose faults are many paid the 4 KiB fault path; one far below it waited.
struct NBCost {
 uint64_t user=0,system=0;int64_t faults=0,pageins=0,switches=0;
 static NBCost now() {
  NBCost c;thread_basic_info_data_t t{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;mach_port_t self=mach_thread_self();
  if(thread_info(self,THREAD_BASIC_INFO,(thread_info_t)&t,&n)==KERN_SUCCESS) {
   c.user=(uint64_t)t.user_time.seconds*1000000+t.user_time.microseconds;c.system=(uint64_t)t.system_time.seconds*1000000+t.system_time.microseconds;
  }
  mach_port_deallocate(mach_task_self(),self);
  task_events_info_data_t e{};n=TASK_EVENTS_INFO_COUNT;
  if(task_info(mach_task_self(),TASK_EVENTS_INFO,(task_info_t)&e,&n)==KERN_SUCCESS){c.faults=e.faults;c.pageins=e.pageins;c.switches=e.csw;}
  return c;
 }
 std::string since(const NBCost &b) const {
  char text[128];snprintf(text,sizeof text," cpu=%llu sys=%llu flt=%lld pin=%lld csw=%lld",(unsigned long long)(user+system-b.user-b.system),
      (unsigned long long)(system-b.system),(long long)(faults-b.faults),(long long)(pageins-b.pageins),(long long)(switches-b.switches));
  return text;
 }
};
// One journal line of a render pipeline creation begun at `began` (microseconds), `refused` its refusal or null. The label is the
// caller's name for the pipeline (QuartzCore's: its spec's encoded name).
static void journalRenderPipeline(MTLRenderPipelineDescriptor *d,uint64_t began,const char *refused,const std::string &cost) {
 if(gJournalPath.empty())return;
 const uint64_t ended=nowUs();uint64_t tid=0;pthread_threadid_np(nullptr,&tid);
 char head[256];
 snprintf(head,sizeof head,"R t=%llu tid=%llu main=%d us=%llu ok=%d samples=%lu topo=%lu ds=%lu/%lu",(unsigned long long)((began-gJournalBegan)/1000),
     (unsigned long long)tid,pthread_main_np(),(unsigned long long)(ended-began),refused?0:1,(unsigned long)d.rasterSampleCount,
     (unsigned long)d.inputPrimitiveTopology,(unsigned long)d.depthAttachmentPixelFormat,(unsigned long)d.stencilAttachmentPixelFormat);
 std::string line=head;
 line+=cost;
 line+=" label=";line+=d.label.length?d.label.UTF8String:"-";
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];if(a.pixelFormat==MTLPixelFormatInvalid)continue;
  snprintf(head,sizeof head," c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.pixelFormat,(unsigned long)a.writeMask,(int)a.blendingEnabled,
      (unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,(unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,
      (unsigned long)a.destinationAlphaBlendFactor,(unsigned long)a.alphaBlendOperation);
  line+=head;
 }
 if(MTLVertexDescriptor *v=d.vertexDescriptor)for(unsigned i=0;i<31;i++) {
  MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
  MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex<31?a.bufferIndex:0];
  snprintf(head,sizeof head," a%u=%lu:%lu:%lu:%lu:%lu:%lu",i,(unsigned long)a.format,(unsigned long)a.offset,(unsigned long)a.bufferIndex,(unsigned long)l.stride,
      (unsigned long)l.stepFunction,(unsigned long)l.stepRate);
  line+=head;
 }
 NBFunction *vf=[(id)d.vertexFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.vertexFunction:nil;
 NBFunction *ff=[(id)d.fragmentFunction isKindOfClass:[NBFunction class]]?(NBFunction *)d.fragmentFunction:nil;
 line+=" | v ";line+=vf?vf.name.UTF8String:"-";line+=" ";line+=vf?vf.constantsText:"";
 line+=" | f ";line+=ff?ff.name.UTF8String:"-";line+=" ";line+=ff?ff.constantsText:"";
 if(refused){line+=" | refused ";line+=refused;}
 line+=" | ";line+=quartzCoreCallers();
 for(char &c:line)if(c=='\n')c=' ';
 journal(line);
}
// A render pipeline descriptor in the bridge's form; false, with the error, for a function of another device.
static bool renderDescriptor(MTLRenderPipelineDescriptor *d,NmbRenderPipelineDescriptor *p,NSError **error) {
 if(![(id)d.vertexFunction isKindOfClass:[NBFunction class]] || (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[NBFunction class]])) {
  if(error)*error=nbError(@"foreign function");return false;
 }
 p->vertex=[(NBFunction *)d.vertexFunction handle];p->fragment=d.fragmentFunction?[(NBFunction *)d.fragmentFunction handle]:nullptr;
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];
  p->colors[i]={(uint64_t)a.pixelFormat,(uint64_t)a.writeMask,(uint64_t)a.blendingEnabled,(uint64_t)a.sourceRGBBlendFactor,(uint64_t)a.destinationRGBBlendFactor,
      (uint64_t)a.rgbBlendOperation,(uint64_t)a.sourceAlphaBlendFactor,(uint64_t)a.destinationAlphaBlendFactor,(uint64_t)a.alphaBlendOperation};
 }
 p->depth_format=d.depthAttachmentPixelFormat;p->stencil_format=d.stencilAttachmentPixelFormat;p->samples=d.rasterSampleCount;
 p->topology=d.inputPrimitiveTopology;p->rasterization=d.rasterizationEnabled;
 if(MTLVertexDescriptor *v=d.vertexDescriptor) {
  for(unsigned i=0;i<31;i++) {
   MTLVertexAttributeDescriptor *a=v.attributes[i];if(a.format==MTLVertexFormatInvalid)continue;
   p->vertex_inputs=1;p->attributes[i]={(uint64_t)a.format,a.offset,a.bufferIndex};
   if(a.bufferIndex<31){MTLVertexBufferLayoutDescriptor *l=v.layouts[a.bufferIndex];p->layouts[a.bufferIndex]={l.stride,(uint64_t)l.stepFunction,l.stepRate};}
  }
 }
 return true;
}

// ---- heaps ----
// Metal heaps over the runtime's: an automatic heap places each resource, a placement heap takes an offset. Resources made from one keep it
// alive (their `heap`); a heap's resources share its storage mode, CPU cache mode and (here) hazard tracking mode.
@interface NBHeap : NBObject
@property(nonatomic) NmbHeap *handle;
@property(nonatomic) MTLResourceOptions options;
@property(nonatomic) MTLHeapType heapType;
@end
@implementation NBHeap
- (void)dealloc {nmb_heap_release(_handle);}
- (NSUInteger)size {return nmb_heap_size(_handle);}
- (NSUInteger)usedSize {return nmb_heap_used_size(_handle);}
- (NSUInteger)currentAllocatedSize {return nmb_heap_allocated_size(_handle);}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment {return nmb_heap_max_available_size(_handle,alignment);}
- (MTLResourceOptions)resourceOptions {return _options;}
- (MTLStorageMode)storageMode {return (MTLStorageMode)((_options&MTLResourceStorageModeMask)>>MTLResourceStorageModeShift);}
- (MTLCPUCacheMode)cpuCacheMode {return (MTLCPUCacheMode)((_options&MTLResourceCPUCacheModeMask)>>MTLResourceCPUCacheModeShift);}
- (MTLHazardTrackingMode)hazardTrackingMode {return (MTLHazardTrackingMode)((_options&MTLResourceHazardTrackingModeMask)>>MTLResourceHazardTrackingModeShift);}
- (MTLHeapType)type {return _heapType;}
- (uint64_t)protectionOptions {return 0;}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
 const uint64_t answer=nmb_heap_set_purgeable_state(_handle,state);
 if(!answer)NBLOG(@"native-metal: heap setPurgeableState refused: %@",lastError());
 return (MTLPurgeableState)answer;
}
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(uint64_t)offset {
 NmbBuffer *handle=nmb_heap_new_buffer(_handle,length,options,offset);
 if(!handle){NBLOG(@"native-metal: heap buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.parentHeap=(id<MTLHeap>)self;
 buffer.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {return [self bufferWithLength:length options:options offset:UINT64_MAX];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset {
 return [self bufferWithLength:length options:options offset:offset];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(uint64_t)offset {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 NmbTexture *handle=nmb_heap_new_texture(_handle,&descriptor,offset);
 if(!handle){NBLOG(@"native-metal: heap texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;
 texture.parentHeap=(id<MTLHeap>)self;texture.heapOffset=offset==UINT64_MAX?0:offset;return (id<MTLTexture>)texture;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {return [self textureWithDescriptor:d offset:UINT64_MAX];}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset {return [self textureWithDescriptor:d offset:offset];}
@end
// Apple's SPI resource groups name resources for cache hints (encodeCacheHintTag:/Finalize:); a group only keeps its resources alive.
@interface NBResourceGroup : NBObject
@property(nonatomic,strong) NSArray *resources;
@end
@implementation NBResourceGroup
@end

// Creation and archive population must use the same complete supported descriptor.
// The handles of a linked-function list, false for one the frontend did not make.
static bool linkedHandles(NSArray<id<MTLFunction>> *functions,std::vector<NmbFunction *> &out) {
 for(id<MTLFunction> f in functions){if(![(id)f isKindOfClass:[NBFunction class]])return false;out.push_back([(NBFunction *)f handle]);}
 return true;
}

// The handles `p` points at live in `linked` and `privateLinked`, which outlive its use.
static bool computeDescriptor(MTLComputePipelineDescriptor *d,NmbComputePipelineDescriptor *p,std::vector<NmbFunction *> &linkedStore,
                              std::vector<NmbFunction *> &privateStore,NSError **error) {
 if(!d || ![(id)d.computeFunction isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign compute function");return false;}
 // Metal may materialize empty stage/linked descriptors on access. Preserve the
 // default meaning while refusing state this frontend cannot yet represent.
 MTLStageInputOutputDescriptor *stage=d.stageInputDescriptor;
 bool empty=!stage || (stage.indexType==MTLIndexTypeUInt16 && stage.indexBufferIndex==0);
 for(NSUInteger index=0;stage && index<31;++index) {
  MTLAttributeDescriptor *a=stage.attributes[index];MTLBufferLayoutDescriptor *b=stage.layouts[index];
  empty=empty && a.format==MTLAttributeFormatInvalid && !a.offset && !a.bufferIndex && !b.stride &&
        b.stepFunction==MTLStepFunctionPerVertex && b.stepRate==1;
 }
 if(!empty){if(error)*error=nbError(@"compute stage inputs are not implemented");return false;}
 MTLLinkedFunctions *linked=d.linkedFunctions;
 if(linked.binaryFunctions.count || linked.groups.count || d.preloadedLibraries.count || d.insertLibraries.count) {
  if(error)*error=nbError(@"compute binary/grouped linked functions and dynamic libraries are not implemented");return false;
 }
 if(!linkedHandles(linked.functions,linkedStore) || !linkedHandles(linked.privateFunctions,privateStore)) {
  if(error)*error=nbError(@"foreign linked function");return false;
 }
 p->linked=linkedStore.data();p->linked_count=linkedStore.size();
 p->private_linked=privateStore.data();p->private_linked_count=privateStore.size();
 p->function=[(NBFunction *)d.computeFunction handle];p->max_threads=d.maxTotalThreadsPerThreadgroup;
 p->multiple_of_execution_width=d.threadGroupSizeIsMultipleOfThreadExecutionWidth;p->indirect=d.supportIndirectCommandBuffers;
 p->max_call_stack_depth=d.maxCallStackDepth;p->adding_binary_functions=d.supportAddingBinaryFunctions;
 for(NSUInteger index=0;index<31;++index)p->mutability[index]=d.buffers[index].mutability;
 return true;
}

// ---- binary archives ----
@interface NBBinaryArchive : NBObject
@property(nonatomic) NmbBinaryArchive *handle;
@end
@implementation NBBinaryArchive
- (void)dealloc {nmb_binary_archive_release(_handle);}
- (NSUInteger)options {return 0;}
- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 auto *p=new NmbRenderPipelineDescriptor();
 const bool ok=renderDescriptor(d,p,error) && nmb_binary_archive_add_render(_handle,p);delete p;
 if(!ok)return failWith<BOOL>(error,@"archive render pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
 NmbComputePipelineDescriptor p={};std::vector<NmbFunction *> linked,privateLinked;
 if(!computeDescriptor(d,&p,linked,privateLinked,error))return NO;
 if(!nmb_binary_archive_add_compute_descriptor(_handle,&p))return failWith<BOOL>(error,@"archive compute pipeline",NO);
 if(error)*error=nil;return YES;
}
- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return NO;}
 if(!nmb_binary_archive_serialize(_handle,url.fileSystemRepresentation))return failWith<BOOL>(error,@"archive serialize",NO);
 if(error)*error=nil;return YES;
}
@end

static bool pipelineArchives(NSArray *list,std::vector<NmbBinaryArchive *> &archives,NSError **error) {
 for(id archive in list) {
  if(![archive isKindOfClass:[NBBinaryArchive class]]){if(error)*error=nbError(@"foreign binary archive");return false;}
  archives.push_back([(NBBinaryArchive *)archive handle]);
 }
 return true;
}

@interface NBDeviceMethods : NSObject
@end
@implementation NBDeviceMethods
- (NSString *)name {return gName;}
- (uint64_t)registryID {return gRegistryID;}
- (NSUInteger)maxBufferLength {return nmb_device_max_buffer_length(gDevice);}
- (BOOL)hasUnifiedMemory {return YES;}
// Core Image's CIMetalDeviceGetLimits reads both (Apple's GPU devices answer them; _MTLDevice does not): without them every CIContext
// creation threw and iconservicesagent aborted while rasterizing symbol glyphs. The GPU has no memory of its own; it shares the system's.
- (uint64_t)dedicatedMemorySize {return 0;}
- (uint64_t)sharedMemorySize {return [NSProcessInfo processInfo].physicalMemory;}
- (BOOL)isLowPower {return NO;}
- (BOOL)isRemovable {return NO;}
- (BOOL)isHeadless {return NO;}
// supportsFamily: and the other queries the runtime answers are installed from it (installAnswers); supportsFamily: also answers the target
// hardware's families (the bridge's TARGET_FAMILIES, journaled with their waived rows as F lines). No legacy feature set is claimed: the
// base implementation reads Apple tables this device does not have.
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {(void)set;return NO;}
// The original discovery (_MTLAddDevice) runs these four on every device it registers. The target's own versions fill the base class's limits
// and feature tables from a hardware profile (`featureProfile`, `indirectArgumentBufferCapabilities`, a table of Apple GPUs) that this device
// has not got: running them would claim an Apple GPU's capabilities. They are empty here, so a limit or feature the runtime measured is answered
// by this file's own answers (installAnswers) and any other stays the base class's neutral 0/NO. Nothing is claimed that nothing measured.
- (void)initLimits {}
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}
- (NSString *)productName {return gName;}
- (NSString *)vendorName {char name[64]={};nmb_device_vendor_name(gDevice,name,sizeof name);return [NSString stringWithUTF8String:name];}
- (MTLSize)maxThreadsPerThreadgroup {
 uint64_t s[3]={};if(!nmb_device_max_threads_per_threadgroup(gDevice,s))NBLOG(@"native-metal: maxThreadsPerThreadgroup: %@",lastError());
 return MTLSizeMake(s[0],s[1],s[2]);
}
- (void)doesNotRecognizeSelector:(SEL)selector {
 if(gMissing)gMissing("NativeBridgeMetalDevice",sel_getName(selector));
 noteProblem([NSString stringWithFormat:@"missing device %s",sel_getName(selector)]);
 [super doesNotRecognizeSelector:selector];
}
- (id<MTLEvent>)newEvent {
 NmbEvent *handle=nmb_event_new(gDevice,false);if(!handle)return nil;
 NBEvent *event=[NBEvent new];event.handle=handle;return (id<MTLEvent>)event;
}
- (id<MTLSharedEvent>)newSharedEvent {
 NmbEvent *handle=nmb_event_new(gDevice,true);if(!handle)return nil;
 NBSharedEvent *event=[NBSharedEvent new];event.handle=handle;return (id<MTLSharedEvent>)event;
}
- (id<MTLFence>)newFence {
 NmbFence *handle=nmb_fence_new(gDevice);if(!handle)return nil;
 NBFence *fence=[NBFence new];fence.handle=handle;return (id<MTLFence>)fence;
}
- (id<MTLCommandQueue>)newCommandQueue {
 NmbQueue *handle=nmb_queue_new(gDevice);if(!handle)return nil;
 NBQueue *queue=[NBQueue new];queue.handle=handle;return (id<MTLCommandQueue>)queue;
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
 if(!d)return nil;
 NmbHeap *handle=nmb_heap_new(gDevice,d.type,d.size,(uint64_t)d.resourceOptions);
 if(!handle){NBLOG(@"native-metal: heap refused: %@",lastError());return nil;}
 NBHeap *heap=[NBHeap new];heap.handle=handle;heap.options=d.resourceOptions;heap.heapType=d.type;return (id<MTLHeap>)heap;
}
- (id)newResourceGroupFromResources:(const id __unsafe_unretained [])resources count:(NSUInteger)count {
 NBResourceGroup *group=[NBResourceGroup new];
 NSMutableArray *held=[NSMutableArray arrayWithCapacity:count];
 for(NSUInteger i=0;i<count;i++)if(resources[i])[held addObject:resources[i]];
 group.resources=held;return group;
}
// Not made here: a texture over raw memory in Apple's layout, and Apple's texture layout object. Both are refused, with the reason, rather than
// answered with a guessed shape.
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *,NSUInteger))deallocator {
 (void)bytes;(void)length;(void)d;(void)deallocator;
 NBLOG(@"native-metal: newTextureWithBytesNoCopy:length:descriptor:deallocator: is not implemented (Apple's texture layout of raw memory is not known)");
 return nil;
}
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed {
 (void)d;(void)backed;
 NBLOG(@"native-metal: newTextureLayoutWithDescriptor:isHeapOrBufferBacked: is not implemented (Apple's layout object is not known)");
 return nil;
}
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count {(void)count;return [(id<MTLDevice>)self newCommandQueue];}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
 ++tBufferOps;
 const uint64_t began=nowUs();
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,nullptr);gTimeCreate.add(began);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
 if(!bytes)return nil;
 NmbBuffer *handle=nmb_buffer_new(gDevice,length,options,bytes);
 if(!handle){NBLOG(@"native-metal: buffer refused: %@",lastError());return nil;}
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;return (id<MTLBuffer>)buffer;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options deallocator:(void (^)(void *,NSUInteger))deallocator {
 void *context=deallocator?(__bridge_retained void *)[deallocator copy]:nullptr;
 NmbBuffer *handle=nmb_buffer_new_no_copy_releasing(gDevice,pointer,length,options,context?releaseBlock:nullptr,context);
 if(!handle) {
  if(context)(void)(__bridge_transfer id)context;
  NBLOG(@"native-metal: no-copy buffer refused: %@",lastError());return nil;
 }
 gNoCopyAll++;if(context)gNoCopyMade++;
 NBBuffer *buffer=[NBBuffer new];buffer.handle=handle;buffer.options=options;buffer.wrapped=pointer;
 return (id<MTLBuffer>)buffer;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
 NmbTextureDescriptor descriptor;if(!textureDescriptor(d,&descriptor))return nil;
 ++tTextureOps;
 const uint64_t began=nowUs();
 NmbTexture *handle=nmb_texture_new(gDevice,&descriptor);gTimeCreate.add(began);noteFormat((uint32_t)descriptor.format,handle!=nullptr);
 if(!handle){NBLOG(@"native-metal: texture refused: %@",lastError());return nil;}
 NBTexture *texture=[NBTexture new];texture.handle=handle;nmb_texture_describe(handle,&descriptor);texture.descriptor=descriptor;return (id<MTLTexture>)texture;
}
// The surface's pages are the texels: its mapping in this process is imported without a copy (pinned by the kernel, mapped into the GPU's
// 4 KiB context by Turnip), so every process that imports the surface renders and samples the same memory.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane {
 // A planar surface (Apple's lossless-compressed BGRA spelling has one plane, video formats two) is imported whole; the texture starts at
 // the plane's own address and rows. Only a plain surface has no planes, and then only plane 0 exists.
 const size_t planes=surface?IOSurfaceGetPlaneCount(surface):0;
 if(!surface || !d || (planes?plane>=planes:plane!=0)){refuseSurface(1,surface,d,plane,0,0);return nil;}
 if(planes)gSurfacePlanar++;
 void *base=IOSurfaceGetBaseAddress(surface);const size_t page=(size_t)getpagesize(),allocation=IOSurfaceGetAllocSize(surface);
 if(!page || (page&(page-1)) || !base || ((uintptr_t)base&(page-1)) || !allocation || allocation>SIZE_MAX-(page-1)){refuseSurface(2,surface,d,plane,(uintptr_t)base,0);return nil;}
 const uintptr_t start=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):(uintptr_t)base;
 const size_t row=planes?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
 if(start<(uintptr_t)base || start-(uintptr_t)base>=allocation || !row){refuseSurface(3,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 const size_t bytes=(allocation+page-1)&~(page-1);
 const uint64_t imported=nowUs();
 NBBuffer *buffer=surfaceBuffer(surface,base,bytes);
 gTimeCreate.add(imported);
 if(!buffer){refuseSurface(4,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 NBTexture *texture=planes>=2?textureOnSurface(buffer,d,surface,plane,start-(uintptr_t)base,row):textureOnBuffer(buffer,d,start-(uintptr_t)base,row,true);
 // Each kind of texture over a surface once in the journal ("T" lines): its size, format, usage and storage, the surface's pixel format,
 // rows and allocation, and whether the runtime made it, so that its messages.log notices (a shadowed texture's copies) meet their surface.
 {
  char line[256];
  snprintf(line,sizeof line,"T surface %lux%lu fmt=%lu usage=0x%lx storage=%lu fourcc=0x%08x planes=%zu plane=%lu offset=%zu row=%zu alloc=%zu made=%d",
   (unsigned long)d.width,(unsigned long)d.height,(unsigned long)d.pixelFormat,(unsigned long)d.usage,(unsigned long)d.storageMode,
   (unsigned)IOSurfaceGetPixelFormat(surface),planes,(unsigned long)plane,(size_t)(start-(uintptr_t)base),row,allocation,texture?1:0);
  static std::mutex seenLock;static std::set<std::string> seen;
  bool fresh;{std::lock_guard<std::mutex> lock(seenLock);fresh=seen.size()<500 && seen.insert(line).second;}
  if(fresh)journal(line);
 }
 if(!texture){refuseSurface(5,surface,d,plane,start-(uintptr_t)base,row);return nil;}
 gSurfaceTextures++;texture.surface=(IOSurfaceRef)CFRetain(surface);return (id<MTLTexture>)texture;
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 const void *bytes=nullptr;size_t size=0;dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes,size,false);(void)mapped;gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
 NSData *bytes=path?[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error]:nil;if(!bytes)return nil;
 const uint64_t libraryBegan=nowUs();
 NmbLibrary *handle=nmb_library_new(gDevice,bytes.bytes,bytes.length,false);gTimeLibrary.add(libraryBegan);
 if(!handle)return failWith<id<MTLLibrary>>(error,@"library",nil);
 NBLibrary *library=[NBLibrary new];library.handle=handle;if(error)*error=nil;return (id<MTLLibrary>)library;
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
 if(!url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
 NSURL *url=[bundle URLForResource:@"default" withExtension:@"metallib"];
 if(!url){if(error)*error=nbError(@"the bundle has no default.metallib");return nil;}
 return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newDefaultLibrary {return [(id<MTLDevice>)self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error {
 return [(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                                                         reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 if(![(id)function isKindOfClass:[NBFunction class]]){if(error)*error=nbError(@"foreign function");return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,nullptr,0};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_options(gDevice,[(NBFunction *)function handle],&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
                                                       reflection:(MTLAutoreleasedComputePipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 NmbComputePipelineDescriptor p={};std::vector<NmbFunction *> linked,privateLinked;
 if(!computeDescriptor(d,&p,linked,privateLinked,error))return nil;
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error))return nil;
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();
 NmbComputePipeline *handle=nmb_compute_pipeline_new_with_descriptor(gDevice,&p,&creation);gTimePipeline.add(pipelineBegan);
 if(!handle)return failWith<id<MTLComputePipelineState>>(error,@"compute pipeline",nil);
 NBComputePipeline *state=[NBComputePipeline new];state.handle=handle;state.label=d.label;gPipelines++;
 if(error)*error=nil;return (id<MTLComputePipelineState>)state;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
 return [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection error:(NSError **)error {
 if(reflection)*reflection=nil;
 auto *p=new NmbRenderPipelineDescriptor();
 if(!renderDescriptor(d,p,error)){delete p;return nil;}
 std::vector<NmbBinaryArchive *> archives;
 if(!pipelineArchives(d.binaryArchives,archives,error)){delete p;return nil;}
 const NmbPipelineOptions creation={(uint64_t)options,archives.data(),archives.size()};
 const uint64_t pipelineBegan=nowUs();const NBCost costBegan=NBCost::now();
 NmbRenderPipeline *handle=nmb_render_pipeline_new_with_options(gDevice,p,&creation);delete p;gTimePipeline.add(pipelineBegan);
 journalRenderPipeline(d,pipelineBegan,handle?nullptr:nmb_last_error(),gJournalPath.empty()?std::string():NBCost::now().since(costBegan));
 if(!handle)return failWith<id<MTLRenderPipelineState>>(error,@"render pipeline",nil);
 NBRenderPipeline *state=[NBRenderPipeline new];state.handle=handle;gPipelines++;if(error)*error=nil;
 if(gDrawCensus)state.censusName=[NSString stringWithFormat:@"%@ | %@",d.label?:@"-",d.fragmentFunction.name?:@"-"];
 return (id<MTLRenderPipelineState>)state;
}
// The asynchronous forms make the pipeline on the caller's thread and call the handler before returning, as the library's
// newFunctionWithName:constantValues:completionHandler: does (RenderBox's pipeline loader, Core Image's bilateral solver).
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler {
 NSError *error=nil;id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d error:&error];
 handler(state,error);
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler {
 NSError *error=nil;id<MTLRenderPipelineState> state=[(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d options:options reflection:nil error:&error];
 handler(state,nil,error);
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler {
 NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function error:&error];
 handler(state,error);
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                          completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler {
 NSError *error=nil;id<MTLComputePipelineState> state=[(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:options reflection:nil error:&error];
 handler(state,nil,error);
}
// SPI: a precompiled pipeline library (RenderBox.framework/default.pipelinelib) holds Apple GPU binaries. None is opened; the caller logs the
// error and builds its pipelines from the shader library instead (RB::Device::render_pipeline_state).
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error {
 if(error)*error=nbError([NSString stringWithFormat:@"no pipeline library on this GPU: %@",path.lastPathComponent]);
 return nil;
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error {
 if(d.url && !d.url.isFileURL){if(error)*error=nbError(@"a file URL is required");return nil;}
 NmbBinaryArchive *handle=nmb_binary_archive_new(gDevice,d.url?d.url.fileSystemRepresentation:nullptr);
 if(!handle)return failWith<id<MTLBinaryArchive>>(error,@"binary archive",nil);
 NBBinaryArchive *archive=[NBBinaryArchive new];archive.handle=handle;if(error)*error=nil;return (id<MTLBinaryArchive>)archive;
}
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d {
 const uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 if(!row)NBLOG(@"native-metal: requiredLinearTextureBytesPerRow: %@",lastError());
 return row;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
 NmbSamplerDescriptor s={(uint64_t)d.minFilter,(uint64_t)d.magFilter,(uint64_t)d.mipFilter,(uint64_t)d.sAddressMode,(uint64_t)d.tAddressMode,(uint64_t)d.rAddressMode,
     (uint64_t)d.borderColor,(uint64_t)d.normalizedCoordinates,d.maxAnisotropy,(uint64_t)d.compareFunction,(uint64_t)d.lodAverage,d.lodMinClamp,d.lodMaxClamp};
 NmbSampler *handle=nmb_sampler_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: sampler refused: %@",lastError());return nil;}
 NBSampler *sampler=[NBSampler new];sampler.handle=handle;return (id<MTLSamplerState>)sampler;
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
 auto face=[](MTLStencilDescriptor *s)->NmbStencil {
  return {(uint64_t)s.stencilCompareFunction,(uint64_t)s.stencilFailureOperation,(uint64_t)s.depthFailureOperation,(uint64_t)s.depthStencilPassOperation,s.readMask,s.writeMask};
 };
 NmbDepthStencilDescriptor s={(uint64_t)d.depthCompareFunction,(uint64_t)d.depthWriteEnabled,face(d.frontFaceStencil),face(d.backFaceStencil)};
 NmbDepthStencil *handle=nmb_depth_stencil_new(gDevice,&s);
 if(!handle){NBLOG(@"native-metal: depth/stencil state refused: %@",lastError());return nil;}
 NBDepthStencil *state=[NBDepthStencil new];state.handle=handle;return (id<MTLDepthStencilState>)state;
}
@end

// The device queries the runtime answers (nmb_device_answer_selector), as methods of the device class. One without an argument is read once
// and installed only where the device has an answer of its own; one with an argument asks the runtime once per argument and keeps the answer
// (every answer is a fixed property of the device; QuartzCore asks thousands a second while the screen moves), and a refusal answers 0/NO
// with the reason logged once.
static std::unordered_map<SEL,uint64_t> gAnswers;
static NSUInteger countAnswer(id,SEL cmd) {return (NSUInteger)gAnswers.at(cmd);}
static BOOL flagAnswer(id,SEL cmd) {return gAnswers.at(cmd)!=0;}
// Answers with an argument (supportsFamily:, a pixel format's alignments, ...) are the device's fixed properties too, but each call asked
// the runtime again, and the runtime audits every family table for a family question: Metal asks supportsFamily: for each texture
// level's info it computes, which was about 30% of the render thread's CPU on the TB128FU (profile of round 054). The first answer for a
// selector and argument is kept; so is a refusal (0), which is logged once.
static std::mutex gArgAnswerLock;
static std::unordered_map<SEL,std::unordered_map<NSUInteger,uint64_t>> gArgAnswers;
static uint64_t askAnswer(SEL cmd,NSUInteger arg) {
 {
  std::lock_guard<std::mutex> lock(gArgAnswerLock);
  auto known=gArgAnswers.find(cmd);
  if(known!=gArgAnswers.end()){auto value=known->second.find(arg);if(value!=known->second.end())return value->second;}
 }
 uint64_t value=0;
 if(!nmb_device_answer(gDevice,sel_getName(cmd),arg,&value)){
  NBLOG(@"native-metal: %s(%lu): %@",sel_getName(cmd),(unsigned long)arg,lastError());value=0;
 }
 // A board may withhold the Apple1 family (NativeMetalWithhold "apple1-family"): Core Animation's Metal context sets the feature bits it
 // blends by framebuffer fetch with from supportsFamily:MTLGPUFamilyApple1 alone (QuartzCore 0x189562720 in iPadOS 17.7.11), whatever
 // the later families answer.
 // That alone changed nothing (tb128fu-081); "apple-families" withholds every Apple family (1001-1999), leaving the common ones.
 // Apple2 stays answered, but only in Preferences: Core Image's Metal gate is supportsFamily:MTLGPUFamilyApple2 (can_use_metal in
 // -[CIContext initWithOptions:], CoreImage 0x197550218), and with every family withheld it takes the OpenGL ES fallback this port has
 // none of (CI::GLContext::init filled a zero function table and jumped to 0: Preferences in Accessibility and iconservicesagent,
 // 2026-10-25). Answering it everywhere stalled the render server at boot (rounds 084558 and 085127: 13-24 commits, then nothing,
 // SpringBoard twelve threads all waiting), so it stays off for the render server; set NMB_APPLE2_CLIENTS to a comma-separated list
 // to change who gets it. The icon daemon has it too since 2026-10-25: without it every graphic symbol icon (Settings' rows) crashed
 // iconservicesagent in CI::GLContext::init (+[CUIShapeEffectStack sharedCIContext]) and those icons stayed blank. Core Animation's framebuffer-fetch blend reads Apple1 alone, so withholding 1001 keeps that cheap.
 static const SEL family=sel_registerName("supportsFamily:");
 static const bool withholdApple1=[]{const char *w=getenv("NMB_WITHHOLD");return w && strstr(w,"apple1-family");}();
 static const bool withholdApple=[]{const char *w=getenv("NMB_WITHHOLD");return w && strstr(w,"apple-families");}();
 static const bool apple2Client=[]{const char *list=getenv("NMB_APPLE2_CLIENTS");if(!list)list="Preferences,iconservicesagent";
  const char *name=getprogname();if(!name)return false;const size_t len=strlen(name);
  for(const char *at=list;*at;) {const char *end=strchr(at,',');const size_t part=end?(size_t)(end-at):strlen(at);
   if(part==len && !strncmp(at,name,len))return true;if(!end)break;at=end+1;}
  return false;}();
 if(cmd==family && ((withholdApple1 && arg==1001) || (withholdApple && arg>=1001 && arg<2000 && !(apple2Client && arg==1002))))value=0;
 std::lock_guard<std::mutex> lock(gArgAnswerLock);
 gArgAnswers[cmd][arg]=value;
 return value;
}
static NSUInteger countArgAnswer(id,SEL cmd,NSUInteger arg) {return (NSUInteger)askAnswer(cmd,arg);}
static BOOL flagArgAnswer(id,SEL cmd,NSUInteger arg) {return askAnswer(cmd,arg)!=0;}
static void installAnswers(Class cls) {
 uint32_t kind=0;
 for(size_t i=0;const char *name=nmb_device_answer_selector(i,&kind);i++) {
  SEL sel=sel_registerName(name);uint64_t value=0;
  switch(kind) {
  case NMB_ANSWER_COUNT: case NMB_ANSWER_FLAG:
   if(!nmb_device_answer(gDevice,name,0,&value)){NBLOG(@"native-metal: no device answer: %@",lastError());continue;}
   // Extended-range 10-bit (XR10) framebuffers are withheld: this display chain presents 8-bit BGRX scanout only. The render server picks its
   // wide-gamut 64-bit (and Apple-compressed) framebuffer when a device offers them, and the display driver could not show it (int-089: the
   // swap's surface was '&w4a', 8 bytes per pixel, and every swap was refused).
   if(!strcmp(name,"supportsExtendedXR10Formats"))value=0;
   gAnswers[sel]=value;
   class_addMethod(cls,sel,kind==NMB_ANSWER_COUNT?(IMP)countAnswer:(IMP)flagAnswer,kind==NMB_ANSWER_COUNT?"Q16@0:8":"B16@0:8");
   break;
  case NMB_ANSWER_COUNT_ARG: class_addMethod(cls,sel,(IMP)countArgAnswer,"Q24@0:8Q16");break;
  case NMB_ANSWER_FLAG_ARG: class_addMethod(cls,sel,(IMP)flagArgAnswer,"B24@0:8Q16");break;
  }
 }
}

extern "C" id native_bridge_metal_open(const char *loader,const char *cache,uint64_t registryID,void (*missing)(const char *,const char *)) {
 if(gDeviceObject)return gDeviceObject;
 gMissing=missing;gInFlight=[NSMutableSet set];
 // Turnip's printed GPU trace (the plugin's NativeTurnipTrace) goes beside the journal, which the board exports; without it an earlier
 // boot's trace is removed (the cache directory persists, and 14 MiB of it overflowed the reader's 32 MiB export copy).
 if(cache && getenv("NMB_GPU_TRACE")){
  setenv("MESA_GPU_TRACEFILE",(std::string(cache)+"/gputrace.txt").c_str(),1);
  gDrawCensus=true;gCensusPath=std::string(cache)+"/draws.txt";
 } else if(cache){
  unlink((std::string(cache)+"/gputrace.txt").c_str());unlink((std::string(cache)+"/draws.txt").c_str());
 }
 gDevice=nmb_device_open(loader,cache);
 if(!gDevice){NBLOG(@"native-metal: device refused: %s",nmb_last_error());return nil;}
 if(cache){gJournalPath=std::string(cache)+"/pipelines.log";gJournalBegan=nowUs();gProfilePath=std::string(cache)+"/profile.txt";}
 char name[128]={};nmb_device_name(gDevice,name,sizeof name);gName=[NSString stringWithUTF8String:name];gRegistryID=registryID;
 // The target families supportsFamily: answers true, with the rows of Apple's tables the device does not meet in them (nmb_device_family_waivers).
 {char waived[4096]={};nmb_device_family_waivers(gDevice,waived,sizeof waived);
  for(char *line=waived,*next;*line;line=next){next=strchr(line,'\n');if(next)*next++=0;else next=line+strlen(line);journal(std::string("F ")+line);}}
 Class base=objc_getClass("_MTLDevice");
#if !TARGET_OS_IPHONE
 // Host instrument: on NSObject every selector a client sends that this file lacks reaches the missing hook, instead of the host's
 // own _MTLDevice implementation.
 if(getenv("NMB_DEVICE_BASE_NSOBJECT"))base=[NSObject class];
#endif
#if TARGET_OS_IPHONE
 // The target's base class ABI (iPadOS 17.7.11): the dynamic subclass adds no instance variables.
 if(!base || class_getInstanceSize(base)!=536){NBLOG(@"native-metal: unexpected _MTLDevice ABI");return nil;}
#endif
 if(!base)base=[NSObject class];
 Class cls=objc_allocateClassPair(base,"NativeBridgeMetalDevice",0);if(!cls)return nil;
 unsigned count=0;Method *methods=class_copyMethodList([NBDeviceMethods class],&count);
 for(unsigned i=0;i<count;i++)class_addMethod(cls,method_getName(methods[i]),method_getImplementation(methods[i]),method_getTypeEncoding(methods[i]));
 free(methods);installAnswers(cls);class_addProtocol(cls,@protocol(MTLDevice));
 // A board may withhold programmable blending (the plugin's NativeMetalWithhold, "programmable-blending"): Core Animation blends every
 // layer in its fragment function through framebuffer fetch where a device offers it (all 1914 of its pipelines in tb128fu-079), and on
 // the Adreno 610 each such draw reads its target back in rasterization order (a full-screen fill took 6 to 21 ms of GPU time). Metal's
 // own answers come from the GPU family, which this device claims. Core Animation does not ask them (tb128fu-080 changed nothing).
 if(const char *withheld=getenv("NMB_WITHHOLD"); withheld && strstr(withheld,"programmable-blending")) {
  IMP no=imp_implementationWithBlock(^BOOL(id) {return NO;});
  for(const char *name:{"supportsProgrammableBlending","supportsFramebufferFetch","_familySupportsProgrammableBlending"})
   class_replaceMethod(cls,sel_registerName(name),no,"B16@0:8");
 }
 // The original registration (_MTLAddDevice) asserts that a plugin's device conforms to Metal's private device protocol as well; the target's own
 // drivers get it from their IOGPU base class. Conformance only answers conformsToProtocol:, the methods are still looked up one by one.
 if(Protocol *spi=objc_getProtocol("MTLDeviceSPI"))class_addProtocol(cls,spi);
 objc_registerClassPair(cls);
 gDeviceObject=[cls new];
 return gDeviceObject;
}

// Bare AIR bitcode (no metallib wrapper), which Metal itself refuses: for the bridge's own fixtures only.
extern "C" id native_bridge_metal_library_with_air(const void *bytes,size_t length) {
 NmbLibrary *handle=gDevice?nmb_library_new(gDevice,bytes,length,true):nullptr;if(!handle)return nil;
 NBLibrary *library=[NBLibrary new];library.handle=handle;return library;
}
extern "C" bool native_bridge_metal_shader_cache_stats(NmbShaderCacheStats *out) {
 return gDevice && nmb_device_shader_cache_stats(gDevice,out);
}
extern "C" bool native_bridge_metal_work_stats(uint64_t out[10]) {
 return gDevice && nmb_device_work_stats(gDevice,out);
}

extern "C" size_t native_bridge_metal_status(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return (size_t)snprintf(out,size,"c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%llu C%llu B%llu %s | %s",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),(unsigned long long)gCompleted.load(),
  (unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),(unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),(unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load(),gLastProblems[0],gLastProblems[1]);
}

static size_t native_bridge_metal_formats_locked(char *out,size_t size) {
 int n=snprintf(out,size,"tex x%llu s%llu/%llu p%llu f",(unsigned long long)gTextureRefusals.load(),(unsigned long long)gSurfaceTextures.load(),
              (unsigned long long)gSurfaceRefusals.load(),(unsigned long long)gSurfacePlanar.load());
 for(unsigned i=0;i<gFormatCount && n>0 && (size_t)n<size;i++)n+=snprintf(out+n,size-(size_t)n,"%u ",gFormats[i]);
 return n>0?(size_t)n:0;
}
// The benchmark's rows (native_bridge_metal_benchmark), kept for the telemetry table.
static std::mutex gBenchLock;
static char gBench[4][96];
static std::atomic<unsigned> gBenchRuns;
extern "C" size_t native_bridge_metal_benchmark(id device,const char *metallib,unsigned pairs,char *out,size_t size) {
 auto now=[]{return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);};
 // the first run fills rows 0-1 (Y, Y1), a later one rows 2-3 (Z, Z1), so a busy and an idle run are both kept
 const unsigned base=gBenchRuns++?2u:0u;
 auto put=[&](unsigned row,const char *format,auto... args){std::lock_guard<std::mutex> lock(gBenchLock);snprintf(gBench[base+row],sizeof gBench[base+row],format,args...);
  if(base)gBench[base+row][0]='Z';};
 // compute: a dependent integer chain (no memory), 100M steps
 uint64_t t=now(),x=0x9e3779b97f4a7c15ull;
 for(uint32_t i=0;i<100000000u;i++){x^=x<<13;x^=x>>7;x^=x<<17;}
 const uint64_t computeUs=(now()-t)/1000;
 // first touch: 32 MiB of fresh anonymous memory (the tablet's free memory is small), one store per 4 KiB page (the kernel's fault and zero-fill), then four memsets of it
 const size_t bytes=32u<<20;
 uint64_t faultUs=0,fillUs=0,shaUs=0;
 void *m=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 if(m!=MAP_FAILED) {
  volatile uint8_t *b=(volatile uint8_t *)m;
  t=now();for(size_t at=0;at<bytes;at+=4096)b[at]=1;faultUs=(now()-t)/1000;
  t=now();for(int k=0;k<4;k++)memset(m,k,bytes);fillUs=(now()-t)/1000;
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  t=now();CC_SHA256(m,(CC_LONG)(16u<<20),digest);shaUs=(now()-t)/1000;
  x^=digest[0];
  munmap(m,bytes);
 }
 put(0,"Y cpu=%llums(100M) fault=%lluus/32MiB=%lluns/pg fill=%lluMB/s sha=%lluMB/s %llx",(unsigned long long)(computeUs/1000),
  (unsigned long long)faultUs,(unsigned long long)(faultUs*1000/8192),(unsigned long long)(fillUs?4ull*32*1000000/fillUs:0),
  (unsigned long long)(shaUs?16ull*1000000/shaUs:0),(unsigned long long)(x&0xf));
 // pipelines: the host tool's pairs, in its order
 uint64_t made=0,refused=0,createUs=0,createMax=0,drawUs=0,drawMax=0,libraryUs=0;
 @autoreleasepool {
  id<MTLDevice> dev=(id<MTLDevice>)device;NSError *error=nil;
  t=now();
  id<MTLLibrary> qc=metallib?[dev newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error]:nil;
  libraryUs=(now()-t)/1000;
  if(qc) {
   NSArray<NSString *> *names=qc.functionNames;
   id<MTLCommandQueue> queue=[dev newCommandQueue];
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
   td.usage=MTLTextureUsageRenderTarget;
   id<MTLTexture> target=[dev newTextureWithDescriptor:td];
   id<MTLBuffer> zeros=[dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
   unsigned n=0;
   for(NSString *name in names) {
    if(n>=pairs)break;
    id<MTLFunction> f=[qc newFunctionWithName:name];
    if(f.functionType!=MTLFunctionTypeVertex)continue;
    NSString *fragmentName=[name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
    if([fragmentName isEqualToString:name] || ![names containsObject:fragmentName])continue;
    ++n;
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=f;d.fragmentFunction=[qc newFunctionWithName:fragmentName];
    d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    t=now();
    id<MTLRenderPipelineState> state=[dev newRenderPipelineStateWithDescriptor:d error:&error];
    const uint64_t took=(now()-t)/1000;
    if(!state){++refused;continue;}
    ++made;createUs+=took;createMax=std::max(createMax,took);
    t=now();
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTLCommandBuffer> commands=[queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder=[commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    for(NSUInteger i=0;i<8;++i){[encoder setVertexBuffer:zeros offset:0 atIndex:i];[encoder setFragmentBuffer:zeros offset:0 atIndex:i];}
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];[commands commit];[commands waitUntilCompleted];
    const uint64_t drew=(now()-t)/1000;
    drawUs+=drew;drawMax=std::max(drawMax,drew);
   }
  }
 }
 put(1,"Y1 qc lib=%llums made=%llu refused=%llu create=%llums max=%llums draw=%llums max=%llums",(unsigned long long)(libraryUs/1000),
  (unsigned long long)made,(unsigned long long)refused,(unsigned long long)(createUs/1000),(unsigned long long)(createMax/1000),
  (unsigned long long)(drawUs/1000),(unsigned long long)(drawMax/1000));
 size_t at=0;
 if(out && size) {
  std::lock_guard<std::mutex> lock(gBenchLock);
  out[0]=0;for(unsigned r=base;r<base+2;r++)at+=(size_t)snprintf(out+at,size>at?size-at:0,"%s\n",gBench[r]);
 }
 return at;
}
// The telemetry table the plugin streams to the kernel's log: newline-separated rows of at most 90 characters.
extern "C" size_t native_bridge_metal_telemetry(char *out,size_t size) {
 size_t at=0;out[0]=0;
 // Turnip's own render pass notes (tu_util.cc, exported as tu_pipa_note_dump): Turnip has no log route in the render server, and its
 // pass geometry (area, format, samples, mode, tile size, GMEM offset) is what tells GMEM's 4-row slices apart (2026-10-24).
 static unsigned (*turnipNotes)(char *, unsigned);static unsigned (*turnipKernel)(char *, unsigned);static unsigned (*turnipPasses)(char *, unsigned);
 static int turnipTried;
 if(!turnipTried) {
  turnipTried=1;
  Dl_info info={};
  if(dladdr((const void *)&native_bridge_metal_telemetry,&info) && info.dli_fname) {
   char path[1024];snprintf(path,sizeof path,"%s",info.dli_fname);
   char *slash=strrchr(path,'/');
   if(slash) {
    snprintf(slash+1,sizeof path-(size_t)(slash+1-path),"libvulkan_freedreno.dylib");
    if(void *lib=dlopen(path,RTLD_NOW|RTLD_LOCAL)) {
     turnipNotes=(unsigned (*)(char *,unsigned))dlsym(lib,"tu_pipa_note_dump");
     turnipKernel=(unsigned (*)(char *,unsigned))dlsym(lib,"tu_pipa_knl_dump");
     turnipPasses=(unsigned (*)(char *,unsigned))dlsym(lib,"tu_pipa_pass_dump");
    }
   }
  }
 }
 // The rows go out in the order they are read (perf report of 2026-10-25): a frame and its waits, where the frame's CPU went, the GPU's own
 // time, then the rest, and Turnip's notes last in whatever room is left. The kernel keeps the first 48 rows of a dump only
 // (NativeAdrenoPublish.cpp's native_adreno_metal_rows[48]) and prints the rows whose text changed, so a row pushed past the 48th is never
 // seen again: the notes (a row per distinct pass, up to 32) had pushed the frame, GPU and per-commit rows out, and those froze at their
 // early values while the counters behind them kept growing.
 unsigned rows=0;
 auto row=[&](const char *format,auto... args){
  if(rows>=48)return;
  char text[200];int n=snprintf(text,sizeof text,format,args...);if(n<=0)return;if(n>90)n=90;
  if(at+(size_t)n+2>=size)return;memcpy(out+at,text,(size_t)n);at+=(size_t)n;out[at++]='\n';out[at]=0;++rows;};
 auto ms=[](uint64_t us){return (unsigned long long)(us/1000);};
 auto mean=[](const Timed &t){const uint64_t n=t.count.load();return (unsigned long long)(n?t.total.load()/n:0);};
 readCopyCensus();
 // While the render thread's profile has rows (summarizeProfile), the rows that only describe single events or the corner work give way to it.
 bool profiling;
 {std::lock_guard<std::mutex> lock(gProfileRowsLock);profiling=!gProfileRows.empty();}
 row("S c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%lluk C%llu B%llu",(unsigned long long)gCommits.load(),(unsigned long long)gCommitFailures.load(),
  (unsigned long long)gCompleted.load(),(unsigned long long)gPipelines.load(),(unsigned long long)gProblems.load(),(unsigned long long)gPasses.load(),
  (unsigned long long)gDraws.load(),(unsigned long long)gFullScreenPasses.load(),(unsigned long long)(gBiggestPass.load()>>10),
  (unsigned long long)gDispatches.load(),(unsigned long long)gBlits.load());
 row("U n=%llu cpu=%llums wall=%llums c %llu %llu %llu %llu|%llu %llu %llu %llu",(unsigned long long)gFrameCostN.load(),
  ms(gFrameCpuUs.load()),ms(gFrameWallUs.load()),(unsigned long long)gFrameCores[0].load(),
  (unsigned long long)gFrameCores[1].load(),(unsigned long long)gFrameCores[2].load(),(unsigned long long)gFrameCores[3].load(),
  (unsigned long long)gFrameCores[4].load(),(unsigned long long)gFrameCores[5].load(),(unsigned long long)gFrameCores[6].load(),
  (unsigned long long)gFrameCores[7].load());
 row("G commit-to-complete n=%llu total=%llums max=%llums mean=%lluus",(unsigned long long)gTimeEncodeToCommit.count.load(),ms(gTimeEncodeToCommit.total.load()),
  ms(gTimeEncodeToCommit.slowest.load()),(unsigned long long)mean(gTimeEncodeToCommit));
 // The 5 s windows (W5): whether a stall comes every so often or at random. fps is the window's frames over its whole span (a screen at rest
 // shows few frames and no slow ones); on/late/slow count the frame intervals up to 16.7 ms, 17 to 33 and 33 and past, intervals under
 // 200 ms only; gpu is the frames' GPU mean and p99 in µs, rp their render passes a frame times ten, kpx their pixels (in 1024s) a frame.
 // X counts the commits past 8, 16 and 32 ms of GPU time, and past 16 and 32 ms of commit-to-callback, since the process began. H1..H5 are
 // the heaviest commits of the last minute, heaviest first: when one committed (seconds of uptime), what it held, and the textures (a
 // replaceRegion, a texture made, a surface imported) and buffers (a buffer made, a buffer-to-texture copy) that led to it.
 {
  std::lock_guard<std::mutex> lock(gWindowLock);
  const FrameWindow &w=gWindow;
  row("W5 fps=%llu on=%u late=%u slow=%u gpu=%llu p99=%llu rp=%llu kpx=%llu",(unsigned long long)w.fps,w.onTime,w.late,w.slow,
   (unsigned long long)w.mean,(unsigned long long)w.p99,(unsigned long long)w.rpX10,(unsigned long long)w.kpxMean);
  row("X spikes gpu8=%llu gpu16=%llu gpu32=%llu ctc16=%llu ctc32=%llu",(unsigned long long)w.spikes[0],(unsigned long long)w.spikes[1],
   (unsigned long long)w.spikes[2],(unsigned long long)w.spikes[3],(unsigned long long)w.spikes[4]);
  HeavyCommit heavy[5];
  memcpy(heavy,w.heavy,sizeof heavy);
  std::sort(heavy,heavy+5,[](const HeavyCommit &a,const HeavyCommit &b){return a.gpu>b.gpu;});
  for(unsigned i=0;i<5 && !profiling;i++)if(heavy[i].at)
   row("H%u t=%llu.%03llus rp=%u dr=%u kpx=%llu tex=%u buf=%u gpu=%u ctc=%u",i+1,(unsigned long long)(heavy[i].at/1000000),
    (unsigned long long)((heavy[i].at/1000)%1000),heavy[i].passes,heavy[i].draws,(unsigned long long)heavy[i].kpx,heavy[i].tex,heavy[i].buf,
    heavy[i].gpu,heavy[i].ctc);
 }
 row("J scheduled wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitScheduled.count.load(),ms(gTimeWaitScheduled.total.load()),ms(gTimeWaitScheduled.slowest.load()));
 row("J completed wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeWaitCompleted.count.load(),ms(gTimeWaitCompleted.total.load()),ms(gTimeWaitCompleted.slowest.load()));
 row("J resource wait n=%llu total=%llums max=%llums",(unsigned long long)gTimeResourceWait.count.load(),ms(gTimeResourceWait.total.load()),ms(gTimeResourceWait.slowest.load()));
 row("J surface clean n=%llu %lluMB dup=%llu same seed %llu us=%llu max=%llu",(unsigned long long)gTimeSurfaceFlush.count.load(),
  (unsigned long long)(gSurfaceFlushBytes.load()>>20),(unsigned long long)gSurfaceFlushDuplicates.load(),
  (unsigned long long)gSurfaceCleanSkips.load(),(unsigned long long)gTimeSurfaceFlush.total.load(),
  (unsigned long long)gTimeSurfaceFlush.slowest.load());
 // The frame's CPU by class of call (count, total and mean per call; the draws' slowest too): the draws, every bind that sets one up
 // (buffers, textures, samplers, inline bytes, the pipeline state), the blit encoder's copies, the pass's open and its close, and the commit
 // with the surfaces it flushes. The frontend only calls the runtime, so a draw or a bind past a few microseconds is time the runtime and
 // Turnip spend inside it.
 row("A0 draw %llu %llums/%lluus/%lluus | bind %llu %llums/%lluus | blit %llu %llums/%lluus",(unsigned long long)gTimeDraw.count.load(),
  ms(gTimeDraw.total.load()),mean(gTimeDraw),(unsigned long long)gTimeDraw.slowest.load(),(unsigned long long)gTimeBind.count.load(),
  ms(gTimeBind.total.load()),mean(gTimeBind),(unsigned long long)gTimeBlit.count.load(),ms(gTimeBlit.total.load()),mean(gTimeBlit));
 row("A1 open %llu %llums/%lluus | close %llu %llums/%lluus | commit %llu %llums/%lluus",(unsigned long long)gTimePassOpen.count.load(),
  ms(gTimePassOpen.total.load()),mean(gTimePassOpen),(unsigned long long)gTimePassClose.count.load(),ms(gTimePassClose.total.load()),
  mean(gTimePassClose),(unsigned long long)gTimeCommit.count.load(),ms(gTimeCommit.total.load()),mean(gTimeCommit));
 row("V gpu timed %llu untimed %llu total=%llums passes=%llums",(unsigned long long)gGpuTimed.load(),(unsigned long long)gGpuUntimed.load(),
  ms(gGpuUs.load()),ms(gGpuPassUs.load()));
 // The GPU's own time by kind of command buffer: those with a render pass, and the scanout copy's, which is one commit a frame of a blit
 // and nothing else (1894 of 3784 commits in the 45 s round of 2026-10-25).
 row("A2 gpu pass n=%llu %llums/%lluus | copy n=%llu %llums/%lluus",(unsigned long long)gGpuPassN.load(),ms(gGpuPassUs.load()),
  (unsigned long long)(gGpuPassN.load()?gGpuPassUs.load()/gGpuPassN.load():0),(unsigned long long)gGpuCopyN.load(),ms(gGpuCopyUs.load()),
  (unsigned long long)(gGpuCopyN.load()?gGpuCopyUs.load()/gGpuCopyN.load():0));
 // The draws by what they cost (count, total, mean): the first of each render encoder, then the later ones by whether the pipeline state
 // changed since the draw before (A3). A draw is a few microseconds of frontend and one call into the runtime, so the rest is the runtime's and
 // Turnip's work inside it.
 if(!profiling)row("A3 draw first %llu %llums/%lluus | state %llu %llums/%lluus | same %llu %llums/%lluus",(unsigned long long)gTimeDrawFirst.count.load(),
  ms(gTimeDrawFirst.total.load()),mean(gTimeDrawFirst),(unsigned long long)gTimeDrawNewState.count.load(),ms(gTimeDrawNewState.total.load()),
  mean(gTimeDrawNewState),(unsigned long long)gTimeDrawSameState.count.load(),ms(gTimeDrawSameState.total.load()),mean(gTimeDrawSameState));
 // The scanout copy, as the service that runs it reports (gpupresent::Cost): frames served, the copy form each took (bytes run together / the
 // frame as a linear texture / row by row), the ns of import, texture, encode, submit and wait a frame, and the textures made and reused.
 // The scanout copy, as the service that runs it reports (gpupresent::Cost): the frames served, the copy form each took (the frame's rows
 // and the scanout's run together / the frame as a linear texture over its pages / row by row), the geometry, and the textures it made
 // and reused. Its five steps are the A5 row.
 row("A4 present %llu %llu/%llu/%llu/%llu geo %llux%llu/%llu o%llux%llu/%llu tx%llu/%llu",(unsigned long long)gPresent[0].load(),
  (unsigned long long)gPresentForms[0].load(),(unsigned long long)gPresentForms[1].load(),(unsigned long long)gPresentForms[2].load(),
  (unsigned long long)gPresentForms[3].load(),
  (unsigned long long)gPresentGeo[0].load(),(unsigned long long)gPresentGeo[1].load(),(unsigned long long)gPresentGeo[2].load(),
  (unsigned long long)gPresentGeo[3].load(),(unsigned long long)gPresentGeo[4].load(),(unsigned long long)gPresentGeo[5].load(),
  (unsigned long long)gPresentTextures[0].load(),(unsigned long long)gPresentTextures[1].load());
 // What those presents cost a frame, in microseconds, and the runtime's copy census (nmb_copy_census): the copies out of a texture, the
 // ones that took one buffer-image copy (img), one buffer copy a row (rows) or a staging buffer (stg), the regions those recorded, and the
 // last rows copy's row count. rows > 0 with a 1800 x 2880 frame is 2880 regions a frame, 5.5 ms of CPU.
 row("A5 imp%llu tex%llu enc%llu sub%llu wait%llu us | copy %llu img%llu rows%llu reg%llu/%llu r%llu",
  (unsigned long long)(gPresent[0].load()?gPresent[1].load()/gPresent[0].load()/1000:0),
  (unsigned long long)(gPresent[0].load()?gPresent[2].load()/gPresent[0].load()/1000:0),
  (unsigned long long)(gPresent[0].load()?gPresent[3].load()/gPresent[0].load()/1000:0),
  (unsigned long long)(gPresent[0].load()?gPresent[4].load()/gPresent[0].load()/1000:0),
  (unsigned long long)(gPresent[0].load()?gPresent[5].load()/gPresent[0].load()/1000:0),
  (unsigned long long)gCopy[0].load(),(unsigned long long)gCopy[1].load(),(unsigned long long)gCopy[2].load(),
  (unsigned long long)gCopy[4].load(),(unsigned long long)gCopy[5].load(),(unsigned long long)gCopy[6].load());
 // The buffer textures the runtime made: those whose linear image is the buffer's own bytes (alias), those whose is not, and the last one's
 // row pitch against the driver's, which decides it: the driver's linear pitch is Metal's only when it is 256-byte aligned.
 if(!profiling)row("A6 buffer tex alias%llu no%llu pitch %llu/%llu align%llu",(unsigned long long)gBufTex[0].load(),(unsigned long long)gBufTex[1].load(),
  (unsigned long long)gBufTex[2].load(),(unsigned long long)gBufTex[3].load(),(unsigned long long)gBufTex[4].load());
 // The encoder's state calls, by class (count, total and mean), and the sets each class skipped because they changed nothing: a frame's
 // draws and passes come to about 5 ms, so a frame's CPU above that is here, in CA's own work, or in calls not yet counted.
 if(!profiling)row("A7 ps %llu %llums/%lluus sk%llu | vp %llu %llums/%lluus sk%llu | sc %llu %llums/%lluus sk%llu",
  (unsigned long long)gTimeState[kStatePipeline].count.load(),ms(gTimeState[kStatePipeline].total.load()),mean(gTimeState[kStatePipeline]),
  (unsigned long long)gStateSkipped[kStatePipeline].load(),
  (unsigned long long)gTimeState[kStateViewport].count.load(),ms(gTimeState[kStateViewport].total.load()),mean(gTimeState[kStateViewport]),
  (unsigned long long)gStateSkipped[kStateViewport].load(),
  (unsigned long long)gTimeState[kStateScissor].count.load(),ms(gTimeState[kStateScissor].total.load()),mean(gTimeState[kStateScissor]),
  (unsigned long long)gStateSkipped[kStateScissor].load());
 if(!profiling)row("A8 ds %llu %llums/%lluus sk%llu | ra %llu %llums/%lluus sk%llu | mix %llu %llums/%lluus | new %llu %llums",
  (unsigned long long)gTimeState[kStateDepthStencil].count.load(),ms(gTimeState[kStateDepthStencil].total.load()),
  mean(gTimeState[kStateDepthStencil]),(unsigned long long)gStateSkipped[kStateDepthStencil].load(),
  (unsigned long long)gTimeState[kStateRaster].count.load(),ms(gTimeState[kStateRaster].total.load()),mean(gTimeState[kStateRaster]),
  (unsigned long long)gStateSkipped[kStateRaster].load(),
  (unsigned long long)(gTimeState[kStateBlend].count.load()+gTimeState[kStateOther].count.load()),
  ms(gTimeState[kStateBlend].total.load()+gTimeState[kStateOther].total.load()),
  (unsigned long long)((gTimeState[kStateBlend].count.load()+gTimeState[kStateOther].count.load())
   ?(gTimeState[kStateBlend].total.load()+gTimeState[kStateOther].total.load())/(gTimeState[kStateBlend].count.load()+gTimeState[kStateOther].count.load()):0),
  (unsigned long long)gTimeCreate.count.load(),ms(gTimeCreate.total.load()));
 // Where the runtime spends the draw, the close and the commit (its own phases, in microseconds a call): the draw's bind state and
 // variant, the command it builds, the heaps and descriptors, the record; the close's pass end, its shadow copies, its bookkeeping; the
 // commit's state collection, its host steps, its planning, its submit. The counts close each row.
 row("D draw us settle%llu cmd%llu heap%llu rec%llu n%llu",(unsigned long long)gPhase[11].load()?gPhase[0].load()/gPhase[11].load()/1000:0,
  (unsigned long long)(gPhase[0+1].load()/1000/(gPhase[11].load()?gPhase[11].load():1)),
  (unsigned long long)(gPhase[2].load()/1000/(gPhase[11].load()?gPhase[11].load():1)),
  (unsigned long long)(gPhase[3].load()/1000/(gPhase[11].load()?gPhase[11].load():1)),(unsigned long long)gPhase[11].load());
 row("C close us pass%llu sh%llu book%llu n%llu",(unsigned long long)(gPhase[4].load()/1000/(gPhase[12].load()?gPhase[12].load():1)),
  (unsigned long long)(gPhase[5].load()/1000/(gPhase[12].load()?gPhase[12].load():1)),
  (unsigned long long)(gPhase[6].load()/1000/(gPhase[12].load()?gPhase[12].load():1)),(unsigned long long)gPhase[12].load());
 row("B commit us state%llu prep%llu plan%llu sub%llu n%llu",(unsigned long long)(gPhase[7].load()/1000/(gPhase[13].load()?gPhase[13].load():1)),
  (unsigned long long)(gPhase[8].load()/1000/(gPhase[13].load()?gPhase[13].load():1)),
  (unsigned long long)(gPhase[9].load()/1000/(gPhase[13].load()?gPhase[13].load():1)),
  (unsigned long long)(gPhase[10].load()/1000/(gPhase[13].load()?gPhase[13].load():1)),(unsigned long long)gPhase[13].load());
 {
  std::lock_guard<std::mutex> lock(gLongFrameLock);
  const uint32_t count=gLongFrameCount.load();
  char text[96];unsigned used=(unsigned)snprintf(text,sizeof text,"L n%u",count);
  for(uint32_t i=count>4?count-4:0;i<count && used<sizeof text;i++) {
   const LongFrame &f=gLongFrames[i%8];
   used+=(unsigned)snprintf(text+used,sizeof text-used," %llu.%01llu:%llu/%llu",(unsigned long long)(f.at/1000000),
    (unsigned long long)((f.at/100000)%10),(unsigned long long)(f.gapUs/1000),(unsigned long long)(f.cpuUs/1000));
  }
  if(count)row("%s",text);
 }
 row("T pipeline n=%llu total=%llums max=%llums",(unsigned long long)gTimePipeline.count.load(),ms(gTimePipeline.total.load()),ms(gTimePipeline.slowest.load()));
 row("N no-copy %llu dealloc %llu/%llu surface %llu/%llu | region n=%llu %llums/%lluus %lluMB",(unsigned long long)gNoCopyAll.load(),
  (unsigned long long)gNoCopyReleased.load(),(unsigned long long)gNoCopyMade.load(),
  (unsigned long long)gSurfaceImportsReleased.load(),(unsigned long long)gSurfaceImportsMade.load(),
  (unsigned long long)gTimeRegion.count.load(),ms(gTimeRegion.total.load()),mean(gTimeRegion),
  (unsigned long long)(gRegionBytes.load()>>20));
 // The runtime's VkPipeline makings (the creation log, METAL2VULKAN_LOG_PIPELINES; they happen at draws, not at the state's creation) and
 // its shader cache: where the driver's compile time goes.
 uint64_t work[10]={};
 if(gDevice && nmb_device_work_stats(gDevice,work)) {
  row("V vkpipe n=%llu total=%llums max=%llums waited=%llums failed=%llu",
   (unsigned long long)work[0],ms(work[1]),ms(work[2]),ms(work[3]),(unsigned long long)work[4]);
  row("V cache hit%llu miss%llu wr%llu | msg i%llu w%llu",(unsigned long long)work[7],(unsigned long long)work[8],(unsigned long long)work[9],(unsigned long long)work[5],
   (unsigned long long)work[6]);
  // Where textures over buffers and surfaces cost copies: linear images at Metal's pitch made, refused, refused again; shadowed textures,
  // their copies in, texels copied back; passes over linear images in place and through copies.
  uint64_t tc[8]={};
  if(nmb_device_texture_counts(gDevice,tc))
   row("L lin %llu/%llu/%llu sh %llu cp %llu bk %llu pass %llu/%llu",(unsigned long long)tc[0],(unsigned long long)tc[1],(unsigned long long)tc[2],
    (unsigned long long)tc[3],(unsigned long long)tc[4],(unsigned long long)tc[5],(unsigned long long)tc[6],(unsigned long long)tc[7]);
  char warning[200];size_t n=nmb_last_warning(warning,sizeof warning);
  if(n){row("W %.86s",warning);if(n>86)row("Wb %.86s",warning+86);}
 }
 {
  std::lock_guard<std::mutex> lock(gProblemLock);
  char formats[160];native_bridge_metal_formats_locked(formats,sizeof formats);if(!profiling)row("F %.86s",formats);
  if(!profiling)row("PR compiling=%u libs=%u/%u made=%u rate=%u",native_prime_compiling,native_prime_libraries_done,native_prime_libraries_total,
   native_prime_made,native_prime_rate);
  const SurfaceRefusal &r=gSurfaceLastRefusal;
  if(r.step)row("SR %u %08x %c%c%c%c pl%u/%u o%llx rb%llu a%llx %llux%llu pf%u",r.step,r.format,(char)(r.format>>24),(char)(r.format>>16),(char)(r.format>>8),
   (char)r.format,r.plane,r.planes,(unsigned long long)r.offset,(unsigned long long)r.row,(unsigned long long)r.allocation,(unsigned long long)r.width,
   (unsigned long long)r.height,r.pixelFormat);
  if(!profiling) {
   row("P0 %.86s",gLastProblems[0]);row("P0b %.86s",strlen(gLastProblems[0])>86?gLastProblems[0]+86:"");
   row("P1 %.86s",gLastProblems[1]);row("P1b %.86s",strlen(gLastProblems[1])>86?gLastProblems[1]+86:"");
  }
 }
 {
  std::lock_guard<std::mutex> lock(gWaitSiteLock);
  for(unsigned i=0;i<2 && gWaitSites[i].count && !profiling;i++) {
   const WaitSite &w=gWaitSites[i];
   row("C%u x%llu %c%lx %c%lx %c%lx %c%lx",i,(unsigned long long)w.count,w.image[0]?w.image[0]:'-',(unsigned long)w.offset[0],
    w.image[1]?w.image[1]:'-',(unsigned long)w.offset[1],w.image[2]?w.image[2]:'-',(unsigned long)w.offset[2],
    w.image[3]?w.image[3]:'-',(unsigned long)w.offset[3]);
  }
 }
 // The command buffer waits by caller (WC) and the frame intervals' attribution (FA). The K ring stays unprinted: the W5, H and X
 // rows say what it did, in fewer of the 48.
 if(!profiling) {
  std::lock_guard<std::mutex> lock(gCommitLock);
  for(unsigned i=0;i<8;i++) {
   const WaitCaller &w=gWaitCallers[i];
   if(!w.n.load())continue;
   row("WC%u lr=%llx n%u us%llu t=%.23s s=%.27s",i,(unsigned long long)w.lr.load(),w.n.load(),(unsigned long long)w.us.load(),w.thread,w.symbol);
  }
  for(unsigned i=0;i<4;i++) {
   const FrameAttribution::Bin &b=gAttribution.bins[i];
   if(!b.n)continue;
   row("FA%u n%u I%llu pg%llu enc%llu res%lld kpx%llu hv%u",i,b.n,(unsigned long long)(b.sumI/b.n),(unsigned long long)(b.sumPg/b.n),
    (unsigned long long)(b.sumEnc/b.n),(long long)(b.sumRes/(int64_t)b.n),(unsigned long long)(b.sumKpx/b.n),b.heavy);
  }
 }
 {
  const unsigned have=profiling?0:gPassShapeCount.load()<3?gPassShapeCount.load():3;
  for(unsigned i=0;i<have;i++)
   row("Y pass %ux%u f%u s%u n%llu",gPassShapes[i].width,gPassShapes[i].height,gPassShapes[i].format,gPassShapes[i].samples,
    (unsigned long long)gPassShapes[i].count.load());
 }
 // The benchmark's rows (native_bridge_metal_benchmark) are for the host tools, which read the table whole.
 {std::lock_guard<std::mutex> lock(gBenchLock);for(unsigned r=0;r<4;r++)if(gBench[r][0])row("%.90s",gBench[r]);}
 // The render passes' GPU time by area and mode (Turnip's tu_pipa_pass_dump, summed since the process began), most first.
 if(turnipPasses) {
  static char passes[1200];static unsigned passBytes;
  {char fresh[1200];unsigned n=turnipPasses(fresh,sizeof fresh);if(n){memcpy(passes,fresh,n+1);passBytes=n;}}
  for(char *line=passes;line<passes+passBytes;) {
   char *eol=strchr(line,'\n');if(!eol)break;
   row("%.*s",(int)(eol-line),line);
   line=eol+1;
  }
 }
 // Turnip's kernel calls (tu_knl_darwin.cc tu_pipa_knl_dump): per ABI method the calls and their milliseconds, then the BOs made by
 // name; the render thread spent a fifth of its CPU in IOConnectCallStructMethod in the App Library (2026-10-25 profile).
 if(turnipKernel) {
  char kernel[1600];unsigned n=turnipKernel(kernel,sizeof kernel),names=0;
  for(char *line=kernel;line<kernel+n;) {
   char *eol=strchr(line,'\n');if(!eol)break;*eol=0;
   // The per-method row whole (it runs over two or three rows), the BO names only as far as the first four: the profile rows follow.
   if(*line && (strncmp(line,"KB",2) || names++<4))
    for(size_t at=0,length=strlen(line);at<length;at+=86)row(at?"KM+ %.86s":"%.86s",line+at);
   line=eol+1;
  }
 }
 // The render thread's profile (summarizeProfile), as many of its rows as the 48 leave, the next ones at the next dump; ahead of Turnip's notes.
 {
  std::lock_guard<std::mutex> lock(gProfileRowsLock);
  static size_t next;
  const size_t total=gProfileRows.size();
  size_t sent=0;
  for(;sent<total && rows<48;sent++)row("%s",gProfileRows[(next+sent)%total].c_str());
  if(total)next=(next+sent)%total;
 }
 // Turnip's own render pass notes (tu_util.cc, exported as tu_pipa_note_dump): Turnip has no log route in the render server, and its
 // pass geometry (area, format, samples, mode, tile size, GMEM offset) is what tells GMEM's 4-row slices apart (2026-10-24). Last, so the
 // rows above are never pushed out of the kernel's 48 (the corner work that needs these can move them up).
 if(turnipNotes) {
  char notes[32*120+32];unsigned n=turnipNotes(notes,sizeof notes);
  for(char *line=notes;line<notes+n;) {
   char *eol=strchr(line,'\n');if(!eol)break;*eol=0;
   if(*line)row("Z %s",line);
   line=eol+1;
  }
 }
 return at;
}

// A client process's compile cost so far, one line for its kernel row (NativeAdrenoMetalPlugin.mm clientReport): seconds since its device
// opened, the Metal pipeline states it made (count, ms, slowest ms), the runtime's VkPipelines, made at draws (count, ms), its shader
// cache's hits and misses, and the libraries and functions made (count, ms). Settings took seconds to show its content the first time
// after a boot (2026-10-25), and the render server's table sees no other process's compiles.
extern "C" size_t native_bridge_metal_client_brief(char *out,size_t size) {
 auto ms=[](uint64_t us){return (unsigned long long)(us/1000);};
 uint64_t work[10]={};
 if(gDevice)nmb_device_work_stats(gDevice,work);
 const int n=snprintf(out,size,"t%llu p%llu/%llu/%llu v%llu/%llu c%llu/%llu l%llu/%llu f%llu/%llu",
  gJournalBegan?(unsigned long long)((nowUs()-gJournalBegan)/1000000):0ULL,(unsigned long long)gTimePipeline.count.load(),
  ms(gTimePipeline.total.load()),ms(gTimePipeline.slowest.load()),(unsigned long long)work[0],ms(work[1]),(unsigned long long)work[7],
  (unsigned long long)work[8],(unsigned long long)gTimeLibrary.count.load(),ms(gTimeLibrary.total.load()),
  (unsigned long long)gTimeFunction.count.load(),ms(gTimeFunction.total.load()));
 return n>0?(size_t)n:0;
}

extern "C" void native_bridge_metal_frame_cost(uint64_t *frames,uint64_t *cpu_us) {
 if(frames)*frames=gFrameCostN.load();
 if(cpu_us)*cpu_us=gFrameCpuUs.load();
}

// The scanout service's cost so far (native_bridge_metal_present_cost): frames, the ns of import, texture, encode, submit and wait, the copy
// forms taken, and the textures made and reused.
extern "C" void native_bridge_metal_present_cost(const uint64_t *values) {
 for(unsigned i=0;i<6;i++)gPresent[i].store(values[i]);
 gPresentForms[0].store(values[6]);gPresentForms[1].store(values[7]);gPresentForms[2].store(values[8]);
 gPresentTextures[0].store(values[9]);gPresentTextures[1].store(values[10]);
 for(unsigned i=0;i<6;i++)gPresentGeo[i].store(values[11+i]);
 gPresentForms[3].store(values[17]);
}

extern "C" uint64_t native_bridge_metal_first_problem(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 if(size)strlcpy(out,gFirstProblem,size);
 return gProblems.load();
}

extern "C" size_t native_bridge_metal_formats(char *out,size_t size) {
 std::lock_guard<std::mutex> lock(gProblemLock);
 return native_bridge_metal_formats_locked(out,size);
}
