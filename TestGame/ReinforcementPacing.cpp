#include "ReinforcementPacing.h"

#include "GameBalance.h"

#include <algorithm>

int SimultaneousBodyTarget(RoomCapacityBand band)
{
    switch (band)
    {
        case RoomCapacityBand::Small:  return 3;
        case RoomCapacityBand::Medium: return 4;
        case RoomCapacityBand::Large:  return 5;
        case RoomCapacityBand::Arena:  return 6;
    }
    return 4;
}

int ReinforcementBatchSize(int active, int pending, int target, int queued)
{
    const int gap = target - (active + pending);
    if (gap <= 0 || queued <= 0) return 0;
    return std::min({ gap, queued, 2 });
}

void AdvancePendingSpawn(PendingEnemySpawn& spawn, float dt)
{
    if (spawn.phase == PendingSpawnPhase::Ready) return;
    if (dt < 0.f) dt = 0.f;

    spawn.timer += dt;

    if (spawn.phase == PendingSpawnPhase::Circle &&
        spawn.timer >= Balance::Rhythm::kSpawnCircleSeconds)
    {
        spawn.phase = PendingSpawnPhase::Smoke;
        spawn.smokeEmitted = true;
    }

    if (spawn.phase == PendingSpawnPhase::Smoke &&
        spawn.timer >= (Balance::Rhythm::kSpawnCircleSeconds + Balance::Rhythm::kSpawnSmokeSeconds))
    {
        spawn.phase = PendingSpawnPhase::Ready;
    }
}
