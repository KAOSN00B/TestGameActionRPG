#pragma once

#include "Enemy.h"

// =============================================================================
// Phantom — a drifting ghost that cycles between two states:
//   TANGIBLE  (~2.2s): solid, can be hit, bites at close range.
//   PHASED    (~1.6s): translucent, UNHITTABLE, drifts faster and floats
//                      straight through props toward the player.
// The fight is about timing damage into its tangible windows.
// Variants: green -> purple -> shadow.
// =============================================================================
class Phantom : public Enemy
{
public:
    Phantom(Vector2 pos);
    ~Phantom() override;

    void Init();
    void ResetForSpawn(Vector2 pos) override;
    static void UnloadSharedResources();
    CreatureFamily GetCreatureFamily() const override { return CreatureFamily::Small; }

    void Update(float dt, Vector2 heroWorldPos, Vector2 navigationTarget, bool hasNavigationTarget,
        const std::vector<std::unique_ptr<Enemy>>& enemies,
        const std::vector<Vector2>& propCenters) override;
    void SetWaveScale(int wave) override;
    void SetVariantTier(int tier) override;
    void TakeDamage(int damage, Vector2 attackerPos) override;
    void DrawEnemy(Vector2 heroWorldPos) override;
    Rectangle GetCollisionRec() const override;
    Capsule2D GetCapsule() const override;

    Phantom* AsPhantom() override { return this; }
    EnemyRole GetEncounterRole() const override { return EnemyRole::Assassin; }
    int       GetSpawnCost()     const override { return 3; }
    const char* GetTuningName() const override { return "Phantom"; }
    bool UsesDirectPursuit()     const override { return true; }
    bool IgnoresPropCollisions() const override { return true; }

    bool IsPhased() const { return _phased; }

    void PlayAttackSound() override;
    void PlayDeathSound() override;

private:
    static void EnsureSharedResourcesLoaded();
    void HandleAnimationLoop(float dt);

    int   _variantTier = 0;    // 0 green, 1 purple, 2 shadow
    bool  _phased      = false;
    float _phaseTimer  = 0.f;
    float _bobTimer    = 0.f;
    float _biteCooldown = 0.f;
    // Tracks whether this tangible window actually landed a bite, so the
    // engagement Commit slot (see Update) is only released back into its
    // recovery window when it was genuinely held — mirrors
    // Enemy::ReleaseAttackCommitment's own "only if an attack was actually in
    // progress" guard, which Phantom can't reuse directly since it never sets
    // the shared _attacking flag.
    bool  _bitThisCycle = false;

    static constexpr float kTangibleDuration = 2.2f;
    static constexpr float kPhasedDuration   = 1.6f;
    static constexpr float kDriftSpeed       = 165.f;
    static constexpr float kPhasedSpeedMult  = 1.7f;
    static constexpr float kBiteRange        = 95.f;
    static constexpr float kBiteCooldown     = 1.4f;

    static constexpr int kVariantCount = 3;
    static Texture2D _sharedIdleAnim[kVariantCount];
    static Texture2D _sharedWalkAnim[kVariantCount];
    static Texture2D _sharedAttackAnim[kVariantCount];
    static Texture2D _sharedHurtAnim[kVariantCount];
    static Texture2D _sharedDeathAnim[kVariantCount];
    static Sound     _sharedAttackSound;
    static Sound     _sharedHurtSound;
    static Sound     _sharedDeathSound;
    static bool      _sharedResourcesLoaded;
};
