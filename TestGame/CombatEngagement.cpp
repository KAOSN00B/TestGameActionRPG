#include "CombatEngagement.h"

#include <algorithm>

namespace
{
    int ClampTier(int tier)
    {
        if (tier < 0) return 0;
        if (tier > 2) return 2;
        return tier;
    }

    // Secondary tie-break used only when two candidates are exactly equidistant.
    // Lower rank wins a contested commit slot first — combat-forward roles rank
    // ahead of roles the design intends to spend more time Supporting/holding.
    int RoleCommitRank(EnemyRole role)
    {
        switch (role)
        {
            case EnemyRole::Grunt:
            case EnemyRole::Charger:
            case EnemyRole::Assassin:
                return 0;
            case EnemyRole::HeavyRanged:
            case EnemyRole::Ranged:
            case EnemyRole::Zoner:
                return 1;
            case EnemyRole::Summoner:
            case EnemyRole::Tank:
                return 2;
            case EnemyRole::Support:
            case EnemyRole::Boss:
            default:
                return 3;
        }
    }

    // Deterministic 64-bit mix (SplitMix64-style finalizer). Pure function of
    // its inputs — no global state, no RNG, no wall-clock reads.
    uint64_t MixHash(uint64_t a, uint64_t b)
    {
        uint64_t h = a + 0x9E3779B97F4A7C15ull;
        h ^= b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= (h >> 33);
        h *= 0xFF51AFD7ED558CCDull;
        h ^= (h >> 33);
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= (h >> 33);
        return h;
    }

    constexpr double kTwoPi = 6.283185307179586476925286766559;

    float OrbitAngleFor(uint64_t id, uint64_t sequence)
    {
        const uint64_t h = MixHash(id, sequence);
        const double frac = static_cast<double>(h % 1000000ull) / 1000000.0;
        return static_cast<float>(frac * kTwoPi);
    }
}

int CombatCommitLimit(int tier, bool swarm)
{
    int limit = Balance::Rhythm::kCommittersByTier[ClampTier(tier)];
    if (swarm) limit += Balance::Rhythm::kSwarmExtraCommitters;
    return limit;
}

std::vector<EngagementAssignment> BuildEngagementAssignments(
    const std::vector<EngagementCandidate>& candidates,
    int tier,
    bool swarm,
    uint64_t sequence)
{
    std::vector<EngagementAssignment> result;
    result.reserve(candidates.size());

    const int commitLimit = CombatCommitLimit(tier, swarm);

    // Locked owners keep Commit unconditionally and are counted against the
    // budget first; every other alive candidate competes for what's left.
    int lockedCount = 0;
    std::vector<size_t> contenders;
    contenders.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const EngagementCandidate& c = candidates[i];
        if (!c.alive) continue;
        if (c.locked) { ++lockedCount; continue; }
        contenders.push_back(i);
    }

    const int remainingSlots = (commitLimit > lockedCount) ? (commitLimit - lockedCount) : 0;

    // Stable, fully deterministic ranking: ascending distance, then role, then
    // ascending id — id guarantees a strict total order so there is never a
    // tie in the resulting selection regardless of std::sort's internals.
    std::vector<size_t> ranked = contenders;
    std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b)
    {
        const EngagementCandidate& ca = candidates[a];
        const EngagementCandidate& cb = candidates[b];
        if (ca.distance != cb.distance) return ca.distance < cb.distance;
        const int ra = RoleCommitRank(ca.role);
        const int rb = RoleCommitRank(cb.role);
        if (ra != rb) return ra < rb;
        return ca.id < cb.id;
    });

    std::vector<bool> selectedToCommit(candidates.size(), false);
    for (int i = 0; i < remainingSlots && i < static_cast<int>(ranked.size()); ++i)
        selectedToCommit[ranked[static_cast<size_t>(i)]] = true;

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const EngagementCandidate& c = candidates[i];
        if (!c.alive) continue;   // dead entries: no assignment at all (see header)

        EngagementAssignment a;
        a.id = c.id;
        a.orbitAngle = OrbitAngleFor(c.id, sequence);

        if (c.locked || selectedToCommit[i])
        {
            a.intent = EngagementIntent::Commit;
        }
        else if (c.role == EnemyRole::Tank || c.role == EnemyRole::Support)
        {
            a.intent = EngagementIntent::Support;
        }
        else
        {
            a.intent = EngagementIntent::Reposition;
        }

        result.push_back(a);
    }

    return result;
}
