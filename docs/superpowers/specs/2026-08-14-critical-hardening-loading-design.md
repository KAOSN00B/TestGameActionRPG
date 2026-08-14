# Combat, Village Persistence, and Loading Hardening Design

## Goal

Fix the audited merge-blocking correctness problems in enemy engagement, reinforcement telegraphs, and village asset persistence, while replacing visible transition freezes with a reusable black loading overlay.

## Scope

This pass fixes:

- Engagement recovery that never advances.
- Custom enemies that bypass commit ownership.
- Stunned or arriving enemies consuming commit slots.
- Specialist reinforcements appearing away from their warning circle.
- Pending reinforcement positions overlapping each other.
- Village Asset Adjuster saves dropping unsupported metadata.
- Village asset names that cannot round-trip through the layout format.
- Synchronous dungeon-loading transitions that visibly freeze the previous screen.

It does not rebalance damage, total encounter population, attack timing, or boss movesets.

## Engagement Lifecycle

CombatDirector is the shared runtime boundary for every enemy type. It ticks each active enemy's engagement latch exactly once per gameplay frame before building engagement candidates.

An enemy may occupy a commit slot only when it is active, alive, not dying, not pit-falling, not arriving, not frozen, and not electro-stunned. Existing authored uninterruptible attacks retain ownership until completion.

Skeleton Archer, Flame Wisp, and Bomber Imp use the same lifecycle as other custom enemies:

1. Require a Commit assignment and an eligible latch before entering an offensive state.
2. Call BeginCommit once when the offensive state begins.
3. Retain ownership during the authored attack.
4. Call EndCommit on completion or cancellation.
5. Reposition during recovery, then become eligible after the latch expires.

## Reinforcement Position Integrity

The reserved pending-spawn coordinate is the final spawn coordinate. SpawnDungeonGrunt accepts an explicit placement policy; telegraphed reinforcements disable its ordinary post-spawn role relocation.

When reserving a batch, the first pending footprint becomes a blocker for the second reservation. A batch cannot create overlapping purple circles or stacked enemy bodies. Final validation still occurs immediately before instantiation.

## Village Asset Persistence

The Village Asset Adjuster retains the complete loaded VillageAssetData model. Saving mutates only editor-owned fields while preserving interactions, doors, ambient configuration, prompts, targets, marker types, and future fields not edited by the tool.

New village asset names use a stable identifier grammar:

- ASCII letters
- digits
- underscore
- hyphen

Names containing spaces or unsupported punctuation are rejected with visible editor feedback. Existing valid filenames and layout serialization remain unchanged.

A load-save-reload regression test proves that unsupported metadata survives an editor save.

## Black Loading Overlay

A reusable loading-transition controller wraps synchronous heavy actions.

### Lifecycle

1. A caller supplies a loading label, success destination behavior, failure behavior, and a single queued action.
2. The controller enters PresentBlack, suppresses input, and draws a fully black screen.
3. At least one complete black frame is presented before heavy work runs.
4. On the next update, the controller enters Loading and invokes the queued action exactly once.
5. The already-presented screen remains black while the single-threaded load blocks.
6. On success, the controller fades from black into the destination using the existing dungeon fade duration.
7. On failure, it returns safely to the originating state and displays the existing error message.

The controller never reports a fabricated percentage and never repeats a callback.

### Visual

The overlay contains:

- Full-screen solid black.
- Centered Loading... label.
- A compact dark loading-bar track beneath the label.
- A small light segment that loops smoothly across the track while frames are available.
- Optional destination text such as Entering the Forest.

The animation communicates activity, not measured completion. During a blocking load the last rendered black frame remains visible, so the prior village or world-map image never appears frozen.

### Initial Integration Points

- Village to a new main run.
- Village to a resumed run or world map when loading work is required.
- World map selection to generated biome.
- Prologue dungeon creation.
- Direct transitions that synchronously generate a dungeon or load a tileset.

Ordinary room scrolling and already-cheap menu transitions continue using their existing fades.

## Error and State Handling

- Input is ignored during loading.
- The queued action is cleared after invocation.
- Nested loading requests are rejected.
- Pause, death, reset, and shutdown clear callbacks safely.
- Audio state changes occur at the same destination boundary as before.
- Loading failures cannot leave DungeonFadeState, game state, or the callback half-active.
- Web and desktop builds use the same controller; no threading is introduced.

## Testing

Automated tests cover:

- Engagement recovery becoming eligible after runtime ticks.
- Latch tick occurring once per enemy runtime frame.
- Ineligible status states not occupying commit slots.
- Archer, Wisp, and Bomber offensive entry requiring Commit.
- Custom attack completion and cancellation releasing into recovery.
- Telegraphed specialists spawning at the reserved coordinate.
- Two pending reservations not overlapping.
- Village asset load-save-reload preserving every non-editor field.
- Village asset identifier validation.
- Loading action cannot execute before a black frame has been presented.
- Loading action executes exactly once.
- Success transitions to fade-in.
- Failure returns to the origin and clears pending state.
- Nested loading requests are rejected.

Verification includes focused tests, all available standalone regression tests, fresh Debug and Release x64 builds, and manual checks of village-to-dungeon, world-map-to-biome, failed generation, and editor metadata round-trip.

## Success Criteria

- Enemies recover and re-enter the commit pool normally.
- No normal offensive action bypasses the shared 2/3 commitment budget.
- Frozen, stunned, or arriving enemies cannot waste commitment slots.
- Every telegraphed reinforcement appears at its warning circle.
- Pending warning circles do not stack.
- Saving a village asset cannot erase metadata the adjuster does not edit.
- Invalid asset names cannot enter a layout that cannot reload them.
- Every expensive dungeon transition presents black before blocking work begins.
- Loading callbacks execute once and transition cleanly on success or failure.

