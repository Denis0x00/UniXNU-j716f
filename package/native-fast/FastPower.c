// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit confirmed PSCI SYSTEM_RESET (0x84000009) and SYSTEM_OFF (0x84000008).
#include "Common.h"
#include <Protocol/LoadedImage.h>

VOID J716fSmc(UINT64 *args);

STATIC UINT64 PsciCall(UINT64 fid, UINT64 arg1)
{
  UINT64 args[8];
  (VOID)mCandidate;
  ZeroMem(args, sizeof(args));
  args[0] = fid;
  args[1] = arg1;
  Barrier();
  J716fSmc(args);
  return args[0];
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
  EFI_STATUS s;
  EFI_INPUT_KEY key;
  UINT32 fid = 0x84000009U;
  BOOLEAN isOff = FALSE;
  UINT64 ver;
  CHAR16 text[128];
  (VOID)system;

  s = Panel();
  if (EFI_ERROR(s)) { return s; }
  s = gBS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid, (VOID **)&loaded);
  if (!EFI_ERROR(s) && loaded && loaded->LoadOptions && loaded->LoadOptionsSize >= 4U) {
    CONST CHAR16 *opt = (CONST CHAR16 *)loaded->LoadOptions;
    if (StrnCmp(opt, L"shutdown", 8U) == 0) {
      fid = 0x84000008U;
      isOff = TRUE;
    }
  }
  ver = PsciCall(0x84000000U, 0);
  Clear();
  Line(0, isOff ? L"J716F POWER OFF (PSCI SYSTEM_OFF 0x84000008)"
                : L"J716F WARM RESTART (PSCI SYSTEM_RESET 0x84000009)");
  UnicodeSPrint(text, sizeof(text), L"PSCI Version=%08lx  FeatureCheck=%08lx",
    ver & 0xffffffffULL, PsciCall(0x8400000aU, fid) & 0xffffffffULL);
  Line(2, text);
  Line(5, L"Press Power (Enter) to execute immediately, or Vol/Esc to cancel.");
  for (;;) {
    s = Key(&key);
    if (EFI_ERROR(s)) { continue; }
    if (key.UnicodeChar == L'\r') {
      PsciCall(fid, 0);
      Line(8, L"PSCI call returned unexpectedly. Press Esc to return.");
    } else {
      return EFI_SUCCESS;
    }
  }
}
