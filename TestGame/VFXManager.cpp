#include "VFXManager.h"
#include "GameBalance.h"
#include "VirtualCanvas.h"
#include "AnimationUtils.h"
#include "VirtualCanvas.h"
#include "raymath.h"
#include "VirtualCanvas.h"

#include <algorithm>
#include <cmath>

// ── Setup ─────────────────────────────────────────────────────────────────────

void VFXManager::Init(Texture2D* fireballCastTex,  Texture2D* fireballHitTex,
                      Texture2D* genericHitTex,    Texture2D* iceHitTex,
                      Texture2D* lightningCastTex, Texture2D* thunderHitTex,
                      Texture2D* healEffectTex)
{
    _fireballCastTex  = fireballCastTex;
    _fireballHitTex   = fireballHitTex;
    _genericHitTex    = genericHitTex;
    _iceHitTex        = iceHitTex;
    _lightningCastTex = lightningCastTex;
    _thunderHitTex    = thunderHitTex;
    _healEffectTex    = healEffectTex;
}

// ── Per-frame ─────────────────────────────────────────────────────────────────

void VFXManager::Update(float dt)
{
    for (auto& effect : _effects)
    {
        if (!effect.active)
            continue;

        effect.runningTime += dt;
        if (effect.runningTime >= effect.frameTime)
        {
            effect.runningTime = 0.f;
            effect.frame++;
            if (effect.frame >= effect.frameCount)
            {
                // Lingering decals loop; one-shot effects finish and are culled.
                if (effect.looping) effect.frame = 0;
                else                effect.active = false;
            }
        }

        // Lingering hazard decals expire on their own timer regardless of frame.
        if (effect.looping)
        {
            effect.lifeRemaining -= dt;
            if (effect.lifeRemaining <= 0.f)
                effect.active = false;
        }
    }

    _effects.erase(
        std::remove_if(_effects.begin(), _effects.end(),
            [](const AnimatedEffect& e) { return !e.active; }),
        _effects.end());

    // Impact sparks — fly out, slow down, fade.
    for (auto& s : _sparks)
    {
        s.timer += dt;
        s.pos = Vector2Add(s.pos, Vector2Scale(s.vel, dt));
        s.vel = Vector2Scale(s.vel, 1.f - 6.f * dt);   // drag
    }
    _sparks.erase(
        std::remove_if(_sparks.begin(), _sparks.end(),
            [](const Spark& s) { return s.timer >= s.life; }),
        _sparks.end());
}

void VFXManager::Draw(Vector2 worldOffset, Vector2 playerWorldPos, Vector2 playerCastOrigin)
{
    for (const auto& effect : _effects)
    {
        if (!effect.active || effect.texture == nullptr || effect.texture->id == 0)
            continue;

        Vector2 worldPos = effect.followPlayer
            ? (effect.followPlayerCenter
                ? Vector2Add(playerWorldPos,  effect.offset)
                : Vector2Add(playerCastOrigin, effect.offset))
            : effect.worldPos;

        Vector2 screenPos = Vector2Add(worldPos, worldOffset);
        screenPos.x += kVirtualWidth  / 2.f;
        screenPos.y += kVirtualHeight / 2.f;

        float rotation = atan2f(effect.direction.y, effect.direction.x) * RAD2DEG;
        Rectangle source = GetAnimationFrameRect(*effect.texture,
                                                  effect.frameWidth, effect.frameHeight,
                                                  effect.frame);
        Rectangle dest{
            screenPos.x,
            screenPos.y,
            effect.frameWidth  * effect.scale,
            effect.frameHeight * effect.scale
        };
        // Lingering decals fade out over their final second so they don't pop off.
        Color tint = effect.tint;
        if (effect.looping && effect.lifeRemaining < 1.f)
        {
            float k = (effect.lifeRemaining > 0.f) ? effect.lifeRemaining : 0.f;
            tint = Fade(effect.tint, (effect.tint.a / 255.f) * k);
        }
        DrawTexturePro(*effect.texture, source, dest,
            Vector2{ dest.width * 0.5f, dest.height * 0.5f }, rotation, tint);
    }
}

void VFXManager::DrawFloatingTexts(Vector2 worldOffset)
{
    float now = (float)GetTime();
    _floatingTexts.erase(
        std::remove_if(_floatingTexts.begin(), _floatingTexts.end(),
            [now](const FloatingText& ft)
            { return now - ft.spawnTime >= FloatingText::kLifetime; }),
        _floatingTexts.end());

    const float sw2 = kVirtualWidth  / 2.f;
    const float sh2 = kVirtualHeight / 2.f;

    // Impact sparks (drawn under the numbers).
    for (const auto& s : _sparks)
    {
        float st    = s.timer / s.life;
        float alpha = 1.f - st;
        float r     = 4.f * (1.f - st) + 1.f;
        DrawCircleV(Vector2{ s.pos.x + worldOffset.x + sw2, s.pos.y + worldOffset.y + sh2 },
                    r, Fade(s.color, alpha));
    }

    for (const auto& ft : _floatingTexts)
    {
        float t      = (now - ft.spawnTime) / FloatingText::kLifetime;
        float yOff   = -55.f * t;
        float screenX = ft.worldPos.x + worldOffset.x + sw2;
        float screenY = ft.worldPos.y + worldOffset.y + sh2 + yOff;

        // Spawn "pop": briefly overshoot the font size, then settle.
        float pop  = (t < 0.18f) ? (1.f + (0.18f - t) / 0.18f * 0.6f) : 1.f;
        int   fs   = (int)(22.f * ft.scale * pop);
        // Word labels (IMMUNE / BLOCKED / ...) render verbatim; otherwise the
        // numeric damage value is formatted.
        const char* txt = ft.label.empty() ? TextFormat("%d", ft.value)
                                            : ft.label.c_str();
        int   tw    = MeasureText(txt, fs);
        float alpha = 1.f - t;
        // Dark outline so big numbers read over any background.
        DrawText(txt, (int)(screenX - tw / 2.f) + 2, (int)screenY + 2, fs, Fade(BLACK, alpha * 0.6f));
        DrawText(txt, (int)(screenX - tw / 2.f),     (int)screenY,     fs, Fade(ft.color, alpha));
    }
}

void VFXManager::Clear()
{
    _effects.clear();
    _floatingTexts.clear();
}

// ── Spawn helpers ─────────────────────────────────────────────────────────────

void VFXManager::SpawnCastEffect(Character::CastType castType,
                                  Vector2 castOrigin, Vector2 facingDir)
{
    AnimatedEffect effect{};
    effect.followPlayer = true;
    effect.worldPos     = castOrigin;
    effect.direction    = facingDir;
    effect.active       = true;

    switch (castType)
    {
    case Character::CastType::FireSpread:
        effect.texture    = _fireballCastTex;
        effect.frameCount = 8;
        effect.scale      = 4.f;
        break;
    case Character::CastType::IceSpread:
        effect.texture    = _iceHitTex;
        effect.frameCount = 8;
        effect.scale      = 4.f;
        effect.tint       = Color{ 100, 200, 255, 255 };
        break;
    case Character::CastType::ElectricSpread:
        effect.texture    = _lightningCastTex;
        effect.frameCount = 8;
        effect.scale      = 4.f;
        break;
    default:
        effect.active = false;
        break;
    }

    if (effect.active)
        _effects.push_back(effect);
}

void VFXManager::SpawnHitEffect(Character::CastType castType,
                                 Vector2 worldPos, Vector2 direction,
                                 float scaleMultiplier)
{
    AnimatedEffect effect{};
    effect.followPlayer = false;
    effect.worldPos     = worldPos;
    effect.direction    = direction;
    effect.active       = true;
    effect.frameTime    = 1.f / 20.f;

    switch (castType)
    {
    case Character::CastType::None:
        effect.texture    = _genericHitTex;
        effect.frameCount = 5;
        effect.scale      = 3.5f;
        effect.tint       = Color{ 255, 150, 150, 255 };
        break;
    case Character::CastType::FireSpread:
        effect.texture    = _fireballHitTex;
        effect.frameCount = 6;
        effect.scale      = 4.f;
        effect.tint       = WHITE;
        break;
    case Character::CastType::IceSpread:
        effect.texture    = _iceHitTex;
        effect.frameCount = 5;
        effect.scale      = 4.f;
        effect.tint       = Color{ 100, 200, 255, 255 };
        break;
    case Character::CastType::ElectricSpread:
        // Real lightning burst (Thunder_Blast: 64px cells, 11 frames — same
        // layout the electric ultimate draws). Falls back to the generic hit
        // if the asset is missing so electric never draws nothing.
        if (_thunderHitTex && _thunderHitTex->id != 0)
        {
            effect.texture     = _thunderHitTex;
            effect.frameWidth  = 64;
            effect.frameHeight = 64;
            effect.frameCount  = 11;
            effect.scale       = 2.5f;
            effect.tint        = WHITE;
        }
        else
        {
            effect.texture    = _genericHitTex;
            effect.frameCount = 5;
            effect.scale      = 4.f;
            effect.tint       = Color{ 255, 230, 120, 255 };   // at least tint it electric
        }
        break;
    default:
        effect.active = false;
        break;
    }

    if (effect.active)
    {
        effect.scale *= std::max(0.1f, scaleMultiplier);
        _effects.push_back(effect);
    }
}

void VFXManager::SpawnHealEffect()
{
    if (_healEffectTex == nullptr || _healEffectTex->id == 0)
        return;

    AnimatedEffect effect{};
    effect.texture            = _healEffectTex;
    effect.followPlayer       = true;
    effect.followPlayerCenter = true;
    effect.offset             = Vector2{ 0.f, -20.f };
    effect.direction          = Vector2{ 1.f,   0.f };
    effect.tint               = WHITE;
    effect.frameCount         = 13;
    effect.frameTime          = 1.f / 16.f;
    effect.scale              = 4.5f;
    effect.active             = true;
    _effects.push_back(effect);
}

void VFXManager::SpawnFloatingText(Vector2 worldPos, int value, Color color, float scale)
{
    FloatingText ft;
    ft.worldPos  = worldPos;
    ft.value     = value * _damageNumberScale;   // cosmetic big-number scale (debug-tunable)
    ft.color     = color;
    ft.scale     = scale;
    ft.spawnTime = (float)GetTime();
    _floatingTexts.push_back(ft);
}

void VFXManager::SpawnFloatingLabel(Vector2 worldPos, const char* text, Color color, float scale)
{
    if (text == nullptr || text[0] == '\0')
        return;
    FloatingText ft;
    ft.worldPos  = worldPos;
    ft.label     = text;                  // shown verbatim; no damage-number scaling
    ft.color     = color;
    ft.scale     = scale;
    ft.spawnTime = (float)GetTime();
    _floatingTexts.push_back(ft);
}

void VFXManager::SpawnSpriteFx(Texture2D* strip, Vector2 worldPos, int frameCount,
                               float scale, float frameTime, Color tint, Vector2 direction)
{
    if (strip == nullptr || strip->id == 0 || frameCount <= 0)
        return;
    AnimatedEffect effect{};
    effect.texture     = strip;
    effect.worldPos    = worldPos;
    effect.direction   = direction;
    effect.tint        = tint;
    effect.scale       = scale;
    effect.frameTime   = frameTime;
    effect.frameWidth  = 64;   // owned FX strips are 64px cells
    effect.frameHeight = 64;
    effect.frameCount  = frameCount;
    effect.active      = true;
    _effects.push_back(effect);
}

void VFXManager::SpawnHazardDecal(Texture2D* strip, Vector2 worldPos, int frameCount,
                                  float scale, float duration, Color tint, float frameTime)
{
    if (strip == nullptr || strip->id == 0 || frameCount <= 0 || duration <= 0.f)
        return;
    AnimatedEffect effect{};
    effect.texture       = strip;
    effect.worldPos      = worldPos;
    effect.direction     = { 1.f, 0.f };   // ground decal, no rotation
    effect.tint          = tint;
    effect.scale         = scale;
    effect.frameTime     = frameTime;
    effect.frameWidth    = 64;             // owned FX strips are 64px cells
    effect.frameHeight   = 64;
    effect.frameCount    = frameCount;
    effect.looping       = true;
    effect.lifeRemaining = duration;
    effect.active        = true;
    _effects.push_back(effect);
}

void VFXManager::SpawnImpactBurst(Vector2 worldPos, Color color, int count, float speed)
{
    // Radial burst (kept for blocked-hit / non-directional callers).
    SpawnImpactBurst(worldPos, color, count, speed, Vector2{ 0.f, 0.f }, 0.f);
}

void VFXManager::SpawnImpactBurst(Vector2 worldPos, Color color, int count, float speed,
                                  Vector2 direction, float spreadRadians)
{
    const float dirLen = Vector2Length(direction);
    const bool  directional = dirLen > 0.001f;
    const float baseAng = directional ? atan2f(direction.y, direction.x) : 0.f;

    for (int i = 0; i < count; i++)
    {
        float ang;
        float spd;
        if (directional)
        {
            // Bias into a cone around the blow's travel — tighter, faster spray so
            // it reads as a cut kicking debris off the hit.
            ang = baseAng + ((float)GetRandomValue(-100, 100) / 100.f) * spreadRadians;
            spd = speed * (0.6f + (float)GetRandomValue(0, 100) / 100.f * 0.7f);
        }
        else
        {
            ang = (float)GetRandomValue(0, 628) / 100.f;
            spd = speed * (0.4f + (float)GetRandomValue(0, 100) / 100.f * 0.6f);
        }
        Spark s;
        s.pos   = worldPos;
        s.vel   = Vector2{ cosf(ang) * spd, sinf(ang) * spd };
        s.color = color;
        s.timer = 0.f;
        s.life  = 0.22f + (float)GetRandomValue(0, 100) / 100.f * 0.18f;
        _sparks.push_back(s);
    }
}

void VFXManager::SpawnSmokeBurst(Vector2 worldPos, Color color)
{
    // Upward-biased, low-speed, wide-cone puff through the existing bounded
    // spark pool — a thin semantic wrapper, no parallel particle system.
    SpawnImpactBurst(worldPos, color, /*count*/ 10, /*speed*/ 40.f,
                     /*direction*/ Vector2{ 0.f, -1.f }, /*spreadRadians*/ 1.1f);
}
