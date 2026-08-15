# Hades-Style Enemy Combat Rhythm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the roster and total encounter population while adding limited attack commitments, distinct role positioning, staged reinforcements, and wall-safe contact.

**Architecture:** Add deterministic policy modules for engagement, reinforcement pacing, and body separation, then integrate them through `CombatDirector`, `Enemy`, and the dungeon portion of `Engine`. Existing attacks remain intact; normal enemy classes only customize approach and recovery. Pending spawns remain Engine-owned because Engine already owns reinforcement queues, spawn validation, VFX, and world rendering.

**Tech Stack:** C++17, raylib 5.5, MSVC v143, existing assert-based C++ tests, Visual Studio 2022.

## Global Constraints

- Preserve existing attacks, art, bosses, scripted prologue encounters, composition, and total planned population.
- Simultaneous standard-room targets are Small 3, Medium 4, Large 5, Arena 6.
- Reinforcement batches contain 1-2 enemies and never bypass the simultaneous target.
- Commit limits are 2 early and 3 mid/late; fragile swarm encounters may add one.
- Spawn sequence is 0.65 seconds purple circle, 0.30 seconds smoke, then spawn.
- Ordinary contact favors enemy displacement and never changes authored forced-push attacks.
- Preserve every pre-existing transferred change in dirty files, especially `Engine.cpp`, `Engine.h`, and `TestGame.vcxproj`.

## File Structure

- Create `CombatEngagement.h/.cpp`: deterministic Commit, Support, and Reposition policy.
- Create `ReinforcementPacing.h/.cpp`: body targets, batch sizing, and spawn phase timing.
- Create `CombatSeparation.h/.cpp`: enemy-first collision response.
- Create `CombatRhythmTests.cpp`: focused non-rendering tests.
- Modify `Enemy` and normal enemy classes: store and consume engagement assignments.
- Modify `CombatDirector`: assign intentions once per frame.
- Modify `Engine`: pending spawn lifecycle, rendering, smoke, and safe collision integration.
- Modify `GameBalance.h`: centralized rhythm constants.
- Modify Visual Studio project files: compile new sources while retaining local edits.

---

### Task 1: Deterministic Engagement Policy

**Files:**
- Create: `TestGame/CombatEngagement.h`
- Create: `TestGame/CombatEngagement.cpp`
- Create: `TestGame/CombatRhythmTests.cpp`
- Modify: `TestGame/GameBalance.h`

**Interfaces:**
- Produces: `enum class EngagementIntent { Commit, Support, Reposition }`
- Produces: `EngagementCandidate { uint64_t id; EnemyRole role; float distance; bool alive; bool locked; bool recovering; bool swarm; }`
- Produces: `EngagementAssignment { uint64_t id; EngagementIntent intent; float orbitAngle; }`
- Produces: `int CombatCommitLimit(int tier, bool swarm)`
- Produces: `BuildEngagementAssignments(candidates, tier, swarm, sequence)`

- [ ] **Step 1: Write failing tests**

Create six candidates and assert tier 0 assigns two committers, tier 1 assigns three, swarm adds only one, locked owners retain Commit, dead entries get no assignment, Tank/Support wait as Support, and others wait as Reposition. Repeat with the same sequence and assert identical output.

- [ ] **Step 2: Compile and verify failure**

```powershell
cl /nologo /std:c++17 /EHsc /DCOMBAT_RHYTHM_TEST_MAIN TestGame\CombatRhythmTests.cpp TestGame\CombatEngagement.cpp /I TestGame /I C:\CLibraries\raylib-5.5_win64_msvc16\include /link /LIBPATH:C:\CLibraries\raylib-5.5_win64_msvc16\lib raylib.lib winmm.lib gdi32.lib user32.lib shell32.lib /OUT:TestGame\CombatRhythmTests.exe
```

Expected: missing policy types/functions.

- [ ] **Step 3: Implement minimal policy**

Add `Balance::Rhythm::kCommittersByTier = {2,3,3}`, `kSwarmExtraCommitters = 1`, and `kPostCommitRepositionSeconds = 0.65f`. Preserve locked owners first, select remaining owners by stable distance/role score with ID tie-break, and derive orbit angles without raylib RNG.

- [ ] **Step 4: Rebuild and run**

```powershell
TestGame\CombatRhythmTests.exe
```

Expected: all engagement tests pass.

- [ ] **Step 5: Commit**

```powershell
git add TestGame/CombatEngagement.h TestGame/CombatEngagement.cpp TestGame/CombatRhythmTests.cpp TestGame/GameBalance.h
git commit -m "feat: add deterministic enemy engagement policy"
```

### Task 2: Stable Commit Ownership Integration

**Files:**
- Modify: `TestGame/Enemy.h`, `TestGame/Enemy.cpp`
- Modify: `TestGame/CombatDirector.h`, `TestGame/CombatDirector.cpp`
- Modify: `TestGame/CombatRhythmTests.cpp`
- Modify: `TestGame/TestGame.vcxproj`, `TestGame/TestGame.vcxproj.filters`

**Interfaces:**
- Consumes: `BuildEngagementAssignments`
- Produces on Enemy: `GetRuntimeId()`, `SetEngagementAssignment(intent,target)`, `GetEngagementIntent()`, `GetEngagementTarget()`, `IsAttackLockedForEngagement()`, `SetSwarmProfile(bool)`
- Produces: `EngagementLatch` with `BeginCommit()`, `EndCommit(seconds)`, `Update(dt)`, and `CanCommit()`

- [ ] **Step 1: Add failing latch tests**

Assert BeginCommit locks ownership; EndCommit(0.65) enters Reposition; Update(0.64) remains ineligible; Update(0.02) restores eligibility.

- [ ] **Step 2: Run and verify failure**

Rebuild/run `CombatRhythmTests.exe`. Expected: missing `EngagementLatch`.

- [ ] **Step 3: Implement latch and Enemy storage**

Assign stable runtime IDs on construction and retain them through pooling. Reset intent, target, swarm flag, and timers in the shared spawn reset path. Authored attacks and uninterruptible special states hold a slot through recovery.

- [ ] **Step 4: Assign once per runtime update**

Before individual enemy updates, build candidates and apply assignments. Replace the scan-based two-attacker gate with assigned Commit intent, retaining a fallback for non-dungeon callers without assignments.

- [ ] **Step 5: Add sources to project files**

Place new entries beside CombatDirector entries and preserve all unrelated project-file differences.

- [ ] **Step 6: Run focused and existing combat tests**

Expected: both suites pass and total-population assertions remain unchanged.

- [ ] **Step 7: Commit**

```powershell
git add TestGame/Enemy.h TestGame/Enemy.cpp TestGame/CombatDirector.h TestGame/CombatDirector.cpp TestGame/CombatEngagement.h TestGame/CombatEngagement.cpp TestGame/CombatRhythmTests.cpp TestGame/TestGame.vcxproj TestGame/TestGame.vcxproj.filters
git commit -m "feat: coordinate stable enemy attack commitments"
```

### Task 3: Role Positioning and Enemy Identity

**Files:**
- Modify: `TestGame/Enemy.cpp`
- Modify: `TestGame/SlimeEnemy.cpp`, `SkeletonArcher.cpp`, `FlameWisp.cpp`, `Sporeling.cpp`
- Modify: `TestGame/Shieldbearer.cpp`, `Phantom.cpp`, `BomberImp.cpp`, `Warchief.cpp`, `LivingBlade.cpp`
- Modify: `TestGame/CombatRhythmTests.cpp`

**Interfaces:**
- Produces: `ComputeEngagementTarget(player,self,allyCentroid,role,intent,orbitAngle)`
- Consumes: Enemy engagement intent and target.

- [ ] **Step 1: Add failing positioning tests**

Assert Reposition stays outside collision range; tanks screen between player and allies; supports anchor near allies; assassins choose off-angles; ranged units change lanes without endless retreat; coincident inputs remain finite.

- [ ] **Step 2: Run and verify failure**

Expected: missing `ComputeEngagementTarget`.

- [ ] **Step 3: Implement shared positioning**

Use deterministic polar positions with role radii. Reposition decelerates or holds at its target and never falls back to direct homing. Commit keeps existing pathfinding and attack entry.

- [ ] **Step 4: Apply minimal identity adaptations**

Shadow Grunt disengages after melee; Slime locks its approach vector; Archer plants, fires, relocates; Wisp takes lateral lanes; Sporeling avoids first commitment and seeks useful cloud space; Shieldbearer screens allies; Phantom flanks and retreats; Bomber relocates after throwing; Warchief anchors to allies; Living Blade passes through and recovers away. Do not change damage or attack functions.

- [ ] **Step 5: Build Debug x64**

Expected: success with no attack-signature changes.

- [ ] **Step 6: Commit**

```powershell
git add TestGame/Enemy.cpp TestGame/SlimeEnemy.cpp TestGame/SkeletonArcher.cpp TestGame/FlameWisp.cpp TestGame/Sporeling.cpp TestGame/Shieldbearer.cpp TestGame/Phantom.cpp TestGame/BomberImp.cpp TestGame/Warchief.cpp TestGame/LivingBlade.cpp TestGame/CombatEngagement.h TestGame/CombatEngagement.cpp TestGame/CombatRhythmTests.cpp
git commit -m "feat: differentiate normal enemy combat rhythms"
```

### Task 4: Reinforcement Pacing and Pending Spawn State

**Files:**
- Create: `TestGame/ReinforcementPacing.h`, `TestGame/ReinforcementPacing.cpp`
- Modify: `TestGame/CombatRhythmTests.cpp`, `TestGame/EncounterPlannerTests.cpp`
- Modify: `TestGame/GameBalance.h`, `TestGame/Engine.h`, `TestGame/Engine.cpp`
- Modify: Visual Studio project files

**Interfaces:**
- Produces: `SimultaneousBodyTarget(RoomCapacityBand)` returning 3/4/5/6
- Produces: `ReinforcementBatchSize(active,pending,target,queued)` capped at 2
- Produces: `PendingSpawnPhase { Circle, Smoke, Ready }`
- Produces: `PendingEnemySpawn { entry, worldPos, phase, timer, smokeEmitted }`
- Produces: `AdvancePendingSpawn(spawn,dt)`

- [ ] **Step 1: Write failing pacing tests**

Test all body targets, batch sizing at/under target, cap two, Circle at 0.64s, Smoke at 0.66s, Ready after 0.96s, and one-time smoke emission. Retain `opening + reinforcements == plannedPopulation`.

- [ ] **Step 2: Run and verify failure**

Expected: missing pacing types/functions.

- [ ] **Step 3: Implement policy**

Add centralized 0.65/0.30 durations, batch max 2, and 3/4/5/6 targets. Consume large-dt overshoot correctly.

- [ ] **Step 4: Integrate pending spawns**

Add `_pendingEnemySpawns`. Reserve 1-2 valid positions only when active plus pending is below target. Revalidate at Ready; if invalid, return the entry to the front of the reinforcement queue. Clear pending entries on room exit, reset, death, and editor reset.

- [ ] **Step 5: Preserve total population**

Partition opening using `min(capacity.openingBodyCap, SimultaneousBodyTarget(band))`; queue all overflow. Do not alter composition or target-population calculations.

- [ ] **Step 6: Add project entries and run tests**

Run rhythm, encounter planner, and combat systems tests. Expected: all pass.

- [ ] **Step 7: Commit**

```powershell
git add TestGame/ReinforcementPacing.h TestGame/ReinforcementPacing.cpp TestGame/CombatRhythmTests.cpp TestGame/EncounterPlannerTests.cpp TestGame/GameBalance.h TestGame/Engine.h TestGame/Engine.cpp TestGame/TestGame.vcxproj TestGame/TestGame.vcxproj.filters
git commit -m "feat: stage smaller reinforcement batches"
```

### Task 5: Purple Spawn Circle and Smoke

**Files:**
- Modify: `TestGame/Engine.cpp`
- Modify: `TestGame/VFXManager.h`, `TestGame/VFXManager.cpp`

**Interfaces:**
- Produces: `VFXManager::SpawnSmokeBurst(Vector2, Color)`
- Produces: `Engine::DrawPendingEnemySpawns() const`

- [ ] **Step 1: Add smoke semantic**

Implement SmokeBurst through the bounded existing impact-particle pool with purple-gray tint, low speed, and upward bias. Load no asset.

- [ ] **Step 2: Draw Circle phase**

Before enemy sprites, draw translucent `DrawCircleV` plus two expanding `DrawCircleLinesV` rings. Contract and brighten during Smoke.

- [ ] **Step 3: Emit smoke once and delay activation**

Emit once at Circle-to-Smoke transition. Spawn only at Ready and apply a short arrival/orientation latch so the enemy cannot move or attack on creation frame.

- [ ] **Step 4: Build and manually verify**

Verify exact position, below-sprite rendering, circle then smoke then enemy, pause freeze, and cleanup on room exit.

- [ ] **Step 5: Commit**

```powershell
git add TestGame/Engine.cpp TestGame/VFXManager.h TestGame/VFXManager.cpp
git commit -m "feat: telegraph enemy reinforcements"
```

### Task 6: Wall-Safe Separation

**Files:**
- Create: `TestGame/CombatSeparation.h`, `TestGame/CombatSeparation.cpp`
- Modify: `TestGame/CombatRhythmTests.cpp`, `TestGame/Engine.cpp`
- Modify: Visual Studio project files

**Interfaces:**
- Produces: `SeparationMove { Vector2 enemyDelta; Vector2 playerDelta; }`
- Produces: `ChooseBodySeparation(playerOutMtv, enemyMoveValid, deepOverlap)`

- [ ] **Step 1: Write failing tests**

Assert normal overlap negates player MTV onto enemy; invalid enemy correction permits the smallest player fallback only for deep overlap; shallow invalid overlap does not move player; four contacts from one stable player position do not sum four player pushes.

- [ ] **Step 2: Run and verify failure**

Expected: missing separation policy.

- [ ] **Step 3: Implement geometry policy**

Return `enemyDelta = -playerOutMtv` for ordinary overlap. Clamp deep-overlap fallback. Leave Character forced-push and boss knockback untouched.

- [ ] **Step 4: Integrate collision ordering**

Capture one stable player position, move ordinary enemies outward, then run dungeon enemy collision. If geometry rejects a deep correction, choose one smallest player fallback, apply it once, and immediately resolve against tiles/props with the existing movement collision helper.

- [ ] **Step 5: Add project entries and test**

Run rhythm tests, room collision tests, and Debug x64 build. Expected: all pass.

- [ ] **Step 6: Reproduce original bug manually**

Test three melee enemies beside every wall and a prop. Verify parallel/away movement, enemy outward separation, unchanged dash, and forced boss knockback stopping at walls.

- [ ] **Step 7: Commit**

```powershell
git add TestGame/CombatSeparation.h TestGame/CombatSeparation.cpp TestGame/CombatRhythmTests.cpp TestGame/Engine.cpp TestGame/TestGame.vcxproj TestGame/TestGame.vcxproj.filters
git commit -m "fix: prevent enemy bodies pinning player to walls"
```

### Task 7: Full Verification and Balance Review

**Files:**
- Modify only if evidence requires: `TestGame/GameBalance.h`
- Modify only if behavior clarification is needed: approved design spec

- [ ] **Step 1: Run automated regression suite**

Run focused rhythm tests and every existing standalone test target. Record exact commands and results.

- [ ] **Step 2: Build Debug and Release x64**

Expected: zero errors.

- [ ] **Step 3: Run gameplay matrix**

Exercise all room capacity bands, tiers, Skirmish/Assault/Swarm, ranged-heavy groups, narrow props, pause/exit/death during spawn, and an authored forced-push boss.

- [ ] **Step 4: Tune centralized constants only**

If evidence requires tuning, adjust only commit count, reposition duration, simultaneous targets, or telegraph durations. Do not change total population, enemy damage, or authored attack timing. Re-run Steps 1-3.

- [ ] **Step 5: Inspect final diff**

Run `git diff --check`, `git status --short`, and a path-scoped diff. Confirm no transferred change was overwritten and no build artifact is staged.

- [ ] **Step 6: Commit verification adjustments if any**

```powershell
git add TestGame/GameBalance.h docs/superpowers/specs/2026-08-13-hades-style-enemy-combat-rhythm-design.md
git commit -m "balance: tune authored enemy combat rhythm"
```

Skip this commit if verification requires no adjustment.

