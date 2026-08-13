#pragma once

#include "Enemy.h"

// =============================================================================
// Sporeling — a waddling mushroom that is more dangerous dead than alive.
// Slow melee chaser reusing the base grunt behaviour; when it dies it bursts
// into a lingering poison cloud (spawned by the Engine via CombatDirector),
// punishing careless melee kills. Kill it with range or dodge out after the hit.
// Variants: red cap -> green cap -> purple cap.
// =============================================================================
class Sporeling : public Enemy
{
public:
    Sporeling(Vector2 pos);
    CreatureFamily GetCreatureFamily() const override { return CreatureFamily::Plant; }
    ~Sporeling() override;

    void Init();
    void ResetForSpawn(Vector2 pos) override;
    static void UnloadSharedResources();

    void SetWaveScale(int wave) override;
    void SetVariantTier(int tier) override;
    Rectangle GetCollisionRec() const override;

    Sporeling* AsSporeling() override { return this; }
    const char* GetTuningName() const override { return "Sporeling"; }

    // "Approaches indirectly ... does not lead the initial rush." (design doc)
    // A curved approach path reads as more deliberate/wary than a beeline.
    // Role deliberately stays the default Grunt (not reclassified to Zoner)
    // to avoid rippling into UsesAttackLunge()/squad steering — the "does not
    // lead the rush" flavour otherwise rides on the shared Reposition/hold
    // behaviour every non-committing enemy now gets (Enemy::HandleMovement).
    float GetApproachLateralBias() const override { return 0.35f; }

    void PlayAttackSound() override;

    // Radius of the poison cloud left on death (read by Engine when spawning it).
    static constexpr float kPoisonCloudRadius = 120.f;

private:
    static void EnsureSharedResourcesLoaded();

    int _variantTier = 0;   // 0 red, 1 green, 2 purple

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
