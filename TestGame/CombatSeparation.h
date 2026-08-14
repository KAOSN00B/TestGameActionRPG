#pragma once

// =============================================================================
// CombatSeparation — deterministic, side-effect-free policy for resolving an
// overlapping player/enemy capsule contact WITHOUT pinning the player against
// solid room geometry (the "wall trapping" bug —
// docs/superpowers/specs/2026-08-13-hades-style-enemy-combat-rhythm-design.md,
// "Current Root Causes" / "Wall-Safe Physical Separation"). Pure data-in/
// data-out logic: no raylib texture/RNG/wall-clock reads, no Engine/Enemy/
// CapsuleCollision coupling — the caller (Engine::UpdateDungeonRun) is
// responsible for computing the capsule MTV via CheckCapsuleCapsule and for
// deciding enemyMoveValid/deepOverlap by checking the candidate enemy
// position against room geometry (the existing dungeon collision pass covers
// that; see Engine.cpp's integration for the exact reasoning/threshold used).
//
// Design intent (from the spec's "Wall-Safe Physical Separation"): ordinary
// contact moves the ENEMY, not the player. The player only ever receives a
// small, clamped nudge, and only when the enemy genuinely cannot be moved out
// cleanly AND the overlap is deep enough to be a real geometry problem (e.g.
// an enemy shoved into a corner) rather than normal jostling. This keeps
// player-side correction bounded no matter how many enemies overlap the
// player in the same frame — see the "four contacts" test in
// CombatRhythmTests.cpp and the matching single-fallback-per-frame cap
// Engine.cpp applies on top of this.
// =============================================================================

#include "raylib.h"   // Vector2

struct SeparationMove
{
    Vector2 enemyDelta{ 0.f, 0.f };
    Vector2 playerDelta{ 0.f, 0.f };
};

// Decides how to resolve one overlapping player/enemy capsule contact this
// frame, given:
//   - playerOutMtv:  the MTV CheckCapsuleCapsule(playerCapsule, enemyCapsule,
//                     mtv) already produced — the vector that pushes the
//                     PLAYER out of the ENEMY (i.e. points away from the
//                     enemy, toward free space around the player).
//   - enemyMoveValid: true if moving the enemy by -playerOutMtv (i.e. the
//                     enemy's own full separation distance, away from the
//                     player) lands the enemy somewhere clear of room walls
//                     and props. The caller determines this by checking the
//                     candidate enemy position against the existing dungeon
//                     spawn/collision validity check — see Engine.cpp.
//   - deepOverlap:    true if the overlap is unusually severe (the caller's
//                     judgment — see Engine.cpp's documented pixel
//                     threshold), distinguishing normal jostling from a
//                     genuinely awkward embed (e.g. an enemy pinned into a
//                     corner) that enemy-only correction cannot resolve.
//
// Behavior:
//   - enemyMoveValid == true (ordinary case, expected to be the vast
//     majority of contacts): enemyDelta = -playerOutMtv (the enemy moves the
//     FULL separation distance away from the player); playerDelta = {0,0}.
//   - enemyMoveValid == false && deepOverlap == false (shallow overlap the
//     enemy can't cleanly be pushed out of, e.g. it would land in a wall,
//     but not severe enough to justify moving the player either): both
//     deltas are {0,0} — hold position this frame rather than push anyone;
//     the contact is re-evaluated next frame against whatever geometry
//     exists then.
//   - enemyMoveValid == false && deepOverlap == true (the enemy is stuck
//     between the player and geometry): playerDelta receives the smallest
//     necessary correction — clamped to a small fixed pixel ceiling AND to a
//     fraction of playerOutMtv's own magnitude, so it is always strictly
//     smaller than a full ordinary-case push regardless of how large
//     playerOutMtv is; enemyDelta stays {0,0} in this branch (the enemy did
//     not have anywhere clean to go).
//
// Pure function: identical inputs always produce identical output, and
// calling it repeatedly (e.g. once per overlapping enemy from one stable
// pre-resolution player position) never accumulates player-side movement on
// its own — playerDelta is {0,0} in the common case and only ever a small
// clamped value in the rare deep-overlap fallback case.
SeparationMove ChooseBodySeparation(Vector2 playerOutMtv, bool enemyMoveValid, bool deepOverlap);
