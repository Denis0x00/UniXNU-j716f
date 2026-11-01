// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board bring-up policy over the pinned upstream menu library. No countdown,
// fallback selection, timer-error boot, idle rotation or automatic retries.
UINTN
UniXNUMenuRun (
  IN UNIXNU_UI *Ui,
  IN UNIXNU_CONFIG *Config,
  IN BOOLEAN Countdown
  )
{
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN Selected, Previous, Index, Signaled;
  (VOID)Countdown;
  Selected = Config->DefaultIndex;
  UniXNUUiDrawFrame (Ui, Config);
  if (Config->EntryCount == 0 || Selected >= Config->EntryCount || gST->ConIn == NULL) {
    UniXNUUiDrawStatus (Ui, 0, L"Native manual gate: no usable configuration/input. HALTED.");
    for (;;) { gBS->Stall (100000); }
  }
  for (Index = 0; Index < Config->EntryCount; Index++) {
    UniXNUUiDrawEntry (Ui, Config, Index, Index == Selected);
  }
  UniXNUUiDrawStatus (Ui, 0, L"Native manual-only: READ first; Query/ARM; one matching launch.");
  UniXNUUiDrawStatus (Ui, 1, L"Volume: select. Power/Enter: execute. No countdown or fallback boot.");
  if (Ui->Notice[0] != L'\0') { UniXNUUiDrawStatus (Ui, 2, Ui->Notice); }
  for (;;) {
    Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Signaled);
    if (EFI_ERROR (Status)) {
      UniXNUUiDrawStatus (Ui, 2, L"Input wait failed. No entry launched; HALTED.");
      for (;;) { gBS->Stall (100000); }
    }
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) { continue; }
    Previous = Selected;
    if (Key.ScanCode == SCAN_UP) { Selected = (Selected + Config->EntryCount - 1) % Config->EntryCount; }
    else if (Key.ScanCode == SCAN_DOWN) { Selected = (Selected + 1) % Config->EntryCount; }
    else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) { return Selected; }
    if (Selected != Previous) {
      UniXNUUiDrawEntry (Ui, Config, Previous, FALSE);
      UniXNUUiDrawEntry (Ui, Config, Selected, TRUE);
    }
  }
}
