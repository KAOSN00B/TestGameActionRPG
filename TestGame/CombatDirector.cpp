#include "CombatDirector.h"

#include "AbyssSlime.h"
#include "AncientBear.h"
#include "BomberImp.h"
#include "Character.h"
#include "ChompBug.h"
#include "CombatEngagement.h"
#include "Cyclops.h"
#include "FlameWisp.h"
#include "GoldPickup.h"
#include "Minotaur.h"
#include "Molarbeast.h"
#include "Ogre.h"
#include "Osiris.h"
#include "PumpkinJack.h"
#include "SkeletonArcher.h"
#include "SlimeEnemy.h"
#include "Sporeling.h"
#include "TitanGuard.h"
#include "ToxicVermin.h"
#include "Werewolf.h"
#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
    constexpr float kBossSupportRespawnDelay = 28.f;
    constexpr float kBossSupportMinPlayerDistance = 520.f;
    constexpr float kBossOgreInitialDelay = 22.f;

    constexpr float kEliteCageRadius = 500.f;
    constexpr float kEliteCageDamageInterval = 0.5f;
    constexpr float kEliteEnrageWarningDuration = 4.0f;
    constexpr float kHazardVolleyMinInterval = 1.5f;
    constexpr float kHazardVolleyMaxInterval = 2.5f;
    constexpr int   kHazardVolleyMinCount = 3;
    constexpr int   kHazardVolleyMaxCount = 6;
}

int CombatDirector::GetActiveEnemyCount(const std::vector<std::unique_ptr<Enemy>>& enemies) const
{
    int count = 0;
    for (const auto& enemy : enemies)
    {
        if (enemy->IsActive())
            count++;
    }
    return count;
}

bool CombatDirector::IsBossFightActive(const std::vector<std::unique_ptr<Enemy>>& enemies) const
{
    // Any living boss counts — every boss class overrides IsBoss(). The old
    // Molarbeast-only check silently broke support respawns, kill-exp
    // suppression and other boss-fight rules for the nine other bosses.
    for (const auto& enemy : enemies)
    {
        if (!enemy->IsActive() || !enemy->IsAlive() || enemy->IsDying())
            continue;
        if (enemy->IsBoss())
            return true;
    }
    return false;
}

void CombatDirector::SpawnEnemies(const CombatSpawnContext& ctx) const
{
    float mapW = ctx.map->width * ctx.mapScale;
    float mapH = ctx.map->height * ctx.mapScale;

    if (ctx.currentRoomType == RoomType::Boss)
    {
        ctx.spawnMolarbeast(Vector2{ mapW * 0.5f, mapH * 0.28f });
        ctx.spawnBossSupportAdds();
        return;
    }

    if (ctx.currentRoomType == RoomType::Rest || ctx.currentRoomType == RoomType::Store)
    {
        if (ctx.currentRoomType == RoomType::Rest)
        {
            int healCount = GetRandomValue(2, 3);
            for (int i = 0; i < healCount; i++)
            {
                Vector2 pos{ mapW * 0.5f, mapH * 0.5f };
                for (int a = 0; a < 30; a++)
                {
                    Vector2 candidate{
                        (float)GetRandomValue(300, (int)mapW - 300),
                        (float)GetRandomValue(300, (int)mapH - 300)
                    };
                    if (ctx.isSpawnPositionValid(candidate)) { pos = candidate; break; }
                }
                auto p = std::make_unique<HealPickup>();
                p->Init(pos);
                ctx.pickups->push_back(std::move(p));
            }
        }
        return;
    }

    int cornerIdx = 0;
    auto spawnPos = [&]() -> Vector2 {
        const float m = 400.f;
        const Vector2 corners[4] = {
            { m,        m        },
            { mapW - m, m        },
            { m,        mapH - m },
            { mapW - m, mapH - m },
        };
        Vector2 base = corners[cornerIdx % 4];
        cornerIdx++;

        if (ctx.isSpawnPositionValid(base)) return base;
        for (int a = 0; a < 10; a++)
        {
            Vector2 p{
                base.x + (float)GetRandomValue(-120, 120),
                base.y + (float)GetRandomValue(-120, 120)
            };
            if (ctx.isSpawnPositionValid(p)) return p;
        }
        return base;
    };

    if (ctx.currentRoomType == RoomType::Elite)
    {
        const int fodderCount = std::min(7, 3 + ctx.currentAct + std::max(0, ctx.currentRoom - 2));
        const int eliteTypeRoll = GetRandomValue(0, 2);
        Enemy* eliteMiniboss = nullptr;
        switch (eliteTypeRoll)
        {
        case 0: eliteMiniboss = ctx.spawnBasicEnemy(spawnPos()); break;
        case 1: eliteMiniboss = ctx.spawnCyclops(spawnPos()); break;
        case 2: eliteMiniboss = ctx.spawnOgre(spawnPos()); break;
        }

        if (eliteMiniboss != nullptr)
            eliteMiniboss->SetIsEliteMiniboss(true);

        // Roll a modifier COMPATIBLE with this elite (EliteModifier ids 0-3).
        // A non-curated elite (Basic/Cyclops → archetype Count) safely falls
        // back to Enrage inside ChooseEliteModifier.
        const EliteArchetype archetype = eliteMiniboss
            ? eliteMiniboss->GetEliteArchetype() : EliteArchetype::Count;
        const std::uint32_t seed = (std::uint32_t)GetRandomValue(1, 0x7ffffffe);
        *ctx.eliteMechanic = (int)ChooseEliteModifier(archetype, seed, ctx.forcedEliteMechanic);
        *ctx.eliteMinibossPtr = eliteMiniboss;
        *ctx.eliteCageRadius = 0.f;
        *ctx.eliteCageDamageTimer = 0.f;
        *ctx.eliteEnrageWarningTimer = kEliteEnrageWarningDuration;
        *ctx.eliteHazardSpawnTimer = 0.f;

        switch (*ctx.eliteMechanic)
        {
        case 0:   // Cage
            *ctx.eliteCageCenter = { mapW * 0.5f, mapH * 0.5f };
            *ctx.eliteCageRadius = kEliteCageRadius;
            *ctx.eliteCageDamageTimer = kEliteCageDamageInterval;
            break;
        case 1:   // Guard Links — reduction while guards live, never immunity
            if (*ctx.eliteMinibossPtr)
                (*ctx.eliteMinibossPtr)->SetEliteGuardLinked(true);
            break;
        case 2:   // Permanent Enrage
            if (*ctx.eliteMinibossPtr)
                (*ctx.eliteMinibossPtr)->ApplyEnrage();
            break;
        case 3:   // Arena Pressure
            *ctx.eliteHazardSpawnTimer = (float)GetRandomValue(
                (int)(kHazardVolleyMinInterval * 100.f),
                (int)(kHazardVolleyMaxInterval * 100.f)) / 100.f;
            break;
        }

        for (int i = 0; i < fodderCount; i++)
            ctx.spawnBasicEnemy(spawnPos());
        return;
    }

    // ── Encounter composition (normal rooms) ──────────────────────────────────
    // Rather than a fixed enemy count, compose the fight from tactical ROLES against
    // a depth-scaled spawn BUDGET, chosen via an encounter TEMPLATE and placed by
    // role (ranged in back, tanks between player and back line, assassins off-angle).
    // Role + cost come from each enemy type (Enemy::GetEncounterRole/GetSpawnCost).
    // Difficulty curve: early rooms stay simple (Swarm/Balanced only); role variety
    // and budget grow with depth before raw stats do.
    const int depth  = (ctx.currentAct - 1) * 5 + ctx.currentRoom;   // 1, 2, 3, ...
    int       budget = 3 + depth;                                    // grunt-equivalents
    const int tier   = (depth <= 2) ? 0 : (depth <= 6 ? 1 : 2);      // role complexity

    // Farthest corner from the player = the back line where ranged/support belong.
    Vector2 backLine{ mapW * 0.5f, mapH * 0.5f };
    {
        const float m = 400.f;
        const Vector2 corners[4] = { { m, m }, { mapW - m, m }, { m, mapH - m }, { mapW - m, mapH - m } };
        float bestDist = -1.f;
        for (const Vector2& c : corners)
        {
            float d = Vector2Distance(c, ctx.playerPos);
            if (d > bestDist) { bestDist = d; backLine = c; }
        }
    }

    // Snap a desired point to a nearby valid spawn (small jitter search).
    auto validNear = [&](Vector2 want) -> Vector2 {
        if (ctx.isSpawnPositionValid(want)) return want;
        for (int a = 0; a < 12; a++)
        {
            Vector2 p{ want.x + (float)GetRandomValue(-160, 160), want.y + (float)GetRandomValue(-160, 160) };
            if (ctx.isSpawnPositionValid(p)) return p;
        }
        return want;
    };

    // Place a spawn according to its tactical role.
    auto posForRole = [&](EnemyRole role) -> Vector2 {
        switch (role)
        {
        case EnemyRole::Ranged:
        case EnemyRole::HeavyRanged:
        case EnemyRole::Zoner:
        case EnemyRole::Support:
        case EnemyRole::Summoner:
            return validNear(backLine);                                     // safe back line
        case EnemyRole::Tank:
            return validNear(Vector2Lerp(ctx.playerPos, backLine, 0.55f));  // screen the back line
        case EnemyRole::Assassin:
        {
            Vector2 toBack = Vector2Subtract(backLine, ctx.playerPos);
            Vector2 side{ -toBack.y, toBack.x };                            // perpendicular = off-angle
            if (Vector2Length(side) > 0.01f) side = Vector2Normalize(side);
            float reach = (GetRandomValue(0, 1) ? 1.f : -1.f) * 360.f;
            return validNear(Vector2Add(ctx.playerPos, Vector2Scale(side, reach)));
        }
        default:
            return spawnPos();                                             // grunts/chargers: anywhere
        }
    };

    // Spend `cost` of the budget and spawn one unit at its role position.
    auto trySpawn = [&](const std::function<Enemy*(Vector2)>& fn, EnemyRole role, int cost) {
        if (!fn || budget < cost) return;
        fn(posForRole(role));
        budget -= cost;
    };

    // Fill whatever budget remains with cheap grunts (guarded against runaway loops).
    auto fillGrunts = [&]() {
        int guard = 0;
        while (budget > 0 && guard++ < 40)
        {
            ctx.spawnBasicEnemy(posForRole(EnemyRole::Grunt));
            budget -= 1;
        }
    };

    // Pick an encounter template appropriate to the depth tier (early tiers keep
    // only the fair, simple compositions).
    enum class Template { Swarm, Balanced, TankRanged, ZonerPressure, HeavyRanged, SupportSwarm, AssassinPressure };
    Template tmpl;
    if (tier == 0)
    {
        const Template pool[] = { Template::Swarm, Template::Balanced };
        tmpl = pool[GetRandomValue(0, 1)];
    }
    else if (tier == 1)
    {
        const Template pool[] = { Template::Balanced, Template::TankRanged, Template::ZonerPressure,
                                  Template::HeavyRanged, Template::SupportSwarm };
        tmpl = pool[GetRandomValue(0, 4)];
    }
    else
    {
        const Template pool[] = { Template::TankRanged, Template::ZonerPressure, Template::HeavyRanged,
                                  Template::SupportSwarm, Template::AssassinPressure, Template::Balanced };
        tmpl = pool[GetRandomValue(0, 5)];
    }

    switch (tmpl)
    {
    case Template::Swarm:            // many low-cost bodies, no heavy unit
        fillGrunts();
        break;
    case Template::Balanced:         // one ranged, one heavy anchor, filler grunts
        trySpawn(ctx.spawnSkeletonArcher, EnemyRole::Ranged, 2);
        trySpawn(ctx.spawnCyclops,        EnemyRole::HeavyRanged, 4);
        fillGrunts();
        break;
    case Template::TankRanged:       // shieldbearer screens archers/wisps behind it
        trySpawn(ctx.spawnShieldbearer,   EnemyRole::Tank, 3);
        trySpawn(ctx.spawnSkeletonArcher, EnemyRole::Ranged, 2);
        if (tier >= 2) trySpawn(ctx.spawnFlameWisp, EnemyRole::Zoner, 3);
        fillGrunts();
        break;
    case Template::ZonerPressure:    // flame wisps control the floor; grunts push you in
        trySpawn(ctx.spawnFlameWisp, EnemyRole::Zoner, 3);
        if (tier >= 2) trySpawn(ctx.spawnFlameWisp, EnemyRole::Zoner, 3);
        fillGrunts();
        break;
    case Template::HeavyRanged:      // cyclops anchor plus a blocker
        trySpawn(ctx.spawnCyclops,      EnemyRole::HeavyRanged, 4);
        trySpawn(ctx.spawnShieldbearer, EnemyRole::Tank, 3);
        fillGrunts();
        break;
    case Template::SupportSwarm:     // warchief buffs a swarm from safety
        trySpawn(ctx.spawnWarchief, EnemyRole::Support, 4);
        fillGrunts();
        break;
    case Template::AssassinPressure: // phantom strikes off-angle while ranged pins you
        trySpawn(ctx.spawnPhantom,        EnemyRole::Assassin, 3);
        trySpawn(ctx.spawnSkeletonArcher, EnemyRole::Ranged, 2);
        fillGrunts();
        break;
    }
}

void CombatDirector::UpdateEliteMechanics(const EliteMechanicsContext& ctx, float dt) const
{
    if (ctx.currentRoomType != RoomType::Elite)
        return;

    if (*ctx.eliteMechanic == 0 && *ctx.eliteCageRadius > 0.f)
    {
        float dist = Vector2Distance(ctx.player->GetWorldPos(), *ctx.eliteCageCenter);
        if (dist > *ctx.eliteCageRadius)
        {
            *ctx.eliteCageDamageTimer -= dt;
            if (*ctx.eliteCageDamageTimer <= 0.f)
            {
                ctx.player->TakeDamage(1, *ctx.eliteCageCenter);
                *ctx.eliteCageDamageTimer = kEliteCageDamageInterval;
            }
        }
        else
        {
            *ctx.eliteCageDamageTimer = kEliteCageDamageInterval;
        }
    }

    if (*ctx.eliteMechanic == 1
        && *ctx.eliteMinibossPtr && (*ctx.eliteMinibossPtr)->IsEliteGuardLinked()
        && (*ctx.eliteMinibossPtr)->IsActive() && !(*ctx.eliteMinibossPtr)->IsDying())
    {
        bool anyGruntAlive = false;
        for (const auto& e : *ctx.enemies)
        {
            if (e.get() == *ctx.eliteMinibossPtr) continue;
            if (e->IsActive() && e->IsAlive() && !e->IsDying())
            { anyGruntAlive = true; break; }
        }
        if (!anyGruntAlive)
        {
            (*ctx.eliteMinibossPtr)->SetEliteGuardLinked(false);
            // Payoff callout: the damage reduction only breaks once its guards
            // fall — this is what teaches the "kill the adds first" rule.
            (*ctx.eliteMinibossPtr)->RequestBossCallout("GUARD BROKEN");
        }
    }

    if (*ctx.eliteEnrageWarningTimer > 0.f)
        *ctx.eliteEnrageWarningTimer -= dt;

    if (*ctx.eliteMechanic == 3)
    {
        *ctx.eliteHazardSpawnTimer -= dt;
        if (*ctx.eliteHazardSpawnTimer <= 0.f)
        {
            // Environmental pressure budget: volleys share the same in-flight
            // cap as room hazards, so Arena Pressure can never smother the
            // room. A full budget briefly delays the volley instead.
            int environmentalShotsInFlight = 0;
            for (const LavaBallProjectile& shot : *ctx.lavaBalls)
                if (shot.IsActive())
                    environmentalShotsInFlight++;
            if (environmentalShotsInFlight >= Balance::Hazards::kEnvProjectileCap)
            {
                *ctx.eliteHazardSpawnTimer = 0.4f;
                return;
            }

            // Themed by elite archetype: the shared shot art carries the
            // elite's identity colour (Infernal keeps the natural lava look).
            Color volleyTint = WHITE;
            if (*ctx.eliteMinibossPtr)
            {
                switch ((*ctx.eliteMinibossPtr)->GetEliteArchetype())
                {
                case EliteArchetype::Bonechill: volleyTint = Color{ 150, 215, 255, 255 }; break;
                case EliteArchetype::Stormclub: volleyTint = Color{ 255, 230, 140, 255 }; break;
                case EliteArchetype::Venomfang: volleyTint = Color{ 140, 235,  90, 255 }; break;
                default: break;   // Ogre (incompatible) / Infernal: natural fire
                }
            }

            const float mapW = (ctx.worldBoundsW > 0.f) ? ctx.worldBoundsW : ctx.map->width  * ctx.mapScale;
            const float mapH = (ctx.worldBoundsH > 0.f) ? ctx.worldBoundsH : ctx.map->height * ctx.mapScale;
            const float marginLeft = 120.f;
            const float marginRight = 120.f;
            const float marginTop = 90.f;
            const float marginBottom = 220.f;
            const Vector2 playerPos = ctx.player->GetWorldPos();
            const int budgetRemaining = Balance::Hazards::kEnvProjectileCap - environmentalShotsInFlight;
            const int volleyCount = std::min(budgetRemaining,
                GetRandomValue(kHazardVolleyMinCount, kHazardVolleyMaxCount));

            for (int i = 0; i < volleyCount; ++i)
            {
                Vector2 spawnPos{};
                bool foundSpawn = false;

                for (int attempt = 0; attempt < 24; ++attempt)
                {
                    spawnPos = {
                        (float)GetRandomValue((int)marginLeft, (int)(mapW - marginRight)),
                        (float)GetRandomValue((int)marginTop,  (int)(mapH - marginBottom))
                    };

                    if (Vector2Distance(spawnPos, playerPos) < 240.f)
                        continue;
                    if (!ctx.isSpawnPositionValid(spawnPos))
                        continue;

                    foundSpawn = true;
                    break;
                }

                if (!foundSpawn)
                    continue;

                Vector2 toPlayer = Vector2Subtract(playerPos, spawnPos);
                float baseAngle = atan2f(toPlayer.y, toPlayer.x);
                float spread = ((float)GetRandomValue(-28, 28)) * DEG2RAD;
                float angle = baseAngle + spread;

                LavaBallProjectile projectile;
                projectile.Init(spawnPos, Vector2{ cosf(angle), sinf(angle) });
                projectile.SetTint(volleyTint);
                ctx.lavaBalls->push_back(projectile);
            }

            *ctx.eliteHazardSpawnTimer = (float)GetRandomValue(
                (int)(kHazardVolleyMinInterval * 100.f),
                (int)(kHazardVolleyMaxInterval * 100.f)) / 100.f;
            if (ctx.triggerScreenShake)
                ctx.triggerScreenShake(2.f, 0.06f);
        }
    }
}

void CombatDirector::UpdateEnemyRuntime(const EnemyRuntimeContext& ctx, float dt) const
{
    Vector2 playerFeet = ctx.player->GetFeetWorldPos();

    _propCentersScratch.clear();
    if (ctx.propCenters != nullptr && !ctx.propCenters->empty())
    {
        _propCentersScratch = *ctx.propCenters;
    }
    else if (ctx.props != nullptr && !ctx.props->empty())
    {
        _propCentersScratch.reserve(ctx.props->size());
        for (const auto& prop : *ctx.props)
            _propCentersScratch.push_back(prop.GetWorldPos());
    }
    const std::vector<Vector2>& propCenters = _propCentersScratch;

    // ── Squad directive: one battlefield read shared by every enemy ──────────
    // Threat assessment (does the player look beatable?), the tank anchoring the
    // advance, whether the player is already engaged, and where allies are
    // massed. Built once here, then handed to each enemy before its Update.
    SquadDirective squadDirective;
    {
        namespace Squad = Balance::Squad;

        Vector2 allySum{};
        Enemy*  leaderTank = nullptr;
        float   leaderTankDist = 0.f;

        for (const auto& enemy : *ctx.enemies)
        {
            if (!enemy->IsActive() || !enemy->IsAlive() || enemy->IsDying())
                continue;
            if (enemy->IsBoss())
                continue;   // bosses fight their own fight, not in the pack

            squadDirective.allyCount++;
            allySum = Vector2Add(allySum, enemy->GetWorldPos());

            float distToPlayer = Vector2Distance(enemy->GetWorldPos(), playerFeet);
            if (enemy->GetEncounterRole() == EnemyRole::Tank &&
                (leaderTank == nullptr || distToPlayer < leaderTankDist))
            {
                leaderTank = enemy.get();
                leaderTankDist = distToPlayer;
            }

            if (enemy->IsCommittedToAttack() && distToPlayer < Squad::kEngagedNearPlayerRadius)
                squadDirective.playerEngaged = true;
        }

        if (squadDirective.allyCount > 0)
            squadDirective.allyCentroid = Vector2Scale(allySum, 1.f / (float)squadDirective.allyCount);
        if (leaderTank != nullptr)
        {
            squadDirective.hasLeader = true;
            squadDirective.leaderPos = leaderTank->GetWorldPos();
        }

        // Threat tiers: a hurt player draws blood-frenzy, a healthy one respect;
        // pack size adds courage, thin numbers add caution.
        float playerHealthFraction = 1.f;
        if (ctx.player->GetMaxHealthValue() > 0.f)
            playerHealthFraction = ctx.player->GetHealthValue() / ctx.player->GetMaxHealthValue();

        float aggression = 1.f;
        if (playerHealthFraction < Squad::kPlayerLowHealthFrac)
            aggression += Squad::kLowHealthAggroBonus;
        else if (playerHealthFraction > Squad::kPlayerHealthyFrac)
            aggression -= Squad::kHealthyAggroPenalty;

        if (squadDirective.allyCount >= Squad::kPackCourageCount)
            aggression += Squad::kPackCourageBonus;
        else if (squadDirective.allyCount <= Squad::kLonelyCount)
            aggression -= Squad::kLonelyPenalty;

        squadDirective.aggression =
            (aggression < Squad::kAggressionMin) ? Squad::kAggressionMin :
            (aggression > Squad::kAggressionMax) ? Squad::kAggressionMax : aggression;
    }

    // ── Engagement assignments: built ONCE per frame, before any enemy Update
    // runs, so every enemy sees the same stable Commit/Support/Reposition
    // picture this frame (see docs/superpowers/plans/2026-08-13-hades-style-
    // enemy-combat-rhythm.md, Task 2). Bosses are deliberately excluded from
    // the candidate pool — like the squad directive above, they fight their
    // own fight rather than competing for a normal-enemy commit slot; their
    // own HandleAttack (if any) falls back to the legacy CanTakeAttackSlot
    // scan via Enemy::HasEngagementAssignment() staying false for them.
    std::vector<EngagementCandidate> engagementCandidates;
    engagementCandidates.reserve(ctx.enemies->size());
    for (const auto& enemy : *ctx.enemies)
    {
        if (enemy->IsBoss())
            continue;

        EngagementCandidate candidate;
        candidate.id = enemy->GetRuntimeId();
        candidate.role = enemy->GetEncounterRole();
        candidate.distance = Vector2Distance(enemy->GetWorldPos(), playerFeet);
        // Dead/inactive/dying/pit-falling enemies do not occupy commit slots
        // (see design "State and Failure Handling"); BuildEngagementAssignments
        // omits alive==false candidates from its output entirely.
        candidate.alive = enemy->IsActive() && enemy->IsAlive() && !enemy->IsDying()
            && !enemy->IsPitFalling();
        // Owns its slot through attack + recovery (see EngagementLatch).
        candidate.locked = enemy->IsAttackLockedForEngagement();
        // "Locked but not currently mid-swing" == the post-attack recovery
        // window specifically (informational; BuildEngagementAssignments does
        // not currently branch on this field).
        candidate.recovering = candidate.locked && !enemy->IsCommittedToAttack();
        candidate.swarm = enemy->IsSwarmProfile();
        engagementCandidates.push_back(candidate);
    }

    const std::vector<EngagementAssignment> engagementAssignments =
        BuildEngagementAssignments(engagementCandidates, ctx.tier, ctx.swarmEncounter,
                                    _engagementSequence++);

    std::unordered_map<std::uint64_t, const EngagementAssignment*> engagementById;
    engagementById.reserve(engagementAssignments.size());
    for (const EngagementAssignment& assignment : engagementAssignments)
        engagementById[assignment.id] = &assignment;

    // Spawning during iteration would invalidate the loop when ctx.enemies
    // reallocates, so boss summons are collected here and executed after.
    std::vector<Vector2> pendingSmallSlimeSpawns;
    std::vector<Vector2> pendingBasicEnemySpawns;

    for (auto& enemy : *ctx.enemies)
    {
        if (!enemy->IsActive())
            continue;

        // Pit falls own movement and scale until the sink animation completes.
        // Running normal AI here would let an enemy attack while dropping away.
        if (enemy->IsPitFalling())
        {
            enemy->UpdatePitFall(dt);
            continue;
        }

        Vector2 navigationTarget = playerFeet;
        bool hasNavigationTarget = false;

        if (!enemy->UsesDirectPursuit() &&
            !ctx.nav->HasLineOfSight(enemy->GetWorldPos(), playerFeet))
        {
            navigationTarget = ctx.nav->GetTarget(enemy->GetWorldPos(), playerFeet);
            hasNavigationTarget = !Vector2Equals(navigationTarget, playerFeet);
        }

        // Apply this frame's engagement assignment before Update() so
        // HandleAttack's Commit gate and (from Task 3 onward) positioning see
        // it immediately. Bosses are never in BuildEngagementAssignments'
        // candidate list (they fight their own fight, not the pack), so they
        // must be skipped here too — otherwise SetEngagementAssignment would
        // still run and HasEngagementAssignment() would report true, which
        // would silently disable a boss's legacy CanTakeAttackSlot fallback
        // in Enemy::HandleAttack forever (it can never match a Commit
        // assignment since it's permanently excluded from the candidate
        // list). Every other enemy with no assignment (anything
        // BuildEngagementAssignments dropped as not-alive) falls back to
        // holding its current position with Reposition intent.
        if (!enemy->IsBoss())
        {
            auto assignmentIt = engagementById.find(enemy->GetRuntimeId());
            if (assignmentIt != engagementById.end())
            {
                const EngagementAssignment& assignment = *assignmentIt->second;
                // A Commit that is actually in its post-attack recovery window
                // (locked from a finished attack, not currently mid-swing) is
                // re-labelled Reposition ONLY for the target computation below —
                // the stored intent (assignment.intent) stays the real Commit so
                // Enemy::HandleAttack's slot gate and next frame's
                // EngagementCandidate::locked still see it as owning its slot
                // through recovery. See Task 3 plan + PROGRESS.md's "IMPORTANT
                // FOR TASK 3" note: GetEngagementIntent()==Commit alone covers
                // both "attacking" and "recovering" (both map to `locked`).
                bool recoveringFromCommit = (assignment.intent == EngagementIntent::Commit)
                    && enemy->IsAttackLockedForEngagement() && !enemy->IsCommittedToAttack();
                EngagementIntent targetIntent = recoveringFromCommit ? EngagementIntent::Reposition : assignment.intent;
                Vector2 target = ComputeEngagementTarget(playerFeet, enemy->GetWorldPos(),
                    squadDirective.allyCentroid, enemy->GetEncounterRole(), targetIntent, assignment.orbitAngle);
                enemy->SetEngagementAssignment(assignment.intent, target);
            }
            else
            {
                enemy->SetEngagementAssignment(EngagementIntent::Reposition, enemy->GetWorldPos());
            }
        }

        enemy->SetHazardZones(ctx.hazards);   // player damage zones to steer around
        enemy->SetSquadDirective(&squadDirective);   // pack behaviour (threat/formation)
        enemy->Update(dt, playerFeet, navigationTarget, hasNavigationTarget, *ctx.enemies, propCenters);

        if (Cyclops* cyclops = enemy->AsCyclops())
        {
            if (cyclops->WantsToFire())
            {
                CyclopsLaserProjectile laser;
                if (cyclops->GetFireMode() == Cyclops::FireMode::Scatter)
                    laser.InitScatter(cyclops->GetWorldPos(), cyclops->GetFireDirection(), cyclops->GetAttackPower());
                else
                    laser.InitSweep(cyclops->GetWorldPos(), cyclops->GetFireDirection(), cyclops->GetAttackPower());
                ctx.cyclopsLasers->push_back(laser);
                cyclops->OnFired();
                cyclops->PlayAttackSound();
            }
        }

        // Boss enrage transition — any boss that just crossed into its enrage /
        // ignite / final phase gets a big screen shake so the phase change reads.
        if (enemy->IsBoss() && enemy->ConsumeEnrageShakeRequest() && ctx.triggerScreenShake)
            ctx.triggerScreenShake(12.f, 0.35f);

        // Floating state word (ENRAGED / PHASE SHIFT) announced on a transition.
        // Independent of the shake consumer above so both fire on the same frame.
        if (const char* callout = enemy->ConsumeBossCallout())
            if (ctx.spawnBossCallout)
                ctx.spawnBossCallout(enemy->GetWorldPos(), callout);

        // Drain this elite's signature events immediately after its Update so
        // zones snapshot world positions from THIS frame. Events translate into
        // bounded attack zones, sounds, VFX and shake — the enemy AI never owns
        // renderer or player-damage responsibilities directly.
        {
            EliteSignatureEvent signatureEvent{};
            while (enemy->ConsumeEliteEvent(signatureEvent))
                SpawnEliteZonesForEvent(signatureEvent, ctx);
        }

        if (Ogre* ogre = enemy->AsOgre())
        {
            if (ogre->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(8.f, 0.14f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(ogre->GetWorldPos(), (int)BossFx::HeavyStrike);
            }
        }

        if (SkeletonArcher* archer = enemy->AsSkeletonArcher())
        {
            if (archer->WantsToFireArrow() && ctx.enemyProjectiles != nullptr)
            {
                EnemyProjectile arrow;
                arrow.Init(archer->GetWorldPos(), archer->GetArrowDirection(),
                    EnemyProjectileKind::Arrow, archer->GetAttackPower());
                ctx.enemyProjectiles->push_back(arrow);
                archer->OnArrowFired();
                archer->PlayAttackSound();
            }
        }

        if (FlameWisp* wisp = enemy->AsFlameWisp())
        {
            if (wisp->WantsToCastBolt() && ctx.enemyProjectiles != nullptr)
            {
                EnemyProjectile bolt;
                bolt.Init(wisp->GetWorldPos(), wisp->GetBoltDirection(),
                    EnemyProjectileKind::FireBolt, wisp->GetAttackPower());
                ctx.enemyProjectiles->push_back(bolt);
                wisp->OnBoltCast();
                wisp->PlayAttackSound();
            }
        }

        if (Molarbeast* molarbeast = enemy->AsMolarbeast())
        {
            if (molarbeast->WantsToFireLavaBall())
            {
                LavaBallProjectile projectile;
                Vector2 toTarget = Vector2Subtract(molarbeast->GetQueuedLavaBallTarget(), molarbeast->GetLavaBallSpawnPos());
                projectile.Init(molarbeast->GetLavaBallSpawnPos(), toTarget, "Molarbeast_Ranged_Volley");
                ctx.lavaBalls->push_back(projectile);
                molarbeast->OnLavaBallSpawned();
            }

            if (molarbeast->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(8.f, 0.14f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(molarbeast->GetWorldPos(), (int)BossFx::HeavyStrike);
            }
        }

        if (AbyssSlime* abyssSlime = enemy->AsAbyssSlime())
        {
            // Abyss Call — small slimes burst out in a ring around the boss.
            int summonCount = abyssSlime->ConsumeSummonRequest();
            if (summonCount > 0 && ctx.spawnSmallSlime)
            {
                for (int i = 0; i < summonCount; i++)
                {
                    float angle = ((float)i / (float)summonCount) * 2.f * PI;
                    pendingSmallSlimeSpawns.push_back(Vector2{
                        abyssSlime->GetWorldPos().x + cosf(angle) * 190.f,
                        abyssSlime->GetWorldPos().y + sinf(angle) * 150.f
                    });
                }
            }

            if (abyssSlime->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(9.f, 0.16f);
                if (ctx.requestHitStop) ctx.requestHitStop(0.045f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(abyssSlime->GetWorldPos(), (int)BossFx::SlimeSlam);
            }
        }

        if (PumpkinJack* pumpkinJack = enemy->AsPumpkinJack())
        {
            // Harvest Volley — fan of fire bolts aimed at the player.
            if (pumpkinJack->WantsToCastVolley() && ctx.enemyProjectiles != nullptr)
            {
                int boltCount = pumpkinJack->GetVolleyBoltCount();
                Vector2 aimDir = pumpkinJack->GetVolleyDirection();
                float baseAngle = atan2f(aimDir.y, aimDir.x);
                float fanSpread = 0.28f;   // radians between adjacent bolts

                for (int i = 0; i < boltCount; i++)
                {
                    float angleOffset = ((float)i - (float)(boltCount - 1) * 0.5f) * fanSpread;
                    Vector2 boltDir{ cosf(baseAngle + angleOffset), sinf(baseAngle + angleOffset) };
                    EnemyProjectile bolt;
                    bolt.Init(pumpkinJack->GetWorldPos(), boltDir,
                        EnemyProjectileKind::FireBolt, pumpkinJack->GetAttackPower(), "PumpkinJack_Volley");
                    ctx.enemyProjectiles->push_back(bolt);
                }
                pumpkinJack->OnVolleyCast();
            }

            // Grave Call — shadow grunts crawl out near the boss.
            int summonCount = pumpkinJack->ConsumeSummonRequest();
            if (summonCount > 0 && ctx.spawnBasicEnemy)
            {
                for (int i = 0; i < summonCount; i++)
                {
                    float angle = ((float)i / (float)summonCount) * 2.f * PI + 0.6f;
                    pendingBasicEnemySpawns.push_back(Vector2{
                        pumpkinJack->GetWorldPos().x + cosf(angle) * 210.f,
                        pumpkinJack->GetWorldPos().y + sinf(angle) * 160.f
                    });
                }
            }
        }

        if (Minotaur* minotaur = enemy->AsMinotaur())
        {
            if (minotaur->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(10.f, 0.18f);
                if (ctx.requestHitStop) ctx.requestHitStop(0.05f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(minotaur->GetWorldPos(), (int)BossFx::CrushingSlam);
            }
        }

        if (BomberImp* bomber = enemy->AsBomberImp())
        {
            if (bomber->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(7.f, 0.14f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(bomber->GetWorldPos(), (int)BossFx::HeavyStrike);
            }
        }

        if (Werewolf* werewolf = enemy->AsWerewolf())
        {
            if (werewolf->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(8.f, 0.14f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(werewolf->GetWorldPos(), (int)BossFx::PounceImpact);
            }
        }

        if (ChompBug* chompBug = enemy->AsChompBug())
        {
            // Acid Barrage — fan of toxic globs.
            if (chompBug->WantsToSpit() && ctx.enemyProjectiles != nullptr)
            {
                int globCount = chompBug->GetSpitCount();
                Vector2 aimDir = chompBug->GetSpitDirection();
                float baseAngle = atan2f(aimDir.y, aimDir.x);
                for (int i = 0; i < globCount; i++)
                {
                    float angleOffset = ((float)i - (float)(globCount - 1) * 0.5f) * 0.24f;
                    EnemyProjectile glob;
                    glob.Init(chompBug->GetWorldPos(),
                        Vector2{ cosf(baseAngle + angleOffset), sinf(baseAngle + angleOffset) },
                        EnemyProjectileKind::Spit, chompBug->GetAttackPower(), "ChompBug_Acid_Spit_Fan");
                    ctx.enemyProjectiles->push_back(glob);
                }
                chompBug->OnSpitFired();
            }
        }

        if (Osiris* osiris = enemy->AsOsiris())
        {
            // Judgement Nova — a ring of bolts with AUTHORED safe wedges: any
            // bolt inside a gap is skipped, so the counterplay is reading the
            // wedge the telegraph showed, not pixel-threading bolt spacing.
            if (osiris->WantsToCastNova() && ctx.enemyProjectiles != nullptr)
            {
                const int boltCount = osiris->GetNovaBoltCount();
                const int gapCount = osiris->GetNovaGapCount();
                const float gapCentre = osiris->GetNovaGapCentreAngle();
                const float gapHalf = osiris->GetNovaGapHalfWidth();
                auto angleInsideGap = [&](float angle)
                {
                    for (int gapIndex = 0; gapIndex < gapCount; ++gapIndex)
                    {
                        float wedgeCentre = gapCentre + PI * (float)gapIndex;
                        float difference = fmodf(angle - wedgeCentre + 3.f * PI, 2.f * PI) - PI;
                        if (fabsf(difference) < gapHalf)
                            return true;
                    }
                    return false;
                };
                for (int i = 0; i < boltCount; i++)
                {
                    float angle = ((float)i / (float)boltCount) * 2.f * PI;
                    if (angleInsideGap(angle))
                        continue;
                    EnemyProjectile bolt;
                    bolt.Init(osiris->GetWorldPos(), Vector2{ cosf(angle), sinf(angle) },
                        EnemyProjectileKind::FireBolt, osiris->GetAttackPower(), "Osiris_Judgement_Nova");
                    ctx.enemyProjectiles->push_back(bolt);
                }
                // Release flash: the judgement ring erupts from real cast art,
                // not just spawning bolts out of thin air.
                if (ctx.spawnEliteFx)
                    ctx.spawnEliteFx(osiris->GetWorldPos(), (int)BossFx::DivineSlash,
                                     6.f, Color{ 255, 225, 130, 255 });
                osiris->OnNovaCast();
            }
            // Wrath Volley — tight aimed fan.
            if (osiris->WantsToCastVolley() && ctx.enemyProjectiles != nullptr)
            {
                int boltCount = osiris->GetVolleyBoltCount();
                Vector2 aimDir = osiris->GetVolleyDirection();
                float baseAngle = atan2f(aimDir.y, aimDir.x);
                for (int i = 0; i < boltCount; i++)
                {
                    float angleOffset = ((float)i - (float)(boltCount - 1) * 0.5f) * 0.18f;
                    EnemyProjectile bolt;
                    bolt.Init(osiris->GetWorldPos(),
                        Vector2{ cosf(baseAngle + angleOffset), sinf(baseAngle + angleOffset) },
                        EnemyProjectileKind::FireBolt, osiris->GetAttackPower(), "Osiris_Wrath_Volley");
                    ctx.enemyProjectiles->push_back(bolt);
                }
                osiris->OnVolleyCast();
            }
        }

        if (TitanGuard* titanGuard = enemy->AsTitanGuard())
        {
            // Bomb Lob — reuses the lavaball projectile like Molarbeast.
            if (titanGuard->WantsToThrowBomb() && ctx.lavaBalls != nullptr)
            {
                LavaBallProjectile bomb;
                Vector2 toTarget = Vector2Subtract(titanGuard->GetBombTarget(), titanGuard->GetBombSpawnPos());
                bomb.Init(titanGuard->GetBombSpawnPos(), toTarget, "TitanGuard_Bomb_Lob");
                ctx.lavaBalls->push_back(bomb);
                titanGuard->OnBombThrown();
            }
            if (titanGuard->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(11.f, 0.2f);
                if (ctx.requestHitStop) ctx.requestHitStop(0.05f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(titanGuard->GetWorldPos(), (int)BossFx::BulwarkSlam);
            }
        }

        if (ToxicVermin* vermin = enemy->AsToxicVermin())
        {
            // Toxic spit fan.
            if (vermin->WantsToSpit() && ctx.enemyProjectiles != nullptr)
            {
                int globCount = vermin->GetSpitCount();
                Vector2 aimDir = vermin->GetSpitDirection();
                float baseAngle = atan2f(aimDir.y, aimDir.x);
                for (int i = 0; i < globCount; i++)
                {
                    float angleOffset = ((float)i - (float)(globCount - 1) * 0.5f) * 0.26f;
                    EnemyProjectile glob;
                    glob.Init(vermin->GetWorldPos(),
                        Vector2{ cosf(baseAngle + angleOffset), sinf(baseAngle + angleOffset) },
                        EnemyProjectileKind::Spit, vermin->GetAttackPower(), "ToxicVermin_Toxic_Spit_Fan");
                    ctx.enemyProjectiles->push_back(glob);
                }
                vermin->OnSpitFired();
            }
            // Poison pools left by burrows and eruptions.
            Vector2 poolPos;
            if (vermin->ConsumePoisonPoolRequest(poolPos))
            {
                if (ctx.spawnBossPoisonPool) ctx.spawnBossPoisonPool(poolPos);
                if (ctx.spawnBossFx) ctx.spawnBossFx(poolPos, (int)BossFx::PoisonPool);
            }

            if (vermin->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(9.f, 0.16f);
                if (ctx.spawnBossFx) ctx.spawnBossFx(vermin->GetWorldPos(), (int)BossFx::ToxicEruption);
            }
        }

        if (AncientBear* bear = enemy->AsAncientBear())
        {
            if (bear->ConsumeImpactShakeRequest())
            {
                if (ctx.triggerScreenShake) ctx.triggerScreenShake(12.f, 0.22f);
                if (ctx.requestHitStop) ctx.requestHitStop(0.055f);
                // The impact ART matches the warned SHAPE: the disc bursts at
                // his feet, the ring scatters bursts around the band, and the
                // wedge bursts march down the sector's centreline.
                if (ctx.spawnBossFx) ctx.spawnBossFx(bear->GetWorldPos(), (int)BossFx::CrushingSlam);
                if (ctx.spawnEliteFx)
                {
                    const Vector2 bearPosition = bear->GetWorldPos();
                    const Color dreamPurple{ 210, 150, 255, 255 };
                    if (bear->GetSlamVariant() == 1)
                    {
                        const float ringRadius = bear->GetSlamRingMidRadius();
                        for (int burstIndex = 0; burstIndex < 4; ++burstIndex)
                        {
                            const float burstAngle = (float)burstIndex * (PI * 0.5f) + 0.4f;
                            ctx.spawnEliteFx(Vector2{ bearPosition.x + cosf(burstAngle) * ringRadius,
                                                      bearPosition.y + sinf(burstAngle) * ringRadius },
                                             (int)BossFx::CrushingSlam, 4.2f, dreamPurple);
                        }
                    }
                    else if (bear->GetSlamVariant() == 2)
                    {
                        const float wedgeAngle = bear->GetSlamWedgeAngle();
                        const float wedgeReach = bear->GetSlamWedgeReach();
                        for (float along = wedgeReach * 0.4f; along <= wedgeReach; along += wedgeReach * 0.3f)
                            ctx.spawnEliteFx(Vector2{ bearPosition.x + cosf(wedgeAngle) * along,
                                                      bearPosition.y + sinf(wedgeAngle) * along },
                                             (int)BossFx::CrushingSlam, 4.2f, dreamPurple);
                    }
                }
            }
        }
    }

    // Safe to grow the enemy vector now that the iteration is finished.
    for (const Vector2& spawnPos : pendingSmallSlimeSpawns)
        ctx.spawnSmallSlime(spawnPos);
    for (const Vector2& spawnPos : pendingBasicEnemySpawns)
        ctx.spawnBasicEnemy(spawnPos);

    // Tick the live elite attack zones (damage + statuses resolve centrally),
    // and re-arm the one-shake-per-cast throttle for the next frame.
    UpdateEliteZones(ctx, dt);
    _eliteImpactFeedbackThisFrame = false;
}

// ── Elite signature zones ────────────────────────────────────────────────────

EliteAttackZone* CombatDirector::AcquireEliteZone() const
{
    for (EliteAttackZone& zone : _eliteZones)
    {
        if (!zone.active)
        {
            zone = EliteAttackZone{};
            zone.active = true;
            zone.sequence = _eliteZoneSequence++;
            return &zone;
        }
    }
    _eliteZonesDropped++;
    return nullptr;
}

void CombatDirector::SpawnEliteZonesForEvent(const EliteSignatureEvent& event,
                                             const EnemyRuntimeContext& ctx) const
{
    namespace Elite = Balance::Elite;
    SfxBank& sfx = SfxBank::Get();

    // One combined impact response per cast: the first Execute this frame plays
    // the sound and shake; further branches/fissures stay silent.
    auto impactFeedback = [&](float shakeStrength, float shakeDuration)
    {
        if (_eliteImpactFeedbackThisFrame)
            return;
        _eliteImpactFeedbackThisFrame = true;
        sfx.PlayEliteImpact((int)event.archetype);
        if (ctx.triggerScreenShake)
            ctx.triggerScreenShake(shakeStrength, shakeDuration);
    };
    auto spawnFx = [&](Vector2 position, BossFx fxId, float scale, Color tint)
    {
        if (ctx.spawnEliteFx)
            ctx.spawnEliteFx(position, (int)fxId, scale, tint);
    };
    auto spawnHazardFx = [&](Vector2 position, BossFx fxId, float scale,
                             float duration, Color tint)
    {
        if (ctx.spawnEliteHazardFx)
            ctx.spawnEliteHazardFx(position, (int)fxId, scale, duration, tint);
    };
    // Sprite bursts spaced along a lane so the whole fissure/branch reads as
    // art rather than one puff at the origin.
    auto spawnLaneFx = [&](Vector2 start, Vector2 direction, float length,
                           BossFx fxId, float scale, Color tint)
    {
        for (float along = length * 0.33f; along <= length; along += length * 0.33f)
            spawnFx(Vector2{ start.x + direction.x * along, start.y + direction.y * along },
                    fxId, scale, tint);
    };

    const Color fireOrange { 255, 140,  60, 255 };
    const Color iceBlue    { 150, 215, 255, 255 };
    const Color stormGold  { 255, 230, 140, 255 };
    const Color toxicGreen { 140, 235,  90, 255 };

    switch (event.kind)
    {
    case EliteEventKind::Telegraph:
        sfx.PlayEliteTelegraph((int)event.archetype);
        return;

    case EliteEventKind::Lock:
        // The lock beat is FELT, not just seen: a short mechanical click as
        // the warning freezes, so "it can no longer turn" registers by ear.
        sfx.Play(SfxId::TrapArm, 0.5f);
        return;

    case EliteEventKind::PhaseChange:
        sfx.PlayElitePhase();
        if (ctx.triggerScreenShake)
            ctx.triggerScreenShake(10.f, 0.3f);
        // A brief red wash + a few frozen frames sell the escalation as an
        // EVENT (Diablo-style), not just a stat change.
        if (ctx.triggerScreenFlash)
            ctx.triggerScreenFlash(Color{ 255, 70, 50, 255 }, 0.14f);
        if (ctx.requestHitStop)
            ctx.requestHitStop(0.06f);
        return;

    case EliteEventKind::Recover:
        // Punish window opens: announce it so players LEARN the rhythm —
        // dodging well is what buys these moments.
        if (ctx.spawnBossCallout)
            ctx.spawnBossCallout(event.origin, "WIDE OPEN");
        return;

    case EliteEventKind::Execute:
    case EliteEventKind::TrailPatch:
        break;    // fall through to the per-move zone table below
    }

    switch (event.move)
    {
    case EliteMove::OgreCharge:
        // The Ogre's body IS the hitbox (its rush code owns contact damage);
        // Execute here only marks the launch beat.
        if (event.kind == EliteEventKind::Execute)
            impactFeedback(5.f, 0.12f);
        break;

    case EliteMove::InfernalCinderMarch:
        if (event.kind == EliteEventKind::TrailPatch)
        {
            // Short-lived flame patch: lingering disc that burns on a tick.
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Disc;
                zone->status = EliteStatusPayload::Burn;
                zone->start = event.origin;
                zone->radius = 70.f;
                zone->activeRemaining = Elite::kInfernalPatchLifetime;
                zone->tickInterval = 0.55f;
                zone->tickRemaining = 0.15f;   // brief grace as the patch ignites
                zone->damage = 1.f;
            }
            spawnHazardFx(event.origin, BossFx::PoisonPool, 2.2f,
                          Elite::kInfernalPatchLifetime, fireOrange);
        }
        break;

    case EliteMove::InfernalFurnaceBurst:
        if (event.kind == EliteEventKind::Execute)
        {
            // One fire fissure: a lane along the locked direction.
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Lane;
                zone->status = EliteStatusPayload::Burn;
                zone->start = event.origin;
                zone->end = Vector2{ event.origin.x + event.direction.x * Elite::kInfernalFissureLength,
                                     event.origin.y + event.direction.y * Elite::kInfernalFissureLength };
                zone->radius = Elite::kInfernalFissureWidth;
                zone->activeRemaining = 0.5f;
                zone->damage = 2.f;
            }
            spawnLaneFx(event.origin, event.direction, Elite::kInfernalFissureLength,
                        BossFx::ToxicEruption, 4.2f, fireOrange);
            impactFeedback(7.f, 0.18f);
        }
        break;

    case EliteMove::BonechillPermafrostSlam:
        if (event.kind == EliteEventKind::Execute)
        {
            // The direct slam: a heavy disc right in front of the wall.
            Vector2 slamCentre{ event.origin.x + event.direction.x * 150.f,
                                event.origin.y + event.direction.y * 150.f };
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Disc;
                zone->status = EliteStatusPayload::Chill;
                zone->start = slamCentre;
                zone->radius = 140.f;
                zone->activeRemaining = 0.35f;
                zone->damage = 3.f;
            }
            spawnFx(slamCentre, BossFx::BulwarkSlam, 6.f, iceBlue);
            impactFeedback(9.f, 0.22f);
        }
        else   // TrailPatch = one ice lane
        {
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Lane;
                zone->status = EliteStatusPayload::Chill;
                zone->start = event.origin;
                zone->end = Vector2{ event.origin.x + event.direction.x * Elite::kBonechillLaneLength,
                                     event.origin.y + event.direction.y * Elite::kBonechillLaneLength };
                zone->radius = Elite::kBonechillLaneWidth;
                zone->activeRemaining = 0.5f;
                zone->damage = 1.f;
            }
            spawnLaneFx(event.origin, event.direction, Elite::kBonechillLaneLength,
                        BossFx::DiveImpact, 3.4f, iceBlue);
        }
        break;

    case EliteMove::StormclubThunderLeap:
        if (event.kind == EliteEventKind::Execute)
        {
            // Central landing impact — most damage plus a real knockback.
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Disc;
                zone->status = EliteStatusPayload::Knockback;
                zone->start = event.target;
                zone->radius = Elite::kStormclubImpactRadius;
                zone->activeRemaining = 0.3f;
                zone->damage = 3.f;
            }
            spawnFx(event.target, BossFx::HeavyStrike, 6.5f, stormGold);
            impactFeedback(10.f, 0.25f);
        }
        else   // TrailPatch = one lightning branch
        {
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Lane;
                zone->status = EliteStatusPayload::Shock;
                zone->start = event.origin;
                zone->end = Vector2{ event.origin.x + event.direction.x * Elite::kStormclubBranchLength,
                                     event.origin.y + event.direction.y * Elite::kStormclubBranchLength };
                zone->radius = Elite::kStormclubBranchWidth;
                zone->activeRemaining = 0.4f;
                zone->damage = 1.f;
            }
            spawnLaneFx(event.origin, event.direction, Elite::kStormclubBranchLength,
                        BossFx::DashDust, 3.2f, stormGold);
        }
        break;

    case EliteMove::MolarbeastCharge:
        // The boss body owns the charge damage (its dash handler); Execute
        // marks the launch beat with dust art, sound, and one combined shake.
        if (event.kind == EliteEventKind::Execute)
        {
            spawnFx(event.origin, BossFx::DashDust, 5.f, fireOrange);
            impactFeedback(6.f, 0.15f);
        }
        break;

    case EliteMove::MolarbeastLavaTrail:
        if (event.kind == EliteEventKind::TrailPatch)
        {
            // Burning ground along a finished stampede lane: ignites after a
            // beat (never instant), burns briefly, then the arena resets.
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Disc;
                zone->status = EliteStatusPayload::Burn;
                zone->start = event.origin;
                zone->radius = 78.f;
                zone->telegraphRemaining = 0.45f;   // visible ignition delay
                zone->telegraphDuration = 0.45f;
                zone->activeRemaining = 2.6f;
                zone->tickInterval = 0.55f;
                zone->tickRemaining = 0.1f;
                zone->damage = 1.f;
            }
            spawnHazardFx(event.origin, BossFx::PoisonPool, 2.4f, 3.0f, fireOrange);
        }
        break;

    case EliteMove::WerewolfClawLane:
        if (event.kind == EliteEventKind::Execute)
        {
            // One claw slash lane: its own 0.4s warning, then a fast strike.
            // Three lanes fan with walkable gaps (the elite spread helper).
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Lane;
                zone->status = EliteStatusPayload::None;
                zone->start = event.origin;
                zone->end = Vector2{ event.origin.x + event.direction.x * 430.f,
                                     event.origin.y + event.direction.y * 430.f };
                zone->radius = 55.f;
                zone->telegraphRemaining = 0.4f;
                zone->telegraphDuration = 0.4f;
                zone->activeRemaining = 0.22f;
                zone->damage = 1.f;
            }
            spawnLaneFx(event.origin, event.direction, 430.f,
                        BossFx::ClawSwipe, 3.6f, Color{ 255, 90, 80, 255 });
            impactFeedback(5.f, 0.12f);
        }
        break;

    case EliteMove::VenomfangPounce:
        if (event.kind == EliteEventKind::TrailPatch)
        {
            // Poison trail patch: no direct damage — stepping in it poisons.
            if (EliteAttackZone* zone = AcquireEliteZone())
            {
                zone->owner = event.archetype; zone->move = event.move;
                zone->shape = EliteZoneShape::Disc;
                zone->status = EliteStatusPayload::Poison;
                zone->start = event.origin;
                zone->radius = 55.f;
                zone->activeRemaining = Elite::kVenomfangTrailLifetime;
                zone->tickInterval = 0.7f;
                zone->tickRemaining = 0.2f;
                zone->damage = 0.f;
            }
            spawnHazardFx(event.origin, BossFx::PoisonPool, 1.8f,
                          Elite::kVenomfangTrailLifetime, toxicGreen);
        }
        else if (event.kind == EliteEventKind::Execute)
        {
            impactFeedback(4.f, 0.10f);   // pounce launch — bite damage is body contact
        }
        break;

    default:
        break;
    }
}

void CombatDirector::UpdateEliteZones(const EnemyRuntimeContext& ctx, float dt) const
{
    if (ctx.player == nullptr)
        return;

    const Rectangle playerRect = ctx.player->GetCollisionRec();
    const Vector2 playerCentre{ playerRect.x + playerRect.width * 0.5f,
                                playerRect.y + playerRect.height * 0.5f };
    const float playerRadius = std::min(playerRect.width, playerRect.height) * 0.5f;

    auto zoneOverlapsPlayer = [&](const EliteAttackZone& zone)
    {
        if (zone.shape == EliteZoneShape::Lane)
            return DistancePointToSegment(playerCentre, zone.start, zone.end)
                   <= zone.radius + playerRadius;
        return Vector2Distance(playerCentre, zone.start) <= zone.radius + playerRadius;
    };

    auto applyZoneHit = [&](EliteAttackZone& zone)
    {
        // Respect the player's protection windows for BOTH damage and status —
        // a dash through a fissure must actually work.
        if (ctx.player->HasActiveIFrames() || !ctx.player->IsAlive())
            return;

        const int damage = (int)std::lround(zone.damage);
        if (damage > 0)
            ctx.player->TakeDamage(damage, zone.start);

        switch (zone.status)
        {
        case EliteStatusPayload::Burn:
            ctx.player->ApplyBurnTicks(0.5f, 2, 0.4f, zone.start);
            break;
        case EliteStatusPayload::Chill:
            ctx.player->ApplyChill(2.0f, 0.6f);
            break;
        case EliteStatusPayload::Shock:
            ctx.player->ApplyChill(0.8f, 0.5f);   // brief stagger-slow
            break;
        case EliteStatusPayload::Poison:
            ctx.player->ApplyPoison(Balance::Elite::kPoisonDuration,
                                    Balance::Elite::kPoisonTickInterval,
                                    Balance::Elite::kPoisonDamagePerTick);
            break;
        case EliteStatusPayload::Knockback:
        {
            Vector2 away = Vector2Subtract(ctx.player->GetWorldPos(), zone.start);
            ctx.player->ApplyKnockbackImpulse(away, 3200.f);
            break;
        }
        default:
            break;
        }
    };

    for (EliteAttackZone& zone : _eliteZones)
    {
        if (!zone.active)
            continue;

        // No damage before the warned moment.
        if (zone.telegraphRemaining > 0.f)
        {
            zone.telegraphRemaining -= dt;
            continue;
        }

        zone.activeRemaining -= dt;
        if (zone.activeRemaining <= 0.f)
        {
            zone.active = false;
            continue;
        }

        if (zone.tickInterval > 0.f)
        {
            // Lingering patch: damages on its authored cadence while overlapped.
            zone.tickRemaining -= dt;
            if (zone.tickRemaining <= 0.f)
            {
                zone.tickRemaining = zone.tickInterval;
                if (zoneOverlapsPlayer(zone))
                    applyZoneHit(zone);
            }
        }
        else if (!zone.hitPlayer && zoneOverlapsPlayer(zone))
        {
            // One-shot zone: hits at most once.
            zone.hitPlayer = true;
            applyZoneHit(zone);
        }
    }
}

void CombatDirector::DrawEliteWorld(const std::vector<std::unique_ptr<Enemy>>& enemies) const
{
    // Per-elite warning geometry (charge lanes, fissure outlines, landing
    // markers). Drawn in world space; the caller supplies the camera transform.
    for (const auto& enemy : enemies)
    {
        if (enemy->IsActive() && enemy->IsAlive() && !enemy->IsDying())
            enemy->DrawEliteTelegraph();
    }

    // Guard Links: visible beams from every living guard to the linked elite,
    // so "kill the adds to break the 60% ward" is taught on sight.
    for (const auto& enemy : enemies)
    {
        if (!enemy->IsActive() || !enemy->IsAlive() || enemy->IsDying() ||
            !enemy->IsEliteMiniboss() || !enemy->IsEliteGuardLinked())
            continue;
        const Vector2 elitePosition = enemy->GetWorldPos();
        const float pulse = 0.35f + 0.20f * sinf((float)GetTime() * 6.f);
        for (const auto& guard : enemies)
        {
            if (guard.get() == enemy.get()) continue;
            if (!guard->IsActive() || !guard->IsAlive() || guard->IsDying())
                continue;
            const Vector2 guardPosition = guard->GetWorldPos();
            DrawLineEx(guardPosition, elitePosition, 4.f,
                       Fade(Color{ 180, 100, 255, 255 }, pulse));
            DrawLineEx(guardPosition, elitePosition, 1.5f,
                       Fade(Color{ 230, 200, 255, 255 }, pulse + 0.15f));
        }
    }

    // Zone warnings and readability outlines. A zone still in its telegraph
    // window draws a REAL warning (fill + outline) — no zone may damage from
    // ground the player was never shown. Active one-shot zones keep a thin
    // outline so the hit area stays readable under the sprite FX.
    for (const EliteAttackZone& zone : _eliteZones)
    {
        if (!zone.active)
            continue;

        if (zone.telegraphRemaining > 0.f)
        {
            // Arm-up ramp: the warning brightens as the strike approaches, so
            // "leave NOW" reads at a glance without watching a timer.
            const float armProgress = (zone.telegraphDuration > 0.01f)
                ? 1.f - zone.telegraphRemaining / zone.telegraphDuration : 1.f;
            const Color warnFill = Fade(Color{ 255, 120, 60, 255 }, 0.10f + 0.18f * armProgress);
            const Color warnEdge = Fade(Color{ 255, 170, 90, 255 }, 0.35f + 0.45f * armProgress);
            if (zone.shape == EliteZoneShape::Lane)
            {
                Vector2 laneDelta = Vector2Subtract(zone.end, zone.start);
                const float laneLength = Vector2Length(laneDelta);
                if (laneLength > 0.01f)
                {
                    Vector2 laneDirection = Vector2Scale(laneDelta, 1.f / laneLength);
                    Vector2 side{ -laneDirection.y, laneDirection.x };
                    Vector2 cornerA = Vector2Add(zone.start, Vector2Scale(side,  zone.radius));
                    Vector2 cornerB = Vector2Add(zone.start, Vector2Scale(side, -zone.radius));
                    Vector2 cornerC = Vector2Add(zone.end,   Vector2Scale(side, -zone.radius));
                    Vector2 cornerD = Vector2Add(zone.end,   Vector2Scale(side,  zone.radius));
                    DrawTriangle(cornerA, cornerB, cornerC, warnFill);
                    DrawTriangle(cornerA, cornerC, cornerD, warnFill);
                    DrawLineEx(cornerA, cornerD, 2.f, warnEdge);
                    DrawLineEx(cornerB, cornerC, 2.f, warnEdge);
                }
            }
            else
            {
                DrawCircleV(zone.start, zone.radius, warnFill);
                DrawCircleLines((int)zone.start.x, (int)zone.start.y, zone.radius, warnEdge);
            }
            continue;
        }

        if (zone.tickInterval > 0.f)
            continue;   // lingering patches carry their own decal art
        const Color outline = Fade(WHITE, 0.30f);
        if (zone.shape == EliteZoneShape::Lane)
            DrawLineEx(zone.start, zone.end, 3.f, outline);
        else
            DrawCircleLines((int)zone.start.x, (int)zone.start.y, zone.radius, outline);
    }
}

void CombatDirector::ClearEliteRuntime()
{
    for (EliteAttackZone& zone : _eliteZones)
        zone.active = false;
    _eliteImpactFeedbackThisFrame = false;
    // Dropped-zone telemetry intentionally persists — it is a health metric.
}

int CombatDirector::GetActiveEliteZoneCount() const
{
    int activeCount = 0;
    for (const EliteAttackZone& zone : _eliteZones)
        if (zone.active)
            activeCount++;
    return activeCount;
}

void CombatDirector::UpdateEnemyDeaths(const EnemyDeathContext& ctx, float dt) const
{
    // Spawning during iteration would invalidate the loop when ctx.enemies
    // reallocates, so slime splits / death clouds are collected here and
    // executed after the loop.
    std::vector<Vector2> pendingSmallSlimeSpawns;
    std::vector<Vector2> pendingPoisonClouds;

    // Relic on-kill effects iterate the enemy list, so defer them until after
    // this loop finishes (same reason as the slime splits).
    struct KillEvent { Vector2 pos; bool burning, frozen, charged, eliteOrBoss; };
    std::vector<KillEvent> pendingKills;

    for (auto& enemy : *ctx.enemies)
    {
        if (!enemy->IsActive())
            continue;

        Vector2 dropPos = enemy->GetWorldPos();
        if (enemy->UpdateDeath(dt))
        {
            if (enemy.get() == ctx.bossCyclopsSupport->enemy && IsBossFightActive(*ctx.enemies))
                ctx.bossCyclopsSupport->respawnTimer = kBossSupportRespawnDelay;
            if (enemy.get() == ctx.bossOgreSupport->enemy && IsBossFightActive(*ctx.enemies))
                ctx.bossOgreSupport->respawnTimer = kBossSupportRespawnDelay;

            bool isBoss = enemy->IsBoss();
            bool isOgre = (dynamic_cast<Ogre*>(enemy.get()) != nullptr);

            // Splitting slime: a big slime bursts into two small ones where it
            // died. The engine provides the spawn callback so the new slimes go
            // through the normal pooled-spawn path.
            if (SlimeEnemy* slime = enemy->AsSlime())
            {
                if (slime->IsBig() && ctx.spawnSmallSlime)
                {
                    pendingSmallSlimeSpawns.push_back(Vector2{ dropPos.x - 42.f, dropPos.y + 16.f });
                    pendingSmallSlimeSpawns.push_back(Vector2{ dropPos.x + 42.f, dropPos.y + 16.f });
                }
            }

            // Sporeling: bursts into a lingering poison cloud where it fell.
            if (enemy->AsSporeling() != nullptr && ctx.spawnPoisonCloud)
                pendingPoisonClouds.push_back(dropPos);

            if (isBoss)
            {
                if (ctx.awardKillExp)
                    *ctx.pendingExp += 10.f * ctx.wave;
                (*ctx.bossesDefeated)++;
                if (*ctx.bossesDefeated >= 2)
                    *ctx.demoCompleted = true;
            }
            else if (ctx.awardKillExp && !IsBossFightActive(*ctx.enemies))
            {
                *ctx.pendingExp += (float)enemy->GetExpValue();
            }

            // Snapshot status for relic on-kill synergies before the body clears.
            if (ctx.onEnemyKilled)
                pendingKills.push_back(KillEvent{
                    dropPos, enemy->IsBurning(), enemy->IsFrozen(), enemy->IsCharged(),
                    isBoss || enemy->IsEliteMiniboss() });

            (*ctx.enemiesKilled)++;
            ctx.spawnEnemyDrop(dropPos, isOgre, isBoss);
            enemy->SetActive(false);
            enemy->Teleport(Vector2{ -5000.f, -5000.f });
        }
    }

    // Safe to grow / re-damage the enemy vector now that iteration is finished.
    for (const KillEvent& kill : pendingKills)
        ctx.onEnemyKilled(kill.pos, kill.burning, kill.frozen, kill.charged, kill.eliteOrBoss);
    for (const Vector2& spawnPos : pendingSmallSlimeSpawns)
        ctx.spawnSmallSlime(spawnPos);
    for (const Vector2& cloudPos : pendingPoisonClouds)
        ctx.spawnPoisonCloud(cloudPos);
}

void CombatDirector::SpawnBossSupportAdds(const BossSupportContext& ctx) const
{
    Vector2 cyclopsPos{};
    if (ctx.tryGetFarSpawnPosition(cyclopsPos, kBossSupportMinPlayerDistance))
        ctx.bossCyclopsSupport->enemy = ctx.spawnCyclops(cyclopsPos);
    ctx.bossCyclopsSupport->respawnTimer = 0.f;

    ctx.bossOgreSupport->enemy = nullptr;
    ctx.bossOgreSupport->respawnTimer = kBossOgreInitialDelay;
}

void CombatDirector::ClearBossSupportAdds(BossSupportState& cyclopsSupport, BossSupportState& ogreSupport) const
{
    if (cyclopsSupport.enemy != nullptr)
    {
        cyclopsSupport.enemy->SetActive(false);
        cyclopsSupport.enemy->Teleport(Vector2{ -5000.f, -5000.f });
    }

    if (ogreSupport.enemy != nullptr)
    {
        ogreSupport.enemy->SetActive(false);
        ogreSupport.enemy->Teleport(Vector2{ -5000.f, -5000.f });
    }

    cyclopsSupport = {};
    ogreSupport = {};
}

void CombatDirector::UpdateBossSupportRespawns(const BossSupportContext& ctx, float dt) const
{
    if (!ctx.isBossFightActive())
    {
        ClearBossSupportAdds(*ctx.bossCyclopsSupport, *ctx.bossOgreSupport);
        return;
    }

    if (ctx.bossCyclopsSupport->enemy != nullptr &&
        !ctx.bossCyclopsSupport->enemy->IsActive() &&
        ctx.bossCyclopsSupport->respawnTimer > 0.f)
    {
        ctx.bossCyclopsSupport->respawnTimer -= dt;
        if (ctx.bossCyclopsSupport->respawnTimer <= 0.f)
        {
            Vector2 spawnPos{};
            if (ctx.tryGetFarSpawnPosition(spawnPos, kBossSupportMinPlayerDistance))
                ctx.bossCyclopsSupport->enemy = ctx.spawnCyclops(spawnPos);
            ctx.bossCyclopsSupport->respawnTimer = 0.f;
        }
    }

    bool ogrePendingSpawn = (ctx.bossOgreSupport->enemy == nullptr && ctx.bossOgreSupport->respawnTimer > 0.f);
    bool ogrePendingRespawn = (ctx.bossOgreSupport->enemy != nullptr &&
                               !ctx.bossOgreSupport->enemy->IsActive() &&
                               ctx.bossOgreSupport->respawnTimer > 0.f);
    if (ogrePendingSpawn || ogrePendingRespawn)
    {
        ctx.bossOgreSupport->respawnTimer -= dt;
        if (ctx.bossOgreSupport->respawnTimer <= 0.f)
        {
            Vector2 spawnPos{};
            if (ctx.tryGetFarSpawnPosition(spawnPos, kBossSupportMinPlayerDistance))
                ctx.bossOgreSupport->enemy = ctx.spawnOgre(spawnPos);
            ctx.bossOgreSupport->respawnTimer = 0.f;
        }
    }
}
