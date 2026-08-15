# Critical Hardening and Loading Overlay — SDD Progress Ledger

Plan: docs/superpowers/plans/2026-08-14-critical-hardening-loading.md
Design: docs/superpowers/specs/2026-08-14-critical-hardening-loading-design.md
Branch: feature/enemy-combat-rhythm (do NOT switch to master; do not push without explicit approval)

## Repo facts learned this session (do not re-derive)

- No `superpowers` plugin/skill is installed in this Claude Code environment. Orchestrator (top-level
  session) is manually replicating subagent-driven-development: fresh general-purpose implementer agent
  per task, fresh general-purpose reviewer agent per task, fix loop before advancing, final whole-branch
  review at end (mirrors the prior 2026-08-13 combat-rhythm plan's ledger, which used the same approach).
- Only one VS project in TestGame.sln: `TestGame\TestGame.vcxproj` (no separate test project).
- Standalone tests are compiled ad hoc with MSVC `cl.exe`, one exe per test file:
  ```
  cl /nologo /std:c++17 /EHsc /D<MACRO>_TEST_MAIN /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\<Impl>.cpp [more .cpp] TestGame\<Tests>.cpp /link /OUT:x64\Debug\<Tests>.exe
  x64\Debug\<Tests>.exe
  ```
  `cl.exe` is NOT on PATH in a plain shell — must run from a VS Developer environment first:
  `"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"` (call
  this, then cl.exe/msbuild are available in that same shell invocation — PowerShell: `cmd /c "call vcvars64.bat && cl ..."`).
- raylib 5.5 win64 msvc16 is installed at `C:\CLibraries\raylib-5.5_win64_msvc16` (include/, lib/).
- Full solution build: `msbuild TestGame.sln /p:Configuration=Debug /p:Platform=x64` (and Release x64) from
  the same vcvars64-initialized shell, run from repo root `C:\Users\rober\OneDrive\Desktop\MysticOnslaught-master`.
- CombatRhythmTests.cpp exists already from the prior combat-rhythm plan and is a plain `int main()` with
  `<cassert>` — no test framework. It is NOT added to TestGame.vcxproj (has its own main, would collide).
- Pre-existing uncommitted changes at session start (DO NOT clobber; preserve unrelated hunks on every edit):
  modified: MYSTIC_ONSLAUGHT_CURRENT_VERSION.txt, TestGame/BomberImp.cpp, TestGame/CutsceneManager.cpp,
  TestGame/Enemy.cpp, TestGame/Enemy.h, TestGame/Engine.cpp, TestGame/Engine.h, TestGame/FlameWisp.cpp,
  TestGame/FlameWisp.h, TestGame/GameBalance.h, TestGame/GameTypes.h, TestGame/MapEditor.cpp,
  TestGame/MapEditor.h, TestGame/MetaProgression.cpp, TestGame/MetaProgression.h, TestGame/Phantom.cpp,
  TestGame/SkeletonArcher.cpp, TestGame/VillageAssetData.cpp, TestGame/VillageAssetData.h,
  TestGame/VillageLayoutData.cpp, TestGame/VillageLayoutData.h, TestGame/VillageLayoutDataTests.cpp,
  TestGame/settings.cfg, VillageAssets/VillageGraveyard.vasset, VillageAssets/VillageLayout.vlayout,
  VillageAssets/ZephsShop.vasset
  deleted: TestGame/VillageMap.cpp, TestGame/VillageMap.h, TestGame/villagemap_village.txt,
  TestGame/villagemap_village_markers.txt, TestGame/villageobject_DirtRoad.txt,
  TestGame/villageobject_Flowers.txt, TestGame/villageobject_ZephsShop.txt
  untracked: .idea/, TestGame/Rooms/Graveyard/graveyard/{e,s,n}_room.mroom, VillageAssets/DirtRoad.png,
  VillageAssets/DirtRoad.vasset, VillageAssets/Flowers.png, VillageAssets/Flowers.vasset,
  VillageAssets/PoeAltar.png, VillageAssets/PoeAltar.vasset, VillageAssets/VillageGate.png,
  VillageAssets/VillageGate.vasset, docs/superpowers/plans/2026-08-13-hades-style-enemy-combat-rhythm*.md,
  docs/superpowers/specs/2026-08-13-hades-style-enemy-combat-rhythm-design.md,
  docs/superpowers/plans/2026-08-14-critical-hardening-loading.md,
  docs/superpowers/specs/2026-08-14-critical-hardening-loading-design.md
  Every commit in this plan must `git add` ONLY the exact files the plan step lists. Never `git add -A` /
  `git add .`. Never touch .idea/, build output dirs, or the village/village-asset files above unless a
  task step explicitly names them (Task 4 explicitly touches VillageAssetData.* and MapEditor.*, which IS
  in scope for Task 4 but must not stage the untracked village PNG/.vasset/.vlayout/.mroom files).
- HEAD at session start: ba6bb2c "docs: design critical hardening and loading overlay".
- Never run: git reset --hard, git checkout --, git restore (on existing work), git clean, destructive
  rebases, forced branch ops, broad deletion commands.
- No push to GitHub without explicit user approval. Local commits per task ARE authorized (plan specifies them).

## Task Status

- [ ] Task 1: Engagement Recovery and Slot Eligibility — NOT STARTED
- [ ] Task 2: Custom Enemy Commitment Compliance — NOT STARTED
- [ ] Task 3: Authoritative Reinforcement Positions — NOT STARTED
- [ ] Task 4: Lossless Village Asset Adjuster — NOT STARTED
- [ ] Task 5: Deterministic Loading Transition Policy — NOT STARTED
- [ ] Task 6: Black Loading Screen Integration — NOT STARTED
- [ ] Task 7: Full Regression and Merge Audit — NOT STARTED
