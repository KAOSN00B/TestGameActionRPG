#pragma once

#include "raylib.h"
#include "GameTypes.h"
#include "Enemy.h"
#include "Prop.h"
#include "Pickup.h"
#include "HealPickup.h"
#include "CyclopsLaserProjectile.h"
#include "EnemyProjectile.h"
#include "LavaBallProjectile.h"
#include "NavigationGrid.h"
#include "VFXManager.h"

#include <functional>
#include <memory>
#include <vector>

class Character;

struct BossSupportState
{
    Enemy* enemy = nullptr;
    float respawnTimer = 0.f;
};

struct CombatSpawnContext
{
    Texture2D* map = nullptr;
    float mapScale = 1.f;
    RoomType currentRoomType = RoomType::Standard;
    int currentAct = 1;
    int currentRoom = 1;
    int forcedEliteMechanic = -1;
    std::vector<std::unique_ptr<Pickup>>* pickups = nullptr;

    int* eliteMechanic = nullptr;
    Enemy** eliteMinibossPtr = nullptr;
    Vector2* eliteCageCenter = nullptr;
    float* eliteCageRadius = nullptr;
    float* eliteCageDamageTimer = nullptr;
    float* eliteEnrageWarningTimer = nullptr;
    float* eliteHazardSpawnTimer = nullptr;

    // Player world position — used to place role-based spawns (ranged in back,
    // tanks between player and back line, assassins off-angle).
    Vector2 playerPos{};

    std::function<bool(Vector2)> isSpawnPositionValid;
    std::function<Enemy*(Vector2)> spawnBasicEnemy;
    std::function<Enemy*(Vector2)> spawnCyclops;
    std::function<Enemy*(Vector2)> spawnOgre;
    std::function<void(Vector2)> spawnMolarbeast;
    std::function<void()> spawnBossSupportAdds;
    // Role enemies the encounter director composes fights from (see EnemyRole).
    std::function<Enemy*(Vector2)> spawnSkeletonArcher;   // Ranged
    std::function<Enemy*(Vector2)> spawnFlameWisp;        // Zoner
    std::function<Enemy*(Vector2)> spawnShieldbearer;     // Tank
    std::function<Enemy*(Vector2)> spawnPhantom;          // Assassin
    std::function<Enemy*(Vector2)> spawnWarchief;         // Support
};

struct EliteMechanicsContext
{
    RoomType currentRoomType = RoomType::Standard;
    Texture2D* map = nullptr;
    float mapScale = 1.f;
    // Override for map->width/height * mapScale (used in DungeonRun where no map exists).
    float worldBoundsW = 0.f;
    float worldBoundsH = 0.f;
    Character* player = nullptr;
    std::vector<std::unique_ptr<Enemy>>* enemies = nullptr;
    std::vector<LavaBallProjectile>* lavaBalls = nullptr;

    int* eliteMechanic = nullptr;
    Enemy** eliteMinibossPtr = nullptr;
    Vector2* eliteCageCenter = nullptr;
    float* eliteCageRadius = nullptr;
    float* eliteCageDamageTimer = nullptr;
    float* eliteEnrageWarningTimer = nullptr;
    float* eliteHazardSpawnTimer = nullptr;

    std::function<bool(Vector2)> isSpawnPositionValid;
    std::function<void(float, float)> triggerScreenShake;
};

// Boss impact/cast FX ids — map 1:1 to FX_Boss*.png sheets loaded by the Engine.
enum class BossFx
{
    SlimeSlam = 0, SlimeSplash, AbyssSummon, PounceImpact, CrushingSlam, BulwarkSlam,
    ToxicEruption, PoisonPool, DreamPull, DashDust, HeavyStrike, DiveImpact,
    ClawSwipe, BloodHowl, DivineSlash, SandStep, TeleportStrike, PumpkinSummon,
    ChitinBurst,
    Count
};

struct EnemyRuntimeContext
{
    Character* player = nullptr;
    NavigationGrid* nav = nullptr;
    const std::vector<Prop>* props = nullptr;
    const std::vector<Vector2>* propCenters = nullptr;
    // Player-made damage zones enemies steer around (see Enemy::SetHazardZones).
    const std::vector<HazardZone>* hazards = nullptr;
    std::vector<std::unique_ptr<Enemy>>* enemies = nullptr;
    // ── Engagement policy inputs (CombatEngagement) ───────────────────────────
    // Encounter tier (0 early / 1 mid / 2 late — matches the same tier index
    // SpawnEnemies derives from depth, see CombatDirector.cpp) and whether this
    // is a fragile-swarm encounter (grants one bonus Commit slot to swarm-
    // profile enemies only — see Enemy::SetSwarmProfile). Neither current
    // Engine.cpp call site sets these yet, so they default to the safest early-
    // game values (tier 0, no swarm) until a later task wires real per-room
    // tier/swarm detection through to this struct.
    int  tier = 0;
    bool swarmEncounter = false;
    std::vector<CyclopsLaserProjectile>* cyclopsLasers = nullptr;
    std::vector<LavaBallProjectile>* lavaBalls = nullptr;
    std::vector<EnemyProjectile>* enemyProjectiles = nullptr;   // arrows + fire bolts
    std::function<void(float, float)> triggerScreenShake;
    std::function<void(Vector2)> spawnSmallSlime;               // Abyss Slime summons
    std::function<Enemy*(Vector2)> spawnBasicEnemy;             // Pumpkin Jack summons
    std::function<void(Vector2)> spawnBossPoisonPool;           // Toxic Vermin pools
    // Play a themed owned FX_Boss*.png sprite at a world position. The int is a
    // BossFx id (see Engine); lets boss impact/cast moments show real art instead
    // of only procedural rings. Safe no-op if unset.
    std::function<void(Vector2, int)> spawnBossFx;
    // Show a floating boss-state word (ENRAGED / PHASE SHIFT / ...) at a world
    // position. Safe no-op if unset. See Enemy::ConsumeBossCallout.
    std::function<void(Vector2, const char*)> spawnBossCallout;
    // Elite signature art: a one-shot animated impact (BossFx id, scale, tint)
    // and a lingering animated hazard decal (id, scale, duration, tint). Both
    // route to Engine-owned sprite strips so ACTIVE attacks are real art —
    // simple shapes remain only for warnings/targeting. Safe no-ops if unset.
    std::function<void(Vector2, int, float, Color)> spawnEliteFx;
    std::function<void(Vector2, int, float, float, Color)> spawnEliteHazardFx;
    // Brief full-screen tint (phase transitions). Safe no-op if unset.
    std::function<void(Color, float)> triggerScreenFlash;
    // A few frames of gameplay freeze on the heaviest impacts (wall crashes,
    // slams) — same juice primitive player hits already use. Safe no-op if unset.
    std::function<void(float)> requestHitStop;
};

struct EnemyDeathContext
{
    std::vector<std::unique_ptr<Enemy>>* enemies = nullptr;
    BossSupportState* bossCyclopsSupport = nullptr;
    BossSupportState* bossOgreSupport = nullptr;
    int wave = 0;
    int* enemiesKilled = nullptr;
    int* bossesDefeated = nullptr;
    bool* demoCompleted = nullptr;
    float* pendingExp = nullptr;
    bool awardKillExp = true;  // legacy/wave mode; dungeon mode pays once per room
    std::function<void(Vector2, bool, bool)> spawnEnemyDrop;
    std::function<void(Vector2)> spawnSmallSlime;    // big slime death split
    std::function<void(Vector2)> spawnPoisonCloud;   // sporeling death burst
    // Relic on-kill effects: pos + (wasBurning, wasFrozen, wasCharged, eliteOrBoss).
    std::function<void(Vector2, bool, bool, bool, bool)> onEnemyKilled;
};

struct BossSupportContext
{
    BossSupportState* bossCyclopsSupport = nullptr;
    BossSupportState* bossOgreSupport = nullptr;
    std::function<bool(Vector2&, float)> tryGetFarSpawnPosition;
    std::function<Enemy*(Vector2)> spawnCyclops;
    std::function<Enemy*(Vector2)> spawnOgre;
    std::function<bool()> isBossFightActive;
};

class CombatDirector
{
public:
    int GetActiveEnemyCount(const std::vector<std::unique_ptr<Enemy>>& enemies) const;
    bool IsBossFightActive(const std::vector<std::unique_ptr<Enemy>>& enemies) const;

    void SpawnEnemies(const CombatSpawnContext& ctx) const;
    void UpdateEliteMechanics(const EliteMechanicsContext& ctx, float dt) const;
    void UpdateEnemyRuntime(const EnemyRuntimeContext& ctx, float dt) const;
    void UpdateEnemyDeaths(const EnemyDeathContext& ctx, float dt) const;

    void SpawnBossSupportAdds(const BossSupportContext& ctx) const;
    void ClearBossSupportAdds(BossSupportState& cyclopsSupport, BossSupportState& ogreSupport) const;
    void UpdateBossSupportRespawns(const BossSupportContext& ctx, float dt) const;

    // ── Elite signature runtime ──────────────────────────────────────────────
    // CombatDirector owns the bounded attack-zone pool: elites emit events,
    // UpdateEnemyRuntime drains them into zones, zones damage/status the player
    // centrally, and DrawEliteWorld renders warnings (world space — the caller
    // wraps it in the camera translation).
    void DrawEliteWorld(const std::vector<std::unique_ptr<Enemy>>& enemies) const;
    void ClearEliteRuntime();                    // room exit / reset / restart
    int  GetActiveEliteZoneCount() const;
    int  GetDroppedEliteZoneCount() const { return _eliteZonesDropped; }

private:
    EliteAttackZone* AcquireEliteZone() const;   // nullptr when the pool is full
    void SpawnEliteZonesForEvent(const EliteSignatureEvent& event,
                                 const EnemyRuntimeContext& ctx) const;
    void UpdateEliteZones(const EnemyRuntimeContext& ctx, float dt) const;

    mutable std::vector<Vector2> _propCentersScratch;
    // Fixed pool — never allocates; the 65th concurrent zone is dropped and
    // counted rather than growing the array. (mutable: mirrors the scratch
    // pattern above because UpdateEnemyRuntime is const.)
    mutable std::array<EliteAttackZone, Balance::Elite::kSignatureZoneCapacity> _eliteZones{};
    mutable std::uint32_t _eliteZoneSequence = 0;
    // Deterministic tie-break/orbit seed handed to BuildEngagementAssignments
    // each frame (see UpdateEnemyRuntime). Incremented once per call — not
    // RNG, just a counter, so orbit angles vary frame-to-frame without needing
    // raylib's random source in a supposedly pure policy call.
    mutable std::uint64_t _engagementSequence = 0;
    mutable int _eliteZonesDropped = 0;
    mutable bool _eliteImpactFeedbackThisFrame = false;   // one shake+sound per cast
};
