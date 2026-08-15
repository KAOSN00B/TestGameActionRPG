#include "Phantom.h"
#include "VirtualCanvas.h"
#include "AssetPaths.h"
#include "Character.h"
#include "raymath.h"
#include <cmath>

Texture2D Phantom::_sharedIdleAnim[Phantom::kVariantCount]{};
Texture2D Phantom::_sharedWalkAnim[Phantom::kVariantCount]{};
Texture2D Phantom::_sharedAttackAnim[Phantom::kVariantCount]{};
Texture2D Phantom::_sharedHurtAnim[Phantom::kVariantCount]{};
Texture2D Phantom::_sharedDeathAnim[Phantom::kVariantCount]{};
Sound     Phantom::_sharedAttackSound{};
Sound     Phantom::_sharedHurtSound{};
Sound     Phantom::_sharedDeathSound{};
bool      Phantom::_sharedResourcesLoaded = false;

namespace
{
    constexpr float kPhantomFrameWidth = 48.f;
    // Tier ladder: green -> purple -> shadow (pack letters A, B, C).
    const char* kPhantomVariantSuffixes[3] = { "", "_B", "_C" };
}

Phantom::Phantom(Vector2 pos)
    : Enemy(pos)
{
}

Phantom::~Phantom() {}

void Phantom::Init()
{
    EnsureSharedResourcesLoaded();

    _attackSound = _sharedAttackSound;
    _hurtSound   = _sharedHurtSound;
    _deathSound  = _sharedDeathSound;
    SetVariantTier(_variantTier);

    ResetForSpawn(_worldPos);
}

void Phantom::SetVariantTier(int tier)
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

void Phantom::ResetForSpawn(Vector2 pos)
{
    Enemy::ResetForSpawn(pos);

    _width  = kPhantomFrameWidth;
    _scale  = 4.4f;
    _speed  = kDriftSpeed;
    _health = 2.f;
    _maxHealth = 2.f;
    _attackPower = 1.f;
    _expValue = 4;

    _capsuleRadius     = 24.f;
    _capsuleHalfHeight = 10.f;
    _capsuleOffset     = { 0.f, 4.f };

    _phased     = false;
    _phaseTimer = kTangibleDuration + (float)GetRandomValue(0, 80) / 100.f;   // desync packs
    _bobTimer   = (float)GetRandomValue(0, 628) / 100.f;
    _biteCooldown = 1.0f;
    _bitThisCycle = false;

    _texture   = _idleAnim;
    _height    = _texture.height;
    _maxFrames = (int)(_texture.width / _width);
    _frame     = GetRandomValue(0, _maxFrames - 1);

    ApplyStoredTuning();
}

void Phantom::Update(float dt, Vector2 heroWorldPos, Vector2 /*navigationTarget*/, bool /*hasNavigationTarget*/,
    const std::vector<std::unique_ptr<Enemy>>& /*enemies*/,
    const std::vector<Vector2>& /*propCenters*/)
{
    if (!_isActive)
        return;

    _worldPosLastFrame = _worldPos;
    _bobTimer += dt;
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
    if (_biteCooldown > 0.f)
        _biteCooldown -= dt;

    if (!_dying)
    {
        if (_target == nullptr)
            return;

        // Reinforcement arrival latch (Task 5 follow-up): a freshly-telegraphed
        // Phantom holds position for a brief beat instead of ambushing on its
        // creation frame. Phantom fully overrides Enemy::Update, so it must
        // check this itself — the shared base-class gate never runs for it.
        if (_arrivalTimer > 0.f)
        {
            UpdateArrivalDelay(dt);
            return;
        }

        // Phase cycle — freezing pauses the cycle so ice can extend the window.
        if (!IsFrozen())
        {
            _phaseTimer -= dt;
            if (_phaseTimer <= 0.f)
            {
                _phased = !_phased;
                _phaseTimer = _phased ? kPhasedDuration : kTangibleDuration;
                // Going phased again ends this ambush turn — release the
                // engagement Commit slot into its post-attack recovery window
                // (only if a bite actually landed this tangible window; see
                // _bitThisCycle's comment).
                if (_phased && _bitThisCycle)
                {
                    _engagementLatch.EndCommit();
                    _bitThisCycle = false;
                }
            }
        }

        bool controlled = IsFrozen() || IsElectroStunned() || _takingDamage;
        Vector2 toPlayer = Vector2Subtract(heroWorldPos, _worldPos);
        float dist = Vector2Length(toPlayer);

        if (!controlled && dist > 60.f)
        {
            float driftSpeed = _speed * (_phased ? kPhasedSpeedMult : 1.f);
            if (HasWarAura())
                driftSpeed *= kWarAuraSpeedMultiplier;

            // Flanking ambush: while PHASED (untouchable, passes through
            // everything) it aims PAST the player so it re-materializes on
            // their far side, instead of always arriving from the front.
            Vector2 driftTarget = heroWorldPos;
            if (_phased && dist > 90.f)
            {
                driftTarget = Vector2Add(heroWorldPos,
                    Vector2Scale(Vector2Normalize(toPlayer), Balance::Squad::kAssassinFlankDepth));
            }

            // "Seeks an off-angle, commits to an ambush, then retreats or
            // repositions after the attack." (design doc) — while this
            // Phantom is not holding this frame's engagement Commit slot, aim
            // for the shared off-angle point (ComputeEngagementTarget's
            // Assassin-role bias) instead of the player/flank-past-player
            // line, so a Phantom whose turn hasn't come up drifts to a new
            // angle and waits there rather than still homing in.
            bool holdingStablePoint = false;
            if (HasEngagementAssignment() && GetEngagementIntent() != EngagementIntent::Commit)
            {
                driftTarget = GetStableEngagementTarget(dt);
                holdingStablePoint = true;
            }

            Vector2 toDriftTarget = Vector2Subtract(driftTarget, _worldPos);
            // Arrival deadzone, ONLY while holding a stable Reposition point
            // (never during a genuine chase, where driftTarget keeps moving
            // with the player/phased-flank line and should still be walked
            // all the way to bite range). Without this, a constant-speed
            // drift toward a now-stationary point (see GetStableEngagementTarget)
            // overshoots it every frame and shakes in place — same root cause
            // already fixed for the shared Enemy::HandleMovement path.
            bool arrivedAtHold = holdingStablePoint &&
                Vector2Length(toDriftTarget) < Balance::Rhythm::kEngagementArrivalDeadzone;
            if (!arrivedAtHold && Vector2Length(toDriftTarget) > 0.01f)
            {
                Vector2 drift = Vector2Scale(Vector2Normalize(toDriftTarget), driftSpeed * dt);
                drift.y += sinf(_bobTimer * 2.6f) * 20.f * dt;
                _worldPos = Vector2Add(_worldPos, drift);
            }
        }

        // Bite while tangible and in range — simple touch attack. Gated on
        // holding this frame's engagement Commit slot (mirrors
        // Enemy::HandleAttack's HasEngagementAssignment() fallback pattern)
        // so Phantoms respect the shared commit-slot budget instead of every
        // tangible Phantom biting simultaneously regardless of assignment.
        bool canCommitBite = HasEngagementAssignment()
            ? (GetEngagementIntent() == EngagementIntent::Commit && _engagementLatch.CanCommit())
            : true;
        if (!_phased && !controlled && dist < kBiteRange && _biteCooldown <= 0.f &&
            canCommitBite && _target->IsAlive())
        {
            _biteCooldown = kBiteCooldown;
            _engagementLatch.BeginCommit();
            _bitThisCycle = true;
            _target->TakeDamage((int)_attackPower, _worldPos);
            PlayAttackSound();
        }

        float dx = heroWorldPos.x - _worldPos.x;
        if      (dx < -20.f) _rightLeft = -1.f;
        else if (dx >  20.f) _rightLeft =  1.f;
    }

    HandleAnimationLoop(dt);
}

void Phantom::HandleAnimationLoop(float dt)
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

void Phantom::TakeDamage(int damage, Vector2 attackerPos)
{
    // Untouchable while phased — the whole point of the enemy.
    if (_phased && !_dying)
        return;

    Enemy::TakeDamage(damage, attackerPos);
}

void Phantom::DrawEnemy(Vector2 heroWorldPos)
{
    if (!_isActive)
        return;

    float drawWidth  = kPhantomFrameWidth * _scale;
    float drawHeight = (_idleAnim.id > 0 ? (float)_idleAnim.height : _height) * _scale;

    Vector2 screenPos = Vector2{ _worldPos.x - heroWorldPos.x + kVirtualWidth / 2.f,
                                 _worldPos.y - heroWorldPos.y + kVirtualHeight / 2.f + sinf(_bobTimer * 2.6f) * 6.f };

    bool frozen         = IsFrozen();
    bool electroStunned = IsElectroStunned();

    Color tint = electroStunned ? Color{ 255, 255,  60, 255 } :
                 frozen         ? Color{ 140, 200, 255, 255 } :
                                  WHITE;
    // Phased: heavy transparency sells "you can't touch me right now".
    if (_phased && !_dying)
        tint = Fade(tint, 0.32f);

    // Warning shimmer half a second before turning tangible again.
    if (_phased && _phaseTimer < 0.5f)
    {
        float pulse = sinf((float)GetTime() * 20.f) * 0.5f + 0.5f;
        tint = Fade(WHITE, 0.32f + pulse * 0.35f);
    }

    Vector2 animDrawOffset = GetCurrentAnimDrawOffset();
    Rectangle source{ _frame * _width, 0.f, _rightLeft * _width, _height };
    Rectangle dest{ screenPos.x - drawWidth / 2.f + animDrawOffset.x,
                    screenPos.y - drawHeight / 2.f + animDrawOffset.y, drawWidth, drawHeight };
    DrawEllipse((int)(screenPos.x + animDrawOffset.x),
        (int)(screenPos.y - sinf(_bobTimer * 2.6f) * 6.f + drawHeight * 0.50f + animDrawOffset.y),
        drawWidth * 0.24f, drawHeight * 0.055f, Fade(BLACK, _phased ? 0.12f : 0.20f));
    DrawTexturePro(_texture, source, dest, Vector2{}, 0.f, tint);

    if (_health != _maxHealth && !_phased)
        DrawHealthBar(screenPos, drawWidth, drawHeight);
    if (_isEliteMiniboss)
        DrawEliteLabel(screenPos, drawWidth, drawHeight);
}

Rectangle Phantom::GetCollisionRec() const
{
    Rectangle animBodyRect;
    if (GetAnimBodyRectWorld(animBodyRect))
        return animBodyRect;
    if (_hasTunedCollision)
        return GetTunedCollisionRec();

    return Rectangle{ _worldPos.x - 34.f, _worldPos.y - 38.f, 68.f, 76.f };
}

Capsule2D Phantom::GetCapsule() const
{
    Capsule2D animBodyCapsule;
    if (GetAnimBodyCapsuleWorld(animBodyCapsule))
        return animBodyCapsule;

    return Capsule2D{
        { _worldPos.x + _capsuleOffset.x, _worldPos.y + _capsuleOffset.y },
        _capsuleHalfHeight,
        (_capsuleRadius > 0.f) ? _capsuleRadius : 24.f
    };
}

void Phantom::SetWaveScale(int wave)
{
    (void)wave;
    _expValue    = 4;
    _health      = 2.f;
    _maxHealth   = 2.f;
    _speed       = kDriftSpeed;
    _attackPower = 1.f;
}

void Phantom::PlayAttackSound()
{
    float pitch = GetRandomValue(60, 85) / 100.f;
    SetSoundPitch(_attackSound, pitch);
    SetSoundVolume(_attackSound, 0.5f);
    PlaySound(_attackSound);
}

void Phantom::PlayDeathSound()
{
    // Ghost dissolves on death — spectral dissipation, higher pitch than most
    SfxBank::Get().Play(SfxId::DeathSmall, 0.55f, true);
}

void Phantom::EnsureSharedResourcesLoaded()
{
    if (_sharedResourcesLoaded)
        return;

    for (int variant = 0; variant < kVariantCount; variant++)
    {
        const char* suffix = kPhantomVariantSuffixes[variant];
        _sharedIdleAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/PhantomIdle%s.png",   suffix)).c_str());
        _sharedWalkAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/PhantomWalk%s.png",   suffix)).c_str());
        _sharedAttackAnim[variant] = LoadTexture(AssetPath(TextFormat("Enemy/PhantomAttack%s.png", suffix)).c_str());
        _sharedHurtAnim[variant]   = LoadTexture(AssetPath(TextFormat("Enemy/PhantomHurt%s.png",   suffix)).c_str());
        _sharedDeathAnim[variant]  = LoadTexture(AssetPath(TextFormat("Enemy/PhantomDeath%s.png",  suffix)).c_str());
    }
    _sharedAttackSound = LoadSound(AssetPath("Sounds/SmallMonsterDamage2.ogg").c_str());
    _sharedHurtSound   = LoadSound(AssetPath("Sounds/SmallMonsterDamage.ogg").c_str());
    _sharedDeathSound  = LoadSound(AssetPath("Sounds/PlayerDeath.ogg").c_str());
    _sharedResourcesLoaded = true;
}

void Phantom::UnloadSharedResources()
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
    UnloadSound(_sharedAttackSound);
    UnloadSound(_sharedHurtSound);
    UnloadSound(_sharedDeathSound);
    _sharedAttackSound = Sound{};
    _sharedHurtSound   = Sound{};
    _sharedDeathSound  = Sound{};
    _sharedResourcesLoaded = false;
}
