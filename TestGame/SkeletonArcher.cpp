#include "SkeletonArcher.h"
#include "VirtualCanvas.h"
#include "AssetPaths.h"
#include "raymath.h"
#include <algorithm>

// ---- Static member definitions ----------------------------------------------
Texture2D SkeletonArcher::_sharedIdleAnim[SkeletonArcher::kVariantCount]{};
Texture2D SkeletonArcher::_sharedWalkAnim[SkeletonArcher::kVariantCount]{};
Texture2D SkeletonArcher::_sharedAttackAnim[SkeletonArcher::kVariantCount]{};
Texture2D SkeletonArcher::_sharedTakeDamageAnim[SkeletonArcher::kVariantCount]{};
Texture2D SkeletonArcher::_sharedDeathAnim[SkeletonArcher::kVariantCount]{};
Sound     SkeletonArcher::_sharedAttackSound{};
Sound     SkeletonArcher::_sharedHurtSound{};
Sound     SkeletonArcher::_sharedDeathSound{};
bool      SkeletonArcher::_sharedResourcesLoaded = false;

namespace
{
    // Tier escalation: bone-white -> gold -> onyx -> flaming red.
    // "" is the original unsuffixed A-variant strip set.
    const char* kArcherVariantSuffixes[4] = { "", "_B", "_C", "_D" };
}

namespace
{
    // All archer sheets are stitched 6-frame strips of 48x48 pixels.
    constexpr int   kArcherFrameCount = 6;
    constexpr float kArcherFrameWidth = 48.f;
    constexpr float kArcherIdleFrameTime = 1.f / 8.f;
    constexpr float kArcherWalkFrameTime = 1.f / 10.f;
}

// =============================================================================
SkeletonArcher::SkeletonArcher(Vector2 pos)
    : Enemy(pos)
{
}

SkeletonArcher::~SkeletonArcher() {}

// =============================================================================
void SkeletonArcher::Init()
{
    EnsureSharedResourcesLoaded();

    _attackSound = _sharedAttackSound;
    _hurtSound   = _sharedHurtSound;
    _deathSound  = _sharedDeathSound;
    SetVariantTier(_variantTier);

    ResetForSpawn(_worldPos);
}

void SkeletonArcher::SetVariantTier(int tier)
{
    EnsureSharedResourcesLoaded();

    _variantTier = (tier < 0) ? 0 : (tier >= kVariantCount) ? kVariantCount - 1 : tier;

    _idleAnim       = _sharedIdleAnim[_variantTier];
    _walkAnim       = _sharedWalkAnim[_variantTier];
    _attackAnim     = _sharedAttackAnim[_variantTier];
    _takeDamageAnim = _sharedTakeDamageAnim[_variantTier];
    _deathAnim      = _sharedDeathAnim[_variantTier];

    // Repoint the live texture so an already-spawned archer swaps immediately.
    SetIdleAnimation(false);
}

// =============================================================================
void SkeletonArcher::ResetForSpawn(Vector2 pos)
{
    _worldPos          = pos;
    _worldPosLastFrame = pos;
    _homePos           = pos;
    _velocity          = Vector2Zero();
    _isActive          = true;

    SetIdleAnimation(false);
    _scale = 4.4f;

    _health      = 2.f;
    _maxHealth   = 2.f;
    _attackPower = 1.f;
    _speed       = 210.f;
    _expValue    = 4;   // ranged threat, more than a basic grunt

    _frame       = GetRandomValue(0, _maxFrames - 1);
    _runningTime = GetRandomValue(0, 200) / 100.f * _updateTime;

    _hitTimer                 = 0.f;
    _deathTimer               = 0.4f;
    _freezeTimer              = 0.f;
    _isCharged                = false;
    _chargeNextStunTime       = 0.f;
    _electricChargeTotalTimer = 0.f;
    _isEliteMiniboss          = false;
    _isInvulnerable           = false;
    _leapInvulnerable         = false;
    _takingDamage = false;
    _attacking    = false;
    _dying        = false;

    _stuckTimer    = 0.f;
    _stuckCheckPos = _worldPos;

    _drawTimer     = 0.f;
    _shotCooldown  = (float)GetRandomValue(60, 160) / 100.f;   // stagger opening shots
    _drawingBow    = false;
    _wantsToFire   = false;
    _relentlessFire = false;
    _fireDirection = Vector2Zero();
    _strafeSign    = (GetRandomValue(0, 1) == 0) ? -1.f : 1.f;
    _strafeSwapTimer = (float)GetRandomValue(150, 320) / 100.f;
    _relocateTimer   = 0.f;

    _drawDurationInst = 0.65f;
    _shotCooldownInst = 2.4f;

    _forcedPushActive    = false;
    _forcedPushDirection = Vector2Zero();
    _forcedPushSpeed     = 0.f;

    _pendingBurns.clear();
    _waypoints.clear();
    _waypointIndex = 0;

    // Character Animator overrides (scale, hitboxes, anim speeds) win last.
    ResetTuningState();
    ApplyStoredTuning();
}

void SkeletonArcher::SetIdleAnimation(bool resetFrame)
{
    _texture    = _idleAnim;
    _width      = kArcherFrameWidth;
    _height     = _idleAnim.height;
    _updateTime = (_editorAnimFrameTimes[0] > 0.f) ? _editorAnimFrameTimes[0] : kArcherIdleFrameTime;
    _maxFrames  = kArcherFrameCount;
    if (resetFrame) { _frame = 0; _runningTime = 0.f; }
}

void SkeletonArcher::SetWalkAnimation(bool resetFrame)
{
    _texture    = _walkAnim;
    _width      = kArcherFrameWidth;
    _height     = _walkAnim.height;
    _updateTime = (_editorAnimFrameTimes[1] > 0.f) ? _editorAnimFrameTimes[1] : kArcherWalkFrameTime;
    _maxFrames  = kArcherFrameCount;
    if (resetFrame) { _frame = 0; _runningTime = 0.f; }
}

void SkeletonArcher::SetAttackAnimation(bool resetFrame)
{
    _texture    = _attackAnim;
    _width      = kArcherFrameWidth;
    _height     = _attackAnim.height;
    // Spread the 6 attack frames across the full draw duration so the release
    // frame lines up with the arrow actually spawning. A tuned frame time
    // takes priority when authored in the Character Animator.
    _updateTime = (_editorAnimFrameTimes[2] > 0.f)
        ? _editorAnimFrameTimes[2]
        : _drawDurationInst / (float)kArcherFrameCount;
    _maxFrames  = kArcherFrameCount;
    if (resetFrame) { _frame = 0; _runningTime = 0.f; }
}

// =============================================================================
void SkeletonArcher::Update(float dt, Vector2 heroWorldPos,
                            Vector2 /*navigationTarget*/, bool hasNavigationTarget,
                            const std::vector<std::unique_ptr<Enemy>>& /*enemies*/,
                            const std::vector<Vector2>& propCenters)
{
    if (!_isActive)
        return;

    _worldPosLastFrame = _worldPos;

    UpdateHit(dt);

    if (_forcedPushActive)
    {
        _worldPos = Vector2Add(_worldPos, Vector2Scale(_forcedPushDirection, _forcedPushSpeed * dt));
        return;
    }

    if (_drawingBow || _wantsToFire)
        _velocity = Vector2Zero();

    ApplyVelocity(dt);
    UpdateBurns(dt);
    UpdateElectricCharge(dt);
    UpdateLaunchVisual(dt);

    if (_freezeTimer > 0.f)
        _freezeTimer -= dt;

    if (!_dying)
    {
        if (_target == nullptr)
            return;

        if (_shotCooldown > 0.f)
            _shotCooldown -= dt;

        float distToPlayer = Vector2Distance(_worldPos, heroWorldPos);
        bool  hasLineOfSight = _relentlessFire || !hasNavigationTarget;

        // Begin the draw-bow telegraph when in range with a clear line.
        if (!_drawingBow && !_takingDamage &&
            _shotCooldown <= 0.f && distToPlayer < _fireMaxRange && hasLineOfSight)
        {
            _drawingBow = true;
            _drawTimer  = 0.f;
            SetAttackAnimation(true);
        }

        if (_drawingBow && (IsFrozen() || IsElectroStunned()))
            CancelDraw();

        if (_drawingBow)
        {
            _drawTimer += dt;
            if (_drawTimer >= _drawDurationInst)
            {
                Vector2 toTarget = Vector2Subtract(_target->GetFeetWorldPos(), _worldPos);
                _wantsToFire   = true;
                _fireDirection = (Vector2LengthSqr(toTarget) > 0.0001f)
                    ? Vector2Normalize(toTarget)
                    : Vector2{ _rightLeft >= 0.f ? 1.f : -1.f, 0.f };
                _drawingBow   = false;
                _shotCooldown = _shotCooldownInst;
                _relocateTimer = _relocateDuration;   // relocate once, then plant again
                SetIdleAnimation(true);
            }
        }

        HandleKiteMovement(dt, propCenters);
    }

    HandleAnimation(dt);
}

// =============================================================================
// Kiting: hold a distance band from the player, strafe sideways inside it.
// =============================================================================
void SkeletonArcher::HandleKiteMovement(float dt, const std::vector<Vector2>& propCenters)
{
    if (_target == nullptr || _dying)
        return;

    if (_relocateTimer > 0.f)
        _relocateTimer -= dt;

    if (_relentlessFire)
    {
        _velocity = Vector2Zero();
        float dx = _target->GetFeetWorldPos().x - _worldPos.x;
        if      (dx < -20.f) _rightLeft = -1.f;
        else if (dx >  20.f) _rightLeft =  1.f;
        return;
    }

    // Stand still and face the player while drawing / releasing the bow.
    if (_drawingBow || _wantsToFire)
    {
        _velocity = Vector2Zero();
        float dx = _target->GetFeetWorldPos().x - _worldPos.x;
        if      (dx < -20.f) _rightLeft = -1.f;
        else if (dx >  20.f) _rightLeft =  1.f;
        return;
    }

    Vector2 toPlayer = Vector2Subtract(_target->GetFeetWorldPos(), _worldPos);
    float   distToPlayer = Vector2Length(toPlayer);
    Vector2 playerDir = (distToPlayer > 0.01f) ? Vector2Scale(toPlayer, 1.f / distToPlayer) : Vector2{ 1.f, 0.f };

    // Periodically flip the strafe direction so archers don't orbit forever.
    _strafeSwapTimer -= dt;
    if (_strafeSwapTimer <= 0.f)
    {
        _strafeSign      = -_strafeSign;
        _strafeSwapTimer = (float)GetRandomValue(180, 380) / 100.f;
    }

    Vector2 moveDir{};
    if (distToPlayer < _kiteRetreatDistance)
    {
        // Too close — back straight away with a slight sideways drift.
        Vector2 strafe{ -playerDir.y * _strafeSign, playerDir.x * _strafeSign };
        moveDir = Vector2Add(Vector2Scale(playerDir, -1.f), Vector2Scale(strafe, 0.35f));
    }
    else if (distToPlayer > _kiteAdvanceDistance)
    {
        moveDir = playerDir;
    }
    else if (_relocateTimer > 0.f)
    {
        // Brief relocation window right after firing: find a new lane instead
        // of settling back where it just stood. Prefer this frame's shared
        // engagement point (already lane/off-angle aware — see
        // ComputeEngagementTarget) when one is assigned; fall back to the
        // plain lateral strafe otherwise.
        if (HasEngagementAssignment() && GetEngagementIntent() != EngagementIntent::Commit)
        {
            Vector2 toLane = Vector2Subtract(GetEngagementTarget(), _worldPos);
            moveDir = (Vector2Length(toLane) > 0.01f)
                ? Vector2Normalize(toLane)
                : Vector2{ -playerDir.y * _strafeSign, playerDir.x * _strafeSign };
        }
        else
        {
            moveDir = Vector2{ -playerDir.y * _strafeSign, playerDir.x * _strafeSign };
        }
    }
    else
    {
        // Inside the comfort band and not relocating — plant. "Does not kite
        // continuously": no more perpetual strafing once it has a good lane.
        moveDir = Vector2Zero();
    }

    // Prop repulsion so archers slide around pillars instead of hugging them.
    Vector2 separation{};
    for (const Vector2& propCenter : propCenters)
    {
        float dist = Vector2Distance(_worldPos, propCenter);
        if (dist < 110.f && dist > 0.f)
        {
            Vector2 away = Vector2Subtract(_worldPos, propCenter);
            if (Vector2Length(away) > 0.01f)
                separation = Vector2Add(separation,
                    Vector2Scale(Vector2Normalize(away), (110.f - dist) / 110.f * 1.8f));
        }
    }
    moveDir = Vector2Add(moveDir, Vector2Scale(separation, 0.6f));

    // Keep away from the arena edges while retreating so the archer never
    // pins itself in a corner (rooms are one screen; walls at the canvas edge).
    const float edgeMargin = 140.f;
    if (_worldPos.x < edgeMargin)                        moveDir.x += 1.f;
    if (_worldPos.x > (float)kVirtualWidth - edgeMargin)  moveDir.x -= 1.f;
    if (_worldPos.y < edgeMargin)                        moveDir.y += 1.f;
    if (_worldPos.y > (float)kVirtualHeight - edgeMargin) moveDir.y -= 1.f;

    if (Vector2Length(moveDir) > 0.01f)
        moveDir = Vector2Normalize(moveDir);

    Vector2 oldPos = _worldPos;
    if (!_attacking && !_takingDamage && !IsFrozen())
        _worldPos = Vector2Add(_worldPos, Vector2Scale(moveDir, _speed * dt));

    Vector2 movement = Vector2Subtract(_worldPos, oldPos);
    if (!_attacking && !_takingDamage)
    {
        if (Vector2Length(movement) > 0.01f)
            SetWalkAnimation(_texture.id != _walkAnim.id);
        else
            SetIdleAnimation(_texture.id != _idleAnim.id);

        // Debounced facing — always face the player, not the walk direction.
        float dx     = _target->GetFeetWorldPos().x - _worldPos.x;
        float wanted = _rightLeft;
        if      (dx < -20.f) wanted = -1.f;
        else if (dx >  20.f) wanted =  1.f;

        if (wanted != _rightLeft)
        {
            _facingTimer += dt;
            if (_facingTimer >= 0.15f)
            {
                _rightLeft   = wanted;
                _facingTimer = 0.f;
            }
        }
        else
        {
            _facingTimer = 0.f;
        }
    }
}

// =============================================================================
void SkeletonArcher::HandleAnimation(float dt)
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
                SetIdleAnimation(true);
                return;
            }
            _frame = 0;
        }
    }
}

// =============================================================================
void SkeletonArcher::DrawEnemy(Vector2 cameraRef)
{
    if (!_isActive)
        return;

    float drawWidth  = kArcherFrameWidth * _scale;
    float drawHeight = (_idleAnim.id > 0 ? (float)_idleAnim.height : _height) * _scale;

    float launchRatio = (_launchVisualDuration > 0.f)
        ? (_launchVisualTimer / _launchVisualDuration)
        : 0.f;
    drawWidth  *= 1.f + _launchVisualScaleBoost * launchRatio;
    drawHeight *= 1.f + _launchVisualScaleBoost * launchRatio;

    Vector2 screenPos = Vector2Subtract(_worldPos, cameraRef);
    screenPos.x += kVirtualWidth  / 2.f;
    screenPos.y += kVirtualHeight / 2.f - _launchVisualLift * launchRatio;

    bool burning        = !_pendingBurns.empty();
    bool frozen         = IsFrozen();
    bool electroStunned = IsElectroStunned();

    Color tint = electroStunned   ? Color{ 255, 255,  60, 255 } :
                 _flickerInWindup ? Color{ 180, 100, 255, 180 } :
                 frozen           ? Color{ 140, 200, 255, 255 } :
                 burning          ? Color{ 255, 180, 180, 255 } :
                                    WHITE;

    if (burning && !frozen)
    {
        for (int i = 0; i < 3; ++i)
        {
            float fx = (float)GetRandomValue(-14, 14);
            float fy = (float)GetRandomValue(-26, -4);
            DrawCircleV({ screenPos.x + fx, screenPos.y + fy }, 5.f, Fade(ORANGE, 0.55f));
        }
    }

    // Aim line telegraph while drawing the bow — thin and short so it warns
    // without dominating the screen like the cyclops laser sight.
    if (_drawingBow && _target != nullptr)
    {
        float drawRatio = _drawTimer / _drawDurationInst;
        Vector2 aimDir = Vector2Subtract(_target->GetFeetWorldPos(), _worldPos);
        if (Vector2LengthSqr(aimDir) > 0.0001f)
        {
            aimDir = Vector2Normalize(aimDir);
            Vector2 lineEnd = Vector2Add(screenPos, Vector2Scale(aimDir, 260.f + drawRatio * 160.f));
            DrawLineEx(screenPos, lineEnd, 2.6f, Fade(Color{ 255, 230, 160, 255 }, 0.25f + drawRatio * 0.35f));
        }
    }

    Vector2 animDrawOffset = GetCurrentAnimDrawOffset();
    Rectangle source{ _frame * _width, 0.f, _rightLeft * _width, _height };
    Rectangle dest{ screenPos.x - drawWidth / 2.f + animDrawOffset.x,
                    screenPos.y - drawHeight / 2.f + animDrawOffset.y, drawWidth, drawHeight };
    DrawEllipse((int)(screenPos.x + animDrawOffset.x),
        (int)(screenPos.y + _launchVisualLift * launchRatio + drawHeight * 0.50f + animDrawOffset.y),
        drawWidth * 0.28f, drawHeight * 0.055f, Fade(BLACK, 0.30f));
    DrawTexturePro(_texture, source, dest, Vector2{}, 0.f, tint);

    if (_graveReviveInvulTimer > 0.f)
    {
        float pulse = sinf((float)GetTime() * 10.f) * 0.4f + 0.6f;
        DrawCircleLines((int)screenPos.x, (int)screenPos.y, 55.f, Fade(Color{ 80, 255, 120, 255 }, pulse));
    }

    if (_health != _maxHealth)
        DrawHealthBar(screenPos, drawWidth, drawHeight);
    if (_isEliteMiniboss)
        DrawEliteLabel(screenPos, drawWidth, drawHeight);
}

Rectangle SkeletonArcher::GetCollisionRec() const
{
    Rectangle animBodyRect;
    if (GetAnimBodyRectWorld(animBodyRect))
        return animBodyRect;
    if (_hasTunedCollision)
        return GetTunedCollisionRec();

    float stableHalfW = kArcherFrameWidth * _scale * 0.5f;
    float stableHalfH = (_idleAnim.id > 0 ? (float)_idleAnim.height : _height) * _scale * 0.5f;

    if (_collisionSize.x == 0.f && stableHalfW > 0.f)
    {
        auto* s = const_cast<SkeletonArcher*>(this);
        // The archer occupies the middle ~third of its 48px frame.
        s->_collisionSize   = { 62.f, 92.f };
        s->_collisionOffset = { stableHalfW - 31.f, stableHalfH - 46.f };
    }
    return Rectangle{
        _worldPos.x - stableHalfW + _collisionOffset.x,
        _worldPos.y - stableHalfH + _collisionOffset.y,
        _collisionSize.x, _collisionSize.y
    };
}

Capsule2D SkeletonArcher::GetCapsule() const
{
    Capsule2D animBodyCapsule;
    if (GetAnimBodyCapsuleWorld(animBodyCapsule))
        return animBodyCapsule;

    if (_capsuleRadius == 0.f)
    {
        auto* s = const_cast<SkeletonArcher*>(this);
        s->_capsuleRadius     = 28.f;
        s->_capsuleHalfHeight = 14.f;
        s->_capsuleOffset     = { 0.f, 8.f };
    }
    return Capsule2D{
        { _worldPos.x + _capsuleOffset.x, _worldPos.y + _capsuleOffset.y },
        _capsuleHalfHeight,
        _capsuleRadius
    };
}

// =============================================================================
void SkeletonArcher::ApplyFreeze(float duration)
{
    if (_dying || !IsAlive())
        return;

    if (duration > _freezeTimer)
        _freezeTimer = duration;

    CancelDraw();
}

void SkeletonArcher::CancelDraw()
{
    if (!_drawingBow)
        return;

    _drawingBow = false;
    _drawTimer  = 0.f;
    // Half cooldown after an interrupted draw so crowd control feels rewarding
    // without fully disarming the archer.
    _shotCooldown = _shotCooldownInst * 0.5f;
    SetIdleAnimation(true);
}

void SkeletonArcher::OnArrowFired()
{
    _wantsToFire   = false;
    _fireDirection = Vector2Zero();
}

void SkeletonArcher::EnableRelentlessFire()
{
    _relentlessFire = true;
    _drawDurationInst = 0.45f;
    _shotCooldownInst = 0.08f;
    _shotCooldown = (float)GetRandomValue(8, 75) / 100.f;
}

// =============================================================================
void SkeletonArcher::SetWaveScale(int wave)
{
    _expValue    = 4;
    _health      = 2.f;
    _maxHealth   = 2.f;
    _speed       = 210.f;
    _attackPower = 1.f;

    // Later rooms: faster draw and shorter downtime between arrows.
    int tier = (wave - 1) / 5;
    _drawDurationInst = std::max(0.42f, 0.65f - tier * 0.05f);
    _shotCooldownInst = std::max(1.5f,  2.4f  - tier * 0.18f);
}

// =============================================================================
void SkeletonArcher::PlayAttackSound()
{
    float pitch = GetRandomValue(120, 160) / 100.f;
    SetSoundPitch(_attackSound, pitch);
    SetSoundVolume(_attackSound, 0.6f);
    PlaySound(_attackSound);
}

// =============================================================================
void SkeletonArcher::EnsureSharedResourcesLoaded()
{
    if (_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        const char* suffix = kArcherVariantSuffixes[variant];
        _sharedIdleAnim[variant]       = LoadTexture(AssetPath(TextFormat("Enemy/SkeletonArcherIdle%s.png",   suffix)).c_str());
        _sharedWalkAnim[variant]       = LoadTexture(AssetPath(TextFormat("Enemy/SkeletonArcherWalk%s.png",   suffix)).c_str());
        _sharedAttackAnim[variant]     = LoadTexture(AssetPath(TextFormat("Enemy/SkeletonArcherAttack%s.png", suffix)).c_str());
        _sharedTakeDamageAnim[variant] = LoadTexture(AssetPath(TextFormat("Enemy/SkeletonArcherHurt%s.png",   suffix)).c_str());
        _sharedDeathAnim[variant]      = LoadTexture(AssetPath(TextFormat("Enemy/SkeletonArcherDeath%s.png",  suffix)).c_str());
    }
    _sharedAttackSound = LoadSound(AssetPath("Sounds/SwordSwipe2.ogg").c_str());
    _sharedHurtSound   = LoadSound(AssetPath("Sounds/SmallMonsterDamage.ogg").c_str());
    _sharedDeathSound  = LoadSound(AssetPath("Sounds/PlayerDeath.ogg").c_str());
    _sharedResourcesLoaded = true;
}

void SkeletonArcher::UnloadSharedResources()
{
    if (!_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        UnloadTexture(_sharedIdleAnim[variant]);
        UnloadTexture(_sharedWalkAnim[variant]);
        UnloadTexture(_sharedAttackAnim[variant]);
        UnloadTexture(_sharedTakeDamageAnim[variant]);
        UnloadTexture(_sharedDeathAnim[variant]);
        _sharedIdleAnim[variant]       = Texture2D{};
        _sharedWalkAnim[variant]       = Texture2D{};
        _sharedAttackAnim[variant]     = Texture2D{};
        _sharedTakeDamageAnim[variant] = Texture2D{};
        _sharedDeathAnim[variant]      = Texture2D{};
    }
    UnloadSound(_sharedAttackSound);
    UnloadSound(_sharedHurtSound);
    UnloadSound(_sharedDeathSound);
    _sharedAttackSound = Sound{};
    _sharedHurtSound   = Sound{};
    _sharedDeathSound  = Sound{};
    _sharedResourcesLoaded = false;
}
