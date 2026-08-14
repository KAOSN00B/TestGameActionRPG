#pragma once
#include "raylib.h"
#include "Character.h"
#include <vector>
#include <string>

// ── VFXManager ────────────────────────────────────────────────────────────────
// Owns all animated sprite effects and floating damage/heal numbers.
// Engine owns the textures; VFXManager holds non-owning pointers to them.
// Engine calls:
//   _vfx.Init(...)                 once after textures are loaded
//   _vfx.Update(dt)                every frame from UpdateGamePlay
//   _vfx.Draw(offset, pos, cast)   every frame from DrawWorld
//   _vfx.DrawFloatingTexts(offset) every frame from DrawWorld (after Draw)
//   _vfx.Clear()                   on run reset / room change
// ─────────────────────────────────────────────────────────────────────────────
class VFXManager
{
public:
    // Call once after all VFX textures are loaded by Engine.
    void Init(Texture2D* fireballCastTex,  Texture2D* fireballHitTex,
              Texture2D* genericHitTex,    Texture2D* iceHitTex,
              Texture2D* lightningCastTex, Texture2D* thunderHitTex,
              Texture2D* healEffectTex);

    // Advance frame counters; remove finished effects.
    void Update(float dt);

    // Render animated sprite effects.
    // playerWorldPos / playerCastOrigin are the live positions for follow-player effects.
    void Draw(Vector2 worldOffset, Vector2 playerWorldPos, Vector2 playerCastOrigin);

    // Cull expired floating numbers then render remaining.
    void DrawFloatingTexts(Vector2 worldOffset);

    // Discard all active effects and floating texts (room reset / run reset).
    void Clear();

    // ── Spawn helpers ────────────────────────────────────────────────────────
    // Cast animation that follows the player's cast origin.
    void SpawnCastEffect(Character::CastType castType, Vector2 castOrigin, Vector2 facingDir);

    // Impact animation at a fixed world position.
    void SpawnHitEffect(Character::CastType castType, Vector2 worldPos,
                        Vector2 direction, float scaleMultiplier = 1.f);

    // Heal animation that follows the player's center.
    void SpawnHealEffect();

    // Floating damage number that rises and fades. `scale` enlarges crits/big
    // hits; the displayed value is cosmetically multiplied (see .cpp) so single-
    // digit balance still reads as satisfying RPG-sized numbers.
    void SpawnFloatingText(Vector2 worldPos, int value, Color color, float scale = 1.f);

    // Floating WORD label (e.g. "IMMUNE", "BLOCKED", "SHIELDED", "CRIT!").
    // Same rise-and-fade behaviour as a damage number, but shows text verbatim
    // and is NOT multiplied by the cosmetic damage-number scale. This is the
    // primitive that combat-readability callouts (blocked-damage feedback,
    // boss-state labels) are built on.
    void SpawnFloatingLabel(Vector2 worldPos, const char* text, Color color, float scale = 1.f);

    // A short burst of impact sparks flying out from a hit/kill.
    void SpawnImpactBurst(Vector2 worldPos, Color color, int count, float speed);
    // Directional variant: sparks spray in a cone around `direction` (the way the
    // blow travelled) instead of a full circle, so a hit reads as a cut/impact
    // rather than an explosion. A near-zero direction falls back to radial.
    void SpawnImpactBurst(Vector2 worldPos, Color color, int count, float speed,
                          Vector2 direction, float spreadRadians);

    // Reinforcement spawn telegraph puff (Task 5): a small burst of the same
    // bounded impact-spark pool, biased upward and slow, so a reinforcement's
    // Circle->Smoke transition reads as a puff of smoke rather than a hit
    // spark spray. Caller passes the tint (purple-gray per the design); no
    // new asset is loaded — this is a thin wrapper over the directional
    // SpawnImpactBurst above.
    void SpawnSmokeBurst(Vector2 worldPos, Color color);

    // Generic one-shot sprite FX from any 64px-cell horizontal strip (boss impacts,
    // ground bursts, etc.). Plays once and removes itself. The reusable path the FX
    // pass is built on — callers just hand it a loaded strip.
    void SpawnSpriteFx(Texture2D* strip, Vector2 worldPos, int frameCount,
                       float scale = 5.f, float frameTime = 1.f / 22.f,
                       Color tint = WHITE, Vector2 direction = { 1.f, 0.f });

    // Lingering ground HAZARD decal — a looping animated sprite pinned to a world
    // position for `duration` seconds, then culled (fading over its final second).
    // This is the path persistent damage zones (poison/acid/lava pools) use so they
    // read as real art instead of prototype Raylib circles; the gameplay hazard keeps
    // its own collision/damage and just spawns one of these for the visual. Warning
    // telegraphs stay simple shapes — only the persistent effect becomes a decal.
    void SpawnHazardDecal(Texture2D* strip, Vector2 worldPos, int frameCount,
                          float scale, float duration, Color tint = WHITE,
                          float frameTime = 1.f / 12.f);

    // Cosmetic damage-number multiplier (tuned live via the debug juice panel).
    void SetDamageNumberScale(int s) { _damageNumberScale = (s > 0) ? s : 1; }

private:
    struct AnimatedEffect
    {
        Texture2D* texture        = nullptr;
        Vector2    worldPos       = {};
        Vector2    offset         = {};
        Vector2    direction      = { 1.f, 0.f };
        Color      tint           = WHITE;
        float      scale          = 4.f;
        float      frameTime      = 1.f / 18.f;
        float      runningTime    = 0.f;
        int        frameWidth     = 32;
        int        frameHeight    = 32;
        int        frameCount     = 1;
        int        frame          = 0;
        bool       followPlayer      = false;
        bool       followPlayerCenter = false;
        bool       active         = false;
        // Lingering hazard decals loop their animation and live for a fixed
        // duration instead of playing once. lifeRemaining drives cull + fade-out.
        bool       looping        = false;
        float      lifeRemaining  = 0.f;
    };

    struct FloatingText
    {
        Vector2     worldPos  = {};
        int         value     = 0;
        std::string label;                 // when non-empty, drawn instead of `value`
        Color       color     = WHITE;
        float       spawnTime = 0.f;
        float       scale     = 1.f;
        static constexpr float kLifetime = 0.75f;
    };

    // Impact sparks (juice) — tiny particles that fly out and fade on hits.
    struct Spark
    {
        Vector2 pos{};
        Vector2 vel{};
        Color   color = WHITE;
        float   timer = 0.f;
        float   life  = 0.35f;
    };

    std::vector<AnimatedEffect> _effects;
    std::vector<FloatingText>   _floatingTexts;
    std::vector<Spark>          _sparks;
    int                         _damageNumberScale = 25;

    // Non-owning pointers — Engine is responsible for load/unload.
    Texture2D* _fireballCastTex  = nullptr;
    Texture2D* _fireballHitTex   = nullptr;
    Texture2D* _genericHitTex    = nullptr;
    Texture2D* _iceHitTex        = nullptr;
    Texture2D* _lightningCastTex = nullptr;
    Texture2D* _thunderHitTex    = nullptr;   // electric impact (Thunder_Blast, 64px cells)
    Texture2D* _healEffectTex    = nullptr;
};
