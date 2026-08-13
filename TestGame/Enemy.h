#pragma once
#include "BaseCharacter.h"
#include "Character.h"
#include "NavigationGrid.h"
#include "GameBalance.h"
#include "EliteSignature.h"
#include "SfxBank.h"
#include "raymath.h"
#include <vector>
#include <memory>
#include <cstdint>

class Cyclops;
class Ogre;
class Molarbeast;
class SkeletonArcher;
class FlameWisp;
class SlimeEnemy;
class AbyssSlime;
class PumpkinJack;
class Minotaur;
class Sporeling;
class Shieldbearer;
class Phantom;
class BomberImp;
class Warchief;
class LivingBlade;
class Werewolf;
class ChompBug;
class Osiris;
class TitanGuard;
class ToxicVermin;
class AncientBear;
class Infernal;
class Bonechill;
class Stormclub;
class Venomfang;

// ── Encounter roles ───────────────────────────────────────────────────────────
// Lightweight tactical role used by the encounter director to compose fights and
// place spawns (ranged in back, tanks mid, assassins off-angle, etc.). Purely
// metadata — it does not change an enemy's own AI. HeavyRanged is a Ranged that
// also counts as an anchor (Cyclops). Boss is set for bosses so the director can
// exclude them from the add budget.
enum class EnemyRole
{
    Grunt, Charger, Ranged, Tank, Support, Assassin, Zoner, Summoner, HeavyRanged, Boss
};

// ── Engagement intent (CombatEngagement policy) ───────────────────────────────
// What CombatDirector's per-frame engagement policy (see CombatEngagement.h)
// wants this enemy doing right now. Defined here (not in CombatEngagement.h)
// because Enemy stores one by value and CombatEngagement.h already includes
// Enemy.h to reuse EnemyRole — including it back the other way would be a
// circular header dependency. CombatEngagement.h reuses this definition the
// same way it reuses EnemyRole.
enum class EngagementIntent
{
    Commit,
    Support,
    Reposition
};

// ── Engagement latch (stable Commit-slot ownership) ───────────────────────────
// Small, self-contained per-enemy runtime companion to the CombatEngagement
// policy: enforces that an enemy which starts an attack keeps its Commit slot
// through the attack AND a short recovery window afterward, so a fresh
// per-frame BuildEngagementAssignments() re-scan cannot displace it mid-turn.
// Lives in Enemy.h (rather than CombatEngagement.h, which is a pure
// data-in/data-out policy library with no runtime state of its own) for the
// same circular-include reason as EngagementIntent above.
class EngagementLatch
{
public:
    // Attack just started — locks ownership; CanCommit() becomes false.
    void BeginCommit() { _committed = true; _recoveryTimer = 0.f; }

    // Attack just ended — releases the lock but starts a recovery countdown;
    // CanCommit() stays false until `recoverySeconds` worth of Update(dt) have
    // elapsed. Defaults to Balance::Rhythm::kPostCommitRepositionSeconds.
    void EndCommit(float recoverySeconds = Balance::Rhythm::kPostCommitRepositionSeconds)
    {
        _committed = false;
        _recoveryTimer = recoverySeconds;
    }

    // Ticks the recovery countdown. Harmless (no-op-ish) while still mid-attack
    // (BeginCommit called without a matching EndCommit yet) since the timer
    // only represents post-attack recovery, not the attack itself.
    void Update(float dt) { if (_recoveryTimer > 0.f) _recoveryTimer -= dt; }

    // False while locked (mid-attack) or still recovering; true once eligible
    // to request a fresh Commit slot.
    bool CanCommit() const { return !_committed && _recoveryTimer <= 0.f; }

    // Hard reset for pooled reuse — no lingering recovery from a previous life.
    void Reset() { _committed = false; _recoveryTimer = 0.f; }

private:
    bool  _committed     = false;
    float _recoveryTimer = 0.f;
};

// ── Shared engagement positioning (CombatEngagement / Task 3) ────────────────
// Pure, deterministic geometry: given the player, this enemy, the ally
// centroid (CombatDirector's SquadDirective::allyCentroid, built once per
// frame), this enemy's tactical role, the INTENT to compute a target for (see
// note below — a Commit that is actually in its post-attack recovery window
// is passed in as Reposition, not Commit), and a deterministic per-frame
// orbit angle (EngagementAssignment::orbitAngle from BuildEngagementAssignments)
// — returns the point this enemy should move toward this frame.
//
// No raylib textures/assets/RNG, so it is safe to unit test exactly like Task
// 1's BuildEngagementAssignments. Defined inline (header-only, like
// EngagementLatch above) so tests never need to link Enemy.cpp's heavy asset
// dependencies (LoadTexture/PlaySound/CharacterTuningStore/...) — just
// include Enemy.h.
//
// Commit is intentionally a pass-through to the player position: a genuinely
// approaching/attacking Commit keeps the EXISTING pathfinding/attack-entry
// logic in Enemy::HandleMovement untouched (see the Task 3 plan's Step 3 —
// "Commit keeps existing pathfinding and attack entry"). CombatDirector is
// responsible for re-labelling a Commit that is actually recovering
// (IsAttackLockedForEngagement() true but IsCommittedToAttack() false) as
// Reposition before calling this function — see CombatDirector.cpp.
inline Vector2 ComputeEngagementTarget(Vector2 player, Vector2 self, Vector2 allyCentroid,
                                        EnemyRole role, EngagementIntent intent, float orbitAngle)
{
    if (intent == EngagementIntent::Commit)
        return player;

    // Positional flavour only (not a damage/pacing lever), so these live here
    // rather than in GameBalance.h — Task 3's scope is approach/positioning.
    constexpr float kRepositionRadius    = 170.f;          // outside melee/collision range
    constexpr float kSupportAnchorRadius = 90.f;            // support hovers near the ally mass
    constexpr float kScreenBias          = 0.42f;           // 0=at ally, 1=at player; screen sits nearer the ally, biased toward the player
    constexpr float kAssassinMinOffAngle = 50.f * DEG2RAD;  // never lines up with its own current bearing

    if (intent == EngagementIntent::Support && role == EnemyRole::Tank)
    {
        // Shieldbearer-style screen: sit on the line between the ally it is
        // protecting (approximated by the ally centroid — CombatDirector does
        // not currently track a specific per-tank ward) and the player,
        // biased toward the player so its body actually blocks the lane.
        return Vector2Lerp(allyCentroid, player, kScreenBias);
    }

    if (intent == EngagementIntent::Support)
    {
        // Ordinary support (Warchief and similar): hover near the pack
        // rather than the player.
        Vector2 offset{ cosf(orbitAngle) * kSupportAnchorRadius, sinf(orbitAngle) * kSupportAnchorRadius };
        return Vector2Add(allyCentroid, offset);
    }

    // Reposition: hold a lane around the player at a safe radius. Assassins
    // bias off whatever angle they are already standing at (relative to the
    // player) so their next lane reads as an off-angle flank rather than
    // pacing straight toward/away from where they already are.
    float angle = orbitAngle;
    if (role == EnemyRole::Assassin)
    {
        Vector2 selfFromPlayer = Vector2Subtract(self, player);
        if (Vector2LengthSqr(selfFromPlayer) > 0.0001f)
        {
            float selfAngle = atan2f(selfFromPlayer.y, selfFromPlayer.x);
            float delta = angle - selfAngle;
            while (delta > PI)  delta -= 2.f * PI;
            while (delta < -PI) delta += 2.f * PI;

            if (fabsf(delta) < kAssassinMinOffAngle)
            {
                // Too close to directly "in front" of its own current bearing.
                angle = selfAngle + ((delta >= 0.f) ? kAssassinMinOffAngle : -kAssassinMinOffAngle);
            }
            else if (fabsf(fabsf(delta) - PI) < kAssassinMinOffAngle)
            {
                // Too close to directly "behind" its own current bearing.
                float push = (delta >= 0.f) ? -1.f : 1.f;
                angle = selfAngle + PI + push * kAssassinMinOffAngle;
            }
        }
    }

    Vector2 offset{ cosf(angle) * kRepositionRadius, sinf(angle) * kRepositionRadius };
    return Vector2Add(player, offset);
}

// ── Player-made ground hazard (Consecrate, poison pool, corruption pool...) ──
// Engine rebuilds the active list each frame from its ticking damage zones and
// shares it with every enemy, so enemies can steer around danger instead of
// standing in it. Hunter traps are deliberately NOT in the list — a trap that
// enemies avoid would never spring.
struct HazardZone
{
    Vector2 pos;      // world center of the zone
    float   radius;   // damage radius (enemies keep an extra margin beyond it)
};

// ── Squad directive (crowd behaviour) ────────────────────────────────────────
// One shared battlefield read the CombatDirector rebuilds every frame and hands
// to every enemy: how bold the pack should be (threat assessment of the player),
// which tank is anchoring the advance, whether someone already has the player
// engaged, and where the allies are massed. Enemies consult it in HandleMovement
// so the crowd behaves like a pack — rallying behind tanks, flanking an engaged
// player, hanging back to support — instead of a stream of solo chargers.
struct SquadDirective
{
    float   aggression    = 1.f;    // kAggressionMin..kAggressionMax (see Balance::Squad)
    bool    hasLeader     = false;  // an active Tank anchor exists
    Vector2 leaderPos{};            // that tank's world position
    bool    playerEngaged = false;  // an ally is attacking the player right now
    Vector2 allyCentroid{};         // average position of active allies
    int     allyCount     = 0;      // active, alive, non-boss enemies
};

class Enemy : public BaseCharacter
{
public:
    Enemy(Vector2 pos);
    ~Enemy() override;
    static void UnloadSharedResources();

    // Main enemy tick. Engine calls this for every active enemy each frame.
    // Derived enemy types can override it when they need custom combat logic,
    // but they still participate in the same pooled enemy vector.
    virtual void Update(float dt, Vector2 heroWorldPos, Vector2 navigationTarget, bool hasNavigationTarget,
        const std::vector<std::unique_ptr<Enemy>>& enemies, const std::vector<Vector2>& propCenters);
    virtual void SetWaveScale(int wave);
    virtual void ApplyEnemyPowerLevel(int enemyPowerLevel);

    // Extra flat multipliers on top of power level — used by the ascension
    // difficulty tiers. Called after ApplyEnemyPowerLevel in ConfigureSpawnedEnemy.
    void ApplyDifficultyScaling(float healthMult, float damageMult);

    // Shorten the gap between attacks (mult < 1 = more aggressive). Used by the
    // Zeph pressure-debt bargains; a depth-based cadence curve can reuse it.
    void QuickenAttacks(float mult) { if (mult > 0.f) _attackDelay *= mult; }

    // Active player-made damage zones this enemy should steer around. Set every
    // frame by CombatDirector before Update; the pointed-to vector is owned by
    // Engine and stays valid for the whole frame. May be null (no hazards).
    void SetHazardZones(const std::vector<HazardZone>* zones) { _hazardZones = zones; }

    // Squad-level crowd directive (threat tier, formation leader, engagement).
    // Set every frame by CombatDirector before Update; the pointed-to struct is
    // owned by the director and stays valid for the whole frame. May be null.
    void SetSquadDirective(const SquadDirective* directive) { _squadDirective = directive; }

    // True while the attack animation is committed — read by CombatDirector to
    // detect "someone already has the player engaged" for the squad directive.
    bool IsCommittedToAttack() const { return _attacking; }

    // ── Stable engagement commitment (CombatEngagement integration) ──────────
    // Stable per-instance id, assigned once (constructor) and RETAINED across
    // pooling/reuse — ResetForSpawn never touches it. Lets CombatDirector match
    // this frame's EngagementAssignment back to the same enemy next frame even
    // though _worldPos/role/etc. all change.
    uint64_t GetRuntimeId() const { return _runtimeId; }

    // Set once per frame by CombatDirector::UpdateEnemyRuntime (before Update)
    // from BuildEngagementAssignments' output. `target` uses the same Vector2
    // convention as other movement targets on this class (see _approachOffset);
    // consuming it for actual positioning is Task 3's ComputeEngagementTarget.
    void SetEngagementAssignment(EngagementIntent intent, Vector2 target)
    {
        _engagementIntent = intent;
        _engagementTarget = target;
        _hasEngagementAssignment = true;
    }
    EngagementIntent GetEngagementIntent() const { return _engagementIntent; }
    Vector2 GetEngagementTarget() const { return _engagementTarget; }
    // True while this enemy owns its Commit slot through attack + recovery —
    // becomes next frame's EngagementCandidate::locked input. Equivalent to
    // "the latch says not eligible to (re)commit yet".
    bool IsAttackLockedForEngagement() const { return !_engagementLatch.CanCommit(); }
    // Whether SetEngagementAssignment has been called for this enemy on the
    // CURRENT run (reset to false on every pooled respawn). Used by HandleAttack
    // to fall back to the legacy CanTakeAttackSlot scan for any caller that
    // never builds per-frame assignments (see HandleAttack for details).
    bool HasEngagementAssignment() const { return _hasEngagementAssignment; }

    // Fragile swarm-profile flag consumed by EngagementCandidate::swarm (the
    // one-extra-committer bonus slot in a swarm encounter). Plumbing only in
    // this task — defaults false; classifying which enemy types "are" swarm-
    // profile belongs to whichever code composes swarm encounters.
    void SetSwarmProfile(bool swarm) { _swarmProfile = swarm; }
    bool IsSwarmProfile() const { return _swarmProfile; }

    // ── Facing & directional checks (see Balance::Facing) ─────────────────────
    // Sprites face horizontally, so the forward vector is {facingSign, 0}. These
    // helpers express front/rear INTENT — use them instead of comparing x coords.
    float GetFacingSign() const { return (_rightLeft < 0.f) ? -1.f : 1.f; }
    bool  IsPositionInFront(Vector2 position, float coneDot) const;
    bool  IsPositionBehind(Vector2 position, float rearDot) const;
    // False while an attack animation or the post-attack recovery lock is active.
    bool  CanTurnDuringCurrentState() const { return !_attacking && _facingLockTimer <= 0.f; }
    // Commitment-aware voluntary turn: flips toward dx only when allowed and at
    // most once per _turnCommitInterval. Attack windups snapshot facing directly.
    void  FaceToward(float dx);
    float GetFacingLockRemaining() const { return _facingLockTimer; }   // debug HUD

    // Colour-variant tier (0-3) — later world zones spawn recoloured, visibly
    // tougher versions. Default is a no-op; types with variant art override.
    virtual void SetVariantTier(int tier) { (void)tier; }

    // What this enemy is "made of" — drives its hurt/death sound family via the
    // SfxBank so a slime pops, a phantom fades, and armour clangs, instead of
    // everything sharing the player's death cry. Subclasses override.
    virtual CreatureFamily GetCreatureFamily() const { return CreatureFamily::Flesh; }

    // ── Character Animator (dev tool) + tuning interface ─────────────────────
    // GetTuningName() returns nullptr for types without tuning support; a name
    // enables loading charactertuning_<Name>.txt at the end of ResetForSpawn.
    virtual const char* GetTuningName() const { return nullptr; }

    // Human-readable type name for the bestiary. Records once per death.
    const char* GetBestiaryName();
    bool BestiaryRecorded() const { return _bestiaryRecorded; }
    void SetBestiaryRecorded() { _bestiaryRecorded = true; }

    // Animation preview: the default implementation covers the five standard
    // grunt sheets; bosses override with their own sheet lists.
    virtual int         GetEditorAnimCount() const { return 5; }
    virtual const char* GetEditorAnimName(int index) const;
    virtual void        PlayEditorAnim(int index);
    void  TickEditorAnimation(float dt);   // generic looping frame advance
    float GetEditorAnimFrameTime(int index) const;
    void  SetEditorAnimFrameTime(int index, float frameTime);

    // Tuned solid hitbox — stored relative to _worldPos so it is convention-free.
    // Every GetCollisionRec override returns this when set.
    void      SetCollisionRecWorld(Rectangle relativeRect) { _tunedCollisionRel = relativeRect; _hasTunedCollision = true; }
    void      ClearTunedCollision() { _hasTunedCollision = false; }
    bool      HasTunedCollision() const { return _hasTunedCollision; }
    Rectangle GetCollisionRecRelative() const;

    void  SetDrawScale(float scale) { _scale = (scale < 0.5f) ? 0.5f : scale; }
    float GetDrawScale() const { return _scale; }
    void  SetEditorFacing(float direction) { _rightLeft = direction; }
    float GetEditorFacing() const { return _rightLeft; }

    // ── Per-animation tuning (Character Animator) ─────────────────────────────
    // Each animation slot can carry its own body circle (drives BOTH the solid
    // capsule and the hurt rect), melee box (facing right), and sprite draw
    // offset. The active slot is derived from whichever sheet is playing.
    virtual int GetCurrentAnimSlot() const;

    bool    GetAnimBodySet(int slot)    const { return (slot >= 0 && slot < kAnimSlots) && _animBodySet[slot]; }
    Vector2 GetAnimBodyOffset(int slot) const { return (slot >= 0 && slot < kAnimSlots) ? _animBodyOffset[slot] : Vector2{}; }
    float   GetAnimBodyRadius(int slot) const { return (slot >= 0 && slot < kAnimSlots) ? _animBodyRadius[slot] : 0.f; }
    void    SetAnimBody(int slot, Vector2 offset, float radius);
    void    ClearAnimBody(int slot);

    bool      GetAnimMeleeSet(int slot)     const { return (slot >= 0 && slot < kAnimSlots) && _animMeleeSet[slot]; }
    Rectangle GetAnimMeleeRelRect(int slot) const { return (slot >= 0 && slot < kAnimSlots) ? _animMeleeRel[slot] : Rectangle{}; }
    void      SetAnimMelee(int slot, Rectangle relativeRect);
    void      ClearAnimMelee(int slot);

    bool    GetAnimDrawSet(int slot)         const { return (slot >= 0 && slot < kAnimSlots) && _animDrawSet[slot]; }
    Vector2 GetAnimDrawOffsetValue(int slot) const { return (slot >= 0 && slot < kAnimSlots) ? _animDrawOffset[slot] : Vector2{}; }
    void    SetAnimDrawOffset(int slot, Vector2 offset);

    static constexpr int kAnimSlots = 10;

    void SetTarget(Character* character)  { _target = character; }

    // Give each enemy a non-owning pointer to the shared nav grid so it can
    // request its own waypoint path instead of relying on the engine to pass
    // a single-step target every frame.
    void SetNavigationGrid(NavigationGrid* nav) { _nav = nav; }
    void Init();
    virtual void ResetForSpawn(Vector2 pos);
    virtual void DrawEnemy(Vector2 heroWorldPos);
    virtual void ApplyBurn(float delay, int damage, Vector2 sourcePos);
    virtual void ApplyFreeze(float duration);
    virtual void ApplyElectricCharge();
    void ClearFreeze() { _freezeTimer = 0.f; }
    void ClearBurn() { _pendingBurns.clear(); }
    void ClearElectricCharge()
    {
        _isCharged = false;
        _electricChargeTotalTimer = 0.f;
        _chargeNextStunTime = 0.f;
    }
    virtual void ApplyExternalImpulse(Vector2 impulse, bool cancelLockedAnimation);

    // ── Hit knockback (juice) ────────────────────────────────────────────────
    // A short shove away from a landed player hit + a brief nav-stagger, so a
    // struck enemy visibly recoils instead of standing planted. Rides its own
    // timer (HandleMovement rewrites _velocity every frame, so an impulse into
    // _velocity would be clobbered). Lighter than ApplyExternalImpulse, which is
    // the heavy "launch/fling" used by ogre throws. `dir` need not be normalised.
    // Virtual so an immovable type (Bonechill) can refuse the shove entirely.
    virtual void ApplyHitKnockback(Vector2 dir, float speed);
    bool IsHitStaggered() const { return _hitKnockbackTimer > 0.f; }

    // ── Shared status effects (ARPG combat-identity pass) ───────────────────────
    // Any class can apply these via the same funcs; bosses take reduced durations
    // (resistance, not immunity — see kBossStatusDurMult) so control effects stay
    // meaningful without trivialising boss fights. Damage-over-time ticks route
    // through the universal per-frame hook (UpdateStatuses, called from UpdateBurns).
    void ApplyPoison(int damagePerTick, float duration, int stacks = 1);  // stacking DoT
    void ApplyBleed(int damagePerTick, float duration);                   // physical DoT, +while moving
    void ApplySlow(float speedMult, float duration);                      // speedMult < 1
    void ApplyVulnerability(float damageTakenMult, float duration);       // mult > 1 (armor break)
    void ApplyMark(float duration);                                       // "marked"/execute window
    // Warlock class identity: cursed enemies take bonus damage from all Warlock
    // effects (see Engine::ScalePlayerHit). A timed tag like Mark, with its own
    // purple indicator so the player can track which targets are primed.
    void ApplyCurse(float duration) { if (duration > _curseTimer) _curseTimer = duration; }
    void UpdateStatuses(float dt);
    void ResetStatuses();                                                 // clear all on (re)spawn

    bool  IsPoisoned()      const { return _poisonTimer  > 0.f; }
    bool  IsBleeding()      const { return _bleedTimer   > 0.f; }
    bool  IsSlowed()        const { return _slowTimer    > 0.f; }
    bool  IsVulnerable()    const { return _vulnTimer    > 0.f; }
    bool  IsMarked()        const { return _markTimer    > 0.f; }
    bool  IsCursed()        const { return _curseTimer   > 0.f; }
    // Multiplier applied to incoming damage (armor break / vulnerability). 1 = none.
    float GetIncomingDamageMult() const { return (_vulnTimer > 0.f) ? _vulnMult : 1.f; }
    // Combined movement multiplier from slow + chill. 1 = full speed, 0 = stopped.
    float GetStatusMoveSpeedMult() const;

    // Ogre charge — sustained push until the enemy slides into a wall or prop.
    void StartForcedPush(Vector2 direction, float speed);
    virtual void OnForcedPushCollision();
    bool IsBeingForcedPushed() const { return _forcedPushActive; }

    // Shared pit fall for basic enemies. CombatDirector pauses normal AI while
    // this shrinks and pulls the sprite inward; Engine finishes it as a normal
    // death so rewards, bestiary credit, and pooled cleanup still run.
    void BeginPitFall(Vector2 pullTarget, Color tint = WHITE);
    void UpdatePitFall(float dt);
    void FinishPitFall();
    bool IsPitFalling() const { return _pitFalling; }
    bool PitFallComplete() const;
    float PitFallProgress() const;

    virtual bool IsFrozen()        const { return _freezeTimer > 0.f; }
    // Burning status for relic synergies (Ember Heart / Wildfire). Based on the
    // shared base burn queue — covers grunts, new enemies, and the new bosses.
    bool IsBurning()               const { return !_pendingBurns.empty(); }

    // Graveyard revive
    void SetGraveReviveAvailable(bool b)   { _graveReviveAvailable = b; }
    bool IsGraveReviveAvailable()    const { return _graveReviveAvailable; }
    bool IsGraveReviveInvulnerable() const { return _graveReviveInvulTimer > 0.f; }
    void TakeDamage(int damage, Vector2 attackerPos) override;
    // Unblockable variant — identical to TakeDamage for normal enemies, but a
    // Shieldbearer overrides it to skip its frontal block (Hunter Puncture Shot).
    virtual void TakeDamageUnblockable(int damage, Vector2 attackerPos) { TakeDamage(damage, attackerPos); }

    // ── Blocked-hit feedback ──────────────────────────────────────────────────
    // When a hit is denied (bodyguard/leap i-frames, or a Shieldbearer's frontal
    // block) TakeDamage records WHY instead of applying damage. The hit code then
    // reads it once to show the matching floating word ("SHIELDED" / "BLOCKED")
    // instead of a phantom damage number. See VFXManager::SpawnFloatingLabel.
    enum class HitBlockReason { None, Shielded, Blocked, Immune };
    HitBlockReason ConsumeHitBlock() { HitBlockReason r = _hitBlock; _hitBlock = HitBlockReason::None; return r; }

    // Dream Realm flicker
    bool    IsFlickerInWindup()   const { return _flickerInWindup; }
    Vector2 GetFlickerTarget()    const { return _flickerTarget; }
    float   GetFlickerCooldown()  const { return _flickerCooldown; }
    void    SetFlickerCooldown(float t) { _flickerCooldown = t; }
    void    StartFlickerWindup(float duration, Vector2 target);
    bool    ConsumeFlickerComplete();
    void    TickFlicker(float dt);
    virtual bool IsElectroStunned() const { return _isCharged && _takingDamage; }
    bool IsCharged()               const { return _isCharged; }
    virtual int  GetExpValue()  const { return _expValue; }
    virtual int  GetAttackPower() const { return (int)_attackPower; }
    bool IsDying()      const { return _dying; }
    virtual bool IsActive()     const { return _isActive; }
    virtual void SetActive(bool active);
    virtual void Teleport(Vector2 pos) { _worldPos = pos; _worldPosLastFrame = pos; _velocity = Vector2Zero(); }
    virtual Cyclops* AsCyclops() { return nullptr; }
    virtual Ogre* AsOgre() { return nullptr; }
    virtual Molarbeast* AsMolarbeast() { return nullptr; }
    virtual SkeletonArcher* AsSkeletonArcher() { return nullptr; }
    virtual FlameWisp* AsFlameWisp() { return nullptr; }
    virtual SlimeEnemy* AsSlime() { return nullptr; }
    virtual AbyssSlime* AsAbyssSlime() { return nullptr; }
    virtual PumpkinJack* AsPumpkinJack() { return nullptr; }
    virtual Minotaur* AsMinotaur() { return nullptr; }
    virtual Sporeling* AsSporeling() { return nullptr; }
    virtual Shieldbearer* AsShieldbearer() { return nullptr; }
    virtual Phantom* AsPhantom() { return nullptr; }
    virtual BomberImp* AsBomberImp() { return nullptr; }
    virtual Warchief* AsWarchief() { return nullptr; }
    virtual LivingBlade* AsLivingBlade() { return nullptr; }
    virtual ChompBug* AsChompBug() { return nullptr; }
    virtual Osiris* AsOsiris() { return nullptr; }
    virtual TitanGuard* AsTitanGuard() { return nullptr; }
    virtual ToxicVermin* AsToxicVermin() { return nullptr; }
    virtual AncientBear* AsAncientBear() { return nullptr; }
    virtual Werewolf* AsWerewolf() { return nullptr; }
    virtual Infernal* AsInfernal() { return nullptr; }
    virtual Bonechill* AsBonechill() { return nullptr; }
    virtual Stormclub* AsStormclub() { return nullptr; }
    virtual Venomfang* AsVenomfang() { return nullptr; }

    // Called once per landed melee hit on the player, right after the damage is
    // dealt (see HandleAttack / the elite lunge). Default does nothing; fire/
    // status enemies (e.g. Infernal) override it to also apply a status like burn.
    virtual void OnMeleeHitPlayer(Character* target) { (void)target; }

    // ── Warchief aura ─────────────────────────────────────────────────────────
    // Nearby allies move faster while inside the warchief's banner radius.
    // The warchief refreshes this every frame; it decays within half a second
    // of leaving the aura. Ticked inside UpdateBurns (which every type calls).
    void  GrantWarAura(float duration) { if (duration > _warAuraTimer) _warAuraTimer = duration; }
    bool  HasWarAura() const { return _warAuraTimer > 0.f; }
    static constexpr float kWarAuraSpeedMultiplier = 1.3f;
    virtual bool UsesDirectPursuit() const { return false; }
    virtual bool IgnoresPropCollisions() const { return false; }
    virtual bool IsBoss() const { return false; }

    // ── Enemy-identity approach hooks (CombatEngagement / Task 3) ────────────
    // Small, opt-in flags consulted by Enemy::HandleMovement's genuine-Commit
    // approach (i.e. actually walking in to attack, not Reposition/Support —
    // see ComputeEngagementTarget above). Both default to "no change" so every
    // existing type keeps its current approach unless it opts in.
    //
    // Slime: "preserves its chosen line rather than continuously tracking
    // every player movement" — once a Commit approach starts, HandleMovement
    // snapshots the first resolved target and keeps walking toward that fixed
    // point instead of re-tracking the player every frame.
    virtual bool LocksApproachLineOnCommit() const { return false; }
    // Sporeling: "approaches indirectly" — a small perpendicular blend added
    // to the direct approach direction while genuinely closing to Commit, so
    // the path curves instead of beelining. 0 = direct line (default).
    virtual float GetApproachLateralBias() const { return 0.f; }

    // ── Elite signature kits ──────────────────────────────────────────────────
    // The generic elite lunge is gone: signature movement belongs to the elite
    // that was designed for it (Ogre's charge, Stormclub's leap, Venomfang's
    // pounce). A curated elite overrides these hooks; ordinary enemies keep the
    // do-nothing defaults. UpdateEliteSignature may temporarily own movement
    // and attacks — returning true skips normal chase/melee for that frame.
    virtual EliteArchetype GetEliteArchetype() const { return EliteArchetype::Count; }
    virtual bool UpdateEliteSignature(float deltaTime, Vector2 navigationTarget,
        bool hasNavigationTarget, const std::vector<std::unique_ptr<Enemy>>& enemies,
        const std::vector<Vector2>& propCenters)
    {
        (void)deltaTime; (void)navigationTarget; (void)hasNavigationTarget;
        (void)enemies; (void)propCenters;
        return false;
    }
    // Draws this elite's warning geometry (world space). CombatDirector calls it
    // during the world pass so telegraphs render under enemies and VFX.
    virtual void DrawEliteTelegraph() const {}
    virtual void DebugForceEliteSignature() {}
    virtual void DebugForceElitePhaseTwo() {}
    virtual const char* GetEliteSignatureStateName() const { return "None"; }
    virtual EliteSignatureTelemetry GetEliteSignatureTelemetry() const;

    // Bounded per-enemy combat-beat queue: elites push, CombatDirector drains
    // right after this enemy's Update. EmitEliteEvent stamps sequence + phase.
    bool EmitEliteEvent(EliteSignatureEvent event);
    bool ConsumeEliteEvent(EliteSignatureEvent& event) { return _eliteEvents.Pop(event); }
    void ClearEliteEvents() { _eliteEvents.Clear(); }

    // Guard Links room modifier: visible damage reduction while linked guards
    // live — replaces the old bodyguard full invulnerability.
    void SetEliteGuardLinked(bool linked) { _eliteGuardLinked = linked; }
    bool IsEliteGuardLinked() const { return _eliteGuardLinked; }
    // True once per reduced hit — damage-number code shows the reduction.
    bool ConsumeGuardReducedFlag() { bool was = _eliteGuardReducedHit; _eliteGuardReducedHit = false; return was; }

    bool IsElitePhaseTwo() const { return _isEliteMiniboss && GetPhase() >= 1; }

    // ── Attack swing weight (juice) ───────────────────────────────────────────
    // True for enemies whose basic attack is a melee swing that reads well with
    // the player's anticipation→overshoot lean (see Enemy::DrawEnemy). Ranged /
    // caster roles return false so a bow-draw or spell-cast doesn't hop forward.
    // Only fires while the SHARED attack sheet (_attackAnim) is playing, so bosses
    // with bespoke attack animations are naturally left untouched; a projectile
    // enemy that keeps a melee/Boss role overrides this to false to be explicit.
    virtual bool UsesAttackLunge() const
    {
        switch (GetEncounterRole())
        {
            case EnemyRole::Ranged:
            case EnemyRole::HeavyRanged:
            case EnemyRole::Zoner:
            case EnemyRole::Summoner:
            case EnemyRole::Support:
                return false;
            default:
                return true;   // Grunt / Charger / Tank / Assassin / melee Boss
        }
    }
    std::uint64_t GetCombatId() const { return _combatId; }
    void SetCombatId(std::uint64_t id) { _combatId = id; }
    bool IsEliteMiniboss() const { return _isEliteMiniboss; }
    void SetIsEliteMiniboss(bool b);

    // ── Encounter metadata ────────────────────────────────────────────────────
    // Tactical role + spawn-budget cost consumed by the encounter director. Base
    // default is a cheap Grunt; specific enemy types override these. Bosses report
    // Boss so the director can keep them out of the add budget.
    virtual EnemyRole GetEncounterRole() const { return IsBoss() ? EnemyRole::Boss : EnemyRole::Grunt; }
    virtual int       GetSpawnCost()     const { return 1; }

    // ── Elite-room mechanics ──────────────────────────────────────────────
    // _isInvulnerable : bodyguard shield — engine clears when all grunts die
    // _leapInvulnerable: gap-closer wind-up — engine sets/clears around leap
    void SetInvulnerable(bool v)   { _isInvulnerable    = v; }
    void SetLeapFrozen(bool v)     { _leapInvulnerable  = v; }
    bool IsInvulnerable()    const { return _isInvulnerable; }
    bool IsLeapFrozen()      const { return _leapInvulnerable; }
    void ApplyEnrage();            // +50% speed, half attack cooldown, called by Engine on elite spawn

    // ── Boss enrage phase ──────────────────────────────────────────────────────
    // A boss enters an enraged "final phase" once its HP drops below
    // _enrageThreshold (a fraction of max HP; 0 = this enemy never enrages). The
    // latch is ONE-WAY within a fight — healing back up will NOT un-enrage it —
    // and auto-resets when the enemy is at full HP (i.e. on a fresh/pooled spawn).
    // Driven every frame from UpdateBurns(); the transition requests a screen
    // shake (ConsumeEnrageShakeRequest) so the phase change actually reads.
    void UpdateEnrageLatch(float dt);
    bool IsEnraged() const { return _enrageLatched; }
    bool ConsumeEnrageShakeRequest();       // true once, on the frame enrage begins
    bool IsEnrageFlashing() const { return _enrageFlashTimer > 0.f; }

    // ── Multi-phase boss system ─────────────────────────────────────────────────
    // Generalises the enrage latch: a boss declares HP-fraction thresholds (in
    // DESCENDING order, e.g. {0.66, 0.33}) and _phase counts how many have been
    // crossed (0 = full-strength opening phase). One-way within a fight; auto-resets
    // at full HP (fresh/pooled spawn). Driven every frame from UpdateBurns().
    // A boss polls ConsumePhaseChange() to react ONCE on the frame a new phase
    // begins (roar, unlock attacks, spawn a hazard); GetPhase() gates ongoing
    // behaviour. Bosses can honour IsInPhaseTransition() to grant a brief
    // invulnerable "set-piece" window while the transition animation plays.
    void SetPhaseThresholds(std::vector<float> descendingHpFractions);
    void UpdatePhaseLatch(float dt);
    int  GetPhase() const { return _phase; }
    int  GetPhaseCount() const { return (int)_phaseThresholds.size() + 1; }
    int  ConsumePhaseChange();              // returns the new phase once, else -1
    void BeginPhaseTransition(float seconds) { _phaseTransitionTimer = seconds; }
    bool IsInPhaseTransition() const { return _phaseTransitionTimer > 0.f; }

    // ── Boss-state callout ────────────────────────────────────────────────────
    // A floating word ("ENRAGED" / "PHASE SHIFT" / "SHIELD DOWN") announced on a
    // state transition. Set from the enrage/phase latches (or by the engine for
    // shield changes); the runtime polls it once per enemy and shows it via
    // VFXManager::SpawnFloatingLabel. Kept separate from the shake/phase
    // consumers so nothing double-consumes. `text` must be a string literal.
    void        RequestBossCallout(const char* text) { _bossCallout = text; }
    const char* ConsumeBossCallout() { const char* c = _bossCallout; _bossCallout = nullptr; return c; }
    Rectangle GetCollisionRec()       const override;
    Capsule2D GetCapsule()            const override;
    virtual Rectangle GetAttackCollisionRec() const;

    float GetAttackBoxWidth()   const { return _attackBoxWidth; }
    float GetAttackBoxHeight()  const { return _attackBoxHeight; }
    float GetAttackBoxOffsetX() const { return _attackBoxOffsetX; }
    float GetAttackBoxOffsetY() const { return _attackBoxOffsetY; }
    void  SetAttackBoxWidth(float v)   { _attackBoxWidth   = std::max(4.f, v); }
    void  SetAttackBoxHeight(float v)  { _attackBoxHeight  = std::max(4.f, v); }
    void  SetAttackBoxOffsetX(float v) { _attackBoxOffsetX = v; }
    void  SetAttackBoxOffsetY(float v) { _attackBoxOffsetY = v; }

    // Wider rect used only for player melee hit-detection; defaults to solid rect.
    // Cyclops overrides this so the body can be hit from any angle while the
    // narrow solid rect still lets the player walk up close.
    virtual Rectangle GetHitCollisionRec() const
    {
        Rectangle r = GetCollisionRec();
        constexpr float padX = 28.f;
        constexpr float padY = 18.f;
        return { r.x - padX, r.y - padY, r.width + padX * 2.f, r.height + padY * 2.f };
    }
	void PlayAttackSound() override;
    void PlayDeathSound() override;
    void PlayHurtSound() override;

protected:
    static void EnsureSharedResourcesLoaded();

    // Applies charactertuning_<Name>.txt overrides (scale, hitboxes, anim
    // speeds). Tunable classes call this at the END of their ResetForSpawn.
    void ApplyStoredTuning();

    // Clears all per-anim + whole-character tuning state; tunable classes call
    // this at the start of their ResetForSpawn before re-applying from file.
    void ResetTuningState();

    // Gameplay helpers for the per-anim data (current facing applied):
    bool    GetAnimBodyCapsuleWorld(Capsule2D& out) const;    // current slot's circle
    bool    GetAnimBodyRectWorld(Rectangle& out) const;       // its bounding square
    bool    GetAnimMeleeRectWorld(int slot, Rectangle& out) const;
    Vector2 GetCurrentAnimDrawOffset() const;                 // sprite-only shift

    Rectangle GetTunedCollisionRec() const
    {
        return Rectangle{
            _worldPos.x + _tunedCollisionRel.x,
            _worldPos.y + _tunedCollisionRel.y,
            _tunedCollisionRel.width,
            _tunedCollisionRel.height
        };
    }

    // Clamps a locked movement segment (leap / pounce endpoint) to the LAST
    // point that is inside the room, on navigable ground, and clear of solid
    // props — the fairness rule "what was warned is what hits" requires the
    // committed path to respect authored room geometry. Uses the same nav-grid
    // truth as pathfinding and the player's Lightning Blink. Returns `desired`
    // unchanged when no nav grid is attached (legacy arena mode).
    Vector2 ClampElitePathToNavigable(Vector2 start, Vector2 desired,
                                      const std::vector<Vector2>& propCenters,
                                      float propClearance = 70.f) const;

    void HandleMovement(float dt, Vector2 navigationTarget, bool hasNavigationTarget,
        const std::vector<std::unique_ptr<Enemy>>& enemies, const std::vector<Vector2>& propCenters);
    Vector2 ResolveNavTarget(float dt, Vector2 playerFeet, Vector2 navigationTarget, bool hasNavigationTarget);
    void HandleAttack(const std::vector<std::unique_ptr<Enemy>>& enemies);
    void PickApproachOffset();
    bool CanTakeAttackSlot(const std::vector<std::unique_ptr<Enemy>>& enemies) const;
    // Single choke point for every "_attacking = false" transition EXCEPT the
    // pooled-reuse hard reset in ResetForSpawn (which resets the whole latch
    // instead, with no recovery countdown). Releases the engagement Commit
    // slot into its post-attack recovery window whenever an attack was
    // actually in progress (natural end, target died, forced-push/impulse
    // interruption) so the latch can never get stuck permanently locked.
    void ReleaseAttackCommitment();
    void UpdateBurnPanic(float dt);
    void UpdateBurns(float dt);
    void UpdateElectricCharge(float dt);
    void UpdateLaunchVisual(float dt);

    void HandleAnimation(float dt);
    virtual void DrawHealthBar(Vector2 screenPos, float w, float h);
    void DrawEliteLabel(Vector2 screenPos, float w, float h);

    // Slice a horizontal sprite strip of `frameCount` frames into the active
    // animation. Shared by every enemy/boss so the frame-math lives in one place;
    // subclasses forward their own _sheetFrameCount.
    void SetSpriteSheet(const Texture2D& sheet, int frameCount, float frameTime, bool resetFrame);

    // Health-bar geometry, tunable per subclass. Defaults match the legacy
    // melee bosses (Cyclops/Ogre); taller sprites override these in their ctor.
    float _healthBarHeight  = 6.f;    // bar thickness in px
    float _healthBarYFrac   = 0.5f;   // fraction of sprite height above the origin
    float _healthBarYOffset = 12.f;   // extra px above that

    struct PendingBurn
    {
        float timer = 0.f;
        int damage = 0;
        Vector2 sourcePos{};
    };

    Character*      _target = nullptr;
    NavigationGrid* _nav    = nullptr;   // non-owning; set once by Engine on spawn
    // Non-owning; refreshed each frame by CombatDirector (see SetHazardZones).
    const std::vector<HazardZone>* _hazardZones = nullptr;
    // Non-owning; refreshed each frame by CombatDirector (see SetSquadDirective).
    const SquadDirective* _squadDirective = nullptr;

    // ── Facing commitment (ticked in Update) ──────────────────────────────────
    float _facingLockTimer    = 0.f;   // > 0: facing frozen (attack + recovery)
    float _turnCommitCooldown = 0.f;   // > 0: voluntary flips refused
    float _turnCommitInterval = Balance::Facing::kTurnCommitInterval; // heavies override

    // ── Waypoint path cache ───────────────────────────────────────────────────
    // Extracted from the shared flow field. Each enemy refreshes its own path
    // on a staggered timer so not all enemies query the grid on the same frame.
    std::vector<Vector2> _waypoints;        // cached path from flow field
    int   _waypointIndex          = 0;     // which waypoint we're heading toward
    float _pathRefreshTimer       = 0.f;   // countdown to next refresh
    float _pathRefreshInterval    = 0.5f;  // randomised per-enemy at spawn
    static constexpr float kPathRefreshMin = 0.30f;  // minimum refresh interval
    static constexpr float kPathRefreshMax = 0.70f;  // maximum refresh interval
    static constexpr int   kMaxWaypoints   = 14;     // path length cap
    bool _isActive        = true;
    bool _pitFalling      = false;
    float _pitFallTimer   = 0.f;
    float _pitStartScale  = 0.f;
    Vector2 _pitStartPos{};
    Vector2 _pitTargetPos{};
    Color _pitFallTint = WHITE;
    static constexpr float kPitFallDuration = 0.20f;
    std::uint64_t _combatId = 0;

    // ── Stable engagement commitment state (see public accessors above) ──────
    // _runtimeId is assigned ONCE in the constructor and is never touched by
    // ResetForSpawn — it must survive pooled reuse so CombatDirector can keep
    // matching this C++ object across frames/lives.
    std::uint64_t    _runtimeId = 0;
    EngagementIntent  _engagementIntent = EngagementIntent::Reposition;
    Vector2           _engagementTarget{};
    bool              _hasEngagementAssignment = false;
    bool              _swarmProfile = false;
    EngagementLatch   _engagementLatch;

    // ── Engagement target consumption (HandleMovement, Task 3) ───────────────
    // CombatEngagement's orbitAngle is re-derived from a per-frame-incrementing
    // sequence (see CombatDirector's _engagementSequence), so GetEngagementTarget()
    // itself is NOT stable frame-to-frame. Rather than chase that per-frame
    // noise directly (which would read as vibrating instead of holding a lane),
    // HandleMovement samples it into this held point on a slow timer — the same
    // pattern _approachOffset already uses for its own re-pick cadence.
    Vector2 _engagementHoldPos{};
    float   _engagementHoldTimer = 0.f;
    bool    _engagementHoldValid = false;
    static constexpr float _engagementHoldDuration = 1.4f;

    // Slime's LocksApproachLineOnCommit(): the fixed point snapshotted the
    // first frame a genuine Commit approach begins, held until attack range
    // or a non-Commit intent breaks the lock.
    Vector2 _lockedApproachTarget{};
    bool    _approachLineLocked = false;

    bool _bestiaryRecorded = false;   // has this death been counted in the bestiary
    bool _isEliteMiniboss = false;
    bool _isInvulnerable  = false;   // bodyguard shield (engine-driven)
    bool _leapInvulnerable = false;  // gap-closer wind-up (engine-driven)
    // Why the most recent hit was denied; set by TakeDamage / a subclass block,
    // cleared when the hit code reads it via ConsumeHitBlock(). Subclasses (e.g.
    // Shieldbearer) set this before returning early from their own TakeDamage.
    HitBlockReason _hitBlock = HitBlockReason::None;

    bool  _attacking    = false;
    bool  _damageApplied = false;

    // ── Elite signature runtime (see the public elite hooks above) ────────────
    EliteEventQueue _eliteEvents;
    std::uint32_t   _eliteEventSequence   = 0;
    int             _eliteDroppedEvents   = 0;
    bool            _eliteGuardLinked     = false;
    bool            _eliteGuardReducedHit = false;
    int             _eliteSignatureCasts  = 0;
    int             _eliteSignatureHits   = 0;

    Vector2 _burnPanicDir = {};
    float   _burnPanicTurnTimer = 0.f;
    float   _burnSoundTimer = 0.f;

    float   _warAuraTimer = 0.f;   // > 0 while buffed by a Warchief banner

    // Boss enrage phase (see UpdateEnrageLatch). _enrageThreshold set per boss.
    float   _enrageThreshold  = 0.f;   // fraction of max HP; 0 = never enrages
    bool    _enrageLatched    = false; // one-way once crossed (until full-HP respawn)
    bool    _enrageShakePending = false; // transition telegraph, consumed by CombatDirector
    float   _enrageFlashTimer = 0.f;   // brief visual flash window on transition
    const char* _bossCallout  = nullptr; // pending floating word, consumed by the runtime

    // Multi-phase boss system (see UpdatePhaseLatch). Empty thresholds = single-phase.
    std::vector<float> _phaseThresholds;      // descending HP fractions, e.g. {0.66,0.33}
    int     _phase              = 0;          // phases crossed so far (0 = opening phase)
    int     _pendingPhaseChange = -1;         // new phase to announce once; -1 = none
    float   _phaseTransitionTimer = 0.f;      // >0 while a transition set-piece plays

    float   _freezeTimer        = 0.f;

    // ── Shared status effects (ARPG combat pass) ────────────────────────────────
    // Bosses scale status durations by this so control never fully dominates them.
    static constexpr float kBossStatusDurMult = 0.5f;
    float _poisonTimer     = 0.f;   // remaining poison duration
    float _poisonTickTimer = 0.f;   // countdown to next poison tick
    int   _poisonPerTick   = 0;     // base damage per tick (before stacks)
    int   _poisonStacks    = 0;     // stacking intensity (capped)
    float _bleedTimer      = 0.f;
    float _bleedTickTimer  = 0.f;
    int   _bleedPerTick    = 0;
    float _slowTimer       = 0.f;
    float _slowMult        = 1.f;   // movement multiplier while slowed (<1)
    float _vulnTimer       = 0.f;
    float _vulnMult        = 1.f;   // incoming-damage multiplier while vulnerable (>1)
    float _markTimer       = 0.f;   // "marked"/execute window
    float _curseTimer      = 0.f;   // Warlock curse tag (bonus damage window)

    // Graveyard revive — set by Engine when entering a Graveyard room
    bool    _graveReviveAvailable  = false;
    float   _graveReviveInvulTimer = 0.f;

    // Dream Realm flicker — biome mechanic, managed externally by Engine
    float   _flickerCooldown    = 0.f;
    float   _flickerWindupTimer = 0.f;
    bool    _flickerInWindup    = false;
    Vector2 _flickerTarget      = {};

    // Electric charge — applied by ElectricSpread hits; repeating random stuns, capped at 10 s
    bool    _isCharged              = false;
    float   _chargeNextStunTime     = 0.f;   // countdown to next stun trigger
    float   _electricChargeTotalTimer = 0.f; // overall charge window; zeroes out the effect

    int     _expValue       = 1;

    // Stuck detection — if barely moved for _stuckThreshold seconds, apply a kick
    float   _stuckTimer     = 0.f;
    Vector2 _stuckCheckPos  = {};
    static constexpr float _stuckThreshold  = 0.4f;
    static constexpr float _stuckMinMove    = 8.f;   // pixels needed to not be considered stuck

    bool    _forcedPushActive    = false;
    Vector2 _forcedPushDirection = {};
    float   _forcedPushSpeed     = 0.f;

    // Hit knockback (see ApplyHitKnockback). Independent of _velocity/nav so a
    // struck enemy recoils cleanly; decays fast and pauses pursuit while active.
    Vector2 _hitKnockbackVel   = {};
    float   _hitKnockbackTimer = 0.f;
    static constexpr float kHitKnockbackDuration = 0.14f; // how long the stagger lasts
    static constexpr float kHitKnockbackDecay    = 11.f;  // higher = snappier settle

    float _attackRange = 110.f;
    float _attackUpdateTime = 1.f / 8.f;
    bool  _inAttackRange = false;
    static constexpr float _attackRangeHysteresis = 15.f;

    float _attackBoxWidth   = 48.f;
    float _attackBoxHeight  = 75.f;
    float _attackBoxOffsetX = 57.f;
    float _attackBoxOffsetY = 0.f;

    // Sprite-only draw offset for the attack animation — compensates for
    // the weapon swing requiring the body to be off-centre in the art.
    // X is multiplied by _rightLeft so the correction mirrors correctly.
    float _attackVisualOffsetX = 0.f;
    float _attackVisualOffsetY = 0.f;

    float _attackCooldown = 0.f;
    float _attackDelay = 1.0f;
    float _launchVisualTimer = 0.f;
    bool _launchHoldingHurtPose = false;
    // Length of the death animation window (mirrors the _deathTimer set in
    // BaseCharacter::TakeDamage / ResetForSpawn) — used by the death-pop juice in
    // DrawEnemy to normalise progress. Keep in sync if the death timer changes.
    static constexpr float kDeathAnimDuration = 0.4f;
    static constexpr float _launchVisualDuration = 0.22f;
    static constexpr float _launchVisualScaleBoost = 0.18f;
    static constexpr float _launchVisualLift = 18.f;

    // Flank tuning: regular enemies should not all aim for the exact same
    // player point. Each one picks a small left/right lane and tries to arc
    // into that lane once it gets near the player.
    float _flankSide = 1.f;
    float _flankDistance = 80.f;
    static constexpr float _flankStartDistance = 0.f;   // disabled — approach offset handles spread
    static constexpr float _flankBlendStrength = 0.35f;
    static constexpr float _minFlankDistance = 55.f;
    static constexpr float _maxFlankDistance = 110.f;
    static constexpr float _propSlideStrength = 0.75f;

    // 8-direction approach: each enemy picks one of 8 slots around the player
    // and moves toward that offset point. Resets on a timer and after each hit
    // so enemies shift positions and never permanently crowd one side.
    Vector2 _approachOffset = {};
    float   _approachOffsetTimer = 0.f;
    static constexpr float _approachOffsetRadius   = 120.f;
    static constexpr float _approachOffsetDuration = 3.5f;

    Vector2 _homePos;
    std::vector<PendingBurn> _pendingBurns;

    // ── Character Animator tuning state ──────────────────────────────────────
    bool      _hasTunedCollision = false;
    Rectangle _tunedCollisionRel{};              // relative to _worldPos
    float     _editorAnimFrameTimes[10] = {};    // per-anim overrides; <=0 = default

    // Per-animation body circle / melee box / draw offset (kAnimSlots entries).
    bool      _animBodySet[10] = {};
    Vector2   _animBodyOffset[10] = {};          // relative to _worldPos, facing right
    float     _animBodyRadius[10] = {};
    bool      _animMeleeSet[10] = {};
    Rectangle _animMeleeRel[10] = {};            // relative to _worldPos, facing right
    bool      _animDrawSet[10] = {};
    Vector2   _animDrawOffset[10] = {};          // sprite-only shift, facing right

    static Texture2D _sharedIdleAnim;
    static Texture2D _sharedWalkAnim;
    static Texture2D _sharedAttackAnim;
    static Texture2D _sharedTakeDamageAnim;
    static Texture2D _sharedDeathAnim;
    static Sound _sharedAttackSound;
    static Sound _sharedHurtSound;
    static Sound _sharedDeathSound;
    static bool _sharedResourcesLoaded;


};
