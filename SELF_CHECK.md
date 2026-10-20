# SELF_CHECK.md

## Layout / Clean Verification 2026-10-20

- Application/transport/firmware behavior was not refactored; existing conforming source layout remains intact.
- Build: PASS: scripts/build_mfc.ps1 -Configuration Release -Platform x64 -CloseRunningTool $false; 0 warnings/errors, normal generated version 1.0.0.038.
- Runtime: PASS: isolated captioned frame/startup/normal close, exit 0, no real INI or hardware connection.
- PASS: shared verifier with `-CleanActualProject -CheckNestedDelivery -CheckReparseRejection`: help is non-destructive, object removed, 17 nested delivery/runtime sentinels plus source retained, repeat-clean and junction rejection PASS. 2 actual protected build files unchanged (SHA-256).
- Five already-ignored root FW_Upload_*.jpg originals moved to ignored tmp/private_screenshots with no overwrite and SHA-256 preservation. Existing sanitized public docs/images copies and README links remain unchanged. Never stage the originals.
- Pre-existing modified build/simple_programming_tool.ini remains local, untouched and excluded from staging.
- NOT RUN: physical devices, destructive workflows, firmware rebuild/programming, complete UI/DPI and new Debug acceptance.
- Commit/push or local-only state is recorded in workspace `#READ_ME_FIRST#/PROJECT_LAYOUT_MIGRATION.md`.

## Folder/repository rename gate (2026-10-14)

- PASS: GitHub renamed in place to simple_nuvoton_isp_tool; same repository
  ID, public visibility and main branch. origin updated to the new URL.
- PASS: project/EXE/INI identities unchanged; the existing user INI edit is
  preserved and not selected for upload.
- PASS: local folder renamed after VS Code/terminals closed; .git/worktree and
  user data preserved. INI firmware path rebased locally, other options kept.
- PASS: post-move Debug/Release x64 builds0warnings/0errors; pre-build version
  event disabled for validation. Original delivery EXE bytes preserved.
- PASS: rebuilt verifiers/logs/backups outside Git; no EXE/INI/logs staged.
- NOT RUN: startup/UI smoke (desktop native pipe unavailable after retry/reset);
  physical I2C/UART/HID/XMODEM and firmware flashing not performed.
- PASS: naming docs committed/pushed as8bd9206; local/remote main equality verified.
- PASS: modified Markdown links/fences and whitespace; clean audit24 scripts.

## Current Verification Results

Date: 2026-07-06

## PC Tool

Command:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_mfc.ps1 -Configuration Release -Platform x64
```

Result: PASS

- Built `build\SimpleProgrammingTool.exe`
- MSBuild result: 0 warnings, 0 errors
- Latest run after moving I2C bridge HID/VCOM controls into the I2C tab: PASS at 2026-07-06 23:28:29
- UI coordinate smoke test: PASS. Top-level rows are Firmware Image then INI/Build; visible I2C tab contains `I2C Bridge - HID`, `I2C Bridge - VCOM`, Bridge status, I2C Host, Nuvoton I2C ISP, and Response sections without main-frame overlap.

## Firmware Keil Build

Result: NOT RUN

Reason: Keil `UV4`/`armclang` command line is not available in this shell.

## Hardware Test

Result: NOT RUN in this workspace session.

Recommended hardware smoke tests:

1. Flash the M032 programming bridge firmware.
2. Launch `build\SimpleProgrammingTool.exe`.
3. Scan/connect VID `0x0416`, PID `0x5020`.
4. Click `Get Info`; expect `m032-programming-bridge/1.0.0`.
5. Wire the selected M032 I2C pin pair to the target board I2C ISP pins and ground.
6. Boot/reset the target into I2C ISP LDROM.
7. Click `Connect Target`; confirm target-ready status and PDID/FW log.
8. Load a small APROM `.bin`, click `Program APROM`, then click `Run APROM`.

## Known Gaps

- Hardware validation must be rerun after the split.
- VCOM PC transport is implemented, but VCOM hardware validation and matching M032 bridge firmware parser validation are NOT RUN in this workspace session.
- UART ISP and XMODEM PC tabs are implemented, but hardware validation against the referenced Nuvoton UART ISP and XMODEM bootloaders is NOT RUN in this workspace session.
