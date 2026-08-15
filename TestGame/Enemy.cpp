#include "Enemy.h"
#include "VirtualCanvas.h"
#include "AssetPaths.h"
#include "CharacterTuning.h"
#include "VirtualCanvas.h"

#include "raymath.h"
#include "VirtualCanvas.h"
#include <algorithm>
#include <climits>
#include <cmath>

Texture2D Enemy::_sharedIdleAnim{};
Texture2D Enemy::_sharedWalkAnim{};
Texture2D Enemy::_sharedAttackAnim{};
Texture2D Enemy::_sharedTakeDamageAnim{};
Texture2D Enemy::_sharedDeathAnim{};
Sound Enemy::_sharedAttackSound{};
Sound Enemy::_sharedHurtSound{};
Sound Enemy::_sharedDeathSound{};
bool Enemy::_sharedResourcesLoaded = false;

namespace
{
    // Monotonically increasing, process-wide — assigned once per C++ Enemy
    // object (constructor only) and never reassigned by ResetForSpawn, so a
    // pooled/reused enemy keeps the same runtime id across its whole pooled
    // lifetime. Starts at 1 so 0 stays available as an "unset" sentinel.
    std::uint64_t gNextEnemyRuntimeId = 1;
}

Enemy::Enemy(Vector2 pos)
{
    _worldPos = pos;
    _homePos = pos;
    _runtimeId = gNextEnemyRuntimeId++;
}

Enemy::~Enemy()
{
}

void Enemy::SetActive(bool active)
{
    _isActive = active;
    if (active) return;

    // A pooled enemy can be removed while its fall is still playing (room
    // transition/debug clear). Restore its authored scale before reuse.
    if (_pitStartScale > 0.f)
        _scale = _pitStartScale;
    _pitFalling = false;
    _pitFallTimer = 0.f;
    _pitStartScale = 0.f;
    _pitFallTint = WHITE;
}

void Enemy::BeginPitFall(Vector2 pullTarget, Color tint)
{
    if (_pitFalling || _dying || !_isActive) return;
    _pitFalling = true;
    _pitFallTimer = 0.f;
    _pitStartScale = _scale;
    _pitStartPos = _worldPos;
    _pitTargetPos = pullTarget;
    _pitFallTint = tint;
    _velocity = Vector2Zero();
    ReleaseAttackCommitment();
    _takingDamage = false;
    _forcedPushActive = false;
    _forcedPushSpeed = 0.f;
    _forcedPushDirection = Vector2Zero();
}

void Enemy::UpdatePitFall(float dt)
{
    if (!_pitFalling) return;
    _pitFallTimer = std::min(kPitFallDuration, _pitFallTimer + std::max(0.f, dt));
    const float p = PitFallProgress();
    const float eased = 1.f - (1.f - p) * (1.f - p);
    _worldPos = Vector2Lerp(_pitStartPos, _pitTargetPos, eased * 0.9f);
    _worldPosLastFrame = _worldPos;
    _scale = _pitStartScale;
}

bool Enemy::PitFallComplete() const
{
    return _pitFalling && _pitFallTimer >= kPitFallDuration;
}

float Enemy::PitFallProgress() const
{
    if (!_pitFalling) return 0.f;
    return std::clamp(_pitFallTimer / kPitFallDuration, 0.f, 1.f);
}

void Enemy::FinishPitFall()
{
    if (!_pitFalling) return;
    _pitFalling = false;
    _pitFallTimer = kPitFallDuration;
    // Environmental death bypasses shields/revive but otherwise enters the
    // standard death timer consumed by CombatDirector::UpdateEnemyDeaths.
    BaseCharacter::TakeDamage(INT_MAX, _pitTargetPos);
}

void Enemy::Init()
{
    EnsureSharedResourcesLoaded();

    _idleAnim = _sharedIdleAnim;
    _walkAnim = _sharedWalkAnim;
    _attackAnim = _sharedAttackAnim;
    _takeDamageAnim = _sharedTakeDamageAnim;
    _deathAnim = _sharedDeathAnim;
    _attackSound = _sharedAttackSound;
    _hurtSound = _sharedHurtSound;
    _deathSound = _sharedDeathSound;

    ResetForSpawn(_worldPos);
}

void Enemy::ResetForSpawn(Vector2 pos)
{
    _worldPos = pos;
    _worldPosLastFrame = pos;
    _homePos = pos;
    _velocity = Vector2Zero();
    _isActive         = true;
    _pitFalling       = false;
    _pitFallTimer     = 0.f;
    _pitStartScale    = 0.f;
    _pitFallTint      = WHITE;
    _arrivalTimer     = 0.f;
    if (_isEliteMiniboss)
        SetPhaseThresholds({});   // previous pooled life was an elite — drop its 50% latch
    _isEliteMiniboss  = false;
    _isInvulnerable   = false;
    _leapInvulnerable = false;
    ResetStatuses();      // clear poison/bleed/slow/vuln/mark from a pooled previous life
    ResetTuningState();   // re-applied from file at the end of the reset
    _texture = _idleAnim;
    _updateTime = 1.f / 8.f;

    _width = 32.f;
    _height = _texture.height;
    _scale = 6.f;
    _speed = 200.f;

    _health = 3.f;
    _maxHealth = 3.f;
    _attackPower = 1.f;

    _maxFrames = (int)(_texture.width / _width);
    _frame = GetRandomValue(0, _maxFrames - 1);
    _runningTime = GetRandomValue(0, 200) / 100.f * _updateTime;
    _hitTimer = 0.f;
    _deathTimer = 0.4f;
    _freezeTimer              = 0.f;
    _isCharged                = false;
    _chargeNextStunTime       = 0.f;
    _electricChargeTotalTimer = 0.f;
    _attacking = false;
    _damageApplied = false;
    // Pooled reuse starts a fresh engagement life: no inherited Commit lock,
    // no leftover recovery countdown, no stale assignment/target/swarm flag
    // from whatever this pooled slot last played as. _runtimeId is NOT reset
    // here — it is the one thing that must survive pooling (see Enemy.h).
    _engagementLatch.Reset();
    _engagementIntent = EngagementIntent::Reposition;
    _engagementTarget = Vector2Zero();
    _hasEngagementAssignment = false;
    _swarmProfile = false;
    _engagementHoldPos = Vector2Zero();
    _engagementHoldTimer = 0.f;
    _engagementHoldValid = false;
    _lockedApproachTarget = Vector2Zero();
    _approachLineLocked = false;
    // Pooled reuse must never leak a previous life's elite state.
    _eliteEvents.Clear();
    _eliteEventSequence   = 0;
    _eliteDroppedEvents   = 0;
    _eliteGuardLinked     = false;
    _eliteGuardReducedHit = false;
    _eliteSignatureCasts  = 0;
    _eliteSignatureHits   = 0;
    _burnPanicDir = Vector2Zero();
    _burnPanicTurnTimer = 0.f;
    _burnSoundTimer = 0.f;
    _takingDamage = false;
    _dying = false;
    _pendingBurns.clear();
    _stuckTimer    = 0.f;
    _stuckCheckPos = _worldPos;

    // Clear the cached waypoint path and stagger each enemy's first refresh
    // with a small random offset so they don't all hit the nav grid at once.
    _waypoints.clear();
    _waypointIndex = 0;
    _pathRefreshInterval = kPathRefreshMin
        + (float)GetRandomValue(0, 100) / 100.f * (kPathRefreshMax - kPathRefreshMin);
    _pathRefreshTimer = (float)GetRandomValue(0, (int)(_pathRefreshInterval * 100.f)) / 100.f;

    _forcedPushActive    = false;
    _forcedPushDirection = Vector2Zero();
    _forcedPushSpeed     = 0.f;
    _inAttackRange       = false;

    _graveReviveAvailable  = false;
    _graveReviveInvulTimer = 0.f;

    // Stagger each enemy's first flicker so a room full of grunts doesn't all
    // vanish at the same moment on the first tick.
    _flickerCooldown    = (float)GetRandomValue(150, 450) / 100.f;
    _flickerInWindup    = false;
    _flickerWindupTimer = 0.f;
    _flickerTarget      = Vector2Zero();

    // Each enemy gets its own flank slot so nearby enemies naturally choose
    // slightly different approach lanes around the player instead of piling
    // into one exact point.
    _flankSide = (GetRandomValue(0, 1) == 0) ? -1.f : 1.f;
    _flankDistance = (float)GetRandomValue((int)_minFlankDistance, (int)_maxFlankDistance);

    PickApproachOffset();
    _approachOffsetTimer = (float)GetRandomValue(0, 250) / 100.f;
}

void Enemy::SetIsEliteMiniboss(bool b)
{
    _isEliteMiniboss = b;

    if (!b)
        return;

    _maxHealth = std::ceil(_maxHealth * 2.5f);
    _health = _maxHealth;
    _attackPower *= 1.25f;
    _speed *= 1.10f;
    _expValue = std::max(_expValue + 4, (int)std::ceil(_expValue * 2.0f));

    // Arm the one-time 50% escalation through the shared phase latch (the same
    // system full bosses use). It does NOT change movement or invulnerability —
    // each elite reacts to ConsumePhaseChange() in its own signature update.
    SetPhaseThresholds({ Balance::Elite::kPhaseThreshold });
}

void Enemy::ApplyEnrage()
{
    _speed       *= 1.5f;
    _attackDelay *= 0.5f;
}

Rectangle Enemy::GetCollisionRec() const
{
    Rectangle animBodyRect;
    if (GetAnimBodyRectWorld(animBodyRect))
        return animBodyRect;
    if (_hasTunedCollision)
        return GetTunedCollisionRec();

    // Stable idle-frame dimensions are the sprite-space reference.
    // _width is always 32 for the grunt idle sheet; _scale is set in ResetForSpawn.
    float stableHalfW = 32.f * _scale * 0.5f;
    float stableHalfH = (_idleAnim.id > 0 ? (float)_idleAnim.height : _height) * _scale * 0.5f;

    if (_collisionSize.x == 0.f && stableHalfW > 0.f)
    {
        auto* s = const_cast<Enemy*>(this);
        s->_collisionSize   = { 87.00f, 79.00f };
        s->_collisionOffset = { 58.00f, 18.00f };
    }
    return Rectangle{
        _worldPos.x - stableHalfW + _collisionOffset.x,
        _worldPos.y - stableHalfH + _collisionOffset.y,
        _collisionSize.x, _collisionSize.y
    };
}

Capsule2D Enemy::GetCapsule() const
{
    Capsule2D animBodyCapsule;
    if (GetAnimBodyCapsuleWorld(animBodyCapsule))
        return animBodyCapsule;

    if (_capsuleRadius == 0.f)
    {
        auto* s = const_cast<Enemy*>(this);
        s->_capsuleRadius     = 36.f;
        s->_capsuleHalfHeight = 0.f;
        s->_capsuleOffset     = { -6.f, 6.f };
    }
    return Capsule2D{
        { _worldPos.x + _capsuleOffset.x, _worldPos.y + _capsuleOffset.y },
        _capsuleHalfHeight,
        _capsuleRadius
    };
}

// =============================================================================
// Character Animator (dev tool) + tuning interface
// =============================================================================

const char* Enemy::GetEditorAnimName(int index) const
{
    static const char* kStandardAnimNames[5] = { "Idle", "Walk", "Attack", "Hurt", "Death" };
    return (index >= 0 && index < 5) ? kStandardAnimNames[index] : "";
}

void Enemy::PlayEditorAnim(int index)
{
    const Texture2D* sheets[5] = { &_idleAnim, &_walkAnim, &_attackAnim, &_takeDamageAnim, &_deathAnim };
    if (index < 0 || index > 4)
        return;

    _texture = *sheets[index];
    if (_width > 0.f)
        _maxFrames = (int)(_texture.width / _width);
    if (_maxFrames < 1)
        _maxFrames = 1;
    _frame       = 0;
    _runningTime = 0.f;

    float frameTimeOverride = _editorAnimFrameTimes[index];
    if (frameTimeOverride > 0.f)
        _updateTime = frameTimeOverride;
}

void Enemy::TickEditorAnimation(float dt)
{
    // Editor-only frame advance: always loops, ignores gameplay state.
    _runningTime += dt;
    if (_runningTime >= _updateTime && _maxFrames > 0)
    {
        _runningTime = 0.f;
        _frame = (_frame + 1) % _maxFrames;
    }
}

float Enemy::GetEditorAnimFrameTime(int index) const
{
    if (index < 0 || index >= 10)
        return 0.f;
    return _editorAnimFrameTimes[index];
}

void Enemy::SetEditorAnimFrameTime(int index, float frameTime)
{
    if (index < 0 || index >= 10)
        return;
    _editorAnimFrameTimes[index] = frameTime;
}

Rectangle Enemy::GetCollisionRecRelative() const
{
    Rectangle rect = GetCollisionRec();
    return Rectangle{ rect.x - _worldPos.x, rect.y - _worldPos.y, rect.width, rect.height };
}

// =============================================================================
// Per-animation tuning — body circle, melee box, sprite draw offset
// =============================================================================

int Enemy::GetCurrentAnimSlot() const
{
    // Derive the slot from whichever sheet is playing so no gameplay code has
    // to remember to update it. Bosses override with their own sheet lists.
    if (_texture.id == _idleAnim.id)       return 0;
    if (_texture.id == _walkAnim.id)       return 1;
    if (_texture.id == _attackAnim.id)     return 2;
    if (_texture.id == _takeDamageAnim.id) return 3;
    if (_texture.id == _deathAnim.id)      return 4;
    return 0;
}

void Enemy::SetAnimBody(int slot, Vector2 offset, float radius)
{
    if (slot < 0 || slot >= kAnimSlots)
        return;
    _animBodySet[slot]    = true;
    _animBodyOffset[slot] = offset;
    _animBodyRadius[slot] = (radius < 4.f) ? 4.f : radius;
}

void Enemy::ClearAnimBody(int slot)
{
    if (slot >= 0 && slot < kAnimSlots)
        _animBodySet[slot] = false;
}

void Enemy::SetAnimMelee(int slot, Rectangle relativeRect)
{
    if (slot < 0 || slot >= kAnimSlots)
        return;
    _animMeleeSet[slot] = true;
    _animMeleeRel[slot] = relativeRect;
}

void Enemy::ClearAnimMelee(int slot)
{
    if (slot >= 0 && slot < kAnimSlots)
        _animMeleeSet[slot] = false;
}

void Enemy::SetAnimDrawOffset(int slot, Vector2 offset)
{
    if (slot < 0 || slot >= kAnimSlots)
        return;
    _animDrawSet[slot]    = true;
    _animDrawOffset[slot] = offset;
}

bool Enemy::GetAnimBodyCapsuleWorld(Capsule2D& out) const
{
    int slot = GetCurrentAnimSlot();

    // A slot only counts if it has a POSITIVE radius — a body circle authored at
    // (or defaulting to) radius 0 must never be treated as valid, or the enemy would
    // have a zero-size hurtbox and be impossible to hit.
    auto usable = [this](int i) {
        return i >= 0 && i < kAnimSlots && _animBodySet[i] && _animBodyRadius[i] > 0.f;
    };

    // Gameplay body collision should stay anchored during normal attack poses.
    // The attack melee box remains authored per animation; this only prevents
    // the enemy's solid/hurt body from hopping when an attack body circle was
    // placed forward to line up with the weapon frame.
    if (slot == 2 && usable(0))
        slot = 0;
    else if (slot == 2 && usable(1))
        slot = 1;
    // If the current animation has no authored body circle, fall back to another
    // authored slot (Idle/slot 0 first, then the first set one) instead of failing.
    // The body barely moves between poses, so this keeps the enemy hittable in EVERY
    // animation — without this, an enemy tuned only in its Idle pose became effectively
    // invincible the moment it played its walk / attack / jump animation.
    if (!usable(slot))
    {
        slot = -1;
        if (usable(0))
            slot = 0;
        else
            for (int i = 0; i < kAnimSlots; ++i)
                if (usable(i)) { slot = i; break; }

        if (slot < 0)
            return false;   // no usable body circle on any slot — use other fallbacks
    }

    // Offsets are authored facing right; mirror X with the sprite.
    out = Capsule2D{
        { _worldPos.x + _animBodyOffset[slot].x * _rightLeft,
          _worldPos.y + _animBodyOffset[slot].y },
        0.f,
        _animBodyRadius[slot]
    };
    return true;
}

bool Enemy::GetAnimBodyRectWorld(Rectangle& out) const
{
    Capsule2D capsule;
    if (!GetAnimBodyCapsuleWorld(capsule))
        return false;

    // The hurt rect is the circle's bounding square so rect-based systems
    // (player melee, projectiles) match what the editor shows.
    out = Rectangle{
        capsule.center.x - capsule.radius,
        capsule.center.y - capsule.radius,
        capsule.radius * 2.f,
        capsule.radius * 2.f
    };
    return true;
}

bool Enemy::GetAnimMeleeRectWorld(int slot, Rectangle& out) const
{
    if (slot < 0 || slot >= kAnimSlots || !_animMeleeSet[slot])
        return false;

    Rectangle rel = _animMeleeRel[slot];
    if (_rightLeft < 0.f)
        rel.x = -(rel.x + rel.width);   // mirror around the sprite centre

    out = Rectangle{ _worldPos.x + rel.x, _worldPos.y + rel.y, rel.width, rel.height };
    return true;
}

Vector2 Enemy::GetCurrentAnimDrawOffset() const
{
    int slot = GetCurrentAnimSlot();
    if (slot < 0 || slot >= kAnimSlots || !_animDrawSet[slot])
        return Vector2{};
    return Vector2{ _animDrawOffset[slot].x * _rightLeft, _animDrawOffset[slot].y };
}

void Enemy::ResetTuningState()
{
    _hasTunedCollision = false;
    for (int i = 0; i < kAnimSlots; i++)
    {
        _editorAnimFrameTimes[i] = 0.f;
        _animBodySet[i]  = false;
        _animMeleeSet[i] = false;
        _animDrawSet[i]  = false;
    }
}

void Enemy::ApplyStoredTuning()
{
    const char* tuningName = GetTuningName();
    if (tuningName == nullptr)
        return;

    const CharacterTuning* tuning = CharacterTuningStore::Get(tuningName);
    if (tuning == nullptr)
        return;

    if (tuning->hasScale)
        _scale = tuning->scale;
    if (tuning->hasCollision)
        SetCollisionRecWorld(tuning->collisionRel);
    if (tuning->hasCapsule)
    {
        SetCapsuleRadius(tuning->capsuleRadius);
        SetCapsuleHalfHeight(tuning->capsuleHalfHeight);
        SetCapsuleOffset(tuning->capsuleOffset);
    }
    if (tuning->hasAttackBox)
    {
        SetAttackBoxWidth(tuning->attackBoxWidth);
        SetAttackBoxHeight(tuning->attackBoxHeight);
        SetAttackBoxOffsetX(tuning->attackBoxOffsetX);
        SetAttackBoxOffsetY(tuning->attackBoxOffsetY);
    }
    for (int i = 0; i < CharacterTuning::kMaxAnims && i < kAnimSlots; i++)
    {
        _editorAnimFrameTimes[i] = tuning->animFrameTime[i];

        if (tuning->animBody[i].set)
        {
            _animBodySet[i]    = true;
            _animBodyOffset[i] = Vector2{ tuning->animBody[i].x, tuning->animBody[i].y };
            _animBodyRadius[i] = tuning->animBody[i].radius;
        }
        if (tuning->animMelee[i].set)
        {
            _animMeleeSet[i] = true;
            _animMeleeRel[i] = tuning->animMelee[i].rect;
        }
        if (tuning->animDraw[i].set)
        {
            _animDrawSet[i]    = true;
            _animDrawOffset[i] = Vector2{ tuning->animDraw[i].x, tuning->animDraw[i].y };
        }
    }

    // Base grunt behaviour reads _attackUpdateTime directly, so the Attack
    // anim override (slot 2) maps onto it for types using the shared attack.
    if (_editorAnimFrameTimes[2] > 0.f)
        _attackUpdateTime = _editorAnimFrameTimes[2];
}

Rectangle Enemy::GetAttackCollisionRec() const
{
    // Per-animation melee box (Character Animator) wins; slot 2 = Attack.
    Rectangle animMeleeRect;
    if (GetAnimMeleeRectWorld(2, animMeleeRect))
        return animMeleeRect;

    // Attack box anchored to sprite center (_worldPos), independent of body offset.
    return Rectangle{
        _worldPos.x + _attackBoxOffsetX * _rightLeft - _attackBoxWidth  * 0.5f,
        _worldPos.y + _attackBoxOffsetY              - _attackBoxHeight * 0.5f,
        _attackBoxWidth,
        _attackBoxHeight
    };
}

void Enemy::PickApproachOffset()
{
    // 6 slots — 3 per side. Enemies cycle through them so each one picks a
    // unique spot: right side spreads NE/E/SE, left side spreads NW/W/SW.
    // This keeps enemies on the flanks while preventing them from stacking.
    static const Vector2 dirs[6] = {
        {  0.707f, -0.707f },   // right-up   (NE)
        {  1.000f,  0.000f },   // right       (E)
        {  0.707f,  0.707f },   // right-down (SE)
        { -0.707f, -0.707f },   // left-up    (NW)
        { -1.000f,  0.000f },   // left        (W)
        { -0.707f,  0.707f },   // left-down  (SW)
    };
    static int s_nextSlot = 0;
    int idx = s_nextSlot % 6;
    s_nextSlot++;
    _approachOffset = Vector2Scale(dirs[idx], _approachOffsetRadius);
    _approachOffsetTimer = _approachOffsetDuration + (float)GetRandomValue(0, 150) / 100.f;
}

void Enemy::Update(float dt, Vector2 heroWorldPos, Vector2 navigationTarget, bool hasNavigationTarget,
    const std::vector<std::unique_ptr<Enemy>>& enemies, const std::vector<Vector2>& propCenters)
{
    if (!_isActive)
        return;

    // UpdateDeath is intentionally NOT called here.
    // It is called once per frame in Engine::UpdateEnemyCount so the
    // drop world position can be captured before Death() teleports the enemy.

    _worldPosLastFrame = _worldPos;

    UpdateHit(dt);

    // Slide in the push direction; Engine::HandleCollisions stops us on walls/props.
    if (_forcedPushActive)
    {
        _worldPos = Vector2Add(_worldPos, Vector2Scale(_forcedPushDirection, _forcedPushSpeed * dt));
        return;
    }

    if (_attacking)
        _velocity = Vector2Zero();
    ApplyVelocity(dt);
    UpdateBurns(dt);
    UpdateElectricCharge(dt);
    UpdateLaunchVisual(dt);

    if (_freezeTimer > 0.f)
        _freezeTimer -= dt;
    if (_graveReviveInvulTimer > 0.f)
        _graveReviveInvulTimer -= dt;
    // Facing commitment timers — the lock only counts down once the attack
    // animation itself is over, so it acts as a post-attack recovery window.
    if (_facingLockTimer > 0.f && !_attacking)
        _facingLockTimer -= dt;
    if (_turnCommitCooldown > 0.f)
        _turnCommitCooldown -= dt;

    // Periodically repick approach direction so enemies shift positions
    // and don't permanently crowd one side of the player.
    _approachOffsetTimer -= dt;
    if (_approachOffsetTimer <= 0.f)
        PickApproachOffset();

    if (!_dying)
    {
        if (_target == nullptr)
            return;

        _attackCooldown -= dt;

        // A curated elite's signature move may own movement/attacks this frame
        // (telegraphs, committed charges, leaps). Ordinary enemies return false.
        if (UpdateEliteSignature(dt, navigationTarget, hasNavigationTarget, enemies, propCenters))
        {
            HandleAnimation(dt);
            return;
        }

        // Reinforcement arrival latch (Task 5): a freshly-telegraphed enemy
        // holds its spawn position/facing for a brief beat instead of moving
        // or attacking on its creation frame (design: "receives a short
        // arrival/orientation delay before it may move or attack"). Everything
        // else this frame (hit reactions, status ticks, animation) still runs
        // normally — only movement/attack intent is withheld.
        if (_arrivalTimer > 0.f)
            UpdateArrivalDelay(dt);
        else
        {
            HandleMovement(dt, navigationTarget, hasNavigationTarget, enemies, propCenters);
            HandleAttack(enemies);
        }

        // Hit knockback — recoil from a landed player hit. Moves the body directly
        // (Engine's collision pass still resolves walls/props afterwards) and
        // suppresses nav pursuit for the brief stagger so the shove actually reads
        // instead of the enemy walking straight back through it.
        if (_hitKnockbackTimer > 0.f)
        {
            _hitKnockbackTimer -= dt;
            _worldPos = Vector2Add(_worldPos, Vector2Scale(_hitKnockbackVel, dt));
            float decay = 1.f - kHitKnockbackDecay * dt;
            if (decay < 0.f) decay = 0.f;
            _hitKnockbackVel = Vector2Scale(_hitKnockbackVel, decay);
            _velocity = Vector2Zero();
        }
    }

    HandleAnimation(dt);
}

// Shared waypoint path helper used by all enemies that don't have their own
// fully custom nav stack. Ticks the refresh timer, rebuilds the waypoint list
// when needed, advances past reached waypoints, then returns the best next target.
Vector2 Enemy::ResolveNavTarget(float dt, Vector2 playerFeet,
                                Vector2 navigationTarget, bool hasNavigationTarget)
{
    _pathRefreshTimer -= dt;
    bool needsRefresh = (_pathRefreshTimer <= 0.f || _waypoints.empty());

    if (needsRefresh && _nav != nullptr)
    {
        _waypoints = _nav->GetWaypointPath(_worldPos, playerFeet, kMaxWaypoints);
        _waypointIndex = 0;
        _pathRefreshTimer = _pathRefreshInterval;
    }

    if (!_waypoints.empty())
    {
        const float waypointReachRadius = _nav ? _nav->GetCellSize() * 0.6f : 48.f;
        while (_waypointIndex < (int)_waypoints.size() - 1 &&
               Vector2Distance(_worldPos, _waypoints[_waypointIndex]) < waypointReachRadius)
        {
            _waypointIndex++;
        }
    }

    if (!_waypoints.empty() && _waypointIndex < (int)_waypoints.size())
        return _waypoints[_waypointIndex];
    if (hasNavigationTarget)
        return navigationTarget;
    return playerFeet;
}

// ── Facing & directional checks ──────────────────────────────────────────────
// Front/rear use a dot-product cone against the horizontal facing vector, so
// "behind" is a real 150° arc rather than an infinitely thin x-comparison edge.
bool Enemy::IsPositionInFront(Vector2 position, float coneDot) const
{
    Vector2 toPosition = Vector2Subtract(position, _worldPos);
    float length = Vector2Length(toPosition);
    if (length < 0.01f)
        return true;   // point-blank overlaps count as front (shield contact)
    float dot = (toPosition.x / length) * GetFacingSign();
    return dot >= coneDot;
}

bool Enemy::IsPositionBehind(Vector2 position, float rearDot) const
{
    Vector2 toPosition = Vector2Subtract(position, _worldPos);
    float length = Vector2Length(toPosition);
    if (length < 0.01f)
        return false;
    float dot = (toPosition.x / length) * GetFacingSign();
    return dot <= -rearDot;
}

void Enemy::FaceToward(float dx)
{
    if (fabsf(dx) < 0.01f)
        return;
    float desired = (dx < 0.f) ? -1.f : 1.f;
    if (desired == GetFacingSign())
        return;
    // Attacks and the recovery window freeze facing; walking flips are rate-
    // limited so an enemy can be circled instead of pivoting frame-perfectly.
    if (!CanTurnDuringCurrentState() || _turnCommitCooldown > 0.f)
        return;
    _rightLeft = desired;
    _turnCommitCooldown = _turnCommitInterval;
}

void Enemy::HandleMovement(float dt, Vector2 navigationTarget, bool hasNavigationTarget,
    const std::vector<std::unique_ptr<Enemy>>& enemies, const std::vector<Vector2>& propCenters)
{
    if (_target == nullptr || _dying)
        return;

    // Update attack-range hysteresis first — needed whether moving or attacking.
    // Zero velocity on the frame we enter attack range so there is no slide.
    {
        float distToPlayer = Vector2Length(Vector2Subtract(_target->GetFeetWorldPos(), _worldPos));
        if (!_inAttackRange)
        {
            if (distToPlayer <= _attackRange)
            {
                _inAttackRange = true;
                _velocity = Vector2Zero();
            }
        }
        else
        {
            _inAttackRange = (distToPlayer <= _attackRange + _attackRangeHysteresis);
        }
    }

    // Position is completely locked during attack — mirrors how Character::HandleMovement works.
    if (_attacking)
        return;

    Vector2 playerCenter = _target->GetFeetWorldPos();

    if (!_pendingBurns.empty() && !IsFrozen() && !_takingDamage)
    {
        UpdateBurnPanic(dt);
        Vector2 oldPos = _worldPos;
        _worldPos = Vector2Add(_worldPos, Vector2Scale(_burnPanicDir, _speed * 1.18f * dt));
        if (Vector2Length(Vector2Subtract(_worldPos, oldPos)) > 0.01f)
        {
            _texture = _walkAnim;
            if (_burnPanicDir.x < 0.f) _rightLeft = -1;
            if (_burnPanicDir.x > 0.f) _rightLeft = 1;
        }
        return;
    }

    // Choose movement target via the shared waypoint path helper.
    Vector2 targetPos = ResolveNavTarget(dt, playerCenter, navigationTarget, hasNavigationTarget);
    bool usingWaypoints = (!_waypoints.empty() && _waypointIndex < (int)_waypoints.size());
    // When no waypoints and no nav target, approach via the personal offset slot.
    if (!usingWaypoints && !hasNavigationTarget)
        targetPos = Vector2Add(playerCenter, _approachOffset);

    // ── Engagement-driven positioning (CombatEngagement / Task 3) ─────────────
    // CombatDirector assigns Commit/Support/Reposition once per frame and
    // pre-computes a role-appropriate point via ComputeEngagementTarget. This
    // enemy is reached here (HandleMovement) only while !_attacking, so
    // IsCommittedToAttack() is always false in this function — the one
    // remaining ambiguity is a genuinely APPROACHING Commit (about to attack,
    // latch not yet locked: keep the existing direct pathing below) versus a
    // Commit that is actually in its post-attack RECOVERY window (latch still
    // locked from a finished attack — see IsAttackLockedForEngagement). Only
    // the latter, plus real Support/Reposition, switch to the shared target.
    bool engagementRecovering = (_engagementIntent == EngagementIntent::Commit)
        && IsAttackLockedForEngagement() && !IsCommittedToAttack();
    bool useEngagementTarget = _hasEngagementAssignment
        && (_engagementIntent != EngagementIntent::Commit || engagementRecovering);

    if (useEngagementTarget)
    {
        // Refresh the held point on a slow timer so per-frame orbit-angle
        // noise doesn't make the enemy vibrate; it settles on one lane and
        // holds it for a while (design: "decelerates or holds at its
        // target"), matching _approachOffset's own re-pick cadence.
        _engagementHoldTimer -= dt;
        if (!_engagementHoldValid || _engagementHoldTimer <= 0.f)
        {
            _engagementHoldPos = _engagementTarget;
            _engagementHoldValid = true;
            _engagementHoldTimer = _engagementHoldDuration;
        }
        targetPos = _engagementHoldPos;
        _approachLineLocked = false;   // not on a Commit approach right now
    }
    else
    {
        _engagementHoldValid = false;   // fresh hold next time we disengage

        // Slime-style locked approach line: snapshot the first resolved
        // Commit-approach target and keep walking toward that fixed point
        // instead of re-tracking the player every frame, until in attack
        // range (HandleMovement returns early once _attacking starts).
        if (_hasEngagementAssignment && _engagementIntent == EngagementIntent::Commit &&
            LocksApproachLineOnCommit() && !_inAttackRange)
        {
            if (!_approachLineLocked)
            {
                _lockedApproachTarget = targetPos;
                _approachLineLocked = true;
            }
            targetPos = _lockedApproachTarget;
        }
        else
        {
            _approachLineLocked = false;
        }
    }

    // ── Squad role steering (see Balance::Squad) ──────────────────────────────
    // Reshapes WHERE this enemy wants to be based on its tactical role and the
    // shared battlefield read. Separation / prop / hazard shaping below still
    // applies on top, so packs flow around obstacles the same way individuals do.
    // Skipped while useEngagementTarget is driving targetPos: the shared
    // ComputeEngagementTarget already role-differentiates Support/Tank/
    // Assassin positioning (screening, anchoring, off-angle) for a non-
    // committing enemy, so this legacy pack heuristic would just fight it.
    bool holdStandoff = false;
    if (_squadDirective != nullptr && !_inAttackRange && !useEngagementTarget)
    {
        namespace Squad = Balance::Squad;
        float distToPlayerNow = Vector2Distance(_worldPos, playerCenter);

        switch (GetEncounterRole())
        {
        case EnemyRole::Grunt:
        case EnemyRole::Charger:
        {
            // Formation coherence: grunts still far from the fight rally toward
            // a slot behind the tank and advance as a pack instead of forming a
            // single-file conga line to the player.
            if (_squadDirective->hasLeader && distToPlayerNow > Squad::kLeaderBreakoffDist)
            {
                float leaderDistToPlayer = Vector2Distance(_squadDirective->leaderPos, playerCenter);
                float distToLeader = Vector2Distance(_worldPos, _squadDirective->leaderPos);
                if (distToLeader < Squad::kLeaderMaxRange &&
                    distToPlayerNow > leaderDistToPlayer + Squad::kLeaderFollowMargin)
                {
                    Vector2 leaderToPlayer = Vector2Subtract(playerCenter, _squadDirective->leaderPos);
                    if (Vector2Length(leaderToPlayer) > 0.01f)
                    {
                        Vector2 behindLeader = Vector2Scale(Vector2Normalize(leaderToPlayer),
                                                            -Squad::kLeaderSlotBehind);
                        // Personal approach offset spreads the pack across the
                        // tank's back line instead of stacking on one point.
                        Vector2 rallySlot = Vector2Add(
                            Vector2Add(_squadDirective->leaderPos, behindLeader),
                            Vector2Scale(_approachOffset, 0.5f));
                        targetPos = Vector2Lerp(targetPos, rallySlot, Squad::kLeaderPullWeight);
                    }
                }
            }
            // Wary standoff: when the pack has low confidence and an ally already
            // has the player engaged, hold the ring instead of piling on.
            if (_squadDirective->aggression < Squad::kWaryThreshold &&
                _squadDirective->playerEngaged &&
                distToPlayerNow < Squad::kStandoffRadius)
            {
                holdStandoff = true;
            }
            break;
        }
        case EnemyRole::Support:
        {
            // Supports fight from the back of the pack: drift toward the ally
            // mass, pushed to its far side from the player. Alone they fight
            // normally — a lone hanging-back Warchief would just be a statue.
            if (_squadDirective->allyCount >= Squad::kSupportMinAllies &&
                distToPlayerNow < Squad::kSupportHangBackDist)
            {
                Vector2 centroidFromPlayer = Vector2Subtract(_squadDirective->allyCentroid, playerCenter);
                if (Vector2Length(centroidFromPlayer) > 0.01f)
                {
                    Vector2 hangPoint = Vector2Add(_squadDirective->allyCentroid,
                        Vector2Scale(Vector2Normalize(centroidFromPlayer), 90.f));
                    targetPos = Vector2Lerp(targetPos, hangPoint, Squad::kSupportAllyPullWeight);
                }
            }
            break;
        }
        case EnemyRole::Assassin:
        {
            // Flank: while an ally holds the player's attention, aim PAST the
            // player so the approach curls onto their far side instead of
            // joining the frontal dogpile.
            if (_squadDirective->playerEngaged && distToPlayerNow > 80.f)
            {
                Vector2 throughPlayer = Vector2Subtract(playerCenter, _worldPos);
                if (Vector2Length(throughPlayer) > 0.01f)
                {
                    targetPos = Vector2Add(playerCenter,
                        Vector2Scale(Vector2Normalize(throughPlayer), Squad::kAssassinFlankDepth));
                }
            }
            break;
        }
        default:
            break;   // Tank/Ranged/Zoner/Summoner keep their own pursuit styles
        }
    }

    Vector2 toPlayer = Vector2Subtract(targetPos, _worldPos);

    Vector2 moveDir = Vector2Zero();

    // Arrival deadzone for a held, STATIONARY engagement point (Reposition/
    // Support/recovering-Commit): without this, a constant-speed walk toward
    // a fixed point with no deceleration overshoots it every frame, flips
    // direction, overshoots back, and repeats — the enemy visibly shakes in
    // place instead of settling. Only applies while useEngagementTarget is
    // actually holding a stationary point; a genuine Commit approach (chasing
    // the moving player) is unaffected and keeps its existing direct pathing.
    bool arrivedAtHoldPoint = useEngagementTarget &&
        Vector2Length(toPlayer) < Balance::Rhythm::kEngagementArrivalDeadzone;

    if (!arrivedAtHoldPoint && Vector2Length(toPlayer) > 0.01f)
        moveDir = Vector2Normalize(toPlayer);

    // Sporeling-style indirect approach: blend in a perpendicular component
    // while genuinely closing to attack (not orbiting an engagement target)
    // so the path curves instead of beelining. No-op for every other type
    // (GetApproachLateralBias() defaults to 0).
    {
        float lateralBias = GetApproachLateralBias();
        if (lateralBias != 0.f && !useEngagementTarget && !_inAttackRange && Vector2Length(moveDir) > 0.01f)
        {
            Vector2 perp{ -moveDir.y, moveDir.x };
            Vector2 biased = Vector2Add(moveDir, Vector2Scale(perp, lateralBias * _flankSide));
            if (Vector2Length(biased) > 0.01f)
                moveDir = Vector2Normalize(biased);
        }
    }

    // Blend a gentle pull toward this enemy's approach slot so enemies fan out
    // along the path rather than single-filing to the same waypoint cell.
    if (usingWaypoints && !hasNavigationTarget)
    {
        Vector2 slotTarget = Vector2Add(playerCenter, _approachOffset);
        Vector2 toSlot = Vector2Subtract(slotTarget, _worldPos);
        if (Vector2Length(toSlot) > 0.01f)
            moveDir = Vector2Add(moveDir, Vector2Scale(Vector2Normalize(toSlot), 0.3f));
    }

    Vector2 separation = Vector2Zero();
    Vector2 propSlide = Vector2Zero();

    for (const auto& enemy : enemies)
    {
        if (enemy.get() == this)
            continue;
        if (!enemy->IsActive() || enemy->IsDying() || !enemy->IsAlive())
            continue;

        float dist = Vector2Distance(_worldPos, enemy->_worldPos);

        if (dist < 130.f && dist > 0.f)
        {
            Vector2 away = Vector2Subtract(_worldPos, enemy->_worldPos);

            if (Vector2Length(away) > 0.01f)
            {
                float strength = (130.f - dist) / 130.f;
                separation = Vector2Add(separation, Vector2Scale(Vector2Normalize(away), strength));
            }
        }
    }

    // Prop repulsion — steer away from nearby pillars
    for (const Vector2& propCenter : propCenters)
    {
        float dist = Vector2Distance(_worldPos, propCenter);
        if (dist < 110.f && dist > 0.f)
        {
            Vector2 away = Vector2Subtract(_worldPos, propCenter);
            if (Vector2Length(away) > 0.01f)
            {
                away = Vector2Normalize(away);
                float strength = (110.f - dist) / 110.f;
                separation = Vector2Add(separation, Vector2Scale(away, strength * 1.8f));

                // Add a small tangential slide along the pillar so enemies keep
                // flowing around props instead of just pushing directly away
                // and bunching up at the same corner.
                Vector2 tangentA = { -away.y, away.x };
                Vector2 tangentB = { away.y, -away.x };
                float dotA = Vector2DotProduct(tangentA, moveDir);
                float dotB = Vector2DotProduct(tangentB, moveDir);
                Vector2 bestTangent = (dotA >= dotB) ? tangentA : tangentB;
                propSlide = Vector2Add(propSlide, Vector2Scale(bestTangent, strength));
            }
        }
    }

    // Hazard repulsion — enemies respect player-made damage zones (Consecrate,
    // poison pools...) the way they respect pillars, but much more strongly:
    // a hard shove away from the zone plus a tangential slide so they skirt the
    // edge toward the player instead of freezing at the rim. The pull toward the
    // player can still win when the player stands inside the zone — they hesitate
    // at the border rather than becoming permanently untouchable.
    if (_hazardZones != nullptr)
    {
        for (const HazardZone& hazard : *_hazardZones)
        {
            float avoidRange = hazard.radius + 70.f;   // margin outside the burn edge
            float dist = Vector2Distance(_worldPos, hazard.pos);
            if (dist >= avoidRange || dist <= 0.f)
                continue;

            Vector2 away = Vector2Subtract(_worldPos, hazard.pos);
            if (Vector2Length(away) <= 0.01f)
                continue;
            away = Vector2Normalize(away);
            float strength = (avoidRange - dist) / avoidRange;
            separation = Vector2Add(separation, Vector2Scale(away, strength * 3.2f));

            // Slide along whichever tangent keeps progress toward the target.
            Vector2 tangentA = { -away.y, away.x };
            Vector2 tangentB = { away.y, -away.x };
            Vector2 bestTangent = (Vector2DotProduct(tangentA, moveDir) >=
                                   Vector2DotProduct(tangentB, moveDir)) ? tangentA : tangentB;
            propSlide = Vector2Add(propSlide, Vector2Scale(bestTangent, strength * 1.2f));
        }
    }

    separation = Vector2Scale(separation, 0.80f);
    propSlide = Vector2Scale(propSlide, _propSlideStrength);
    moveDir = Vector2Add(moveDir, separation);
    moveDir = Vector2Add(moveDir, propSlide);

    if (Vector2Length(moveDir) > 0.01f)
        moveDir = Vector2Normalize(moveDir);

    // Wary standoff overrides pursuit: circle the ring around the player and
    // drift gently outward, waiting for the engaged ally's turn to end.
    if (holdStandoff)
    {
        Vector2 fromPlayer = Vector2Subtract(_worldPos, playerCenter);
        if (Vector2Length(fromPlayer) > 0.01f)
        {
            Vector2 radial  = Vector2Normalize(fromPlayer);
            Vector2 tangent = { -radial.y * _flankSide, radial.x * _flankSide };
            moveDir = Vector2Normalize(Vector2Add(tangent, Vector2Scale(radial, 0.35f)));
        }
    }

    Vector2 oldPos = _worldPos;
    // Legacy proximity-based flank-away-from-attack-range gate. Superseded by
    // the engagement system whenever an assignment exists this frame: holding
    // a genuine Commit approach already means this enemy IS the committer (no
    // need to flank off), and Support/Reposition are already being driven
    // toward their held engagement point above (useEngagementTarget), so the
    // player-relative flank offset below would just fight that. Only callers
    // that never build per-frame assignments (see HasEngagementAssignment()
    // callers in HandleAttack) still fall back to the old scan.
    bool slotAvailable = _hasEngagementAssignment ? true : CanTakeAttackSlot(enemies);
    bool shouldFlank = _inAttackRange && !slotAvailable && !useEngagementTarget;
    bool inAttackRange = _inAttackRange && !shouldFlank;

    if (shouldFlank)
    {
        Vector2 toPlayerNow = Vector2Subtract(playerCenter, _worldPos);
        if (Vector2Length(toPlayerNow) > 0.01f)
        {
            Vector2 forward = Vector2Normalize(toPlayerNow);
            Vector2 lateral = { -forward.y * _flankSide, forward.x * _flankSide };
            Vector2 desired = Vector2Add(Vector2Add(playerCenter, _approachOffset), Vector2Scale(lateral, _flankDistance));
            Vector2 flankDir = Vector2Subtract(desired, _worldPos);
            if (Vector2Length(flankDir) > 0.01f)
                moveDir = Vector2Normalize(flankDir);
        }
    }

    bool intentionalMove = false;
    if (!_takingDamage && !IsFrozen() && !inAttackRange)
    {
        // Warchief banner: allies inside the aura move noticeably faster.
        float moveSpeed = HasWarAura() ? _speed * kWarAuraSpeedMultiplier : _speed;

        // Threat tiers: a frenzied pack pushes faster, a wary one creeps; the
        // tank gets a small lead bonus when it has a pack to bring with it.
        if (_squadDirective != nullptr)
        {
            namespace Squad = Balance::Squad;
            if (_squadDirective->aggression >= Squad::kFrenzyThreshold)
                moveSpeed *= Squad::kFrenzySpeedMult;
            else if (_squadDirective->aggression < Squad::kWaryThreshold)
                moveSpeed *= Squad::kWarySpeedMult;

            if (GetEncounterRole() == EnemyRole::Tank &&
                _squadDirective->allyCount >= Squad::kTankLeadMinPack)
            {
                moveSpeed *= Squad::kTankLeadSpeedMult;
            }
        }

        _worldPos = Vector2Add(_worldPos, Vector2Scale(moveDir, moveSpeed * dt));
        intentionalMove = true;
    }

    // Position-level push: resolve physical overlap with other enemies.
    // minSep must be close to the body diameter or sprites visibly pile on top
    // of each other (the old 60 was far smaller than the ~80px bodies, so a pack
    // orbited the player as one blob). The resolve fraction is high enough that a
    // dense cluster actually spreads within a few frames instead of hovering at
    // the overlap threshold — both enemies in a pair run this, so 0.5 each frees
    // the full gap between any pair per frame.
    {
        const float minSep = 84.f;
        for (const auto& other : enemies)
        {
            if (other.get() == this) continue;
            if (!other->IsActive() || other->IsDying()) continue;

            float dist = Vector2Distance(_worldPos, other->_worldPos);
            if (dist < minSep && dist > 0.01f)
            {
                Vector2 push = Vector2Normalize(Vector2Subtract(_worldPos, other->_worldPos));
                _worldPos = Vector2Add(_worldPos, Vector2Scale(push, (minSep - dist) * 0.5f));
            }
        }
    }

    // Stuck detection — only runs when the enemy is actively trying to chase.
    // Skipped inside attack range since standing still there is intentional.
    if (!_takingDamage && !IsFrozen() && !inAttackRange)
    {
        _stuckTimer += dt;

        if (_stuckTimer >= _stuckThreshold)
        {
            float moved = Vector2Distance(_worldPos, _stuckCheckPos);

            if (moved < _stuckMinMove)
            {
                // Sample 8 directions and pick the one that points most toward
                // the player while avoiding the direction we're already stuck in.
                // This routes the enemy around the wall rather than bouncing off it.
                Vector2 toPlayer = Vector2Normalize(
                    Vector2Subtract(_target->GetFeetWorldPos(), _worldPos));
                float bestDot  = -2.f;
                Vector2 bestDir = { -moveDir.y, moveDir.x };   // perp fallback

                for (int d = 0; d < 8; d++)
                {
                    float angle = d * (PI / 4.f);
                    Vector2 candidate = { cosf(angle), sinf(angle) };
                    // Skip directions too close to the stuck heading
                    if (Vector2DotProduct(candidate, moveDir) > 0.7f) continue;
                    float dot = Vector2DotProduct(candidate, toPlayer);
                    if (dot > bestDot) { bestDot = dot; bestDir = candidate; }
                }

                _velocity = Vector2Add(_velocity, Vector2Scale(bestDir, _speed * 2.0f));
            }

            _stuckTimer    = 0.f;
            _stuckCheckPos = _worldPos;
        }
    }
    else
    {
        // Reset timer whenever frozen/attacking so the check stays meaningful
        _stuckTimer    = 0.f;
        _stuckCheckPos = _worldPos;
    }

    // Only switch to walk when the enemy is intentionally chasing and actually moving.
    if (!_takingDamage)
    {
        if (intentionalMove && Vector2Length(Vector2Subtract(_worldPos, oldPos)) > 0.01f)
        {
            _texture = _walkAnim;
            FaceToward(moveDir.x);   // rate-limited flip (facing commitment)
        }
        else
        {
            _texture = _idleAnim;
        }
    }
}

bool Enemy::CanTakeAttackSlot(const std::vector<std::unique_ptr<Enemy>>& enemies) const
{
    int committed = 0;
    constexpr int kMaxCommittedGrunts = 2;
    constexpr float kSlotRadius = 260.f;

    for (const auto& enemy : enemies)
    {
        const Enemy* other = enemy.get();
        if (other == this)
            continue;
        if (!other->IsActive() || other->IsDying() || !other->IsAlive())
            continue;
        if (Vector2Distance(_worldPos, other->_worldPos) > kSlotRadius)
            continue;
        if (other->_attacking)
            committed++;
    }

    // Blood frenzy (see Balance::Squad): a pack that smells a kill lets one
    // extra attacker commit, so pressure visibly ramps as the player weakens.
    int maxCommitted = kMaxCommittedGrunts;
    if (_squadDirective != nullptr &&
        _squadDirective->aggression >= Balance::Squad::kFrenzyThreshold)
    {
        maxCommitted += Balance::Squad::kFrenzyExtraAttackers;
    }

    return committed < maxCommitted;
}

void Enemy::ReleaseAttackCommitment()
{
    // Only start the post-attack recovery countdown if an attack was actually
    // in progress — calling EndCommit() unconditionally here would re-arm a
    // fresh recovery window every time this is called on an enemy that was
    // already idle (e.g. two interrupts landing back-to-back), extending its
    // downtime for no reason.
    if (_attacking)
        _engagementLatch.EndCommit(Balance::Rhythm::kPostCommitRepositionSeconds);
    _attacking = false;
}

Vector2 Enemy::ClampElitePathToNavigable(Vector2 start, Vector2 desired,
                                         const std::vector<Vector2>& propCenters,
                                         float propClearance) const
{
    if (_nav == nullptr || _nav->GetCellSize() <= 0.f)
        return desired;

    Vector2 delta = Vector2Subtract(desired, start);
    const float totalDistance = Vector2Length(delta);
    if (totalDistance < 0.01f)
        return desired;
    const Vector2 direction = Vector2Scale(delta, 1.f / totalDistance);

    auto pointIsBlocked = [&](Vector2 point)
    {
        const float cellSize = _nav->GetCellSize();
        const int column = (int)(point.x / cellSize);
        const int row    = (int)(point.y / cellSize);
        // Outside the room grid counts as blocked — never commit past a wall.
        if (column < 0 || row < 0 || column >= _nav->GetCols() || row >= _nav->GetRows())
            return true;
        if (_nav->IsCellBlocked(column, row))
            return true;
        for (const Vector2& propCenter : propCenters)
            if (Vector2Distance(point, propCenter) < propClearance)
                return true;
        return false;
    };

    // Walk the warned segment and stop at the LAST valid point before the
    // first blocked one, preserving the locked direction.
    constexpr float kStepDistance = 24.f;
    Vector2 lastValid = start;
    for (float along = kStepDistance; along <= totalDistance; along += kStepDistance)
    {
        Vector2 candidate = Vector2Add(start, Vector2Scale(direction, along));
        if (pointIsBlocked(candidate))
            return lastValid;
        lastValid = candidate;
    }
    return pointIsBlocked(desired) ? lastValid : desired;
}

bool Enemy::EmitEliteEvent(EliteSignatureEvent event)
{
    event.sequence = _eliteEventSequence++;
    event.phase = GetPhase();
    if (!_eliteEvents.Push(event))
    {
        _eliteDroppedEvents++;
        return false;
    }
    return true;
}

EliteSignatureTelemetry Enemy::GetEliteSignatureTelemetry() const
{
    EliteSignatureTelemetry telemetry;
    telemetry.phase = GetPhase();
    telemetry.casts = _eliteSignatureCasts;
    telemetry.hits = _eliteSignatureHits;
    telemetry.droppedEvents = _eliteDroppedEvents;
    return telemetry;
}

void Enemy::UpdateBurnPanic(float dt)
{
    _burnPanicTurnTimer -= dt;
    _burnSoundTimer -= dt;

    if (_burnPanicTurnTimer <= 0.f || Vector2Length(_burnPanicDir) < 0.01f)
    {
        float angle = (float)GetRandomValue(0, 359) * DEG2RAD;
        _burnPanicDir = { cosf(angle), sinf(angle) };
        _burnPanicTurnTimer = (float)GetRandomValue(18, 45) / 100.f;
    }

    if (_burnSoundTimer <= 0.f && _deathSound.frameCount > 0)
    {
        float pitch = (float)GetRandomValue(150, 210) / 100.f;
        SetSoundPitch(_deathSound, pitch);
        SetSoundVolume(_deathSound, 0.18f);
        PlaySound(_deathSound);
        _burnSoundTimer = (float)GetRandomValue(55, 110) / 100.f;
    }
}

void Enemy::HandleAttack(const std::vector<std::unique_ptr<Enemy>>& enemies)
{
    if (_dying || _target == nullptr || IsFrozen() || _takingDamage || !_pendingBurns.empty())
        return;

    float distance = Vector2Length(Vector2Subtract(_target->GetFeetWorldPos(), _worldPos));

    // Stable slot gate: prefer the per-frame CombatDirector assignment (this
    // frame's EngagementIntent::Commit) plus the latch's own eligibility check
    // (not still mid-attack or recovering) over the legacy proximity scan. The
    // legacy CanTakeAttackSlot scan is kept ONLY as a fallback for any caller
    // that never builds per-frame assignments for this enemy at all — e.g. a
    // boss (excluded from BuildEngagementAssignments' candidate pool, see
    // CombatDirector::UpdateEnemyRuntime) or an editor/tool path that ticks
    // Enemy::Update() directly without going through CombatDirector. Both
    // current in-game callers (Engine's dungeon-room and legacy wave-mode
    // paths) route through CombatDirector::UpdateEnemyRuntime, which now sets
    // an assignment for every alive, active, non-boss enemy every frame, so
    // this fallback branch should be rare in practice.
    bool canCommitAttack = HasEngagementAssignment()
        ? (GetEngagementIntent() == EngagementIntent::Commit && _engagementLatch.CanCommit())
        : CanTakeAttackSlot(enemies);

    if (distance <= _attackRange && !_attacking && _attackCooldown <= 0.f && canCommitAttack)
    {
        _attacking = true;
        _engagementLatch.BeginCommit();
        _damageApplied = false;
        _velocity = Vector2Zero();

        // Windup snapshot: one free flip toward the player as the swing starts,
        // then facing stays locked through the attack AND a recovery beat after
        // (the lock timer only ticks down once _attacking clears). Stepping
        // around a committed attacker is now a real dodge.
        Vector2 toPlayer = Vector2Subtract(_target->GetFeetWorldPos(), _worldPos);
        if (toPlayer.x < 0.f) _rightLeft = -1;
        else if (toPlayer.x > 0.f) _rightLeft = 1;
        _facingLockTimer = Balance::Facing::kAttackRecoveryFacingLock;

        _attackCooldown = _attackDelay;

        _texture = _attackAnim;
        _frame = 0;
        _runningTime = 0.f;

        _maxFrames = (int)(_texture.width / _width);
        _updateTime = _attackUpdateTime;

        PlayAttackSound();
    }

    if (_attacking && !_damageApplied && _frame == 2)
    {
        if (CheckCollisionRecs(GetAttackCollisionRec(), _target->GetCollisionRec()))
        {
            _target->TakeDamage((int)_attackPower, _worldPos);
            OnMeleeHitPlayer(_target);   // fire/status enemies burn/etc. on contact
            _damageApplied = true;
            PickApproachOffset();
        }
    }

    if (!_target->IsAlive())
    {
        ReleaseAttackCommitment();
        _texture = _idleAnim;
        _updateTime = 1.f / 8.f;
        _maxFrames = (int)(_texture.width / _width);
        _frame = 0;
        _runningTime = 0.f;
        _speed = 0.f;
    }
}
void Enemy::DrawEnemy(Vector2 heroWorldPos)
{
    if (!_isActive)
        return;

    float baseW = _width * _scale;
    float baseH = _height * _scale;
    float w = baseW;
    float h = baseH;

    // External launch effects briefly make enemies read as if they were thrown
    // upward by the ogre charge. The sprite grows slightly and lifts off the
    // ground, then settles back to normal as the timer expires.
    float launchRatio = (_launchVisualDuration > 0.f)
        ? (_launchVisualTimer / _launchVisualDuration)
        : 0.f;
    float launchScale = 1.f + _launchVisualScaleBoost * launchRatio;
    float launchLift = _launchVisualLift * launchRatio;
    w *= launchScale;
    h *= launchScale;

    // Death pop (juice): a killed enemy punches up in scale for an instant, then
    // deflates and fades over the death animation, so the kill reads as a payoff
    // instead of a flat death frame that just blinks out. Alpha folds into the
    // sprite tint below.
    float deathAlpha = 1.f;
    if (_dying)
    {
        float dp = 1.f - (_deathTimer / kDeathAnimDuration);   // 0 at death start -> 1 at end
        if (dp < 0.f) dp = 0.f; else if (dp > 1.f) dp = 1.f;
        float deathScale = 1.f + 0.30f * (1.f - dp) - 0.42f * dp;   // ~1.30 -> ~0.58
        w *= deathScale;
        h *= deathScale;
        deathAlpha = 1.f - dp * dp * 0.9f;                     // hold, then fade out late
    }

    Vector2 screenPos = Vector2Subtract(_worldPos, heroWorldPos);
    screenPos.x += kVirtualWidth / 2.f;
    screenPos.y += kVirtualHeight / 2.f - launchLift;

    Rectangle source{ _frame * _width, 0.f, _rightLeft * _width, _height };

    float visualOffsetX = (_texture.id == _attackAnim.id) ? _attackVisualOffsetX * _rightLeft : 0.f;
    float visualOffsetY = (_texture.id == _attackAnim.id) ? _attackVisualOffsetY              : 0.f;

    // Per-animation sprite offset authored in the Character Animator.
    Vector2 animDrawOffset = GetCurrentAnimDrawOffset();
    visualOffsetX += animDrawOffset.x;
    visualOffsetY += animDrawOffset.y;

    Rectangle dest{ screenPos.x - w / 2.f + visualOffsetX, screenPos.y - h / 2.f + visualOffsetY, w, h };

    // Attack swing weight (juice): the same anticipation→overshoot lean the player
    // has. Only while the shared attack sheet is playing and the enemy's role reads
    // as melee (see UsesAttackLunge) — bosses with bespoke attack anims and ranged
    // casters are left alone. Sprite-only (the shadow below stays planted); scaled
    // by sprite height so bigger enemies lean more.
    if (_attacking && !_dying && _texture.id == _attackAnim.id &&
        _maxFrames > 0 && _updateTime > 0.f && UsesAttackLunge())
    {
        float p = ((float)_frame + _runningTime / _updateTime) / (float)_maxFrames;
        if (p < 0.f) p = 0.f; else if (p > 1.f) p = 1.f;
        const float antic = baseH * 0.04f;   // lean back during the windup
        const float peak  = baseH * 0.14f;   // forward overshoot at the strike
        float swing = (p < 0.3f) ? -antic * (p / 0.3f)
                                 :  peak  * sinf(((p - 0.3f) / 0.7f) * 3.14159265f);
        dest.x += GetFacingSign() * swing;
    }

    if (_pitFalling)
    {
        const float p = PitFallProgress();
        const float visible = std::max(0.02f, 1.f - p);
        source.height = _height * visible;
        dest.x += w * 0.04f;
        dest.y += p * h * 0.42f;
        dest.width = w * 0.92f;
        dest.height = h * visible;
        DrawTexturePro(_texture, source, dest, {}, 0.f,
                       Fade(_pitFallTint, 1.f - p * 0.72f));
        return;
    }

    bool burning       = !_pendingBurns.empty();
    bool frozen        = IsFrozen();
    bool electroStunned = IsElectroStunned();
    bool charged       = _isCharged && !electroStunned;

    Color tint = electroStunned   ? Color{ 255, 255,  60, 255 } :   // bright yellow — stunned
                 charged         ? Color{ 220, 220,  80, 255 } :   // dim yellow — charged, not stunned
                 _flickerInWindup ? Color{ 180, 100, 255, 180 } :  // purple + semi-transparent — flicker windup
                 frozen          ? Color{ 140, 200, 255, 255 } :
                 burning         ? Color{ 255, 180, 180, 255 } :
                                   WHITE;

    if (burning)
    {
        for (int i = 0; i < 3; ++i)
        {
            float flickerX = (float)GetRandomValue(-14, 14);
            float flickerY = (float)GetRandomValue(-26, -4);
            DrawCircleV(Vector2{ screenPos.x + flickerX, screenPos.y + flickerY }, 5.f, Fade(ORANGE, 0.55f));
            DrawCircleV(Vector2{ screenPos.x + flickerX * 0.7f, screenPos.y + flickerY - 6.f }, 3.f, Fade(YELLOW, 0.45f));
        }
    }

    float shadowX = screenPos.x + visualOffsetX;
    float shadowY = screenPos.y + launchLift + baseH * 0.5f + visualOffsetY - 2.f;
    DrawEllipse((int)shadowX, (int)shadowY, baseW * 0.28f, baseH * 0.06f, Fade(BLACK, 0.28f));

    DrawTexturePro(_texture, source, dest, Vector2{}, 0.f, _dying ? Fade(tint, deathAlpha) : tint);

    // Graveyard revive invincibility window — pulsing green ring so it's obvious when testing.
    if (_graveReviveInvulTimer > 0.f)
    {
        float pulse = sinf((float)GetTime() * 10.f) * 0.4f + 0.6f;
        DrawCircleLines((int)screenPos.x, (int)screenPos.y, 55.f, Fade(Color{  80, 255, 120, 255 }, pulse));
        DrawCircleLines((int)screenPos.x, (int)screenPos.y, 42.f, Fade(Color{ 160, 255, 200, 255 }, pulse * 0.5f));
    }

    // Hunter Mark indicator — a pulsing red chevron hovering above a marked enemy,
    // so the player always knows which target their bonus damage applies to.
    if (IsMarked())
    {
        float bob    = sinf((float)GetTime() * 6.f) * 4.f;                 // gentle hover
        float pulse  = sinf((float)GetTime() * 9.f) * 0.25f + 0.75f;       // alpha pulse
        float tipY   = screenPos.y - h * 0.5f - 14.f + bob;                // chevron tip
        float half   = 11.f;                                               // half-width
        Color markColor = Fade(Color{ 255, 70, 70, 255 }, pulse);
        // Downward-pointing triangle (counter-clockwise winding so raylib fills it).
        DrawTriangle(Vector2{ screenPos.x - half, tipY - 14.f },
                     Vector2{ screenPos.x,        tipY },
                     Vector2{ screenPos.x + half, tipY - 14.f }, markColor);
        DrawTriangleLines(Vector2{ screenPos.x - half, tipY - 14.f },
                          Vector2{ screenPos.x,        tipY },
                          Vector2{ screenPos.x + half, tipY - 14.f }, Fade(BLACK, pulse * 0.6f));
    }

    // Warlock Curse indicator — a pulsing purple diamond above a cursed enemy.
    // Offset sideways when a Hunter Mark is also up so the two never overlap.
    if (IsCursed())
    {
        float bob    = sinf((float)GetTime() * 5.f) * 4.f;
        float pulse  = sinf((float)GetTime() * 8.f) * 0.25f + 0.75f;
        float cx     = screenPos.x + (IsMarked() ? 24.f : 0.f);
        float cy     = screenPos.y - h * 0.5f - 21.f + bob;   // diamond centre
        float half   = 8.f;
        Color curseColor = Fade(Color{ 190, 90, 255, 255 }, pulse);
        // Diamond = two triangles (counter-clockwise winding so raylib fills them).
        DrawTriangle(Vector2{ cx - half, cy }, Vector2{ cx, cy + half },
                     Vector2{ cx + half, cy }, curseColor);
        DrawTriangle(Vector2{ cx + half, cy }, Vector2{ cx, cy - half },
                     Vector2{ cx - half, cy }, curseColor);
    }

    // No empty health bar / elite label flashing over the death pop.
    if (_health != _maxHealth && !_dying)
        DrawHealthBar(screenPos, w, h);
    if (_isEliteMiniboss && !_dying)
        DrawEliteLabel(screenPos, w, h);
}

void Enemy::HandleAnimation(float dt)
{
    // Ogre launch reactions intentionally hold enemies on the second hurt
    // frame so the shove reads as a sustained airborne hit, not a normal
    // walk/idle transition.
    if (_launchHoldingHurtPose)
        return;

    _runningTime += dt;

    if (_runningTime >= _updateTime)
    {
        _runningTime = 0.f;
        _frame++;

        if (_frame >= _maxFrames)
        {
            if (_dying)
            {
                _frame = _maxFrames - 1;
                return;
            }

            // Frozen: hold on the last frame, no looping or state transitions
            if (IsFrozen())
            {
                _frame = _maxFrames - 1;
                return;
            }

            if (_takingDamage)
            {
                _takingDamage = false;
                _texture = _idleAnim;
                _updateTime = 1.f / 10.f;
                _maxFrames = (int)(_texture.width / _width);
                _frame = 0;
                return;
            }

            if (_attacking)
            {
                // Natural end of the attack animation — the canonical trigger
                // for entering the post-commit recovery window (see
                // ReleaseAttackCommitment / Balance::Rhythm::kPostCommitRepositionSeconds).
                ReleaseAttackCommitment();
                _texture = _idleAnim;
                _updateTime = 1.f / 10.f;
                _maxFrames = (int)(_texture.width / _width);
            }

            _frame = 0;
        }
    }
}

void Enemy::DrawHealthBar(Vector2 screenPos, float w, float h)
{
    if (_health <= 0)
        return;

    float healthPercent = (float)_health / (float)_maxHealth;

    float barWidth = w * 0.8f;

    float barX = screenPos.x - barWidth / 2.f;

    // Anchor the bar just above the actual body (top of the collision capsule),
    // NOT the padded sprite frame. Big/boss sprites have lots of empty frame above
    // the art, so the old "half the frame height" anchor floated their bars way up.
    // The capsule sits on the real character for every enemy, so this gives the same
    // small gap above the head that the legacy monsters have. (_healthBarYOffset is
    // still a per-enemy nudge in each ctor for the rare exception.)
    Capsule2D cap = GetCapsule();
    float bodyTopRelY = (cap.center.y - GetWorldPos().y) - cap.halfHeight - cap.radius;
    float barY = screenPos.y + bodyTopRelY - _healthBarYOffset;

    DrawRectangle((int)barX, (int)barY, (int)barWidth, (int)_healthBarHeight, RED);
    DrawRectangle((int)barX, (int)barY, (int)(barWidth * healthPercent), (int)_healthBarHeight, GREEN);

    // Phase notches: bosses and elites show their escalation gates (66%/33% or
    // the elite 50%) as ticks on the bar, so the player can SEE the next phase
    // coming and plan cooldowns around it — classic roguelite readability.
    if ((IsBoss() || _isEliteMiniboss) && !_phaseThresholds.empty())
    {
        for (float thresholdFraction : _phaseThresholds)
        {
            const int notchX = (int)(barX + barWidth * thresholdFraction);
            DrawRectangle(notchX - 1, (int)(barY - 2.f), 2,
                          (int)_healthBarHeight + 4, Color{ 20, 16, 12, 255 });
            DrawRectangle(notchX, (int)barY, 1,
                          (int)_healthBarHeight, Color{ 255, 220, 130, 255 });
        }
    }
}

void Enemy::SetSpriteSheet(const Texture2D& sheet, int frameCount, float frameTime, bool resetFrame)
{
    _texture    = sheet;
    _width      = (float)sheet.width / (float)frameCount;
    _height     = (float)sheet.height;
    _updateTime = frameTime;
    _maxFrames  = frameCount;
    if (resetFrame) { _frame = 0; _runningTime = 0.f; }
}

void Enemy::DrawEliteLabel(Vector2 screenPos, float w, float h)
{
    float barWidth = w * 0.8f;
    float barY = screenPos.y - h / 2.f - 12.f;
    Vector2 labelPos{ screenPos.x - barWidth * 0.5f, barY - 18.f };

    static Font font = GetFontDefault();
    static constexpr float kFontSize = 20.f;
    static constexpr float kSpacing = 1.f;

    // Unique entrance identity per curated elite: its title in a restrained
    // archetype accent, plus a matching underline that doubles as the
    // health-bar accent strip. Non-curated elites keep the plain gold ELITE.
    const char* text = "ELITE";
    Color accent{ 255, 210, 70, 255 };
    switch (GetEliteArchetype())
    {
    case EliteArchetype::Ogre:      text = "THE BATTERING RAM";   accent = Color{ 205, 120,  70, 255 }; break;  // rust
    case EliteArchetype::Infernal:  text = "THE LIVING FURNACE";  accent = Color{ 255, 100,  60, 255 }; break;  // red-orange
    case EliteArchetype::Bonechill: text = "THE FROZEN WALL";     accent = Color{ 140, 210, 255, 255 }; break;  // ice blue
    case EliteArchetype::Stormclub: text = "THE THUNDER BREAKER"; accent = Color{ 255, 225, 110, 255 }; break;  // electric gold
    case EliteArchetype::Venomfang: text = "THE AMBUSH PREDATOR"; accent = Color{ 150, 235, 100, 255 }; break;  // toxic green
    default: break;
    }

    DrawTextEx(font, text, { labelPos.x + 1.f, labelPos.y + 1.f }, kFontSize, kSpacing, Fade(BLACK, 0.6f));
    DrawTextEx(font, text, labelPos, kFontSize, kSpacing, accent);
    DrawLineEx({ labelPos.x, barY - 2.f }, { labelPos.x + barWidth, barY - 2.f }, 2.f, Fade(accent, 0.85f));
}

void Enemy::ApplyFreeze(float duration)
{
    if (_dying || !IsAlive())
        return;

    static constexpr float kMaxFreezeDuration = 10.f;
    float capped = std::min(duration, kMaxFreezeDuration);
    if (capped > _freezeTimer)
        _freezeTimer = capped;
}

void Enemy::TakeDamage(int damage, Vector2 attackerPos)
{
    // Fresh hit — clear any stale block reason from a previous damage source.
    _hitBlock = HitBlockReason::None;

    // The environment owns this death once the fall starts. Overlapping attacks
    // cannot interrupt it or display damage that was not actually applied.
    if (_pitFalling)
    {
        _hitBlock = HitBlockReason::Immune;
        return;
    }

    // If the revive one-shot invul window is active, deny the hit visibly so it
    // reads as protection instead of a broken/missing damage number.
    if (_graveReviveInvulTimer > 0.f)
    {
        _hitBlock = HitBlockReason::Immune;
        return;
    }

    // Bodyguard shield (immune while its fodder live) or leap wind-up i-frames
    // deny the hit outright. Flag it so the hit code shows "SHIELDED" feedback
    // instead of a damage number. TakeDamageUnblockable routes here too, so a
    // Shieldbearer's Enemy::TakeDamage call still respects an active shield.
    if (_isInvulnerable || _leapInvulnerable)
    {
        _hitBlock = HitBlockReason::Shielded;
        return;
    }

    // Guard Links elite modifier: while linked guards live, damage is visibly
    // REDUCED (never zeroed) — attacks always connect and always progress the
    // fight, unlike the old bodyguard invulnerability.
    if (_eliteGuardLinked && damage > 0)
    {
        damage = ApplyGuardLinkReduction(damage);
        _eliteGuardReducedHit = true;
    }

    // Check whether this hit would kill us and the revive is still available.
    bool wouldKill = (_health - damage <= 0);
    if (wouldKill && _graveReviveAvailable)
    {
        _graveReviveAvailable  = false;
        _graveReviveInvulTimer = 1.5f;
        _health                = _maxHealth * 0.5f;
        // Play hurt reaction so the revive is visually readable.
        _takingDamage = true;
        _texture      = _takeDamageAnim;
        _frame        = 0;
        _runningTime  = 0.f;
        _maxFrames    = (int)(_texture.width / _width);
        _hitTimer     = _maxFrames * _updateTime + 0.05f;
        return;
    }

    BaseCharacter::TakeDamage(damage, attackerPos);
}

void Enemy::StartFlickerWindup(float duration, Vector2 target)
{
    _flickerInWindup    = true;
    _flickerWindupTimer = duration;
    _flickerTarget      = target;
}

bool Enemy::ConsumeFlickerComplete()
{
    if (_flickerInWindup && _flickerWindupTimer <= 0.f)
    {
        _flickerInWindup = false;
        return true;
    }
    return false;
}

void Enemy::TickFlicker(float dt)
{
    if (_flickerCooldown > 0.f)    _flickerCooldown    -= dt;
    if (_flickerInWindup)           _flickerWindupTimer -= dt;
}

void Enemy::ApplyElectricCharge()
{
    if (_dying || !IsAlive())
        return;

    // Start the 10-second cap window on the very first charge application.
    if (!_isCharged)
        _electricChargeTotalTimer = 10.f;

    _isCharged    = true;
    _takingDamage = true;
    _texture      = _takeDamageAnim;
    _frame        = 0;
    _runningTime  = 0.f;
    _maxFrames    = (int)(_texture.width / _width);
    _hitTimer     = _maxFrames * _updateTime + 0.1f;
    // Schedule the next stun after this one finishes
    _chargeNextStunTime = (float)GetRandomValue(150, 400) / 100.f;
}

void Enemy::UpdateElectricCharge(float dt)
{
    if (!_isCharged || _dying || !IsAlive() || _takingDamage)
        return;

    _electricChargeTotalTimer -= dt;
    if (_electricChargeTotalTimer <= 0.f)
    {
        _isCharged = false;
        _electricChargeTotalTimer = 0.f;
        return;
    }

    _chargeNextStunTime -= dt;
    if (_chargeNextStunTime <= 0.f)
        ApplyElectricCharge();
}

void Enemy::StartForcedPush(Vector2 direction, float speed)
{
    _forcedPushDirection = (Vector2Length(direction) > 0.01f)
        ? Vector2Normalize(direction)
        : Vector2{ 1.f, 0.f };
    _forcedPushSpeed  = speed;
    _forcedPushActive = true;

    ReleaseAttackCommitment();
    _velocity  = Vector2Zero();
    _launchHoldingHurtPose = false;
    _launchVisualTimer     = 0.f;

    // Hold on frame 3 of the hurt animation while sliding.
    _texture     = _takeDamageAnim;
    _maxFrames   = _texture.width / _width;
    _frame       = std::min(3, _maxFrames - 1);
    _runningTime = 0.f;
    _updateTime  = 1.f / 12.f;
}

void Enemy::OnForcedPushCollision()
{
    if (!_forcedPushActive)
        return;

    UndoMovement();
    _forcedPushActive    = false;
    _forcedPushSpeed     = 0.f;
    _forcedPushDirection = Vector2Zero();
    _velocity            = Vector2Zero();

    _texture     = _idleAnim;
    _frame       = 0;
    _runningTime = 0.f;
    _maxFrames   = _texture.width / _width;
    _updateTime  = 1.f / 10.f;
}

void Enemy::ApplyExternalImpulse(Vector2 impulse, bool cancelLockedAnimation)
{
    // This helper lets special enemies, like the ogre, throw other enemies
    // around without needing to know their internal animation state. It also
    // starts a short launch visual so the shove reads as a heavy upward fling.
    _velocity = Vector2Add(_velocity, impulse);
    _launchVisualTimer = _launchVisualDuration;
    _launchHoldingHurtPose = true;
    _takingDamage = true;
    _texture = _takeDamageAnim;
    _frame = 1;
    _runningTime = 0.f;
    _updateTime = 1.f / 12.f;
    _maxFrames = (int)(_texture.width / _width);

    if (cancelLockedAnimation)
    {
        ReleaseAttackCommitment();
    }
}

void Enemy::ApplyHitKnockback(Vector2 dir, float speed)
{
    // Don't shove something the environment already owns (a fall/throw) or that is
    // already dead — that would fight those systems or move a corpse.
    if (_dying || !IsAlive() || _pitFalling || _forcedPushActive)
        return;

    const float len = Vector2Length(dir);
    if (len < 0.001f || speed <= 0.f)
        return;

    _hitKnockbackVel   = Vector2Scale(dir, speed / len);   // normalise * speed
    _hitKnockbackTimer = kHitKnockbackDuration;
}

void Enemy::UpdateLaunchVisual(float dt)
{
    if (_launchVisualTimer <= 0.f)
        return;

    _launchVisualTimer -= dt;
    if (_launchVisualTimer < 0.f)
        _launchVisualTimer = 0.f;

    if (_launchVisualTimer == 0.f && _launchHoldingHurtPose)
    {
        _launchHoldingHurtPose = false;
        _takingDamage = false;
        _texture = _idleAnim;
        _frame = 0;
        _runningTime = 0.f;
        _updateTime = 1.f / 10.f;
        _maxFrames = (int)(_texture.width / _width);
    }
}

void Enemy::SetWaveScale(int /*wave*/)
{
    // Fixed base profile — all stat growth comes from ApplyEnemyPowerLevel.
    // Wave parameter kept for virtual signature compatibility.
    _expValue    = Balance::Grunt::kBaseExpValue;
    _health      = Balance::Grunt::kBaseHealth;
    _maxHealth   = Balance::Grunt::kBaseHealth;
    _attackPower = Balance::Grunt::kBaseAttack;
    _speed       = Balance::Grunt::kBaseSpeed;
    _attackDelay = Balance::Grunt::kBaseAttackDelay;
}

void Enemy::ApplyEnemyPowerLevel(int enemyPowerLevel)
{
    // Single growth system: advances every few rooms (see
    // GetEnemyPowerLevelForWave). Steeper scaling gives the run a real
    // roguelite ramp — late-zone grunts are genuine threats, not fodder.
    // +16% HP, +8% damage, +4% speed per power level above 1.
    if (enemyPowerLevel <= 1)
        return;

    const float t = (float)(enemyPowerLevel - 1);
    _maxHealth   = std::ceil(_maxHealth   * (1.f + Balance::Curve::kHealthPerLevel * t));
    _health      = _maxHealth;
    _attackPower *= (1.f + Balance::Curve::kDamagePerLevel * t);
    _speed       *= (1.f + Balance::Curve::kSpeedPerLevel  * t);

    // Attack cadence pressure: deeper enemies strike more OFTEN, not just
    // harder — difficulty from tempo instead of ever-spongier HP. Floored so
    // no depth turns an authored rhythm into a blender.
    const float delayFactor = std::max(Balance::Curve::kMinAttackDelayFactor,
                                       1.f - Balance::Curve::kAttackDelayPerLevel * t);
    _attackDelay *= delayFactor;

    _expValue    += (enemyPowerLevel - 1);
}

void Enemy::ApplyDifficultyScaling(float healthMult, float damageMult)
{
    if (healthMult > 0.f && healthMult != 1.f)
    {
        _maxHealth = std::max(1.f, std::ceil(_maxHealth * healthMult));
        _health    = _maxHealth;
    }
    if (damageMult > 0.f && damageMult != 1.f)
        _attackPower *= damageMult;
}

void Enemy::ApplyBurn(float delay, int damage, Vector2 sourcePos)
{
    if (_dying || !IsAlive())
        return;

    static constexpr float kMaxBurnDelay = 10.f;
    if (delay > kMaxBurnDelay)
        return;

    _pendingBurns.push_back(PendingBurn{ delay, damage, sourcePos });
}

void Enemy::UpdateEnrageLatch(float dt)
{
    if (_enrageFlashTimer > 0.f)
        _enrageFlashTimer -= dt;

    if (_enrageThreshold <= 0.f)
        return;

    // Full HP → a fresh (or pooled-reused) spawn: clear the latch so last fight's
    // enrage doesn't carry over.
    if (_health >= _maxHealth)
    {
        _enrageLatched = false;
        return;
    }

    // One-way transition into the enrage phase.
    if (!_enrageLatched && IsAlive() && _health <= _maxHealth * _enrageThreshold)
    {
        _enrageLatched      = true;
        _enrageShakePending = true;   // telegraph consumed by CombatDirector
        _enrageFlashTimer   = 0.6f;
        _bossCallout        = "ENRAGED";   // floating word, consumed by the runtime
    }
}

bool Enemy::ConsumeEnrageShakeRequest()
{
    bool r = _enrageShakePending;
    _enrageShakePending = false;
    return r;
}

// ── Multi-phase boss system ──────────────────────────────────────────────────
void Enemy::SetPhaseThresholds(std::vector<float> descendingHpFractions)
{
    _phaseThresholds = std::move(descendingHpFractions);
    _phase = 0;
    _pendingPhaseChange = -1;
    _phaseTransitionTimer = 0.f;
}

void Enemy::UpdatePhaseLatch(float dt)
{
    if (_phaseTransitionTimer > 0.f)
        _phaseTransitionTimer -= dt;

    if (_phaseThresholds.empty())
        return;

    // Full HP → fresh/pooled spawn: reset so last fight's phase doesn't carry over.
    if (_health >= _maxHealth)
    {
        _phase = 0;
        ResetStatuses();   // also drop any status carried from a pooled previous life
        return;
    }

    // One-way: advance through each threshold the HP has dropped below. Guarded so a
    // single big hit that crosses several thresholds only announces the last one.
    while (IsAlive() && _phase < (int)_phaseThresholds.size() &&
           _health <= _maxHealth * _phaseThresholds[_phase])
    {
        _phase++;
        _pendingPhaseChange = _phase;   // boss announces / reacts via ConsumePhaseChange
        _bossCallout = "PHASE SHIFT";   // floating word, consumed by the runtime
    }
}

int Enemy::ConsumePhaseChange()
{
    int p = _pendingPhaseChange;
    _pendingPhaseChange = -1;
    return p;
}

// ── Shared status effects (ARPG combat-identity pass) ────────────────────────
void Enemy::ApplyPoison(int damagePerTick, float duration, int stacks)
{
    if (_dying || !IsAlive() || damagePerTick <= 0) return;
    if (IsBoss()) duration *= kBossStatusDurMult;
    const int maxStacks = IsBoss() ? 5 : 8;   // capped, harder to overwhelm bosses
    _poisonStacks  = std::min(maxStacks, _poisonStacks + std::max(1, stacks));
    _poisonPerTick = std::max(_poisonPerTick, damagePerTick);
    _poisonTimer   = std::max(_poisonTimer, duration);
    if (_poisonTickTimer <= 0.f) _poisonTickTimer = 0.5f;
}

void Enemy::ApplyBleed(int damagePerTick, float duration)
{
    if (_dying || !IsAlive() || damagePerTick <= 0) return;
    if (IsBoss()) duration *= kBossStatusDurMult;
    _bleedPerTick = std::max(_bleedPerTick, damagePerTick);
    _bleedTimer   = std::max(_bleedTimer, duration);
    if (_bleedTickTimer <= 0.f) _bleedTickTimer = 0.5f;
}

void Enemy::ApplySlow(float speedMult, float duration)
{
    if (_dying || !IsAlive()) return;
    if (IsBoss()) duration *= kBossStatusDurMult;
    speedMult = std::clamp(speedMult, 0.1f, 1.f);
    // Strongest slow currently active wins; refresh its duration.
    _slowMult  = (_slowTimer > 0.f) ? std::min(_slowMult, speedMult) : speedMult;
    _slowTimer = std::max(_slowTimer, duration);
}

void Enemy::ApplyVulnerability(float damageTakenMult, float duration)
{
    if (_dying || !IsAlive()) return;
    if (IsBoss()) duration *= kBossStatusDurMult;
    damageTakenMult = std::clamp(damageTakenMult, 1.f, 3.f);
    _vulnMult  = (_vulnTimer > 0.f) ? std::max(_vulnMult, damageTakenMult) : damageTakenMult;
    _vulnTimer = std::max(_vulnTimer, duration);
}

void Enemy::ApplyMark(float duration)
{
    if (_dying || !IsAlive()) return;
    if (IsBoss()) duration *= kBossStatusDurMult;
    _markTimer = std::max(_markTimer, duration);
}

float Enemy::GetStatusMoveSpeedMult() const
{
    return (_slowTimer > 0.f) ? _slowMult : 1.f;
}

void Enemy::ResetStatuses()
{
    _poisonTimer = _poisonTickTimer = 0.f; _poisonPerTick = 0; _poisonStacks = 0;
    _bleedTimer  = _bleedTickTimer  = 0.f; _bleedPerTick  = 0;
    _slowTimer   = 0.f; _slowMult = 1.f;
    _vulnTimer   = 0.f; _vulnMult = 1.f;
    _markTimer   = 0.f;
    _curseTimer  = 0.f;
}

void Enemy::UpdateStatuses(float dt)
{
    // Poison — stacking DoT.
    if (_poisonTimer > 0.f)
    {
        _poisonTimer -= dt;
        _poisonTickTimer -= dt;
        if (_poisonTickTimer <= 0.f && IsAlive() && !_dying)
        {
            _poisonTickTimer += 0.5f;
            TakeDamage(_poisonPerTick * std::max(1, _poisonStacks), _worldPos);
        }
        if (_poisonTimer <= 0.f) { _poisonStacks = 0; _poisonPerTick = 0; }
    }

    // Bleed — physical DoT, hits harder while the enemy is moving.
    if (_bleedTimer > 0.f)
    {
        _bleedTimer -= dt;
        _bleedTickTimer -= dt;
        if (_bleedTickTimer <= 0.f && IsAlive() && !_dying)
        {
            _bleedTickTimer += 0.5f;
            float dx = _worldPos.x - _worldPosLastFrame.x;
            float dy = _worldPos.y - _worldPosLastFrame.y;
            bool moving = (dx * dx + dy * dy) > 4.f;
            int dmg = moving ? (int)std::ceil(_bleedPerTick * 1.5f) : _bleedPerTick;
            TakeDamage(std::max(1, dmg), _worldPos);
        }
        if (_bleedTimer <= 0.f) _bleedPerTick = 0;
    }

    if (_slowTimer > 0.f) { _slowTimer -= dt; if (_slowTimer <= 0.f) _slowMult = 1.f; }
    if (_vulnTimer > 0.f) { _vulnTimer -= dt; if (_vulnTimer <= 0.f) _vulnMult = 1.f; }
    if (_markTimer > 0.f) { _markTimer -= dt; }
    if (_curseTimer > 0.f) { _curseTimer -= dt; }
}

void Enemy::UpdateBurns(float dt)
{
    // Warchief aura decay lives here because every enemy type calls
    // UpdateBurns each frame regardless of its custom Update logic.
    if (_warAuraTimer > 0.f)
        _warAuraTimer -= dt;

    // Same reasoning for the boss enrage latch — piggy-backs on the universal
    // per-frame hook so no boss needs its own call (Molarbeast, which doesn't
    // call UpdateBurns, invokes UpdateEnrageLatch directly).
    UpdateEnrageLatch(dt);
    UpdatePhaseLatch(dt);
    UpdateStatuses(dt);

    int writeIndex = 0;

    for (int i = 0; i < static_cast<int>(_pendingBurns.size()); ++i)
    {
        PendingBurn burn = _pendingBurns[i];
        burn.timer -= dt;

        if (burn.timer <= 0.f)
        {
            if (IsAlive() && !_dying)
                TakeDamage(burn.damage, burn.sourcePos);
            continue;
        }

        _pendingBurns[writeIndex++] = burn;
    }

    _pendingBurns.resize(writeIndex);
}

void Enemy::PlayAttackSound()
{
    float pitch = GetRandomValue(100, 140) / 100.f;
    SetSoundPitch(_attackSound, pitch);
    SetSoundVolume(_attackSound, 0.5f);
    PlaySound(_attackSound);
}

void Enemy::PlayDeathSound()
{
    // Family-based death (slime pop, spectral fade, metal clang, beast roar...)
    // via the shared SfxBank — replaces the old shared PlayerDeath.ogg so a dead
    // grunt no longer sounds like the hero dying.
    SfxBank::Get().PlayCreatureDeath(GetCreatureFamily());
}

void Enemy::PlayHurtSound()
{
    float pitch = GetRandomValue(140, 180) / 100.f;
    SetSoundPitch(_hurtSound, pitch);
    SetSoundVolume(_hurtSound, 0.85f);
    StopSound(_hurtSound);
    PlaySound(_hurtSound);
}

void Enemy::EnsureSharedResourcesLoaded()
{
    if (_sharedResourcesLoaded)
        return;

    _sharedIdleAnim = LoadTexture(AssetPath("Enemy/EnemyIdle.png").c_str());
    _sharedWalkAnim = LoadTexture(AssetPath("Enemy/EnemyWalk.png").c_str());
    _sharedAttackAnim = LoadTexture(AssetPath("Enemy/EnemyAttack.png").c_str());
    _sharedTakeDamageAnim = LoadTexture(AssetPath("Enemy/EnemyDamage.png").c_str());
    _sharedDeathAnim = LoadTexture(AssetPath("Enemy/EnemyDeath.png").c_str());
    _sharedAttackSound = LoadSound(AssetPath("Sounds/SwordSwipe2.ogg").c_str());
    _sharedHurtSound = LoadSound(AssetPath("Sounds/SmallMonsterDamage.ogg").c_str());
    _sharedDeathSound = LoadSound(AssetPath("Sounds/PlayerDeath.ogg").c_str());
    _sharedResourcesLoaded = true;
}

void Enemy::UnloadSharedResources()
{
    if (!_sharedResourcesLoaded)
        return;

    UnloadTexture(_sharedIdleAnim);
    UnloadTexture(_sharedWalkAnim);
    UnloadTexture(_sharedAttackAnim);
    UnloadTexture(_sharedTakeDamageAnim);
    UnloadTexture(_sharedDeathAnim);
    UnloadSound(_sharedAttackSound);
    UnloadSound(_sharedHurtSound);
    UnloadSound(_sharedDeathSound);

    _sharedIdleAnim = Texture2D{};
    _sharedWalkAnim = Texture2D{};
    _sharedAttackAnim = Texture2D{};
    _sharedTakeDamageAnim = Texture2D{};
    _sharedDeathAnim = Texture2D{};
    _sharedAttackSound = Sound{};
    _sharedHurtSound = Sound{};
    _sharedDeathSound = Sound{};
    _sharedResourcesLoaded = false;
}
