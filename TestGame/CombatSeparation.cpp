#include "CombatSeparation.h"

#include <cmath>
#include <algorithm>

namespace
{
    // Hard pixel ceiling on the rare deep-overlap player fallback nudge — a
    // few pixels is enough to unstick a pinch without reintroducing the old
    // "player gets shoved around" feel.
    constexpr float kPlayerFallbackMaxPx = 6.f;

    // Never more than this fraction of the raw MTV either, so the fallback
    // is provably always smaller than a full ordinary-case push (which would
    // apply 100% of the MTV to the enemy) regardless of how large the MTV is.
    constexpr float kPlayerFallbackFraction = 0.2f;
}

SeparationMove ChooseBodySeparation(Vector2 playerOutMtv, bool enemyMoveValid, bool deepOverlap)
{
    SeparationMove result;

    if (enemyMoveValid)
    {
        // Ordinary case: the enemy moves the full separation distance away
        // from the player. The player does not move at all.
        result.enemyDelta = { -playerOutMtv.x, -playerOutMtv.y };
        return result;
    }

    if (!deepOverlap)
    {
        // Shallow overlap the enemy can't be cleanly pushed out of (e.g. it
        // would land in a wall), but not severe enough to move the player
        // either. Hold position this frame; let different geometry or a
        // later frame resolve it.
        return result;
    }

    // Deep overlap + the enemy has nowhere clean to go: apply the smallest
    // necessary correction to the player instead, clamped well below a full
    // MTV push.
    const float mtvLen = std::sqrt(playerOutMtv.x * playerOutMtv.x + playerOutMtv.y * playerOutMtv.y);
    if (mtvLen < 0.0001f)
        return result; // degenerate/zero MTV — nothing meaningful to apply

    const float fallbackLen = std::min(kPlayerFallbackMaxPx, kPlayerFallbackFraction * mtvLen);
    const float scale = fallbackLen / mtvLen;
    result.playerDelta = { playerOutMtv.x * scale, playerOutMtv.y * scale };
    return result;
}
