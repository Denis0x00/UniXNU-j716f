// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the production manual loop with mocked UEFI, not a device test.
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>
#define IN
#define VOID void
#define TRUE 1
#define FALSE 0
#define SCAN_UP 1
#define SCAN_DOWN 2
#define CHAR_CARRIAGE_RETURN 13
#define EFI_ERROR(s) ((s) != 0)
typedef size_t UINTN;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef struct { unsigned ScanCode, UnicodeChar; } EFI_INPUT_KEY;
typedef struct { wchar_t Notice[2]; } UNIXNU_UI;
typedef struct { UINTN DefaultIndex, EntryCount; } UNIXNU_CONFIG;
typedef struct Input Input;
struct Input { void *WaitForKey; EFI_STATUS (*ReadKeyStroke)(Input *, EFI_INPUT_KEY *); };
typedef struct { Input *ConIn; } System;
typedef struct {
  EFI_STATUS (*WaitForEvent)(UINTN, void **, UINTN *);
  void (*Stall)(UINTN);
} Services;
static jmp_buf stop;
static EFI_INPUT_KEY keys[8];
static unsigned cursor, count, read_failures;
static int wait_error;
static EFI_STATUS read_key(Input *input, EFI_INPUT_KEY *key) {
  (void)input;
  if (read_failures) { --read_failures; return 1; }
  assert(cursor < count); *key=keys[cursor++]; return 0;
}
static EFI_STATUS wait_key(UINTN n, void **event, UINTN *signaled) {
  (void)event; *signaled=0; assert(n==1);
  if (wait_error) return 1;
  if (cursor==count) longjmp(stop,3); // would block awaiting human input
  return 0;
}
static void stall(UINTN delay) { assert(delay==100000); longjmp(stop,2); }
static Input input={NULL,read_key};
static System system_table={&input}, *gST=&system_table;
static Services services={wait_key,stall}, *gBS=&services;
static void UniXNUUiDrawFrame(UNIXNU_UI *u, UNIXNU_CONFIG *c) { (void)u; (void)c; }
static void UniXNUUiDrawEntry(UNIXNU_UI *u, UNIXNU_CONFIG *c, UINTN i, BOOLEAN b) {
  (void)u; (void)b; assert(i<c->EntryCount);
}
static void UniXNUUiDrawStatus(UNIXNU_UI *u, UINTN row, const wchar_t *text) {
  (void)u; (void)row; (void)text;
}
#include "ManualMenu.inc"
static void check(UNIXNU_CONFIG config, int outcome, UINTN selected) {
  UNIXNU_UI ui={{0}};
  int signal=setjmp(stop);
  if (!signal) {
    UINTN result=UniXNUMenuRun(&ui,&config,TRUE);
    assert(outcome==1 && result==selected);
  } else assert(signal==outcome);
  cursor=count=read_failures=0; wait_error=0; gST->ConIn=&input;
}
int main(void) {
  UNIXNU_CONFIG config={0,4};
  check(config,3,0); // silence does not launch default reader or native
  gST->ConIn=NULL; check(config,2,0);
  check((UNIXNU_CONFIG){0,0},2,0);
  check((UNIXNU_CONFIG){4,4},2,0);
  wait_error=1; check(config,2,0);
  keys[0]=(EFI_INPUT_KEY){0,13}; count=1; check(config,1,0);
  keys[0]=(EFI_INPUT_KEY){2,0}; keys[1]=keys[0];
  keys[2]=(EFI_INPUT_KEY){0,13}; count=3; check(config,1,2);
  keys[0]=(EFI_INPUT_KEY){1,0}; keys[1]=(EFI_INPUT_KEY){0,13};
  count=2; check(config,1,3);
  keys[0]=(EFI_INPUT_KEY){2,0}; keys[1]=(EFI_INPUT_KEY){0,13};
  count=2; check((UNIXNU_CONFIG){3,4},1,0);
  keys[0]=(EFI_INPUT_KEY){23,0}; keys[1]=(EFI_INPUT_KEY){0,'x'};
  count=2; check(config,3,0);
  keys[0]=(EFI_INPUT_KEY){0,13}; count=1; read_failures=2; check(config,1,0);
  puts("PASS: 11 manual-menu cases; only explicit Enter returns a selection");
  return 0;
}
