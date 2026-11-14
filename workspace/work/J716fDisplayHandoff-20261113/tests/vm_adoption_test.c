// The generator inserts the exact patched XNU function through this include.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
typedef uint32_t ppnum_t;
typedef int boolean_t;
// Ordinary ARM64 XNU exposes a runtime int, not a constant PAGE_SHIFT.
int PAGE_SHIFT_CONST=12;
#define PAGE_SHIFT PAGE_SHIFT_CONST
enum { TRUE=1, FALSE=0 };
typedef struct { unsigned vmp_fictitious; } vm_page;
typedef vm_page *vm_page_t;
static vm_page page;
static jmp_buf rejected;
static unsigned checks, grabs, releases, clears, locks, unlocks, panics;
static unsigned vm_page_pages;
static ppnum_t first_seen, last_seen;
static _Noreturn void panic(const char *format, ...) {
    (void)format; ++panics; longjmp(rejected,1);
}
static vm_page_t vm_page_grab_fictitious_common(ppnum_t p, boolean_t value) {
    if (!value) abort();
    if (!grabs) first_seen=p;
    last_seen=p; ++grabs; page.vmp_fictitious=1; return &page;
}
static void pmap_clear_noencrypt(ppnum_t p) { (void)p; ++clears; }
static void vm_free_page_lock(void) { ++locks; }
static void vm_free_page_unlock(void) { ++unlocks; }
static void vm_page_release(vm_page_t p, boolean_t locked) {
    if (p->vmp_fictitious || locked) abort(); ++releases;
}
#include "vm_page_create_exact.inc"
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %u: %s\n",__LINE__,#x); abort(); } } while (0)
static void run(ppnum_t start, ppnum_t end, int must_panic, unsigned count) {
    grabs=releases=clears=locks=unlocks=panics=0; vm_page_pages=7;
    first_seen=last_seen=0;
    int did_panic=setjmp(rejected);
    if (!did_panic) vm_page_create(start,end);
    CHECK((did_panic!=0)==must_panic);
    CHECK(panics==(unsigned)must_panic);
    CHECK(grabs==count && releases==count && clears==count && locks==count && unlocks==count);
    CHECK(vm_page_pages==7+count);
    if (count) { CHECK(first_seen==start); CHECK(last_seen==end-1); }
}
int main(void) {
    // Exact endpoints do not overlap; empty/reversed requests do no work.
    run(0x9bfff,0x9c000,0,1); run(0x9e400,0x9e401,0,1);
    run(0x9c000,0x9c000,0,0); run(0x9e400,0x9e400,0,0);
    run(0x9c001,0x9c000,0,0); run(0,0,0,0); run(UINT32_MAX,UINT32_MAX,0,0);
    run(UINT32_MAX-2,UINT32_MAX,0,2);
    run(0,0x9c001,1,0); run(0x9e3ff,UINT32_MAX,1,0);
    run(0x9bfff,0x9e401,1,0); run(0x9c000,0x9e400,1,0);
    for (ppnum_t p=0x9c000;p<0x9e400;++p) run(p,p+1,1,0);
    // Leading safe pages must not be partly committed before rejection.
    for (ppnum_t n=1;n<=128;++n) {
        run(0x9c000-n,0x9c001,1,0);
        run(0x9e3ff,0x9e400+n,1,0);
    }
    for (int shift=0;shift<=64;++shift) if (shift!=12) {
        PAGE_SHIFT_CONST=shift;
        run(0x100,0x101,1,0); // Reject before any address shift or page release.
    }
    PAGE_SHIFT_CONST=12;
    printf("vm adoption checks=%u: PASS (exact patched vm_page_create; no partial release)\n",checks);
    return 0;
}
