# Hades-Style Enemy Combat Rhythm — SDD Progress Ledger

Plan: docs/superpowers/plans/2026-08-13-hades-style-enemy-combat-rhythm.md
Design: docs/superpowers/specs/2026-08-13-hades-style-enemy-combat-rhythm-design.md
Branch: feature/enemy-combat-rhythm (created from master at session start, preserving all pre-existing uncommitted transferred changes)

## Repo facts learned this session (do not re-derive)

- Only one VS project in TestGame.sln: `TestGame\TestGame.vcxproj` (no separate test project).
- Standalone tests are compiled ad hoc with MSVC `cl.exe`, one exe per test file, pattern proven in prior plan
  (docs/superpowers/plans/2026-07-18-elite-signature-kits-implementation-plan.md):
  ```
  cl /nologo /std:c++17 /EHsc /D<MACRO>_TEST_MAIN /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\<Impl>.cpp [more .cpp] TestGame\<Tests>.cpp /link /OUT:x64\Debug\<Tests>.exe
  x64\Debug\<Tests>.exe
  ```
  `cl.exe` is NOT on PATH in a plain shell — must run from a VS Developer environment. VS install found at:
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` (has VC.Tools.x86.x64). Use its
  `VC\Auxiliary\Build\vcvars64.bat` to initialize the environment, or `vswhere` to relocate if paths differ.
- raylib 5.5 win64 msvc16 is installed at `C:\CLibraries\raylib-5.5_win64_msvc16` (include/, lib/).
- All plan-referenced source files already exist: GameBalance.h, Enemy.h/cpp, CombatDirector.h/cpp,
  SlimeEnemy, SkeletonArcher, FlameWisp, Sporeling, Shieldbearer, Phantom, BomberImp, Warchief, LivingBlade,
  VFXManager.h/cpp, CapsuleCollision.h, EncounterPlanner.h/cpp, EncounterPlannerTests.cpp.
- Existing test files are plain `int main()` with `<cassert>` — no test framework.
- Pre-existing uncommitted changes at session start (DO NOT clobber; preserve on every edit to these files):
  modified: TestGame/CutsceneManager.cpp, TestGame/Engine.cpp, TestGame/Engine.h, TestGame/TestGame.vcxproj,
  TestGame/settings.cfg, VillageAssets/VillageGraveyard.vasset, VillageAssets/ZephsShop.vasset
  deleted: TestGame/VillageMap.cpp, TestGame/VillageMap.h, TestGame/villagemap_village.txt,
  TestGame/villagemap_village_markers.txt
  untracked: .idea/, TestGame/Rooms/Graveyard/graveyard/{e,s,n}_room.mroom
- No `superpowers` plugin/skill is installed in this Claude Code environment. Orchestrator (top-level session)
  is manually replicating subagent-driven-development: fresh general-purpose implementer agent per task,
  fresh general-purpose reviewer agent per task, fix loop before advancing, final whole-branch review at end.
- No push to GitHub without explicit user approval. Local commits per task ARE authorized (plan specifies them).
- Never use git reset --hard / checkout -- / clean / forced ops on this tree.

## Task Status

- [x] Task 1: Deterministic Engagement Policy — DONE (commits 30ff5a7a "feat: add deterministic enemy
      engagement policy", 2e869a0 "fix: restrict fragile-swarm bonus commit slot to swarm-profile candidates").
      Reviewer found the swarm bonus slot wasn't gated to per-candidate `swarm==true` (design: "only for
      fragile swarm-profile enemies"); fixed directly (base pool fills from any rank-ordered contender, bonus
      pool only fillable by swarm==true contenders, locked overflow eats into bonus pool too). Rebuilt +
      reran CombatRhythmTests.exe, exit 0. Nits from review (redundant vector copy, no explicit tie-break
      test) left as-is, non-blocking.
- [x] Task 2: Stable Commit Ownership Integration — DONE (commit 4ee1543 "feat: coordinate stable enemy
      attack commitments"). EngagementIntent and a new EngagementLatch class now live in Enemy.h (not
      CombatEngagement.h) — CombatEngagement.h already includes Enemy.h for EnemyRole, and Enemy needs both
      types BY VALUE, so the reverse include would be circular; CombatEngagement.h now just reuses both from
      Enemy.h the same way it already reused EnemyRole. Enemy gained GetRuntimeId() (assigned once in the
      constructor via a static counter, never touched by ResetForSpawn), SetEngagementAssignment/
      GetEngagementIntent/GetEngagementTarget, IsAttackLockedForEngagement(), SetSwarmProfile/IsSwarmProfile,
      and a private EngagementLatch member. CombatDirector::UpdateEnemyRuntime now builds one
      EngagementCandidate list (bosses excluded) and calls BuildEngagementAssignments once per frame, before
      the per-enemy Update loop, then calls SetEngagementAssignment on each enemy (Reposition-at-current-pos
      fallback for anything with no assignment, e.g. bosses). Enemy::HandleAttack's attack-start gate now
      checks (assigned Commit intent && latch.CanCommit()) when an assignment exists this run, falling back
      to the legacy CanTakeAttackSlot scan otherwise (HasEngagementAssignment() gate). Added tier/swarmEncounter
      fields to EnemyRuntimeContext (both default to tier 0 / no-swarm; neither Engine.cpp call site sets them
      yet — Engine.cpp was NOT touched, per the file-scope constraint). CombatEngagement.cpp was added to
      TestGame.vcxproj/.filters (Task 1 left it out; CombatDirector.cpp now calls BuildEngagementAssignments
      so the main game needs it linked) — CombatRhythmTests.cpp was deliberately NOT added to the vcxproj
      despite the plan text suggesting it, since it has its own `int main()` and would collide with main.cpp
      in the whole-game build; this matches existing precedent (EncounterPlannerTests.cpp/CombatSystemsTests.cpp
      are likewise standalone-only, never in the vcxproj). GetRuntimeId()/pooling-stability has no literal test
      (no existing test file constructs a real Enemy — they'd need a live raylib window/audio context via
      EnsureSharedResourcesLoaded) — verified by inspection instead (see CombatRhythmTests.cpp's comment block).
      Rebuilt + reran CombatRhythmTests.exe (exit 0, includes new EngagementLatch tests), CombatSystemsTests.exe
      (exit 0, needed /MD + raylib.lib linking + /DCOMBAT_SYSTEMS_TEST_MAIN — pre-existing requirement, unrelated
      to this task), and EncounterPlannerTests.exe (exit 0, needed RoomCapacity.cpp). Full Debug x64
      `msbuild TestGame.sln` build succeeds with zero errors (only pre-existing C4244 warnings).
- [x] Task 2 review follow-up — DONE (commit 4c09a30 "fix: exclude bosses from per-frame engagement assignment
      fallback"). Reviewer found `CombatDirector.cpp`'s per-enemy assignment-apply loop had no `IsBoss()` guard
      even though the candidate-building loop above it excludes bosses and the code's own comment claimed
      bosses stay `HasEngagementAssignment()==false`. Dormant bug (all 10 boss classes fully override Update()
      and never reach the affected code path), but fixed now before it could silently break a future boss
      variant. Rebuilt Debug x64, exit 0, zero errors.
      IMPORTANT FOR TASK 3: reviewer also flagged that `GetEngagementIntent()==Commit` currently covers BOTH
      "actively attacking" and "in post-attack recovery" (both map to `EngagementCandidate.locked==true` in
      Task 1's policy, and `BuildEngagementAssignments` doesn't branch on the separate `recovering` field yet).
      `ComputeEngagementTarget`/enemy positioning must NOT treat `Commit` alone as "approach the player" —
      also check `Enemy::IsCommittedToAttack()` (reads `_attacking`) to tell true mid-attack apart from
      post-attack recovery, or a recovering enemy will incorrectly keep homing toward the player instead of
      repositioning during its 0.65s recovery window (design explicitly wants recovery to reposition briefly).
- [x] Task 3: Role Positioning and Enemy Identity — DONE (commit 5529a16 "feat:
      differentiate normal enemy combat rhythms"). Files touched: Enemy.h,
      Enemy.cpp, SlimeEnemy.h/.cpp, SkeletonArcher.h/.cpp, FlameWisp.cpp,
      Sporeling.h, Shieldbearer.h, Phantom.h/.cpp, BomberImp.cpp, Warchief.h,
      LivingBlade.cpp, CombatRhythmTests.cpp, CombatDirector.cpp.
      CombatEngagement.h/.cpp were NOT touched (not needed).

      **ComputeEngagementTarget** lives in Enemy.h as an inline free function
      (NOT a CombatEngagement.h addition, and NOT defined in Enemy.cpp) —
      deliberate: it needs zero raylib texture/asset/RNG dependencies, so
      keeping it header-only means CombatRhythmTests.cpp tests it by just
      including CombatEngagement.h (which already includes Enemy.h for
      EnemyRole) — no need to link Enemy.cpp's heavy asset-loading dependency
      chain (LoadTexture/CharacterTuningStore/SfxBank/...) into the standalone
      test binary. Signature matches the plan exactly: `ComputeEngagementTarget
      (player, self, allyCentroid, role, intent, orbitAngle)`. Commit is a
      pure pass-through to `player` (existing pathfinding/attack-entry stays
      authoritative for a genuine approach — see below); Support+Tank screens
      on the ally-centroid↔player segment biased toward the player (Shieldbearer);
      Support (non-Tank) hovers near allyCentroid (Warchief); Reposition holds a
      170px lane around the player, with an Assassin-role bias that forces the
      target at least 50° off the enemy's own current bearing from the player
      (Phantom) so it never picks a point directly in front of/behind itself.

      **CombatDirector.cpp**: replaced the placeholder `kEngagementOrbitRadius`
      inline computation with a call to `ComputeEngagementTarget`, passing
      `squadDirective.allyCentroid` (already computed once per frame, reused
      unchanged) and `enemy->GetEncounterRole()`. Nothing else in that file was
      touched. **Commit-vs-recovering distinction**: implemented exactly per the
      Task 2 "IMPORTANT FOR TASK 3" note — at the CombatDirector call site,
      `recoveringFromCommit = (assignment.intent==Commit) &&
      enemy->IsAttackLockedForEngagement() && !enemy->IsCommittedToAttack()`;
      when true, the TARGET is computed as if intent were Reposition, but the
      STORED intent passed to `SetEngagementAssignment` stays the real Commit
      (so HandleAttack's slot gate and next frame's `EngagementCandidate::locked`
      still see it as owning its slot through recovery — only the positioning
      target changes, not the ownership bookkeeping).

      **Enemy::HandleMovement** (base class — this is also "Shadow Grunt":
      confirmed via Engine.cpp's `SpawnBasicEnemy`/`TryGetPooledEnemy`, which
      instantiate the plain `Enemy` class directly with no tuning name; its
      `GetBestiaryName()` resolves to "Grunt" and no `ShadowGrunt.cpp` exists).
      Added a `useEngagementTarget` gate computed the same way (Commit-but-
      recovering counts): when true, `targetPos` is overridden to a LOCALLY
      HELD point (`_engagementHoldPos`), refreshed only every 1.4s — NOT
      re-read from `GetEngagementTarget()` every frame. This matters:
      `EngagementAssignment::orbitAngle` is derived from
      `CombatDirector::_engagementSequence`, which increments every frame, so
      raw per-frame orbit angles are NOT stable frame-to-frame (Task 1/2's own
      design — sequence is a tie-break/orbit seed, not meant to be constant).
      Consuming it directly every frame would make a "holding" enemy vibrate
      instead of settling on a lane. The 1.4s hold timer (same pattern as the
      pre-existing `_approachOffsetTimer`) fixes this without touching
      CombatEngagement.cpp's sequence/orbit-angle logic at all. When
      `useEngagementTarget` is false (genuine Commit approach), existing
      waypoint/approach-offset pathing runs unchanged, and the legacy squad-
      role steering block + `CanTakeAttackSlot`-based flank fallback are now
      gated off (`!useEngagementTarget`) since the shared engagement target
      already role-differentiates positioning for a non-committing enemy —
      only callers with no per-frame assignment at all (bosses, stray editor
      paths) still hit the old code paths, mirroring the existing
      `HasEngagementAssignment()` fallback pattern in `HandleAttack`.

      Two small opt-in virtual hooks added to `Enemy` (both default to
      no-op/0, so every other type is unaffected unless it overrides):
      `LocksApproachLineOnCommit()` (Slime overrides true — snapshots the
      first resolved Commit-approach target and holds that fixed line instead
      of re-tracking the player every frame, implemented via
      `_lockedApproachTarget`/`_approachLineLocked`) and
      `GetApproachLateralBias()` (Sporeling overrides 0.35 — blends a
      perpendicular component into the approach direction so its path curves
      instead of beelining, using the existing per-instance `_flankSide` for
      left/right variety).

      **Classes with an existing custom Update() override** (SkeletonArcher,
      FlameWisp, Phantom, BomberImp, LivingBlade) never call
      `Enemy::HandleMovement`, so each got a small, targeted, additive change
      to its OWN approach/state logic instead — attack functions and damage
      values were not touched in any of them:
        - SkeletonArcher: added `_relocateTimer` (0.55s). The old "inside
          comfort band → pure strafe" branch (continuous kiting) is now
          "plant (moveDir=0) unless `_relocateTimer>0`", and the timer is only
          armed right when an arrow is released (`_wantsToFire=true`); while
          relocating it steers toward `GetEngagementTarget()` when assigned
          and not Commit, else the old lateral strafe as a fallback. Retreat/
          advance branches (safety distance band) are unchanged.
        - FlameWisp: `PickTeleportSpot()` now teleports toward
          `GetEngagementTarget()` when this frame's assignment isn't Commit,
          instead of a fully random ring point — falls back to the old random
          logic only when no assignment exists. Cross-wisp lane-separation
          ("maintains spacing from other ranged enemies") was NOT implemented
          — would need the `enemies` vector threaded into a currently-`const`,
          enemies-blind helper; left as a documented simplification for a
          future pass.
        - Phantom: while PHASED, drifts toward `GetEngagementTarget()` instead
          of the player/flank-past-player line whenever it doesn't hold this
          frame's Commit intent. The tangible-range bite is now gated on
          `HasEngagementAssignment() ? (intent==Commit && latch.CanCommit()) :
          true` (mirrors `HandleAttack`'s own fallback pattern) and now calls
          `_engagementLatch.BeginCommit()`/`EndCommit()` around the
          bite/phase-out transition (tracked via a new `_bitThisCycle` bool so
          the recovery window only arms if a bite actually landed that
          tangible window) — Phantom never touched the shared latch before
          this task; it's a real (small) integration, not just a comment.
        - BomberImp: in `Seeking`, accelerates toward `GetEngagementTarget()`
          instead of the player when not holding Commit intent; the fuse
          trigger (`dist < kFuseDistance`) still always uses REAL distance to
          the player (an imp that isn't "due" is still dangerous point-blank
          — detonation/damage logic itself was not touched at all).
        - LivingBlade: the Resting→WindingUp transition (i.e. starting a new
          dash) now requires `!HasEngagementAssignment() ||
          GetEngagementIntent()==Commit`; gained matching
          `_engagementLatch.BeginCommit()` (entering WindingUp) /
          `EndCommit()` (returning to Resting, whether from a completed dash
          or a controlled-interrupt) calls. Dash mechanics/damage untouched.

      **Shieldbearer / Warchief**: no functional changes — both already carry
      the role tags (`Tank` / `Support`) that `ComputeEngagementTarget`
      keys its screening/anchoring logic on, and both already route fully
      through base `Enemy::HandleMovement`/`Enemy::Update`, so they inherit
      the new behaviour automatically. Added doc comments only, explaining the
      new meaning of their existing `GetEncounterRole()` override so a future
      reader doesn't miss the connection.

      **SetSwarmProfile wiring**: wired `true` for `SlimeEnemy` Small only
      (in `SlimeEnemy::ResetForSpawn`, after `Enemy::ResetForSpawn` has
      already zeroed it) — its own doc comment already calls it "fast,
      fragile, swarms the player." Deliberately did NOT wire it for the base
      `Enemy`/Shadow Grunt: the only spawn site (`SpawnBasicEnemy`) lives in
      Engine.cpp, out of this task's file scope, and defaulting
      `_swarmProfile=true` inside `Enemy::ResetForSpawn` itself would also
      silently tag every OTHER type that doesn't override `GetEncounterRole()`
      (BomberImp, LivingBlade, big Slime, Sporeling — none clearly "fragile
      swarm chaff"), which is over-broad. Left for a future pass where
      Engine.cpp's actual swarm-encounter spawn call site (which already
      threads a `swarmEncounter` bool into `EnemyRuntimeContext` since Task 2)
      can tag instances with real context.

      **Tests**: appended one new block to `CombatRhythmTests.cpp` (Task 1/2
      tests untouched) covering, per the plan's Step 1: Reposition stays
      outside a representative 120px collision range; Tank+Support screens
      strictly on the ally↔player segment (checked via projection); Support
      (non-Tank) stays within 150px of allyCentroid; Assassin-role Reposition
      is off-angle from its own current bearing across 4 different orbit
      angles (dot product bounded away from ±1); Ranged-role Reposition
      changes lanes across 3 different orbit angles (pairwise distance > 50px,
      no endless-retreat regression); Commit passes through to the player
      position; and a degenerate all-origin/all-role/all-intent sweep asserts
      finite (non-NaN/non-Inf) output. Compiled + ran clean (exit 0) with:
      `cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include"
      TestGame\CombatEngagement.cpp TestGame\CombatRhythmTests.cpp /link
      /OUT:x64\Debug\CombatRhythmTests.exe` (run from a vcvars64.bat-initialized
      PowerShell — Bash's cl.exe invocation still produces no visible output on
      this VS install, confirming the prior agent's note). Full Debug x64
      `msbuild TestGame.sln /p:Configuration=Debug /p:Platform=x64` also
      succeeded with zero errors (only pre-existing C4244 int/float warnings,
      same as Task 2).

      **What's left / hooks for later tasks**: no wall-safe separation yet
      (Task 6) — engagement Reposition targets are NOT currently checked
      against room geometry/props, so a computed lane point could in theory
      land inside a wall; Task 6's separation pass and/or a future nav-aware
      clamp on `ComputeEngagementTarget`'s output would need to cover this.
      No reinforcement-pacing interaction yet (Task 4) — new pending spawns
      will get a fresh `_hasEngagementAssignment=false` life until
      CombatDirector's next frame assigns them, same as any other spawn.
      `ctx.tier`/`ctx.swarmEncounter` on `EnemyRuntimeContext` are still not
      set by either real Engine.cpp call site (noted in Task 2 too) — Task 3
      did not touch Engine.cpp, so tier/swarm-aware commit budgets and the
      swarm bonus slot are effectively inert in the live game until a future
      task wires those two fields from the actual room/encounter state.
- [x] Task 3 review follow-up — DONE (commit 2034dbf "fix: honor recovery latch in LivingBlade dash gate;
      clarify screen-bias comment"). Reviewer approved with 2 nits, both fixed: (1) LivingBlade.cpp's dash
      commit gate checked `GetEngagementIntent()==Commit` but not `_engagementLatch.CanCommit()` (unlike
      Phantom's equivalent gate), so LivingBlade could re-BeginCommit ~0.1s before its own 0.55s rest timer
      actually finished (kRestDuration < kPostCommitRepositionSeconds=0.65s) — harmless to CombatDirector's
      external slot bookkeeping but shortened LivingBlade's own recovery window; fixed to match Phantom's
      pattern. (2) Enemy.h's kScreenBias comment said "screen sits nearer the player" but t=0.42 (<0.5) in
      Vector2Lerp(allyCentroid, player, 0.42f) actually sits nearer the ally — comment corrected, behavior
      unchanged (still satisfies "on the segment, biased toward the player" from Task 3's own summary).
      Rebuilt CombatRhythmTests.exe (exit 0) and full Debug x64 msbuild (exit 0, zero errors). No blocking
      findings from this review; Task 4/5/6 can build on Task 3 as-is.
- [x] Task 4: Reinforcement Pacing and Pending Spawn State — DONE (commit 11d437c "feat: stage smaller
      reinforcement batches"). Files touched: created TestGame/ReinforcementPacing.h/.cpp; modified
      TestGame/CombatRhythmTests.cpp, TestGame/EncounterPlannerTests.cpp, TestGame/GameBalance.h,
      TestGame/Engine.h, TestGame/Engine.cpp, TestGame/TestGame.vcxproj, TestGame/TestGame.vcxproj.filters.

      **ReinforcementPacing.h/.cpp** (new, dependency-light like Task 1's CombatEngagement — includes
      raylib.h for Vector2, EncounterPlanner.h for EncounterSpawnEntry, RoomCapacity.h for RoomCapacityBand;
      does NOT link EncounterPlanner.cpp/RoomCapacity.cpp, only uses their header-declared types):
      `SimultaneousBodyTarget(RoomCapacityBand)` → 3/4/5/6 Small/Medium/Large/Arena, exact plan signature.
      `ReinforcementBatchSize(active, pending, target, queued)` → `min(gap, queued, 2)` where
      `gap = target - (active+pending)`, 0 if gap<=0 or queued<=0. `PendingSpawnPhase{Circle,Smoke,Ready}` and
      `PendingEnemySpawn{entry, worldPos, phase, timer, smokeEmitted}` exactly per plan (field names/types
      unchanged from the plan's suggestion). `AdvancePendingSpawn(spawn, dt)`: adds dt to timer; Circle->Smoke
      guarded by `timer >= Balance::Rhythm::kSpawnCircleSeconds` (flips `smokeEmitted=true` in the same
      branch, so it only ever fires once — the guard requires phase still be Circle); Smoke->Ready guarded by
      `timer >= kSpawnCircleSeconds+kSpawnSmokeSeconds`; both checks run in the same call so a large single dt
      (e.g. 2.0f) walks Circle->Smoke->Ready in one call, no multi-call convergence needed; no-ops once Ready
      (timer stops accumulating). New `Balance::Rhythm::kSpawnCircleSeconds=0.65f` /
      `kSpawnSmokeSeconds=0.30f` added to GameBalance.h's existing Task-1 `Balance::Rhythm` namespace
      (constants only, nothing else in that namespace touched).

      **Engine.h**: added `#include "ReinforcementPacing.h"` next to the existing EncounterPlanner.h include;
      added `std::vector<PendingEnemySpawn> _pendingEnemySpawns;` immediately after the existing
      `_roomPressureCapDbg` field (same block as `_dungeonReinforcements`) — chosen as a plain `std::vector`
      per the plan's "simplest is fine" guidance since in practice it never holds more than 2 entries at once
      (one batch pending at a time, capped at `ReinforcementBatchSize`'s hard 2-per-batch limit, and a new
      batch is never reserved while the previous one is still pending — see below). Added three new private
      method declarations next to `SpawnDungeonGrunt`: `TryReserveDungeonReinforcementBatch(cellW, cellH)`,
      `AdvanceDungeonPendingSpawns(dt, cellW, cellH)`, `IsDungeonReinforcementPosStillValid(pos, cellW, cellH) const`.

      **Engine.cpp integration** (all four pieces the grounding called out):
      1. *Revalidation helper* `IsDungeonReinforcementPosStillValid` (new, placed right after
         `SpawnDungeonGrunt`, ~line 16463): reuses the exact existing `GetDungeonSpawnBlockers` +
         `IsDungeonEnemySpawnPositionValid(nullptr, pos, cellW, cellH, blockers)` pair, plus the same
         `cellW*4.f` minimum-player-distance threshold `GetDungeonSpawnPos` itself uses — deliberately NOT a
         new validity policy, just the existing one applied to a fixed position instead of a search.
      2. *Reservation* `TryReserveDungeonReinforcementBatch` (new, same location): bails immediately if
         `_dungeonReinforcements` is empty OR `_pendingEnemySpawns` is non-empty (this is the literal
         "no spawn telegraph batch is already pending" gate from the design — a second batch can never start
         while the first's Circle/Smoke telegraph is still running, since the pending vector only empties
         when an entry resolves to Ready and is spawned/requeued). Computes
         `target=SimultaneousBodyTarget(_roomCombatCapacity.band)`, `active=GetActiveEnemyCount()`, calls
         `ReinforcementBatchSize(active, pending.size(), target, queue.size())`, then pops that many entries
         off the FRONT of `_dungeonReinforcements`, reserves each one's position via
         `GetDungeonSpawnPosForRole(entry.role, cellW, cellH)` (role-aware placement, same as the existing
         opening-slice spawn path), and pushes a fresh `PendingEnemySpawn{phase=Circle, timer=0}` — never
         calls `SpawnDungeonGrunt` here.
      3. *Advance* `AdvanceDungeonPendingSpawns` (new, same location): calls `AdvancePendingSpawn` on every
         pending entry; entries that reach Ready are revalidated via
         `IsDungeonReinforcementPosStillValid` — valid ones call `SpawnDungeonGrunt(entry, worldPos, ...)`
         and are erased from pending; invalid ones are pushed back onto the FRONT of `_dungeonReinforcements`
         (design: "Failed reservations stay queued and retry later; they do not spawn at an unsafe fallback")
         and erased from pending without spawning.
      4. *Release-site rewrite* (~Engine.cpp:20460-20505, was "-- Reinforcement waves --"): the OLD code
         computed `bodySlots = _dungeonOpeningCap - activeEnemies` and looped `SpawnDungeonGrunt` up to that
         many times in one shot on timer/active-count eligibility — this was the exact horde-rush bug. NEW
         code keeps the same eligibility gate (`_dungeonReinforcementTimer <= 0.f ||
         activeEnemies <= Balance::Pressure::kReinforceRefillActive`, unchanged from before — timer/low-count
         still makes a batch ELIGIBLE) but on eligibility now only calls
         `TryReserveDungeonReinforcementBatch` (never `SpawnDungeonGrunt` directly), so
         `ReinforcementBatchSize` — not the old `_dungeonOpeningCap`-based `bodySlots` — is what actually
         bounds how many enemies enter play. `_dungeonOpeningCap` itself is untouched/still set (only other
         remaining use: the Holdout-room refill's `_dungeonOpeningCap / 2` sizing, unrelated to this gate).
         The old inner pressure-budget loop (`rolePressure`/`pressureSlots`/`activePressure`) was REMOVED
         entirely — the plan's grounding for this exact site describes only a body-count-gap replacement, no
         mention of preserving the pressure sub-budget; judgment call, flagged below. Separately and
         unconditionally (not nested inside the eligibility block, so it keeps running even once the queue
         drains to empty and mid-batch even before the next eligibility window), `AdvanceDungeonPendingSpawns`
         is now called every frame pending spawns exist and `!_dungeonScrolling`. The "REINFORCEMENTS!"
         floating label now fires at RESERVATION time (batch begins) rather than at actual spawn (which is
         now ~0.95s later) — a deliberate small UX judgment call, not specified either way by the design.
      5. *Opening-slice partition* (~Engine.cpp:16778-16810): added
         `immediateOpeningCount = min(openingCount, SimultaneousBodyTarget(_roomCombatCapacity.band))`.
         Both the Ancient-Castle-cluster branch and the normal branch now loop `immediateOpeningCount` times
         instead of `openingCount` times for the immediate `SpawnDungeonGrunt` calls. The overflow
         (`encounter.opening[immediateOpeningCount..openingCount)`) is prepended (in original order) to a
         fresh deque, followed by `encounter.reinforcements` unchanged, and the result replaces
         `_dungeonReinforcements` (previously just `= encounter.reinforcements`). Population is exactly
         preserved: `immediateOpeningCount` (spawned now) + `(openingCount - immediateOpeningCount)` (queued
         overflow) + `encounter.reinforcements.size()` (queued as before) == `openingCount +
         encounter.reinforcements.size()` == the same total as pre-Task-4, byte for byte — nothing is ever
         dropped, only deferred into the same telegraphed-reservation pipeline as pre-existing reinforcement
         surplus. Debug TraceLog updated to show `opening %d->%d` (was `%d, opening %d` — a) so `_debug`
         overlay users can see the new/old split; this is a log-string-only change, not a behavior change.
      6. *Room-clear correctness fix*: `allDead` in the room-clear-detection block was
         `_dungeonReinforcements.empty()` only — a pending Circle/Smoke telegraph (already popped out of the
         queue) would have been invisible to this check, letting the room be marked cleared while an enemy
         was still about to appear. Changed to `_dungeonReinforcements.empty() && _pendingEnemySpawns.empty()`.
      7. *Clear-site parity*: added `_pendingEnemySpawns.clear();` immediately beside EVERY one of the 5
         confirmed `_dungeonReinforcements.clear()` call sites (Engine.cpp, POST-Task-4 line numbers: room
         entry line 1092, editor-playtest-disabled branch in `SpawnDungeonRoomEnemies` line 16558, prologue-
         room-setup branch line 16608, `SetPlaytestEnemies` editor reset line 17019, Holdout-room-complete
         branch line 20432) — confirmed there were exactly 5 occurrences (matching the grounding's estimate
         exactly, no extra ones found; re-grep `_pendingEnemySpawns.clear()` to relocate after further edits). Also added `_pendingEnemySpawns.empty()` to the Holdout-room refill's trigger condition
         (`GetActiveEnemyCount()==0 && _dungeonReinforcements.empty() && _pendingEnemySpawns.empty()`) so a
         Holdout room doesn't double-queue cheap filler while a telegraph is still resolving. Room exit routes
         through `Engine::EnterDungeonRoom` (confirmed: line ~1092's clear site is inside this function), and
         BOTH run-reset and death route back through `EnterDungeonRoom` transitively (death →
         `UpdateDeathRevive` → always `EnterVillage()`, never an in-place same-room revive; re-entering the
         dungeon later always goes through `EnterDungeonRoom`) — so death and run-reset needed no separate
         clear site beyond the room-entry one, per the design's own "cleared on room exit, run reset, death,
         editor-playtest reset" (all four collapse to the same one or two call sites). One remaining
         pre-existing gap NOT touched (out of this task's stated file-modification scope, and it does not
         clear `_dungeonReinforcements` today either): `DebugRestartDungeonRoomAs` (a dev-only debug-panel
         room-type-swap function) does not clear either queue — pre-existing behavior, not introduced by this
         task, flagged here for awareness only.
      8. *Pause freezing*: verified BY INSPECTION rather than by adding new gating code — pausing sets
         `_gameState = GameState::Pause`, an entirely different `GameState` value, and `UpdateDungeonRun(dt)`
         (which contains every piece of code this task touched, including the new
         `AdvanceDungeonPendingSpawns`/`TryReserveDungeonReinforcementBatch` calls) is only ever invoked from
         the `case GameState::DungeonRun:` arm of the top-level state switch — so while paused,
         `UpdateDungeonRun` simply never runs at all, and telegraph timers freeze "for free" as a structural
         consequence, with zero new pause-specific code needed. (Hit-stop/slow-mo inside `UpdateDungeonRun`
         also transitively freeze/scale the new logic exactly like everything else in that function, since
         `dt` is already hit-stop/slow-mo-adjusted before reaching the new code.)
      9. *Debug overlay*: the existing `PRESSURE %d / %d   waves %d   hazards %d   shots %d/%d` HUD line (F-key
         debug panel) got one more field: `telegraphing %d` showing `_pendingEnemySpawns.size()`, so
         Task 5/6 debugging has a visible pending-count readout without needing to add one later.

      **Tests**: `CombatRhythmTests.cpp` — appended `SimultaneousBodyTarget` (all 4 bands),
      `ReinforcementBatchSize` (small gap = full gap, large gap capped at 2, at/over target = 0, pending
      counts against target, capped by queued, never negative), and `AdvancePendingSpawn` (starts Circle;
      0.64s still Circle; crossing 0.66s → Smoke + smokeEmitted flips once; staying in Smoke past that does
      NOT re-flip smokeEmitted; crossing 0.96s → Ready; further calls after Ready are a no-op — timer doesn't
      keep accumulating; a single large dt=2.0f walks Circle→Smoke→Ready in one call). `EncounterPlannerTests.cpp`
      — appended `(int)plan.opening.size() + (int)plan.reinforcements.size() == plan.debug.plannedPopulation`
      assertions to all three existing plan scenarios (tight/fewerAbilities/moreAbilities/swarm) rather than
      writing a new scenario: read `EncounterPlanner.cpp::Build` first and confirmed every `kCandidates`
      entry and `CheapFiller`'s output has `populationCost==1`, so `plannedPopulation` already equals the
      total authored ENTRY COUNT exactly, and Engine.cpp's Task 4 change only redistributes which subset of
      `encounter.opening`'s own entries get spawned immediately vs. queued at spawn time — it never adds to or
      removes from the planner's own `opening`/`reinforcements` output, so this invariant holding at the
      planner level is sufficient; no separate Engine.cpp-level population test was written (would require
      constructing a live Engine/raylib context, out of scope for these header-only assert tests, same
      precedent as every other Engine.cpp-touching task so far).

      **Build/test commands that worked** (PowerShell — Bash's cl.exe invocation still produces zero visible
      output on this VS install, confirmed again this task):
      ```
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\CombatEngagement.cpp TestGame\ReinforcementPacing.cpp TestGame\CombatRhythmTests.cpp /link /OUT:x64\Debug\CombatRhythmTests.exe'
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\EncounterPlanner.cpp TestGame\RoomCapacity.cpp TestGame\EncounterPlannerTests.cpp /link /OUT:x64\Debug\EncounterPlannerTests.exe'
      cmd /c '"...\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /MD /DCOMBAT_SYSTEMS_TEST_MAIN /I"...raylib\include" TestGame\DamageNumberManager.cpp TestGame\EncounterPlanner.cpp TestGame\RoomCapacity.cpp TestGame\CombatSystemsTests.cpp /link /LIBPATH:...raylib\lib raylib.lib winmm.lib gdi32.lib user32.lib shell32.lib /OUT:x64\Debug\CombatSystemsTests.exe'
      cmd /c '"...\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /I"...raylib\include" TestGame\RoomCapacity.cpp TestGame\RoomCapacityTests.cpp /link /OUT:x64\Debug\RoomCapacityTests.exe'
      cmd /c '"...\vcvars64.bat" && msbuild TestGame.sln /p:Configuration=Debug /p:Platform=x64 /nologo /v:minimal'
      ```
      All five exes ran with exit 0 (CombatRhythmTests, EncounterPlannerTests, CombatSystemsTests,
      RoomCapacityTests all pass; CombatSystemsTests needed `DamageNumberManager.cpp` added to its link line
      beyond what Task 2's ledger note mentioned — its own test file constructs a real `DamageNumberManager`).
      Full Debug x64 `msbuild TestGame.sln` succeeded with zero errors.

      **Hooks for Task 5 (VFX telegraph)**: `_pendingEnemySpawns` (Engine.h, next to `_dungeonReinforcements`)
      is the collection to iterate for drawing. `PendingEnemySpawn::phase` (Circle/Smoke/Ready) and
      `::worldPos` are ready to consume as-is for `DrawPendingEnemySpawns()` — draw before enemy sprites, per
      the design's "below enemy sprites but above the room floor". `smokeEmitted` is ready to consume exactly
      as specified: it is guaranteed to flip `false→true` exactly once, on the exact frame `AdvancePendingSpawn`
      transitions Circle→Smoke, and never flips again — Task 5 should call `VFXManager::SpawnSmokeBurst` at
      the point it first observes `smokeEmitted==true` for a given entry (e.g. track "did I already fire VFX
      for this" separately, or fire it unconditionally right when `AdvanceDungeonPendingSpawns` — which Task 5
      does not need to touch — transitions the phase, by adding the VFX call at the exact same call site
      inside `AdvanceDungeonPendingSpawns` in Engine.cpp guarded on the phase actually changing this call, OR
      simplest: since `smokeEmitted` only flips true on the transition frame, a caller that draws every frame
      can safely call `SpawnSmokeBurst` whenever it sees `smokeEmitted==true && phase==Smoke && timer` is
      within one frame of the transition — but the cleanest hook is to add the `VFXManager::SpawnSmokeBurst`
      call directly inside `AdvanceDungeonPendingSpawns`'s `AdvancePendingSpawn` call site in Engine.cpp,
      right where `spawn.smokeEmitted` becomes true, since Engine.cpp already owns `_vfx`). Ready-phase enemy
      instantiation and removal-from-pending is ALREADY fully implemented by this task (in
      `AdvanceDungeonPendingSpawns`) — Task 5 does not need to touch spawn-at-Ready logic, only add the
      "short arrival/orientation latch so the enemy cannot move or attack on creation frame" the design
      mentions, which is NOT yet implemented (known gap, flagged for Task 5).
      **Hooks for Task 6 (wall-safe separation)**: none directly — reservation positions already run through
      the existing blocker/min-distance validity checks (`IsDungeonReinforcementPosStillValid`), so newly
      spawned reinforcements should already be wall-safe on arrival; no additional Task 6 dependency created.

      **Known gaps / judgment calls left for review**:
      - Dropped the old pressure-budget sub-gate (`rolePressure`/`pressureSlots`) from the reinforcement
        release site entirely — the plan's grounding for this exact site only describes a body-count-target
        replacement and says nothing about preserving the old pressure throttle, so a batch can now include
        an expensive specialist without the old per-wave pressure ceiling. `ReinforcementBatchSize`'s hard cap
        of 2 and the simultaneous-body target still bound how many bodies appear, just not their combined
        `pressureCost` per wave. Flagged for Task 7's balance review if this reads as too punishing/lenient
        in playtesting.
      - "REINFORCEMENTS!" floating label now fires at batch-RESERVATION time (telegraph begins) instead of at
        actual spawn (previously simultaneous with `SpawnDungeonGrunt`) — reads correctly conceptually ("a
        wave is coming") but means the label now precedes the enemy's appearance by ~0.95s instead of being
        simultaneous with it. Not specified by the design either way; flagged as a judgment call.
      - `EncounterSpawnEntry::swarmProfile` (planner-set field) is NOT threaded into the newly-spawned
        reinforcement enemy by this task's changes — `SpawnDungeonGrunt` already reads `entry.swarmProfile`
        itself (pre-existing, line ~16457: `if (entry.swarmProfile) spawned->ApplyDifficultyScaling(...)`) and
        this task's `TryReserveDungeonReinforcementBatch` preserves the full `EncounterSpawnEntry` by value
        into `PendingEnemySpawn::entry`, so `entry.swarmProfile` reaches `SpawnDungeonGrunt` unchanged when the
        entry is finally spawned at Ready — this pre-existing wiring was NOT broken by deferring the spawn.
        The separate note from the task brief (whether to thread `entry.swarmProfile` into `Enemy::
        SetSwarmProfile()`, Task 3's mechanism) remains exactly the nice-to-have gap Task 3 already flagged —
        this task did not add or remove any wiring there, since `SpawnDungeonGrunt` itself (unchanged by this
        task) is the only place that would need it and it wasn't in this task's edit scope.
      - `DebugRestartDungeonRoomAs` (dev debug-panel room-type swap) does not clear either
        `_dungeonReinforcements` or `_pendingEnemySpawns` — pre-existing gap, not introduced by this task, out
        of stated scope, flagged for awareness only.
- [x] Task 4 review — APPROVED, no fixes needed (commit 11d437c "feat: stage smaller reinforcement batches").
      This was the first task to touch Engine.cpp/Engine.h. Reviewer independently confirmed the pre-existing
      VillageMap→VillageObjectLayer refactor (transferred uncommitted work) is fully preserved — the diff is a
      pure mechanical rename (VillageMap.h free functions → Engine:: member functions keyed on
      VillageRuntimeObjectDef fields), nothing deleted/altered. All 5 `_dungeonReinforcements.clear()` sites
      have matching `_pendingEnemySpawns.clear()` (one pre-existing, unrelated gap at
      `DebugRestartDungeonRoomAs:1157` noted but not introduced by this task). Batch/target/timing math,
      revalidation-and-requeue, and population-preservation across the opening/reinforcement partition all
      verified directly against code. Independent rebuild + CombatRhythmTests/EncounterPlannerTests/full
      Debug x64 all green.
      TWO BALANCE-RELEVANT BEHAVIOR CHANGES FLAGGED FOR TASK 7 (not bugs, deliberate judgment calls, user
      should be aware before playtesting): (1) the old per-wave pressure-budget throttle (rolePressure/
      pressureSlots, capped how many high-cost specialists could release in one wave) was REMOVED at the
      reinforcement-release site — only the new body-count/target-gap limit remains, so a batch can now
      include up to 2 high-pressure specialists back-to-back with no separate pressure ceiling. (2) the
      "REINFORCEMENTS!" floating-text callout now fires at batch-RESERVATION time instead of at actual spawn
      time, so it now precedes the enemies' visual appearance by ~0.95s (the full Circle+Smoke telegraph
      duration) rather than being simultaneous with it.
- [x] Task 5: Purple Spawn Circle and Smoke — DONE (commit 958f521 "feat: telegraph enemy
      reinforcements"). Files touched: TestGame/Engine.h, TestGame/Engine.cpp, TestGame/VFXManager.h,
      TestGame/VFXManager.cpp, TestGame/GameBalance.h, PLUS TestGame/Enemy.h/TestGame/Enemy.cpp — a
      deliberate deviation from the plan's stated file list (Engine.cpp/VFXManager only), required by the
      design's "State and Failure Handling" arrival-delay requirement that Task 4 explicitly flagged as a
      known gap and deferred to this task. TestGame/CombatDirector.cpp was NOT touched (see below — this
      matters because it's on the "do not modify" list for this task).

      **VFXManager::SpawnSmokeBurst(Vector2 worldPos, Color color)** (VFXManager.h/.cpp): a thin wrapper
      over the existing directional `SpawnImpactBurst(worldPos, color, count=10, speed=40.f,
      direction={0,-1}, spreadRadians=1.1f)` — upward bias, low speed, modest cone, caller-supplied
      purple-gray tint. Renders/culls/fades through the exact same bounded `_sparks` pool as every other
      impact burst; zero new code added to `Update`/`Draw`/`DrawFloatingTexts` (sparks are drawn from
      `DrawFloatingTexts`, confirmed by reading the existing spark-render loop before wiring this up). No
      asset loaded.

      **Engine::DrawPendingEnemySpawns(Vector2 worldOffset) const** (new, Engine.cpp, placed directly after
      `AdvanceDungeonPendingSpawns`; declared in Engine.h next to the other reinforcement-pacing private
      methods): iterates `_pendingEnemySpawns`, converts `spawn.worldPos` to screen space using the exact
      same `screenPos = worldPos + worldOffset; screenPos.x += kVirtualWidth/2; screenPos.y +=
      kVirtualHeight/2;` pattern every sibling `Draw(worldOffset)` call in that block uses (EnemyProjectile,
      SpreadProjectile, VFXManager::Draw, etc. — confirmed by reading several before choosing). Took
      `worldOffset` AS A PARAMETER (not recomputed internally) per the task's own steer, matching
      `_vfx.Draw(worldOffset, ...)`'s style since it's called from the same block that already computes it.
      Circle phase: a filled circle whose radius/alpha "breathes" via `sinf(spawn.timer * 6.5f)` (spawn.timer
      is the SAME timer PendingEnemySpawn already tracks — no GetTime()/wall-clock read anywhere, so
      pause/hit-stop/slow-mo freeze this exactly like the rest of UpdateDungeonRun, for free) PLUS two
      `DrawCircleLinesV` rings on a 0.45s repeating cycle (`fmodf(elapsed [+ half-cycle offset for the second
      ring], period)/period`), radius growing 0→46px, alpha fading to 0 as each ring expands — a continuous
      pulse rather than one static ring, matching "one or two expanding outline rings" and reading as
      constantly pulsing rather than looping visibly. Smoke phase: `t = (spawn.timer -
      kSpawnCircleSeconds)/kSpawnSmokeSeconds` (0→1) drives radius shrinking 20px→6px and alpha rising
      0.4→1.0 — "contracts and brightens" per the design, literally. Ready phase: skipped (defensive
      `continue`, not expected to actually hit in practice since AdvanceDungeonPendingSpawns removes Ready
      entries the same frame — matches the task brief's own guidance to handle it gracefully rather than
      assume it can't happen). A single `constexpr Color kSpawnTelegraphColor{170,110,210,255}` (purple-gray)
      is declared file-scope in Engine.cpp right before `IsDungeonReinforcementPosStillValid` and shared by
      both the circle draw AND the smoke-burst emission call below, so the whole telegraph reads as one
      consistent color motif.
      **Call site**: `DrawPendingEnemySpawns(worldOffset);` inserted directly after `_vfx.Draw(worldOffset,
      ...)` and before the pickup-draw loop (~Engine.cpp line 21005-21007), i.e. AFTER room hazards/floor
      tiles/props (drawn earlier in the same block) and BEFORE the `enemy->DrawEnemy(shakenCamRef)` loop
      (drawn later in the same block) — exactly the design's "below enemy sprites but above the room floor"
      ordering, verified by reading the surrounding draw order before placing the call.

      **Smoke emission** (Engine.cpp, inside `AdvanceDungeonPendingSpawns`, the exact hook point Task 4's
      ledger called out): `bool wasEmitted = spawn.smokeEmitted;` captured BEFORE calling
      `AdvancePendingSpawn(spawn, dt)`, then `if (!wasEmitted && spawn.smokeEmitted)
      _vfx.SpawnSmokeBurst(spawn.worldPos, kSpawnTelegraphColor);` right after — fires exactly once, on the
      exact frame `smokeEmitted` flips false→true (guarding on the flip itself rather than `phase==Smoke`
      means even a single large-dt frame that walks the whole Circle→Smoke→Ready state machine in one
      `AdvancePendingSpawn` call still only emits once, matching ReinforcementPacing's own one-time-flip
      guarantee). `ReinforcementPacing.h/.cpp` itself was NOT touched — confirmed out of scope and unneeded,
      per this task's own file-list constraint.

      **Arrival/orientation latch** (design requirement, flagged as an explicit known gap by Task 4 — "NOT
      implemented... flagged for Task 5"): added `float _arrivalTimer = 0.f` to `Enemy` (Enemy.h, next to the
      PitFall fields it's modeled after) plus three small inline methods: `BeginArrivalDelay(float seconds)`,
      `UpdateArrivalDelay(float dt)`, `IsArriving() const`. Reset to 0 in `Enemy::ResetForSpawn` (pooled reuse
      must not inherit a stale arrival delay from a previous pooled life). New
      `Balance::Rhythm::kSpawnArrivalDelaySeconds = 0.20f` added to GameBalance.h beside the existing
      Task 1/4 Rhythm constants (judgment call: 0.20s — brief enough to read as a beat, not a stun; sits
      comfortably inside the design's "short" wording and the task brief's suggested 0.15-0.25s range).
      `AdvanceDungeonPendingSpawns`'s Ready-phase branch now captures `Enemy* spawned =
      SpawnDungeonGrunt(...)` and calls `spawned->BeginArrivalDelay(Balance::Rhythm::kSpawnArrivalDelaySeconds)`
      immediately after, guarded on `spawned != nullptr`. This ONLY applies to reinforcement-path spawns
      (the call site inside `AdvanceDungeonPendingSpawns`) — the two other `SpawnDungeonGrunt` call sites
      (opening-slice immediate spawns, ~Engine.cpp lines 16804/16813/16819, now shifted slightly by this
      task's edits) were deliberately left untouched: they have no telegraph and the design's arrival-delay
      wording is specifically about telegraphed reinforcements, not room-opening spawns.

      **IMPORTANT JUDGMENT CALL — where the actual movement/attack gate lives**: the task brief offered two
      options for shape/spirit ("(A) CombatDirector::UpdateEnemyRuntime's per-enemy loop skips this enemy
      like PitFall, OR (B) Enemy::Update/HandleAttack/HandleMovement early-out internally"). Chose (B) and
      did NOT touch CombatDirector.cpp at all, because this task's own explicit "Do not modify or stage"
      list names `TestGame/CombatDirector.*` — touching it to add a PitFall-style skip there would have
      violated that constraint directly. Implemented instead as a small in-place branch inside the shared
      `Enemy::Update` (Enemy.cpp, replacing the two-line `HandleMovement(...); HandleAttack(...);` call with
      `if (_arrivalTimer > 0.f) UpdateArrivalDelay(dt); else { HandleMovement(...); HandleAttack(...); }`) —
      genuinely a true movement/attack gate (not just a rendering pause): while arriving, the enemy's
      steering/pathing and attack-start logic are both skipped entirely, while hit reactions, status ticks,
      knockback, and animation still run normally so the enemy doesn't look frozen/broken if hit during its
      brief arrival window.
      **KNOWN GAP, flag for Task 6/7 and any future reviewer**: this early-out lives in the SHARED
      `Enemy::Update` path only. Per Task 3's own ledger notes, FIVE enemy subclasses
      (SkeletonArcher, FlameWisp, Phantom, BomberImp, LivingBlade) have a fully custom `Update()` override
      that never calls `Enemy::Update`/`HandleMovement`/`HandleAttack` at all — confirmed by grep before
      writing this note. For those five types, `_arrivalTimer` counts down correctly (nothing decrements it
      except `UpdateArrivalDelay`, which is only called from the gated branch above, so for these five it
      would just sit un-decremented) — meaning **a telegraphed reinforcement of one of these five types
      currently gets NO enforced arrival delay at all**: it can move/attack immediately on its spawn frame,
      identically to pre-Task-5 behavior. Grunt/Slime/Sporeling/Shieldbearer/Warchief (which route fully
      through base `Enemy::Update` per Task 3's own findings) DO get the enforced delay correctly. Extending
      this to the other five would mean adding a matching one-line early-out inside each of their five
      `Update()` overrides (SkeletonArcher.cpp/FlameWisp.cpp/Phantom.cpp/BomberImp.cpp/LivingBlade.cpp) —
      deliberately NOT done here: those five files are not in this task's stated file list, the task brief
      explicitly said "keep this change small and additive... do not restructure Enemy's update pipeline,"
      and the commit-message file list in the task brief itself only names Enemy.h/Enemy.cpp as the
      justified deviation. Flagged here explicitly so Task 7's balance/verification pass (or a human) can
      decide whether this partial coverage is acceptable or needs a follow-up patch.

      **Tests**: no new automated tests added — this task is pure rendering + a small runtime-only gameplay
      gate, and per the task brief's own framing ("no automated test suite covers rendering — do what you
      can"), CombatRhythmTests.cpp/ReinforcementPacing.h/.cpp were deliberately left untouched (out of this
      task's stated scope; Task 4 already covers the underlying phase/timer state machine exhaustively).
      Rebuilt and reran `CombatRhythmTests.exe` (exit 0) purely as a regression check since it transitively
      depends on `Enemy.h` for `EngagementIntent`/`ComputeEngagementTarget`; also rebuilt/reran
      `EncounterPlannerTests.exe` (exit 0) as a regression check since `GameBalance.h` changed.

      **Build/verification commands that worked**:
      ```
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\CombatEngagement.cpp TestGame\ReinforcementPacing.cpp TestGame\CombatRhythmTests.cpp /link /OUT:x64\Debug\CombatRhythmTests.exe'
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\EncounterPlanner.cpp TestGame\RoomCapacity.cpp TestGame\EncounterPlannerTests.cpp /link /OUT:x64\Debug\EncounterPlannerTests.exe'
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && msbuild TestGame.sln /p:Configuration=Debug /p:Platform=x64 /nologo /v:minimal'
      ```
      Both standalone exes exit 0. Full Debug x64 `msbuild` succeeded with zero errors (only pre-existing
      C4244 int/float warnings in Warchief.cpp etc., same as every prior task). NOTE: same as every prior
      task, Bash's `cl.exe`/`msbuild` invocations via vcvars64.bat produce no visible captured stdout on this
      VS install (confirmed again) — PowerShell's `cmd /c '...' 2>&1 | Out-String | Set-Content` was needed
      to actually see msbuild's output and confirm zero errors/only-pre-existing-warnings; Bash was still
      used successfully for the standalone `cl.exe` builds by checking the resulting .exe's existence and
      running it directly (exit code) rather than reading captured stdout.
      **Manual smoke test**: launched the real `x64\Debug\TestGame.exe` via PowerShell `Start-Process`,
      confirmed it stayed running (did not crash/assert) for 4 seconds, then terminated it. Did NOT verify
      the actual visual telegraph in a live dungeon room — that would require manually playing through the
      village → dungeon → surviving to a reinforcement wave, which was not practical to automate in this
      environment (no input-injection/screenshot tooling available for a native raylib window on this
      platform, and no project skill exists for driving this app — checked via the `run` skill's project-skill
      lookup, found none). The full zero-error Debug x64 build plus the "launches and doesn't immediately
      crash" check is the evidence available for this task; flagged as weaker than an actual observed run,
      per the task brief's own instruction to say so explicitly rather than skip silently.

      **Diff-integrity check**: `git diff 11d437c -- TestGame/Engine.cpp TestGame/Engine.h` after this task's
      edits shows exactly ONE removed line total (the old bare `SpawnDungeonGrunt(spawn.entry, spawn.worldPos,
      cellW, cellH);` call inside `AdvanceDungeonPendingSpawns`, replaced by the `Enemy* spawned = ...` +
      arrival-delay wrapper) — every other change in both files is a pure addition. `VillageRuntimeObjectDef`
      (the marker for the pre-existing VillageMap→VillageObjectLayer refactor) still appears 26 times in
      Engine.cpp and 9 times in Engine.h, matching pre-task counts — the transferred refactor is fully intact.
- [x] Task 5 review follow-up — DONE (commit ed854a1 "fix: honor reinforcement arrival latch in specialist
      enemy Update overrides"). Reviewer assessed the self-flagged arrival-latch gap as LIVE, not dormant:
      traced EncounterPlanner.cpp's kCandidates pool and confirmed SkeletonArcher/FlameWisp/Phantom/
      BomberImp/LivingBlade are unrestricted candidates for plan.reinforcements (no opening-only
      restriction), and Engine::SpawnDungeonGrunt is the single spawn function used by both the immediate
      and Ready-phase reinforcement paths — so a telegraphed reinforcement of any of these 5 types could
      move/attack instantly on its creation frame, contradicting the design's arrival-delay requirement for
      roughly half the roster. Fixed by adding the same `if (_arrivalTimer > 0.f) { UpdateArrivalDelay(dt);
      return; }` gate (matching the shared Enemy::Update pattern) into each of the 5 subclasses' own fully-
      overridden Update(), right after their existing `_target == nullptr` guard, before any movement/attack
      state-machine logic — housekeeping above that point (ApplyVelocity, UpdateHit, burns, freeze) still
      runs normally each frame; only movement/attack intent is withheld for the ~0.20s arrival window,
      matching the base class's scope exactly. Rebuilt full Debug x64 (zero errors) and CombatRhythmTests.exe
      (exit 0). No blocking findings remain from Task 5's review.
- [x] Task 6: Wall-Safe Separation — DONE (commit ce4993e "fix: prevent enemy bodies pinning player to
      walls"). Files touched: created TestGame/CombatSeparation.h/.cpp; modified TestGame/CombatRhythmTests.cpp,
      TestGame/Engine.cpp, TestGame/Engine.h, TestGame/TestGame.vcxproj, TestGame/TestGame.vcxproj.filters.

      **CombatSeparation.h/.cpp** (new, dependency-light like Task 1/4's modules — only includes raylib.h for
      Vector2, no Enemy/Engine/CapsuleCollision coupling): `SeparationMove{ Vector2 enemyDelta; Vector2
      playerDelta; }` and `SeparationMove ChooseBodySeparation(Vector2 playerOutMtv, bool enemyMoveValid, bool
      deepOverlap)`, exact plan signature. Pure geometry policy, three branches:
        - `enemyMoveValid==true` (ordinary case): `enemyDelta = -playerOutMtv` (enemy moves the full
          separation distance away from the player), `playerDelta = {0,0}` — matches unconditionally
          regardless of `deepOverlap` (deepOverlap must not matter once the enemy move itself is valid).
        - `enemyMoveValid==false && deepOverlap==false` (shallow invalid overlap): both deltas `{0,0}` — hold
          position, don't push anyone; a future frame/different geometry resolves it.
        - `enemyMoveValid==false && deepOverlap==true` (deep invalid overlap): `playerDelta` gets the
          smallest necessary correction — clamped to `min(6px hard ceiling, 20% of |playerOutMtv|)` in the
          direction of `playerOutMtv` (away from the enemy, never toward it); `enemyDelta` stays `{0,0}` (the
          enemy had nowhere clean to go). The dual clamp (fixed pixel ceiling AND a fraction of the raw MTV)
          guarantees `|playerDelta| < |playerOutMtv|` unconditionally for any positive MTV magnitude, which is
          exactly what the plan's own test-list wording ("smallest player fallback... bounded") requires and
          what the fixed CombatRhythmTests.cpp assertions verify directly.

      **Tests** (appended to CombatRhythmTests.cpp, Task 1/2/3/4 tests untouched): ordinary overlap negates
      exactly onto enemy with player untouched (checked for both deepOverlap=false and deepOverlap=true, since
      deepOverlap must not matter when enemyMoveValid is true); invalid+shallow overlap leaves both deltas at
      exactly zero; invalid+deep overlap produces a nonzero, strictly-smaller-than-the-full-MTV playerDelta
      pointed in the same direction as playerOutMtv, plus a large-MTV (500px) case proving the fallback is
      capped by a small absolute ceiling and not just a proportional fraction; a four-simultaneous-contacts
      test that sums `playerDelta` across four `ChooseBodySeparation` calls (using four different MTVs, worst
      case all `enemyMoveValid=false/deepOverlap=true` — the only branch that ever touches playerDelta) and
      asserts the summed magnitude is far less than what four full ordinary-case player pushes (four full MTVs
      applied directly, i.e. the OLD buggy behavior) would have totaled — plus a second pass showing the far
      more common all-`enemyMoveValid=true` case sums to exactly zero. Compiled + ran clean (exit 0):
      `cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\CombatEngagement.cpp
      TestGame\ReinforcementPacing.cpp TestGame\CombatSeparation.cpp TestGame\CombatRhythmTests.cpp /link
      /OUT:x64\Debug\CombatRhythmTests.exe` (PowerShell, vcvars64.bat-initialized — Bash's cl.exe invocation
      still produces no output on this VS install, confirmed again this task).

      **Engine.cpp integration** (`Engine::UpdateDungeonRun`, the exact site the plan identified, confirmed at
      line 20355-20356 pre-edit — matches the plan's predicted ~20358 almost exactly, only shifted by Task 5's
      earlier edits): replaced the buggy block (`HandlePlayerMeleeDamage(); ResolveDungeonEnemyCollisions();`
      followed by the per-enemy loop that re-read `_player.GetWorldPos()` fresh every iteration and applied
      the FULL MTV directly to the player with zero wall re-validation — the literal root cause of the
      wall-trap bug) with:
        1. `HandlePlayerMeleeDamage();` unchanged, still runs first (melee still sees pre-wall-correction enemy
           positions this frame, identical to before — the reorder below doesn't touch this).
        2. **New separation block**, gated on `!_player.IsDashing()` (preserved exactly, dash still passes
           through enemies): captures `stablePlayerPos`/`stablePlayerCapsule` ONCE before the enemy loop (every
           contact this frame is measured against the same pre-resolution player, not a mid-loop-mutated one —
           this alone kills the "compounding pushes" bug). For each `enemy->IsActive() && enemy->IsAlive()`
           enemy overlapping the player (both guards preserved exactly, same as before) that is NOT skipped by
           `_player.IsBeingForcedPushed()` (guard preserved exactly, same position in the loop as the original
           code — authored forced-push attacks are completely untouched by this block, they skip it entirely
           and keep their own knockback mechanic):
             - Computes `mtv` via `CheckCapsuleCapsule(stablePlayerCapsule, enemy->GetCapsule(), mtv)` (same
               call, now against the stable capsule instead of a live one).
             - `enemyMoveValid`: tentatively computes `candidateEnemyPos = enemyPos - mtv` (the position the
               enemy would land at if moved the full ordinary-case separation distance) and checks it via the
               ALREADY-EXISTING `IsDungeonEnemySpawnPositionValid(enemy.get(), candidateEnemyPos, cellW, cellH,
               blockers)` (Task 4's reinforcement-placement validity check, reused wholesale — zero new wall/prop
               detection code written for this). Blockers (`GetDungeonSpawnBlockers`) are computed lazily via a
               small local lambda only if at least one enemy actually overlaps the player this frame (cheap
               short-circuit for the common no-contact case).
             - `deepOverlap`: `Vector2Length(mtv) > 24.f`. Reasoning documented inline in Engine.cpp: default
               enemy capsule radius is 36px (`Enemy::GetCapsule`), player's is 50px (`BaseCharacter::GetCapsule`);
               24px (two-thirds of the smaller, enemy-side radius) separates "normal jostling" (a few px up to
               roughly a third of those radii) from "a genuinely deep embed" (e.g. spawned/shoved well inside
               the player) that enemy-only correction may not resolve cleanly.
             - Calls `ChooseBodySeparation(mtv, enemyMoveValid, deepOverlap)`. Applies `move.enemyDelta` via
               `enemy->Teleport(enemyPos + enemyDelta)` (matches how `ResolveDungeonEnemyCollisions` itself
               moves enemies — `Teleport`, not `SetWorldPos`, confirmed by reading that function first).
             - **No-summed-player-pushes guarantee, TWO independent layers**: (a) `ChooseBodySeparation` itself
               only ever returns nonzero `playerDelta` in the rare deep-overlap-fallback branch, clamped small;
               (b) on top of that, Engine.cpp's integration accepts AT MOST ONE contact's `playerDelta` per
               frame via a `playerFallbackApplied` bool set on the first nonzero `playerDelta` seen and checked
               before accepting any subsequent one — so even in a pathological all-deep-overlap frame, only one
               small nudge is ever collected into `totalPlayerDelta`, never a sum across multiple contacts. This
               makes the "four contacts don't sum four player pushes" property true by construction at the
               integration site, not just probabilistically true from the policy function's own small clamp.
        3. **Player fallback application + immediate re-validation**: if `totalPlayerDelta != {0,0}`, computes
           `candidatePlayerPos = stablePlayerPos + totalPlayerDelta` and a matching `candidatePlayerBody`
           (offsetting the player's real `GetCollisionRec()` by the same delta), then validates it via
           `IsRoomSpawnAreaValid(_dungeonRoomLayout, candidatePlayerBody, cellW, cellH, blockers)` — THE SAME
           free function `IsDungeonEnemySpawnPositionValid` itself wraps (RoomCollision.h/.cpp, confirmed it
           checks room bounds + extra blockers (props) + `room.colliders` (walls), no player-distance logic) —
           applied here directly to the player's own real body instead of through the enemy-specific wrapper.
           `_player.SetWorldPos(candidatePlayerPos)` only fires if valid; if the tiny nudge would itself land in
           geometry, it's simply skipped rather than applied (the nudge exists only to unstick a pinch, never to
           push the player into a wall — a stricter guarantee than "apply then correct"). **Judgment call**: this
           reuses `IsRoomSpawnAreaValid` (an existing, already-battle-tested free function) rather than either
           (a) inventing a new player-wall-correction system or (b) trying to extract the ~160-line inline
           player-movement-collision block at Engine.cpp:19971-20135 into a reusable function (which was
           considered and rejected — that block also handles doors, handcrafted-room interior tiles via
           `ResolveHandcraftedTileMovement`, and pit-fall state, none of which apply to a few-pixel geometry
           nudge, and extracting it would be a much larger, riskier restructure than this task's "small,
           additive edits only" constraint allows for a rarely-hit fallback path).
        4. **Ordering decision — reordered, not a second call**: `ResolveDungeonEnemyCollisions()` (wall/prop
           correction for enemies) now runs ONCE, immediately AFTER the new separation block, instead of
           immediately BEFORE it as in the pre-Task-6 code. This is a literal reading of the plan's own Step 4
           wording — "Capture one stable player position, move ordinary enemies outward, then run dungeon enemy
           collision" — and was chosen over adding a SECOND redundant call after the block (the task brief's own
           explicitly preferred option) because a single reordered call already achieves full coverage: it
           still corrects that frame's earlier AI-movement wall violations (nothing about the reorder changes
           what `ResolveDungeonEnemyCollisions` itself checks) AND now additionally catches any enemy this
           block's `enemy->Teleport(enemyDelta)` calls pushed toward a wall/prop, in the same frame, before that
           position is used for the next block (the room-bounds clamp) or drawn. `HandlePlayerMeleeDamage()`
           was verified unaffected: it ran before `ResolveDungeonEnemyCollisions()` in the OLD code too, so
           melee's view of enemy positions this frame is unchanged by the reorder.

      **Build/test commands that worked** (PowerShell only — Bash's cl.exe/msbuild invocations via vcvars64.bat
      continue to produce no visible output on this VS install, confirmed yet again this task):
      ```
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\CombatEngagement.cpp TestGame\ReinforcementPacing.cpp TestGame\CombatSeparation.cpp TestGame\CombatRhythmTests.cpp /link /OUT:x64\Debug\CombatRhythmTests.exe'
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cl /nologo /std:c++17 /EHsc /I"C:\CLibraries\raylib-5.5_win64_msvc16\include" TestGame\EncounterPlanner.cpp TestGame\RoomCapacity.cpp TestGame\EncounterPlannerTests.cpp /link /OUT:x64\Debug\EncounterPlannerTests.exe'
      cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && msbuild TestGame.sln /p:Configuration=Debug /p:Platform=x64 /nologo /v:minimal'
      ```
      `CombatRhythmTests.exe` and `EncounterPlannerTests.exe` both exit 0. Full Debug x64 `msbuild` succeeded
      with zero errors (first attempt failed with an expected `LNK2019 unresolved external ChooseBodySeparation`
      until `CombatSeparation.cpp`/`.h` were added to `TestGame.vcxproj`/`.filters` beside the existing
      `CombatEngagement`/`ReinforcementPacing` entries — second attempt built clean). Only pre-existing warnings
      remain (C4244 int/float conversions, plus one pre-existing LNK4098 MSVCRT-conflicts-with-other-libs
      warning, same class of warning noted in every prior task).

      **Manual smoke test**: launched `x64\Debug\TestGame.exe` via PowerShell `Start-Process`, confirmed it
      stayed running (no crash/assert) for 5 seconds, then terminated it cleanly. Did NOT verify the actual
      wall-trap fix visually in a live dungeon room (no input-injection/screenshot automation available for a
      native raylib window on this platform/environment, same limitation every prior task's ledger entry
      flagged) — the unit tests on `ChooseBodySeparation` prove the policy math in isolation, and the
      Engine.cpp integration was verified by careful reading (guard preservation, control flow, reuse of
      already-correct existing functions) plus a clean zero-error full build, but the real-world gameplay feel
      of "does an enemy actually get shoved off the player near a wall instead of pinning the player" needs
      human playtesting per the plan's own Task 7 framing. Flagged explicitly here as weaker evidence than an
      observed run, per the task brief's own instruction to say so rather than skip silently.

      **Diff-integrity check**: `git diff ed854a1 -- TestGame/Engine.cpp TestGame/Engine.h` shows exactly 4
      removed lines total, ALL inside the intentionally-replaced buggy block (the old `ResolveDungeonEnemyCollisions();`
      call at its old position, the old direct `CheckCapsuleCapsule(_player.GetCapsule(), ...)` call, and the
      old `Vector2 ppos = _player.GetWorldPos();` / `_player.SetWorldPos(...)` pair) — every other line in both
      files is a pure addition (the new separation block, the new `#include "CombatSeparation.h"` in Engine.h,
      and the relocated `ResolveDungeonEnemyCollisions();` call). `VillageRuntimeObjectDef` (the marker for the
      pre-existing VillageMap→VillageObjectLayer refactor) still appears exactly 26 times in Engine.cpp and 9
      times in Engine.h — identical counts to the pre-Task-6 tip (commit ed854a1) — confirming the transferred
      refactor and every prior task's work is fully intact.

      **Forced-push/boss-knockback preservation, explicitly verified**: `_player.IsBeingForcedPushed()` is
      still checked per-enemy inside the loop, in the same position relative to the MTV check as the original
      code, and its effect is identical — any enemy whose overlap is detected while the player is mid-forced-push
      simply `continue`s past this block entirely, touching neither `enemyDelta` nor `playerDelta` for that
      contact; the forced-push mechanic itself (`Character`/`Enemy`'s own knockback application and
      `OnForcedPushCollision`) lives entirely outside this block and was not touched by this task. Rushing
      Ogre / dashing Molarbeast special-casing inside `ResolveDungeonEnemyCollisions` (unchanged function body,
      only its call site moved) is likewise fully preserved — this task did not modify
      `ResolveDungeonEnemyCollisions` itself in any way, only where it is called from.

      **What's left for Task 7**: the two Task 4-flagged balance judgment calls (dropped per-wave pressure
      budget; "REINFORCEMENTS!" label timing) remain open, unrelated to this task. New for Task 6: the 24px
      deep-overlap threshold and the 6px/20%-of-MTV player-fallback clamp are both judgment calls with
      documented reasoning but no in-game playtesting evidence behind the exact numbers — Task 7's gameplay
      matrix (specifically "three melee enemies beside every wall and a prop" from this task's own Step 6,
      which is now effectively deferred to Task 7 since no interactive input/automation was available this
      session either) should verify these feel right: enemies visibly get shoved off the player near a wall
      rather than the player sticking to the wall, the player is never visibly pushed into geometry, and an
      authored forced-push boss attack still knocks the player back and stops flush at a wall exactly as before.
- [x] Task 6 review — APPROVED WITH NITS, no fixes required (commit ce4993e "fix: prevent enemy bodies
      pinning player to walls"). Reviewer independently verified the single most important thing in this
      task: `enemyMoveValid` is sourced from `Engine::IsDungeonEnemySpawnPositionValid` (pure room/blocker
      geometry check, NO minimum-player-distance component), not the distance-gated
      `TryFindDungeonEnemySpawnPosition` wrapper — confirming the "ordinary case" (move the enemy, not the
      player) is genuinely reachable near the player and the fix isn't silently defeated. Also independently
      confirmed: stable player position captured once before the per-enemy loop (root cause of the original
      bug eliminated), exactly one `ResolveDungeonEnemyCollisions()` call site (reordered, not duplicated),
      single-fallback-per-frame guard, re-validation of the player nudge against room geometry before commit,
      dash-passthrough and forced-push exceptions preserved unchanged, zero boss-knockback code touched.
      VillageRuntimeObjectDef marker counts (26/9) unchanged — pre-existing content fully preserved.
      Independent rebuild + CombatRhythmTests/EncounterPlannerTests/full Debug x64 all green. Nits (non-
      blocking, no fix applied): the 6px fallback ceiling resolves a genuinely deep overlap incrementally
      over a few frames rather than in one shot (acknowledged design tension, converges since player position
      is recaptured each frame); no dedicated integration test for the `playerFallbackApplied` per-frame
      guard (verified by code reading instead, matches the plan's stated test scope); the "default 36px
      enemy capsule" rationale for the 24px deep-overlap threshold is only the Enemy base-class fallback,
      several subclasses override it — doesn't affect the constant's correctness, just its stated rationale.
      MANUAL PLAYTEST NEEDED (flagged by both implementer and reviewer): the actual wall-trap fix has not
      been visually confirmed in a live dungeon room — no input-injection/screenshot automation was available
      to drive the built game. Task 7 should include this as a manual verification item: three melee enemies
      beside every wall and a prop, confirm parallel/away movement, enemy outward separation, unchanged dash,
      forced boss knockback still stops at walls.
- [x] Task 7: Full Verification and Balance Review — DONE.
      Step 1 (regression suite): all 19 standalone TestGame/*Tests.cpp files compile and pass (exit 0),
      including the 2 this branch modified (CombatRhythmTests.cpp, EncounterPlannerTests.cpp). Zero
      pre-existing or branch-caused failures.
      Step 2 (builds): Debug x64 and Release x64 both build with zero errors (Release built successfully for
      the first time this session — no warnings-as-errors, no Release-only issue).
      Final whole-branch review (fresh reviewer, independent of all 6 per-task reviews) verdict: SHIP WITH
      CAVEATS. Cross-checked all 9 of the user's literal required outcomes against merged code: 8 fully
      delivered, 1 (commit-limit escalation "2 early and 3 later, with the fragile-swarm exception") found
      only PARTIALLY delivered — EnemyRuntimeContext::tier/swarmEncounter were always 0/false at the only two
      construction sites in Engine.cpp (one live, one dead code), so the 3-committer escalation and swarm
      bonus slot never activated in real gameplay despite being fully implemented and correctly tested at
      the policy level since Task 1. FIXED (commit 1e3ba8f "fix: wire real per-room tier/swarm into
      engagement policy"): eCtx.tier now derives from the same depth formula CombatDirector::SpawnEnemies
      already uses ((currentAct-1)*5+currentRoom, banded <=2/<=6/else), eCtx.swarmEncounter now reads
      `_currentEncounterProfile == EncounterProfile::Swarm`. Rebuilt Debug+Release x64, both zero errors.
      Other reviewer findings, NOT fixed (deliberate — balance/design judgment calls belong to the user, not
      unilateral changes; also a genuinely out-of-scope pre-existing dead-code observation):
      - Dropped per-wave pressure-budget throttle at the reinforcement-release site (Task 4) — batches can
        now include 2 high-pressure specialists back-to-back with no separate pressure ceiling.
      - "REINFORCEMENTS!" floating label now fires ~0.95s before the enemy actually appears (Task 4).
      - 6px/20%-of-mtv player-fallback clamp for deep enemy-player overlaps resolves incrementally over a
        few frames rather than instantly (Task 6, self-acknowledged design tension, not a bug).
      - FlameWisp cross-wisp lane separation not implemented (Task 3, self-flagged as unimplemented).
      - Found (not introduced by this branch, not fixed): `Engine::HandleCollisions()` at Engine.cpp:~4853
        contains the EXACT old wall-trap bug pattern Task 6 fixed elsewhere, but is dead code — its only
        caller `Engine::UpdateGamePlay` has zero call sites (`GameState::Play` has no case arm in the main
        switch). Flagged as a follow-up cleanup candidate (delete the dead path, or apply Task 6's fix there
        too if `GameState::Play` is ever revived) — explicitly out of this branch's scope.
      git diff --check clean; git status --short shows only the same pre-existing uncommitted transferred
      files documented at session start; nothing lost or overwritten across all 7 tasks and 12 commits.
      MANUAL PLAYTEST STILL NEEDED (no input-automation/screenshot tooling was available to any agent this
      session — every visual/feel claim in this branch is verified by code reading and build success only,
      never by an actual play session): wall-trap repro (3 melee enemies beside every wall+prop, confirm
      enemies get shoved not the player, dash passes through, forced boss knockback still stops at walls),
      reinforcement telegraph visual timing across all 10 enemy types (especially the 5 specialist types
      whose arrival latch was patched in separately from the shared Enemy::Update path), room transition
      mid-telegraph, swarm-room feel now that the bonus slot is actually live, ranged-heavy group lane
      behavior, enemy-Reposition-vs-wall-separation jitter at close range (a theoretical two-systems-tug
      the reviewer couldn't rule out from code alone), narrow-room escape feel.

## Log

- 2026-08-13: Session start. Branch created. Repo facts above gathered. Ledger initialized.
- 2026-08-13: Task 2 complete (commit 4ee1543). See Task Status entry above for details. Note for Task 3:
  CombatDirector.cpp currently computes each enemy's engagement target as a placeholder orbit point
  (`kEngagementOrbitRadius = 130.f` around the player, or the player's own position for Commit) — this is
  explicitly a stand-in for ComputeEngagementTarget and should be replaced, not built on top of.
- 2026-08-13: Task 3 complete (commit 5529a16 "feat: differentiate normal enemy combat rhythms"). See Task
  Status entry above for full detail. Placeholder orbit computation in CombatDirector.cpp replaced with
  Enemy.h's new inline ComputeEngagementTarget. CombatRhythmTests.exe and the full Debug x64 msbuild both
  green. Note for Task 4+: Engine.cpp's EnemyRuntimeContext::tier/swarmEncounter fields are still never set
  by either real call site (flagged already in Task 2's log) — tier-aware commit budgets and the swarm bonus
  slot stay effectively inert in the live game until a future task wires those from actual room/encounter
  state. Reposition/Support engagement targets are not yet nav/wall-aware (Task 6's problem).
- 2026-08-13: Task 4 complete (commit 11d437c "feat: stage smaller reinforcement batches"). See Task Status
  entry above for full detail. New ReinforcementPacing.h/.cpp module (SimultaneousBodyTarget,
  ReinforcementBatchSize, PendingSpawnPhase, PendingEnemySpawn, AdvancePendingSpawn). Engine.cpp's
  immediate-multi-spawn reinforcement release replaced with reservation+telegraph via
  TryReserveDungeonReinforcementBatch/AdvanceDungeonPendingSpawns; opening-slice spawn now capped at
  SimultaneousBodyTarget(band) with overflow deferred into the same queue, total population preserved
  exactly. All 5 _dungeonReinforcements.clear() sites got a parallel _pendingEnemySpawns.clear(); room-clear
  allDead check and Holdout refill trigger both updated to also require pending spawns empty. Pause-freezing
  verified structurally (UpdateDungeonRun never runs during GameState::Pause), no new code needed. All tests
  green (CombatRhythmTests, EncounterPlannerTests, CombatSystemsTests, RoomCapacityTests), full Debug x64
  msbuild zero errors. Note for Task 5: smoke-emission hook point is inside AdvanceDungeonPendingSpawns in
  Engine.cpp, right where AdvancePendingSpawn flips smokeEmitted true — add VFXManager::SpawnSmokeBurst there.
  Ready-phase arrival/orientation latch (design: enemy can't move/attack on creation frame) is NOT yet
  implemented — flagged as a known gap for Task 5. Note for Task 6: reservation positions already reuse the
  existing blocker/min-distance validity path, so no additional wall-safety dependency exists for Task 6.
- 2026-08-13: Task 5 complete (commit 958f521 "feat: telegraph enemy reinforcements"). See Task Status entry
  above for full detail. VFXManager::SpawnSmokeBurst added as a thin wrapper over the existing directional
  SpawnImpactBurst (upward bias, low speed, purple-gray tint, no new asset). Engine::DrawPendingEnemySpawns
  added and wired into the dungeon draw block right after _vfx.Draw, before the enemy sprite loop (below
  sprites, above floor, per design); circle phase pulses via spawn.timer (deterministic, pause/hitstop-safe);
  smoke phase contracts and brightens. Smoke burst fires exactly once at the Circle->Smoke transition inside
  AdvanceDungeonPendingSpawns via a before/after smokeEmitted flip guard. Arrival/orientation latch added to
  Enemy (_arrivalTimer/BeginArrivalDelay/UpdateArrivalDelay/IsArriving, reset in ResetForSpawn) and gated
  inside the SHARED Enemy::Update (NOT CombatDirector.cpp, which is on this task's do-not-modify list) —
  Balance::Rhythm::kSpawnArrivalDelaySeconds=0.20f. IMPORTANT KNOWN GAP for Task 6/7: the arrival gate only
  takes effect for enemy types that route through base Enemy::Update (Grunt/Slime/Sporeling/Shieldbearer/
  Warchief) — the five subclasses with a fully custom Update() override (SkeletonArcher, FlameWisp, Phantom,
  BomberImp, LivingBlade) do not check _arrivalTimer at all, so a telegraphed reinforcement of one of those
  five types gets no enforced arrival delay today. Deliberately not extended further (would touch 5 more
  files outside this task's stated/justified scope). All tests green (CombatRhythmTests, EncounterPlannerTests),
  full Debug x64 msbuild zero errors (only pre-existing C4244 warnings). Manual verification limited to
  launch-and-stay-running (4s, no crash) via PowerShell Start-Process — did not observe the actual in-game
  telegraph visually (no automation path available for driving a native raylib window to a live dungeon
  reinforcement wave in this environment). git diff 11d437c -- Engine.cpp Engine.h shows exactly one removed
  line (a call-site replacement); the pre-existing VillageMap->VillageObjectLayer refactor markers
  (VillageRuntimeObjectDef) are untouched at their pre-task counts.
- 2026-08-13: Task 6 complete (commit ce4993e "fix: prevent enemy bodies pinning player to walls"). See Task
  Status entry above for full detail. New CombatSeparation.h/.cpp module (SeparationMove, ChooseBodySeparation)
  is a pure geometry policy: ordinary overlap negates the MTV fully onto the enemy (player untouched); invalid
  enemy correction + shallow overlap holds position (both deltas zero); invalid enemy correction + deep overlap
  applies a small clamped player fallback (min of a 6px hard ceiling and 20% of the raw MTV, always strictly
  smaller than a full push). Engine.cpp's UpdateDungeonRun buggy player-enemy separation block (the literal
  wall-trap root cause: re-read _player.GetWorldPos() every loop iteration, applied full MTV directly, zero
  wall re-validation) was replaced with: capture one stable player position/capsule before the enemy loop;
  per overlapping enemy, determine enemyMoveValid by reusing the existing IsDungeonEnemySpawnPositionValid
  check against the enemy's tentative post-separation position, and deepOverlap via a documented 24px MTV-
  magnitude threshold; call ChooseBodySeparation and apply enemyDelta via enemy->Teleport; accept at most ONE
  contact's playerDelta per frame (structural guarantee against summed pushes, on top of the policy's own
  small clamp); apply the accepted player fallback only after validating it via the existing IsRoomSpawnAreaValid
  free function against the player's real collision body, skipping it entirely if it would land in geometry.
  ResolveDungeonEnemyCollisions() was reordered to run once, immediately AFTER the new separation block instead
  of before it, per the plan's literal Step 4 wording, so any enemy pushed toward a wall/prop by separation gets
  corrected the same frame. Forced-push/dash guards preserved exactly in their original positions; authored
  forced-push and boss knockback mechanics were not touched. All tests green (CombatRhythmTests.exe including
  4 new Task 6 test blocks, EncounterPlannerTests.exe), full Debug x64 msbuild zero errors after adding
  CombatSeparation.cpp/.h to TestGame.vcxproj/.filters beside the existing CombatEngagement/ReinforcementPacing
  entries. git diff ed854a1 -- Engine.cpp Engine.h shows exactly 4 removed lines, all inside the intentionally
  replaced buggy block; VillageRuntimeObjectDef marker counts (26 in Engine.cpp, 9 in Engine.h) are unchanged
  from the pre-task tip, confirming the transferred VillageMap->VillageObjectLayer refactor and all prior
  tasks' work remain fully intact. Manual verification limited to launch-and-stay-running (5s, no crash) via
  PowerShell Start-Process — did not visually confirm the wall-trap fix in a live dungeon room (no input-
  injection/screenshot automation available in this environment, same limitation as every prior task). Flagged
  for Task 7: the 24px deep-overlap threshold and the 6px/20%-of-MTV player-fallback clamp are documented
  judgment calls with no in-game playtesting evidence yet; Task 7's own gameplay-matrix step ("three melee
  enemies beside every wall and a prop") should verify these feel right and that an authored forced-push boss
  still knocks the player back and stops flush at a wall exactly as before.
