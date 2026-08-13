#pragma once

#include "Enemy.h"

// =============================================================================
// Warchief — an orc commander whose war banner drives nearby enemies into a
// frenzy: every ally inside the aura ring moves 30% faster. It fights like a
// standard bruiser itself, but leaving it alive turns every room into a
// stampede — priority target number one.
// Variants: brown -> black -> red.
// =============================================================================
class Warchief : public Enemy
{
public:
    Warchief(Vector2 pos);
    CreatureFamily GetCreatureFamily() const override { return CreatureFamily::Beast; }
    ~Warchief() override;

    void Init();
    void ResetForSpawn(Vector2 pos) override;
    static void UnloadSharedResources();

    void Update(float dt, Vector2 heroWorldPos, Vector2 navigationTarget, bool hasNavigationTarget,
        const std::vector<std::unique_ptr<Enemy>>& enemies,
        const std::vector<Vector2>& propCenters) override;
    void SetWaveScale(int wave) override;
    void SetVariantTier(int tier) override;
    void DrawEnemy(Vector2 heroWorldPos) override;
    Rectangle GetCollisionRec() const override;

    Warchief* AsWarchief() override { return this; }
    // Support feeds the shared CombatEngagement anchoring logic (Task 3):
    // ComputeEngagementTarget hovers a non-Tank Support intent near the ally
    // centroid rather than the player — "anchors near the ally centroid and
    // avoids becoming the nearest enemy unless isolated" (design doc). It
    // only pursues/attacks directly once BuildEngagementAssignments' rank-by-
    // distance selection actually grants it a Commit slot (i.e. it becomes
    // the closest/most eligible candidate — "unless isolated"). No positioning
    // code of its own needed; it calls base Enemy::Update() unchanged below.
    EnemyRole GetEncounterRole() const override { return EnemyRole::Support; }
    int       GetSpawnCost()     const override { return 4; }
    const char* GetTuningName() const override { return "Warchief"; }

    void PlayAttackSound() override;

    static constexpr float kAuraRadius = 380.f;

private:
    static void EnsureSharedResourcesLoaded();

    int _variantTier = 0;   // 0 brown, 1 black, 2 red

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
