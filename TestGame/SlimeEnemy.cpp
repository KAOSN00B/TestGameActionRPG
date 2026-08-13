#include "SlimeEnemy.h"
#include "VirtualCanvas.h"
#include "AssetPaths.h"

#include <algorithm>

// ---- Static member definitions ----------------------------------------------
Texture2D SlimeEnemy::_sharedBigIdleAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedBigWalkAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedBigAttackAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedBigHurtAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedBigDeathAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedSmallIdleAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedSmallWalkAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedSmallAttackAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedSmallHurtAnim[SlimeEnemy::kVariantCount]{};
Texture2D SlimeEnemy::_sharedSmallDeathAnim[SlimeEnemy::kVariantCount]{};
Sound     SlimeEnemy::_sharedAttackSound{};
Sound     SlimeEnemy::_sharedHurtSound{};
Sound     SlimeEnemy::_sharedDeathSound{};
bool      SlimeEnemy::_sharedResourcesLoaded = false;

namespace
{
    // Both slime strips are stitched 6-frame rows: Big = 48x48, Small = 32x32.
    constexpr float kBigSlimeFrameWidth   = 48.f;
    constexpr float kSmallSlimeFrameWidth = 32.f;

    // Tier escalation: blue -> green -> gold -> red.
    // Pack letters: A blue, D green, C gold, B red.
    const char* kSlimeVariantSuffixes[4] = { "", "_D", "_C", "_B" };
}

// =============================================================================
SlimeEnemy::SlimeEnemy(Vector2 pos, SlimeSize size)
    : Enemy(pos)
    , _size(size)
{
}

SlimeEnemy::~SlimeEnemy() {}

// =============================================================================
void SlimeEnemy::Init()
{
    EnsureSharedResourcesLoaded();

    _attackSound = _sharedAttackSound;
    _hurtSound   = _sharedHurtSound;
    _deathSound  = _sharedDeathSound;
    SetVariantTier(_variantTier);

    ResetForSpawn(_worldPos);
}

void SlimeEnemy::SetVariantTier(int tier)
{
    EnsureSharedResourcesLoaded();

    _variantTier = (tier < 0) ? 0 : (tier >= kVariantCount) ? kVariantCount - 1 : tier;

    if (_size == SlimeSize::Big)
    {
        _idleAnim       = _sharedBigIdleAnim[_variantTier];
        _walkAnim       = _sharedBigWalkAnim[_variantTier];
        _attackAnim     = _sharedBigAttackAnim[_variantTier];
        _takeDamageAnim = _sharedBigHurtAnim[_variantTier];
        _deathAnim      = _sharedBigDeathAnim[_variantTier];
    }
    else
    {
        _idleAnim       = _sharedSmallIdleAnim[_variantTier];
        _walkAnim       = _sharedSmallWalkAnim[_variantTier];
        _attackAnim     = _sharedSmallAttackAnim[_variantTier];
        _takeDamageAnim = _sharedSmallHurtAnim[_variantTier];
        _deathAnim      = _sharedSmallDeathAnim[_variantTier];
    }

    // Repoint the live texture so an already-spawned slime swaps immediately.
    // _width is 0 until the first ResetForSpawn, which recomputes this anyway.
    _texture = _idleAnim;
    _height  = _texture.height;
    if (_width > 0.f)
        _maxFrames = (int)(_texture.width / _width);
}

// =============================================================================
void SlimeEnemy::ResetForSpawn(Vector2 pos)
{
    // Base reset gives us all the shared grunt behaviour state; then the slime
    // overrides the sprite metrics and stats for its size class.
    Enemy::ResetForSpawn(pos);

    if (_size == SlimeSize::Big)
    {
        _width  = kBigSlimeFrameWidth;
        _scale  = 5.2f;
        _speed  = 135.f;
        _health = 5.f;
        _maxHealth = 5.f;
        _attackPower = 1.f;
        _expValue = 3;
        // Big slime body is wide — larger capsule so it feels solid.
        _capsuleRadius     = 34.f;
        _capsuleHalfHeight = 0.f;
        _capsuleOffset     = { 0.f, 10.f };
    }
    else
    {
        _width  = kSmallSlimeFrameWidth;
        _scale  = 4.6f;
        _speed  = 265.f;
        _health = 1.f;
        _maxHealth = 1.f;
        _attackPower = 1.f;
        _expValue = 1;
        _capsuleRadius     = 20.f;
        _capsuleHalfHeight = 0.f;
        _capsuleOffset     = { 0.f, 6.f };
        // Small slimes bite at close range with a small hit box.
        _attackRange       = 80.f;
        _attackBoxWidth    = 36.f;
        _attackBoxHeight   = 50.f;
        _attackBoxOffsetX  = 36.f;
        // Fast, one-HP, and swarms the player (see class doc comment) — the
        // clearest "fragile swarm-profile" candidate in the roster for
        // BuildEngagementAssignments' one-extra-committer bonus slot in swarm
        // encounters (Balance::Rhythm::kSwarmExtraCommitters). Big slimes are
        // tankier space-takers, not fragile, so they stay unmarked.
        SetSwarmProfile(true);
    }

    _texture   = _idleAnim;
    _height    = _texture.height;
    _maxFrames = (int)(_texture.width / _width);
    _frame     = GetRandomValue(0, _maxFrames - 1);

    // Character Animator overrides (scale, hitboxes, anim speeds) win last.
    // Enemy::ResetForSpawn above already cleared the tuned-collision state.
    ApplyStoredTuning();
}

// =============================================================================
Rectangle SlimeEnemy::GetCollisionRec() const
{
    Rectangle animBodyRect;
    if (GetAnimBodyRectWorld(animBodyRect))
        return animBodyRect;
    if (_hasTunedCollision)
        return GetTunedCollisionRec();

    // Solid rect centred on the body, sitting slightly low like the sprite.
    float rectWidth  = (_size == SlimeSize::Big) ? 110.f : 64.f;
    float rectHeight = (_size == SlimeSize::Big) ?  78.f : 48.f;
    float yOffset    = (_size == SlimeSize::Big) ?  14.f : 10.f;

    return Rectangle{
        _worldPos.x - rectWidth  * 0.5f,
        _worldPos.y - rectHeight * 0.5f + yOffset,
        rectWidth,
        rectHeight
    };
}

// =============================================================================
void SlimeEnemy::SetWaveScale(int wave)
{
    // Fixed base profile per size; global growth comes from ApplyEnemyPowerLevel.
    (void)wave;
    if (_size == SlimeSize::Big)
    {
        _expValue    = 3;
        _health      = 5.f;
        _maxHealth   = 5.f;
        _speed       = 135.f;
        _attackPower = 1.f;
    }
    else
    {
        _expValue    = 1;
        _health      = 1.f;
        _maxHealth   = 1.f;
        _speed       = 265.f;
        _attackPower = 1.f;
    }
}

// =============================================================================
void SlimeEnemy::PlayAttackSound()
{
    float pitch = GetRandomValue(_size == SlimeSize::Big ? 70 : 130,
                                 _size == SlimeSize::Big ? 95 : 170) / 100.f;
    SetSoundPitch(_attackSound, pitch);
    SetSoundVolume(_attackSound, 0.6f);
    PlaySound(_attackSound);
}

// =============================================================================
void SlimeEnemy::EnsureSharedResourcesLoaded()
{
    if (_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        const char* suffix = kSlimeVariantSuffixes[variant];
        _sharedBigIdleAnim[variant]     = LoadTexture(AssetPath(TextFormat("Enemy/SlimeBigIdle%s.png",     suffix)).c_str());
        _sharedBigWalkAnim[variant]     = LoadTexture(AssetPath(TextFormat("Enemy/SlimeBigWalk%s.png",     suffix)).c_str());
        _sharedBigAttackAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/SlimeBigAttack%s.png",   suffix)).c_str());
        _sharedBigHurtAnim[variant]     = LoadTexture(AssetPath(TextFormat("Enemy/SlimeBigHurt%s.png",     suffix)).c_str());
        _sharedBigDeathAnim[variant]    = LoadTexture(AssetPath(TextFormat("Enemy/SlimeBigDeath%s.png",    suffix)).c_str());
        _sharedSmallIdleAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/SlimeSmallIdle%s.png",   suffix)).c_str());
        _sharedSmallWalkAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/SlimeSmallWalk%s.png",   suffix)).c_str());
        _sharedSmallAttackAnim[variant] = LoadTexture(AssetPath(TextFormat("Enemy/SlimeSmallAttack%s.png", suffix)).c_str());
        _sharedSmallHurtAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/SlimeSmallHurt%s.png",   suffix)).c_str());
        _sharedSmallDeathAnim[variant]  = LoadTexture(AssetPath(TextFormat("Enemy/SlimeSmallDeath%s.png",  suffix)).c_str());
    }
    _sharedAttackSound = LoadSound(AssetPath("Sounds/SwordSwipe2.ogg").c_str());
    _sharedHurtSound   = LoadSound(AssetPath("Sounds/SmallMonsterDamage.ogg").c_str());
    _sharedDeathSound  = LoadSound(AssetPath("Sounds/PlayerDeath.ogg").c_str());
    _sharedResourcesLoaded = true;
}

void SlimeEnemy::UnloadSharedResources()
{
    if (!_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        UnloadTexture(_sharedBigIdleAnim[variant]);
        UnloadTexture(_sharedBigWalkAnim[variant]);
        UnloadTexture(_sharedBigAttackAnim[variant]);
        UnloadTexture(_sharedBigHurtAnim[variant]);
        UnloadTexture(_sharedBigDeathAnim[variant]);
        UnloadTexture(_sharedSmallIdleAnim[variant]);
        UnloadTexture(_sharedSmallWalkAnim[variant]);
        UnloadTexture(_sharedSmallAttackAnim[variant]);
        UnloadTexture(_sharedSmallHurtAnim[variant]);
        UnloadTexture(_sharedSmallDeathAnim[variant]);
        _sharedBigIdleAnim[variant]     = Texture2D{};
        _sharedBigWalkAnim[variant]     = Texture2D{};
        _sharedBigAttackAnim[variant]   = Texture2D{};
        _sharedBigHurtAnim[variant]     = Texture2D{};
        _sharedBigDeathAnim[variant]    = Texture2D{};
        _sharedSmallIdleAnim[variant]   = Texture2D{};
        _sharedSmallWalkAnim[variant]   = Texture2D{};
        _sharedSmallAttackAnim[variant] = Texture2D{};
        _sharedSmallHurtAnim[variant]   = Texture2D{};
        _sharedSmallDeathAnim[variant]  = Texture2D{};
    }
    UnloadSound(_sharedAttackSound);
    UnloadSound(_sharedHurtSound);
    UnloadSound(_sharedDeathSound);
    _sharedAttackSound = Sound{};
    _sharedHurtSound   = Sound{};
    _sharedDeathSound  = Sound{};
    _sharedResourcesLoaded = false;
}
