#pragma once

#include "Enemy.h"

// =============================================================================
// Shieldbearer — an armoured orc whose raised shield BLOCKS ALL frontal damage.
// Reuses the base grunt chase/attack behaviour, but TakeDamage ignores hits
// coming from the side it faces (with a visible spark) — flank it, bait its
// swing, or knock it around with abilities to land real damage.
// Variants: brown -> black -> red.
// =============================================================================
class Shieldbearer : public Enemy
{
public:
    Shieldbearer(Vector2 pos);
    CreatureFamily GetCreatureFamily() const override { return CreatureFamily::Beast; }
    ~Shieldbearer() override;

    void Init();
    void ResetForSpawn(Vector2 pos) override;
    static void UnloadSharedResources();

    void SetWaveScale(int wave) override;
    void SetVariantTier(int tier) override;
    void TakeDamage(int damage, Vector2 attackerPos) override;
    // Puncture Shot ignores the raised shield — deal straight to the base HP.
    void TakeDamageUnblockable(int damage, Vector2 attackerPos) override { Enemy::TakeDamage(damage, attackerPos); }
    void DrawEnemy(Vector2 heroWorldPos) override;
    Rectangle GetCollisionRec() const override;
    void PlayDeathSound() override;

    Shieldbearer* AsShieldbearer() override { return this; }
    // Tank feeds the shared CombatEngagement screening logic (Task 3):
    // ComputeEngagementTarget seats a Tank-role Support intent on the line
    // between the ally centroid and the player, biased toward the player —
    // "occupying the line between that ally and the player" (design doc).
    // Shieldbearer needs no positioning code of its own; it inherits base
    // Enemy::HandleMovement's engagement consumption unchanged. It commits
    // offensively (chases/attacks normally) whenever BuildEngagementAssignments
    // grants it a Commit slot, i.e. "when its formation breaks or a slot is
    // available."
    EnemyRole GetEncounterRole() const override { return EnemyRole::Tank; }
    int       GetSpawnCost()     const override { return 3; }
    const char* GetTuningName() const override { return "Shieldbearer"; }

    void PlayAttackSound() override;

private:
    static void EnsureSharedResourcesLoaded();

    int   _variantTier     = 0;    // 0 brown, 1 black, 2 red
    float _blockFlashTimer = 0.f;  // shield spark after a blocked hit

    static constexpr int kVariantCount = 3;
    static Texture2D _sharedIdleAnim[kVariantCount];
    static Texture2D _sharedWalkAnim[kVariantCount];
    static Texture2D _sharedAttackAnim[kVariantCount];
    static Texture2D _sharedHurtAnim[kVariantCount];
    static Texture2D _sharedDeathAnim[kVariantCount];
    static Sound     _sharedAttackSound;
    static Sound     _sharedHurtSound;
    static Sound     _sharedDeathSound;
    static Sound     _sharedBlockSound;
    static bool      _sharedResourcesLoaded;
};
