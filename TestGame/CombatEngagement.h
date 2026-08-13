#pragma once

// =============================================================================
// CombatEngagement — deterministic, side-effect-free policy that decides which
// normal enemies get to Commit to an attack this frame vs. Support vs.
// Reposition, given a fixed commit-slot budget (Balance::Rhythm). Pure
// data-in/data-out logic: no raylib RNG, no wall-clock reads, no engine
// coupling. Consumed by CombatDirector / Enemy in later tasks (see
// docs/superpowers/plans/2026-08-13-hades-style-enemy-combat-rhythm.md).
//
// Dead-entry policy: candidates with alive == false receive NO assignment at
// all — they are simply omitted from the returned vector. Callers must treat
// "no assignment found for this id" as a no-op for dead/inactive enemies.
// =============================================================================

#include "Enemy.h"   // reuse the existing EnemyRole enum rather than duplicating it

#include <cstdint>
#include <vector>

enum class EngagementIntent
{
    Commit,
    Support,
    Reposition
};

struct EngagementCandidate
{
    uint64_t id = 0;
    EnemyRole role = EnemyRole::Grunt;
    float distance = 0.f;   // distance to the engagement focus (player), world px
    bool alive = true;
    bool locked = false;    // already committed & mid-attack/recovery from a prior frame
    bool recovering = false;
    bool swarm = false;
};

struct EngagementAssignment
{
    uint64_t id = 0;
    EngagementIntent intent = EngagementIntent::Reposition;
    float orbitAngle = 0.f;   // radians, deterministic function of id + sequence
};

// Number of concurrent commit slots for the given encounter tier (0 = early,
// 1 = mid, 2 = late — matches Balance::Pressure's tier index), plus the
// fragile-swarm bonus slot (Balance::Rhythm::kSwarmExtraCommitters) when
// swarm is true. Tier is clamped into [0,2].
int CombatCommitLimit(int tier, bool swarm);

// Builds one assignment per ALIVE candidate (dead candidates are omitted —
// see header comment above). Locked candidates always keep Commit and are
// never displaced; they fill commit slots first. Any remaining slots go to
// the best-scoring remaining candidates: primarily ascending distance
// (closer wants the slot), then role (secondary tie-break for equal
// distance), then ascending id (final deterministic tie-break — the output
// ordering never contains ties). Non-committing candidates whose role is
// Tank or Support receive Support; every other non-committing role receives
// Reposition. `sequence` is a deterministic tie-break/orbit seed, not RNG —
// calling this twice with identical inputs (including the same sequence)
// always produces byte-identical output.
std::vector<EngagementAssignment> BuildEngagementAssignments(
    const std::vector<EngagementCandidate>& candidates,
    int tier,
    bool swarm,
    uint64_t sequence);
