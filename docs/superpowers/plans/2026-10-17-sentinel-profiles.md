# Sentinel Demo Profiles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the board select and retain two device-specific Sentinel baselines through power-off.

**Architecture:** A small profile store owns four checksummed records in the last 256 bytes of the reserved user-MRAM area, two records per profile. It writes from ITCM in 16-byte units with interrupts masked, then verifies the new record. Sentinel loads the profiles into RAM, selects one with SW2, and keeps the current scoring rules unchanged.

**Tech Stack:** C11, Arm Compiler 6, CMSIS-Toolbox, E8 MRAM/ITCM, host C tests.

**Spec:** `docs/superpowers/specs/2026-10-17-sentinel-profiles.md`

## Global Constraints

- Preserve all existing Arm notices and the dirty working tree.
- Never write outside the final 256 bytes of `APP_MRAM_USER_BASE`/`APP_MRAM_USER_SIZE`.
- No pre-learned baseline or unverified new sensor in the firmware.
- Do not claim persistence until a power-cycle test passes on the board.

## Review Focus

- Empty MRAM: both profiles load as empty; first learn saves.
- Interrupted write at each 16-byte step: previous valid slot stays readable.
- Corrupt or foreign bytes in the reserved area: never accepted or silently overwritten.
- Switching profiles mid-window/while alarmed: no stale sensor state leaks.
- Save failure after successful learning: live monitoring continues but LCD/UART warn that the new baseline is not saved.

---

### Task 1: Profile Store

**Files:**
- Create: `board/DevKit-E8/sentinel_profile_store.h`
- Create: `board/DevKit-E8/sentinel_profile_store.c`
- Create: `tests/sentinel_profile_store_test.c`
- Modify: `board/DevKit-E8/linker_ac6_mram.sct.src`
- Modify: `board/DevKit-E8/Board-U85.clayer.yml`

**Interfaces:**
- Produces: `int sentinel_profile_load(unsigned id, sentinel_baseline_t *out)` (1 present, 0 empty, -1 storage error).
- Produces: `int sentinel_profile_save(unsigned id, const sentinel_baseline_t *value)` (0 saved and verified, -1 error).
- `sentinel_baseline_t` contains four finite `float` means and four positive finite `float` standard deviations.

- [ ] Write host tests for empty storage, both profiles, overwrite, checksum corruption, foreign data, and each simulated interrupted write.
- [ ] Run host tests; verify they fail before implementation.
- [ ] Implement checksummed two-slot records and an injectable 16-byte storage backend.
- [ ] Run host tests; verify they pass.
- [ ] Add the target-only ITCM write routine and linker placement; verify the map places the write routine in ITCM and no profile record enters the flash image.

### Task 2: Sentinel Selection and Learning

**Files:**
- Modify: `board/DevKit-E8/sentinel.c`
- Modify: `src/app_main.cpp`
- Modify: `README.md`

**Interfaces:**
- Consumes: `sentinel_profile_load`, `sentinel_profile_save`.
- Produces: `void board_sentinel_select(unsigned id)`; existing `board_sentinel_learn` learns only the selected profile.

- [ ] Add a host-testable state transition test covering empty, learned, switching, and save failure.
- [ ] Run the test and verify it fails before implementation.
- [ ] Wire SW2 left/right/up, baseline load/save, LCD profile label, and UART diagnostics.
- [ ] Run host test and full E8 rebuild; verify both pass.
- [ ] On board, learn profile 1, power-cycle, reselect it, and confirm OK without relearning; do the same for profile 2 once its device is chosen. Check flashing separately for data preservation.

### Task 3: Finish

- [ ] Review diff for unrelated changes and generated files.
- [ ] Document the exact button sequence and limits in README.
- [ ] Report which hardware checks passed and which remain; do not call profile 2 validated before its appliance is tested.
