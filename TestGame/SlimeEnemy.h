#pragma once

#include "Enemy.h"
#include <vector>
#include <memory>

// =============================================================================
// SlimeEnemy — classic splitting slime in two sizes.
// Big: slow, tanky, lunges like a regular grunt. When it dies it splits into
//      two Small slimes (spawned by CombatDirector via the death context).
// Small: fast, fragile, swarms the player.
// Movement, attacks, and pathfinding all reuse the base Enemy logic — only the
// sprites, stats, and the split-on-death flag differ.
// =============================================================================
enum class SlimeSize { Big, Small };

class SlimeEnemy : public Enemy
{
public:
    SlimeEnemy(Vector2 pos, SlimeSize size);
    ~SlimeEnemy() override;

    void Init();
    void ResetForSpawn(Vector2 pos) override;
    static void UnloadSharedResources();

    void SetWaveScale(int wave) override;
    void SetVariantTier(int tier) override;

    // The base Enemy collision math assumes 32px grunt frames; the big slime
    // uses 48px frames, so both sizes compute their own centred rect here.
    Rectangle GetCollisionRec() const override;

    SlimeEnemy* AsSlime() override { return this; }
    const char* GetTuningName() const override { return IsBig() ? "SlimeBig" : "SlimeSmall"; }
    CreatureFamily GetCreatureFamily() const override { return CreatureFamily::Slime; }
    SlimeSize   GetSize() const { return _size; }
    bool        IsBig()   const { return _size == SlimeSize::Big; }

    // "Advances slowly and directly as a space-taker. Once committed, it
    // preserves its chosen line rather than continuously tracking every
    // player movement." (design doc) — see Enemy::LocksApproachLineOnCommit.
    bool LocksApproachLineOnCommit() const override { return true; }

    void PlayAttackSound() override;

private:
    static void EnsureSharedResourcesLoaded();

    SlimeSize _size = SlimeSize::Big;
    int _variantTier = 0;   // 0 blue, 1 green, 2 gold, 3 red

    // Big and small sizes use separate stitched strips; each size has one
    // texture set per colour-variant tier.
    static constexpr int kVariantCount = 4;
    static Texture2D _sharedBigIdleAnim[kVariantCount];
    static Texture2D _sharedBigWalkAnim[kVariantCount];
    static Texture2D _sharedBigAttackAnim[kVariantCount];
    static Texture2D _sharedBigHurtAnim[kVariantCount];
    static Texture2D _sharedBigDeathAnim[kVariantCount];
    static Texture2D _sharedSmallIdleAnim[kVariantCount];
    static Texture2D _sharedSmallWalkAnim[kVariantCount];
    static Texture2D _sharedSmallAttackAnim[kVariantCount];
    static Texture2D _sharedSmallHurtAnim[kVariantCount];
    static Texture2D _sharedSmallDeathAnim[kVariantCount];
    static Sound     _sharedAttackSound;
    static Sound     _sharedHurtSound;
    static Sound     _sharedDeathSound;
    static bool      _sharedResourcesLoaded;
};
