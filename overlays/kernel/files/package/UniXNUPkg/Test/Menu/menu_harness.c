/* Tests include the unmodified production menu functions, with only Internal.h
   replaced by menu_shim.h by the runner. No copy of the menu algorithm lives here. */
#include "menu_shim.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  unsigned Tick;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
} KEY_EVENT;

static jmp_buf Stop;
static BOOLEAN Hold, Physical;
static unsigned Tick, Limit, ConsoleAt, ConsoleGoneAt;
static unsigned Stalls, Waits, Creates, Sets, Closes, Reads, Draws;
static unsigned Locates, Protocols;
static unsigned Frames, StatusDraws[3];
static unsigned KeyCount, KeyNext, Passed;
static UINTN Highlight;
static BOOLEAN SawHoldLabel, SawSelectLabel;
static EFI_STATUS CreateResult, WaitResult, EnumerationResult;
static KEY_EVENT Keys[8];
static UNIXNU_UI Ui;
static UNIXNU_ENTRY Entries[] = {
  {"xnu", L"XNU"}, {"exit", L"Return to firmware"}, {"tools", L"Tools"}
};
static UNIXNU_CONFIG Config;
static EFI_STATUS ReadKey(SIMPLE_INPUT *Self, EFI_INPUT_KEY *Key);
static SIMPLE_INPUT Input = {ReadKey, (void *)2};
static SYSTEM_TABLE Table;
SYSTEM_TABLE *gST = &Table;
int gEfiSimpleTextInProtocolGuid;
int gEfiDevicePathProtocolGuid;

BOOLEAN PcdGetBool(int Token) {
  assert(Token == PcdUniXNUDiagnosticMenuHold);
  return Hold;
}
void FreePool(void *Memory) { free(Memory); }
void UnicodeSPrint(CHAR16 *Buffer, UINTN Size, const CHAR16 *Format, ...) {
  /* Preserve status text for call-contract checks; no pixel rendering here. */
  assert(Size >= sizeof(CHAR16));
  wcsncpy(Buffer, Format, Size / sizeof(CHAR16) - 1);
  Buffer[Size / sizeof(CHAR16) - 1] = 0;
}
void UniXNULog(const CHAR8 *Format, ...) { (void)Format; }
void UniXNUUiDrawStatus(UNIXNU_UI *UnusedUi, UINTN Line, const CHAR16 *Text) {
  assert(Line < 3 && Frames == 1 && Draws >= Config.EntryCount);
  StatusDraws[Line]++;
  if (wcsstr(Text, L"Diagnostic hold") != NULL) SawHoldLabel = TRUE;
  if (wcscmp(Text, L"Select an entry") == 0) SawSelectLabel = TRUE;
}
void UniXNUUiDrawFrame(UNIXNU_UI *UnusedUi, UNIXNU_CONFIG *UnusedConfig) {
  Frames++;
  assert(Frames == 1 && Draws == 0);
}
void UniXNUUiDrawEntry(UNIXNU_UI *UnusedUi, UNIXNU_CONFIG *UnusedConfig,
                      UINTN Index, BOOLEAN Selected) {
  assert(Index < Config.EntryCount);
  Draws++;
  if (Selected) Highlight = Index;
}
static EFI_STATUS Locate(int Search, const void *Protocol, void *Unused,
                         UINTN *Count, EFI_HANDLE **Handles) {
  Locates++;
  assert(!Hold);
  if (EnumerationResult != EFI_SUCCESS) return EnumerationResult;
  *Count = 1;
  *Handles = malloc(sizeof(**Handles));
  assert(*Handles != NULL);
  (*Handles)[0] = (void *)3;
  return EFI_SUCCESS;
}
static EFI_STATUS Protocol(EFI_HANDLE Handle, const void *Guid, void **Path) {
  Protocols++;
  assert(!Hold);
  *Path = NULL;
  return Physical ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
static EFI_STATUS Create(int Type, int Tpl, void *Notify, void *Context, EFI_EVENT *Event) {
  Creates++;
  assert(!Hold);
  if (CreateResult != EFI_SUCCESS) return CreateResult;
  *Event = (void *)1;
  return EFI_SUCCESS;
}
static EFI_STATUS Set(EFI_EVENT Event, int Type, uint64_t Period) {
  assert(Event == (void *)1);
  assert(!Hold);
  Sets++;
  return EFI_SUCCESS;
}
static void Advance(void) {
  Tick++;
  if ((ConsoleAt != 0) && (Tick >= ConsoleAt)) Table.ConIn = &Input;
  if ((ConsoleGoneAt != 0) && (Tick >= ConsoleGoneAt)) Table.ConIn = NULL;
  if (Tick > Limit) longjmp(Stop, 1);
}
static EFI_STATUS Wait(UINTN Count, EFI_EVENT *Events, UINTN *Signaled) {
  assert(!Hold);
  assert(Count >= 1 && Events[0] == (void *)1);
  Waits++;
  Advance();
  if (WaitResult != EFI_SUCCESS) return WaitResult;
  *Signaled = (Count > 1 && KeyNext < KeyCount && Keys[KeyNext].Tick <= Tick) ? 1 : 0;
  return EFI_SUCCESS;
}
static EFI_STATUS Close(EFI_EVENT Event) {
  assert(!Hold && Event == (void *)1);
  Closes++;
  return EFI_SUCCESS;
}
static EFI_STATUS Stall(UINTN Microseconds) {
  assert(Hold && Microseconds == 100000);
  if (Stalls == 0) {
    /* First frame is complete before polling, with no line-0 clear/text call. */
    assert(Frames == 1 && Draws == Config.EntryCount);
    assert(StatusDraws[0] == 0 && StatusDraws[1] == 1 && SawHoldLabel);
    assert(Highlight == Config.DefaultIndex);
  }
  Stalls++;
  Advance();
  return EFI_SUCCESS;
}
static BOOT_SERVICES Services = {Locate, Protocol, Create, Set, Wait, Close, Stall};
BOOT_SERVICES *gBS = &Services;
static EFI_STATUS ReadKey(SIMPLE_INPUT *Self, EFI_INPUT_KEY *Key) {
  Reads++;
  assert(Self == &Input);
  if (KeyNext < KeyCount && Keys[KeyNext].Tick <= Tick) {
    KEY_EVENT Event = Keys[KeyNext++];
    *Key = Event.Key;
    return Event.Status;
  }
  return EFI_NOT_READY;
}

#include "menu_under_test.c"

static void Reset(BOOLEAN Diagnostic, BOOLEAN Console, BOOLEAN RealInput) {
  memset(&Ui, 0, sizeof(Ui));
  memset(Keys, 0, sizeof(Keys));
  Hold = Diagnostic;
  Physical = RealInput;
  Table.ConIn = Console ? &Input : NULL;
  Input.ReadKeyStroke = ReadKey;
  Input.WaitForKey = (void *)2;
  Config = (UNIXNU_CONFIG){Entries, 3, 1, 10, 1, 1};
  Tick = ConsoleAt = ConsoleGoneAt = 0;
  Limit = 120;
  Stalls = Waits = Creates = Sets = Closes = Reads = Draws = 0;
  Locates = Protocols = 0;
  Frames = 0;
  memset(StatusDraws, 0, sizeof(StatusDraws));
  KeyCount = KeyNext = 0;
  Highlight = 99;
  SawHoldLabel = SawSelectLabel = FALSE;
  CreateResult = WaitResult = EnumerationResult = EFI_SUCCESS;
}
static void KeyAt(unsigned At, unsigned Scan, CHAR16 Character) {
  assert(KeyCount < sizeof(Keys) / sizeof(Keys[0]));
  Keys[KeyCount++] = (KEY_EVENT){At, {Scan, Character}, EFI_SUCCESS};
}
static void ErrorAt(unsigned At) {
  KeyAt(At, 0, 0);
  Keys[KeyCount - 1].Status = EFI_DEVICE_ERROR;
}
static void Check(const char *Name, BOOLEAN Countdown, INTN Expected) {
  if (setjmp(Stop) == 0) {
    UINTN Returned = UniXNUMenuRun(&Ui, &Config, Countdown);
    if (Expected < 0 || Returned != (UINTN)Expected) {
      fprintf(stderr, "FAIL %s: returned %zu, expected %zd\n", Name,
              (size_t)Returned, (ptrdiff_t)Expected);
      abort();
    }
  } else {
    if (Expected >= 0) {
      fprintf(stderr, "FAIL %s: menu did not return expected %zd\n", Name, (ptrdiff_t)Expected);
      abort();
    }
  }
  if (Hold) {
    assert(SawHoldLabel && Stalls > 0);
    /* Covers idle, navigation and selection: even an empty line-0 call fails. */
    assert(Frames == 1 && StatusDraws[0] == 0 && StatusDraws[1] == 1);
    assert(!SawSelectLabel);
    assert(Creates == 0 && Sets == 0 && Closes == 0 && Waits == 0);
    assert(Locates == 0 && Protocols == 0);
  } else {
    assert(!SawHoldLabel && Stalls == 0);
    assert(Frames == 1 && StatusDraws[1] == 1);
    if (Waits > 0) assert(StatusDraws[0] > 0);
    assert(Locates == (Table.ConIn != NULL ? 1U : 0U));
    assert(Protocols == ((Table.ConIn != NULL && EnumerationResult == EFI_SUCCESS) ? 1U : 0U));
  }
  Passed++;
  printf("PASS %s\n", Name);
}

int main(void) {
  Reset(TRUE, FALSE, FALSE);
  Check("hold built-in-style positive timeout without console", TRUE, -1);
  assert(Draws == Config.EntryCount && Highlight == Config.DefaultIndex);
  Reset(TRUE, FALSE, FALSE);
  Config.Timeout = 0;
  Check("hold loaded-style timeout zero without console", TRUE, -1);
  Reset(TRUE, FALSE, FALSE);
  Config.Timeout = -1;
  Check("hold negative timeout without console", TRUE, -1);
  Reset(TRUE, FALSE, FALSE);
  Check("hold returned-entry or Tools countdown false without console", FALSE, -1);
  Reset(TRUE, TRUE, TRUE);
  Check("hold with physical input but no keys", TRUE, -1);
  Reset(TRUE, TRUE, FALSE);
  Check("hold virtual console without physical handle", TRUE, -1);
  Reset(TRUE, TRUE, FALSE);
  Input.ReadKeyStroke = NULL;
  Check("hold missing ReadKeyStroke callback", TRUE, -1);
  Reset(TRUE, TRUE, TRUE);
  ErrorAt(1); ErrorAt(2); ErrorAt(3);
  Check("hold input errors do not select", TRUE, -1);
  Reset(TRUE, TRUE, TRUE);
  EnumerationResult = EFI_DEVICE_ERROR;
  KeyAt(15, 0, L'3');
  Check("hold bypasses enumeration error and accepts explicit key", TRUE, 2);
  Reset(TRUE, FALSE, FALSE);
  ConsoleAt = 20;
  KeyAt(21, SCAN_DOWN, 0); KeyAt(22, 0, L'\r');
  Check("hold console arriving later remains usable", TRUE, 2);
  Reset(TRUE, TRUE, TRUE);
  ConsoleGoneAt = 3;
  Check("hold disappearing console remains waiting", TRUE, -1);
  Reset(TRUE, TRUE, FALSE);
  CreateResult = WaitResult = EFI_DEVICE_ERROR;
  Input.WaitForKey = NULL;
  KeyAt(2, 0, L'1');
  Check("hold ignores broken event services and WaitForKey", TRUE, 0);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, SCAN_ESC, 0); KeyAt(2, 0, L'9'); KeyAt(3, 0, L'x');
  Check("hold Escape invalid digit and unknown key never select", TRUE, -1);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, SCAN_UP, 0); KeyAt(2, 0, L'\r');
  Check("hold Up and Enter", TRUE, 0);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, SCAN_DOWN, 0); KeyAt(2, 0, L'\r');
  Check("hold Down and Enter", TRUE, 2);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, SCAN_HOME, 0); KeyAt(2, 0, L'\r');
  Check("hold Home and Enter", TRUE, 0);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, SCAN_END, 0); KeyAt(2, 0, L'\r');
  Check("hold End and Enter", TRUE, 2);
  Reset(TRUE, TRUE, FALSE);
  KeyAt(1, 0, L'k'); KeyAt(2, 0, L'\r');
  Check("hold k on unenumerated console", TRUE, 0);
  Reset(TRUE, TRUE, FALSE);
  KeyAt(1, 0, L'j'); KeyAt(2, 0, L'\r');
  Check("hold j on unenumerated console", TRUE, 2);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, 0, L'\r');
  Check("hold explicit Return-to-firmware default", TRUE, 1);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, 0, L'\n');
  Check("hold linefeed selection", TRUE, 1);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, 0, L' ');
  Check("hold space selection", TRUE, 1);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, SCAN_RIGHT, 0);
  Check("hold right selection", TRUE, 1);
  Reset(TRUE, TRUE, TRUE);
  KeyAt(1, SCAN_SUSPEND, 0);
  Check("hold suspend selection", TRUE, 1);
  Reset(TRUE, TRUE, TRUE);
  Config.DefaultIndex = 0;
  KeyAt(1, 0, L'\r');
  Check("hold explicit XNU default", TRUE, 0);
  Reset(TRUE, TRUE, TRUE);
  Config.DefaultIndex = 0;
  KeyAt(1, 0, L'3');
  Check("hold explicit Tools entry with countdown disabled", FALSE, 2);
  Reset(TRUE, TRUE, TRUE);
  Config.EntryCount = 1;
  Config.DefaultIndex = 0;
  KeyAt(1, SCAN_UP, 0); KeyAt(2, SCAN_DOWN, 0); KeyAt(3, 0, L'\r');
  Check("hold single-entry navigation", TRUE, 0);

  Reset(TRUE, FALSE, FALSE);
  Config.DefaultIndex = 2;
  Check("hold Tools default first frame and idle skip status line zero", TRUE, -1);
  assert(Draws == Config.EntryCount && Highlight == 2);

  Reset(FALSE, FALSE, FALSE);
  Config.Timeout = -1;
  Check("normal no-input negative timeout still falls back", TRUE, 1);
  assert(Waits == 10 && Creates == 1 && Closes == 1);
  Reset(FALSE, FALSE, FALSE);
  Config.Timeout = 0;
  Check("normal timeout zero remains immediate", TRUE, 1);
  assert(Creates == 0 && Waits == 0);
  Reset(FALSE, TRUE, TRUE);
  Config.Timeout = 1;
  Check("normal physical-input positive countdown", TRUE, 1);
  assert(Waits == 10);
  Reset(FALSE, FALSE, FALSE);
  Check("normal countdown false without input still falls back", FALSE, 1);
  assert(Waits == 10);
  Reset(FALSE, TRUE, TRUE);
  Config.Timeout = -1;
  KeyAt(2, SCAN_DOWN, 0); KeyAt(3, 0, L'\r');
  Check("normal negative timeout accepts input", TRUE, 2);
  Reset(FALSE, TRUE, TRUE);
  Config.Timeout = -1;
  EnumerationResult = EFI_DEVICE_ERROR;
  Check("normal enumeration failure still uses fallback timeout", TRUE, 1);
  assert(Waits == 10 && Locates == 1 && Protocols == 0);
  Reset(FALSE, TRUE, TRUE);
  CreateResult = EFI_DEVICE_ERROR;
  Check("normal CreateEvent failure behavior preserved", TRUE, 1);
  assert(Creates == 1 && Closes == 0);
  Reset(FALSE, TRUE, TRUE);
  WaitResult = EFI_DEVICE_ERROR;
  Check("normal WaitForEvent failure behavior preserved", TRUE, 1);
  assert(Closes == 1);
  Reset(FALSE, FALSE, FALSE);
  Config.Timeout = 3;
  Check("normal rotation still boots default", TRUE, 1);
  assert(Draws > Config.EntryCount && Waits == 30);
  Reset(FALSE, TRUE, TRUE);
  Config.Timeout = 1;
  KeyAt(1, SCAN_ESC, 0);
  Check("normal first key still cancels countdown", TRUE, -1);
  Reset(FALSE, TRUE, TRUE);
  Config.Timeout = -1;
  KeyAt(2, 0, L'\r');
  Check("normal manual menu retains status line zero prompt", TRUE, 1);
  assert(SawSelectLabel && StatusDraws[0] == 1);
  printf("%u menu cases passed\n", Passed);
  return 0;
}
