// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included in arm_vm_init.c, where static table allocators are in scope.
#include "NativeLog.h"
#include "NativeMap.h"
#include "NativeHooks.h"
#include "NativeCandidate.h"
#if ARM_PGBYTES != 4096 || XNU_MONITOR || __ARM_KERNEL_PROTECT__ || KASAN
#error J716F logger requires reviewed 4K non-PPL non-KPTI non-KASAN single-core build
#endif
#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR)
#error J716F logger mapping is not qualified with hardware text lockdown
#endif
// File-backed, dedicated pages within permanent kernel __DATA. These bytes
// reserve VIRTUAL address ownership; the dummy DRAM backing is never freed.
// Only this section is rebound to reserved RAM, before either root is active.
__attribute__((section("__DATA,__j716f_log"), aligned(4096), used))
unsigned char j716f_native_window[JL_CONSOLE_BYTES] = {0x7f};
static JL64 j716f_boot_l3[2][512] __attribute__((aligned(4096))) = {{1}};
static unsigned int j716f_boot_pages;
static volatile unsigned int j716f_native_ready;
static volatile unsigned int j716f_native_fault_busy;
static const JL64 j716f_candidate[4] __attribute__((used)) = JL_CANDIDATE_WORDS;
extern volatile uint64_t armcpu_timer_interrupts;

static void jl_barrier(void) { __asm__ volatile ("dsb sy" ::: "memory"); }
static JL64 jl_counter(void) {
  JL64 v; __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v;
}
static void jl_registers(JL64 data[10], JL64 a, JL64 b, JL64 c) {
  data[0]=a; data[1]=b; data[2]=c;
  __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(data[3]));
  __asm__ volatile ("mrs %0, ttbr1_el1" : "=r"(data[4]));
  __asm__ volatile ("mov %0, sp" : "=r"(data[5]));
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(data[6]));
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(data[7]));
  data[8]=(JL64)j716f_native_window;
  __asm__ volatile ("mrs %0, daif" : "=r"(data[9]));
}
static void jl_physical_event(unsigned int slot, JL64 a, JL64 b, JL64 c) {
  JL64 data[10];
  volatile JLHeader *h=jl_find((volatile void *)JL_CONSOLE_PA,j716f_candidate);
  jl_registers(data,a,b,c); (void)jl_write(h,slot,jl_counter(),data,jl_barrier);
}

__attribute__((no_stack_protector)) int j716f_native_begin(JL64 boot_args) {
  JL64 el,mpidr;
  volatile JLHeader *h;
  __asm__ volatile ("mrs %0, CurrentEL; mrs %1, mpidr_el1" : "=r"(el), "=r"(mpidr));
  if (el!=4 || (mpidr&0xff00ffffffULL)) return 0;
  h=jl_find((volatile void *)JL_CONSOLE_PA,j716f_candidate);
  if (!h || h->state!=JL_ARMED) return 0;
  h->state=JL_USED; jl_barrier();
  jl_physical_event(0,boot_args,0,0);
  return jl_valid(h,jl_record(h,0),0);
}
static JL64 *jl_boot_resolve(JL64 pa) { return (JL64 *)pa; }
static JL64 *jl_boot_allocate(JL64 *pa) {
  JL64 *p;
  if(j716f_boot_pages>=2) return 0;
  p=j716f_boot_l3[j716f_boot_pages++]; *pa=(JL64)p; return p;
}
__attribute__((no_stack_protector)) int j716f_native_bootstrap(JL64 root, JL64 delta) {
  JLMapOps ops={jl_boot_resolve,jl_boot_allocate};
  JL64 window=(JL64)j716f_native_window;
  JL64 attrs=ARM_PTE_TYPE_VALID|ARM_PTE_AF|ARM_PTE_SH(SH_OUTER_MEMORY)|
    ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DISABLE)|ARM_PTE_NX|ARM_PTE_PNX;
  if (!jl_map_window((JL64 *)root,window+delta,window,&ops,attrs)) {
    jl_physical_event(43,root,window,delta); return 0;
  }
  jl_barrier(); j716f_native_ready=1;
  jl_physical_event(1,root,window+delta,attrs); return 1;
}
void j716f_native_early_fault(JL64 esr, JL64 far, JL64 elr, JL64 spsr) {
  if(j716f_native_fault_busy) return;
  j716f_native_fault_busy=1; jl_barrier();
  JL64 data[10];
  volatile JLHeader *h=jl_find((volatile void *)JL_CONSOLE_PA,j716f_candidate);
  jl_registers(data,esr,far,elr); data[8]=spsr;
  (void)jl_write(h,40,jl_counter(),data,jl_barrier);
}

void j716f_native_event(unsigned int slot, JL64 a, JL64 b, JL64 c) {
  JL64 data[10],daif;
  volatile JLHeader *h;
  if(!j716f_native_ready || slot>=JL_SLOTS) return;
  if(slot==41 || slot==42) {
    if(j716f_native_fault_busy) return;
    j716f_native_fault_busy=1;
  }
  __asm__ volatile ("mrs %0, daif; msr daifset, #3; isb" : "=r"(daif) :: "memory");
  h=jl_find(j716f_native_window,j716f_candidate);
  jl_registers(data,a,b,c); data[9]=daif;
  (void)jl_write(h,slot,jl_counter(),data,jl_barrier);
  __asm__ volatile ("msr daif, %0; isb" :: "r"(daif) : "memory");
}
void j716f_native_irq(unsigned int phase, unsigned int logical,
    const volatile JL64 *trace, const JL64 *r) {
  JL64 data[10],n=trace[6];
  JL32 sample,slot;
  volatile JLHeader *h;
  if(!j716f_native_ready || logical || phase<1 || phase>3) return;
  if(n==1) sample=0; else if(n==2) sample=1; else if(n==8) sample=2; else return;
  slot=24+sample*3+phase-1;
  h=jl_find(j716f_native_window,j716f_candidate);
  data[0]=r[9]; data[1]=trace[6]; data[2]=trace[7]; data[3]=trace[8];
  data[4]=r[4]; data[5]=r[6]; data[6]=r[7]; data[7]=r[8];
  data[8]=(r[10]<<32)|r[11]; data[9]=r[12];
  (void)jl_write(h,slot,r[5],data,jl_barrier);
}
void j716f_native_sched(unsigned int phase, JL64 from, JL64 to) {
  JL64 n=armcpu_timer_interrupts;
  if(phase<2) j716f_native_event(50+phase,from,to,n);
  if(phase<2 && n>=8) j716f_native_event(52+phase,from,to,n);
}

static JL64 *jl_vm_resolve(JL64 pa) { return (JL64 *)phystokv(pa); }
static JL64 *jl_vm_allocate(JL64 *pa) {
  vm_offset_t p=alloc_ptpage(TRUE); *pa=kvtophys(p); return (JL64 *)p;
}
static void j716f_native_vm_map(void) {
  JLMapOps ops={jl_vm_resolve,jl_vm_allocate};
  vm_offset_t va=(vm_offset_t)j716f_native_window;
  JL64 attrs=ARM_PTE_TYPE_VALID|ARM_PTE_AF|ARM_PTE_SH(SH_OUTER_MEMORY)|
    ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DISABLE)|ARM_PTE_NX|ARM_PTE_PNX;
  // Protect ownership and attributes before publishing the NEW root. No pmap
  // heap allocation owns these VAs; __DATA survives prot_finalize/BOOTDATA free.
  if(kva_active || va<segDATAB || va+JL_CONSOLE_BYTES>segDATAB+segSizeDATA ||
     (va&4095) || gPhysBase<JL_CONSOLE_PA+JL_CONSOLE_BYTES ||
     !jl_map_window((JL64 *)cpu_tte,va,va-gVirtBase+gPhysBase,&ops,attrs)) {
    j716f_native_event(44,va,cpu_ttep,gPhysBase);
    panic("J716F native log mapping refused");
  }
  jl_barrier();
  j716f_native_event(4,va,cpu_ttep,attrs);
}
