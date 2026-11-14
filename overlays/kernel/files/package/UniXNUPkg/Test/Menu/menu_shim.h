/* Minimal fake UEFI surface for the actual Menu.c loop host tests. */
#ifndef MENU_SHIM_H
#define MENU_SHIM_H

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

#define STATIC static
#define VOID void
#define IN
#define CONST const
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;
typedef uintptr_t UINTN;
typedef intptr_t INTN;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef char CHAR8;
typedef wchar_t CHAR16;
typedef int EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
#define EFI_SUCCESS 0
#define EFI_NOT_READY 1
#define EFI_DEVICE_ERROR 2
#define EFI_ERROR(Status) ((Status) != EFI_SUCCESS)
#define EFI_TIMER_PERIOD_MILLISECONDS(Ms) ((uint64_t)(Ms) * 10000)
#define ByProtocol 0
#define EVT_TIMER 1
#define TPL_CALLBACK 8
#define TimerPeriodic 1
#define TimerCancel 0
#define SCAN_UP 1
#define SCAN_DOWN 2
#define SCAN_RIGHT 3
#define SCAN_HOME 5
#define SCAN_END 6
#define SCAN_ESC 0x17
#define CHAR_CARRIAGE_RETURN L'\r'
#define CHAR_LINEFEED L'\n'
#define PcdUniXNUDiagnosticMenuHold 6

typedef struct {
  unsigned ScanCode;
  CHAR16 UnicodeChar;
} EFI_INPUT_KEY;
typedef struct SIMPLE_INPUT SIMPLE_INPUT;
struct SIMPLE_INPUT {
  EFI_STATUS (*ReadKeyStroke)(SIMPLE_INPUT *, EFI_INPUT_KEY *);
  EFI_EVENT WaitForKey;
};
typedef struct {
  SIMPLE_INPUT *ConIn;
} SYSTEM_TABLE;
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(int, const void *, void *, UINTN *, EFI_HANDLE **);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE, const void *, void **);
  EFI_STATUS (*CreateEvent)(int, int, void *, void *, EFI_EVENT *);
  EFI_STATUS (*SetTimer)(EFI_EVENT, int, uint64_t);
  EFI_STATUS (*WaitForEvent)(UINTN, EFI_EVENT *, UINTN *);
  EFI_STATUS (*CloseEvent)(EFI_EVENT);
  EFI_STATUS (*Stall)(UINTN);
} BOOT_SERVICES;
typedef struct {
  CHAR8 *Id;
  CHAR16 *Title;
} UNIXNU_ENTRY;
typedef struct {
  UNIXNU_ENTRY *Entries;
  UINTN EntryCount;
  UINTN DefaultIndex;
  INTN Timeout;
  UINTN FallbackTimeout;
  UINTN RotateSeconds;
} UNIXNU_CONFIG;
typedef struct {
  CHAR16 Notice[160];
} UNIXNU_UI;

extern SYSTEM_TABLE *gST;
extern BOOT_SERVICES *gBS;
extern int gEfiSimpleTextInProtocolGuid;
extern int gEfiDevicePathProtocolGuid;
BOOLEAN PcdGetBool(int Token);
void FreePool(void *Memory);
void UnicodeSPrint(CHAR16 *Buffer, UINTN Size, const CHAR16 *Format, ...);
void UniXNUUiDrawStatus(UNIXNU_UI *Ui, UINTN Line, const CHAR16 *Text);
void UniXNUUiDrawFrame(UNIXNU_UI *Ui, UNIXNU_CONFIG *Config);
void UniXNUUiDrawEntry(UNIXNU_UI *Ui, UNIXNU_CONFIG *Config, UINTN Index, BOOLEAN Selected);
void UniXNULog(const CHAR8 *Format, ...);

#endif
