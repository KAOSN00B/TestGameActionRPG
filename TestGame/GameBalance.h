#pragma once

// =============================================================================
// GameBalance.h — THE single source of truth for Mystic Onslaught's tuning.
//
// Target feel (chosen 2026-07-06): "Moderate / Hades-like" — the base run has
// real teeth and a first clear is an achievement, but it's fair. All *extra*
// difficulty lives in the Ascension ladder (see Ascension.h), NOT here.
//
// RULE: gameplay code must READ these constants, never hardcode the number
// beside them. If you find yourself typing a balance literal in a .cpp, add it
// here instead. Everything is constexpr, so it inlines at zero runtime cost.
//
// Consumed by: Enemy.cpp (grunt base + power curve), every boss's SetWaveScale
// (HP), Engine.cpp (power-level rate, drop chance, spawn caps), ShopManager.cpp
// (prices). Included via Enemy.h and Engine.h so it reaches all of them.
// =============================================================================

// ── Player base stats ────────────────────────────────────────────────────────
// Per-class base stats live in PlayerClass.cpp (kClassTable) — classes differ,
// so a single shared constant here would lie. A Balance::Player block used to
// sit here but was never read by any code; it was removed in the balance truth
// pass so nobody tunes dead numbers.

// ── Player levelling ─────────────────────────────────────────────────────────
namespace Balance::Levelling
{
    // Level 1 is the run start, so level 9 represents eight Power Choices.
    // The full-run target is 6-8 choices; room-clear XP below makes enemy count
    // irrelevant to that pacing.
    inline constexpr int kMaxLevel         = 9;
    inline constexpr int kExpToNextBase    = 20;  // level 1 -> 2
    inline constexpr int kExpThresholdStep = 8;   // 20, 28, 36, 44, ...

    inline constexpr int ExpToNextForLevel(int currentLevel)
    {
        return kExpToNextBase + (currentLevel - 1) * kExpThresholdStep;
    }

    // Dungeon-run XP is awarded once when a combat room is cleared. These
    // values produce roughly 90 XP per six-room act before optional contracts.
    inline constexpr int kStandardRoomClearExp = 12;
    inline constexpr int kEliteRoomClearExp    = 18;
    inline constexpr int kBossRoomClearExp     = 30;
}

// ── Ability damage ───────────────────────────────────────────────────────────
namespace Balance::AbilityDamage
{
    inline constexpr float kSpreadBase     = 1.0f;
    inline constexpr float kSpreadBurnBase = 1.0f;
    inline constexpr float kBoltBase       = 3.0f;   // 3x spread per shot
    inline constexpr float kBoltBurnBase   = 2.0f;
    inline constexpr float kUpgradeBonus   = 0.25f;  // _abilityDamageMultiplier per upgrade

    // Class-ability damage per ability level above 1 (Engine dmgVal). Level 3
    // = 1.5x base. The pre-rework formula was a raw x2/x3 level multiplier —
    // the single biggest damage snowball in the run.
    inline constexpr float kClassLevelStep = 0.25f;
}

// ── Enemy power-level curve ──────────────────────────────────────────────────
// A single global multiplier that advances as the run deepens. Every enemy and
// boss shares it (applied in Enemy::ApplyEnemyPowerLevel), so the whole cast
// toughens together — the roguelite ramp. Per-level growth is deliberately
// gentle so player upgrades stay ahead of the curve on a normal run.
namespace Balance::Curve
{
    inline constexpr int   kRoomsPerPowerLevel = 5;      // power up every N rooms entered
    // Balance rework: HP ramp softened 0.16 -> 0.12 because the player no
    // longer auto-gains +0.4 attack per level — deep enemies should press
    // harder (cadence below), not just take longer to kill.
    inline constexpr float kHealthPerLevel     = 0.12f;  // +12% max HP per level above 1
    inline constexpr float kDamagePerLevel     = 0.08f;  // +8%  damage
    inline constexpr float kSpeedPerLevel      = 0.04f;  // +4%  move speed

    // Attack cadence by depth — the new pressure lever. Enemies attack more
    // often as the run deepens (delay shrinks 5%/level), floored so no enemy
    // ever attacks faster than 65% of its authored gap.
    inline constexpr float kAttackDelayPerLevel  = 0.05f;
    inline constexpr float kMinAttackDelayFactor = 0.65f;
}

// ── Grunt (basic enemy) base profile at power level 1 ────────────────────────
namespace Balance::Grunt
{
    inline constexpr int   kBaseExpValue    = 3;
    inline constexpr float kBaseHealth      = 4.f;
    inline constexpr float kBaseAttack      = 1.f;
    inline constexpr float kBaseSpeed       = 185.f;
    inline constexpr float kBaseAttackDelay = 1.5f;
}

// ── Elite minibosses (Ogre / Cyclops) base HP at power level 1 ───────────────
namespace Balance::Elite
{
    inline constexpr float kOgreHealth    = 10.f;
    inline constexpr float kCyclopsHealth = 7.f;

    // ── Elite signature kits (shared) ────────────────────────────────────────
    // Health stays on the existing 2.5x elite conversion — fight length comes
    // from mechanics, not inflated HP. These knobs shape the mechanics.
    inline constexpr float kPhaseThreshold       = 0.50f;  // one-time escalation at 50% max HP
    inline constexpr float kGuardLinksDamageTaken = 0.40f; // linked elite takes 40% (visible reduction, never immune)
    inline constexpr int   kSignatureZoneCapacity = 64;    // bounded attack-zone pool in CombatDirector

    // Ogre — The Battering Ram
    inline constexpr float kOgreWallStunMin      = 0.70f;  // wall punish window never falls below this
    inline constexpr float kOgreRetargetPause    = 0.55f;  // visible pause between phase-two charges

    // Infernal — The Living Furnace
    inline constexpr float kInfernalMarchTelegraph = 0.55f;
    inline constexpr float kInfernalMarchActive    = 1.60f;  // committed line walk
    inline constexpr float kInfernalMarchRecovery  = 0.45f;
    inline constexpr float kInfernalPatchSpacing   = 95.f;   // gap between flame patches (crossable)
    inline constexpr float kInfernalPatchLifetime  = 2.4f;   // short-lived — room never stays divided
    inline constexpr int   kInfernalPatchCap       = 6;
    inline constexpr float kInfernalBurstTelegraph = 0.70f;
    inline constexpr float kInfernalBurstActive    = 0.35f;
    inline constexpr float kInfernalBurstRecovery  = 0.90f;
    inline constexpr float kInfernalBurstRecoveryP2 = 1.35f; // longer exhausted window after OVERHEATED
    inline constexpr float kInfernalFissureLength  = 420.f;
    inline constexpr float kInfernalFissureWidth   = 58.f;   // half-width; gaps between the 3 fissures stay walkable
    inline constexpr float kInfernalSignatureCooldown = 5.0f;
    inline constexpr float kInfernalPhaseMeleeMult = 0.85f;  // slightly faster melee cadence in phase two

    // Bonechill — The Frozen Wall
    inline constexpr float kBonechillFrontDamageTaken = 0.55f; // frontal frost armour: 45% reduction, never zero
    inline constexpr float kBonechillSlamTelegraph = 0.80f;
    inline constexpr float kBonechillSlamActive    = 0.30f;
    inline constexpr float kBonechillSlamRecovery  = 0.85f;
    inline constexpr float kBonechillLaneLength    = 460.f;
    inline constexpr float kBonechillLaneWidth     = 52.f;
    inline constexpr float kBonechillSignatureCooldown = 5.5f;
    inline constexpr float kBonechillPhaseSpeed    = 1.25f;  // ARMOUR SHATTERED movement bonus
    inline constexpr float kBonechillPhaseCooldownMult = 0.80f;

    // Stormclub — The Thunder Breaker
    inline constexpr float kStormclubLeapTelegraph = 0.70f;
    inline constexpr float kStormclubLeapSpeed     = 900.f;  // continuous travel — never a teleport
    inline constexpr float kStormclubLeapRange     = 520.f;
    inline constexpr float kStormclubLeapRangeP2   = 360.f;  // Tempest leaps are shorter
    inline constexpr float kStormclubImpactRadius  = 120.f;
    inline constexpr float kStormclubBranchLength  = 380.f;
    inline constexpr float kStormclubBranchWidth   = 46.f;
    inline constexpr float kStormclubHitRecovery   = 0.70f;
    inline constexpr float kStormclubMissRecovery  = 1.30f;  // embedded-club punish window
    inline constexpr float kStormclubSignatureCooldown = 5.0f;

    // Venomfang — The Ambush Predator
    inline constexpr float kVenomfangPounceTelegraph = 0.60f;
    inline constexpr float kVenomfangPounceSpeed     = 980.f;
    inline constexpr float kVenomfangPounceRange     = 460.f;
    inline constexpr float kVenomfangPounceRecovery  = 0.80f;
    inline constexpr float kVenomfangSignatureCooldown = 4.5f;
    inline constexpr int   kPredatorMarkCap          = 3;
    inline constexpr float kPredatorMarkDuration     = 6.f;   // expires without another bite
    inline constexpr float kVenomfangTrailLifetime   = 2.0f;
    inline constexpr float kVenomfangPhaseCircleSpeed = 1.30f; // BLOOD SCENT circling bonus
    inline constexpr float kVenomfangSecondPounceDelay = 0.85f;

    // Player poison (applied by Venomfang bites/trail; distinct from burn)
    inline constexpr int   kPoisonStackCap   = 3;
    inline constexpr float kPoisonDuration   = 4.0f;
    inline constexpr float kPoisonTickInterval = 0.8f;
    inline constexpr float kPoisonDamagePerTick = 0.5f;   // per stack, fractional accumulates
}

// ── Bosses — unified role model ──────────────────────────────────────────────
// Every boss HP derives from ONE budget x a role multiplier, so the cast forms a
// smooth spread instead of ad-hoc numbers. Bosses are then power-scaled by the
// curve above at the depth they actually spawn, so a tanky boss met late is
// genuinely tanky. To make ALL bosses tankier/squishier, change kBaseHealth.
//   First boss (Caverns) is a gentle intro; the final boss (Demon's Insides) is
//   the wall. The eight middle bosses can appear at any zone 1-4, so they share
//   the mid budget and differ only by combat role.
namespace Balance::Boss
{
    inline constexpr float kBaseHealth = 46.f;   // "standard mid-boss" budget

    // Role multipliers.
    inline constexpr float kRoleFirst    = 0.55f;  // tutorial first boss
    inline constexpr float kRoleGlass    = 0.80f;  // fast / fragile
    inline constexpr float kRoleFast     = 0.85f;
    inline constexpr float kRoleCaster   = 0.95f;
    inline constexpr float kRoleStandard = 1.00f;
    inline constexpr float kRoleHeavy    = 1.15f;  // hard-hitting bruiser
    inline constexpr float kRoleTank     = 1.25f;
    inline constexpr float kRoleFinal    = 1.55f;  // final boss

    // Per-boss resolved HP (base x role). Bosses read THESE in SetWaveScale.
    inline constexpr float kMolarbeastHealth  = kBaseHealth * kRoleFirst;    // ~25
    inline constexpr float kChompBugHealth    = kBaseHealth * kRoleGlass;    // ~37
    inline constexpr float kWerewolfHealth    = kBaseHealth * kRoleFast;     // ~39
    inline constexpr float kOsirisHealth      = kBaseHealth * kRoleCaster;   // ~44
    inline constexpr float kPumpkinJackHealth = kBaseHealth * kRoleStandard; // ~46
    inline constexpr float kToxicVerminHealth = kBaseHealth * kRoleStandard; // ~46
    inline constexpr float kAncientBearHealth = kBaseHealth * kRoleHeavy;    // ~53
    inline constexpr float kMinotaurHealth    = kBaseHealth * kRoleTank;     // ~58
    inline constexpr float kTitanGuardHealth  = kBaseHealth * kRoleTank;     // ~58
    inline constexpr float kAbyssSlimeHealth  = kBaseHealth * kRoleFinal;    // ~71
}

// ── Sustain ──────────────────────────────────────────────────────────────────
// The roguelite pressure model: HP is a resource that carries BETWEEN rooms.
// Entering a room no longer heals at all; recovery comes only from pickups,
// Zeph, abilities/relics — and all repeatable sources are capped per room so
// damage growth can't buy unlimited healing.
namespace Balance::Sustain
{
    inline constexpr float kBossClearHealFraction = 0.40f;  // partial breath after a boss, not a reset
    inline constexpr int   kMaxHealDropsPerRoom   = 2;      // chance + timed heal pickups combined
    inline constexpr int   kLifestealCapPerRoom   = 6;      // HP returned by lifesteal per room
    inline constexpr float kManaRegenPerSecond    = 0.25f;  // base regen (was 0.2) — compensates the
                                                            // removed +5 mana per level
}

// ── Economy ──────────────────────────────────────────────────────────────────
namespace Balance::Economy
{
    inline constexpr int   kHealDropChancePercent = 8;     // bonus heal-drop base chance
    inline constexpr int   kRerollBaseCost        = 45;
    inline constexpr int   kRerollStep            = 35;    // added per reroll
    inline constexpr int   kReserveStockCost      = 15;    // lock one card through rerolls
    inline constexpr float kActPriceInflation     = 0.25f; // +25% shop prices per act
}

// ── Room / spawn structure ───────────────────────────────────────────────────
namespace Balance::Rooms
{
    inline constexpr int kRoomsPerAct      = 6;   // 5 normal + 1 boss
    inline constexpr int kMaxActiveEnemies = 16;
}

// ── Pickup spawn timing ──────────────────────────────────────────────────────
namespace Balance::Pickups
{
    inline constexpr float kDefaultInterval = 60.f;  // seconds between timed drops
    inline constexpr float kBossInterval    = 2.f;
}

// ── Boss support adds ────────────────────────────────────────────────────────
namespace Balance::BossSupport
{
    inline constexpr float kRespawnDelay      = 20.f;
    inline constexpr float kMinPlayerDistance = 520.f;
}

// ── Game feel / juice ────────────────────────────────────────────────────────
namespace Balance::Feel
{
    // Damage is single-digit internally; floating combat numbers are multiplied
    // by this for display only (balance untouched). Bump for bigger, juicier
    // numbers. 25 turns an 8-damage hit into "200".
    inline constexpr int kDamageNumberScale = 25;
}

// ── Enemy facing & directional combat ────────────────────────────────────────
// Facing commitment stops enemies flipping toward the player every frame, which
// made shield fronts unbeatable and backstabs impossible. Front/rear checks use
// a dot-product cone against the enemy's horizontal facing vector (sprites face
// left/right), not a raw x-comparison.
namespace Balance::Facing
{
    // cos-space cone edges: dot(facing, toAttacker) >= kFrontConeDot counts as
    // "in front" (0.25 ≈ a 150° shielded arc); dot <= -kRearConeDot counts as
    // "behind" (a matching 150° rear arc). The sides belong to neither.
    inline constexpr float kFrontConeDot = 0.25f;
    inline constexpr float kRearConeDot  = 0.25f;
    // Minimum seconds between voluntary facing flips while walking.
    inline constexpr float kTurnCommitInterval      = 0.22f;
    // Heavy shield units pivot much slower — circling them is a real tactic.
    inline constexpr float kHeavyTurnCommitInterval = 0.55f;
    // Extra facing lock after an attack animation ends (recovery window the
    // player can exploit by repositioning).
    inline constexpr float kAttackRecoveryFacingLock = 0.35f;
    // Rogue Backstab: damage multiplier applied ONLY when the rear check passes.
    inline constexpr float kBackstabRearMult = 2.2f;
}

// ── Room pressure budget ─────────────────────────────────────────────────────
// How much simultaneous danger a combat room may field. Every threat costs
// pressure (grunts 1, specialists 2, Warchief 3, hazards per type); the room's
// composition is clamped to the cap for its tier, and bodies beyond the opening
// cap arrive as reinforcement waves instead of an unreadable wall of enemies.
// Tier index everywhere: 0 = early rooms, 1 = mid, 2 = late (cleared-room count).
namespace Balance::Pressure
{
    // Population controls spectacle; danger controls how many costly roles may
    // threaten the player simultaneously. Keeping these separate lets a room
    // contain a large fragile crowd without also becoming a wall of tanks and
    // ranged fire.
    inline constexpr int   kPopulationMin[3]    = { 5, 7, 9 };
    inline constexpr int   kPopulationMax[3]    = { 7, 10, 13 };
    inline constexpr int   kOpeningBodyCap[3]   = { 4, 6, 8 };
    inline constexpr int   kDangerCap[3]        = { 7, 10, 13 };
    inline constexpr int   kSwarmPeakMin        = 14;
    inline constexpr int   kSwarmPeakMax        = 18;

    inline constexpr int   kRangedCap[3]        = { 1, 2, 3 };
    inline constexpr int   kTankCap[3]          = { 1, 1, 2 };
    inline constexpr int   kSupportCap[3]       = { 0, 1, 1 };
    inline constexpr int   kAssassinCap[3]      = { 1, 2, 2 };
    inline constexpr int   kZonerCap[3]         = { 1, 2, 2 };
    inline constexpr int   kExpensiveUnitCap[3] = { 1, 2, 3 };
    inline constexpr int   kEnemyProjectileCap[3]       = { 12, 16, 20 };
    inline constexpr int   kEnvironmentalProjectileCap[3] = { 8, 10, 12 };

    // Legacy names remain temporarily for non-standard encounters while the
    // standard-room planner uses the clearer population/danger vocabulary.
    inline constexpr int   kRoomPressureCap[3]  = { 7, 10, 13 };
    // Max enemies alive when the fight OPENS; the surplus becomes waves.
    inline constexpr int   kOpeningActiveCap[3] = { 4, 6, 8 };
    // Standard-room body counts rolled before the pressure clamp.
    inline constexpr int   kMinBasics[3] = { 5, 7, 9 };
    inline constexpr int   kMaxBasics[3] = { 7, 10, 13 };
    // A reinforcement wave releases when live enemies drop to this count,
    // or on the interval timer — whichever comes first.
    inline constexpr int   kReinforceRefillActive = 4;
    inline constexpr float kReinforceInterval     = 6.f;
}

namespace Balance::Squad
{
    // ── Crowd behaviour: threat tiers, formation coherence, role tactics ─────
    // The CombatDirector reads the battlefield once per frame (player health,
    // ally count, who is attacking) and hands every enemy a SquadDirective.
    // Enemies use it to fight like a pack instead of a queue of solo chargers.

    // Aggression (threat assessment). 1.0 = neutral. Enemies smell blood when
    // the player is hurt and grow cautious when outnumbered themselves.
    inline constexpr float kAggressionMin            = 0.70f;
    inline constexpr float kAggressionMax            = 1.30f;
    inline constexpr float kPlayerLowHealthFrac      = 0.35f;  // below this → frenzy bonus
    inline constexpr float kPlayerHealthyFrac        = 0.80f;  // above this → respect
    inline constexpr float kLowHealthAggroBonus      = 0.25f;
    inline constexpr float kHealthyAggroPenalty      = 0.10f;
    inline constexpr int   kPackCourageCount         = 5;      // this many allies → bolder
    inline constexpr float kPackCourageBonus         = 0.15f;
    inline constexpr int   kLonelyCount              = 2;      // this few allies → warier
    inline constexpr float kLonelyPenalty            = 0.15f;

    // What aggression does. Frenzied enemies move faster and one extra attacker
    // may commit; wary enemies hold a standoff ring while an ally has the fight.
    inline constexpr float kFrenzyThreshold          = 1.15f;  // above → frenzy effects
    inline constexpr float kWaryThreshold            = 0.85f;  // below → standoff effects
    inline constexpr float kFrenzySpeedMult          = 1.12f;
    inline constexpr float kWarySpeedMult            = 0.92f;
    inline constexpr int   kFrenzyExtraAttackers     = 1;      // added to the 2-attacker slot cap
    inline constexpr float kStandoffRadius           = 330.f;  // wary grunts circle here
    inline constexpr float kEngagedNearPlayerRadius  = 260.f;  // "ally is on the player" check

    // Formation coherence. Grunts farther from the player than the tank rally
    // toward a slot behind it and advance as a pack until the fight is joined.
    inline constexpr float kLeaderMaxRange           = 900.f;  // leader too far → ignore
    inline constexpr float kLeaderFollowMargin       = 150.f;  // must be this much farther than leader
    inline constexpr float kLeaderSlotBehind         = 130.f;  // rally point offset behind the tank
    inline constexpr float kLeaderPullWeight         = 0.85f;  // blend strength toward the slot
    inline constexpr float kLeaderBreakoffDist       = 380.f;  // this close to player → free hunt

    // Role tactics.
    inline constexpr float kTankLeadSpeedMult        = 1.08f;  // tank leads the charge
    inline constexpr int   kTankLeadMinPack          = 3;      // needs a pack behind to push
    inline constexpr float kSupportHangBackDist      = 430.f;  // support keeps this far from player
    inline constexpr float kSupportAllyPullWeight    = 0.75f;  // pull toward the ally centroid
    inline constexpr int   kSupportMinAllies         = 2;      // alone → fight normally
    inline constexpr float kAssassinFlankDepth       = 150.f;  // aim point past/behind the player
}

// ── Enemy engagement rhythm (CombatEngagement) ───────────────────────────────
// Deterministic Commit/Support/Reposition policy: only a limited number of
// normal enemies may actively commit to an attack at once. Tier index matches
// Balance::Pressure — 0 = early rooms, 1 = mid, 2 = late (cleared-room count).
namespace Balance::Rhythm
{
    inline constexpr int   kCommittersByTier[3]    = { 2, 3, 3 };
    inline constexpr int   kSwarmExtraCommitters   = 1;   // fragile swarm-profile bonus slot
    inline constexpr float kPostCommitRepositionSeconds = 0.65f; // forced reposition after releasing a slot

    // Reinforcement spawn telegraph (ReinforcementPacing): purple circle warning,
    // then a smoke transition, then the enemy is instantiated. ~0.95s total —
    // see docs/superpowers/specs/2026-08-13-hades-style-enemy-combat-rhythm-design.md,
    // "Spawn Telegraph and VFX".
    inline constexpr float kSpawnCircleSeconds = 0.65f;   // purple circle warning
    inline constexpr float kSpawnSmokeSeconds  = 0.30f;   // smoke burst before spawn

    // Short arrival/orientation delay after a telegraphed reinforcement is
    // instantiated (design: "receives a short arrival/orientation delay
    // before it may move or attack"). Brief on purpose — this is a readability
    // beat, not a stun mechanic.
    inline constexpr float kSpawnArrivalDelaySeconds = 0.20f;
}

namespace Balance::DamageNumbers
{
    inline constexpr int   kVisibleCap      = 32;
    inline constexpr int   kMinFontSize     = 20;
    inline constexpr int   kMaxFontSize     = 42;
    inline constexpr float kDamageReference = 12.f;
    inline constexpr float kRiseSpeed       = 92.f;
    inline constexpr float kLifetime        = 0.90f;
    inline constexpr float kOutline         = 2.f;
    inline constexpr float kMergeWindow     = 0.20f;
}

// ── Room hazards (RoomHazardDirector) ────────────────────────────────────────
// Shared tuning for environmental hazards (Fire Totem / Lava Pool / Fireball
// Torch...). Damage stays modest on purpose — hazards are movement pressure,
// not damage races. Frequencies/placement are consumed in generation (Phase 6).
namespace Balance::Hazards
{
    inline constexpr float kTelegraphSeconds   = 1.1f;   // warning before a hazard arms
    inline constexpr float kDisabledSeconds    = 4.f;    // downtime after the player disables one
    inline constexpr float kFirstActionGrace   = 1.5f;   // no firing right as the player enters
    inline constexpr int   kEnvProjectileCap   = 10;     // hard cap on environmental shots in flight
    inline constexpr int   kHazardTickDamage   = 1;
    inline constexpr float kHazardTickInterval = 0.5f;
    // Fraction of COMBAT rooms that roll a hazard, by tier.
    inline constexpr float kRoomFrequencyByTier[3] = { 0.38f, 0.58f, 0.72f };
    // Safe-placement rules (world px).
    inline constexpr float kMinDistFromDoorway = 240.f;
    inline constexpr float kMinDistFromEntry   = 320.f;
    inline constexpr float kMinDistBetween     = 260.f;
    inline constexpr float kRoomEdgeMargin     = 120.f;

    // Hazard fireballs are deliberately SMALL — a wisp's bolt at 55% size for
    // both the sprite and the hitbox. This is the knob if they still feel big.
    // Damage scales with the run: base below × the enemy damage growth curve
    // (Balance::Curve::kDamagePerLevel per power level), computed per room.
    inline constexpr float kHazardBoltScale   = 0.55f;

    // Fire Totem: aims with a visible telegraph line, then fires one bolt.
    inline constexpr float kTotemFireInterval = 3.2f;   // seconds between shots
    inline constexpr float kTotemAimSeconds   = 0.9f;   // telegraph line duration
    inline constexpr float kTotemAimLock      = 0.35f;  // aim freezes this long before firing (dodge window)
    inline constexpr int   kTotemBoltDamage   = 1;      // BASE damage (scaled by power level)
    inline constexpr float kTotemHealth       = 3.f;    // player hits to destroy
    inline constexpr float kTotemScale        = 5.5f;   // 16x32 sprite -> world size

    // Lava Pool: persistent floor zone, modest tick damage, entry grace.
    inline constexpr float kLavaGraceSeconds  = 0.45f;  // free window when first touched
    inline constexpr float kLavaRadius        = 100.f;  // damage circle inside the sprite
    inline constexpr float kLavaScale         = 3.4f;   // 84x96 sprite -> world size

    // Fireball Torch: wall-anchored lane launcher on a learnable rhythm.
    inline constexpr float kTorchFireInterval = 2.6f;   // lane shot cadence
    inline constexpr float kTorchPrefireFlash = 0.5f;   // bright flash before each shot
    inline constexpr int   kTorchBoltDamage   = 1;
    inline constexpr float kTorchHealth       = 3.f;
    inline constexpr float kTorchScale        = 5.f;    // 16x16 emitter sprite
    // Keep the lane clear of the east/west door band (door sits at mid-height).
    inline constexpr float kTorchDoorBandHalf = 170.f;
}

// ── Timings / miscellaneous ──────────────────────────────────────────────────
namespace Balance::Misc
{
    inline constexpr float kWaveSpawnProtection = 2.f;
    inline constexpr float kBossWarningDuration = 4.f;
}
