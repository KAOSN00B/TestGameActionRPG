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

        const auto tier0Swarm = BuildEngagementAssignments(six, 0, true, 42);
        assert(CountCommit(tier0Swarm) == 3);

        const auto tier1Swarm = BuildEngagementAssignments(six, 1, true, 42);
        assert(CountCommit(tier1Swarm) == 4);
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

    return 0;
}
