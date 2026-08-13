#pragma once

// =============================================================================
// ReinforcementPacing — deterministic, side-effect-free policy for how many
// enemy bodies a room may host on screen at once (SimultaneousBodyTarget) and
// how quickly the reinforcement queue drains into new telegraphed spawn
// reservations (ReinforcementBatchSize), plus the pure phase-timing state
// machine for one reserved-but-not-yet-spawned enemy (PendingEnemySpawn /
// AdvancePendingSpawn). Pure data-in/data-out logic: no raylib RNG, no
// wall-clock reads, no rendering/VFX calls — Task 5 owns drawing the purple
// circle and emitting the actual smoke particles; this module only tracks
// phase + timer state (see the `smokeEmitted` field below). Consumed by
// Engine (see docs/superpowers/plans/2026-08-13-hades-style-enemy-combat-rhythm.md,
// Task 4, and docs/superpowers/specs/2026-08-13-hades-style-enemy-combat-rhythm-design.md,
// "Reinforcement Staging" / "Spawn Telegraph and VFX").
// =============================================================================

#include "raylib.h"            // Vector2
#include "EncounterPlanner.h"  // EncounterSpawnEntry
#include "RoomCapacity.h"      // RoomCapacityBand

// Recommended simultaneous on-screen body target for a room's capacity band
// (design: "Recommended simultaneous body targets are derived from room
// capacity" — Small 3, Medium 4, Large 5, Arena 6). This governs how many
// bodies may be active/visible at once; it does NOT change the total
// population EncounterPlanner allots to the room.
int SimultaneousBodyTarget(RoomCapacityBand band);

// How many NEW reinforcement reservations to make right now, given the
// current active body count, how many reservations are already pending
// (reserved but not yet spawned), the room's SimultaneousBodyTarget, and how
// many entries remain in the reinforcement queue. Never negative; never more
// than 2 per batch (design: "Standard reinforcement batches contain 1-2
// enemies"); never exceeds the remaining gap to `target` (active + pending
// already accounts for bodies that are on screen or about to be); never
// exceeds `queued` (can't reserve more than remain in the queue).
int ReinforcementBatchSize(int active, int pending, int target, int queued);

enum class PendingSpawnPhase { Circle, Smoke, Ready };

// One reserved-but-not-yet-instantiated reinforcement slot. `entry` is the
// enemy to spawn once Ready; `worldPos` is its reserved, pre-validated
// position (revalidated again immediately before instantiation — see the
// design's "State and Failure Handling"). `phase`/`timer` drive
// AdvancePendingSpawn's state machine. `smokeEmitted` is a plain state flag
// that flips true exactly once, at the Circle->Smoke transition — Task 5
// reads it to fire VFXManager::SpawnSmokeBurst exactly once; this module
// never calls VFX itself.
struct PendingEnemySpawn
{
    EncounterSpawnEntry entry;
    Vector2 worldPos{};
    PendingSpawnPhase phase = PendingSpawnPhase::Circle;
    float timer = 0.f;
    bool smokeEmitted = false;
};

// Advances one pending spawn's phase timer by dt. Circle -> Smoke once
// Balance::Rhythm::kSpawnCircleSeconds (0.65s) has elapsed; Smoke -> Ready
// once a further Balance::Rhythm::kSpawnSmokeSeconds (0.30s) has elapsed
// (0.95s total). A single large dt (e.g. a frame hitch) correctly walks
// through every phase transition in one call rather than requiring multiple
// calls to converge. Once `phase == Ready`, further calls are a no-op —
// callers are expected to consume (spawn + remove) a Ready entry the same
// frame it becomes Ready.
void AdvancePendingSpawn(PendingEnemySpawn& spawn, float dt);
