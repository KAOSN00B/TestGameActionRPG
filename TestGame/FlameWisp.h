#pragma once

#include "Enemy.h"
#include "raymath.h"
#include <vector>
#include <memory>

// =============================================================================
// FlameWisp — a floating fire spirit caster.
// Drifts lazily toward the player, then teleports to a new position around
// them (short shrink-out / shrink-in telegraph) and lobs a fire bolt.
// It floats, so it ignores prop collision and never uses pathfinding.
// =============================================================================
class FlameWisp : public Enemy
{
public:
    FlameWisp(Vector2 pos);
    CreatureFamily GetCreatureFamily() const override { return CreatureFamily::Small; }
    ~FlameWisp() override;

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

    FlameWisp* AsFlameWisp() override { return this; }
    EnemyRole GetEncounterRole() const override { return EnemyRole::Zoner; }
    int       GetSpawnCost()     const override { return 3; }
    const char* GetTuningName() const override { return "FlameWisp"; }
    bool UsesDirectPursuit()      const override { return true; }
    bool IgnoresPropCollisions()  const override { return true; }

    // Fire bolt interface (read by CombatDirector after Update)
    bool    WantsToCastBolt() const { return _wantsToCast; }
    Vector2 GetBoltDirection() const { return _castDirection; }
    void    OnBoltCast();

    void PlayAttackSound() override;
    void PlayDeathSound() override;

private:
    enum class WispState
    {
        Drifting,      // slow float toward a hover point near the player
        TeleportOut,   // shrinking away
        TeleportIn,    // reappearing at the new spot
        Casting        // wind-up before the fire bolt
    };

    static void EnsureSharedResourcesLoaded();

    void SetIdleAnimation(bool resetFrame);
    void HandleAnimation(float dt);
    Vector2 PickTeleportSpot(float dt);

    WispState _state = WispState::Drifting;
    float   _stateTimer      = 0.f;
    float   _teleportCooldown = 0.f;
    float   _castWindupInst  = 0.55f;
    float   _teleportCooldownInst = 3.4f;
    float   _hoverBobTimer   = 0.f;
    Vector2 _teleportTarget{};
    bool    _wantsToCast     = false;
    Vector2 _castDirection{};

    static constexpr float _teleportOutDuration = 0.28f;
    static constexpr float _teleportInDuration  = 0.24f;
    static constexpr float _teleportRadiusMin   = 260.f;
    static constexpr float _teleportRadiusMax   = 430.f;

    int _variantTier = 0;   // 0 orange, 1 blue flame, 2 eerie green, 3 magenta

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
