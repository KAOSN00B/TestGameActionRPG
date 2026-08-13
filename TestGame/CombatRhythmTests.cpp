#include "CombatEngagement.h"

#include <cassert>

namespace
{
    EngagementCandidate MakeCandidate(uint64_t id, EnemyRole role, float distance,
                                       bool alive = true, bool locked = false,
                                       bool recovering = false, bool swarm = false)
    {
        EngagementCandidate c;
        c.id = id;
        c.role = role;
        c.distance = distance;
        c.alive = alive;
        c.locked = locked;
        c.recovering = recovering;
        c.swarm = swarm;
        return c;
    }

    int CountCommit(const std::vector<EngagementAssignment>& assignments)
    {
        int count = 0;
        for (const EngagementAssignment& a : assignments)
            if (a.intent == EngagementIntent::Commit) ++count;
        return count;
    }

    const EngagementAssignment* FindAssignment(const std::vector<EngagementAssignment>& assignments, uint64_t id)
    {
        for (const EngagementAssignment& a : assignments)
            if (a.id == id) return &a;
        return nullptr;
    }
}

int main()
{
    // ── 1. CombatCommitLimit ─────────────────────────────────────────────────
    assert(CombatCommitLimit(0, false) == 2);
    assert(CombatCommitLimit(1, false) == 3);
    assert(CombatCommitLimit(2, false) == 3);
    assert(CombatCommitLimit(0, true) == 3);
    assert(CombatCommitLimit(1, true) == 4);
    assert(CombatCommitLimit(2, true) == 4);

    // ── 2. Six candidates: tier/swarm commit counts ─────────────────────────
    std::vector<EngagementCandidate> six = {
        MakeCandidate(1, EnemyRole::Grunt,    100.f),
        MakeCandidate(2, EnemyRole::Charger,  150.f),
        MakeCandidate(3, EnemyRole::Ranged,   300.f),
        MakeCandidate(4, EnemyRole::Tank,     120.f),
        MakeCandidate(5, EnemyRole::Support,  400.f),
        MakeCandidate(6, EnemyRole::Assassin, 90.f),
    };

    {
        const auto tier0 = BuildEngagementAssignments(six, 0, false, 42);
        assert(tier0.size() == 6);
        assert(CountCommit(tier0) == 2);

        const auto tier1 = BuildEngagementAssignments(six, 1, false, 42);
        assert(tier1.size() == 6);
        assert(CountCommit(tier1) == 3);

        // No candidate in `six` is swarm-profile, so the fragile-swarm bonus
        // slot (Balance::Rhythm::kSwarmExtraCommitters) goes unused even with
        // swarm mode active — the bonus slot is reserved for swarm-profile
        // enemies specifically, not a free budget increase for everyone.
        const auto tier0Swarm = BuildEngagementAssignments(six, 0, true, 42);
        assert(CountCommit(tier0Swarm) == 2);

        const auto tier1Swarm = BuildEngagementAssignments(six, 1, true, 42);
        assert(CountCommit(tier1Swarm) == 3);
    }

    // ── 2b. Fragile-swarm bonus slot: only usable by swarm-profile candidates
    {
        std::vector<EngagementCandidate> withSwarmProfile = {
            MakeCandidate(40, EnemyRole::Grunt,   50.f),
            MakeCandidate(41, EnemyRole::Charger, 60.f),
            MakeCandidate(42, EnemyRole::Ranged, 500.f, /*alive*/true, /*locked*/false,
                          /*recovering*/false, /*swarm*/true),
        };

        // Base limit (tier 0) is 2; the two closest (40, 41) fill it. With
        // swarm=true the bonus slot goes to the swarm-profile candidate (42)
        // despite its much greater distance, because only a fragile
        // swarm-profile candidate may claim the bonus slot.
        const auto withSwarm = BuildEngagementAssignments(withSwarmProfile, 0, true, 5);
        assert(CountCommit(withSwarm) == 3);
        assert(FindAssignment(withSwarm, 42)->intent == EngagementIntent::Commit);

        // Without swarm mode active, the bonus slot doesn't exist at all, so
        // the same swarm-profile candidate does not commit.
        const auto withoutSwarm = BuildEngagementAssignments(withSwarmProfile, 0, false, 5);
        assert(CountCommit(withoutSwarm) == 2);
        assert(FindAssignment(withoutSwarm, 42)->intent != EngagementIntent::Commit);
    }

    // ── 2c. Bonus pool stays capped even with multiple eligible candidates ──
    {
        std::vector<EngagementCandidate> manySwarm = {
            MakeCandidate(50, EnemyRole::Grunt,   50.f),
            MakeCandidate(51, EnemyRole::Charger, 60.f),
            MakeCandidate(52, EnemyRole::Ranged, 100.f, true, false, false, /*swarm*/true),
            MakeCandidate(53, EnemyRole::Zoner,  110.f, true, false, false, /*swarm*/true),
        };

        // Base pool (2) fills with 50/51; only ONE of the two remaining
        // swarm-profile candidates gets the single bonus slot — the closer
        // one (52) wins under the same rank ordering used for the base pool.
        const auto assignments = BuildEngagementAssignments(manySwarm, 0, true, 8);
        assert(CountCommit(assignments) == 3);
        assert(FindAssignment(assignments, 52)->intent == EngagementIntent::Commit);
        assert(FindAssignment(assignments, 53)->intent != EngagementIntent::Commit);
    }

    // ── 3. Locked owners retain Commit and are never displaced ──────────────
    {
        std::vector<EngagementCandidate> withLocked = {
            MakeCandidate(10, EnemyRole::Tank, 999.f, /*alive*/true, /*locked*/true),
            MakeCandidate(11, EnemyRole::Grunt,    50.f),
            MakeCandidate(12, EnemyRole::Charger,  60.f),
            MakeCandidate(13, EnemyRole::Assassin, 70.f),
            MakeCandidate(14, EnemyRole::Ranged,   80.f),
            MakeCandidate(15, EnemyRole::Zoner,    90.f),
        };

        // Tier 0 (limit 2): the locked candidate holds 1 slot despite its huge
        // distance; only 1 remaining slot goes to the best-scoring contender.
        const auto assignments = BuildEngagementAssignments(withLocked, 0, false, 7);
        assert(assignments.size() == 6);

        const EngagementAssignment* lockedAssignment = FindAssignment(assignments, 10);
        assert(lockedAssignment != nullptr);
        assert(lockedAssignment->intent == EngagementIntent::Commit);

        assert(CountCommit(assignments) == 2);

        // The closest non-locked contender (id 11, distance 50) must win the
        // single remaining slot.
        const EngagementAssignment* closest = FindAssignment(assignments, 11);
        assert(closest != nullptr);
        assert(closest->intent == EngagementIntent::Commit);
    }

    // ── 4. Dead entries get no assignment at all ─────────────────────────────
    {
        std::vector<EngagementCandidate> withDead = {
            MakeCandidate(20, EnemyRole::Grunt, 50.f),
            MakeCandidate(21, EnemyRole::Charger, 60.f, /*alive*/false),
            MakeCandidate(22, EnemyRole::Ranged, 70.f),
        };
        const auto assignments = BuildEngagementAssignments(withDead, 0, false, 3);
        assert(assignments.size() == 2);
        assert(FindAssignment(assignments, 21) == nullptr);
        assert(FindAssignment(assignments, 20) != nullptr);
        assert(FindAssignment(assignments, 22) != nullptr);
    }

    // ── 5. Non-committing Tank/Support wait as Support; others as Reposition ─
    {
        std::vector<EngagementCandidate> roles = {
            MakeCandidate(30, EnemyRole::Grunt,    50.f),   // expected Commit (closest)
            MakeCandidate(31, EnemyRole::Assassin, 60.f),   // expected Commit (2nd closest)
            MakeCandidate(32, EnemyRole::Tank,     200.f),  // expected Support
            MakeCandidate(33, EnemyRole::Support,  210.f),  // expected Support
            MakeCandidate(34, EnemyRole::Zoner,    220.f),  // expected Reposition
            MakeCandidate(35, EnemyRole::Ranged,   230.f),  // expected Reposition
        };
        const auto assignments = BuildEngagementAssignments(roles, 0, false, 99);
        assert(assignments.size() == 6);

        assert(FindAssignment(assignments, 30)->intent == EngagementIntent::Commit);
        assert(FindAssignment(assignments, 31)->intent == EngagementIntent::Commit);
        assert(FindAssignment(assignments, 32)->intent == EngagementIntent::Support);
        assert(FindAssignment(assignments, 33)->intent == EngagementIntent::Support);
        assert(FindAssignment(assignments, 34)->intent == EngagementIntent::Reposition);
        assert(FindAssignment(assignments, 35)->intent == EngagementIntent::Reposition);
    }

    // ── 6/7. Determinism: identical inputs (incl. sequence) => identical output
    {
        const auto first = BuildEngagementAssignments(six, 1, true, 12345);
        const auto second = BuildEngagementAssignments(six, 1, true, 12345);
        assert(first.size() == second.size());
        for (size_t i = 0; i < first.size(); ++i)
        {
            assert(first[i].id == second[i].id);
            assert(first[i].intent == second[i].intent);
            assert(first[i].orbitAngle == second[i].orbitAngle);
        }

        // A different sequence is still fully deterministic (repeatable), even
        // though it may produce different orbit angles.
        const auto thirdA = BuildEngagementAssignments(six, 1, true, 999);
        const auto thirdB = BuildEngagementAssignments(six, 1, true, 999);
        for (size_t i = 0; i < thirdA.size(); ++i)
        {
            assert(thirdA[i].id == thirdB[i].id);
            assert(thirdA[i].intent == thirdB[i].intent);
            assert(thirdA[i].orbitAngle == thirdB[i].orbitAngle);
        }
    }

    // ── Task 2: EngagementLatch — stable Commit-slot ownership ──────────────
    // BeginCommit() locks ownership (CanCommit() == false); EndCommit(0.65)
    // transitions into the post-attack recovery countdown (still ineligible
    // immediately after); ticking short of the full duration stays ineligible;
    // crossing the total duration restores eligibility. See Enemy.h for the
    // full EngagementLatch definition (it lives there, not CombatEngagement.h,
    // to avoid a circular include — see the comment at the top of this file's
    // sibling CombatEngagement.h).
    {
        EngagementLatch latch;
        assert(latch.CanCommit() == true);   // fresh latch starts eligible

        latch.BeginCommit();
        assert(latch.CanCommit() == false);

        latch.EndCommit(0.65f);
        assert(latch.CanCommit() == false);   // recovery just started

        latch.Update(0.64f);
        assert(latch.CanCommit() == false);   // still short of 0.65s total

        latch.Update(0.02f);   // 0.64 + 0.02 = 0.66s >= 0.65s
        assert(latch.CanCommit() == true);

        // A second commit/recovery cycle behaves identically (no leftover
        // state from the first cycle).
        latch.BeginCommit();
        assert(latch.CanCommit() == false);
        latch.EndCommit(0.65f);
        latch.Update(0.30f);
        assert(latch.CanCommit() == false);
        latch.Update(0.30f);
        assert(latch.CanCommit() == false);   // 0.60s total, still short
        latch.Update(0.05f);
        assert(latch.CanCommit() == true);    // 0.65s total, eligible again

        // Reset() hard-clears both the lock and any in-progress recovery —
        // used by Enemy::ResetForSpawn on pooled reuse so a fresh spawn never
        // inherits a previous pooled life's recovery countdown.
        EngagementLatch pooled;
        pooled.BeginCommit();
        pooled.EndCommit(0.65f);
        assert(pooled.CanCommit() == false);
        pooled.Reset();
        assert(pooled.CanCommit() == true);
    }

    // ── Task 2: Enemy::GetRuntimeId() / pooling stability ───────────────────
    // NOT covered by a literal test here: constructing a real Enemy requires
    // EnsureSharedResourcesLoaded() (raylib texture/sound loading via
    // LoadTexture/LoadSound), which needs a live raylib graphics/audio context
    // (InitWindow/InitAudioDevice). This standalone assert-based test binary
    // links no raylib.lib and opens no window (matching the existing precedent
    // in this codebase: EncounterPlannerTests.cpp and CombatSystemsTests.cpp
    // both avoid constructing real Enemy/Character objects for the same
    // reason). Forcing a real Enemy construction here would require pulling in
    // the full raylib runtime and asset files, which is out of scope for a
    // fast, non-rendering unit test.
    //
    // Instead, the runtime-id stability contract is verified by inspection:
    //   - Enemy::_runtimeId is assigned exactly once, from a monotonically
    //     increasing counter, in the Enemy(Vector2) constructor (Enemy.cpp).
    //   - Enemy::ResetForSpawn(Vector2) — the shared pooled-reuse reset path
    //     called on every respawn from the object pool — does NOT assign to
    //     _runtimeId anywhere in its body; it only resets _engagementLatch,
    //     _engagementIntent, _engagementTarget, _hasEngagementAssignment, and
    //     _swarmProfile (see the block immediately after `_damageApplied =
    //     false;`).
    //   - Since a pooled enemy is reused via `enemy->ResetForSpawn(pos)` on
    //     the SAME C++ object (never re-constructed — see Engine.cpp's spawn
    //     helpers, which only call `make_unique<Enemy>(pos)` + `Init()` for a
    //     brand-new pool slot), GetRuntimeId() is therefore guaranteed stable
    //     across every pooled life of that object, and unique across distinct
    //     objects (the counter never resets or repeats within a process).

    return 0;
}
