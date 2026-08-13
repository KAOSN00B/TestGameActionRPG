#include "BomberImp.h"
#include "VirtualCanvas.h"
#include "AssetPaths.h"
#include "Character.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>

Texture2D BomberImp::_sharedIdleAnim[BomberImp::kVariantCount]{};
Texture2D BomberImp::_sharedWalkAnim[BomberImp::kVariantCount]{};
Texture2D BomberImp::_sharedAttackAnim[BomberImp::kVariantCount]{};
Texture2D BomberImp::_sharedHurtAnim[BomberImp::kVariantCount]{};
Texture2D BomberImp::_sharedDeathAnim[BomberImp::kVariantCount]{};
Sound     BomberImp::_sharedFuseSound{};
Sound     BomberImp::_sharedHurtSound{};
Sound     BomberImp::_sharedBlastSound{};
bool      BomberImp::_sharedResourcesLoaded = false;

namespace
{
    constexpr float kImpFrameWidth = 48.f;
    // Tier ladder: pale -> red -> purple (pack letters A, B, C).
    const char* kImpVariantSuffixes[3] = { "", "_B", "_C" };
}

BomberImp::BomberImp(Vector2 pos)
    : Enemy(pos)
{
}

BomberImp::~BomberImp() {}

void BomberImp::Init()
{
    EnsureSharedResourcesLoaded();

    _hurtSound  = _sharedHurtSound;
    _deathSound = _sharedBlastSound;
    SetVariantTier(_variantTier);

    ResetForSpawn(_worldPos);
}

void BomberImp::SetVariantTier(int tier)
{
    EnsureSharedResourcesLoaded();

    _variantTier = (tier < 0) ? 0 : (tier >= kVariantCount) ? kVariantCount - 1 : tier;

    _idleAnim       = _sharedIdleAnim[_variantTier];
    _walkAnim       = _sharedWalkAnim[_variantTier];
    _attackAnim     = _sharedAttackAnim[_variantTier];
    _takeDamageAnim = _sharedHurtAnim[_variantTier];
    _deathAnim      = _sharedDeathAnim[_variantTier];

    _texture = _idleAnim;
    _height  = _texture.height;
    if (_width > 0.f)
        _maxFrames = (int)(_texture.width / _width);
}

void BomberImp::ResetForSpawn(Vector2 pos)
{
    Enemy::ResetForSpawn(pos);

    _width  = kImpFrameWidth;
    _scale  = 3.8f;
    _speed  = kMaxFlySpeed;
    _health = 1.f;         // one hit sets it off — the danger is WHERE it dies
    _maxHealth = 1.f;
    _attackPower = 2.f;    // blast damage
    _expValue = 3;

    _capsuleRadius     = 20.f;
    _capsuleHalfHeight = 0.f;
    _capsuleOffset     = { 0.f, 0.f };

    _state = ImpState::Seeking;
    _fuseTimer = 0.f;
    _detonateTimer = 0.f;
    _blastDamageApplied = false;
    _impactShakeRequested = false;
    _flyVelocity = Vector2Zero();
    _bobTimer = (float)GetRandomValue(0, 628) / 100.f;

    _texture   = _idleAnim;
    _height    = _texture.height;
    _maxFrames = (int)(_texture.width / _width);
    _frame     = GetRandomValue(0, _maxFrames - 1);

    ApplyStoredTuning();
}

void BomberImp::Update(float dt, Vector2 heroWorldPos, Vector2 /*navigationTarget*/, bool /*hasNavigationTarget*/,
    const std::vector<std::unique_ptr<Enemy>>& /*enemies*/,
    const std::vector<Vector2>& /*propCenters*/)
{
    if (!_isActive)
        return;

    _worldPosLastFrame = _worldPos;
    _bobTimer += dt;

    ApplyVelocity(dt);
    UpdateHit(dt);
    UpdateBurns(dt);
    UpdateElectricCharge(dt);

    if (_freezeTimer > 0.f)
        _freezeTimer -= dt;

    if (_dying)
    {
        HandleAnimationLoop(dt);
        return;
    }
    if (_target == nullptr)
        return;

    bool controlled = IsFrozen() || IsElectroStunned();
    Vector2 toPlayer = Vector2Subtract(heroWorldPos, _worldPos);
    float dist = Vector2Length(toPlayer);

    switch (_state)
    {
    case ImpState::Seeking:
    {
        if (!controlled)
        {
            // "Searches for a useful throw lane, commits to a bomb action,
            // then relocates." (design doc) — while not holding this frame's
            // engagement Commit slot, accelerate toward the shared lane point
            // instead of the player directly, so a non-committing imp reads
            // as searching for an angle rather than beelining in lockstep
            // with every other imp. The fuse trigger below always still uses
            // real distance to the player (see `dist`) — an imp that isn't
            // "due" for its bomb run is still dangerous at point-blank range.
            Vector2 seekAnchor = heroWorldPos;
            if (HasEngagementAssignment() && GetEngagementIntent() != EngagementIntent::Commit)
                seekAnchor = GetEngagementTarget();
            Vector2 toAnchor = Vector2Subtract(seekAnchor, _worldPos);
            float distToAnchor = Vector2Length(toAnchor);

            // Accelerating pursuit with a wavy flight path.
            Vector2 desired = (distToAnchor > 0.01f) ? Vector2Scale(toAnchor, 1.f / distToAnchor) : Vector2{ 1.f, 0.f };
            float accel = HasWarAura() ? kAcceleration * kWarAuraSpeedMultiplier : kAcceleration;
            _flyVelocity = Vector2Add(_flyVelocity, Vector2Scale(desired, accel * dt));
            float maxSpeed = HasWarAura() ? kMaxFlySpeed * kWarAuraSpeedMultiplier : kMaxFlySpeed;
            if (Vector2Length(_flyVelocity) > maxSpeed)
                _flyVelocity = Vector2Scale(Vector2Normalize(_flyVelocity), maxSpeed);

            Vector2 step = Vector2Scale(_flyVelocity, dt);
            step.y += sinf(_bobTimer * 5.f) * 30.f * dt;
            _worldPos = Vector2Add(_worldPos, step);
        }

        if (dist < kFuseDistance)
        {
            _state = ImpState::Fusing;
            _fuseTimer = kFuseDuration;
            SetSoundVolume(_sharedFuseSound, 0.6f);
            PlaySound(_sharedFuseSound);
        }
        break;
    }

    case ImpState::Fusing:
        // Fuse is lit — it keeps coasting on its last velocity, so a dash
        // sideways escapes the blast.
        if (!controlled)
            _worldPos = Vector2Add(_worldPos, Vector2Scale(_flyVelocity, dt * 0.4f));
        _fuseTimer -= dt;
        if (_fuseTimer <= 0.f)
            BeginDetonation();
        break;

    case ImpState::Detonating:
        _detonateTimer -= dt;
        if (!_blastDamageApplied)
        {
            _blastDamageApplied = true;
            if (_target->IsAlive() &&
                Vector2Distance(_worldPos, _target->GetFeetWorldPos()) < kBlastRadius)
            {
                _target->TakeDamage((int)_attackPower, _worldPos);
                Vector2 away = Vector2Subtract(_target->GetWorldPos(), _worldPos);
                if (Vector2Length(away) > 0.01f)
                    _target->StartForcedPush(Vector2Normalize(away), 1200.f);
            }
        }
        if (_detonateTimer <= 0.f)
        {
            // The blast consumed it — skip the normal death animation flow.
            _health   = 0.f;
            _isActive = false;
            Teleport(Vector2{ -5000.f, -5000.f });
        }
        return;   // no facing/animation updates while exploding
    }

    float dx = heroWorldPos.x - _worldPos.x;
    if      (dx < -14.f) _rightLeft = -1.f;
    else if (dx >  14.f) _rightLeft =  1.f;

    HandleAnimationLoop(dt);
}

void BomberImp::BeginDetonation()
{
    if (_state == ImpState::Detonating)
        return;

    _state = ImpState::Detonating;
    _detonateTimer = kDetonateDuration;
    _blastDamageApplied = false;
    _impactShakeRequested = true;
    SetSoundVolume(_sharedBlastSound, 0.8f);
    PlaySound(_sharedBlastSound);
}

void BomberImp::TakeDamage(int damage, Vector2 attackerPos)
{
    (void)damage;
    (void)attackerPos;

    if (_dying || _state == ImpState::Detonating)
        return;

    // Any hit sets it off right where it floats — range kills are safe,
    // point-blank sword kills are not.
    BeginDetonation();
}

bool BomberImp::ConsumeImpactShakeRequest()
{
    bool requested = _impactShakeRequested;
    _impactShakeRequested = false;
    return requested;
}

void BomberImp::HandleAnimationLoop(float dt)
{
    _runningTime += dt;
    if (_runningTime >= _updateTime)
    {
        _runningTime = 0.f;
        _frame = (_frame + 1) % std::max(1, _maxFrames);
    }
}

void BomberImp::DrawEnemy(Vector2 heroWorldPos)
{
    if (!_isActive)
        return;

    Vector2 screenPos = Vector2{ _worldPos.x - heroWorldPos.x + kVirtualWidth / 2.f,
                                 _worldPos.y - heroWorldPos.y + kVirtualHeight / 2.f + sinf(_bobTimer * 5.f) * 5.f };

    // Detonation: expanding blast circles instead of the sprite.
    if (_state == ImpState::Detonating)
    {
        float t = 1.f - (_detonateTimer / kDetonateDuration);
        float radius = kBlastRadius * (0.3f + t * 0.7f);
        DrawCircleV(screenPos, radius,          Fade(Color{ 255, 140,  40, 255 }, 0.55f * (1.f - t)));
        DrawCircleV(screenPos, radius * 0.65f,  Fade(Color{ 255, 220,  90, 255 }, 0.65f * (1.f - t)));
        DrawCircleV(screenPos, radius * 0.32f,  Fade(WHITE, 0.8f * (1.f - t)));
        return;
    }

    float drawWidth  = kImpFrameWidth * _scale;
    float drawHeight = (_idleAnim.id > 0 ? (float)_idleAnim.height : _height) * _scale;

    bool frozen = IsFrozen();
    Color tint = IsElectroStunned() ? Color{ 255, 255,  60, 255 } :
                 frozen             ? Color{ 140, 200, 255, 255 } :
                                      WHITE;

    // Fusing: rapid red flash — unmistakable "get away" signal.
    if (_state == ImpState::Fusing)
    {
        float pulse = sinf((float)GetTime() * 30.f) * 0.5f + 0.5f;
        tint = (pulse > 0.5f) ? Color{ 255, 90, 60, 255 } : WHITE;
        DrawCircleLines((int)screenPos.x, (int)screenPos.y, kBlastRadius,
            Fade(Color{ 255, 120, 60, 255 }, 0.35f + 0.3f * pulse));
    }

    Vector2 animDrawOffset = GetCurrentAnimDrawOffset();
    Rectangle source{ _frame * _width, 0.f, _rightLeft * _width, _height };
    Rectangle dest{ screenPos.x - drawWidth / 2.f + animDrawOffset.x,
                    screenPos.y - drawHeight / 2.f + animDrawOffset.y, drawWidth, drawHeight };
    DrawEllipse((int)(screenPos.x + animDrawOffset.x),
        (int)(screenPos.y - sinf(_bobTimer * 5.f) * 5.f + drawHeight * 0.50f + animDrawOffset.y),
        drawWidth * 0.24f, drawHeight * 0.055f, Fade(BLACK, 0.20f));
    DrawTexturePro(_texture, source, dest, Vector2{}, 0.f, tint);

    if (_isEliteMiniboss)
        DrawEliteLabel(screenPos, drawWidth, drawHeight);
}

Rectangle BomberImp::GetCollisionRec() const
{
    Rectangle animBodyRect;
    if (GetAnimBodyRectWorld(animBodyRect))
        return animBodyRect;
    if (_hasTunedCollision)
        return GetTunedCollisionRec();

    return Rectangle{ _worldPos.x - 30.f, _worldPos.y - 30.f, 60.f, 60.f };
}

Capsule2D BomberImp::GetCapsule() const
{
    Capsule2D animBodyCapsule;
    if (GetAnimBodyCapsuleWorld(animBodyCapsule))
        return animBodyCapsule;

    return Capsule2D{
        { _worldPos.x + _capsuleOffset.x, _worldPos.y + _capsuleOffset.y },
        0.f,
        (_capsuleRadius > 0.f) ? _capsuleRadius : 20.f
    };
}

void BomberImp::SetWaveScale(int wave)
{
    (void)wave;
    _expValue    = 3;
    _health      = 1.f;
    _maxHealth   = 1.f;
    _speed       = kMaxFlySpeed;
    _attackPower = 2.f;
}

void BomberImp::EnsureSharedResourcesLoaded()
{
    if (_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        const char* suffix = kImpVariantSuffixes[variant];
        _sharedIdleAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/BomberImpIdle%s.png",   suffix)).c_str());
        _sharedWalkAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/BomberImpWalk%s.png",   suffix)).c_str());
        _sharedAttackAnim[variant] = LoadTexture(AssetPath(TextFormat("Enemy/BomberImpAttack%s.png", suffix)).c_str());
        _sharedHurtAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/BomberImpHurt%s.png",   suffix)).c_str());
        _sharedDeathAnim[variant]  = LoadTexture(AssetPath(TextFormat("Enemy/BomberImpDeath%s.png",  suffix)).c_str());
    }
    _sharedFuseSound  = LoadSound(AssetPath("Sounds/LightningSound.ogg").c_str());
    _sharedHurtSound  = LoadSound(AssetPath("Sounds/SmallMonsterDamage.ogg").c_str());
    _sharedBlastSound = LoadSound(AssetPath("Sounds/Explosion.ogg").c_str());
    _sharedResourcesLoaded = true;
}

void BomberImp::UnloadSharedResources()
{
    if (!_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        UnloadTexture(_sharedIdleAnim[variant]);
        UnloadTexture(_sharedWalkAnim[variant]);
        UnloadTexture(_sharedAttackAnim[variant]);
        UnloadTexture(_sharedHurtAnim[variant]);
        UnloadTexture(_sharedDeathAnim[variant]);
        _sharedIdleAnim[variant]   = Texture2D{};
        _sharedWalkAnim[variant]   = Texture2D{};
        _sharedAttackAnim[variant] = Texture2D{};
        _sharedHurtAnim[variant]   = Texture2D{};
        _sharedDeathAnim[variant]  = Texture2D{};
    }
    UnloadSound(_sharedFuseSound);
    UnloadSound(_sharedHurtSound);
    UnloadSound(_sharedBlastSound);
    _sharedFuseSound  = Sound{};
    _sharedHurtSound  = Sound{};
    _sharedBlastSound = Sound{};
    _sharedResourcesLoaded = false;
}
