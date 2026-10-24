# AGENTS.md

## Verified x64 size configuration (2026-10-25)

- Only x64 defines _AFX_NO_MFC_CONTROLS_IN_DIALOGS after standard/common/custom
  control review. Reassess before adding CMFC controls; Win32 is not changed.
- Keep /O2, /GL, /MT, /GS, PE security, architecture, protocol and existing features.
- Use isolated hash-matched startup/control/Debug/close checks and scoped paired
  measurements; no user INIs, device commands or unrelated full-suite reruns.
- Preserve the preexisting modified personal INI unstaged; never publish its values.
- Actual results and scope: SELF_CHECK.md; raw sanitized performance evidence in
  docs/verification/exe-slimming-20261025.json.


## Layout / Clean Rules

- Follow workspace `#READ_ME_FIRST#/PROJECT_LAYOUT.md`; keep current source/module/firmware/protocol paths stable unless a verified move is needed.
- Use `scripts/clean_mfc.bat` for file-allowlist cleanup with complete path/reparse preflight. Never purge a whole folder by its name; EXE/INI/DLL/data/log/unknown files remain protected even under obj/tests/verify.
- Codex performs build, safe tests and actual clean-preservation verification; never assign the audit to the user.
- Canonical Release entry is scripts/build_release_mfc.bat; the original build_mfc.ps1 entry remains supported.
- Public screenshots are sanitized copies in docs/images. Raw screenshots in ignored tmp/private_screenshots and the user's existing INI are local-only; never stage their contents.
