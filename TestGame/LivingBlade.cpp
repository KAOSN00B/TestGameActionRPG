#include "LivingBlade.h"
#include "VirtualCanvas.h"
#include "AssetPaths.h"
#include "Character.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>

Texture2D LivingBlade::_sharedIdleAnim[LivingBlade::kVariantCount]{};
Texture2D LivingBlade::_sharedWalkAnim[LivingBlade::kVariantCount]{};
Texture2D LivingBlade::_sharedAttackAnim[LivingBlade::kVariantCount]{};
Texture2D LivingBlade::_sharedHurtAnim[LivingBlade::kVariantCount]{};
Texture2D LivingBlade::_sharedDeathAnim[LivingBlade::kVariantCount]{};
Sound     LivingBlade::_sharedDashSound{};
Sound     LivingBlade::_sharedHurtSound{};
Sound     LivingBlade::_sharedDeathSound{};
bool      LivingBlade::_sharedResourcesLoaded = false;

namespace
{
    constexpr float kBladeFrameWidth = 48.f;
    // Tier ladder: rusted -> shadow -> blood (pack letters A, C, D).
    const char* kBladeVariantSuffixes[3] = { "", "_C", "_D" };
}

LivingBlade::LivingBlade(Vector2 pos)
    : Enemy(pos)
{
}

LivingBlade::~LivingBlade() {}

void LivingBlade::Init()
{
    EnsureSharedResourcesLoaded();

    _attackSound = _sharedDashSound;
    _hurtSound   = _sharedHurtSound;
    _deathSound  = _sharedDeathSound;
    SetVariantTier(_variantTier);

    ResetForSpawn(_worldPos);
}

void LivingBlade::SetVariantTier(int tier)
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

void LivingBlade::ResetForSpawn(Vector2 pos)
{
    Enemy::ResetForSpawn(pos);

    _width  = kBladeFrameWidth;
    _scale  = 4.2f;
    _speed  = kDashSpeed;
    _health = 2.f;
    _maxHealth = 2.f;
    _attackPower = 1.f;
    _expValue = 4;

    _capsuleRadius     = 20.f;
    _capsuleHalfHeight = 16.f;
    _capsuleOffset     = { 0.f, 0.f };

    _state = BladeState::Resting;
    _stateTimer = (float)GetRandomValue(20, 70) / 100.f;   // desync packs
    _dashDirection = Vector2{ 1.f, 0.f };
    _dashHitApplied = false;
    _spinAngle = 0.f;
    _wobbleTimer = (float)GetRandomValue(0, 628) / 100.f;

    _texture   = _idleAnim;
    _height    = _texture.height;
    _maxFrames = (int)(_texture.width / _width);
    _frame     = GetRandomValue(0, _maxFrames - 1);

    ApplyStoredTuning();
}

void LivingBlade::Update(float dt, Vector2 heroWorldPos, Vector2 /*navigationTarget*/, bool /*hasNavigationTarget*/,
    const std::vector<std::unique_ptr<Enemy>>& /*enemies*/,
    const std::vector<Vector2>& /*propCenters*/)
{
    if (!_isActive)
        return;

    _worldPosLastFrame = _worldPos;
    _wobbleTimer += dt;
    UpdateHit(dt);

    if (_forcedPushActive)
    {
        _worldPos = Vector2Add(_worldPos, Vector2Scale(_forcedPushDirection, _forcedPushSpeed * dt));
        return;
    }

    ApplyVelocity(dt);
    UpdateBurns(dt);
    UpdateElectricCharge(dt);
    UpdateLaunchVisual(dt);

    if (_freezeTimer > 0.f)
        _freezeTimer -= dt;

    if (!_dying && _target != nullptr)
    {
        bool controlled = IsFrozen() || IsElectroStunned() || _takingDamage;
        _stateTimer -= dt;

        switch (_state)
        {
        case BladeState::Resting:
            if (_stateTimer <= 0.f && !controlled)
            {
                // "Fast pass-through commitments followed by a clear recovery
                // and separation phase." (design doc) — a dash is itself a
                // Commit, so it now respects the shared commit-slot budget:
                // only wind up once this Living Blade actually holds this
                // frame's Commit slot. Otherwise it holds its rest a beat
                // longer and re-checks next frame rather than falling back to
                // direct homing (it was already standing still, so this reads
                // as a natural extra beat, not a stall).
                bool canCommitDash = !HasEngagementAssignment() || GetEngagementIntent() == EngagementIntent::Commit;
                if (canCommitDash)
                {
                    _state = BladeState::WindingUp;
                    _stateTimer = kWindupDuration;
                    _engagementLatch.BeginCommit();
                }
            }
            break;

        case BladeState::WindingUp:
            if (controlled)
            {
                _state = BladeState::Resting;
                _stateTimer = kRestDuration;
                _engagementLatch.EndCommit();
                break;
            }
            if (_stateTimer <= 0.f)
            {
                // Lock the dash line with a bit of aim scatter — erratic by design.
                Vector2 toPlayer = Vector2Subtract(_target->GetFeetWorldPos(), _worldPos);
                float baseAngle = atan2f(toPlayer.y, toPlayer.x);
                baseAngle += (float)GetRandomValue(-100, 100) / 100.f * kAimScatter;
                _dashDirection = Vector2{ cosf(baseAngle), sinf(baseAngle) };
                _state = BladeState::Dashing;
                _stateTimer = kDashDuration;
                _dashHitApplied = false;
                PlayAttackSound();
            }
            break;

        case BladeState::Dashing:
        {
            float dashSpeed = HasWarAura() ? kDashSpeed * kWarAuraSpeedMultiplier : kDashSpeed;
            _worldPos = Vector2Add(_worldPos, Vector2Scale(_dashDirection, dashSpeed * dt));
            _spinAngle += 1400.f * dt;

            // Slices anything it passes through — once per dash.
            if (!_dashHitApplied && _target->IsAlive() &&
                CheckCollisionRecs(GetCollisionRec(), _target->GetCollisionRec()))
            {
                _dashHitApplied = true;
                _target->TakeDamage((int)_attackPower, _worldPos);
            }

            // Stay inside the arena.
            _worldPos.x = std::clamp(_worldPos.x, 90.f, (float)kVirtualWidth  - 90.f);
            _worldPos.y = std::clamp(_worldPos.y, 90.f, (float)kVirtualHeight - 90.f);

            if (_stateTimer <= 0.f)
            {
                _state = BladeState::Resting;
                _stateTimer = kRestDuration;
                _spinAngle = 0.f;
                // Pass-through complete — release the Commit slot into its
                // post-attack recovery window (Balance::Rhythm::
                // kPostCommitRepositionSeconds) so it can't immediately wind
                // up again ahead of other enemies waiting on the budget.
                _engagementLatch.EndCommit();
            }
            break;
        }
        }

        if (_dashDirection.x < -0.1f) _rightLeft = -1.f;
        if (_dashDirection.x >  0.1f) _rightLeft =  1.f;
    }

    HandleAnimationLoop(dt);
}

void LivingBlade::HandleAnimationLoop(float dt)
{
    if (_launchHoldingHurtPose)
        return;

    _runningTime += dt;
    if (_runningTime >= _updateTime)
    {
        _runningTime = 0.f;
        _frame++;
        if (_frame >= _maxFrames)
        {
            if (_dying || IsFrozen())
            {
                _frame = _maxFrames - 1;
                return;
            }
            if (_takingDamage)
            {
                _takingDamage = false;
                _texture    = _idleAnim;
                _maxFrames  = (int)(_texture.width / _width);
                _updateTime = 1.f / 8.f;
            }
            _frame = 0;
        }
    }
}

void LivingBlade::DrawEnemy(Vector2 heroWorldPos)
{
    if (!_isActive)
        return;

    float drawWidth  = kBladeFrameWidth * _scale;
    float drawHeight = (_idleAnim.id > 0 ? (float)_idleAnim.height : _height) * _scale;

    // Rest wobble sells "a sword standing on its tip".
    float wobble = (_state == BladeState::Resting) ? sinf(_wobbleTimer * 9.f) * 5.f : 0.f;

    Vector2 screenPos = Vector2{ _worldPos.x - heroWorldPos.x + kVirtualWidth / 2.f,
                                 _worldPos.y - heroWorldPos.y + kVirtualHeight / 2.f };

    bool frozen = IsFrozen();
    Color tint = IsElectroStunned() ? Color{ 255, 255,  60, 255 } :
                 frozen             ? Color{ 140, 200, 255, 255 } :
                 !_pendingBurns.empty() ? Color{ 255, 180, 180, 255 } :
                                      WHITE;

    // Windup glint — the warning flash before the dash line is locked.
    if (_state == BladeState::WindingUp)
    {
        float pulse = sinf((float)GetTime() * 26.f) * 0.5f + 0.5f;
        DrawCircleV(screenPos, 20.f + pulse * 12.f, Fade(WHITE, 0.25f + 0.25f * pulse));
    }

    // Spin while dashing (rotation needs a centre origin, so this path draws
    // with DrawTexturePro rotation instead of the base corner-anchored draw).
    float rotation = (_state == BladeState::Dashing) ? _spinAngle : wobble;

    Vector2 animDrawOffset = GetCurrentAnimDrawOffset();
    Rectangle source{ _frame * _width, 0.f, _rightLeft * _width, _height };
    Rectangle dest{ screenPos.x + animDrawOffset.x, screenPos.y + animDrawOffset.y, drawWidth, drawHeight };
    DrawEllipse((int)(screenPos.x + animDrawOffset.x), (int)(screenPos.y + drawHeight * 0.48f + animDrawOffset.y),
        drawWidth * 0.20f, drawHeight * 0.05f, Fade(BLACK, 0.20f));
    DrawTexturePro(_texture, source, dest,
        Vector2{ drawWidth * 0.5f, drawHeight * 0.5f }, rotation, tint);

    // Dash trail.
    if (_state == BladeState::Dashing)
    {
        for (int i = 1; i <= 3; i++)
        {
            Vector2 ghost = Vector2Subtract(screenPos, Vector2Scale(_dashDirection, (float)i * 26.f));
            DrawCircleV(ghost, 12.f - i * 3.f, Fade(Color{ 200, 210, 230, 255 }, 0.22f - i * 0.05f));
        }
    }

    if (_health != _maxHealth)
        DrawHealthBar(screenPos, drawWidth, drawHeight);
    if (_isEliteMiniboss)
        DrawEliteLabel(screenPos, drawWidth, drawHeight);
}

Rectangle LivingBlade::GetCollisionRec() const
{
    Rectangle animBodyRect;
    if (GetAnimBodyRectWorld(animBodyRect))
        return animBodyRect;
    if (_hasTunedCollision)
        return GetTunedCollisionRec();

    return Rectangle{ _worldPos.x - 26.f, _worldPos.y - 44.f, 52.f, 88.f };
}

Capsule2D LivingBlade::GetCapsule() const
{
    Capsule2D animBodyCapsule;
    if (GetAnimBodyCapsuleWorld(animBodyCapsule))
        return animBodyCapsule;

    return Capsule2D{
        { _worldPos.x + _capsuleOffset.x, _worldPos.y + _capsuleOffset.y },
        (_capsuleHalfHeight > 0.f) ? _capsuleHalfHeight : 16.f,
        (_capsuleRadius > 0.f) ? _capsuleRadius : 20.f
    };
}

void LivingBlade::SetWaveScale(int wave)
{
    (void)wave;
    _expValue    = 4;
    _health      = 2.f;
    _maxHealth   = 2.f;
    _speed       = kDashSpeed;
    _attackPower = 1.f;
}

void LivingBlade::PlayAttackSound()
{
    float pitch = GetRandomValue(120, 165) / 100.f;
    SetSoundPitch(_attackSound, pitch);
    SetSoundVolume(_attackSound, 0.55f);
    PlaySound(_attackSound);
}

void LivingBlade::PlayDeathSound()
{
    // Possessed blade shatters on death — high-pitched metallic crack
    SfxBank::Get().Play(SfxId::DeathSmall, 0.65f, true);
}

void LivingBlade::EnsureSharedResourcesLoaded()
{
    if (_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        const char* suffix = kBladeVariantSuffixes[variant];
        _sharedIdleAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/LivingBladeIdle%s.png",   suffix)).c_str());
        _sharedWalkAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/LivingBladeWalk%s.png",   suffix)).c_str());
        _sharedAttackAnim[variant] = LoadTexture(AssetPath(TextFormat("Enemy/LivingBladeAttack%s.png", suffix)).c_str());
        _sharedHurtAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/LivingBladeHurt%s.png",   suffix)).c_str());
        _sharedDeathAnim[variant]  = LoadTexture(AssetPath(TextFormat("Enemy/LivingBladeDeath%s.png",  suffix)).c_str());
    }
    _sharedDashSound  = LoadSound(AssetPath("Sounds/SwordSwipe.ogg").c_str());
    _sharedHurtSound  = LoadSound(AssetPath("Sounds/SmallMonsterDamage.ogg").c_str());
    _sharedDeathSound = LoadSound(AssetPath("Sounds/PlayerDeath.ogg").c_str());
    _sharedResourcesLoaded = true;
}

void LivingBlade::UnloadSharedResources()
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
    UnloadSound(_sharedDashSound);
    UnloadSound(_sharedHurtSound);
    UnloadSound(_sharedDeathSound);
    _sharedDashSound  = Sound{};
    _sharedHurtSound  = Sound{};
    _sharedDeathSound = Sound{};
    _sharedResourcesLoaded = false;
}
