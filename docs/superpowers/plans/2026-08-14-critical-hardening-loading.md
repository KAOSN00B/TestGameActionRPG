# Critical Hardening and Loading Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Repair the audited combat and editor correctness bugs and present a black Loading... overlay before every synchronous dungeon-generation transition.

**Architecture:** Fix combat ownership at the shared CombatDirector boundary, make telegraphed coordinates authoritative, and preserve the complete loaded village asset model during editor saves. Add a small deterministic LoadingTransition policy type; Engine owns callbacks and rendering while the policy guarantees a black frame precedes one-shot heavy work.

**Tech Stack:** C++17, raylib 5.5, MSVC v143, Visual Studio 2022, existing assert-based standalone tests.

## Global Constraints

- Do not change damage, total encounter population, authored attack timing, boss movesets, or forced-push behavior.
- Tick each engagement latch exactly once per active gameplay frame.
- Commit limits remain 2 early, 3 mid/late, plus only the approved fragile-swarm exception.
- Telegraphed reinforcement coordinates are final spawn coordinates.
- Village editor saves must preserve fields the editor does not own.
- New village asset identifiers allow only ASCII letters, digits, underscore, and hyphen.
- Loading must present at least one complete black frame before synchronous heavy work.
- The loading callback executes exactly once; no fake percentage and no threading.
- Preserve all current uncommitted transferred village/editor work and never reset, clean, or overwrite it.
- Do not push to GitHub without explicit approval.

---

### Task 1: Engagement Recovery and Slot Eligibility

**Files:**
- Modify: `TestGame/Enemy.h`
- Modify: `TestGame/CombatDirector.cpp`
- Modify: `TestGame/CombatRhythmTests.cpp`

**Interfaces:**
- Produces on Enemy: `void UpdateEngagementRuntime(float dt)`
- Produces on Enemy: `bool CanOccupyEngagementSlot() const`
- Consumes existing `EngagementLatch::Update`, arrival, frozen, electric-stun, life, dying, and pit-fall state.

- [ ] **Step 1: Add failing lifecycle tests**

Extend CombatRhythmTests with a small runtime-facing harness that proves: BeginCommit then EndCommit(0.65) is ineligible; 0.64 seconds of shared runtime ticks remains ineligible; another 0.02 seconds becomes eligible; exactly one tick of 0.10 subtracts 0.10 rather than 0.20.

- [ ] **Step 2: Add failing eligibility tests**

Test a pure eligibility input/helper for active/alive/arriving/frozen/electro-stunned/dying/pit-falling combinations. Assert only a fully actionable enemy is eligible.

- [ ] **Step 3: Compile and verify failure**

Use the existing vcvars64 compile command recorded in the prior progress ledger for CombatRhythmTests. Expected: missing shared runtime/eligibility interface or failed assertions.

- [ ] **Step 4: Implement shared ticking**

Implement `Enemy::UpdateEngagementRuntime(dt)` as the single public boundary that updates the latch. Call it once in CombatDirector before candidates are built. Do not also tick it in base or custom Enemy Update methods.

- [ ] **Step 5: Implement shared eligibility**

Implement `CanOccupyEngagementSlot()` from existing state accessors. Exclude inactive, dead, dying, pit-falling, arriving, frozen, and electro-stunned enemies. Use it while building candidates; preserve authored locked attacks until their completion path releases them.

- [ ] **Step 6: Run tests and builds**

Run CombatRhythmTests and Debug x64. Expected: lifecycle/eligibility tests pass and build has zero errors.

- [ ] **Step 7: Commit**

```powershell
git add TestGame/Enemy.h TestGame/CombatDirector.cpp TestGame/CombatRhythmTests.cpp
git commit -m "fix: advance engagement recovery and exclude disabled enemies"
```

### Task 2: Custom Enemy Commitment Compliance

**Files:**
- Modify: `TestGame/SkeletonArcher.cpp`, `TestGame/SkeletonArcher.h`
- Modify: `TestGame/FlameWisp.cpp`, `TestGame/FlameWisp.h`
- Modify: `TestGame/BomberImp.cpp`, `TestGame/BomberImp.h`
- Modify: `TestGame/CombatRhythmTests.cpp`

**Interfaces:**
- Consumes: `HasEngagementAssignment()`, `GetEngagementIntent()`, and engagement latch eligibility.
- Produces per custom enemy: one explicit offensive-state entry predicate and exactly paired BeginCommit/EndCommit transitions.

- [ ] **Step 1: Extract and test offensive-entry policy**

Add a pure helper `CanBeginAssignedAttack(bool hasAssignment, EngagementIntent intent, bool latchEligible)`. Test legacy/no-assignment compatibility, rejection of Support/Reposition, rejection during recovery, and acceptance of eligible Commit.

- [ ] **Step 2: Run and verify failure**

Rebuild CombatRhythmTests. Expected: missing helper.

- [ ] **Step 3: Gate Skeleton Archer**

Require the helper before entering bow-draw. Begin commitment at draw start. End commitment after firing and when draw is cancelled by hurt/death/state reset. Do not change aim duration, projectile, damage, or relocation duration.

- [ ] **Step 4: Gate Flame Wisp**

Require Commit before entering the teleport/cast sequence. Begin once when the offensive cycle begins and end after cast recovery or cancellation. Reposition-only teleports must not begin an offensive commitment.

- [ ] **Step 5: Gate Bomber Imp**

Require Commit and latch eligibility before lighting the fuse from distance. Begin once at fuse entry and end on detonation completion, interrupted reset, or cancellation. Point-blank contact remains dangerous only after a valid commitment.

- [ ] **Step 6: Add state-transition regression assertions**

For each class, test or expose a lightweight debug fact proving one Begin and one End per cycle and cancellation releases recovery. Avoid constructing texture-dependent enemies if the existing test harness cannot safely initialize raylib; in that case test the shared transition helper and add static debug counters guarded by test macros.

- [ ] **Step 7: Run focused tests and Debug build**

Expected: CombatRhythmTests pass and Debug x64 has zero errors.

- [ ] **Step 8: Commit**

```powershell
git add TestGame/SkeletonArcher.cpp TestGame/SkeletonArcher.h TestGame/FlameWisp.cpp TestGame/FlameWisp.h TestGame/BomberImp.cpp TestGame/BomberImp.h TestGame/CombatRhythmTests.cpp
git commit -m "fix: enforce commitment budget for specialist attacks"
```

### Task 3: Authoritative Reinforcement Positions

**Files:**
- Modify: `TestGame/Engine.h`
- Modify: `TestGame/Engine.cpp`
- Modify: `TestGame/ReinforcementPacing.h`, `TestGame/ReinforcementPacing.cpp`
- Modify: `TestGame/CombatRhythmTests.cpp`

**Interfaces:**
- Produces: `enum class DungeonSpawnPlacement { ApplyRolePlacement, PreserveRequestedPosition }`
- Changes: `SpawnDungeonGrunt(entry,pos,cellW,cellH,placement = ApplyRolePlacement)`
- Produces: `bool PendingFootprintsOverlap(Vector2 lhs, Vector2 rhs, float minimumDistance)`

- [ ] **Step 1: Add failing placement-policy tests**

Add a pure `ResolveDungeonSpawnPosition(requested, roleSuggested, placement)` helper. Test that ordinary opening spawns select ApplyRolePlacement, pending/telegraphed spawns select PreserveRequestedPosition, and preserved input remains bitwise equal.

- [ ] **Step 2: Add failing reservation tests**

Test that a second pending spawn closer than the conservative footprint distance is rejected, while a separated position is accepted.

- [ ] **Step 3: Run and verify failure**

Expected: missing placement policy/overlap helper.

- [ ] **Step 4: Make telegraphed position final**

Add the placement parameter. Skip the post-spawn role teleport only for PreserveRequestedPosition. Pass it from AdvanceDungeonPendingSpawns; leave opening and non-telegraphed callers on the default behavior.

- [ ] **Step 5: Reserve batch footprints sequentially**

After reserving the first entry, include its conservative body footprint when validating the next candidate. Retry through existing position search; if no safe second position exists, create a one-enemy batch and leave the other entry queued.

- [ ] **Step 6: Verify final revalidation**

Ensure Ready-phase validation checks blockers, player distance where currently required, and other still-pending footprints without relocating the enemy away from its rendered warning.

- [ ] **Step 7: Run tests and Debug build**

Run CombatRhythmTests, EncounterPlannerTests, and Debug x64.

- [ ] **Step 8: Commit**

```powershell
git add TestGame/Engine.h TestGame/Engine.cpp TestGame/ReinforcementPacing.h TestGame/ReinforcementPacing.cpp TestGame/CombatRhythmTests.cpp
git commit -m "fix: keep reinforcement spawns on their telegraphs"
```

### Task 4: Lossless Village Asset Adjuster

**Files:**
- Modify: `TestGame/MapEditor.h`
- Modify: `TestGame/MapEditor.cpp`
- Modify: `TestGame/VillageAssetData.h`, `TestGame/VillageAssetData.cpp`
- Modify: `TestGame/VillageLayoutDataTests.cpp`
- Create: `TestGame/VillageAssetDataTests.cpp` if no focused test exists

**Interfaces:**
- Produces: `bool IsValidVillageAssetIdentifier(std::string_view name)`
- MapEditor retains a complete `VillageAssetData _loadedVillageAsset` and mutates editor-owned fields before save.

- [ ] **Step 1: Write failing identifier tests**

Assert acceptance of `VillageGate`, `flower-bed`, `road_02`, and `A1`. Assert rejection of empty strings, spaces, periods, slashes, backslashes, quotes, control characters, and non-ASCII bytes.

- [ ] **Step 2: Write failing lossless round-trip test**

Construct an asset containing colliders, typed markers, interaction service/prompt/target, door data, ambient settings, and animation. Save, load, simulate editor-owned mutations, save again, reload, and assert all non-editor fields remain identical.

- [ ] **Step 3: Run and verify failure**

Compile/run the focused VillageAssetData test. Expected: validation absent and/or round-trip metadata lost by the adjuster save path.

- [ ] **Step 4: Implement identifier validation**

Add the shared validation function in VillageAssetData. MapEditor rejects invalid names before creating or saving a layout reference and shows a visible message describing allowed characters.

- [ ] **Step 5: Preserve full loaded model**

On adjuster load, store the complete loaded asset. On save, copy that model and update only texture path, dimensions/scale, colliders, editor-exposed markers, and animation fields. Do not reconstruct a blank VillageAssetData.

- [ ] **Step 6: Preserve typed markers**

When editing marker positions, update matching marker records without replacing their type or unrelated properties. New markers receive the existing default type explicitly.

- [ ] **Step 7: Run village tests and build**

Run VillageAssetDataTests, VillageLayoutDataTests, and Debug x64.

- [ ] **Step 8: Commit only intended source/tests**

```powershell
git add TestGame/MapEditor.h TestGame/MapEditor.cpp TestGame/VillageAssetData.h TestGame/VillageAssetData.cpp TestGame/VillageAssetDataTests.cpp TestGame/VillageLayoutDataTests.cpp
git commit -m "fix: preserve village asset metadata during editor saves"
```

### Task 5: Deterministic Loading Transition Policy

**Files:**
- Create: `TestGame/LoadingTransition.h`
- Create: `TestGame/LoadingTransition.cpp`
- Create: `TestGame/LoadingTransitionTests.cpp`
- Modify: `TestGame/TestGame.vcxproj`
- Modify: `TestGame/TestGame.vcxproj.filters`

**Interfaces:**
- Produces: `enum class LoadingPhase { Idle, PresentBlack, Loading, FadingIn, Failed }`
- Produces: `struct LoadingTransitionState { phase, blackFramesPresented, actionConsumed, activityTime }`
- Produces: `bool BeginLoadingTransition(state)`
- Produces: `void MarkLoadingFramePresented(state)`
- Produces: `bool ShouldRunLoadingAction(const state)`
- Produces: `void CompleteLoadingTransition(state, bool success)`
- Produces: `void UpdateLoadingTransition(state, float dt)`

- [ ] **Step 1: Write failing state-machine tests**

Assert Begin enters PresentBlack; action is false before a presented frame; MarkLoadingFramePresented permits exactly one action; nested Begin fails; success enters FadingIn; failure enters Failed; action cannot run twice; fade completion returns Idle.

- [ ] **Step 2: Run and verify failure**

Compile LoadingTransitionTests with its test-main macro. Expected: missing policy types/functions.

- [ ] **Step 3: Implement minimal policy**

Keep it callback-free and raylib-free. Engine will own the std::function. The policy only manages phase, presentation proof, one-shot consumption, activity time, and fade completion.

- [ ] **Step 4: Add project entries**

Add LoadingTransition.cpp/.h beside other small runtime policy modules. Do not add LoadingTransitionTests.cpp to the main game project because it owns a standalone main.

- [ ] **Step 5: Run tests and Debug build**

Expected: loading tests pass and game links.

- [ ] **Step 6: Commit**

```powershell
git add TestGame/LoadingTransition.h TestGame/LoadingTransition.cpp TestGame/LoadingTransitionTests.cpp TestGame/TestGame.vcxproj TestGame/TestGame.vcxproj.filters
git commit -m "feat: add deterministic loading transition state"
```

### Task 6: Black Loading Screen Integration

**Files:**
- Modify: `TestGame/Engine.h`
- Modify: `TestGame/Engine.cpp`
- Modify: `TestGame/LoadingTransitionTests.cpp`

**Interfaces:**
- Consumes LoadingTransition policy.
- Produces on Engine: `bool QueueLoadingTransition(std::string label, std::string destination, std::function<bool()> action, GameState failureState)`
- Produces: `void UpdateLoadingOverlay(float dt)`
- Produces: `void DrawLoadingOverlay()`
- Engine stores one callback, label, destination, origin/failure state, and LoadingTransitionState.

- [ ] **Step 1: Add callback contract tests**

Using a test harness around the policy/controller boundary, assert the callback count remains zero until one black frame is marked, becomes one afterward, remains one on later updates, and failure clears the callback.

- [ ] **Step 2: Add Engine-owned queue/update logic**

Queue captures the origin state and ignores nested requests. Update suppresses ordinary state updates during PresentBlack/Loading, moves the callback out before invoking it, records success/failure, and never calls it twice.

- [ ] **Step 3: Draw the approved visual**

Clear to solid black. Center `Loading...`. Draw a compact dark rounded track beneath it and a short light segment whose horizontal position loops using activityTime. Draw optional destination text below. Do not display a percentage.

- [ ] **Step 4: Guarantee presentation ordering**

At the end of DrawLoadingOverlay, call MarkLoadingFramePresented. Ensure the main draw switch routes to this overlay whenever loading is non-idle, before destination drawing. The callback must therefore run no earlier than the next update.

- [ ] **Step 5: Integrate village transitions**

Wrap village-to-StartMainRun and any resumed path that performs synchronous generation/loading. Preserve first-visit gate blocking. Use labels such as `Entering the Forest`.

- [ ] **Step 6: Integrate world-map and prologue generation**

Move synchronous biome tileset load, dungeon generation, and room entry into queued actions for world-map selection and prologue creation. Return false from the action on existing generation failure and preserve the current user-facing error.

- [ ] **Step 7: Integrate remaining synchronous dungeon loads**

Search every direct call to Generate, GenerateHandcraftedDungeon, LoadTilesetForBiome, and StartMainRun. Wrap only paths that can block and transition screens; retain ordinary same-room/editor operations where a loading overlay would be disruptive.

- [ ] **Step 8: Clear state safely**

Reset, death, shutdown, and failure clear the callback and loading strings. Success hands off to existing dungeon fade-in without double-fading. Input and pause remain suppressed until loading completes.

- [ ] **Step 9: Run tests and builds**

Run LoadingTransitionTests, combat/village focused tests, Debug x64, and Release x64.

- [ ] **Step 10: Manual transition checks**

Verify village to new run shows black before work; world map to biome shows black; prologue shows black; the activity segment animates before work; successful loads fade in; forced generation failure returns safely with its message; repeated input cannot queue two loads.

- [ ] **Step 11: Commit**

```powershell
git add TestGame/Engine.h TestGame/Engine.cpp TestGame/LoadingTransitionTests.cpp
git commit -m "feat: show black loading overlay during dungeon setup"
```

### Task 7: Full Regression and Merge Audit

**Files:**
- Modify only if evidence requires: files touched in Tasks 1-6.
- Do not stage unrelated village assets or settings automatically.

- [ ] **Step 1: Recompile all standalone tests**

Rebuild tests from current sources rather than trusting stale executables. Run every available test and record commands/results.

- [ ] **Step 2: Build Debug and Release x64**

Expected: zero errors in both configurations.

- [ ] **Step 3: Run git integrity checks**

Run `git diff --check`, `git status --short`, and inspect diffs against the Task 1 base. Confirm transferred uncommitted work remains intact and .idea/build artifacts are unstaged.

- [ ] **Step 4: Perform focused manual gameplay matrix**

Check repeat attacks after recovery, frozen/arriving slot exclusion, Archer/Wisp/Bomber commitment limits, exact telegraph spawn positions, two-enemy batches, wall escape, village asset lossless saving, identifier rejection, and every loading path.

- [ ] **Step 5: Request whole-branch review**

Review the full hardening range against the approved spec. Fix Critical and Important findings, rerun covering tests, and perform one scoped re-review.

- [ ] **Step 6: Commit verified adjustments if needed**

Use a narrowly scoped commit describing only evidence-driven fixes. Do not push.
