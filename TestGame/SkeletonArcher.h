#pragma once

#include "Enemy.h"
#include "raymath.h"
#include <vector>
#include <memory>

// =============================================================================
// SkeletonArcher — the game's first true ranged grunt.
// Keeps a comfortable distance band from the player: retreats when crowded,
// advances when too far, and strafes sideways inside the band. Fires a single
// aimed arrow after a short draw-bow telegraph (Engine spawns the projectile).
// =============================================================================
class SkeletonArcher : public Enemy
{
public:
    SkeletonArcher(Vector2 pos);
    CreatureFamily GetCreatureFamily() const override { return CreatureFamily::Small; }
    ~SkeletonArcher() override;

    void Init();
    void ResetForSpawn(Vector2 pos) override;
    static void UnloadSharedResources();

    void Update(float dt, Vector2 heroWorldPos,
                Vector2 navigationTarget, bool hasNavigationTarget,
                const std::vector<std::unique_ptr<Enemy>>& enemies,
                const std::vector<Vector2>& propCenters) override;

    void SetWaveScale(int wave) override;
    void SetVariantTier(int tier) override;
    void DrawEnemy(Vector2 cameraRef) override;
    Rectangle GetCollisionRec() const override;
    Capsule2D GetCapsule()      const override;

    void ApplyFreeze(float duration) override;
    SkeletonArcher* AsSkeletonArcher() override { return this; }
    EnemyRole GetEncounterRole() const override { return EnemyRole::Ranged; }
    int       GetSpawnCost()     const override { return 2; }
    const char* GetTuningName() const override { return "SkeletonArcher"; }

    // Arrow firing interface (read by CombatDirector after Update)
    bool    WantsToFireArrow() const { return _wantsToFire; }
    Vector2 GetArrowDirection() const { return _fireDirection; }
    void    OnArrowFired();
    void    EnableRelentlessFire();

    void PlayAttackSound() override;

private:
    static void EnsureSharedResourcesLoaded();

    void SetIdleAnimation(bool resetFrame);
    void SetWalkAnimation(bool resetFrame);
    void SetAttackAnimation(bool resetFrame);
    void HandleKiteMovement(float dt, const std::vector<Vector2>& propCenters);
    void HandleAnimation(float dt);
    void CancelDraw();

    float _facingTimer = 0.f;

    // Draw-bow telegraph + shot cooldown
    float   _drawTimer      = 0.f;
    float   _shotCooldown   = 0.f;
    bool    _drawingBow     = false;
    bool    _wantsToFire    = false;
    bool    _relentlessFire = false;
    Vector2 _fireDirection  = {};
    float   _strafeSign     = 1.f;   // which way to circle inside the band
    float   _strafeSwapTimer = 0.f;

    // "Finds a clear firing lane, plants during its existing aim telegraph,
    // fires, and relocates before firing again. It does not kite
    // continuously." (design doc) — the archer now only actively repositions
    // for this short window right after loosing an arrow; the rest of the
    // time it plants in its current lane instead of perpetually strafing.
    float   _relocateTimer  = 0.f;
    static constexpr float _relocateDuration = 0.55f;

    // Tuned by SetWaveScale
    float _drawDurationInst  = 0.65f;
    float _shotCooldownInst  = 2.4f;

    int _variantTier = 0;   // 0 bone-white, 1 gold, 2 onyx, 3 flaming red

    static constexpr float _kiteRetreatDistance  = 380.f;   // player closer -> back away
    static constexpr float _kiteAdvanceDistance  = 620.f;   // player farther -> approach
    static constexpr float _fireMaxRange         = 900.f;

    // One texture set per colour-variant tier.
    static constexpr int kVariantCount = 4;
    static Texture2D _sharedIdleAnim[kVariantCount];
    static Texture2D _sharedWalkAnim[kVariantCount];
    static Texture2D _sharedAttackAnim[kVariantCount];
    static Texture2D _sharedTakeDamageAnim[kVariantCount];
    static Texture2D _sharedDeathAnim[kVariantCount];
    static Sound     _sharedAttackSound;
    static Sound     _sharedHurtSound;
    static Sound     _sharedDeathSound;
    static bool      _sharedResourcesLoaded;
};
