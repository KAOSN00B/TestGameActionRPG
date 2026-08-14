#define _CRT_SECURE_NO_WARNINGS
#include "DevTools.h"
#include "Engine.h"
#include "VillageAssetData.h"
#include "VirtualCanvas.h"

#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

#include "AnimationUtils.h"
#include "VirtualCanvas.h"
#include "AssetPaths.h"
#include "SfxBank.h"
#include "AttackTuning.h"
#include "ElementalCombos.h"
#include "CellPickup.h"
#include "VirtualCanvas.h"
#include "CapsuleCollision.h"
#include "VirtualCanvas.h"
#include "CutsceneAction.h"
#include "VirtualCanvas.h"
#include "NineSlice.h"
#include "VirtualCanvas.h"
#include "RoomLayout.h"
#include "RoomCollision.h"
#include "VirtualCanvas.h"
#include "raymath.h"
#include "rlgl.h"
#include "VirtualCanvas.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <queue>
#include <set>

namespace
{
    const char* kPoeRevivalLine =
        "How unfortunate. The dungeon claims another soul... but I am not finished "
        "with you yet, little mystic. Rise -- and tell me what you will become.";
    const char* kFirstZephLines[] = {
        "Oh. You must be new here.",
        "You'll find sanctuary here, at least for now. The monsters tend to keep their distance.",
        "If you've got coin, I can sell you supplies. Might even teach you a few abilities...",
        "For the right price, of course."
    };

    AbilityType StarterAbilityForClass(PlayerClass playerClass)
    {
        switch (playerClass)
        {
        case PlayerClass::Warrior: return AbilityType::WarCleave;
        case PlayerClass::Hunter:  return AbilityType::PiercingShot;
        case PlayerClass::Mage:    return AbilityType::FireBolt;
        case PlayerClass::Rogue:   return AbilityType::FanOfKnives;
        case PlayerClass::Paladin: return AbilityType::Smite;
        case PlayerClass::Warlock: return AbilityType::ShadowBolt;
        default:                   return AbilityType::FireBolt;
        }
    }

    Color FallSurfaceTint(FallSurface surface)
    {
        switch (surface)
        {
        case FallSurface::Water: return Color{ 150, 215, 255, 255 };
        case FallSurface::Lava:  return Color{ 255, 155, 75, 255 };
        default:                 return Color{ 185, 160, 215, 255 };
        }
    }

    const char* FallSurfaceMessage(FallSurface surface)
    {
        switch (surface)
        {
        case FallSurface::Water: return "You fell into the water";
        case FallSurface::Lava:  return "The lava burned you";
        default:                 return "You fell into the void";
        }
    }

    void DrawScrollingCheckerboard(float sw, float sh, Color dark, Color light, float speedX, float speedY, int cell = 80)
    {
        const int period = cell * 2;
        float t    = (float)GetTime();
        int   offX = (int)fmodf(t * speedX, (float)period);
        int   offY = (int)fmodf(t * speedY, (float)period);
        int   phaseX = offX / cell;
        int   phaseY = offY / cell;
        int   pixX   = offX % cell;
        int   pixY   = offY % cell;

        for (int gy = -1; gy <= (int)(sh / cell) + 1; gy++)
        {
            for (int gx = -1; gx <= (int)(sw / cell) + 1; gx++)
            {
                bool isDark = (((gx + phaseX) + (gy + phaseY)) % 2 + 2) % 2 == 0;
                DrawRectangle(gx * cell - pixX, gy * cell - pixY,
                    cell, cell, isDark ? dark : light);
            }
        }
    }
    // -- Resource economy constants --------------------------------------------
    // Mana is now primarily passive-regen (see Character::kManaRegenPerSecond).
    // Pickups are supplemental / precious, not the main resource source.
    // Boss fights suppress all timed and drop pickups ? they should be won on
    // build and execution, not on floor loot.
    //
    // Store hook: when a store is added between key waves, these intervals can
    // be raised further and the store becomes the main recovery vector.

    // Seconds between timed heal drops during normal waves.
    constexpr float kDefaultTimedPickupInterval = 45.f;

    // Drop chance per enemy kill (normal waves only ? boss fight suppresses drops).
    constexpr int kEnemyDropChancePercent = Balance::Economy::kHealDropChancePercent;

    // Poe's rotating greetings, shown on his Echoes (meta) screen. One is picked
    // each time the player opens his altar — gives the melancholy spirit a voice.
    const char* kPoeGreetings[] = {
        "\"Another echo returns to me. Rest a moment, wanderer.\"",
        "\"The fallen whisper your name. Spend well what they left behind.\"",
        "\"Death claimed their bodies. I keep the rest. Now choose.\"",
        "\"So many came before you. So few return. You... keep returning.\"",
        "\"I cannot leave this place - but you can. Carry their strength.\"",
        "\"Their echoes are heavy with regret. Make yours count.\"",
    };
    constexpr int kPoeGreetingCount = (int)(sizeof(kPoeGreetings) / sizeof(kPoeGreetings[0]));

    constexpr float kBiomeFadeOutDuration = 3.f;
    constexpr float kBiomeFadeInDuration = 1.f;

    // Spawn protection gives the player time to reposition if a wave enters on
    // top of them or very near the player after pathing/spawn validation.
    constexpr float kWaveSpawnProtectionDuration = 2.f;

    // Boss support adds keep the fight active, but they should respawn far
    // enough away that the player gets a real reposition window before the
    // next Ogre/Cyclops pressure cycle starts.
    constexpr float kBossSupportRespawnDelay = 28.f;
    constexpr float kBossSupportMinPlayerDistance = 520.f;
    // Ogre enters the boss fight on a delay so the player has time to read
    // the boss before juggling three threats simultaneously.
    constexpr float kBossOgreInitialDelay = 22.f;

    int ClampInt(int value, int minValue, int maxValue)
    {
        if (value < minValue)
            return minValue;
        if (value > maxValue)
            return maxValue;
        return value;
    }

    constexpr int kDungeonDoorSpanCols = 5;
    constexpr int kDungeonDoorSpanRows = 3;

    int GetDungeonDoorStartCol()
    {
        return ClampInt(RoomLayout::kCols / 2 - kDungeonDoorSpanCols / 2, 1, RoomLayout::kCols - 1 - kDungeonDoorSpanCols);
    }

    int GetDungeonDoorStartRow()
    {
        return ClampInt(RoomLayout::kRows / 2 - kDungeonDoorSpanRows / 2, 1, RoomLayout::kRows - 1 - kDungeonDoorSpanRows);
    }

    // Modifier accent colours, in EliteModifier order. The label/condition
    // STRINGS come from the shared EliteModifier* functions in EliteSignature
    // so every HUD surface teaches the same (current) rules.
    Color GetEliteModifierColor(int modifier)
    {
        constexpr Color kColors[(int)EliteModifier::Count] = {
            Color{220,  40, 200, 255},   // Cage
            Color{180, 100, 255, 255},   // Guard Links
            Color{255,  60,  60, 255},   // Enrage
            Color{255, 220,  80, 255},   // Arena Pressure
        };
        int idx = ClampInt(modifier, 0, (int)EliteModifier::Count - 1);
        return kColors[idx];
    }

    const char* GetDebugRoomTypeName(RoomType type)
    {
        switch (type)
        {
        case RoomType::Elite:    return "Elite";
        case RoomType::Rest:     return "Rest";
        case RoomType::Treasure: return "Treasure";
        case RoomType::Store:    return "Shop";
        case RoomType::Boss:     return "Boss";
        default:                 return "Standard";
        }
    }
}

// Biomes that have a saved TileMapper export and a tilesheet PNG.
// Add new entries here as tilesets are created and verified.
// DreamRealm: txt file contains Forest data (wrong biome selected at export) ? needs re-export.
static constexpr Biome kTilesetBiomes[]  = { Biome::Caverns, Biome::AncientCastle,
                                             Biome::DemonsInsides, Biome::Graveyard,
                                             Biome::Forest, Biome::Jungle, Biome::DreamRealm,
                                             Biome::Wastelands, Biome::LostCity,
                                             Biome::TheSanctuary };
static constexpr int   kTilesetBiomeCount = 10;

Engine::Engine()
    : _gameState(_runState.StateRef())
    , _howToPlayFrom(_runState.HowToPlayFromRef())
    , _htpTab(_runState.HowToPlayTabRef())
    , _htpSlideOffset(_runState.HowToPlaySlideOffsetRef())
{
    Init();
}

Engine::~Engine()
{
    _nav.CancelAndReset();

    Enemy::UnloadSharedResources();
    Cyclops::UnloadSharedResources();
    Ogre::UnloadSharedResources();
    Molarbeast::UnloadSharedResources();
    SkeletonArcher::UnloadSharedResources();
    FlameWisp::UnloadSharedResources();
    SlimeEnemy::UnloadSharedResources();
    Sporeling::UnloadSharedResources();
    Shieldbearer::UnloadSharedResources();
    Phantom::UnloadSharedResources();
    BomberImp::UnloadSharedResources();
    Warchief::UnloadSharedResources();
    LivingBlade::UnloadSharedResources();
    AbyssSlime::UnloadSharedResources();
    PumpkinJack::UnloadSharedResources();
    Minotaur::UnloadSharedResources();
    Werewolf::UnloadSharedResources();
    ChompBug::UnloadSharedResources();
    Osiris::UnloadSharedResources();
    TitanGuard::UnloadSharedResources();
    ToxicVermin::UnloadSharedResources();
    AncientBear::UnloadSharedResources();
    Infernal::UnloadSharedResources();
    Bonechill::UnloadSharedResources();
    Stormclub::UnloadSharedResources();
    Venomfang::UnloadSharedResources();
    HealPickup::UnloadSharedResources();
    GoldPickup::UnloadSharedResources();
    CellPickup::UnloadSharedResources();
    SpreadProjectile::UnloadSharedResources();
    LavaBallProjectile::UnloadSharedResources();
    EnemyProjectile::UnloadSharedResources();
    RoomHazardDirector::UnloadSharedTextures();
    for (int i = 0; i < (int)PlayerClass::Count; i++)
        if (_classPortraits[i].id != 0) UnloadTexture(_classPortraits[i]);
    if (_appearancePortrait.id != 0) UnloadTexture(_appearancePortrait);
    if (_cellsMerchantTex.id != 0) UnloadTexture(_cellsMerchantTex);
    UnloadVillagePlayground();
    if (_minionTex.id != 0) UnloadTexture(_minionTex);
    for (int i = 0; i < 7; i++)
        if (_relicIcons[i].id != 0) UnloadTexture(_relicIcons[i]);
    for (int i = 0; i < (int)AbilityType::Count; i++)
    {
        if (_abilityIcons[i].id != 0) UnloadTexture(_abilityIcons[i]);
        if (_abilityFx[i].id != 0) UnloadTexture(_abilityFx[i]);
    }
    for (Texture2D& bf : _bossFx)
        if (bf.id != 0) UnloadTexture(bf);
    if (_audioInitialised)
    {
        UnloadSound(_pickupSound);
        UnloadSound(_fireballCastSound);
        UnloadSound(_explosionSound);
        UnloadSound(_roomClearExplosionSound);
        UnloadSound(_lavaBallImpactSound);
        UnloadSound(_buttonPressSound);
        SfxBank::Get().Unload();
        _audio.Shutdown();
    }
    UnloadTexture(_map);
    UnloadTexture(_pillarTex);
    UnloadTexture(_torchTex);
    UnloadTexture(_pillarTorchTex);
    UnloadTexture(_treeTex);
    UnloadTexture(_smallTreeTex);
    UnloadTexture(_rockTex);
    UnloadTexture(_bigRockTex);
    UnloadTexture(_fireballCastTex);
    UnloadTexture(_fireballHitTex);
    UnloadTexture(_genericHitTex);
    UnloadTexture(_iceHitTex);
    UnloadTexture(_lightningCastTex);
    UnloadTexture(_thunderHitTex);
    UnloadTexture(_healEffectTex);
    UnloadTexture(_roomClearExplosionTex);
    UnloadTexture(_abilityIconFireTex);
    UnloadTexture(_abilityIconIceTex);
    UnloadTexture(_abilityIconElectricTex);
    UnloadTexture(_mapIconNormal);
    UnloadTexture(_mapIconElite);
    UnloadTexture(_mapIconShop);
    UnloadTexture(_mapIconTreasure);
    UnloadTexture(_mapIconBoss);
    UnloadTexture(_mapIconRest);
    UnloadTexture(_upgradeAttackPowerTex);
    UnloadTexture(_upgradeAttackRangeTex);
    UnloadTexture(_upgradeHealthTex);
    UnloadTexture(_upgradeMagicTex);
    UnloadTexture(_upgradeDefenseTex);
    UnloadTexture(_upgradeMoveSpeedTex);
    UnloadTexture(_shopBorderTex);
    UnloadTexture(_shopZephTex);
    UnloadTexture(_htpBorderTex);
    UnloadTexture(_magicGemTex);
    UnloadTexture(_bossBarrierTex);
    UnloadTexture(_swordCursorTex);
    _tileRenderer.Unload();
    _dungeonScrollTileRenderer.Unload();
    _pauseUI.Unload();
    UnloadTexture(_settingsBorderTex);
    UnloadRenderTexture(_virtualCanvas);
    if (_audioInitialised)
        CloseAudioDevice();
    CloseWindow();
}

// -- Cutscene definitions ------------------------------------------------------
// To add a new cutscene: define a new static array here and call
// _cutscene.Play(array, count) from wherever the scene should start.

// Played once per session, the first time the player enters the shop.
static const CutsceneAction kIntroCutscene[] =
{
    FadeIn(2.0f),
    Wait(0.5f),
    Dialogue("Zeph", "Ah... you're awake."),
    Dialogue("Zeph", "You were out cold when I found you. Three hours, give or take."),
    Dialogue("Zeph", "I don't know what brought you here. But I can sense it."),
    Dialogue("Zeph", "You're a Mystic."),
    Wait(0.4f),
    Dialogue("Zeph", "I can't do much for you. But I can offer this."),
    Dialogue("Zeph", "Take the sword. And choose one of these ? first spell's on me."),
    OpenAbilitySelect(),
    Dialogue("Zeph", "Good choice."),
    Wait(0.3f),
    Dialogue("Zeph", "Why am I here? Same reason as you ? trying to stop whatever's out there."),
    Dialogue("Zeph", "Now go. And be careful. The Onslaught doesn't wait."),
    UnlockDoor(),
    EndCutscene()
};

// Played when the player dies and respawns back in the shop.
static const CutsceneAction kRespawnCutscene[] =
{
    FadeIn(1.0f),
    Dialogue("Zeph", "You're back. I was starting to worry."),
    Dialogue("Zeph", "Take a breath. The dungeon will still be there."),
    EndCutscene()
};

// Played when arriving at Zeph in zones 2-5 (after the first intro).
static const CutsceneAction kZone2ArrivalCutscene[] =
{
    FadeIn(1.0f),
    Dialogue("Zeph", "Oh, great to see you again. I was worried about you."),
    Dialogue("Zeph", "You're pushing deeper than most. That takes guts."),
    EndCutscene()
};

static const CutsceneAction kZone3ArrivalCutscene[] =
{
    FadeIn(1.0f),
    Dialogue("Zeph", "Back again. You're tougher than I gave you credit for."),
    Dialogue("Zeph", "I can feel it now - something ancient is stirring ahead."),
    EndCutscene()
};

static const CutsceneAction kZone4ArrivalCutscene[] =
{
    FadeIn(1.0f),
    Dialogue("Zeph", "You made it this far. I won't pretend I'm not impressed."),
    Dialogue("Zeph", "Whatever's ahead... you'll need everything you've got."),
    EndCutscene()
};

static const CutsceneAction kZone5ArrivalCutscene[] =
{
    FadeIn(1.0f),
    Dialogue("Zeph", "I can't follow you any further."),
    Dialogue("Zeph", "Whatever's waiting in the Demon's Insides... only you can end this. Good luck."),
    EndCutscene()
};

void Engine::Init()
{
    InitWindow(_windowWidth, _windowHeight, "Mystic Onslaught");
    SetTargetFPS(60);

    // Load persisted settings and apply them before anything else draws.
    _settingsMgr.Load();
    _settingsMgr.ApplyWindow();

    // Load meta progression (banked cells + permanent unlocks) from disk.
    _meta.Load();

    // Virtual 1920x1080 canvas - everything draws here, then it is letterboxed onto the real window.
    _virtualCanvas = LoadRenderTexture(kVirtualWidth, kVirtualHeight);

#ifdef PLATFORM_WEB
    {
        int mobile = emscripten_run_script_int(
            "(/android|iphone|ipad|mobile/i.test(navigator.userAgent)) ? 1 : 0"
        );
        _touchModeActive = (mobile > 0);
    }
#endif

    EnsureAudioInitialized();

    SetExitKey(KEY_NULL);

    _pauseUI.Init();
    LoadKeybindings();

    _menu.Init();

    _pillarTex      = LoadTexture(AssetPath("TileSet/Pillar.png").c_str());
    _torchTex       = LoadTexture(AssetPath("TileSet/Torch.png").c_str());
    _pillarTorchTex = LoadTexture(AssetPath("TileSet/PillarTorch.png").c_str());
    _treeTex        = LoadTexture(AssetPath("ForestLevel/Tree.png").c_str());
    _smallTreeTex   = LoadTexture(AssetPath("ForestLevel/SmallTree.png").c_str());
    _rockTex        = LoadTexture(AssetPath("ForestLevel/Rock.png").c_str());
    _bigRockTex     = LoadTexture(AssetPath("ForestLevel/BigRock.png").c_str());
    _fireballCastTex = LoadTexture(AssetPath("PowerUps/Fireball_Cast.png").c_str());
    _fireballHitTex  = LoadTexture(AssetPath("PowerUps/Fireball_Hit.png").c_str());
    _genericHitTex   = LoadTexture(AssetPath("PowerUps/Hit03.png").c_str());
    _iceHitTex       = LoadTexture(AssetPath("PowerUps/Ice_Shard_Hit.png").c_str());
    // Was "LightningCast.png" (no underscore) — that file doesn't exist, so the
    // electric cast effect was silently invisible. The real asset is Lightning_Cast.png.
    _lightningCastTex = LoadTexture(AssetPath("PowerUps/Lightning_Cast.png").c_str());
    // Electric IMPACT sprite — electric hits used to reuse the red generic Hit03.
    // Thunder_Blast is a 64px-cell, 11-frame burst (same layout the electric
    // ultimate already draws, so the slicing is known-good).
    _thunderHitTex   = LoadTexture(AssetPath("PowerUps/Thunder_Blast.png").c_str());
    _healEffectTex = LoadTexture(AssetPath("PowerUps/Health_Up.png").c_str());
    _roomClearExplosionTex = LoadTexture(AssetPath("PowerUps/Flame_Explosion.png").c_str());
    _vfx.Init(&_fireballCastTex, &_fireballHitTex, &_genericHitTex,
              &_iceHitTex, &_lightningCastTex, &_thunderHitTex, &_healEffectTex);
    _damageNumbers.Init(GetFontDefault());
    DamageNumberSettings& damageSettings = _damageNumbers.GetSettings();
    damageSettings.visibleCap = Balance::DamageNumbers::kVisibleCap;
    damageSettings.minFontSize = Balance::DamageNumbers::kMinFontSize;
    damageSettings.maxFontSize = Balance::DamageNumbers::kMaxFontSize;
    damageSettings.damageReference = Balance::DamageNumbers::kDamageReference;
    damageSettings.riseSpeed = Balance::DamageNumbers::kRiseSpeed;
    damageSettings.lifetime = Balance::DamageNumbers::kLifetime;
    damageSettings.outlineOffset = Balance::DamageNumbers::kOutline;
    damageSettings.mergeWindow = Balance::DamageNumbers::kMergeWindow;
    _abilityIconFireTex     = LoadTexture(AssetPath("PowerUps/FireBallPickup.png").c_str());
    _abilityIconIceTex      = LoadTexture(AssetPath("PowerUps/IceSpellPickup.png").c_str());
    _abilityIconElectricTex = LoadTexture(AssetPath("PowerUps/LightningPickup.png").c_str());
    _settingsBorderTex      = LoadTexture(AssetPath("UI/SettingsBorder.png").c_str());
    _shopBorderTex          = LoadTexture(AssetPath("UI/PauseBoarder.png").c_str());
    _shopZephTex            = LoadTexture(AssetPath("UI/Zeph.png").c_str());
    _htpBorderTex           = LoadTexture(AssetPath("UI/HowToPlayBorder.png").c_str());
    _magicGemTex            = LoadTexture(AssetPath("TileSet/Key.png").c_str());
    _bossBarrierTex         = LoadTexture(AssetPath("TileSet/Barrier.png").c_str());
    _swordCursorTex         = LoadTexture(AssetPath("UI/CursorSword.png").c_str());
    if (_swordCursorTex.id != 0)
        SetTextureFilter(_swordCursorTex, TEXTURE_FILTER_POINT); // crisp pixels: the sprite is 32x32 pixel art scaled up
    _shop.Init(ShopTextures{
        &_shopBorderTex,
        &_shopZephTex,
        &_upgradeAttackPowerTex,
        &_upgradeAttackRangeTex,
        &_upgradeHealthTex,
        &_upgradeMagicTex,
        &_upgradeDefenseTex,
        &_upgradeMoveSpeedTex,
        &_abilityIconFireTex,
        &_abilityIconIceTex,
        &_abilityIconElectricTex,
        _abilityIcons,
        (int)AbilityType::Count
    });
    _shop.SetMetaProgression(&_meta);

    // Class-select portraits (idle sheet per class).
    for (int i = 0; i < (int)PlayerClass::Count; i++)
    {
        const char* prefix = GetPlayerClassInfo((PlayerClass)i).spritePrefix;
        _classPortraits[i] = LoadTexture(AssetPath(TextFormat("Hero/%s_Idle.png", prefix)).c_str());
    }

    // Poe — the ghostly spirit who keeps your Echoes (not Zeph). A spectral
    // Phantom sprite so he reads as a hovering spirit, matching Poe's purple.
    _cellsMerchantTex = LoadTexture(AssetPath("Enemy/PhantomIdle_B.png").c_str());
    _minionTex        = LoadTexture(AssetPath("Enemy/BomberImpIdle_B.png").c_str());   // Warlock imp

    // Relic icons (one per archetype).
    for (int i = 0; i < 7; i++)
        _relicIcons[i] = LoadTexture(AssetPath(TextFormat("PowerUps/Relic_%d.png", i)).c_str());

    // Per-ability icons + animated FX strips for non-elemental class abilities.
    for (int i = 0; i < (int)AbilityType::Count; i++)
    {
        const char* stem = GetAbilityIconStem((AbilityType)i);
        if (stem && stem[0] != '\0')
            _abilityIcons[i] = LoadTexture(AssetPath(TextFormat("PowerUps/Ability_%s.png", stem)).c_str());

        // FX strip: an Attack Editor override (fx=<stem>) replaces the default.
        const AttackTuning* t = AttackTuningStore::Get(AttackTuningKeyForAbility((AbilityType)i));
        const char* fxStem = (t && t->hasFx) ? t->fxStem.c_str() : stem;
        if (fxStem && fxStem[0] != '\0')
        {
            _abilityFx[i] = LoadTexture(AssetPath(TextFormat("PowerUps/FX_%s.png", fxStem)).c_str());
            if (_abilityFx[i].id != 0 && _abilityFx[i].height > 0)
                _abilityFxFrames[i] = _abilityFx[i].width / 64;   // 64px cells
        }
    }

    // Owned boss impact/cast FX sheets — order MUST match enum BossFx (CombatDirector.h).
    {
        static const char* kBossFxNames[] = {
            "SlimeSlam", "SlimeSplash", "AbyssSummon", "PounceImpact", "CrushingSlam", "BulwarkSlam",
            "ToxicEruption", "PoisonPool", "DreamPull", "DashDust", "HeavyStrike", "DiveImpact",
            "ClawSwipe", "BloodHowl", "DivineSlash", "SandStep", "TeleportStrike", "PumpkinSummon",
            "ChitinBurst",
        };
        const int n = (int)(sizeof(kBossFxNames) / sizeof(kBossFxNames[0]));
        _bossFx.assign(n, Texture2D{});
        _bossFxFrames.assign(n, 0);
        for (int i = 0; i < n; ++i)
        {
            _bossFx[i] = LoadTexture(AssetPath(TextFormat("PowerUps/FX_Boss%s.png", kBossFxNames[i])).c_str());
            if (_bossFx[i].id != 0 && _bossFx[i].height > 0)
                _bossFxFrames[i] = _bossFx[i].width / 64;   // 64px cells
        }
    }

    _mapIconNormal   = LoadTexture(AssetPath("TileSet/MapIcons/NormalRoom.png").c_str());
    _mapIconElite    = LoadTexture(AssetPath("TileSet/MapIcons/EliteRoom.png").c_str());
    _mapIconShop     = LoadTexture(AssetPath("TileSet/MapIcons/ShopRoom.png").c_str());
    _mapIconTreasure = LoadTexture(AssetPath("TileSet/MapIcons/TreasureRoom.png").c_str());
    _mapIconBoss     = LoadTexture(AssetPath("TileSet/MapIcons/BossRoom.png").c_str());
    _mapIconRest     = LoadTexture(AssetPath("TileSet/MapIcons/RestRoom.png").c_str());

    _upgradeAttackPowerTex = LoadTexture(AssetPath("UI/Upgrades/AttackPower.png").c_str());
    _upgradeAttackRangeTex = LoadTexture(AssetPath("UI/Upgrades/AttackRange.png").c_str());
    _upgradeHealthTex      = LoadTexture(AssetPath("UI/Upgrades/HealthUpgrade.png").c_str());
    _upgradeMagicTex       = LoadTexture(AssetPath("UI/Upgrades/Magic.png").c_str());
    _upgradeDefenseTex     = LoadTexture(AssetPath("UI/Upgrades/Defense.png").c_str());
    _upgradeMoveSpeedTex   = LoadTexture(AssetPath("UI/Upgrades/MoveSpeed.png").c_str());

    _props.clear();
    _pickups.clear();
    _spreadProjectiles.clear();
    _ultimateBlasts.clear();
    _lavaBalls.clear();
    _enemyProjectiles.clear();
    _poisonClouds.clear();
    _vfx.Clear();
    _damageNumbers.Clear();
    _enemies.clear();

    _pickupSpawnTimer = kDefaultTimedPickupInterval;

    ResetRunState();
}

void Engine::EnsureAudioInitialized()
{
    if (_audioInitialised)
        return;

    InitAudioDevice();
    _audioInitialised = true;

    // Give every streamed Music a larger ring buffer than raylib's 4096-frame
    // default. UpdateMusicStream refills the buffer once per frame on the main
    // thread, so a single long frame (entering a shop, loading textures, a
    // hitch) can drain a small buffer and produce an audible stutter — most
    // noticeable on the quiet menu/shop themes where nothing masks it. Doubling
    // the buffer gives roughly twice the headroom before an underrun. Must be
    // set before any LoadMusicStream call below.
    SetAudioStreamBufferSizeDefault(8192);

    _pickupSound       = LoadSound(AssetPath("Sounds/PickupSound.ogg").c_str());
    _fireballCastSound = LoadSound(AssetPath("Sounds/GS1_Spell_Fire.ogg").c_str());
    _explosionSound    = LoadSound(AssetPath("Sounds/GS1_Spell_Explode.ogg").c_str());
    _roomClearExplosionSound = LoadSound(AssetPath("Sounds/Explosion.ogg").c_str());
    {
        // Lavaball impact should only play for the first second of the clip.
        // Cropping the wave at load time keeps playback simple and lets each
        // collision just trigger a normal PlaySound call.
        Wave lavaImpactWave = LoadWave(AssetPath("Sounds/Explosion.ogg").c_str());
        int oneSecondFrame = lavaImpactWave.sampleRate;
        if (oneSecondFrame > 0 && (int)lavaImpactWave.frameCount > oneSecondFrame)
            WaveCrop(&lavaImpactWave, 0, oneSecondFrame);
        _lavaBallImpactSound = LoadSoundFromWave(lavaImpactWave);
        UnloadWave(lavaImpactWave);
    }
    _buttonPressSound = LoadSound(AssetPath("Sounds/ButtonPress.ogg").c_str());
    _audio.Init();
    // Categorized SFX library (per-class basics, element casts, creature/boss
    // families, pickups). Keep its master volume in sync with the SFX slider.
    SfxBank::Get().Load();
    SfxBank::Get().SetVolumeScale(_audio.GetSfxVolumeScale());

    SetSoundPitch(_buttonPressSound, 1.25f);
    SetSoundVolume(_buttonPressSound, 0.35f);

    SetSoundPitch(_pickupSound, 1.35f);
    SetSoundVolume(_pickupSound, 0.45f);
    SetSoundVolume(_lavaBallImpactSound, 0.45f);
    SetSoundVolume(_roomClearExplosionSound, 0.60f);

    // Reload player sounds that failed to load during Engine::Init() because
    // the audio device didn't exist yet (web deferred-init path).
    _player.ReloadSounds();

    // Apply saved volume settings now that audio is fully initialised.
    _settingsMgr.ApplyVolumes(_audio);
    ApplySfxVolume();
}

void Engine::StartVictoryMusic(MusicCue cue)
{
    if (cue == MusicCue::BattleVictory)
    {
        _audio.StartBattleVictory();
        return;
    }
    if (cue == MusicCue::BossVictory)
        _audio.StartBossVictory();
}

void Engine::ResetMusicState()
{
    _audio.Reset();
    _demoEndTouchHeld = false;
}

void Engine::UpdateMusicSystem()
{
    AudioContext ctx{};
    ctx.gameState = _gameState;
    ctx.howToPlayFrom = _howToPlayFrom;
    ctx.awaitingStartingAbility = _awaitingStartingAbility;
    ctx.currentRoomType = _currentRoomType;
    ctx.roomClearPending = _roomClearPending;
    ctx.bossFightActive = IsBossFightActive();
    // Combat = live enemies in the current gameplay room. Drives the biome
    // music swell (calmer when the room is clear / being explored).
    ctx.inCombat = (_gameState == GameState::DungeonRun || _gameState == GameState::Play)
                   && GetActiveEnemyCount() > 0;
    ctx.actBiome = GetBiomeForAct(_currentAct);
    ctx.currentBiome = _currentBiome;
    _audio.Update(ctx);
}

// -- Room progression ? replaces the old SpawnWave() system -------------------
// Called whenever the player is about to enter a specific room type.
// Handles act advancement, biome transitions, and encounter setup.
void Engine::StartNextRoom(RoomType type)
{
    _currentRoom++;
    if (_currentRoom > 6)
    {
        _currentRoom = 1;
        _currentAct++;
    }
    _wave++;  // _wave = total rooms entered ? feeds GetEnemyPowerLevelForWave()
    _currentRoomType = type;

    // Non-combat rooms get a rest timer before the choice screen appears.
    if (type == RoomType::Rest || type == RoomType::Store)
        _roomClearTimer = 5.0f;
    else
        _roomClearTimer = 0.f;

    _eliteRewardGranted      = false;
    // Clear all elite-room systems before room setup/spawns so the new room can
    // repopulate them correctly. Doing this after SpawnEnemies() breaks the
    // main-game elite flow by wiping the miniboss pointer/mechanic state while
    // leaving the spawned elite invulnerability intact.
    _eliteMechanic           = -1;
    _eliteMinibossPtr        = nullptr;
    _eliteCageRadius         = 0.f;
    _eliteEnrageWarningTimer = 0.f;
    _eliteHazardSpawnTimer   = 0.f;

    // Store room ? place Zeph at map centre and stock the shop
    if (type == RoomType::Store)
    {
        float mapW = _map.width  * _mapScale;
        float mapH = _map.height * _mapScale;
        _shop.Enter({ mapW * 0.5f, mapH * 0.40f }, _player, _currentAct);
    }

    std::string actName = GetBiomeName(GetBiomeForAct(_currentAct));
    _message = "Act " + std::to_string(_currentAct) + " - " + actName;

    _waveStarting   = true;
    _waveIntroTimer = 0.f;

    // Biome transition when the act changes
    Biome nextBiome = GetBiomeForAct(_currentAct);
    if (nextBiome != _currentBiome)
    {
        _pendingBiome              = nextBiome;
        _biomeTransitionActive     = true;
        _biomeTransitionSwapped    = false;
        _biomeTransitionTimer      = kBiomeFadeOutDuration + kBiomeFadeInDuration;
    }
}

void Engine::DebugStartRun()
{
    _isDailyRun = false;   // debug runs are never daily-seeded
    ResetRunState();
#if MO_DEV_TOOLS
    _debug.Activate();
#endif
    _awaitingStartingAbility = false;
    _starterAbilityGiftClaimed = false;
    _startingAbilityPickCount = 0;
    for (int i = 0; i < 6; i++)
        _startingAbilitySelected[i] = false;
    _levelUpReturnState = GameState::DungeonRun;
    _message = "Debug Dungeon";

    _dungeonGen.Generate();
    _currentBiome = kTilesetBiomes[GetRandomValue(0, kTilesetBiomeCount - 1)];
    _pendingBiome = _currentBiome;
    LoadTilesetForBiome(_currentBiome);

    int startIdx = _dungeonGen.GetStartIndex();
    EnterDungeonRoom(startIdx, DungeonDoorSide::None, GetDungeonEntranceSpawnPos(), true);

    _fadeInTimer = 1.0f;
    _fadeInDuration = 1.0f;
}


Vector2 Engine::GetDungeonEntranceSpawnPos() const
{
    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;
    const int startIdx = _dungeonGen.GetStartIndex();
    const auto& rooms = _dungeonGen.GetRooms();
    if (startIdx < 0 || startIdx >= (int)rooms.size())
        return { sw * 0.5f, sh - cellH * 2.2f };

    const DungeonRoom& entrance = rooms[(std::size_t)startIdx];
    if (entrance.row == 0)
        return { sw * 0.5f, cellH * 2.2f };
    if (entrance.col == 0)
        return { cellW * 2.2f, sh * 0.5f };
    if (entrance.col == DungeonGen::kGridSize - 1)
        return { sw - cellW * 2.2f, sh * 0.5f };
    return { sw * 0.5f, sh - cellH * 2.2f };
}

void Engine::EnterDungeonShopIfNeeded(const DungeonRoom& room)
{
    if (room.type != RoomType::Store)
        return;

    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;
    _shop.Enter({ sw * 0.5f, sh * 0.42f }, _player, _currentAct);

    // Zeph stabilizes a bad run, but does not erase the damage taken to get here.
    float maxHp = _player.GetMaxHealthValue();
    float curHp = _player.GetHealthValue();
    if (maxHp > 0.f && curHp < maxHp * 0.35f)
    {
        float missingHp = maxHp - curHp;
        _player.Heal((int)std::ceil(std::min(missingHp, maxHp * 0.20f)));
    }

    // Reaching Zeph banks all carried Echoes — the Dead Cells "made it to
    // the Collector" moment. Cells still carried when dying are lost instead.
    int carriedCells = _player.TakeCells();
    if (carriedCells > 0)
    {
        _meta.BankCells(carriedCells);
        _cellsBankedToastAmount = carriedCells;
        _cellsBankedToastTimer  = 4.f;
    }

    // New biome reached: the previous Cursed Wager expires and the shrine re-arms
    // so a fresh wager can be placed for the rooms ahead.
    _wagerTier = 0;
    _player.SetWagerRewardMult(1.f);
    _curseShrineUsed = false;
}

void Engine::RefreshHandcraftedRooms()
{
    _roomLibrary.Refresh(AssetFolderPath("Rooms"));
    _roomAssetCatalog.Refresh(AssetFolderPath("MapTilesets"));
    _tileRenderer.LoadRoomAssetCatalog(_roomAssetCatalog);
    _lastHandcraftedRoomId.clear();
}

bool Engine::GenerateHandcraftedDungeon(Biome biome,
                                        std::vector<std::string>& roomIds,
                                        int maxAttempts)
{
    roomIds.clear();
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        _dungeonGen.Generate();
        const auto& rooms = _dungeonGen.GetRooms();
        std::vector<std::string> candidateIds(rooms.size());
        bool complete = true;
        for (int i = 0; i < (int)rooms.size(); ++i)
        {
            const DungeonRoom& room = rooms[(std::size_t)i];
            const unsigned char mask = RoomDoorMask(
                room.hasNorth, room.hasSouth, room.hasEast, room.hasWest);
            std::vector<const RoomBlueprint*> candidates =
                _roomLibrary.PlaytestCandidates(biome, room.type, mask);
            if (candidates.empty())
            {
                complete = false;
                break;
            }
            const int pick = GetRandomValue(0, (int)candidates.size() - 1);
            candidateIds[(std::size_t)i] = candidates[(std::size_t)pick]->id;
        }
        if (complete)
        {
            roomIds = std::move(candidateIds);
            return true;
        }
    }
    return false;
}

RoomLayout Engine::BuildDungeonRoomLayout(int roomIdx, const TileDefSet& definitions,
                                          int visualVariant, int propDensityBonus)
{
    const auto& rooms = _dungeonGen.GetRooms();
    if (roomIdx < 0 || roomIdx >= (int)rooms.size()) return {};
    const DungeonRoom& room = rooms[(std::size_t)roomIdx];

    // Editor playtest loads the exact in-memory blueprint, including unsaved
    // edits. A bad blueprint must fail visibly, never turn into a procgen room.
    if (_editorPlaytestActive)
    {
        const RoomBlueprint* blueprint = nullptr;
        if (roomIdx == _editorPlaytestStartRoomIdx)
            blueprint = &_editorPlaytestBlueprint;
        else if (roomIdx >= 0 && roomIdx < (int)_editorPlaytestRoomIds.size())
            blueprint = _roomLibrary.FindById(_editorPlaytestRoomIds[(std::size_t)roomIdx]);

        if (blueprint == nullptr)
        {
            TraceLog(LOG_ERROR, "ROOM PLAYTEST: graph node %d has no handcrafted room", roomIdx);
            return {};
        }

        std::string warning;
        std::optional<RoomLayout> authored = BuildRoomLayout(
            *blueprint, definitions, warning, &_roomAssetCatalog);
        if (!warning.empty())
            TraceLog(authored.has_value() ? LOG_WARNING : LOG_ERROR,
                     "ROOM PLAYTEST: %s", warning.c_str());
        if (authored.has_value())
        {
            // Exact rooms already have this mask. A four-door safety room is
            // adapted here: only graph-required door zones can open; unused
            // sides keep their authored wall art and collision intact.
            const unsigned char mask = RoomDoorMask(
                room.hasNorth, room.hasSouth, room.hasEast, room.hasWest);
            ApplyActiveRoomDoorMask(*authored, mask);
            authored->visualVariant = visualVariant;
            return std::move(*authored);
        }
        return {};
    }

    if (_useHandcraftedDungeonRooms)
    {
        if (roomIdx >= 0 && roomIdx < (int)_handcraftedDungeonRoomIds.size())
        {
            const RoomBlueprint* selected =
                _roomLibrary.FindById(_handcraftedDungeonRoomIds[(std::size_t)roomIdx]);
            if (selected != nullptr)
            {
                std::string warning;
                std::optional<RoomLayout> authored = BuildRoomLayout(
                    *selected, definitions, warning, &_roomAssetCatalog);
                if (authored.has_value())
                {
                    ApplyActiveRoomDoorMask(*authored, RoomDoorMask(
                        room.hasNorth, room.hasSouth, room.hasEast, room.hasWest));
                    authored->visualVariant = visualVariant;
                    if (!warning.empty())
                        TraceLog(LOG_WARNING, "ROOM: %s", warning.c_str());
                    return std::move(*authored);
                }
            }
        }

        std::string mapperStem = GetBiomeName(_currentBiome);
        if (visualVariant >= 0 && visualVariant < (int)_dungeonVisualVariants.size())
            mapperStem = _dungeonVisualVariants[(std::size_t)visualVariant].mapperStem;

        RoomRequest request;
        request.biome = _currentBiome;
        request.tilesetStem = mapperStem;
        request.roomType = room.type;
        request.doorMask = RoomDoorMask(room.hasNorth, room.hasSouth,
                                        room.hasEast, room.hasWest);

        std::string selectedId;
        std::string warning;
        if (!_forcedHandcraftedRoomId.empty())
        {
            const RoomBlueprint* forced = _roomLibrary.FindById(_forcedHandcraftedRoomId);
            if (forced != nullptr && forced->biome == request.biome &&
                forced->tilesetStem == request.tilesetStem &&
                forced->roomType == request.roomType &&
                forced->DoorMask() == request.doorMask)
            {
                std::optional<RoomLayout> authored =
                    BuildRoomLayout(*forced, definitions, warning, &_roomAssetCatalog);
                if (authored.has_value())
                {
                    authored->visualVariant = visualVariant;
                    _lastHandcraftedRoomId = forced->id;
                    _forcedHandcraftedRoomId.clear();
                    if (!warning.empty())
                        TraceLog(LOG_WARNING, "ROOM: %s", warning.c_str());
                    return std::move(*authored);
                }
            }
        }
        std::optional<RoomLayout> authored = _roomLibrary.Resolve(
            request, definitions, &_roomAssetCatalog,
            _lastHandcraftedRoomId, selectedId, warning);
        if (authored.has_value())
        {
            authored->visualVariant = visualVariant;
            _lastHandcraftedRoomId = selectedId;
            if (!warning.empty())
                TraceLog(LOG_WARNING, "ROOM: %s", warning.c_str());
            return std::move(*authored);
        }

        // Authored mode never leaks back into generated floors, walls, props,
        // or decor. Missing content remains obvious while a region is authored.
        TraceLog(LOG_ERROR, "ROOM: no handcrafted layout for biome %d, node %d",
                 (int)_currentBiome, roomIdx);
        return {};
    }

    RoomLayout generated = RoomLayout::Generate(
        room.hasNorth, room.hasSouth, room.hasEast, room.hasWest, room.type,
        &definitions, propDensityBonus);
    generated.visualVariant = visualVariant;
    return generated;
}

void Engine::EnterDungeonRoom(int roomIdx, DungeonDoorSide entryDoorSide, Vector2 playerSpawnPos, bool resetRoomStates)
{
    const auto& rooms = _dungeonGen.GetRooms();
    if (roomIdx < 0 || roomIdx >= (int)rooms.size())
        return;

    const DungeonRoom& room = rooms[roomIdx];
    _dungeonRoomIdx = roomIdx;
    _dungeonEntryDoorSide = entryDoorSide;
    int visualVariant = GetDungeonVisualVariantForRoom(roomIdx);
    if (visualVariant != _currentDungeonVisualVariant)
    {
        LoadDungeonVisualVariantAssets(visualVariant, _tileDefs, _tileRenderer);
        _currentDungeonVisualVariant = visualVariant;
    }
    _dungeonScrollTileRenderer.Unload();
    // Forest/Jungle stay the densest biomes — smaller bonus now that every
    // biome's base prop counts were raised (footprint placement keeps it clean).
    int propDensityBonus = (_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle) ? 2 : 0;
    _dungeonRoomLayout = BuildDungeonRoomLayout(
        roomIdx, _tileDefs, visualVariant, propDensityBonus);

    if (resetRoomStates)
    {
        _dungeonRoomStates.clear();
        _hasMagicGem = false;
        _magicGemSpawned = false;
        _magicGemCollected = false;
        _bossBarrierUnlocked = false;
        _bossBarrierMessageTimer = 0.f;
        if (!_prologueActive && !_editorPlaytestActive)
            RollDungeonRoomSpecials();   // tag decision rooms for this fresh dungeon
    }

    _currentRoomType = room.type;
    _currentEncounterProfile = room.encounterProfile;
    _roomObjectiveComplete = false;
    _roomObjectiveTimer = 0.f;
    if (_currentEncounterProfile == EncounterProfile::Holdout)
    {
        DungeonRoomState& objectiveState = _dungeonRoomStates[_dungeonRoomIdx];
        if (objectiveState.holdoutTimeRemaining < 0.f)
            objectiveState.holdoutTimeRemaining = 30.f;
        _roomObjectiveTimer = objectiveState.holdoutTimeRemaining;
        _roomObjectiveComplete = _roomObjectiveTimer <= 0.f;
    }
    _currentRoom = (_currentRoomType == RoomType::Boss) ? 6 : std::max(1, _currentRoom + 1);
    if (resetRoomStates)
        _currentRoom = 1;

    // Roll this room's affix (Standard combat rooms only) before enemies spawn.
    if (_prologueActive || room.startsEmpty) _currentRoomAffix = RoomAffix::None;
    else RollRoomAffix(_dungeonRoomIdx, room.type);

    _dungeonEnemiesSpawned = false;
    _bossNoEnemyTimer      = 0.f;
    _dungeonScrolling = false;
    _dungeonView = DungeonView::Play;
    _gameState = GameState::DungeonRun;
    _roomClearPending = false;
    _waveStarting = false;
    _waveIntroTimer = 0.f;

    ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
    _roomCombatCapacity = RoomCapacityAnalyzer::Analyze(_dungeonRoomLayout, _tileDefs);
    // Decision rooms (Risk Shrine, …) are peaceful like Rest/Store: no enemies,
    // doors open. SpawnDungeonRoomEnemies() then skips them (it early-outs on cleared).
    RoomSpecialType roomSpecial = _dungeonRoomStates[_dungeonRoomIdx].special;
    if (room.type == RoomType::Rest || room.type == RoomType::Store ||
        (room.startsEmpty && !_prologueActive) ||
        roomSpecial != RoomSpecialType::None)
        _dungeonRoomStates[_dungeonRoomIdx].cleared = true;

    // Entering a fresh combat room arms any pending Risk Shrine contract onto the
    // enemies about to spawn; peaceful/cleared rooms clear the room levers instead.
    bool freshCombatRoom = roomSpecial == RoomSpecialType::None
                        && !(room.startsEmpty && !_prologueActive)
                        && !_dungeonRoomStates[_dungeonRoomIdx].cleared
                        && (room.type == RoomType::Standard || room.type == RoomType::Elite
                         || room.type == RoomType::Treasure || room.type == RoomType::Boss);
    if (freshCombatRoom) ActivatePendingModifiers();
    else { _roomModHpMult = 1.f; _roomModDmgMult = 1.f; }

    const float roomCellW = (float)kVirtualWidth / (float)RoomLayout::kCols;
    const float roomCellH = (float)kVirtualHeight / (float)RoomLayout::kRows;
    _dungeonRoomEntrySpawnPos = _dungeonRoomLayout.handcrafted
        ? FindNearestSafeRoomPosition(_dungeonRoomLayout, playerSpawnPos,
                                      roomCellW, roomCellH)
        : playerSpawnPos;
    _dungeonFallRecoveryCooldown = 0.f;
    _dungeonLastSafePos = _dungeonRoomEntrySpawnPos;   // edge-respawn seed
    _pitDragTimer = 0.f;
    _player.EndPitFall();   // never carry a fall between rooms
    _player.SetWorldPos(_dungeonRoomEntrySpawnPos);
    _player.SetCombatLocked(false);
    _player.SetDashAllowedWhileCombatLocked(false);
    _player.SetManaRegenPaused(false);
    // Sustain rework: entering a room no longer heals — HP carries between
    // rooms. Death still fully heals via Revive() on the village respawn path.
    _player.RefreshForRoomEntry();
    _roomLifestealHealed = 0;   // per-room sustain caps reset here
    _roomHealDrops       = 0;
    // Telemetry: stamp the room so the clear block can measure it.
    _roomEnterTime = GetTime();
    _roomEnterGold = _player.GetGold();
    _player.ResetTelemetryCounters();
    _cameraPos = { (float)kVirtualWidth * 0.5f, (float)kVirtualHeight * 0.5f };
    _props.clear();
    // Pending reinforcement waves and room hazards never follow the player
    // through a door — each room owns its own pressure.
    _dungeonReinforcements.clear();
    _pendingEnemySpawns.clear();
    _dungeonReinforcementTimer = 0.f;
    _roomHazards.ClearRoom();
    _roomPressureSpent  = 0;
    _roomPressureCapDbg = 0;

    RebuildDungeonNav();
    ClearDungeonEnemies();
    EnterDungeonShopIfNeeded(room);
    SpawnDungeonRoomEnemies();
    if (!_prologueActive) InitBiomeModifierRoom();

    // First visit: the map remembers this room's type, and special rooms
    // announce themselves with a temporary intro banner (standard rooms stay
    // silent — the HUD only speaks up for exceptions).
    bool firstVisitToRoom = !_dungeonRoomStates[_dungeonRoomIdx].visited;
    _dungeonRoomStates[_dungeonRoomIdx].visited = true;
    _roomIntroTimer = 0.f;
    if (firstVisitToRoom)
        QueueRoomIntroBanner();

    if (_dungeonRoomIdx == _dungeonGen.GetBossIndex())
        ApplyDungeonBossExitTiles(TileType::DoorLocked);

    // -- Cutscene triggers -----------------------------------------------------
    if (room.type == RoomType::Store)
    {
        if (!_cutsceneIntroPlayed)
        {
            // First time ever entering the shop ? play the full intro.
            _cutsceneIntroPlayed = true;
            SetStoreDoorTiles(TileType::DoorLocked);   // lock until ability is chosen
            _cutscene.Play(kIntroCutscene, (int)(sizeof(kIntroCutscene) / sizeof(kIntroCutscene[0])));
        }
        else if (resetRoomStates)
        {
            // Arriving at Zeph in a new zone (not a respawn) - zone-aware greeting.
            const CutsceneAction* seq = nullptr;
            int cnt = 0;
            switch (_worldZone)
            {
            case 2: seq = kZone2ArrivalCutscene; cnt = (int)(sizeof(kZone2ArrivalCutscene)/sizeof(*kZone2ArrivalCutscene)); break;
            case 3: seq = kZone3ArrivalCutscene; cnt = (int)(sizeof(kZone3ArrivalCutscene)/sizeof(*kZone3ArrivalCutscene)); break;
            case 4: seq = kZone4ArrivalCutscene; cnt = (int)(sizeof(kZone4ArrivalCutscene)/sizeof(*kZone4ArrivalCutscene)); break;
            case 5: seq = kZone5ArrivalCutscene; cnt = (int)(sizeof(kZone5ArrivalCutscene)/sizeof(*kZone5ArrivalCutscene)); break;
            default: break;
            }
            if (seq && cnt > 0)
                _cutscene.Play(seq, cnt);
        }
        else
        {
            // Returning after death - short respawn scene.
            _cutscene.Play(kRespawnCutscene, (int)(sizeof(kRespawnCutscene) / sizeof(kRespawnCutscene[0])));
        }
    }
}

void Engine::DebugSetEliteMechanic(int mechanic)
{
    _debug.SetForcedEliteMechanic(mechanic);
    DebugRestartRoomAs(RoomType::Elite);
}


void Engine::DebugRestartDungeonRoomAs(RoomType type)
{
    if (_dungeonRoomIdx < 0) return;

    ClearDungeonEnemies();
    _roomClearPending = false;
    _pendingExp = 0.f;
    _expTallyAccum = 0.f;
    _expTallyDone = false;
    _tallyChoiceChaining = false;
    _eliteRewardGranted = false;
    ResetEliteRoomRuntime();
    _dungeonClearEffects.clear();

    const auto& rooms = _dungeonGen.GetRooms();
    bool hasNorth = true;
    bool hasSouth = true;
    bool hasEast = true;
    bool hasWest = true;
    if (_dungeonRoomIdx >= 0 && _dungeonRoomIdx < (int)rooms.size())
    {
        const DungeonRoom& room = rooms[_dungeonRoomIdx];
        hasNorth = room.hasNorth;
        hasSouth = room.hasSouth;
        hasEast  = room.hasEast;
        hasWest  = room.hasWest;
    }

    _currentRoomType = type;
    _currentRoom = (_currentRoom % 5) + 1;
    if (type == RoomType::Boss)
        _currentRoom = 6;
    _message = std::string("Debug - ") + GetDebugRoomTypeName(type) + " Tile Room";

    _dungeonRoomLayout = RoomLayout::Generate(
        hasNorth, hasSouth, hasEast, hasWest, type, &_tileDefs);
    _dungeonRoomStates[_dungeonRoomIdx] = DungeonRoomState{};
    ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);

    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;
    _player.SetWorldPos({ sw * 0.5f, sh * 0.5f });
    _player.RefreshForRoomEntry();   // debug path — same no-heal rule as live rooms
    _player.GrantInvulnerability(kWaveSpawnProtectionDuration);
    _cameraPos = { sw * 0.5f, sh * 0.5f };

    RebuildDungeonNav();

    auto spawnAt = [&](auto spawnFn, int count = 1) {
        for (int n = 0; n < count; n++)
            spawnFn(GetDungeonSpawnPos(cellW, cellH));
    };

    if (type == RoomType::Boss)
    {
        SpawnMolarbeast({ sw * 0.5f, sh * 0.28f });
        ApplyDungeonBossExitTiles(TileType::DoorLocked);
        _dungeonEnemiesSpawned = true;
    }
    else if (type == RoomType::Elite)
    {
        Enemy* miniboss = SpawnEliteMiniboss(GetDungeonSpawnPos(cellW, cellH));
        spawnAt([&](Vector2 p) { SpawnBasicEnemy(p); }, 2);
        // One shared setup path — called after ALL room enemies exist so
        // Guard Links can count the living guards.
        InitializeEliteRoomRuntime(miniboss, _dungeonRoomIdx, sw, sh);
        _dungeonEnemiesSpawned = true;
    }
    else if (type == RoomType::Standard)
    {
        spawnAt([&](Vector2 p) { SpawnBasicEnemy(p); }, 2);
        SpawnCyclops(GetDungeonSpawnPos(cellW, cellH));
        _dungeonEnemiesSpawned = true;
    }
    else if (type == RoomType::Treasure)
    {
        spawnAt([&](Vector2 p) { SpawnBasicEnemy(p); }, 2);
        SpawnCyclops(GetDungeonSpawnPos(cellW, cellH));
        _dungeonEnemiesSpawned = true;
    }
    else
    {
        _dungeonRoomStates[_dungeonRoomIdx].cleared = true;
        ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
    }

    _dungeonScrolling = false;
    _dungeonView = DungeonView::Play;
    _gameState = GameState::DungeonRun;
}
void Engine::DebugRestartRoomAs(RoomType type)
{
    if (_gameState == GameState::DungeonRun)
    {
        DebugRestartDungeonRoomAs(type);
        return;
    }
    for (auto& enemy : _enemies)
    {
        enemy->SetActive(false);
        enemy->Teleport(Vector2{ -5000.f, -5000.f });
    }

    _pickups.clear();
    _spreadProjectiles.clear();
    _ultimateBlasts.clear();
    _vfx.Clear();
    _damageNumbers.Clear();
    _cyclopsLasers.clear();
    _lavaBalls.clear();
    _enemyProjectiles.clear();
    _poisonClouds.clear();
    ClearBossSupportAdds();
    _bossCyclopsSupport.enemy = nullptr;
    _bossCyclopsSupport.respawnTimer = 0.f;
    _bossOgreSupport.enemy = nullptr;
    _bossOgreSupport.respawnTimer = 0.f;

    _roomClearPending = false;
    _pendingExp = 0.f;
    _expTallyAccum = 0.f;
    _expTallyDone = false;
    _tallyChoiceChaining = false;
    _eliteRewardGranted = false;
    ResetEliteRoomRuntime();

    _currentRoomType = type;
    _currentRoom = (_currentRoom % 5) + 1;
    if (type == RoomType::Boss)
        _currentRoom = 6;
    _wave = std::max(1, _wave + 1);

    Biome nextBiome = GetBiomeForAct(_currentAct);
    _currentBiome = nextBiome;
    _pendingBiome = nextBiome;
    _biomeTransitionActive = false;
    _biomeTransitionSwapped = false;
    _biomeTransitionTimer = 0.f;
    ApplyBiome(nextBiome);
    PopulatePropsForBiome(nextBiome);
    {
        std::vector<Rectangle> propRects;
        propRects.reserve(_props.size() + 1);
        for (auto& prop : _props)
            propRects.push_back(prop.GetCollisionRec());
        // Block the bottom boundary zone so A* never routes waypoints into it.
        {
            const float navMapW = _map.width  * _mapScale;
            const float navMapH = _map.height * _mapScale;
            const float navBot  = (_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle) ? 160.f : 220.f;
            propRects.push_back({ 0.f, navMapH - navBot, navMapW, navBot });
        }
        _nav.Rebuild(_map.width * _mapScale, _map.height * _mapScale, propRects);
    }

    _waveStarting = false;
    _waveIntroTimer = 0.f;
    _fadeInTimer = 0.6f;
    _fadeInDuration = 0.6f;
    _pickupSpawnTimer = kDefaultTimedPickupInterval;
    _player.GrantInvulnerability(kWaveSpawnProtectionDuration);

    _message = std::string("Debug - ") + GetDebugRoomTypeName(type) + " Area";

    if (type == RoomType::Treasure)
    {
        _roomClearPending = true;
        const float mapW = _map.width * _mapScale;
        const float mapH = _map.height * _mapScale;
        const Vector2 centre{ mapW * 0.5f, mapH * 0.5f };
        auto spawnGold = [&](GoldDenomination denom, float ox, float oy)
        {
            auto g = std::make_unique<GoldPickup>();
            g->Init(Vector2{ centre.x + ox, centre.y + oy }, denom);
            _pickups.push_back(std::move(g));
        };
        spawnGold(GoldDenomination::Ten, 0.f, 0.f);
        spawnGold(GoldDenomination::Five, -60.f, 24.f);
        spawnGold(GoldDenomination::Five, 60.f, 24.f);
    }
    else if (type == RoomType::Store)
    {
        _roomClearPending = true;
        float mapW = _map.width  * _mapScale;
        float mapH = _map.height * _mapScale;
        _shop.Enter({ mapW * 0.5f, mapH * 0.40f }, _player, _currentAct);
    }
    else if (type == RoomType::Rest)
    {
        _roomClearPending = true;
    }
    else
    {
        SpawnEnemies();
    }

    _gameState = GameState::Play;
}

// (ShowRoomChoiceScreen / GenerateRoomChoices removed ? replaced by map system)

Biome Engine::GetBiomeForAct(int act) const
{
    int idx = act - 1;
    if (idx >= 0 && idx < (int)_biomeSequence.size())
        return _biomeSequence[idx];
    // Fallback if sequence not generated yet
    bool isDungeon = _startBiomeDungeon ? ((act % 2) == 1) : ((act % 2) == 0);
    return isDungeon ? Biome::Caverns : Biome::Forest;
}


// -- GenerateActMap ------------------------------------------------------------
// Builds a 6-row branching DAG for the current act.
// Row 0 = entry (Standard), rows 1-4 = branching, row 5 = Boss.
//
// Special room guarantee:
//   Combat/reward rooms appear early, with recovery available later:
//     Row 2 (early)  - Elite + Treasure
//     Row 3 (late)   - Standard + Rest (Zeph is no longer a dungeon room)
//   Each row has 4 nodes: 2 specials + 2 Standard fillers.
//   The 2 special positions are chosen independently per row (out of 4 slots)
//   so the rewarded lanes shift each run and force different routing decisions.
//
//   A path visits exactly one node per row ? at most 2 specials per run.
//   Rows 1 and 4 are all Standard (2?4 nodes) for additional branching variety.
void Engine::GenerateActMap()
{
    _actMap.clear();
    _currentMapNodeIdx = -1;
    _mapOpenTimer = 0.f;

    // -- 1. Assign specials to rows by type -------------------------------
    // Row 2 keeps Elite + Treasure. Row 3 offers Rest beside another
    // Standard route; Zeph is no longer generated inside dungeons.
    // Each pair is randomly swapped so which side of the row each appears on
    // is still unpredictable.
    RoomType row2Pair[2] = { RoomType::Elite,    RoomType::Treasure };
    RoomType row3Pair[2] = { RoomType::Standard,  RoomType::Rest     };
    if (GetRandomValue(0, 1)) std::swap(row2Pair[0], row2Pair[1]);
    if (GetRandomValue(0, 1)) std::swap(row3Pair[0], row3Pair[1]);

    // -- 2. Pick special positions in each 4-node special row -------------
    // Each special row has 4 nodes: 2 specials + 2 Standard fillers.
    // Pick 2 distinct positions (0-3) for the specials; the other 2 are Standard.
    // Chosen independently per row so the "empty lanes" shift each run.
    auto pickTwoDistinct = [](int& a, int& b, int max) {
        a = GetRandomValue(0, max);
        do { b = GetRandomValue(0, max); } while (b == a);
    };

    int sp2a, sp2b;   // special positions in row 2
    int sp3a, sp3b;   // special positions in row 3
    pickTwoDistinct(sp2a, sp2b, 3);
    pickTwoDistinct(sp3a, sp3b, 3);

    // Build the 4-element type arrays for rows 2 and 3
    RoomType row2Types[4], row3Types[4];
    {
        int si = 0;
        for (int i = 0; i < 4; i++)
            row2Types[i] = (i == sp2a || i == sp2b) ? row2Pair[si++] : RoomType::Standard;
        si = 0;
        for (int i = 0; i < 4; i++)
            row3Types[i] = (i == sp3a || i == sp3b) ? row3Pair[si++] : RoomType::Standard;
    }

    // -- 3. Decide per-row node counts -------------------------------------
    // Rows 2 and 3 are fixed at 4 (needed for special placement).
    // Rows 1 and 4 vary (2-4) for additional graph shape variety.
    int rowCounts[6];
    rowCounts[0] = 1;
    rowCounts[1] = GetRandomValue(2, 4);
    rowCounts[2] = 4;
    rowCounts[3] = 4;
    rowCounts[4] = GetRandomValue(2, 4);
    rowCounts[5] = 1;

    // -- 4. Create nodes ---------------------------------------------------
    int rowStart[6];
    for (int r = 0; r < 6; r++)
    {
        rowStart[r] = (int)_actMap.size();
        for (int i = 0; i < rowCounts[r]; i++)
        {
            MapNode n;
            n.row   = r;
            // normX: single-node rows centre at 0.5; multi-node rows spread 0.2?0.8
            n.normX = (rowCounts[r] == 1) ? 0.5f
                    : 0.2f + (float)i / (rowCounts[r] - 1) * 0.6f;

            if      (r == 0) n.type = RoomType::Standard;
            else if (r == 5) n.type = RoomType::Boss;
            else if (r == 2) n.type = row2Types[i];
            else if (r == 3) n.type = row3Types[i];
            else             n.type = RoomType::Standard;   // rows 1 and 4

            n.available = (r == 0);
            n.completed = false;
            _actMap.push_back(n);
        }
    }

    // -- 5. Build connections ----------------------------------------------
    for (int r = 0; r < 5; r++)
    {
        int sR  = rowStart[r],     cR  = rowCounts[r];
        int sR1 = rowStart[r + 1], cR1 = rowCounts[r + 1];

        // Primary: map each row-r node proportionally to a row-(r+1) node
        for (int i = 0; i < cR; i++)
        {
            float t = (cR > 1) ? (float)i / (cR - 1) : 0.5f;
            int j = (int)roundf(t * (float)(cR1 - 1));
            j = std::max(0, std::min(cR1 - 1, j));
            int src = sR + i, dst = sR1 + j;
            auto& nxt = _actMap[src].nextNodes;
            if (std::find(nxt.begin(), nxt.end(), dst) == nxt.end())
                nxt.push_back(dst);
        }

        // Guarantee every row-(r+1) node has at least one incoming connection
        for (int j = 0; j < cR1; j++)
        {
            int dst = sR1 + j;
            bool found = false;
            for (int i = 0; i < cR && !found; i++)
            {
                auto& nxt = _actMap[sR + i].nextNodes;
                found = std::find(nxt.begin(), nxt.end(), dst) != nxt.end();
            }
            if (!found)
            {
                float dX = _actMap[dst].normX;
                int bestSrc = sR;
                float bestD = 999.f;
                for (int i = 0; i < cR; i++)
                {
                    float d = fabsf(_actMap[sR + i].normX - dX);
                    if (d < bestD) { bestD = d; bestSrc = sR + i; }
                }
                _actMap[bestSrc].nextNodes.push_back(dst);
            }
        }

        // Optional extra branches (~30% chance) for wider path variety
        for (int i = 0; i < cR; i++)
        {
            if (cR1 > 1 && GetRandomValue(0, 9) < 3)
            {
                auto& nxt = _actMap[sR + i].nextNodes;
                if (nxt.empty()) continue;
                int baseJ = nxt[0] - sR1;
                int altJ  = (baseJ > 0) ? baseJ - 1 : baseJ + 1;
                altJ = std::max(0, std::min(cR1 - 1, altJ));
                int alt = sR1 + altJ;
                if (std::find(nxt.begin(), nxt.end(), alt) == nxt.end())
                    nxt.push_back(alt);
            }
        }
    }

    // -- 6. Compute draw positions -----------------------------------------
    // Boss (row 5) sits at the top; entry (row 0) at the bottom.
    // Graph sits between the left stats panel and the right journey panel.
    const float mapTop  = 130.f;
    const float mapBot  = (float)_windowHeight - 110.f;
    const float rowH    = (mapBot - mapTop) / 5.f;
    const float mapLeft = (float)_windowWidth  * 0.30f;
    const float mapRight= (float)_windowWidth  * 0.76f;  // right side reserved for journey panel

    for (auto& node : _actMap)
    {
        float y = mapBot - node.row * rowH;
        float x = mapLeft + node.normX * (mapRight - mapLeft);
        node.drawPos = { x, y };
    }
}

// -- EnterMapRoom --------------------------------------------------------------
// Called when the player clicks an available map node.
// Sets up the room state and transitions to Play.
void Engine::EnterMapRoom(int idx)
{
    if (idx < 0 || idx >= (int)_actMap.size()) return;

    _roomClearPending = false;

    // Lock all nodes at or behind the entered row so the player can never go backward.
    int enteredRow = _actMap[idx].row;
    for (auto& n : _actMap)
        if (n.row <= enteredRow)
            n.available = false;

    _currentMapNodeIdx = idx;

    // Row 0 = Room 1, Row 5 = Room 6 (Boss)
    _currentRoom     = _actMap[idx].row + 1;
    _wave++;
    _currentRoomType = _actMap[idx].type;

    if (_currentRoomType == RoomType::Rest || _currentRoomType == RoomType::Store)
        _roomClearTimer = 5.0f;
    else
        _roomClearTimer = 0.f;

    // Store room ? place Zeph at map centre and stock the shop
    if (_currentRoomType == RoomType::Store)
    {
        float mapW = _map.width  * _mapScale;
        float mapH = _map.height * _mapScale;
        _shop.Enter({ mapW * 0.5f, mapH * 0.40f }, _player, _currentAct);
    }

    _message = "Act " + std::to_string(_currentAct) + " - " + GetBiomeName(GetBiomeForAct(_currentAct));

    _waveStarting   = true;
    _waveIntroTimer = 0.f;

    Biome nextBiome = GetBiomeForAct(_currentAct);
    if (nextBiome != _currentBiome)
    {
        _pendingBiome           = nextBiome;
        _biomeTransitionActive  = true;
        _biomeTransitionSwapped = false;
        _biomeTransitionTimer   = kBiomeFadeOutDuration + kBiomeFadeInDuration;
    }
    else
    {
        // Same biome ? re-randomise prop layout so every room feels distinct.
        PopulatePropsForBiome(_currentBiome);
        {
            std::vector<Rectangle> propRects;
            propRects.reserve(_props.size() + 1);
            for (auto& prop : _props)
                propRects.push_back(prop.GetCollisionRec());
            // Block the bottom boundary zone so A* never routes waypoints into it.
            {
                const float navMapW = _map.width  * _mapScale;
                const float navMapH = _map.height * _mapScale;
                const float navBot  = (_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle) ? 160.f : 220.f;
                propRects.push_back({ 0.f, navMapH - navBot, navMapW, navBot });
            }
            _nav.Rebuild(_map.width * _mapScale, _map.height * _mapScale, propRects);
        }

        // Most room-to-room transitions should feel immediate. For same-biome
        // rooms we start the encounter right away instead of pausing behind
        // the intro banner.
        _waveStarting = false;
        if (_currentRoomType == RoomType::Treasure)
        {
            _roomClearPending = true;

            // Spawn a pile of gold coins at the centre of the map.
            const float mapW = _map.width  * _mapScale;
            const float mapH = _map.height * _mapScale;
            const Vector2 centre{ mapW * 0.5f, mapH * 0.5f };
            const float spread = 60.f;

            auto spawnGold = [&](GoldDenomination denom, float ox, float oy)
            {
                auto g = std::make_unique<GoldPickup>();
                g->Init(Vector2{ centre.x + ox, centre.y + oy }, denom);
                _pickups.push_back(std::move(g));
            };

            // 1 ten-gold in the middle, 2 five-golds, and 3 singles scattered around
            spawnGold(GoldDenomination::Ten,    0.f,     0.f);
            spawnGold(GoldDenomination::Five,  -spread,  spread * 0.5f);
            spawnGold(GoldDenomination::Five,   spread,  spread * 0.5f);
            spawnGold(GoldDenomination::Single, 0.f,    -spread);
            spawnGold(GoldDenomination::Single,-spread * 0.6f, -spread * 0.6f);
            spawnGold(GoldDenomination::Single, spread * 0.6f, -spread * 0.6f);
        }
        else
        {
            SpawnEnemies();
            _player.GrantInvulnerability(kWaveSpawnProtectionDuration);
        }
    }

    // Fade in from black each time a room is entered.
    _fadeInTimer    = 1.2f;
    _fadeInDuration = 1.2f;
    _gameState = GameState::Play;
}

void Engine::CompleteCurrentMapNode()
{
    if (_currentMapNodeIdx < 0 || _currentMapNodeIdx >= (int)_actMap.size())
        return;

    _actMap[_currentMapNodeIdx].completed = true;
    for (int next : _actMap[_currentMapNodeIdx].nextNodes)
        if (next >= 0 && next < (int)_actMap.size())
            _actMap[next].available = true;
}

void Engine::HandleRoomContinueAction()
{
    _roomClearPending = false;

    if (_currentRoomType == RoomType::Treasure)
    {
        CompleteCurrentMapNode();
        GenerateLevelUpOptions(LevelUpOfferContext::TreasureBasic);
        _levelUpReturnState = GameState::Map;
        _levelUpOpenTimer   = 0.25f;
        _gameState          = GameState::LevelUpChoice;
        return;
    }

    if (_currentRoomType == RoomType::Boss)
    {
        CompleteCurrentMapNode();

        if (_bossesDefeated >= 2)
        {
            _gameState = GameState::DemoEnd;
            return;
        }

        _currentAct++;
        GenerateActMap();
        _mapKeySelectedIdx = -1;
        _mapOpenTimer = 0.4f;
        _gameState = GameState::Map;
        return;
    }

    CompleteCurrentMapNode();
    _mapKeySelectedIdx = -1;
    _mapOpenTimer = 0.4f;
    _gameState = GameState::Map;
}

void Engine::SpawnEnemies()
{
    CombatSpawnContext ctx{};
    ctx.map = &_map;
    ctx.mapScale = _mapScale;
    ctx.currentRoomType = _currentRoomType;
    ctx.currentAct = _currentAct;
    ctx.currentRoom = _currentRoom;
    ctx.forcedEliteMechanic = _debug.GetForcedEliteMechanic();
    ctx.pickups = &_pickups;
    ctx.eliteMechanic = &_eliteMechanic;
    ctx.eliteMinibossPtr = &_eliteMinibossPtr;
    ctx.eliteCageCenter = &_eliteCageCenter;
    ctx.eliteCageRadius = &_eliteCageRadius;
    ctx.eliteCageDamageTimer = &_eliteCageDamageTimer;
    ctx.eliteEnrageWarningTimer = &_eliteEnrageWarningTimer;
    ctx.eliteHazardSpawnTimer = &_eliteHazardSpawnTimer;
    ctx.isSpawnPositionValid = [&](Vector2 pos) { return IsSpawnPositionValid(pos); };
    ctx.playerPos = _player.GetWorldPos();
    ctx.spawnBasicEnemy = [&](Vector2 pos) { return SpawnBasicEnemy(pos); };
    ctx.spawnCyclops = [&](Vector2 pos) { return SpawnCyclops(pos); };
    ctx.spawnOgre = [&](Vector2 pos) { return SpawnOgre(pos); };
    ctx.spawnMolarbeast = [&](Vector2 pos) { SpawnMolarbeast(pos); };
    ctx.spawnBossSupportAdds = [&]() { SpawnBossSupportAdds(); };
    ctx.spawnSkeletonArcher = [&](Vector2 pos) { return SpawnSkeletonArcher(pos); };
    ctx.spawnFlameWisp      = [&](Vector2 pos) { return SpawnFlameWisp(pos); };
    ctx.spawnShieldbearer   = [&](Vector2 pos) { return SpawnShieldbearer(pos); };
    ctx.spawnPhantom        = [&](Vector2 pos) { return SpawnPhantom(pos); };
    ctx.spawnWarchief       = [&](Vector2 pos) { return SpawnWarchief(pos); };
    _combatDirector.SpawnEnemies(ctx);
}

void Engine::Run()
{
    while (!WindowShouldClose() && !_shouldClose)
        RunFrame();
}

void Engine::RunFrame()
{
    float dt = GetFrameTime();

    Update(dt);
    UpdateMusicSystem();

    // Draw everything to the fixed 1920x1080 virtual canvas.
    BeginTextureMode(_virtualCanvas);
    ClearBackground(WHITE);
    Draw();
    EndTextureMode();

    // Scale the virtual canvas onto the real window with letterboxing.
    // Black bars appear on wider/taller windows; nothing in the game changes.
    BeginDrawing();
    ClearBackground(BLACK);
    {
        LetterboxTransform lb = GetLetterboxTransform();
        DrawTexturePro(
            _virtualCanvas.texture,
            { 0.f, 0.f, (float)kVirtualWidth, -(float)kVirtualHeight }, // negative H flips Raylib's inverted render texture Y
            { lb.offsetX, lb.offsetY, (float)kVirtualWidth * lb.scale, (float)kVirtualHeight * lb.scale },
            { 0.f, 0.f }, 0.f, WHITE);
    }
    UpdateSwordCursor(); // custom sword cursor over the letterboxed game (drawn in real window space)
    EndDrawing();
}

void Engine::UpdateSwordCursor()
{
    // Any editor/tool screen keeps the normal OS pointer so precise clicking and
    // dragging feels right. Everything else (menus + gameplay) gets the sword.
    bool inEditor =
        _gameState == GameState::TileMapper ||
        _gameState == GameState::NineSliceEditor ||
        _gameState == GameState::TouchButtonMapping ||
        _hudEditorActive || _mapEditorActive ||
        _isHitboxEditorActive || _isDlgEditorActive;

    // No cursor texture, an editor, or the mouse left the window -> normal cursor.
    if (_swordCursorTex.id == 0 || inEditor || !IsCursorOnScreen())
    {
        ShowCursor();
        return;
    }

    HideCursor(); // hide the OS arrow; we draw our own sword instead

    Vector2 mouse = GetMousePosition();

    // Cursor size tracks the window so it feels consistent on any resolution.
    float cursorSize = GetScreenHeight() * 0.05f;
    if (cursorSize < 24.f) cursorSize = 24.f;
    if (cursorSize > 72.f) cursorSize = 72.f;

    // The blade tip (the click hotspot) sits near the top-left of this 32x32 sprite (mirrored to point left).
    const float hotspotX = 2.f / 32.f;
    const float hotspotY = 2.f / 32.f;

    Rectangle src = { 0.f, 0.f, (float)_swordCursorTex.width, (float)_swordCursorTex.height };
    Rectangle dst = {
        mouse.x - hotspotX * cursorSize,
        mouse.y - hotspotY * cursorSize,
        cursorSize, cursorSize };

    DrawTexturePro(_swordCursorTex, src, dst, { 0.f, 0.f }, 0.f, WHITE);
}

InputPromptMode Engine::GetPromptModeForUi() const
{
    return _inputPromptMode;
}

void Engine::UpdateInputPromptMode()
{
    bool keyboardOrMouseUsed = false;

    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_A) || IsKeyDown(KEY_S) || IsKeyDown(KEY_D) ||
        IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT) ||
        IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
        IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_TAB) ||
        IsKeyPressed(KEY_E) || IsKeyPressed(KEY_Q) || IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_TWO) ||
        IsKeyPressed(KEY_THREE) || IsKeyPressed(KEY_FOUR) || IsKeyPressed(KEY_M))
    {
        keyboardOrMouseUsed = true;
    }

    Vector2 mouseDelta = GetMouseDelta();
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || IsMouseButtonPressed(MOUSE_RIGHT_BUTTON) ||
        IsMouseButtonPressed(MOUSE_MIDDLE_BUTTON) || fabsf(GetMouseWheelMove()) > 0.01f ||
        (mouseDelta.x * mouseDelta.x + mouseDelta.y * mouseDelta.y > 4.f))
    {
        keyboardOrMouseUsed = true;
    }

    if (keyboardOrMouseUsed)
        _inputPromptMode = InputPromptMode::KeyboardMouse;

    if (IsGamepadAvailable(GamepadInput::kGamepad))
    {
        bool gamepadUsed = fabsf(GetGamepadAxisMovement(GamepadInput::kGamepad, GAMEPAD_AXIS_LEFT_X)) > GamepadInput::kDeadZone ||
                           fabsf(GetGamepadAxisMovement(GamepadInput::kGamepad, GAMEPAD_AXIS_LEFT_Y)) > GamepadInput::kDeadZone ||
                           fabsf(GetGamepadAxisMovement(GamepadInput::kGamepad, GAMEPAD_AXIS_RIGHT_X)) > GamepadInput::kDeadZone ||
                           fabsf(GetGamepadAxisMovement(GamepadInput::kGamepad, GAMEPAD_AXIS_RIGHT_Y)) > GamepadInput::kDeadZone;
        for (int b = 1; b <= 17 && !gamepadUsed; ++b)
            gamepadUsed = IsGamepadButtonDown(GamepadInput::kGamepad, (GamepadButton)b);
        if (gamepadUsed)
            _inputPromptMode = InputPromptMode::Gamepad;
    }

    if (GetTouchPointCount() > 0)
        _inputPromptMode = InputPromptMode::Touch;
}
void Engine::Update(float dt)
{
    // A real run always launches from the menu, so clear the playtest flag there —
    // guarantees the Back-to-Editor overlay can never leak into a normal game.
    if (_gameState == GameState::Menu)
    {
        _editorPlaytestActive = false;
        _player.SetInvulnerableLock(false);
    }

    // While playtesting a room from the map editor, a "Back to Editor" button
    // (top-centre) or F1 returns to the editor with the room still open. A second
    // button flips enemies on/off live so door locking/unlocking can be tested.
    if (_editorPlaytestActive)
    {
        const Vector2 mp = GetVirtualMousePos();
        const Rectangle backBtn{ (float)kVirtualWidth * 0.5f - 130.f, 12.f, 260.f, 42.f };
        const Rectangle enemyBtn{ backBtn.x + backBtn.width + 14.f, 12.f, 230.f, 42.f };
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mp, enemyBtn))
            SetPlaytestEnemies(!_editorPlaytestEnemiesOn);
        const bool clicked = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
                             CheckCollisionPointRec(mp, backBtn);
        if (clicked || IsKeyPressed(KEY_F1))
        {
            _editorPlaytestActive = false;
            _player.SetInvulnerableLock(false);
            _gameState = GameState::TileMapper;
            return;
        }
        if (IsKeyPressed(KEY_F2))   // keyboard shortcut for the enemy toggle
            SetPlaytestEnemies(!_editorPlaytestEnemiesOn);
    }

    // Juice timers tick in real time (slow-mo only scales the gameplay sim).
    if (_critFocusTimer   > 0.f) _critFocusTimer   -= dt;
    if (_screenFlashTimer > 0.f) _screenFlashTimer -= dt;
    if (_slowMoTimer      > 0.f) _slowMoTimer      -= dt;
    // Gold flash + a satisfying micro-pause on level-up (applied silently in AddExp).
    int lvl = _player.GetLevel();
    if (lvl > _lastPlayerLevel)
    {
        TriggerScreenFlash(Color{ 255, 210, 90, 255 }, 0.35f);
        RequestHitStop(0.06f);   // brief "power gained" beat
        // TODO(SFX): level-up chime hook.
    }
    _lastPlayerLevel = lvl;

    // Player hurt red-edge vignette — fires whenever the player's HP drops.
    if (_playerHurtTimer > 0.f) _playerHurtTimer -= dt;
    if (_deathVignette   > 0.f) _deathVignette   -= dt * 0.6f;   // slow fade during the death beat

    if (_debug.IsActive() && IsKeyPressed(KEY_J)) _juicePanelOpen = !_juicePanelOpen;
    if (_juicePanelOpen) UpdateJuicePanel();

    // Debug [F6]: turn the current dungeon room into a Risk Shrine on the spot
    // (clears enemies, spawns the shrine) so the room-events flow is testable.
    if (_debug.IsActive() && _gameState == GameState::DungeonRun && IsKeyPressed(KEY_F6))
    {
        DungeonRoomState& st = _dungeonRoomStates[_dungeonRoomIdx];
        st.special = RoomSpecialType::RiskShrine;
        st.specialOptions[0] = 0; st.specialOptions[1] = 1; st.specialOptions[2] = 2;
        st.specialClaimed = false; st.specialChoice = -1;
        st.cleared = true;
        ClearDungeonEnemies();
        _dungeonEnemiesSpawned = false;
    }

    // Debug [F7]: HAZARD PREVIEW — force-spawn one of each environmental hazard
    // right next to the player so their animation/behaviour can be inspected on
    // demand instead of waiting for a lucky random roll. RestoreHazard bypasses
    // placement validation, so they appear exactly where we put them.
    if (_debug.IsActive() && _gameState == GameState::DungeonRun && IsKeyPressed(KEY_F7))
    {
        Vector2 p = _player.GetWorldPos();
        _roomHazards.ClearRoom();
        _roomHazards.RestoreHazard(RoomHazardType::LavaPool,      { p.x - 260.f, p.y },        { 1.f, 0.f }, 1.f);
        _roomHazards.RestoreHazard(RoomHazardType::FireTotem,     { p.x + 260.f, p.y - 60.f }, { 1.f, 0.f }, Balance::Hazards::kTotemHealth);
        _roomHazards.RestoreHazard(RoomHazardType::FireballTorch, { p.x,          p.y - 250.f },{ 1.f, 0.f }, Balance::Hazards::kTorchHealth);
        _vfx.SpawnFloatingLabel({ p.x, p.y - 120.f }, "HAZARD PREVIEW [F7]", Color{ 255, 180, 70, 255 }, 2.0f);
    }
    float hpNow = _player.GetHealthValue();
    if (hpNow < _lastPlayerHp - 0.01f) _playerHurtTimer = 0.4f;   // hurt SFX already handled by the damage source
    _lastPlayerHp = hpNow;

    UpdateInputPromptMode();
    _shop.SetPromptMode(GetPromptModeForUi());
    _worldMap.SetPromptMode(GetPromptModeForUi());

#if MO_DEV_TOOLS
    // Secret unlock: F12 or \ to enable debug mode access.
    if (IsKeyPressed(KEY_F12) || IsKeyPressed(KEY_BACKSLASH))
        _demoCompleted = true;
#endif

    switch (_gameState)
    {
    case GameState::Menu:
    {
        if (IsKeyPressed(KEY_F1)) _menu.ToggleBorderEditor();
#if MO_DEV_TOOLS
        _menu.SetDebugUnlocked(_demoCompleted);
#endif
        _menu.Update();

        // Ascension selector — clickable arrows to pick the run's difficulty.
        if (_meta.GetMaxAscensionUnlocked() > 0)
        {
            Vector2 mouse = GetVirtualMousePos();
            float sw = (float)kVirtualWidth;
            Rectangle leftArrow{ sw * 0.5f - 190.f, 118.f, 44.f, 44.f };
            Rectangle rightArrow{ sw * 0.5f + 146.f, 118.f, 44.f, 44.f };
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            {
                if (CheckCollisionPointRec(mouse, leftArrow))
                    _meta.SetSelectedAscension(_meta.GetSelectedAscension() - 1);
                else if (CheckCollisionPointRec(mouse, rightArrow))
                    _meta.SetSelectedAscension(_meta.GetSelectedAscension() + 1);
            }
            if (IsKeyPressed(KEY_LEFT_BRACKET))  _meta.SetSelectedAscension(_meta.GetSelectedAscension() - 1);
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) _meta.SetSelectedAscension(_meta.GetSelectedAscension() + 1);
        }

        if (_menu.StartPressed())
        {
            // New Game is the repeatable onboarding path while the tutorial is
            // being tuned. Saved onboarding progress is deliberately untouched.
            _isDailyRun = false;   // normal (randomly seeded) run
            _prologueEntryMode = PrologueEntryMode::NewGame;
            _classSelectCursor = (int)_player.GetClass();
            ReloadAppearancePortrait();
            _classSelectReturnState = GameState::Menu;
            _gameState = GameState::ClassSelect;
        }
        if (_menu.ContinuePressed())
        {
            // Continue is the testing shortcut into the established village
            // flow. Hero selection still makes class and appearance explicit.
            _isDailyRun = false;
            _prologueEntryMode = PrologueEntryMode::Continue;
            _classSelectCursor = (int)_player.GetClass();
            ReloadAppearancePortrait();
            _classSelectReturnState = GameState::Menu;
            _gameState = GameState::ClassSelect;
        }
        if (IsKeyPressed(KEY_T))   // start today's seeded Daily Run
        {
            _isDailyRun = true;
            _dailySeed  = ComputeDailySeed();
            _prologueEntryMode = PrologueEntryMode::Continue;
            _classSelectCursor = (int)_player.GetClass();
            ReloadAppearancePortrait();
            _classSelectReturnState = GameState::Menu;
            _gameState = GameState::ClassSelect;
        }
        if (IsKeyPressed(KEY_B))   // open the Bestiary from the main menu
        {
            _bestiaryReturnState = GameState::Menu;
            _gameState = GameState::Bestiary;
        }
#if MO_DEV_TOOLS
        if (_menu.DebugPressed() && _demoCompleted)
        {
            DebugStartRun();
        }
#endif
        if (_menu.QuitPressed())
            _shouldClose = true;
#if MO_DEV_TOOLS
        if (_menu.DungeonRunPressed())
        {
            _isMainGameRun = false;
            _forcedHandcraftedRoomId.clear();
            _dungeonView          = DungeonView::Graph;
            _dungeonRoomIdx = -1;

            // Pick a random available biome and load its tileset.
            _currentBiome = kTilesetBiomes[GetRandomValue(0, kTilesetBiomeCount - 1)];
            _pendingBiome = _currentBiome;
            _useHandcraftedDungeonRooms = _currentBiome == Biome::Forest;
            _handcraftedDungeonRoomIds.clear();
            if (_useHandcraftedDungeonRooms)
            {
                RefreshHandcraftedRooms();
                if (!GenerateHandcraftedDungeon(
                        _currentBiome, _handcraftedDungeonRoomIds))
                    _message = "Forest room library cannot form a complete dungeon yet";
            }
            else
                _dungeonGen.Generate();
            LoadTilesetForBiome(_currentBiome);

            _gameState = GameState::DungeonRun;
        }
        if (_menu.TileMapperPressed())
        {
            _tileMapper.Init(kTilesheetFolder);
            _gameState = GameState::TileMapper;
        }
        if (_menu.NineSliceEditorPressed())
        {
            _nineSliceEditor.Init(kUIFolder);
            _gameState = GameState::NineSliceEditor;
        }
        if (_menu.CharacterAnimatorPressed())
        {
            _charAnimator.Init();
            _gameState = GameState::CharacterAnimator;
        }
        if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_K))
        {
            // Both shortcuts now enter the same authoritative animator. Its
            // start screen contains live character tuning and the FX library.
            _charAnimator.Init();
            _gameState = GameState::CharacterAnimator;
        }
        if (IsKeyPressed(KEY_V))   // dev: open the Village Asset Adjuster
        {
            _mapEditor.Init();
            _gameState = GameState::MapEditor;
        }
        if (IsKeyPressed(KEY_Y))   // dev: playable village-builder placement playground
        {
            EnterVillagePlayground();
        }
#endif
        if (_menu.SettingsPressed())
        {
            _stateBeforeSettings = GameState::Menu;
            _settingsTab         = 0;
            _settingsDragSlider  = -1;
            _settingsRebindSlot  = -1;
            _keybindingsEdit     = _player.GetBindings();
            _gameState           = GameState::Settings;
        }
        break;
    }

    case GameState::Settings:
        UpdateSettings(dt);
        break;

    case GameState::ClassSelect:
        UpdateClassSelect();
        break;

    case GameState::MetaShop:
        UpdateMetaShop(dt);
        break;

    case GameState::CurseShrine:
        UpdateCurseShrine();
        break;

    case GameState::DecisionRoom:
        UpdateDecisionRoom();
        break;

    case GameState::RelicChoice:
        UpdateRelicChoice();
        break;

    case GameState::Bestiary:
        UpdateBestiary();
        break;

    case GameState::DungeonRun:
        UpdateDungeonRun(dt);
        break;

    case GameState::WorldMap:
        UpdateWorldMap(dt);
        break;

    case GameState::TileMapper:
        _tileMapper.Update();
        if (_tileMapper.ConsumeRoomPlaytestRequest())
        {
            const RoomBlueprint playtestRoom = _tileMapper.EditedRoom();
            // Keep the map editor loaded so "Back to Editor" can return to it with
            // the room still open (see the _editorPlaytestActive handling below).
            _editorPlaytestActive = true;
            _editorPlaytestBlueprint = playtestRoom;

            _isMainGameRun = false;
            _useHandcraftedDungeonRooms = false;
            _editorPlaytestEnemiesOn = true;   // start with enemies (doors locked)
            _player.SetInvulnerableLock(true);  // can't die while testing
            RefreshHandcraftedRooms();
            _forcedHandcraftedRoomId.clear();
            _currentBiome = playtestRoom.biome;
            _pendingBiome = _currentBiome;
            _dungeonRoomIdx = -1;

            // Which door layouts the designer has authored a room for (bit mask
            // N=1,S=2,E=4,W=8). A dungeon is "fully handmade" when every room's
            // door layout is one of these — then NO procgen room is ever used.
            auto candidatesFor = [&](const DungeonRoom& room)
            {
                return _roomLibrary.PlaytestCandidates(
                    playtestRoom.biome, room.type,
                    RoomDoorMask(room.hasNorth, room.hasSouth,
                                 room.hasEast, room.hasWest));
            };
            auto findEditedSlot = [&]() -> int {
                const auto& rooms = _dungeonGen.GetRooms();
                for (int i = 0; i < (int)rooms.size(); ++i)
                {
                    const DungeonRoom& c = rooms[(std::size_t)i];
                    const unsigned char graphMask = RoomDoorMask(
                        c.hasNorth, c.hasSouth, c.hasEast, c.hasWest);
                    if (c.type == playtestRoom.roomType &&
                        (graphMask == playtestRoom.DoorMask() ||
                         playtestRoom.DoorMask() == 15))
                        return i;
                }
                return -1;
            };
            auto fullyHandcraftable = [&]() -> bool {
                for (const DungeonRoom& c : _dungeonGen.GetRooms())
                    if (candidatesFor(c).empty()) return false;
                return true;
            };

            static unsigned int playtestSequence = 0;
            const auto seedTicks = std::chrono::high_resolution_clock::now()
                .time_since_epoch().count();
            const unsigned int playtestSeed = (unsigned int)seedTicks ^
                (++playtestSequence * 0x9e3779b9u);
            SetRandomSeed(playtestSeed);

            int playtestRoomIdx = -1;
            // Phase 1: prefer a layout every room of which maps to an authored
            // room (no procgen at all), that also has a slot for the edited room.
            for (int attempt = 0; attempt < 600 && playtestRoomIdx < 0; ++attempt)
            {
                _dungeonGen.GenerateEditorPlaytest();
                const int slot = findEditedSlot();
                if (slot >= 0 && fullyHandcraftable()) playtestRoomIdx = slot;
            }
            // A second search gives constrained libraries more chances, but it
            // still requires complete handcrafted coverage.
            if (playtestRoomIdx < 0)
            {
                for (int attempt = 0; attempt < 64 && playtestRoomIdx < 0; ++attempt)
                {
                    _dungeonGen.GenerateEditorPlaytest();
                    const int slot = findEditedSlot();
                    if (slot >= 0 && fullyHandcraftable()) playtestRoomIdx = slot;
                }
                if (playtestRoomIdx < 0)
                {
                    auto maskName = [](unsigned char mask)
                    {
                        std::string name;
                        if (mask & 1) name += "N";
                        if (mask & 2) name += "S";
                        if (mask & 8) name += "W";
                        if (mask & 4) name += "E";
                        return name.empty() ? std::string("None") : name;
                    };
                    std::set<std::string> missing;
                    for (const DungeonRoom& c : _dungeonGen.GetRooms())
                    {
                        if (!candidatesFor(c).empty()) continue;
                        const unsigned char mask = RoomDoorMask(
                            c.hasNorth, c.hasSouth, c.hasEast, c.hasWest);
                        missing.insert(std::string(GetDebugRoomTypeName(c.type)) + " " +
                                       maskName(mask));
                    }
                    std::string list;
                    for (const std::string& item : missing)
                    {
                        if (!list.empty()) list += ", ";
                        list += item;
                    }
                    if (!list.empty())
                        _message = "Missing handcrafted rooms: " + list;
                    else
                        _message = "No matching " +
                            std::string(GetDebugRoomTypeName(playtestRoom.roomType)) +
                            " slot for this room's doors";
                }
            }

            _editorPlaytestStartRoomIdx = playtestRoomIdx;
            _editorPlaytestRoomIds.clear();
            if (playtestRoomIdx >= 0)
            {
                const auto& graphRooms = _dungeonGen.GetRooms();
                _editorPlaytestRoomIds.resize(graphRooms.size());
                for (int i = 0; i < (int)graphRooms.size(); ++i)
                {
                    if (i == playtestRoomIdx) continue;
                    const DungeonRoom& graphRoom = graphRooms[(std::size_t)i];
                    const unsigned char requiredMask = RoomDoorMask(
                        graphRoom.hasNorth, graphRoom.hasSouth,
                        graphRoom.hasEast, graphRoom.hasWest);
                    std::vector<const RoomBlueprint*> candidates =
                        _roomLibrary.PlaytestCandidates(playtestRoom.biome,
                                                        graphRoom.type,
                                                        requiredMask);
                    if (candidates.empty())
                    {
                        playtestRoomIdx = -1;
                        break;
                    }
                    const int pick = GetRandomValue(0, (int)candidates.size() - 1);
                    _editorPlaytestRoomIds[(std::size_t)i] =
                        candidates[(std::size_t)pick]->id;
                }
                _editorPlaytestStartRoomIdx = playtestRoomIdx;
            }

            LoadTilesetForBiome(_currentBiome);
            if (playtestRoomIdx >= 0)
            {
                for (int roomIdx = 0; roomIdx < (int)_dungeonGen.GetRooms().size(); ++roomIdx)
                {
                    const RoomBlueprint* selected = roomIdx == playtestRoomIdx
                        ? &playtestRoom
                        : _roomLibrary.FindById(_editorPlaytestRoomIds[(std::size_t)roomIdx]);
                    if (selected == nullptr) continue;
                    for (int variantIdx = 0; variantIdx < (int)_dungeonVisualVariants.size(); ++variantIdx)
                    {
                        if (_dungeonVisualVariants[(std::size_t)variantIdx].mapperStem == selected->tilesetStem)
                        {
                            if (roomIdx < (int)_dungeonRoomVisualVariants.size())
                                _dungeonRoomVisualVariants[(std::size_t)roomIdx] = variantIdx;
                            break;
                        }
                    }
                }
                // Spawn in the middle of the room the designer is editing.
                EnterDungeonRoom(playtestRoomIdx, DungeonDoorSide::None,
                                 { (float)kVirtualWidth * 0.5f, (float)kVirtualHeight * 0.5f },
                                 true);
            }
            else
            {
                _forcedHandcraftedRoomId.clear();
                _editorPlaytestActive = false;
                _player.SetInvulnerableLock(false);
                if (_message.empty())
                    _message = "No dungeon room matches this door layout yet";
                _tileMapper.SetRoomPlaytestError(_message);
                _gameState = GameState::TileMapper;
            }
            break;
        }
        if (_tileMapper.WantsToExit())
        {
            _tileMapper.Unload();
            _menu.Init();
            _gameState = GameState::Menu;
        }
        break;

    case GameState::CharacterAnimator:
        _charAnimator.Update();
        if (_charAnimator.WantsToExit())
        {
            _charAnimator.Unload();
            _menu.Init();
            _gameState = GameState::Menu;
        }
        break;

    case GameState::AttackEditor:
        _attackEditor.Update();
        if (_attackEditor.WantsToExit())
        {
            _attackEditor.Unload();
            _menu.Init();
            _gameState = GameState::Menu;
        }
        break;

    case GameState::MapEditor:
        _mapEditor.Update();
        if (_mapEditor.WantsToExit())
        {
            _mapEditor.Unload();
            _menu.Init();
            _gameState = GameState::Menu;
        }
        break;

    case GameState::VillagePlayground:
    case GameState::Village:
    {
        _gamepad.Update(_gamepadBindingsEdit);
        if (IsKeyPressed(KEY_ESCAPE) || (_gamepad.isActive && _gamepad.pausePressed))
        {
            _stateBeforePause = _gameState;
            _gameState = GameState::Pause;
            break;
        }
        UpdateVillagePlayground(dt);
        break;
    }

    case GameState::NineSliceEditor:
        _nineSliceEditor.Update();
        if (_nineSliceEditor.WantsToExit())
        {
            _nineSliceEditor.Unload();
            _menu.Init();
            _gameState = GameState::Menu;
        }
        break;

    case GameState::HowToPlay:
    {
        if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE))
        {
            if (_howToPlayFrom == GameState::Menu)
                _menu.Init();
            _runState.ReturnFromHowToPlay();
        }
        if (IsGamepadAvailable(0))
        {
            // B / Circle = back
            if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            {
                if (_howToPlayFrom == GameState::Menu) _menu.Init();
                _runState.ReturnFromHowToPlay();
            }

            _htpGpCooldown -= dt;
            float axisX = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
            bool navLeft  = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT)  || (axisX < -0.5f && _htpGpCooldown <= 0.f);
            bool navRight = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || (axisX >  0.5f && _htpGpCooldown <= 0.f);

            if (navLeft && _htpTab > 0)
            {
                --_htpTab;
                _htpGpCooldown = 0.22f;
            }
            if (navRight && _htpTab < 3)
            {
                ++_htpTab;
                _htpGpCooldown = 0.22f;
            }
        }
        break;
    }

    case GameState::Keybindings:
    case GameState::TouchButtonMapping:
        // Navigation handled in the Draw case (Back/Save/Default buttons)
        break;

    case GameState::Pause:
    {
        if (IsKeyPressed(KEY_F1))     _pauseUI.ToggleBorderEditor();
        if (IsKeyPressed(KEY_ESCAPE)) _gameState = _stateBeforePause;

        break;
    }

    case GameState::GameOver:
        break;

    case GameState::DeathRevive:
        UpdateDeathRevive();
        break;

    case GameState::BossChoice:
        UpdateBossChoice();
        break;

    case GameState::DemoEnd:
    {
        // Reaching the end screen counts as clearing the run at the current
        // ascension tier — unlocks the next tier. Guarded so it fires once.
        if (!_ascensionRecorded)
        {
            _ascensionRecorded = true;
            _meta.RecordAscensionCleared(_ascensionTier);
            _meta.RecordGameCompleted();
        }

        bool anyKey   = (GetKeyPressed() != 0);
        bool anyMouse = IsMouseButtonPressed(MOUSE_LEFT_BUTTON)
                     || IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)
                     || IsMouseButtonPressed(MOUSE_MIDDLE_BUTTON);
        bool touchDown = (_touchModeActive && GetTouchPointCount() > 0);
        bool touchTap  = touchDown && !_demoEndTouchHeld;
        _demoEndTouchHeld = touchDown;

        bool anyGamepad = false;
        if (IsGamepadAvailable(GamepadInput::kGamepad))
        {
            for (int b = 1; b <= 17; b++)
                if (IsGamepadButtonPressed(GamepadInput::kGamepad, (GamepadButton)b))
                    { anyGamepad = true; break; }
        }

        if (anyKey || anyMouse || touchTap || anyGamepad)
        {
            ResetRunState();
            _menu.Init();
            _gameState = GameState::Menu;
        }
        break;
    }

    case GameState::LevelUpChoice:
    {
        if (_levelUpOpenTimer > 0.f)
        {
            _levelUpOpenTimer -= dt;
            _levelUpGpCursor = 0;
            _levelUpGpRow    = 1;
        }
        _gamepad.Update(_gamepadBindingsEdit);
        if (_gamepad.isActive && _levelUpOpenTimer <= 0.f)
        {
            if (_levelUpGpCooldown > 0.f) _levelUpGpCooldown -= dt;
            bool showUlt = _showUltimateRow && !_ultimateRowPicked;
            bool showReg = !_regularRowPicked;
            float axisX = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
            float axisY = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
            bool navLeft  = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT)  || (axisX < -0.5f && _levelUpGpCooldown <= 0.f);
            bool navRight = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || (axisX >  0.5f && _levelUpGpCooldown <= 0.f);
            bool navUp    = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_UP)    || (axisY < -0.5f && _levelUpGpCooldown <= 0.f);
            bool navDown  = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN)  || (axisY >  0.5f && _levelUpGpCooldown <= 0.f);
            if (navLeft  && _levelUpGpCursor > 0) { --_levelUpGpCursor; _levelUpGpCooldown = 0.18f; }
            if (navRight && _levelUpGpCursor < 2) { ++_levelUpGpCursor; _levelUpGpCooldown = 0.18f; }
            if (navUp   && showUlt && _levelUpGpRow == 1) { _levelUpGpRow = 0; _levelUpGpCooldown = 0.18f; }
            if (navDown && showReg && _levelUpGpRow == 0) { _levelUpGpRow = 1; _levelUpGpCooldown = 0.18f; }
        }
        break;
    }

    case GameState::AbilityChoice:
    {
        if (_abilityChoiceOpenTimer > 0.f)
        {
            _abilityChoiceOpenTimer -= dt;
            _abilityChoiceGpCursor = 0;
        }
        _gamepad.Update(_gamepadBindingsEdit);
        if (_gamepad.isActive && _abilityChoiceOpenTimer <= 0.f)
        {
            if (_abilityChoiceGpCooldown > 0.f) _abilityChoiceGpCooldown -= dt;
            int count = _abilityChoiceSwapPending ? _player.GetLearnedCount() : _abilityChoiceOptionCount;
            float axisX = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
            bool navLeft  = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT)  || (axisX < -0.5f && _abilityChoiceGpCooldown <= 0.f);
            bool navRight = IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || (axisX >  0.5f && _abilityChoiceGpCooldown <= 0.f);
            if (navLeft  && _abilityChoiceGpCursor > 0)         { --_abilityChoiceGpCursor; _abilityChoiceGpCooldown = 0.18f; }
            if (navRight && _abilityChoiceGpCursor < count - 1) { ++_abilityChoiceGpCursor; _abilityChoiceGpCooldown = 0.18f; }
        }
        break;
    }

    case GameState::Shop:
    {
        _gamepad.Update(_gamepadBindingsEdit);
        bool gpLeave = _shop.UpdateGamepadNav(GetFrameTime(), _player);
        if (gpLeave || _shop.Update(_player, _debug.IsActive()))
        {
            ShopContractType accepted = _shop.ConsumeAcceptedContract();
            if (accepted != ShopContractType::Count)
            {
                RunModifier contract;
                contract.shopContract = accepted;
                contract.active = false;
                contract.roomsRemaining = 1;
                contract.tint = Color{ 215, 155, 70, 255 };
                switch (accepted)
                {
                case ShopContractType::Untouched:       contract.label = "UNTOUCHED"; break;
                case ShopContractType::ArcaneRestraint: contract.label = "ARCANE RESTRAINT"; break;
                case ShopContractType::AgainstTheClock: contract.label = "AGAINST THE CLOCK"; break;
                default: break;
                }
                _runModifiers.push_back(contract);
                _contractToastText = contract.label + " ARMED FOR NEXT FIGHT";
                _contractToastTimer = 3.f;
            }

            if (_levelUpReturnState == GameState::DungeonRun)
                _gameState = GameState::DungeonRun;
            else if (_levelUpReturnState == GameState::VillagePlayground ||
                     _levelUpReturnState == GameState::Village)   // Zeph in the village
                _gameState = _levelUpReturnState;
            else
                _gameState = GameState::Play;
            if (_currentRoomType == RoomType::Store && _gameState == GameState::Play)
                _roomClearPending = true;
        }
        break;
    }

    default: break;
    }
}

void Engine::UpdateGamePlay(float dt)
{
    // Hit-stop: freeze the simulation for a few frames so impacts land hard.
    if (_hitStopTimer > 0.f) { _hitStopTimer -= GetFrameTime(); return; }
    // Slow-mo (crit / boss death): scale the gameplay sim (timer ticks in Update).
    if (_slowMoTimer > 0.f) dt *= _slowMoScale;

    if (_debug.IsActive())
    {
        if (HandleDebugToggleTabInput())
            return;

        if (_debug.IsGodMode())
            _player.GrantInvulnerability(0.2f);

        DebugCommand cmd = _debug.Update();
        if (cmd.issued)
        {
            Vector2 spawnBase = _player.GetWorldPos();
            switch (cmd.action)
            {
            case DebugActionKind::GrantInvuln:
                _player.GrantInvulnerability(5.f); break;
            case DebugActionKind::ClearEnemiesContinue:
                for (auto& e : _enemies) { e->SetActive(false); e->Teleport(Vector2{ -5000.f, -5000.f }); }
                _roomClearPending = true; break;
            case DebugActionKind::RestartRoom:
                DebugRestartRoomAs((RoomType)cmd.value); break;
            case DebugActionKind::SetEliteMechanic:
                _debug.SetForcedEliteMechanic(cmd.value);
                DebugRestartRoomAs(RoomType::Elite); break;
            case DebugActionKind::SetEliteType:
                _debug.SetForcedEliteType(cmd.value);
                DebugRestartRoomAs(RoomType::Elite); break;
            case DebugActionKind::ForceEliteSignature:
                if (Enemy* encounter = FindEncounterDebugTarget())
                    encounter->DebugForceEliteSignature();
                break;
            case DebugActionKind::ForceElitePhaseTwo:
                if (Enemy* encounter = FindEncounterDebugTarget())
                    encounter->DebugForceElitePhaseTwo();
                break;
            case DebugActionKind::SpawnGrunt:
                SpawnBasicEnemy(Vector2Add(spawnBase, Vector2{ 220.f, 40.f })); break;
            case DebugActionKind::SpawnCyclops:
                SpawnCyclops(Vector2Add(spawnBase, Vector2{ 260.f, -60.f })); break;
            case DebugActionKind::SpawnOgre:
                SpawnOgre(Vector2Add(spawnBase, Vector2{ -240.f, 50.f })); break;
            case DebugActionKind::SpawnBoss:
                SpawnMolarbeast(Vector2Add(spawnBase, Vector2{ 0.f, -260.f })); break;
            case DebugActionKind::SpawnNewEnemy:
                DebugSpawnNewEnemy(cmd.value, Vector2Add(spawnBase, Vector2{ 260.f, -40.f })); break;
            case DebugActionKind::SpawnNewBoss:
                DebugSpawnNewBoss(cmd.value, Vector2Add(spawnBase, Vector2{ 0.f, -300.f })); break;
            case DebugActionKind::GrantRandomRelic:
                GrantRelic(RollRandomRelic()); break;
            case DebugActionKind::GrantAllRelics:
                for (int i = 0; i < (int)RelicType::Count; i++) _player.AddRelic((RelicType)i); break;
            case DebugActionKind::UnlockAscension:
                _meta.RecordAscensionCleared(_meta.GetMaxAscensionUnlocked());
                _meta.SetSelectedAscension(_meta.GetMaxAscensionUnlocked()); break;
            case DebugActionKind::Heal:
                _player.Heal(cmd.value); break;
            case DebugActionKind::RestoreMana:
                _player.RestoreMana(cmd.value); break;
            case DebugActionKind::AddGold:
                _player.AddGold(cmd.value); break;
            case DebugActionKind::SpawnLoot:
                DebugSpawnLoot(); break;
            case DebugActionKind::ForceLevelUp:
                _player.AddExp(60); break;
            case DebugActionKind::AddExp:
                _player.AddExp(cmd.value); break;
            case DebugActionKind::TreasureCards:
                GenerateLevelUpOptions(LevelUpOfferContext::TreasureBasic);
                _levelUpReturnState = GameState::Play; _levelUpOpenTimer = 0.1f;
                _gameState = GameState::LevelUpChoice; break;
            case DebugActionKind::EliteReward:
                GenerateLevelUpOptions(LevelUpOfferContext::EliteReward);
                _levelUpReturnState = GameState::Play; _levelUpOpenTimer = 0.1f;
                _gameState = GameState::LevelUpChoice; break;
            case DebugActionKind::AbilityReward:
                GenerateAbilityChoiceOptions(); _abilityChoiceOpenTimer = 0.1f;
                _pendingRoomChoice = false; _gameState = GameState::AbilityChoice; break;
            case DebugActionKind::ApplyUpgrade:
                _player.ApplyUpgrade((UpgradeType)cmd.value); break;
            default: break;
            }
        }
    }

    // Hitbox editor toggle - 0 key while debug panel is active
    if (_debug.IsActive() && IsKeyPressed(KEY_ZERO))
    {
        _isHitboxEditorActive = !_isHitboxEditorActive;
        _hitboxSelectedEntity = nullptr;
        _hitboxEditAttack     = false;
        _hitboxNudgeAccum     = -kHitboxNudgeInitDelay;
        if (_isHitboxEditorActive)
            _debug.SetOpen(false);  // hide panel while editing
    }
    // Safety: auto-exit editor if debug is deactivated from outside
    if (_isHitboxEditorActive && !_debug.IsActive())
    {
        _isHitboxEditorActive = false;
        _hitboxSelectedEntity = nullptr;
    }
    if (_isHitboxEditorActive)
    {
        UpdateHitboxEditor();
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        _stateBeforePause = GameState::Play;
        _gameState = GameState::Pause;
        return;
    }

    // M key confirms the current room's continue action.
    if (_roomClearPending && IsKeyPressed(KEY_M))
    {
        HandleRoomContinueAction();
        return;
    }

    if (_fadeInTimer > 0.f)
        _fadeInTimer -= dt;
    if (_bossWarningTimer > 0.f)
        _bossWarningTimer -= dt;
    if (_biomeTransitionActive)
        UpdateBiomeTransition(dt);

    // Block attacks during room intro and ultimate cinematic.
    // Also lock combat in non-combat rooms (rest/store) so the player
    // can't accidentally trigger the attack animation while browsing.
    bool inNonCombatRoom = (_currentRoomType == RoomType::Rest  ||
                            _currentRoomType == RoomType::Store ||
                            _debug.IsOpen());
    const bool ultActive = (_ultimatePhase != UltimatePhase::None);
    _player.SetCombatLocked(_waveStarting || ultActive || inNonCombatRoom);
    _player.SetManaRegenPaused(ultActive);

    // Touch controls - must be set on player before Update() consumes them
    _player.SetTouchModeEnabled(_touchModeActive);
    if (_touchModeActive)
        UpdateTouchControls();

    // Gamepad input - works alongside keyboard and touch on all platforms
    _gamepad.Update(_gamepadBindingsEdit);
    if (_gamepad.isActive)
    {
        // Always push the direction (including zero) so releasing the stick
        // clears _touchMoveDir and the player stops instead of drifting.
        _player.SetTouchDirection(_gamepad.moveDir);
        if (_gamepad.attackPressed)  _player.SetTouchAttack();
        if (_gamepad.dashPressed)    _player.SetTouchDash();
        if (_gamepad.pausePressed)
        {
            _stateBeforePause = GameState::Play;
            _gameState = GameState::Pause;
            return;
        }
    }

    UpdateAbilityAiming();

    _player.Update(dt);

    // During the cinematic ultimate sequence everything except the player
    // animation is frozen. Drain any pending cast request so it doesn't fire
    // twice when play resumes.
    if (_ultimatePhase != UltimatePhase::None)
    {
        _player.ConsumeCastRequest();
        UpdateUltimateSequence(dt);
        return;
    }

    HandlePlayerCastRequest();

    if (_player.GetHealthValue() <= 0.f && !_playerDying)
    {
        if (_secondWindAvailable)
        {
            // Second Wind meta unlock: revive once at 40% HP with brief i-frames.
            _secondWindAvailable = false;
            _player.Heal((int)ceilf(_player.GetMaxHealthValue() * 0.4f));
            _player.GrantInvulnerability(2.0f);
            TriggerScreenShake(12.f, 0.5f);
            _secondWindToastTimer = 2.5f;
            StopSound(_explosionSound);
            PlaySound(_explosionSound);
        }
        else
        {
            _playerDying = true;
            _gameOverTimer = _gameOverDelay;
            TriggerSlowMo(1.2f, 0.35f);   // death slow-mo
            _deathVignette = 1.2f;         // dark-red overlay (decays in Update)
            // TODO(SFX): player-death sting.
        }
    }

    if (_playerDying)
    {
        _gameOverTimer -= dt;

        if (_gameOverTimer <= 0.f)
        {
            HandlePlayerDeathMetaPenalty();
            _gameState = GameState::GameOver;
        }

        return;
    }

    if (_waveStarting)
    {
        _waveIntroTimer -= dt;

        if (!_biomeTransitionActive && _waveIntroTimer <= 0.f)
        {
            _waveStarting = false;

            // Treasure rooms wait for the same explicit Continue input as every
            // other room. Pressing Continue opens the free upgrade screen.
            if (_currentRoomType == RoomType::Treasure)
            {
                _roomClearPending = true;
                return;
            }

            SpawnEnemies();
            _player.GrantInvulnerability(kWaveSpawnProtectionDuration);
        }
    }
    else
    {
        _gameTimer += dt;
        _nav.ApplyPendingRefresh();

        // Timed heal drops - boss fights and non-combat rooms suppress this.
        if (_currentRoomType != RoomType::Rest  &&
            _currentRoomType != RoomType::Store)
        {
            _pickupSpawnTimer -= dt;
            if (_pickupSpawnTimer <= 0.f)
            {
                SpawnTimedPickup();
                _pickupSpawnTimer = kDefaultTimedPickupInterval;
            }
        }

        EliteMechanicsContext eliteCtx{};
        eliteCtx.currentRoomType = _currentRoomType;
        eliteCtx.map = &_map;
        eliteCtx.mapScale = _mapScale;
        eliteCtx.player = &_player;
        eliteCtx.enemies = &_enemies;
        eliteCtx.lavaBalls = &_lavaBalls;
        eliteCtx.eliteMechanic = &_eliteMechanic;
        eliteCtx.eliteMinibossPtr = &_eliteMinibossPtr;
        eliteCtx.eliteCageCenter = &_eliteCageCenter;
        eliteCtx.eliteCageRadius = &_eliteCageRadius;
        eliteCtx.eliteCageDamageTimer = &_eliteCageDamageTimer;
        eliteCtx.eliteEnrageWarningTimer = &_eliteEnrageWarningTimer;
        eliteCtx.eliteHazardSpawnTimer = &_eliteHazardSpawnTimer;
        eliteCtx.isSpawnPositionValid = [&](Vector2 pos) { return IsSpawnPositionValid(pos); };
        eliteCtx.triggerScreenShake = [&](float strength, float duration) { TriggerScreenShake(strength, duration); };
        _combatDirector.UpdateEliteMechanics(eliteCtx, dt);

        // Rest areas complete immediately once their intro ends. Store areas
        // wait until the player actually leaves Zeph's shop before showing the
        // continue prompt.
        if (_currentRoomType == RoomType::Rest)
        {
            _roomClearPending = true;
        }

        // -- Store room - Zeph NPC logic -----------------------------------
        if (_currentRoomType == RoomType::Store)
        {
            Vector2 shopWorldOffset = { -_cameraPos.x + _shakeOffset.x, -_cameraPos.y + _shakeOffset.y };
            bool gamepadInteract = _gamepad.isActive && (_gamepad.attackPressed || _gamepad.dashPressed);
            if (_shop.UpdateNpc(_player, shopWorldOffset, _touchModeActive, gamepadInteract)) { _levelUpReturnState = GameState::Play; _gameState = GameState::Shop; }
        }
        else if (!_roomClearPending && GetActiveEnemyCount() == 0 && !_debug.IsActive())
        {
            // Elite clear bonus: scatter some gold near the player's position.
            if (_currentRoomType == RoomType::Elite && !_eliteRewardGranted)
            {
                _eliteRewardGranted = true;
                const Vector2 dropAnchor = _player.GetWorldPos();
                auto dropGold = [&](GoldDenomination denom, float ox, float oy)
                {
                    auto g = std::make_unique<GoldPickup>();
                    g->Init(Vector2{ dropAnchor.x + ox, dropAnchor.y + oy }, denom);
                    _pickups.push_back(std::move(g));
                };
                dropGold(GoldDenomination::Ten,    0.f,   -50.f);
                dropGold(GoldDenomination::Five,  -55.f,   20.f);
                dropGold(GoldDenomination::Five,   55.f,   20.f);
            }

            if (_currentRoomType == RoomType::Boss)
                StartVictoryMusic(MusicCue::BossVictory);
            else if (_currentRoomType == RoomType::Standard || _currentRoomType == RoomType::Elite)
                StartVictoryMusic(MusicCue::BattleVictory);

            // Transition to post-battle EXP tally.
            // Boss AbilityChoice is triggered from UpdateExpTally once the tally finishes.
            _expTallyDone           = false;
            _expTallyAccum          = 0.f;
            _tallyStartLevel        = _player.GetLevel();
            _tallyLevelUpsRemaining = 0;
            _tallyChoiceChaining    = false;
            _gameState              = GameState::ExpTally;
        }

        Vector2 playerFeet = _player.GetFeetWorldPos();
        _nav.TickRefresh(dt, playerFeet);

        EnemyRuntimeContext enemyRuntimeCtx{};
        enemyRuntimeCtx.player = &_player;
        enemyRuntimeCtx.nav = &_nav;
        enemyRuntimeCtx.props = &_props;
        enemyRuntimeCtx.enemies = &_enemies;
        enemyRuntimeCtx.cyclopsLasers = &_cyclopsLasers;
        enemyRuntimeCtx.lavaBalls = &_lavaBalls;
        enemyRuntimeCtx.enemyProjectiles = &_enemyProjectiles;
        enemyRuntimeCtx.triggerScreenShake = [&](float strength, float duration) { TriggerScreenShake(strength, duration); };
        enemyRuntimeCtx.spawnSmallSlime = [&](Vector2 pos) { SpawnSlime(pos, SlimeSize::Small); };
        enemyRuntimeCtx.spawnBasicEnemy = [&](Vector2 pos) { return SpawnBasicEnemy(pos); };
        enemyRuntimeCtx.spawnBossPoisonPool = [&](Vector2 pos) { SpawnPoisonCloud(pos, 130.f); };
        enemyRuntimeCtx.spawnBossFx = [&](Vector2 pos, int fxId) { SpawnBossFx(pos, fxId); };
        enemyRuntimeCtx.spawnBossCallout = [&](Vector2 pos, const char* text) { ShowBossCallout(pos, text); };
        enemyRuntimeCtx.spawnEliteFx = [&](Vector2 pos, int fxId, float scale, Color tint)
            { SpawnEliteFx(pos, fxId, scale, tint); };
        enemyRuntimeCtx.spawnEliteHazardFx = [&](Vector2 pos, int fxId, float scale, float duration, Color tint)
            { SpawnEliteHazardFx(pos, fxId, scale, duration, tint); };
        enemyRuntimeCtx.triggerScreenFlash = [&](Color color, float duration)
            { TriggerScreenFlash(color, duration); };
        enemyRuntimeCtx.requestHitStop = [&](float seconds) { RequestHitStop(seconds); };
        RebuildEnemyHazardZones();
        enemyRuntimeCtx.hazards = &_enemyHazardZones;
        _combatDirector.UpdateEnemyRuntime(enemyRuntimeCtx, dt);

        HandlePlayerMeleeDamage();
        UpdateWarriorEffects(dt);
        UpdateMageSpells(dt);
        if (_secondWindToastTimer > 0.f) _secondWindToastTimer -= dt;

        // Bestiary: count each enemy death once as it dies.
        for (auto& e : _enemies)
            if (e->IsActive() && !e->IsAlive() && !e->BestiaryRecorded())
            {
                _meta.RecordBestiaryKill(e->GetBestiaryName());
                e->SetBestiaryRecorded();
            }

        // Relic reward: once the room is clear of enemies, present the 3-card pick.
        if (_pendingRelicChoices > 0 && _gameState == GameState::DungeonRun &&
            _currentRoomType != RoomType::Store && GetActiveEnemyCount() == 0 &&
            _dungeonFadeState == DungeonFadeState::None && !_awaitingStartingAbility)
        {
            OpenRelicChoice();
        }
        UpdateSpreadProjectiles(dt);
        UpdateLavaBallProjectiles(dt);
        UpdateEnemyProjectiles(dt);
        ApplyPendingReflect();
        UpdateWarlockMinions(dt);
        UpdatePoisonClouds(dt);
        _vfx.Update(dt);
        _damageNumbers.Update(dt);
        UpdateDungeonClearEffects(dt);
        UpdateEnemyCount(dt);
        UpdateBossSupportRespawns(dt);
        UpdateCyclopsLasers(dt);
    }

    Vector2 lootCentre = _player.GetWorldPos();
    for (auto& pickup : _pickups)
    {
        if (!pickup->IsActive())
            continue;

        Vector2 pp = pickup->GetWorldPos();
        float dx = lootCentre.x - pp.x, dy = lootCentre.y - pp.y;
        if (dx * dx + dy * dy < 210.f * 210.f)
            pickup->Magnetize(lootCentre, std::min(1.f, GetFrameTime() * 9.f));

        if (CheckCollisionRecs(_player.GetCollisionRec(), pickup->GetCollisionRec()))
        {
            switch (pickup->GetType())
            {
            case PickupType::Gold: SfxBank::Get().Play(SfxId::PickupGold,  0.5f); break;
            case PickupType::Cell: SfxBank::Get().Play(SfxId::PickupCell,  0.6f); break;
            default:               SfxBank::Get().Play(SfxId::PickupMagic, 0.5f); break;   // Heal / Mana
            }
            _vfx.SpawnImpactBurst(pickup->GetWorldPos(), Color{ 255, 220, 120, 255 }, 5, 170.f);
            pickup->OnCollect(_player);
        }
    }

    _pickups.erase(
        std::remove_if(_pickups.begin(), _pickups.end(),
            [](const std::unique_ptr<Pickup>& pickup) { return !pickup->IsActive(); }),
        _pickups.end());

    int healEffectsToSpawn = _player.ConsumeHealEffectRequests();
    for (int i = 0; i < healEffectsToSpawn; ++i)
        _vfx.SpawnHealEffect();

    HandleCollisions();

    {
        // WorldConfig::ClampCamera handles both the scroll-clamp (world > screen)
        // and the centring case (world = screen - e.g. FitToScreen on a large monitor).
        // GetMapScreenPos converts the clamped world-pos to a screen-space top-left
        // for the map texture draw call. Using the live screen size here means
        // a window resize mid-session (or a phone rotation) adapts automatically.
        const int sw = kVirtualWidth;
        const int sh = kVirtualHeight;

        _cameraPos = _worldConfig.ClampCamera(_player.GetWorldPos(), sw, sh);
        _mapPos    = _worldConfig.GetMapScreenPos(_cameraPos, sw, sh);
    }

    if (_shakeTimer > 0.f)
    {
        _shakeTimer -= dt;

        float x = GetRandomValue(-100, 100) / 50.f * _shakeStrength;
        float y = GetRandomValue(-100, 100) / 50.f * _shakeStrength;
        _shakeOffset = Vector2{ x, y };
    }
    else
    {
        _shakeOffset = Vector2Zero();
    }
}


namespace
{
    constexpr float kVillagePlaygroundTilePx = 48.f;
    constexpr int kVillagePlaygroundCols = 40;   // one standard-room width at 48 px
    constexpr int kVillagePlaygroundRows = 23;   // one standard-room height at 48 px

    Rectangle VillagePlaygroundFieldRect()
    {
        return Rectangle{ 0.f, 0.f,
                          kVillagePlaygroundCols * kVillagePlaygroundTilePx,
                          kVillagePlaygroundRows * kVillagePlaygroundTilePx };
    }

    // The area inside the wall border ring — players, buildings, and citizens
    // all stay within this.
    Rectangle VillagePlaygroundWalkableRect()
    {
        return Rectangle{ kVillagePlaygroundTilePx, kVillagePlaygroundTilePx,
                          (kVillagePlaygroundCols - 2) * kVillagePlaygroundTilePx,
                          (kVillagePlaygroundRows - 2) * kVillagePlaygroundTilePx };
    }

    // Deterministic per-cell hash so floor variant tiles stay put frame to frame.
    bool VillageFloorCellUsesVariant(int cellCol, int cellRow)
    {
        unsigned int hash = (unsigned int)(cellCol * 73856093) ^ (unsigned int)(cellRow * 19349663);
        return (hash % 13u) == 0u;
    }

    // Poe's Graveyard is authored as VillageAssets/VillageGraveyard.png with
    // VillageGraveyard.vasset markers for Poe and the player respawn point.
    constexpr float kVillageGraveyardAssetScale = 2.f;
    constexpr float kVillageZephShopAssetScale = 2.f;
    constexpr float kVillageGraveyardAssetW = 196.f;
    constexpr float kVillageGraveyardAssetH = 132.f;
    constexpr float kVillageGraveyardX = 310.f;
    constexpr float kVillageGraveyardY = 190.f;

    Rectangle VillageGraveyardWorldRect()
    {
        float w = kVillageGraveyardAssetW * kVillageGraveyardAssetScale;
        float h = kVillageGraveyardAssetH * kVillageGraveyardAssetScale;
        return Rectangle{ kVillageGraveyardX, kVillageGraveyardY, w, h };
    }

    Vector2 VillageGraveyardLocalToWorld(Vector2 local)
    {
        Rectangle graveyard = VillageGraveyardWorldRect();
        return Vector2{ graveyard.x + local.x * kVillageGraveyardAssetScale,
                        graveyard.y + local.y * kVillageGraveyardAssetScale };
    }

    Rectangle VillageGraveyardLocalRectToWorld(Rectangle local)
    {
        Rectangle graveyard = VillageGraveyardWorldRect();
        return Rectangle{ graveyard.x + local.x * kVillageGraveyardAssetScale,
                          graveyard.y + local.y * kVillageGraveyardAssetScale,
                          local.width * kVillageGraveyardAssetScale,
                          local.height * kVillageGraveyardAssetScale };
    }

    Vector2 VillageZephShopLocalToWorld(int cellCol, int cellRow, Vector2 local)
    {
        return Vector2{ cellCol * kVillagePlaygroundTilePx + local.x * kVillageZephShopAssetScale,
                        cellRow * kVillagePlaygroundTilePx + local.y * kVillageZephShopAssetScale };
    }

    Rectangle VillageZephShopLocalRectToWorld(int cellCol, int cellRow, Rectangle local)
    {
        return Rectangle{ cellCol * kVillagePlaygroundTilePx + local.x * kVillageZephShopAssetScale,
                          cellRow * kVillagePlaygroundTilePx + local.y * kVillageZephShopAssetScale,
                          local.width * kVillageZephShopAssetScale,
                          local.height * kVillageZephShopAssetScale };
    }

    // The fixed village exits north. Crossing it starts or resumes the run.
    constexpr int kVillageGateCols = 4;

    Rectangle VillageGateWorldRect()
    {
        int col0 = (kVillagePlaygroundCols - kVillageGateCols) / 2;
        return Rectangle{ col0 * kVillagePlaygroundTilePx,
                          0.f,
                          kVillageGateCols * kVillagePlaygroundTilePx,
                          kVillagePlaygroundTilePx };
    }

    // Zone just inside the wall where the gate prompt shows.
    Rectangle VillageGateApproachRect()
    {
        Rectangle gate = VillageGateWorldRect();
        gate.y += kVillagePlaygroundTilePx;
        gate.height += kVillagePlaygroundTilePx * 1.5f;
        return gate;
    }

    // Citizen NPCs - ambient wanderers that appear as the village grows.
    constexpr int kVillageCitizenCap = 10;            // hard cap on wandering citizens
    constexpr int kVillageBuildingsPerCitizen = 1;    // one new citizen per building placed
    constexpr float kVillageCitizenWalkSpeed = 95.f;
    constexpr float kVillageCitizenChatRange = 128.f; // ~2 cells: close enough to chat

    Rectangle VillageCitizenRect(Vector2 position)
    {
        return Rectangle{ position.x - 12.f, position.y - 16.f, 24.f, 36.f };
    }

    // Placeholder clothing colours until real villager art exists.
    constexpr Color kVillageCitizenTints[] = {
        Color{ 178,  98,  70, 255 }, Color{  92, 128, 168, 255 }, Color{ 122, 152,  86, 255 },
        Color{ 158, 110, 160, 255 }, Color{ 190, 160,  84, 255 }, Color{  96, 156, 148, 255 },
        Color{ 172,  84, 110, 255 }, Color{ 110, 110, 170, 255 }, Color{ 146, 120,  92, 255 },
        Color{  88, 140, 104, 255 },
    };

}

void Engine::EnterVillagePlayground() { EnterVillageShared(true); }
void Engine::EnterVillage() { EnterVillageShared(false); }

void Engine::EnterVillageShared(bool sandboxMode)
{
    LoadVillagePlaygroundSheets();
    LoadVillagePlaygroundCatalog();
    _villageShopNpcActive = false;
    _villageInsideInterior = false;
    _villageBuildMode = false;
    _villageSandboxMode = sandboxMode;
    LoadVillageLayout();
    _villageCatalogScroll = 0.f;
    _villageActiveObjectIndex = _villageObjectCatalog.empty() ? -1 : 0;
    _villagePlaygroundMessage = _villageObjectCatalog.empty() ? "No villageobject_*.txt assets saved yet" : (sandboxMode ? "Village tester: walk around, press B to build" : "Welcome back to the village");
    if (!sandboxMode)
        _villagePlaygroundMessage = _firstVillageVisit ? "Find Zeph at the shop." : "Welcome back to the village";
    _villagePlaygroundMessageTimer = 4.f;
    _player.Revive();
    _player.RestoreMana(_player.GetMaxMana());
    _player.SetCombatLocked(true);
    _player.SetDashAllowedWhileCombatLocked(true);
    _player.SetManaRegenPaused(false);
    _player.SetWorldPos(sandboxMode ? Vector2{ VillagePlaygroundFieldRect().width * 0.5f, VillagePlaygroundFieldRect().height * 0.5f } : VillageGraveyardLocalToWorld(_villageGraveyardRespawnLocal));
    _cameraPos = _player.GetWorldPos();
    _shakeOffset = Vector2Zero();
    _gameState = sandboxMode ? GameState::VillagePlayground : GameState::Village;
}

bool Engine::VillageHasPlacedObject(const std::string& defName) const
{
    for (const VillagePlacedObject& placed : _villagePlacedObjects) if (placed.defName == defName) return true;
    return false;
}

void Engine::UnloadVillagePlayground()
{
    for (Texture2D& tex : _villagePlaygroundSheets) if (tex.id != 0) UnloadTexture(tex);
    for (VillageRuntimeObjectDef& def : _villageObjectCatalog) if (def.pngTexture.id != 0) UnloadTexture(def.pngTexture);
    _villagePlaygroundSheets.clear(); _villagePlaygroundSheetNames.clear(); _villageObjectCatalog.clear(); _villagePlacedObjects.clear(); _villageCitizens.clear(); _villageActiveObjectIndex = -1;
    _villageGraveyardColliders.clear();
    _villageZephShopColliders.clear();
    if (_villageFieldSheet.id != 0) { UnloadTexture(_villageFieldSheet); _villageFieldSheet = {}; }
    if (_villageFieldGroundSheet.id != 0) { UnloadTexture(_villageFieldGroundSheet); _villageFieldGroundSheet = {}; }
    if (_villageGraveyardTex.id != 0) { UnloadTexture(_villageGraveyardTex); _villageGraveyardTex = {}; }
    if (_villageZephShopTex.id != 0) { UnloadTexture(_villageZephShopTex); _villageZephShopTex = {}; }
}

void Engine::LoadVillagePlaygroundSheets()
{
    if (!_villagePlaygroundSheets.empty()) return;
    const char* pinned[] = { "Village_Ground", "Village", "Village_Objects01", "Village_Objects02", "Interior" };
    std::set<std::string> pinnedSet;
    for (const char* stem : pinned)
    {
        pinnedSet.insert(stem);
        _villagePlaygroundSheetNames.push_back(stem);
        Texture2D tex = LoadTexture(AssetPath(TextFormat("MapTilesets/%s.png", stem)).c_str());
        if (tex.id != 0) SetTextureFilter(tex, TEXTURE_FILTER_POINT);
        _villagePlaygroundSheets.push_back(tex);
    }
    std::vector<std::string> discovered;
    FilePathList files = LoadDirectoryFiles(AssetFolderPath("MapTilesets").c_str());
    for (unsigned int i = 0; i < files.count; i++)
    {
        const char* fileName = GetFileName(files.paths[i]);
        if (!fileName || !IsFileExtension(fileName, ".png")) continue;
        std::string stem = fileName; size_t dot = stem.find_last_of('.'); if (dot != std::string::npos) stem = stem.substr(0, dot);
        if (pinnedSet.find(stem) == pinnedSet.end()) discovered.push_back(stem);
    }
    UnloadDirectoryFiles(files);
    std::sort(discovered.begin(), discovered.end());
    for (const std::string& stem : discovered)
    {
        _villagePlaygroundSheetNames.push_back(stem);
        Texture2D tex = LoadTexture(AssetPath(TextFormat("MapTilesets/%s.png", stem.c_str())).c_str());
        if (tex.id != 0) SetTextureFilter(tex, TEXTURE_FILTER_POINT);
        _villagePlaygroundSheets.push_back(tex);
    }
    _villageTileDefs = {};
    _villageTileDefs.LoadFromFile(AssetPath("tilemapper_Forest.txt").c_str());
    _villageFieldSheet = LoadTexture(AssetPath("MapTilesets/Forest.png").c_str());
    _villageFieldGroundSheet = LoadTexture(AssetPath("MapTilesets/Ground TIles.png").c_str());

    _villageGraveyardPoeLocal = Vector2{ 30.90f, 123.72f };
    _villageGraveyardRespawnLocal = Vector2{ 104.76f, 117.84f };
    _villageGraveyardColliders.clear();
    _villageZephShopColliders.clear();
    std::string graveyardImage = "VillageGraveyard.png";
    FILE* graveMeta = fopen(AssetPath("VillageAssets/VillageGraveyard.vasset").c_str(), "r");
    if (graveMeta)
    {
        char line[256];
        while (fgets(line, sizeof(line), graveMeta))
        {
            char imageName[128] = {};
            if (sscanf(line, "image %127s", imageName) == 1)
            {
                graveyardImage = imageName;
                continue;
            }

            float cx = 0.f, cy = 0.f, cw = 0.f, ch = 0.f;
            if (sscanf(line, "collider %f %f %f %f", &cx, &cy, &cw, &ch) == 4)
            {
                if (cw > 0.f && ch > 0.f) _villageGraveyardColliders.push_back(Rectangle{ cx, cy, cw, ch });
                continue;
            }

            char markerName[64] = {}; float x = 0.f, y = 0.f;
            if (sscanf(line, "marker %63s %f %f", markerName, &x, &y) != 3) continue;
            if (strcmp(markerName, "Poe") == 0) _villageGraveyardPoeLocal = Vector2{ x, y };
            else if (strcmp(markerName, "Respawn") == 0) _villageGraveyardRespawnLocal = Vector2{ x, y };
        }
        fclose(graveMeta);
    }
    _villageGraveyardTex = LoadTexture(AssetPath(TextFormat("VillageAssets/%s", graveyardImage.c_str())).c_str());
    if (_villageGraveyardTex.id == 0 && graveyardImage != "VillageGraveyard.png")
        _villageGraveyardTex = LoadTexture(AssetPath("VillageAssets/VillageGraveyard.png").c_str());
    if (_villageGraveyardTex.id != 0) SetTextureFilter(_villageGraveyardTex, TEXTURE_FILTER_POINT);

    _villageZephShopZephLocal = Vector2{ 50.22f, 124.20f };
    _villageZephShopColliders.clear();
    std::string zephShopImage = "ZephsShop.png";
    FILE* zephMeta = fopen(AssetPath("VillageAssets/ZephsShop.vasset").c_str(), "r");
    if (zephMeta)
    {
        char line[256];
        while (fgets(line, sizeof(line), zephMeta))
        {
            char imageName[128] = {};
            if (sscanf(line, "image %127s", imageName) == 1)
            {
                zephShopImage = imageName;
                continue;
            }

            float cx = 0.f, cy = 0.f, cw = 0.f, ch = 0.f;
            if (sscanf(line, "collider %f %f %f %f", &cx, &cy, &cw, &ch) == 4)
            {
                if (cw > 0.f && ch > 0.f) _villageZephShopColliders.push_back(Rectangle{ cx, cy, cw, ch });
                continue;
            }

            char markerName[64] = {}; float x = 0.f, y = 0.f;
            if (sscanf(line, "marker %63s %f %f", markerName, &x, &y) != 3) continue;
            if (strcmp(markerName, "Zeph") == 0) _villageZephShopZephLocal = Vector2{ x, y };
        }
        fclose(zephMeta);
    }
    _villageZephShopTex = LoadTexture(AssetPath(TextFormat("VillageAssets/%s", zephShopImage.c_str())).c_str());
    if (_villageZephShopTex.id == 0 && zephShopImage != "ZephsShop.png")
        _villageZephShopTex = LoadTexture(AssetPath("VillageAssets/ZephsShop.png").c_str());
    if (_villageZephShopTex.id != 0) SetTextureFilter(_villageZephShopTex, TEXTURE_FILTER_POINT);
}

void Engine::LoadVillagePlaygroundCatalog()
{
    _villageObjectCatalog.clear();
    auto scanFolder = [&](const char* folder)
    {
        FilePathList files = LoadDirectoryFiles(folder);
        for (unsigned int i = 0; i < files.count; i++)
        {
            const char* fileName = GetFileName(files.paths[i]); if (!fileName) continue;
            std::string name = fileName;
            if (name.rfind("villageobject_", 0) != 0 || !IsFileExtension(fileName, ".txt")) continue;
            std::string path = (strcmp(folder, ".") == 0) ? name : (std::string(folder) + "/" + name);
            VillageRuntimeObjectDef def; if (LoadVillageRuntimeObject(path, def)) _villageObjectCatalog.push_back(def);
        }
        UnloadDirectoryFiles(files);
    };
    scanFolder("."); scanFolder("TestGame");

    FilePathList assetFiles = LoadDirectoryFiles(AssetFolderPath("VillageAssets").c_str());
    for (unsigned int i = 0; i < assetFiles.count; ++i)
    {
        const char* fileName = GetFileName(assetFiles.paths[i]);
        if (!fileName || !IsFileExtension(fileName, ".vasset")) continue;
        std::string stem = fileName;
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        if (stem == "VillageGraveyard") continue;

        VillageRuntimeObjectDef def;
        if (!LoadVillageRuntimeAssetObject(std::string("VillageAssets/") + fileName, def)) continue;

        auto existing = std::find_if(_villageObjectCatalog.begin(), _villageObjectCatalog.end(), [&](const VillageRuntimeObjectDef& e) { return e.name == def.name; });
        if (existing != _villageObjectCatalog.end())
        {
            if (existing->pngTexture.id != 0) UnloadTexture(existing->pngTexture);
            *existing = def;
        }
        else
        {
            _villageObjectCatalog.push_back(def);
        }
    }
    UnloadDirectoryFiles(assetFiles);

    std::sort(_villageObjectCatalog.begin(), _villageObjectCatalog.end(), [](const VillageRuntimeObjectDef& a, const VillageRuntimeObjectDef& b) { return a.name < b.name; });
}

bool Engine::LoadVillageRuntimeObject(const std::string& path, VillageRuntimeObjectDef& outDef) const
{
    FILE* f = fopen(AssetPath(path.c_str()).c_str(), "r");
    if (!f) f = fopen(path.c_str(), "r");
    if (!f) return false;

    outDef = {};
    outDef.path = path;
    int minCol = INT_MAX, minRow = INT_MAX, maxCol = INT_MIN, maxRow = INT_MIN;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char name[128] = {};
        if (sscanf(line, "object %127s", name) == 1) { outDef.name = name; continue; }
        int cost = 0;
        if (sscanf(line, "cost %d", &cost) == 1) { outDef.costGold = cost; continue; }
        if (strncmp(line, "decor", 5) == 0) { outDef.isDecoration = true; continue; }

        char layerChar = 0, sheetName[128] = {};
        int localCol = 0, localRow = 0, sheet = -1, col = 0, row = 0;
        if (sscanf(line, "part %c %d %d %d %d %d %127s", &layerChar, &localCol, &localRow, &sheet, &col, &row, sheetName) >= 6)
        {
            VillageRuntimePart part{};
            part.layer = (layerChar == 'g') ? VillageObjectLayer::Ground : (layerChar == 'h') ? VillageObjectLayer::Overhead : VillageObjectLayer::Objects;
            part.localCol = (short)localCol;
            part.localRow = (short)localRow;
            part.sheet = (short)sheet;
            part.col = (short)col;
            part.row = (short)row;
            if (sheetName[0])
            {
                for (int i = 0; i < (int)_villagePlaygroundSheetNames.size(); ++i)
                {
                    if (_villagePlaygroundSheetNames[i] == sheetName) { part.sheet = (short)i; break; }
                }
            }
            outDef.parts.push_back(part);
            minCol = std::min(minCol, localCol);
            minRow = std::min(minRow, localRow);
            maxCol = std::max(maxCol, localCol);
            maxRow = std::max(maxRow, localRow);
            continue;
        }

        int w = 0, h = 0;
        if (sscanf(line, "solid %d %d %d %d", &localCol, &localRow, &w, &h) == 4)
        {
            VillageRuntimeSolid solid{};
            solid.localCol = (short)localCol;
            solid.localRow = (short)localRow;
            solid.w = (short)std::max(1, w);
            solid.h = (short)std::max(1, h);
            outDef.solids.push_back(solid);
            continue;
        }

        char doorName[128] = {}, target[128] = {};
        int doorId = 0, blocksClosed = 1, openAnim = -1;
        if (sscanf(line, "door %d %127s pos=%d,%d size=%d,%d target=%127s blocks_closed=%d open_anim=%d",
                   &doorId, doorName, &localCol, &localRow, &w, &h, target, &blocksClosed, &openAnim) >= 7)
        {
            VillageRuntimeDoor door{};
            door.doorName = doorName;
            door.localCol = (short)localCol;
            door.localRow = (short)localRow;
            door.w = (short)std::max(1, w);
            door.h = (short)std::max(1, h);
            door.targetInterior = target;
            door.blocksWhenClosed = blocksClosed != 0;
            door.openAnimationIndex = openAnim;
            outDef.doors.push_back(door);
            continue;
        }

        char npcName[128] = {}, assignment[128] = {};
        if (sscanf(line, "npc %127s pos=%d,%d assignment=%127s", npcName, &localCol, &localRow, assignment) >= 3)
        {
            VillageRuntimeNpc npc{};
            npc.npcName = npcName;
            npc.localCol = (short)localCol;
            npc.localRow = (short)localRow;
            npc.assignment = assignment;
            outDef.npcs.push_back(npc);
            continue;
        }
    }
    fclose(f);

    if (outDef.name.empty())
    {
        std::string file = GetFileName(path.c_str());
        size_t dot = file.find_last_of('.');
        if (dot != std::string::npos) file = file.substr(0, dot);
        const std::string prefix = "villageobject_";
        if (file.rfind(prefix, 0) == 0) file = file.substr(prefix.size());
        outDef.name = file;
    }

    if (!outDef.parts.empty() && minCol != INT_MAX)
    {
        for (VillageRuntimePart& part : outDef.parts) { part.localCol -= (short)minCol; part.localRow -= (short)minRow; }
        for (VillageRuntimeSolid& solid : outDef.solids) { solid.localCol -= (short)minCol; solid.localRow -= (short)minRow; }
        for (VillageRuntimeDoor& door : outDef.doors) { door.localCol -= (short)minCol; door.localRow -= (short)minRow; }
        for (VillageRuntimeNpc& npc : outDef.npcs) { npc.localCol -= (short)minCol; npc.localRow -= (short)minRow; }
        outDef.cols = std::max(1, maxCol - minCol + 1);
        outDef.rows = std::max(1, maxRow - minRow + 1);
    }

    // PNG-authored Zeph shop: keep it in the build catalog, but let the asset
    // dimensions drive the placement footprint so the ghost matches the image.
    if (outDef.name == "ZephsShop" && _villageZephShopTex.id != 0)
    {
        outDef.cols = std::max(1, (int)std::ceil((_villageZephShopTex.width * kVillageZephShopAssetScale) / kVillagePlaygroundTilePx));
        outDef.rows = std::max(1, (int)std::ceil((_villageZephShopTex.height * kVillageZephShopAssetScale) / kVillagePlaygroundTilePx));
        outDef.costGold = 0;
    }
    return true;
}

bool Engine::LoadVillageRuntimeAssetObject(const std::string& path, VillageRuntimeObjectDef& outDef) const
{
    VillageAssetData data;
    if (!VillageAssetLoader::Load(AssetPath(path.c_str()), data))
    {
        if (!VillageAssetLoader::Load(path, data)) return false;
    }

    outDef = {};
    outDef.name = data.id.empty() ? std::string(GetFileName(path.c_str())) : data.id;
    size_t dot = outDef.name.find_last_of('.');
    if (dot != std::string::npos) outDef.name = outDef.name.substr(0, dot);
    outDef.path = path;
    outDef.costGold = data.cost;
    outDef.isDecoration = data.category == VillageBuildCategory::Decor;
    outDef.required = data.required;
    outDef.uniqueInVillage = data.unique;
    outDef.removable = data.removable;
    outDef.serviceName = VillageAssetLoader::ToString(data.service);
    outDef.pngScale = (outDef.name == "ZephsShop") ? kVillageZephShopAssetScale : 3.f;
    outDef.pngAnimated = data.animation.enabled;
    outDef.pngAnimCols = std::max(1, data.animation.columns);
    outDef.pngAnimRows = std::max(1, data.animation.rows);
    outDef.pngAnimFrames = std::max(1, std::min(data.animation.frameCount, outDef.pngAnimCols * outDef.pngAnimRows));
    outDef.pngAnimFps = std::max(0.1f, data.animation.fps);

    std::string imageFile = data.imageFile.empty() ? (outDef.name + ".png") : data.imageFile;
    outDef.pngTexture = LoadTexture(AssetPath(TextFormat("VillageAssets/%s", imageFile.c_str())).c_str());
    if (outDef.pngTexture.id != 0) SetTextureFilter(outDef.pngTexture, TEXTURE_FILTER_POINT);
    if (outDef.pngTexture.id == 0) return false;

    float imageW = data.imageSize.x > 0.f ? data.imageSize.x : (float)(outDef.pngAnimated ? outDef.pngTexture.width / outDef.pngAnimCols : outDef.pngTexture.width);
    float imageH = data.imageSize.y > 0.f ? data.imageSize.y : (float)(outDef.pngAnimated ? outDef.pngTexture.height / outDef.pngAnimRows : outDef.pngTexture.height);
    outDef.cols = std::max(1, (int)std::ceil((imageW * outDef.pngScale) / kVillagePlaygroundTilePx));
    outDef.rows = std::max(1, (int)std::ceil((imageH * outDef.pngScale) / kVillagePlaygroundTilePx));

    for (const VaRect& collider : data.colliders)
    {
        if (collider.w > 0.f && collider.h > 0.f)
            outDef.pngColliders.push_back(Rectangle{ collider.x, collider.y, collider.w, collider.h });
    }
    return true;
}
int Engine::FindVillageObjectDefIndex(const std::string& defName) const
{
    for (int i = 0; i < (int)_villageObjectCatalog.size(); ++i)
        if (_villageObjectCatalog[i].name == defName) return i;
    return -1;
}

void Engine::SaveVillageLayout() const
{
    if (_villageSandboxMode) return;
    FILE* f = fopen(AssetPath("TestGame/village_layout.txt").c_str(), "w");
    if (!f) f = fopen("TestGame/village_layout.txt", "w");
    if (!f) return;
    fprintf(f, "village_layout 1\n");
    for (const VillagePlacedObject& placed : _villagePlacedObjects)
        fprintf(f, "place %s %d %d\n", placed.defName.c_str(), placed.cellCol, placed.cellRow);
    fclose(f);
}

void Engine::LoadVillageLayout()
{
    _villagePlacedObjects.clear();
    if (_villageSandboxMode) return;

    VillageLayout layout = VillageLayoutLoader::Load(AssetPath("VillageAssets/VillageLayout.vlayout"));
    for (const VillageLayoutObject& object : layout.objects)
    {
        if (object.assetName == "VillageGraveyard") continue; // drawn by its dedicated runtime path
        VillagePlacedObject placed{};
        placed.defName = object.assetName;
        placed.defIndex = FindVillageObjectDefIndex(placed.defName);
        placed.cellCol = (int)std::round(object.worldOrigin.x / kVillagePlaygroundTilePx);
        placed.cellRow = (int)std::round(object.worldOrigin.y / kVillagePlaygroundTilePx);
        if (placed.defIndex >= 0) _villagePlacedObjects.push_back(placed);
    }
    RefreshVillageShopNpc();
}

bool Engine::VillageObjectHasSolidAt(const VillageRuntimeObjectDef& def, int localCol, int localRow) const
{
    for (const VillageRuntimeSolid& solid : def.solids)
    {
        if (localCol >= solid.localCol && localCol < solid.localCol + solid.w &&
            localRow >= solid.localRow && localRow < solid.localRow + solid.h)
            return true;
    }
    return false;
}

bool Engine::VillageObjectIsOneTimeService(const VillageRuntimeObjectDef& def)
{
    return def.uniqueInVillage;
}

bool Engine::VillageObjectIsPermanentService(const VillageRuntimeObjectDef& def)
{
    return def.required && !def.removable;
}

const char* Engine::VillageObjectRuleLabel(const VillageRuntimeObjectDef& def)
{
    if (VillageObjectIsPermanentService(def)) return "one-time service";
    if (def.isDecoration) return "decor";
    return "buildable";
}

Rectangle Engine::VillagePlacedObjectWorldRect(const VillagePlacedObject& placed, const VillageRuntimeSolid* solid) const
{
    if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) return Rectangle{};
    const VillageRuntimeObjectDef& def = _villageObjectCatalog[placed.defIndex];
    int localCol = solid ? solid->localCol : 0;
    int localRow = solid ? solid->localRow : 0;
    int w = solid ? solid->w : def.cols;
    int h = solid ? solid->h : def.rows;
    return Rectangle{ (placed.cellCol + localCol) * kVillagePlaygroundTilePx,
                      (placed.cellRow + localRow) * kVillagePlaygroundTilePx,
                      w * kVillagePlaygroundTilePx,
                      h * kVillagePlaygroundTilePx };
}

bool Engine::VillagePlaygroundCanPlace(int defIndex, int cellCol, int cellRow) const
{
    if (defIndex < 0 || defIndex >= (int)_villageObjectCatalog.size()) return false;
    const VillageRuntimeObjectDef& def = _villageObjectCatalog[defIndex];
    if (!_villageSandboxMode && VillageObjectIsOneTimeService(def) && VillageHasPlacedObject(def.name)) return false;

    Rectangle footprint{ cellCol * kVillagePlaygroundTilePx, cellRow * kVillagePlaygroundTilePx,
                         def.cols * kVillagePlaygroundTilePx, def.rows * kVillagePlaygroundTilePx };
    Rectangle walk = VillagePlaygroundWalkableRect();
    if (footprint.x < walk.x || footprint.y < walk.y ||
        footprint.x + footprint.width > walk.x + walk.width ||
        footprint.y + footprint.height > walk.y + walk.height) return false;

    auto objectBlockingRects = [&](const VillageRuntimeObjectDef& objectDef, int col, int row)
    {
        std::vector<Rectangle> rects;
        if (!objectDef.pngColliders.empty())
        {
            for (const Rectangle& local : objectDef.pngColliders)
                rects.push_back(Rectangle{ col * kVillagePlaygroundTilePx + local.x * objectDef.pngScale,
                                           row * kVillagePlaygroundTilePx + local.y * objectDef.pngScale,
                                           local.width * objectDef.pngScale,
                                           local.height * objectDef.pngScale });
        }
        else if (objectDef.name == "ZephsShop" && !_villageZephShopColliders.empty())
        {
            for (const Rectangle& local : _villageZephShopColliders)
                rects.push_back(VillageZephShopLocalRectToWorld(col, row, local));
        }
        else
        {
            for (const VillageRuntimeSolid& solid : objectDef.solids)
            {
                rects.push_back(Rectangle{ (col + solid.localCol) * kVillagePlaygroundTilePx,
                                           (row + solid.localRow) * kVillagePlaygroundTilePx,
                                           solid.w * kVillagePlaygroundTilePx,
                                           solid.h * kVillagePlaygroundTilePx });
            }
        }
        if (rects.empty())
        {
            rects.push_back(Rectangle{ col * kVillagePlaygroundTilePx,
                                       row * kVillagePlaygroundTilePx,
                                       objectDef.cols * kVillagePlaygroundTilePx,
                                       objectDef.rows * kVillagePlaygroundTilePx });
        }
        return rects;
    };

    std::vector<Rectangle> candidateRects = objectBlockingRects(def, cellCol, cellRow);

    for (const Rectangle& candidate : candidateRects)
    {
        if (_villageGraveyardColliders.empty())
        {
            if (CheckCollisionRecs(candidate, VillageGraveyardWorldRect())) return false;
        }
        else
        {
            for (const Rectangle& collider : _villageGraveyardColliders)
                if (CheckCollisionRecs(candidate, VillageGraveyardLocalRectToWorld(collider))) return false;
        }
        if (CheckCollisionRecs(candidate, VillageGateApproachRect())) return false;
    }

    for (const VillagePlacedObject& placed : _villagePlacedObjects)
    {
        if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
        const VillageRuntimeObjectDef& other = _villageObjectCatalog[placed.defIndex];
        std::vector<Rectangle> otherRects = objectBlockingRects(other, placed.cellCol, placed.cellRow);
        for (const Rectangle& candidate : candidateRects)
            for (const Rectangle& otherRect : otherRects)
                if (CheckCollisionRecs(candidate, otherRect)) return false;
    }
    return true;
}

bool Engine::VillageCitizenSpotIsClear(Vector2 position) const
{
    Rectangle rect = VillageCitizenRect(position);
    Rectangle walk = VillagePlaygroundWalkableRect();
    if (rect.x < walk.x || rect.y < walk.y || rect.x + rect.width > walk.x + walk.width || rect.y + rect.height > walk.y + walk.height) return false;
    if (_villageGraveyardColliders.empty())
    {
        if (CheckCollisionRecs(rect, VillageGraveyardWorldRect())) return false;
    }
    else
    {
        for (const Rectangle& collider : _villageGraveyardColliders)
            if (CheckCollisionRecs(rect, VillageGraveyardLocalRectToWorld(collider))) return false;
    }
    for (const VillagePlacedObject& placed : _villagePlacedObjects)
    {
        if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
        const VillageRuntimeObjectDef& def = _villageObjectCatalog[placed.defIndex];
        for (const VillageRuntimeSolid& solid : def.solids)
            if (CheckCollisionRecs(rect, VillagePlacedObjectWorldRect(placed, &solid))) return false;
    }
    return true;
}

Vector2 Engine::VillageRandomStandablePoint() const
{
    Rectangle walk = VillagePlaygroundWalkableRect();
    for (int i = 0; i < 80; ++i)
    {
        Vector2 p{ walk.x + (float)GetRandomValue(32, (int)walk.width - 32),
                   walk.y + (float)GetRandomValue(32, (int)walk.height - 32) };
        if (VillageCitizenSpotIsClear(p)) return p;
    }
    return Vector2{ walk.x + walk.width * 0.5f, walk.y + walk.height * 0.5f };
}

void Engine::SyncVillageCitizenCount()
{
    if (_villageSandboxMode) return;
    int buildingCount = 0;
    for (const VillagePlacedObject& placed : _villagePlacedObjects)
    {
        if (placed.defIndex >= 0 && placed.defIndex < (int)_villageObjectCatalog.size() && !_villageObjectCatalog[placed.defIndex].isDecoration)
            ++buildingCount;
    }
    int wanted = std::min(kVillageCitizenCap, buildingCount / kVillageBuildingsPerCitizen);
    while ((int)_villageCitizens.size() < wanted)
    {
        VillageCitizen c{};
        c.position = VillageRandomStandablePoint();
        c.walkTarget = VillageRandomStandablePoint();
        c.behaviour = GetRandomValue(0, 1);
        c.behaviourTimer = (float)GetRandomValue(60, 180) / 30.f;
        c.tint = kVillageCitizenTints[_villageCitizens.size() % (sizeof(kVillageCitizenTints) / sizeof(kVillageCitizenTints[0]))];
        _villageCitizens.push_back(c);
    }
    while ((int)_villageCitizens.size() > wanted) _villageCitizens.pop_back();
}

void Engine::UpdateVillageCitizens(float deltaTime)
{
    for (VillageCitizen& c : _villageCitizens)
    {
        if (c.chatCooldown > 0.f) c.chatCooldown -= deltaTime;
        if (c.behaviour != 0)
        {
            c.behaviourTimer -= deltaTime;
            if (c.behaviourTimer <= 0.f)
            {
                c.behaviour = 0;
                c.chatPartnerIndex = -1;
                c.walkTarget = VillageRandomStandablePoint();
            }
            continue;
        }

        Vector2 to = Vector2Subtract(c.walkTarget, c.position);
        float dist = Vector2Length(to);
        if (dist < 6.f)
        {
            c.behaviour = 1;
            c.behaviourTimer = (float)GetRandomValue(50, 140) / 30.f;
        }
        else
        {
            Vector2 step = Vector2Scale(Vector2Normalize(to), kVillageCitizenWalkSpeed * deltaTime);
            Vector2 before = c.position;
            c.position = Vector2Add(c.position, step);
            c.facingLeft = step.x < -0.01f;
            c.walkBobPhase += deltaTime * 8.f;
            if (!VillageCitizenSpotIsClear(c.position))
            {
                c.position = before;
                c.walkTarget = VillageRandomStandablePoint();
            }
        }
    }

    for (int i = 0; i < (int)_villageCitizens.size(); ++i)
    {
        if (_villageCitizens[i].behaviour != 0 || _villageCitizens[i].chatCooldown > 0.f) continue;
        for (int j = i + 1; j < (int)_villageCitizens.size(); ++j)
        {
            if (_villageCitizens[j].behaviour != 0 || _villageCitizens[j].chatCooldown > 0.f) continue;
            if (Vector2Distance(_villageCitizens[i].position, _villageCitizens[j].position) > kVillageCitizenChatRange) continue;
            _villageCitizens[i].behaviour = _villageCitizens[j].behaviour = 2;
            _villageCitizens[i].chatPartnerIndex = j;
            _villageCitizens[j].chatPartnerIndex = i;
            _villageCitizens[i].behaviourTimer = _villageCitizens[j].behaviourTimer = 2.5f;
            _villageCitizens[i].chatCooldown = _villageCitizens[j].chatCooldown = 8.f;
            break;
        }
    }
}

void Engine::DrawVillageCitizens(Vector2 worldOffset) const
{
    for (const VillageCitizen& c : _villageCitizens)
    {
        Vector2 p{ worldOffset.x + c.position.x, worldOffset.y + c.position.y };
        float bob = (c.behaviour == 0) ? sinf(c.walkBobPhase) * 2.f : 0.f;
        DrawCircle((int)p.x, (int)(p.y + 18.f), 11.f, Fade(BLACK, 0.25f));
        DrawRectangle((int)(p.x - 9.f), (int)(p.y - 10.f + bob), 18, 24, c.tint);
        DrawCircle((int)p.x, (int)(p.y - 17.f + bob), 10.f, Color{ 226, 184, 140, 255 });
        DrawRectangle((int)(p.x + (c.facingLeft ? -7.f : 4.f)), (int)(p.y - 19.f + bob), 3, 3, BLACK);
        if (c.behaviour == 2) DrawText("...", (int)(p.x - 10.f), (int)(p.y - 44.f), 18, RAYWHITE);
    }
}

Rectangle Engine::VillageDoorWorldRect(const VillagePlacedObject& placed, const VillageRuntimeDoor& door) const
{
    return Rectangle{ (placed.cellCol + door.localCol) * kVillagePlaygroundTilePx,
                      (placed.cellRow + door.localRow) * kVillagePlaygroundTilePx,
                      door.w * kVillagePlaygroundTilePx,
                      door.h * kVillagePlaygroundTilePx };
}

bool Engine::VillageDoorIsOpen(const VillagePlacedObject& placed, const VillageRuntimeDoor& door) const
{
    Rectangle doorRect = VillageDoorWorldRect(placed, door);
    Rectangle trigger = doorRect;
    trigger.x -= 20.f; trigger.y -= 20.f; trigger.width += 40.f; trigger.height += 40.f;
    return CheckCollisionPointRec(_player.GetWorldPos(), trigger);
}

void Engine::RefreshVillageShopNpc()
{
    _villageShopNpcActive = false;
    for (const VillagePlacedObject& placed : _villagePlacedObjects)
    {
        if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
        const VillageRuntimeObjectDef& def = _villageObjectCatalog[placed.defIndex];
        if (def.name == "ZephsShop" && _villageZephShopTex.id != 0)
        {
            Vector2 npcPos = VillageZephShopLocalToWorld(placed.cellCol, placed.cellRow, _villageZephShopZephLocal);
            _shop.Enter(npcPos, _player, _currentAct);
            _villageShopNpcActive = true;
            return;
        }
        for (const VillageRuntimeNpc& npc : def.npcs)
        {
            if (npc.npcName != "Zeph") continue;
            Vector2 npcPos{ (placed.cellCol + npc.localCol + 0.5f) * kVillagePlaygroundTilePx,
                            (placed.cellRow + npc.localRow + 0.9f) * kVillagePlaygroundTilePx };
            _shop.Enter(npcPos, _player, _currentAct);
            _villageShopNpcActive = true;
            return;
        }
    }
}

void Engine::ResolveVillagePlaygroundCollision(Vector2 beforePos)
{
    Vector2 pos = _player.GetWorldPos();
    Rectangle walk = VillagePlaygroundWalkableRect();
    if (pos.x < walk.x + 16.f || pos.y < walk.y + 16.f || pos.x > walk.x + walk.width - 16.f || pos.y > walk.y + walk.height - 16.f)
    {
        _player.SetWorldPos(Vector2{ Clamp(pos.x, walk.x + 16.f, walk.x + walk.width - 16.f), Clamp(pos.y, walk.y + 16.f, walk.y + walk.height - 16.f) });
        pos = _player.GetWorldPos();
    }

    Rectangle playerRect{ pos.x - 14.f, pos.y - 20.f, 28.f, 36.f };
    for (const Rectangle& collider : _villageGraveyardColliders)
    {
        if (CheckCollisionRecs(playerRect, VillageGraveyardLocalRectToWorld(collider)))
        {
            _player.SetWorldPos(beforePos);
            return;
        }
    }

    for (const VillagePlacedObject& placed : _villagePlacedObjects)
    {
        if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
        const VillageRuntimeObjectDef& def = _villageObjectCatalog[placed.defIndex];
        if (!def.pngColliders.empty())
        {
            for (const Rectangle& collider : def.pngColliders)
            {
                Rectangle worldCollider{ placed.cellCol * kVillagePlaygroundTilePx + collider.x * def.pngScale,
                                         placed.cellRow * kVillagePlaygroundTilePx + collider.y * def.pngScale,
                                         collider.width * def.pngScale,
                                         collider.height * def.pngScale };
                if (CheckCollisionRecs(playerRect, worldCollider))
                {
                    _player.SetWorldPos(beforePos);
                    return;
                }
            }
        }
        else if (def.name == "ZephsShop")
        {
            for (const Rectangle& collider : _villageZephShopColliders)
            {
                if (CheckCollisionRecs(playerRect, VillageZephShopLocalRectToWorld(placed.cellCol, placed.cellRow, collider)))
                {
                    _player.SetWorldPos(beforePos);
                    return;
                }
            }
        }
        for (const VillageRuntimeSolid& solid : def.solids)
        {
            bool blockedByDoor = false;
            for (const VillageRuntimeDoor& door : def.doors)
            {
                if (solid.localCol == door.localCol && solid.localRow == door.localRow && door.blocksWhenClosed && VillageDoorIsOpen(placed, door))
                    blockedByDoor = true;
            }
            if (!blockedByDoor && CheckCollisionRecs(playerRect, VillagePlacedObjectWorldRect(placed, &solid)))
            {
                _player.SetWorldPos(beforePos);
                return;
            }
        }
    }
}

void Engine::EnterVillageInterior(const std::string& interiorName, Vector2 villageReturnPos)
{
    _villageInsideInterior = true;
    _villageInteriorName = interiorName.empty() ? "interior_default" : interiorName;
    _villageInteriorReturnPos = villageReturnPos;
    _player.SetWorldPos(Vector2{ kVirtualWidth * 0.5f, kVirtualHeight * 0.62f });
    _cameraPos = _player.GetWorldPos();
    _villagePlaygroundMessage = "Inside " + _villageInteriorName + " - press E by the mat to leave";
    _villagePlaygroundMessageTimer = 2.f;
}

void Engine::ExitVillageInterior()
{
    _villageInsideInterior = false;
    _player.SetWorldPos(_villageInteriorReturnPos);
    _cameraPos = _player.GetWorldPos();
}

namespace
{
    Rectangle VillageInteriorExitMatRect()
    {
        return Rectangle{ kVirtualWidth * 0.5f - 48.f, kVirtualHeight * 0.74f, 96.f, 42.f };
    }
}

void Engine::UpdateVillageInterior(float dt)
{
    Vector2 before = _player.GetWorldPos();
    _player.Update(dt);
    Vector2 pos = _player.GetWorldPos();
    pos.x = Clamp(pos.x, 96.f, (float)kVirtualWidth - 96.f);
    pos.y = Clamp(pos.y, 120.f, (float)kVirtualHeight - 72.f);
    _player.SetWorldPos(pos);
    _cameraPos = _player.GetWorldPos();
    if ((IsKeyPressed(KEY_E) || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) &&
        CheckCollisionPointRec(_player.GetWorldPos(), VillageInteriorExitMatRect()))
    {
        ExitVillageInterior();
    }
    (void)before;
}

void Engine::DrawVillageInterior()
{
    ClearBackground(Color{ 20, 18, 22, 255 });
    DrawRectangle(96, 96, kVirtualWidth - 192, kVirtualHeight - 160, Color{ 48, 38, 34, 255 });
    DrawRectangleLinesEx(Rectangle{ 96.f, 96.f, (float)kVirtualWidth - 192.f, (float)kVirtualHeight - 160.f }, 4.f, Color{ 96, 74, 54, 255 });
    DrawRectangleRec(VillageInteriorExitMatRect(), Color{ 74, 54, 42, 255 });
    DrawText("Exit", (int)(VillageInteriorExitMatRect().x + 26.f), (int)(VillageInteriorExitMatRect().y + 10.f), 20, RAYWHITE);
    _player.DrawPlayer(_cameraPos);
}

void Engine::UpdateVillagePlayground(float dt)
{
    if (MO_DEV_TOOLS && !_villageSandboxMode && _firstVillageVisit && _demoCompleted && IsKeyPressed(KEY_F8))
    {
        _villageIntroDialogueActive = false;
        _villageIntroDialogueLine = 0;
        _firstVillageVisit = false;
        _meta.SetOnboardingComplete();
        _villagePlaygroundMessage = "Head north when you are ready.";
        _villagePlaygroundMessageTimer = 5.f;
    }

    if (_villageInsideInterior)
    {
        UpdateVillageInterior(dt);
        return;
    }

    if (_villagePlaygroundMessageTimer > 0.f) _villagePlaygroundMessageTimer -= dt;
    if (_villageSandboxMode && IsKeyPressed(KEY_B)) _villageBuildMode = !_villageBuildMode;
    if (!_villageSandboxMode) _villageBuildMode = false;
    if (_villageSandboxMode && IsKeyPressed(KEY_R))
    {
        _villagePlacedObjects.clear();
        _villageCitizens.clear();
        _villageShopNpcActive = false;
        _villagePlaygroundMessage = "Village tester cleared";
        _villagePlaygroundMessageTimer = 2.f;
    }

    bool tutorialLockActive = false;
    int zephShopDefIndex = FindVillageObjectDefIndex("ZephsShop");
    if (_villageBuildMode)
    {
        float wheel = GetMouseWheelMove();
        if (fabsf(wheel) > 0.01f) _villageCatalogScroll = std::max(0.f, _villageCatalogScroll - wheel * 44.f);
        if (IsKeyPressed(KEY_DOWN)) _villageActiveObjectIndex = std::min((int)_villageObjectCatalog.size() - 1, _villageActiveObjectIndex + 1);
        if (IsKeyPressed(KEY_UP)) _villageActiveObjectIndex = std::max(0, _villageActiveObjectIndex - 1);

        Vector2 mouse = GetVirtualMousePos();
        Rectangle panel{ kVirtualWidth - 448.f, 88.f, 430.f, 430.f };
        if (CheckCollisionPointRec(mouse, panel))
        {
            for (int i = 0; i < (int)_villageObjectCatalog.size(); ++i)
            {
                float y = panel.y + 130.f + i * 42.f - _villageCatalogScroll;
                Rectangle row{ panel.x + 8.f, y, panel.width - 16.f, 38.f };
                if (CheckCollisionPointRec(mouse, row) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                {
                    if (!tutorialLockActive || i == zephShopDefIndex) _villageActiveObjectIndex = i;
                }
            }
        }
        else
        {
            Vector2 mouseWorld{ mouse.x + _cameraPos.x - kVirtualWidth * 0.5f,
                                mouse.y + _cameraPos.y - kVirtualHeight * 0.5f };
            int cellCol = (int)floorf(mouseWorld.x / kVillagePlaygroundTilePx);
            int cellRow = (int)floorf(mouseWorld.y / kVillagePlaygroundTilePx);
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
            {
                for (int i = (int)_villagePlacedObjects.size() - 1; i >= 0; --i)
                {
                    VillagePlacedObject& placed = _villagePlacedObjects[i];
                    if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
                    const VillageRuntimeObjectDef& def = _villageObjectCatalog[placed.defIndex];
                    Rectangle rect{ placed.cellCol * kVillagePlaygroundTilePx, placed.cellRow * kVillagePlaygroundTilePx,
                                    def.cols * kVillagePlaygroundTilePx, def.rows * kVillagePlaygroundTilePx };
                    if (CheckCollisionPointRec(mouseWorld, rect))
                    {
                        if (!_villageSandboxMode && VillageObjectIsPermanentService(def))
                        {
                            _villagePlaygroundMessage = "This service building cannot be removed.";
                            _villagePlaygroundMessageTimer = 2.f;
                            break;
                        }
                        if (!_villageSandboxMode && def.costGold > 0) _player.AddGold(def.costGold);
                        _villagePlacedObjects.erase(_villagePlacedObjects.begin() + i);
                        SaveVillageLayout();
                        RefreshVillageShopNpc();
                        SyncVillageCitizenCount();
                        break;
                    }
                }
            }
            else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && _villageActiveObjectIndex >= 0)
            {
                if (tutorialLockActive && _villageActiveObjectIndex != zephShopDefIndex)
                {
                    _villagePlaygroundMessage = "Poe: Zeph's shop comes first.";
                    _villagePlaygroundMessageTimer = 2.f;
                }
                else if (VillagePlaygroundCanPlace(_villageActiveObjectIndex, cellCol, cellRow))
                {
                    const VillageRuntimeObjectDef& def = _villageObjectCatalog[_villageActiveObjectIndex];
                    if (!_villageSandboxMode && _player.GetGold() < def.costGold)
                    {
                        _villagePlaygroundMessage = "Not enough gold";
                        _villagePlaygroundMessageTimer = 2.f;
                    }
                    else
                    {
                        if (!_villageSandboxMode && def.costGold > 0) _player.AddGold(-def.costGold);
                        VillagePlacedObject placed{};
                        placed.defName = def.name;
                        placed.defIndex = _villageActiveObjectIndex;
                        placed.cellCol = cellCol;
                        placed.cellRow = cellRow;
                        _villagePlacedObjects.push_back(placed);
                        SaveVillageLayout();
                        RefreshVillageShopNpc();
                        SyncVillageCitizenCount();
                        if (!_villageSandboxMode && def.name == "ZephsShop")
                        {
                            _villageBuildMode = false;
                            _villagePlaygroundMessage = "Zeph has opened shop in the village.";
                            _villagePlaygroundMessageTimer = 3.f;
                        }
                    }
                }
                else
                {
                    const VillageRuntimeObjectDef& def = _villageObjectCatalog[_villageActiveObjectIndex];
                    if (!_villageSandboxMode && VillageObjectIsOneTimeService(def) && VillageHasPlacedObject(def.name))
                        _villagePlaygroundMessage = "Zeph's shop is already built.";
                    else
                        _villagePlaygroundMessage = "That spot is blocked.";
                    _villagePlaygroundMessageTimer = 1.5f;
                }
            }
        }
    }
    else
    {
        if (!_villageIntroDialogueActive)
        {
            Vector2 before = _player.GetWorldPos();
            _player.Update(dt);
            ResolveVillagePlaygroundCollision(before);
        }
        UpdateVillageCitizens(dt);

        bool gamepadInteract = _gamepad.isActive && (_gamepad.attackPressed || _gamepad.dashPressed);
        if (_villageShopNpcActive)
        {
            Vector2 shopWorldOffset{ -_cameraPos.x, -_cameraPos.y };
            if (_villageIntroDialogueActive)
            {
                bool advance = IsKeyPressed(KEY_E) || IsKeyPressed(KEY_ENTER) ||
                               IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || gamepadInteract;
                if (advance)
                {
                    if (_villageIntroDialogueLine < 3)
                        ++_villageIntroDialogueLine;
                    else
                    {
                        _villageIntroDialogueActive = false;
                        _firstVillageVisit = false;
                        _meta.SetOnboardingComplete();
                        _villagePlaygroundMessage = "Head north when you are ready.";
                        _villagePlaygroundMessageTimer = 5.f;
                    }
                }
            }
            else if (_shop.UpdateNpc(_player, shopWorldOffset, _touchModeActive, gamepadInteract))
            {
                if (_firstVillageVisit)
                {
                    _villageIntroDialogueActive = true;
                    _villageIntroDialogueLine = 0;
                }
                else
                {
                    _levelUpReturnState = _gameState;
                    _gameState = GameState::Shop;
                }
                return;
            }
        }

        _poeAltarBobTimer += dt;
        Vector2 poeWorld = VillageGraveyardLocalToWorld(_villageGraveyardPoeLocal);
        _nearPoeAltar = Vector2Distance(_player.GetWorldPos(), poeWorld) < 155.f;
        if (_nearPoeAltar && !_shop.IsNearNpc())
        {
            bool interactPressed = IsKeyPressed(KEY_E) || gamepadInteract;
            if (_touchModeActive && IsGestureDetected(GESTURE_TAP))
            {
                Vector2 tap = GetVirtualTouchPos(0);
                Vector2 poeScreen{ poeWorld.x - _cameraPos.x + kVirtualWidth * 0.5f,
                                   poeWorld.y - _cameraPos.y + kVirtualHeight * 0.5f };
                if (Vector2Distance(tap, poeScreen) < 170.f)
                    interactPressed = true;
            }
            if (interactPressed)
            {
                _metaShopCursor      = 0;
                _metaShopOpenTimer   = 0.25f;
                _metaShopReturnState = _gameState;
                _poeGreetingIdx      = GetRandomValue(0, kPoeGreetingCount - 1);
                _gameState           = GameState::MetaShop;
                return;
            }
        }

        if (!_villageSandboxMode && CheckCollisionPointRec(_player.GetWorldPos(), VillageGateApproachRect()))
        {
            if (_firstVillageVisit)
            {
                _villagePlaygroundMessage = "Speak with Zeph before leaving.";
                _villagePlaygroundMessageTimer = 2.f;
                return;
            }
            _villagePlaygroundMessage = "";
            if (_runSessionData.IsPausedInVillage())
            {
                _runSessionData.Resume();
                OpenWorldMap();
            }
            else
            {
                _pendingNewRunFromVillage = false;
                StartMainRun();
            }
            return;
        }

        for (const VillagePlacedObject& placed : _villagePlacedObjects)
        {
            if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
            const VillageRuntimeObjectDef& def = _villageObjectCatalog[placed.defIndex];
            if (def.name == "ZephsShop" && _villageZephShopTex.id != 0) continue;
            for (const VillageRuntimeDoor& door : def.doors)
            {
                if (!VillageDoorIsOpen(placed, door)) continue;
                if (CheckCollisionPointRec(_player.GetWorldPos(), VillageDoorWorldRect(placed, door)) && IsKeyPressed(KEY_E))
                {
                    Vector2 returnPos = _player.GetWorldPos();
                    returnPos.y += kVillagePlaygroundTilePx * 0.75f;
                    EnterVillageInterior(door.targetInterior, returnPos);
                    return;
                }
            }
        }
    }

    Rectangle field = VillagePlaygroundFieldRect();
    _cameraPos = _player.GetWorldPos();
    _cameraPos.x = Clamp(_cameraPos.x, kVirtualWidth * 0.5f, field.width - kVirtualWidth * 0.5f);
    _cameraPos.y = Clamp(_cameraPos.y, kVirtualHeight * 0.5f, field.height - kVirtualHeight * 0.5f);
}

void Engine::DrawVillageRuntimeObject(const VillageRuntimeObjectDef& def, int cellCol, int cellRow, Vector2 worldOffset, Color tint, VillageObjectLayer layer) const
{
    const Texture2D* png = def.pngTexture.id != 0 ? &def.pngTexture : ((def.name == "ZephsShop" && _villageZephShopTex.id != 0) ? &_villageZephShopTex : nullptr);
    if (png)
    {
        if (layer != VillageObjectLayer::Objects) return;
        int animCols = std::max(1, def.pngAnimCols);
        int animRows = std::max(1, def.pngAnimRows);
        int frameW = std::max(1, def.pngAnimated ? png->width / animCols : png->width);
        int frameH = std::max(1, def.pngAnimated ? png->height / animRows : png->height);
        int frameCount = std::max(1, std::min(def.pngAnimFrames, animCols * animRows));
        int frame = def.pngAnimated ? ((int)(GetTime() * std::max(0.1f, def.pngAnimFps)) % frameCount) : 0;
        Rectangle src{ (float)((frame % animCols) * frameW), (float)((frame / animCols) * frameH), (float)frameW, (float)frameH };
        Rectangle dst{ worldOffset.x + cellCol * kVillagePlaygroundTilePx,
                       worldOffset.y + cellRow * kVillagePlaygroundTilePx,
                       frameW * def.pngScale,
                       frameH * def.pngScale };
        DrawTexturePro(*png, src, dst, Vector2{}, 0.f, tint);
        return;
    }

    for (const VillageRuntimePart& part : def.parts)
    {
        if (part.layer != layer) continue;
        if (part.sheet < 0 || part.sheet >= (int)_villagePlaygroundSheets.size()) continue;
        const Texture2D& tex = _villagePlaygroundSheets[part.sheet];
        if (tex.id == 0) continue;
        Rectangle src{ part.col * 16.f, part.row * 16.f, 16.f, 16.f };
        Rectangle dst{ worldOffset.x + (cellCol + part.localCol) * kVillagePlaygroundTilePx,
                       worldOffset.y + (cellRow + part.localRow) * kVillagePlaygroundTilePx,
                       kVillagePlaygroundTilePx, kVillagePlaygroundTilePx };
        DrawTexturePro(tex, src, dst, Vector2{}, 0.f, tint);
    }
}

void Engine::DrawVillageField(Vector2 worldOffset) const
{
    Rectangle field = VillagePlaygroundFieldRect();
    DrawRectangle((int)worldOffset.x, (int)worldOffset.y, (int)field.width, (int)field.height, Color{ 54, 91, 54, 255 });

    int col0 = std::max(0, (int)floorf((-worldOffset.x) / kVillagePlaygroundTilePx) - 1);
    int row0 = std::max(0, (int)floorf((-worldOffset.y) / kVillagePlaygroundTilePx) - 1);
    int col1 = std::min(kVillagePlaygroundCols - 1, (int)ceilf((kVirtualWidth - worldOffset.x) / kVillagePlaygroundTilePx) + 1);
    int row1 = std::min(kVillagePlaygroundRows - 1, (int)ceilf((kVirtualHeight - worldOffset.y) / kVillagePlaygroundTilePx) + 1);

    for (int row = row0; row <= row1; ++row)
    {
        for (int col = col0; col <= col1; ++col)
        {
            TileType type = VillageFloorCellUsesVariant(col, row) ? TileType::FloorVariant : TileType::Floor;
            if (row == 0 && col == 0) type = TileType::WallCornerTL;
            else if (row == 0 && col == kVillagePlaygroundCols - 1) type = TileType::WallCornerTR;
            else if (row == kVillagePlaygroundRows - 1 && col == 0) type = TileType::WallCornerBL;
            else if (row == kVillagePlaygroundRows - 1 && col == kVillagePlaygroundCols - 1) type = TileType::WallCornerBR;
            else if (row == 0) type = TileType::WallTopFace;
            else if (row == kVillagePlaygroundRows - 1) type = TileType::WallBottom;
            else if (col == 0) type = TileType::WallLeft;
            else if (col == kVillagePlaygroundCols - 1) type = TileType::WallRight;

            int typeIdx = (int)type;
            bool useGround = typeIdx >= 0 && typeIdx < (int)TileType::Count && _villageTileDefs.fromGround[typeIdx];
            const Texture2D& tex = useGround ? _villageFieldGroundSheet : _villageFieldSheet;
            Rectangle dst{ worldOffset.x + col * kVillagePlaygroundTilePx,
                           worldOffset.y + row * kVillagePlaygroundTilePx,
                           kVillagePlaygroundTilePx, kVillagePlaygroundTilePx };

            if (tex.id != 0)
            {
                Rectangle src = _villageTileDefs.Get(type);
                DrawTexturePro(tex, src, dst, Vector2{}, 0.f, WHITE);
            }
            else
            {
                Color tint = (row == 0 || col == 0 || row == kVillagePlaygroundRows - 1 || col == kVillagePlaygroundCols - 1)
                    ? Color{ 45, 64, 42, 255 }
                    : (VillageFloorCellUsesVariant(col, row) ? Color{ 64, 103, 58, 255 } : Color{ 58, 96, 55, 255 });
                DrawRectangleRec(dst, tint);
            }
        }
    }

    Rectangle gate = VillageGateWorldRect();
    Rectangle gateScreen{ worldOffset.x + gate.x, worldOffset.y + gate.y, gate.width, gate.height };
    DrawRectangleRec(gateScreen, Color{ 68, 50, 38, 255 });
    DrawRectangleLinesEx(gateScreen, 3.f, Color{ 130, 100, 70, 255 });
    DrawText("DUNGEON GATE", (int)(gateScreen.x + 20.f), (int)(gateScreen.y + 20.f), 22, GOLD);
}

void Engine::DrawVillagePlacementGhost(Vector2 worldOffset, Vector2 mouseWorld)
{
    if (_villageActiveObjectIndex < 0 || _villageActiveObjectIndex >= (int)_villageObjectCatalog.size()) return;
    int cellCol = (int)floorf(mouseWorld.x / kVillagePlaygroundTilePx);
    int cellRow = (int)floorf(mouseWorld.y / kVillagePlaygroundTilePx);
    const VillageRuntimeObjectDef& def = _villageObjectCatalog[_villageActiveObjectIndex];
    bool ok = VillagePlaygroundCanPlace(_villageActiveObjectIndex, cellCol, cellRow);
    Color tint = ok ? Color{ 180, 255, 180, 170 } : Color{ 255, 80, 80, 165 };
    DrawVillageRuntimeObject(def, cellCol, cellRow, worldOffset, tint, VillageObjectLayer::Ground);
    DrawVillageRuntimeObject(def, cellCol, cellRow, worldOffset, tint, VillageObjectLayer::Objects);
    DrawVillageRuntimeObject(def, cellCol, cellRow, worldOffset, tint, VillageObjectLayer::Overhead);

    Rectangle footprint{ worldOffset.x + cellCol * kVillagePlaygroundTilePx,
                         worldOffset.y + cellRow * kVillagePlaygroundTilePx,
                         def.cols * kVillagePlaygroundTilePx,
                         def.rows * kVillagePlaygroundTilePx };
    DrawRectangleLinesEx(footprint, 2.f, ok ? Fade(GREEN, 0.55f) : Fade(RED, 0.55f));

    std::vector<Rectangle> colliderRects;
    if (!def.pngColliders.empty())
    {
        for (const Rectangle& local : def.pngColliders)
        {
            colliderRects.push_back(Rectangle{ worldOffset.x + cellCol * kVillagePlaygroundTilePx + local.x * def.pngScale,
                                               worldOffset.y + cellRow * kVillagePlaygroundTilePx + local.y * def.pngScale,
                                               local.width * def.pngScale,
                                               local.height * def.pngScale });
        }
    }
    else if (def.name == "ZephsShop" && !_villageZephShopColliders.empty())
    {
        for (const Rectangle& local : _villageZephShopColliders)
        {
            Rectangle worldRect = VillageZephShopLocalRectToWorld(cellCol, cellRow, local);
            colliderRects.push_back(Rectangle{ worldOffset.x + worldRect.x, worldOffset.y + worldRect.y, worldRect.width, worldRect.height });
        }
    }
    else
    {
        for (const VillageRuntimeSolid& solid : def.solids)
        {
            colliderRects.push_back(Rectangle{ worldOffset.x + (cellCol + solid.localCol) * kVillagePlaygroundTilePx,
                                               worldOffset.y + (cellRow + solid.localRow) * kVillagePlaygroundTilePx,
                                               solid.w * kVillagePlaygroundTilePx,
                                               solid.h * kVillagePlaygroundTilePx });
        }
    }

    for (const Rectangle& colliderRect : colliderRects)
    {
        DrawRectangleRec(colliderRect, ok ? Fade(GREEN, 0.16f) : Fade(RED, 0.18f));
        DrawRectangleLinesEx(colliderRect, 3.f, ok ? GREEN : RED);
    }
}

void Engine::DrawVillagePlayground()
{
    if (_villageInsideInterior)
    {
        DrawVillageInterior();
        return;
    }

    Vector2 worldOffset{ -_cameraPos.x + kVirtualWidth * 0.5f, -_cameraPos.y + kVirtualHeight * 0.5f };
    ClearBackground(Color{ 18, 24, 20, 255 });
    DrawVillageField(worldOffset);

    auto drawPlacedLayer = [&](VillageObjectLayer layer)
    {
        for (const VillagePlacedObject& placed : _villagePlacedObjects)
        {
            if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
            DrawVillageRuntimeObject(_villageObjectCatalog[placed.defIndex], placed.cellCol, placed.cellRow, worldOffset, WHITE, layer);
        }
    };

    drawPlacedLayer(VillageObjectLayer::Ground);

    Rectangle graveyard = VillageGraveyardWorldRect();
    Rectangle graveyardScreen{ worldOffset.x + graveyard.x, worldOffset.y + graveyard.y, graveyard.width, graveyard.height };
    if (_villageGraveyardTex.id != 0)
    {
        Rectangle src{ 0.f, 0.f, (float)_villageGraveyardTex.width, (float)_villageGraveyardTex.height };
        DrawTexturePro(_villageGraveyardTex, src, graveyardScreen, Vector2{}, 0.f, WHITE);
    }
    else
    {
        DrawRectangleRec(graveyardScreen, Color{ 42, 44, 56, 255 });
        DrawRectangleLinesEx(graveyardScreen, 2.f, Color{ 120, 118, 150, 255 });
        DrawText("Poe's Graveyard", (int)graveyardScreen.x + 20, (int)graveyardScreen.y + 20, 24, RAYWHITE);
    }

    Vector2 poeWorld = VillageGraveyardLocalToWorld(_villageGraveyardPoeLocal);
    Vector2 poeScreen{ worldOffset.x + poeWorld.x, worldOffset.y + poeWorld.y };
    float poeBob = sinf(_poeAltarBobTimer * 2.2f) * 5.f;
    DrawCircleV(Vector2{ poeScreen.x, poeScreen.y + poeBob }, 42.f, Fade(Color{ 184, 170, 236, 255 }, 0.14f));
    if (_cellsMerchantTex.id != 0)
    {
        const int frames = 6;
        float frameW = _cellsMerchantTex.width / (float)frames;
        int frame = ((int)(_poeAltarBobTimer * 6.f)) % frames;
        float scale = 3.0f;
        Rectangle src{ frame * frameW, 0.f, frameW, (float)_cellsMerchantTex.height };
        Rectangle dst{ poeScreen.x, poeScreen.y + poeBob, frameW * scale, _cellsMerchantTex.height * scale };
        DrawTexturePro(_cellsMerchantTex, src, dst, Vector2{ dst.width * 0.5f, dst.height * 0.5f }, 0.f, WHITE);
    }
    else
    {
        DrawCircle((int)poeScreen.x, (int)(poeScreen.y - 22.f + poeBob), 16.f, Color{ 142, 120, 255, 190 });
        DrawCircleLines((int)poeScreen.x, (int)(poeScreen.y - 22.f + poeBob), 17.f, Color{ 218, 210, 255, 220 });
    }
    DrawText("Poe", (int)(poeScreen.x - 18.f), (int)(poeScreen.y - 92.f + poeBob), 18, Color{ 220, 214, 255, 235 });
    if (_nearPoeAltar && !_shop.IsNearNpc())
    {
        const char* poePrompt = "[E] Poe";
        int promptFs = 24;
        int promptW = MeasureText(poePrompt, promptFs);
        DrawText(poePrompt, (int)(poeScreen.x - promptW * 0.5f), (int)(poeScreen.y + 78.f + poeBob), promptFs, Color{ 212, 194, 255, 255 });
    }

    drawPlacedLayer(VillageObjectLayer::Objects);

    for (const VillagePlacedObject& placed : _villagePlacedObjects)
    {
        if (placed.defIndex < 0 || placed.defIndex >= (int)_villageObjectCatalog.size()) continue;
        if (_villageObjectCatalog[placed.defIndex].name == "ZephsShop" && _villageZephShopTex.id != 0) continue;
        for (const VillageRuntimeDoor& door : _villageObjectCatalog[placed.defIndex].doors)
        {
            Rectangle doorRect = VillageDoorWorldRect(placed, door);
            Rectangle doorScreen{ worldOffset.x + doorRect.x, worldOffset.y + doorRect.y, doorRect.width, doorRect.height };
            if (VillageDoorIsOpen(placed, door))
            {
                DrawRectangleRec(doorScreen, Fade(GOLD, 0.28f));
                DrawRectangleLinesEx(doorScreen, 2.f, Fade(GOLD, 0.9f));
            }
            else
            {
                DrawRectangleLinesEx(doorScreen, 1.f, Fade(RAYWHITE, 0.25f));
            }
        }
    }

    DrawVillageCitizens(worldOffset);
    if (_villageShopNpcActive) _shop.DrawNpc(Vector2{ -_cameraPos.x, -_cameraPos.y });

    auto drawVillageIntroDialogue = [&]()
    {
        if (!_villageIntroDialogueActive) return;
        const char* line = kFirstZephLines[std::clamp(_villageIntroDialogueLine, 0, 3)];
        Rectangle panel{ kVirtualWidth * 0.14f, kVirtualHeight * 0.70f,
                         kVirtualWidth * 0.72f, kVirtualHeight * 0.20f };
        DrawRectangleRounded(panel, 0.08f, 8, Color{ 24, 20, 28, 242 });
        DrawRectangleRoundedLinesEx(panel, 0.08f, 8, 3.f, Color{ 220, 174, 82, 255 });
        DrawText("Zeph", (int)panel.x + 28, (int)panel.y + 18, 30, GOLD);
        const int fontSize = 25;
        const float maxWidth = panel.width - 56.f;
        float drawX = panel.x + 28.f;
        float drawY = panel.y + 64.f;
        std::string word;
        std::string wrappedLine;
        std::string dialogue(line);
        for (size_t i = 0; i <= dialogue.size(); ++i)
        {
            char ch = i < dialogue.size() ? dialogue[i] : ' ';
            if (ch == ' ')
            {
                std::string trial = wrappedLine.empty() ? word : wrappedLine + " " + word;
                if (!wrappedLine.empty() && MeasureText(trial.c_str(), fontSize) > (int)maxWidth)
                {
                    DrawText(wrappedLine.c_str(), (int)drawX, (int)drawY, fontSize, RAYWHITE);
                    drawY += fontSize + 7.f;
                    wrappedLine = word;
                }
                else
                {
                    wrappedLine = trial;
                }
                word.clear();
            }
            else
            {
                word += ch;
            }
        }
        if (!wrappedLine.empty())
            DrawText(wrappedLine.c_str(), (int)drawX, (int)drawY, fontSize, RAYWHITE);
        DrawText("E / Enter", (int)(panel.x + panel.width - 130.f),
                 (int)(panel.y + panel.height - 34.f), 20, Fade(RAYWHITE, 0.7f));
    };

    Vector2 mouse = GetVirtualMousePos();
    Vector2 mouseWorld{ mouse.x - worldOffset.x, mouse.y - worldOffset.y };
    bool mouseOverUi = _villageBuildMode && CheckCollisionPointRec(mouse, Rectangle{ kVirtualWidth - 448.f, 88.f, 430.f, 430.f });
    if (_villageBuildMode && !mouseOverUi) DrawVillagePlacementGhost(worldOffset, mouseWorld);

    _player.DrawPlayer(_cameraPos);
    drawPlacedLayer(VillageObjectLayer::Overhead);

    if (!_villageSandboxMode && !_villageBuildMode && CheckCollisionPointRec(_player.GetWorldPos(), VillageGateApproachRect()))
        DrawText("Head north when you are ready", 24, 92, 24, GOLD);

    DrawHUD(true);

    if (!_villageSandboxMode && _firstVillageVisit && _demoCompleted)
        DrawText("[F8] Skip Onboarding", (int)kVirtualWidth - 260, 22, 20, Fade(RAYWHITE, 0.75f));

    bool tutorialLockActive = false;
    int zephShopDefIndex = FindVillageObjectDefIndex("ZephsShop");
    if (_villageBuildMode)
    {
        Rectangle panel{ kVirtualWidth - 448.f, 88.f, 430.f, 430.f };
        DrawRectangleRounded(panel, 0.08f, 8, Fade(Color{ 18, 20, 18, 255 }, 0.82f));
        DrawRectangleRoundedLines(panel, 0.08f, 8, Color{ 100, 120, 90, 255 });
        DrawText("Build Mode", (int)panel.x + 14, (int)panel.y + 12, 22, GOLD);
        DrawText(_villageSandboxMode ? "B walk mode | R clear | tester is free" : TextFormat("B walk mode | your gold: %d", _player.GetGold()),
                 (int)panel.x + 14, (int)panel.y + 40, 15, Fade(RAYWHITE, 0.75f));
        DrawText(_villageSandboxMode ? "Left place | right remove | sandbox freely tests" : "Left place | right remove (services stay)",
                 (int)panel.x + 14, (int)panel.y + 60, 15, Fade(RAYWHITE, 0.75f));
        if (_villageActiveObjectIndex >= 0 && _villageActiveObjectIndex < (int)_villageObjectCatalog.size())
        {
            const VillageRuntimeObjectDef& selected = _villageObjectCatalog[_villageActiveObjectIndex];
            DrawText(TextFormat("Selected: %s", selected.name.c_str()), (int)panel.x + 14, (int)panel.y + 84, 16, RAYWHITE);
            const char* selectedCostText = selected.costGold > 0 ? TextFormat("%dg", selected.costGold) : "free";
            std::string selectedDetail = TextFormat("Cost: %s | Footprint: %dx%d | %s", selectedCostText, selected.cols, selected.rows, VillageObjectRuleLabel(selected));
            DrawText(selectedDetail.c_str(), (int)panel.x + 14, (int)panel.y + 104, 15, Fade(RAYWHITE, 0.72f));
        }

        Rectangle rows{ panel.x + 8.f, panel.y + 130.f, panel.width - 16.f, panel.height - 138.f };
        BeginScissorMode((int)rows.x, (int)rows.y, (int)rows.width, (int)rows.height);
        for (int i = 0; i < (int)_villageObjectCatalog.size(); ++i)
        {
            const VillageRuntimeObjectDef& entry = _villageObjectCatalog[i];
            float y = rows.y + i * 42.f - _villageCatalogScroll;
            if (y + 40.f < rows.y || y > rows.y + rows.height) continue;
            Rectangle row{ rows.x, y, rows.width, 38.f };
            bool active = i == _villageActiveObjectIndex;
            bool locked = tutorialLockActive && i != zephShopDefIndex;
            DrawRectangleRec(row, active ? Color{ 78, 92, 62, 235 } : Color{ 38, 44, 36, locked ? (unsigned char)140 : (unsigned char)225 });
            DrawRectangleLinesEx(row, 1.f, active ? GOLD : Color{ 82, 94, 72, 220 });
            DrawText(entry.name.c_str(), (int)row.x + 8, (int)row.y + 10, 17, active ? GOLD : (locked ? Fade(RAYWHITE, 0.35f) : RAYWHITE));
            if (locked) DrawText("locked", (int)(row.x + row.width - 132), (int)row.y + 11, 14, Fade(RAYWHITE, 0.4f));
            else
            {
                const bool alreadyBuilt = !_villageSandboxMode && VillageObjectIsOneTimeService(entry) && VillageHasPlacedObject(entry.name);
                const char* costText = entry.costGold > 0 ? TextFormat("%dg", entry.costGold) : "free";
                DrawText(costText, (int)(row.x + 170), (int)row.y + 11, 14, GOLD);
                DrawText(TextFormat("%dx%d", entry.cols, entry.rows), (int)(row.x + 224), (int)row.y + 11, 14, Fade(RAYWHITE, 0.68f));
                DrawText(alreadyBuilt ? "built" : VillageObjectRuleLabel(entry), (int)(row.x + 278), (int)row.y + 11, 14, alreadyBuilt ? Fade(GOLD, 0.78f) : Fade(RAYWHITE, 0.68f));
            }
        }
        if (_villageObjectCatalog.empty()) DrawText("No saved village objects yet", (int)rows.x + 10, (int)rows.y + 18, 20, Fade(RAYWHITE, 0.75f));
        EndScissorMode();
    }
    else if (_villageSandboxMode)
    {
        DrawText("[B] Build mode", 24, (int)(kVirtualHeight - 42), 22, Color{ 150, 170, 200, 220 });
    }

    if (tutorialLockActive)
    {
        const char* banner = "Poe: Press B to build Zeph's shop. It is free.";
        int bannerFs = 22;
        int bannerWidth = MeasureText(banner, bannerFs);
        DrawRectangle((int)(kVirtualWidth * 0.5f - bannerWidth * 0.5f - 14), 100, bannerWidth + 28, 36, Fade(Color{ 30, 20, 45, 255 }, 0.8f));
        DrawText(banner, (int)(kVirtualWidth * 0.5f - bannerWidth * 0.5f), 107, bannerFs, Color{ 200, 190, 255, 240 });
    }

    if (_villagePlaygroundMessageTimer > 0.f && !_villagePlaygroundMessage.empty())
    {
        int fs = 24;
        int w = MeasureText(_villagePlaygroundMessage.c_str(), fs);
        DrawRectangle((int)(kVirtualWidth * 0.5f - w * 0.5f - 16), 54, w + 32, 38, Fade(BLACK, 0.65f));
        DrawText(_villagePlaygroundMessage.c_str(), (int)(kVirtualWidth * 0.5f - w * 0.5f), 62, fs, GOLD);
    }

    // Dialogue is the final village foreground pass so HUD and world UI can
    // never cover the speaker or text.
    drawVillageIntroDialogue();
}

void Engine::Draw()
{
    switch (_gameState)
    {
    case GameState::Menu:
    {
        _menu.Draw();

        // Bestiary + Daily Run hints — bottom-left corner.
        DrawText("[B] Bestiary    [T] Daily Run", 24, (int)(kVirtualHeight - 42), 22, Color{ 150, 170, 200, 220 });

        // Ascension selector overlay (only once the player has unlocked tier 1+).
        if (_meta.GetMaxAscensionUnlocked() > 0)
        {
            float sw = (float)kVirtualWidth;
            int tier = _meta.GetSelectedAscension();
            const AscensionTierDef* def = GetAscensionTierDef(tier);
            const char* label = (tier == 0) ? "Ascension: OFF"
                                            : TextFormat("Ascension %d - %s", tier, def ? def->name : "");
            int fs = 30;
            int lw = MeasureText(label, fs);
            Color tierColor = (tier == 0) ? Color{ 190, 190, 200, 255 }
                                          : Color{ 255, 130, 90, 255 };
            DrawText(label, (int)(sw * 0.5f - lw * 0.5f), 126, fs, tierColor);

            // Show the rule this tier adds (cumulative on top of all lower tiers).
            const char* effect = (tier == 0) ? "Standard difficulty"
                                             : (def ? def->effect : "");
            int efs = 20;
            int ew = MeasureText(effect, efs);
            DrawText(effect, (int)(sw * 0.5f - ew * 0.5f), 166, efs, Color{ 200, 200, 214, 220 });

            Rectangle leftArrow{ sw * 0.5f - 190.f, 118.f, 44.f, 44.f };
            Rectangle rightArrow{ sw * 0.5f + 146.f, 118.f, 44.f, 44.f };
            bool canLeft  = tier > 0;
            bool canRight = tier < _meta.GetMaxAscensionUnlocked();
            DrawText("<", (int)(leftArrow.x + 12.f), (int)(leftArrow.y + 4.f), 40, canLeft ? RAYWHITE : Color{ 90, 90, 100, 255 });
            DrawText(">", (int)(rightArrow.x + 12.f), (int)(rightArrow.y + 4.f), 40, canRight ? RAYWHITE : Color{ 90, 90, 100, 255 });
        }
        break;
    }

    case GameState::Pause:
    {
        {
            if (_stateBeforePause == GameState::WorldMap)
            {
                // Show the map dimmed behind the pause menu.
                _worldMap.Draw(_player);
            }
            else if (_stateBeforePause == GameState::Village || _stateBeforePause == GameState::VillagePlayground)
            {
                DrawVillagePlayground();
            }
            else
            {
                ClearBackground(Color{ 8, 6, 10, 255 });
                float scaleX = (float)kVirtualWidth  / (RoomLayout::kCols * 16.f);
                float scaleY = (float)kVirtualHeight / (RoomLayout::kRows * 16.f);
                if (_tileRenderer.IsLoaded())
                    _tileRenderer.DrawRoom(_dungeonRoomLayout, scaleX, scaleY, { 0.f, 0.f });
            }
        }

        int pauseResult = _pauseUI.DrawPause(GetPromptModeForUi());
        if (pauseResult != 0) { StopSound(_buttonPressSound); PlaySound(_buttonPressSound); }
        if (pauseResult == 1)
            _gameState = _stateBeforePause;
        else if (pauseResult == 2)
        {
            _runState.OpenHowToPlay(GameState::Pause, _touchModeActive);
        }
        else if (pauseResult == 3)
            _shouldClose = true;
        else if (pauseResult == 4)
        {
            if (_touchModeActive)
            {
                EnterTouchButtonMapping();
                _gameState = GameState::TouchButtonMapping;
            }
            else
            {
                _stateBeforeSettings = GameState::Pause;
                _settingsTab         = 2;
                _settingsDragSlider  = -1;
                _settingsRebindSlot  = -1;
                _keybindingsEdit     = _player.GetBindings();
                _gameState           = GameState::Settings;
            }
        }
        else if (pauseResult == 5)
        {
            ResetRunState();
            _menu.Init();
            _gameState = GameState::Menu;
        }
        else if (pauseResult == 6)
        {
            _stateBeforeSettings = GameState::Pause;
            _settingsTab         = 0;
            _settingsDragSlider  = -1;
            _settingsRebindSlot  = -1;
            _keybindingsEdit     = _player.GetBindings();
            _gameState           = GameState::Settings;
        }

        break;
    }

    case GameState::DemoEnd:
        DrawDemoEnd();
        break;

    case GameState::DungeonRun:
        DrawDungeonRun();
        break;

    case GameState::WorldMap:
        DrawWorldMap();
        break;

    case GameState::Settings:
        DrawSettings();
        break;

    case GameState::ClassSelect:
        DrawClassSelect();
        break;

    case GameState::MetaShop:
        DrawMetaShop();
        break;

    case GameState::CurseShrine:
        DrawCurseShrine();
        break;

    case GameState::DecisionRoom:
        DrawDecisionRoom();
        break;

    case GameState::RelicChoice:
        DrawRelicChoice();
        break;

    case GameState::Bestiary:
        DrawBestiary();
        break;

    case GameState::TileMapper:
        _tileMapper.Draw();
        break;

    case GameState::NineSliceEditor:
        _nineSliceEditor.Draw();
        break;

    case GameState::CharacterAnimator:
        _charAnimator.Draw();
        break;

    case GameState::AttackEditor:
        _attackEditor.Draw();
        break;

    case GameState::MapEditor:
        _mapEditor.Draw();
        break;

    case GameState::VillagePlayground:
    case GameState::Village:
        DrawVillagePlayground();
        break;

    case GameState::GameOver:
    {
        int goResult = _pauseUI.DrawGameOver(GetPromptModeForUi());
        if (goResult != 0) { StopSound(_buttonPressSound); PlaySound(_buttonPressSound); }
        if (goResult == 1)
        {
            // Death sends the player back to the village, waking up at Poe's
            // Graveyard. No ResetRunState here — retained gold (Deep Pockets)
            // stays spendable on buildings; the next run resets at the gate.
            EnterVillage();

            _fadeInTimer    = 2.0f;
            _fadeInDuration = 2.0f;
        }
        else if (goResult == 2) { ResetRunState(); _menu.Init(); _gameState = GameState::Menu; }
        else if (goResult == 3) _shouldClose = true;
        break;
    }

    case GameState::DeathRevive:
        DrawDeathRevive();
        break;

    case GameState::BossChoice:
        DrawBossChoice();
        break;

    case GameState::HowToPlay:
    {
        DrawHowToPlay();
        break;
    }

    case GameState::Keybindings:
        // Legacy path: redirect straight into the Settings KEYBINDINGS tab
        _stateBeforeSettings = GameState::Pause;
        _settingsTab         = 2;
        _settingsDragSlider  = -1;
        _settingsRebindSlot  = -1;
        _keybindingsEdit     = _player.GetBindings();
        _gameState           = GameState::Settings;
        break;

    case GameState::TouchButtonMapping:
    {
        int r = DrawTouchButtonMapping();
        if (r == 1)  // Save
        {
            for (int i = 0; i < 6; i++) _touchCustomPos[i] = _touchMappingPos[i];
            _touchLayoutCustom = true;
            ApplyTouchCustomLayout();
            _gameState = GameState::Pause;
        }
        else if (r == 2)  // Back
        {
            _gameState = GameState::Pause;
        }
        else if (r == 3)  // Default
        {
            // Restore everything to the original baked values captured on first open
            if (_touchDefaults.captured)
            {
                _hudCfg.touchAtkPadR    = _touchDefaults.atkPadR;
                _hudCfg.touchAtkPadB    = _touchDefaults.atkPadB;
                _hudCfg.touchDashOffset = _touchDefaults.dashOffset;
                _hudCfg.touchDashBotPad = _touchDefaults.dashBotPad;
                for (int s = 0; s < 4; s++) _touchSlotOffset[s] = _touchDefaults.slotOffset[s];
                for (int i = 0; i < 6; i++) _touchMappingPos[i] = _touchDefaults.pos[i];
            }
            _touchLayoutCustom   = false;
            _touchMappingDragIdx = -1;
        }
        break;
    }

    case GameState::LevelUpChoice:
    {
        ClearBackground(Color{ 8, 6, 10, 255 });
        float scaleX = (float)kVirtualWidth  / (RoomLayout::kCols * 16.f);
        float scaleY = (float)kVirtualHeight / (RoomLayout::kRows * 16.f);
        if (_tileRenderer.IsLoaded())
            _tileRenderer.DrawRoom(_dungeonRoomLayout, scaleX, scaleY, { 0.f, 0.f });
        DrawHUD();
        DrawLevelUpChoice();
        break;
    }

    case GameState::AbilityChoice:
    {
        ClearBackground(Color{ 8, 6, 10, 255 });
        float scaleX = (float)kVirtualWidth  / (RoomLayout::kCols * 16.f);
        float scaleY = (float)kVirtualHeight / (RoomLayout::kRows * 16.f);
        if (_tileRenderer.IsLoaded())
            _tileRenderer.DrawRoom(_dungeonRoomLayout, scaleX, scaleY, { 0.f, 0.f });
        DrawHUD();
        DrawAbilityChoice();
        break;
    }

    case GameState::Shop:
    {
        _shop.Draw(_player, _debug.IsActive());
        break;
    }

    default: break;
    }

    DrawScreenFx();   // crit spotlight + level-up/ultimate flash, over the whole canvas

    // Playtest overlay: a clear way back to the map editor (input handled in Update).
    if (_editorPlaytestActive)
    {
        const Rectangle backBtn{ (float)kVirtualWidth * 0.5f - 130.f, 12.f, 260.f, 42.f };
        const bool hov = CheckCollisionPointRec(GetVirtualMousePos(), backBtn);
        DrawRectangleRounded(backBtn, 0.3f, 6, hov ? Color{60,120,190,235} : Color{28,40,70,225});
        DrawRectangleRoundedLines(backBtn, 0.3f, 6, hov ? WHITE : Fade(SKYBLUE, .7f));
        const char* label = "< Back to Editor  (F1)";
        DrawText(label, (int)(backBtn.x + backBtn.width * 0.5f - MeasureText(label, 22) * 0.5f),
                 (int)(backBtn.y + 10.f), 22, RAYWHITE);

        // Live enemy/door toggle.
        const Rectangle enemyBtn{ backBtn.x + backBtn.width + 14.f, 12.f, 230.f, 42.f };
        const bool ehov = CheckCollisionPointRec(GetVirtualMousePos(), enemyBtn);
        const bool on = _editorPlaytestEnemiesOn;
        DrawRectangleRounded(enemyBtn, 0.3f, 6,
            on ? (ehov ? Color{190,90,70,235} : Color{120,45,40,225})
               : (ehov ? Color{70,150,90,235} : Color{35,90,55,225}));
        DrawRectangleRoundedLines(enemyBtn, 0.3f, 6, ehov ? WHITE : Fade(WHITE, .55f));
        const char* elabel = on ? "Enemies: ON  (F2)" : "Enemies: OFF  (F2)";
        DrawText(elabel, (int)(enemyBtn.x + enemyBtn.width * 0.5f - MeasureText(elabel, 20) * 0.5f),
                 (int)(enemyBtn.y + 11.f), 20, RAYWHITE);
    }
}

void Engine::HandleCollisions()
{
    float mapW = _map.width  * _mapScale;
    float mapH = _map.height * _mapScale;

    // Per-side player boundaries
    const float marginLeft   = 76.f;
    const float marginRight  = 96.f;
    const float marginTop    = 42.f;   // let the player go a little higher
    const float marginBottom = ((_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle)) ? 160.f : 220.f;

    Vector2 pos = _player.GetWorldPos();
    if (pos.x < marginLeft  || pos.x > mapW - marginRight
     || pos.y < marginTop   || pos.y > mapH - marginBottom)
    {
        if (_player.IsBeingForcedPushed())
            _player.OnForcedPushCollision();

        // Always clamp - stops forced pushes and normal movement at the boundary
        pos.x = std::max(marginLeft,        std::min(pos.x, mapW - marginRight));
        pos.y = std::max(marginTop,         std::min(pos.y, mapH - marginBottom));
        _player.SetWorldPos(pos);
    }

    for (auto& prop : _props)
    {
        Vector2 mtv{};
        if (CheckCapsuleRect(_player.GetCapsule(), prop.GetCollisionRec(), mtv))
        {
            if (_player.IsBeingForcedPushed())
                _player.OnForcedPushCollision();

            Vector2 ppos = _player.GetWorldPos();
            _player.SetWorldPos({ ppos.x + mtv.x, ppos.y + mtv.y });
        }

        for (auto& enemy : _enemies)
        {
            if (!enemy->IsActive() || enemy->IsPitFalling())
                continue;
            if (enemy->IgnoresPropCollisions())
                continue;
            if (CheckCollisionCircleRec(prop.GetEnemyCollisionCenter(), prop.GetEnemyCollisionRadius(), enemy->GetCollisionRec()))
            {
                if (enemy->IsBeingForcedPushed())
                {
                    enemy->OnForcedPushCollision();
                    continue;
                }

                if (Ogre* ogre = enemy->AsOgre())
                {
                    if (ogre->IsRushing())
                        ogre->OnRushBlocked();
                }
                else if (Molarbeast* molarbeast = enemy->AsMolarbeast())
                {
                    if (molarbeast->IsDashing())
                        molarbeast->OnDashBlocked();
                }

                // Eject the enemy via capsule MTV
                Vector2 eMtv{};
                if (CheckCapsuleRect(enemy->GetCapsule(), prop.GetCollisionRec(), eMtv))
                {
                    Vector2 epos = enemy->GetWorldPos();
                    enemy->Teleport({ epos.x + eMtv.x, epos.y + eMtv.y });
                }
                else
                {
                    enemy->UndoMovement();
                }
            }
        }

    }

      // Enemies should obey the same arena rectangle as the player. Using the
      // shared per-side margins keeps all actors inside the intended playable
      // space instead of letting special enemies, like the ogre, rush beyond
      // the lower boundary.
      for (auto& enemy : _enemies)
      {
          if (!enemy->IsActive() || enemy->IsPitFalling())
              continue;
          if (enemy->IsDying()) continue;
          Vector2 pos = enemy->GetWorldPos();
          if (pos.x < marginLeft  || pos.x > mapW - marginRight ||
              pos.y < marginTop   || pos.y > mapH - marginBottom)
          {
              if (enemy->IsBeingForcedPushed())
                  enemy->OnForcedPushCollision();
              else if (Ogre* ogre = enemy->AsOgre())
              {
                  if (ogre->IsRushing())
                      ogre->OnRushBlocked();
                  else
                      enemy->UndoMovement();
              }
              else if (Molarbeast* molarbeast = enemy->AsMolarbeast())
              {
                  if (molarbeast->IsDashing())
                      molarbeast->OnDashBlocked();
                  else
                      enemy->UndoMovement();
              }
              else
              {
                  enemy->UndoMovement();
              }
        }
    }


    // Player vs enemy solid collision - dash passes straight through.
    // After the dash ends, eject the player if they landed inside an enemy.
    if (!_player.IsDashing())
    {
        for (auto& enemy : _enemies)
        {
            if (!enemy->IsActive() || !enemy->IsAlive() || enemy->IsPitFalling())
                continue;

            Vector2 peMtv{};
            if (!CheckCapsuleCapsule(_player.GetCapsule(), enemy->GetCapsule(), peMtv))
                continue;

            if (_player.IsBeingForcedPushed())
                continue;

            // Apply MTV directly without undoing movement - lateral motion is preserved
            // giving a natural slide around enemy capsules instead of a hard stop.
            Vector2 pos = _player.GetWorldPos();
            _player.SetWorldPos({ pos.x + peMtv.x, pos.y + peMtv.y });
        }
    }
}

void Engine::UpdateEnemyCount(float dt)
{
    EnemyDeathContext ctx{};
    ctx.enemies = &_enemies;
    ctx.bossCyclopsSupport = &_bossCyclopsSupport;
    ctx.bossOgreSupport = &_bossOgreSupport;
    ctx.wave = _wave;
    ctx.enemiesKilled = &_enemiesKilled;
    ctx.bossesDefeated = &_bossesDefeated;
    ctx.demoCompleted = &_demoCompleted;
    ctx.pendingExp = &_pendingExp;
    ctx.awardKillExp = (_gameState != GameState::DungeonRun);
    ctx.spawnEnemyDrop = [&](Vector2 worldPos, bool isOgre, bool isBoss) {
        if (!_prologueActive) SpawnEnemyDrop(worldPos, isOgre, isBoss);
    };
    ctx.spawnSmallSlime = [&](Vector2 pos) { SpawnSlime(pos, SlimeSize::Small); };
    ctx.spawnPoisonCloud = [&](Vector2 pos) { SpawnPoisonCloud(pos, Sporeling::kPoisonCloudRadius); };
    ctx.onEnemyKilled = [&](Vector2 pos, bool burning, bool frozen, bool charged, bool eliteOrBoss) {
        if (_prologueActive) return;
        _player.OnEnemyKilled(eliteOrBoss);
        ApplyRelicOnKill(pos, burning, frozen, charged, eliteOrBoss);
        // Volatile room affix — the corpse erupts into a lingering toxic cloud.
        if (GetRoomAffixDef(_currentRoomAffix).volatileDeath)
            SpawnPoisonCloud(pos, Sporeling::kPoisonCloudRadius);
        // Elites and bosses owe a relic — deferred to a 3-card pick once the room
        // is clear (see the trigger in the gameplay update).
        if (eliteOrBoss)
            _pendingRelicChoices++;
    };
    _combatDirector.UpdateEnemyDeaths(ctx, dt);
}

void Engine::TriggerScreenShake(float strength, float duration)
{
    _shakeStrength = strength * _juiceShakeMult;   // global multiplier (debug juice panel)
    _shakeTimer = duration;
}

void Engine::DrawHUD(bool villageMode)
{
    {
    const HUDConfig& hc = _hudCfg;

    auto drawLabelBox = [&](const char* text, float x, float y, int fontSize, Color textColor)
    {
        int textW = MeasureText(text, fontSize);
        const float padX = 12.f;
        const float padY = 8.f;
        DrawRectangleRounded(
            Rectangle{ x - padX, y - padY, textW + padX * 2.f, fontSize + padY * 2.f },
            0.18f, 6, Fade(BLACK, 0.55f));
        DrawText(text, (int)x, (int)y, fontSize, textColor);
    };

    // Counters tick smoothly toward the real value (satisfying loot feel).
    float ctrK = std::min(1.f, GetFrameTime() * 10.f);
    _displayGold  += ((float)_player.GetGold()  - _displayGold)  * ctrK;
    _displayCells += ((float)_player.GetCells() - _displayCells) * ctrK;
    if (fabsf(_displayGold  - _player.GetGold())  < 0.6f) _displayGold  = (float)_player.GetGold();
    if (fabsf(_displayCells - _player.GetCells()) < 0.6f) _displayCells = (float)_player.GetCells();

    drawLabelBox(("Gold: " + std::to_string((int)(_displayGold + 0.5f))).c_str(),
        hc.goldX, hc.goldY, (int)hc.goldFs, GOLD);
    if (!villageMode && _currentRoomType != RoomType::Store)
        drawLabelBox(("Enemies Left: " + std::to_string(GetActiveEnemyCount())).c_str(),
            hc.enemiesX, hc.enemiesY, (int)hc.enemiesFs, RAYWHITE);

    // Carried Echoes - pink to match the pickup orbs; lost on death.
    float echoesY = villageMode ? hc.enemiesY : hc.enemiesY + hc.enemiesFs + 24.f;
    drawLabelBox(("Echoes: " + std::to_string((int)(_displayCells + 0.5f))).c_str(),
        hc.goldX, echoesY, (int)hc.goldFs, Color{ 255, 120, 210, 255 });

    // "+N Echoes banked" toast after reaching Zeph with carried cells.
    if (_cellsBankedToastTimer > 0.f)
    {
        _cellsBankedToastTimer -= GetFrameTime();
        float toastAlpha = std::min(1.f, _cellsBankedToastTimer / 0.75f);
        const char* toastText = TextFormat("+%d Echoes banked with Zeph", _cellsBankedToastAmount);
        int toastFontSize = 40;
        int toastWidth = MeasureText(toastText, toastFontSize);
        float toastX = (float)kVirtualWidth * 0.5f - toastWidth * 0.5f;
        float toastY = 150.f;
        DrawRectangleRounded(
            Rectangle{ toastX - 18.f, toastY - 10.f, toastWidth + 36.f, toastFontSize + 20.f },
            0.3f, 6, Fade(BLACK, 0.55f * toastAlpha));
        DrawText(toastText, (int)toastX, (int)toastY, toastFontSize,
            Fade(Color{ 255, 120, 210, 255 }, toastAlpha));
    }

    // "CONTRACT HONORED +N gold +N XP" toast when a Risk Shrine contract resolves.
    if (_contractToastTimer > 0.f)
    {
        _contractToastTimer -= GetFrameTime();
        float toastAlpha = std::min(1.f, _contractToastTimer / 0.75f);
        int toastFontSize = 38;
        int toastWidth = MeasureText(_contractToastText.c_str(), toastFontSize);
        float toastX = (float)kVirtualWidth * 0.5f - toastWidth * 0.5f;
        float toastY = 200.f;
        DrawRectangleRounded(
            Rectangle{ toastX - 18.f, toastY - 10.f, toastWidth + 36.f, toastFontSize + 20.f },
            0.3f, 6, Fade(BLACK, 0.55f * toastAlpha));
        DrawText(_contractToastText.c_str(), (int)toastX, (int)toastY, toastFontSize,
            Fade(Color{ 255, 200, 110, 255 }, toastAlpha));
    }

    // Owned relics strip + "New Relic!" toast.
    DrawOwnedRelics();
    if (_relicToastTimer > 0.f)
    {
        _relicToastTimer -= GetFrameTime();
        float toastAlpha = std::min(1.f, _relicToastTimer / 0.75f);
        const char* toastText = TextFormat("New Relic:  %s", _relicToastName.c_str());
        int toastFontSize = 44;
        int toastWidth = MeasureText(toastText, toastFontSize);
        float toastX = (float)kVirtualWidth * 0.5f - toastWidth * 0.5f;
        float toastY = 220.f;
        DrawRectangleRounded(
            Rectangle{ toastX - 20.f, toastY - 12.f, toastWidth + 40.f, toastFontSize + 24.f },
            0.3f, 6, Fade(BLACK, 0.6f * toastAlpha));
        DrawText(toastText, (int)toastX, (int)toastY, toastFontSize,
            Fade(Color{ 255, 220, 120, 255 }, toastAlpha));
    }


    auto drawOrb = [&](Vector2 centre, float radius, float pct, Color fill, const char* label)
    {
        pct = std::clamp(pct, 0.f, 1.f);
        DrawCircleV(centre, radius + 8.f, Fade(BLACK, 0.55f));
        DrawCircleV(centre, radius, Fade(BLACK, 0.82f));
        float fillRadius = radius - 4.f;
        float fillH = fillRadius * 2.f * pct;
        BeginScissorMode(
            (int)(centre.x - fillRadius),
            (int)(centre.y + fillRadius - fillH),
            (int)(fillRadius * 2.f),
            (int)std::ceil(fillH));
        DrawCircleV(centre, fillRadius, fill);
        EndScissorMode();
        DrawCircleLinesV(centre, radius, Fade(WHITE, 0.45f));
        DrawCircleLinesV(centre, radius - 7.f, Fade(WHITE, 0.38f));

        int fs = (int)hc.barLabelFs + 4;
        int labelW = MeasureText(label, fs);
        DrawText(label, (int)(centre.x - labelW * 0.5f), (int)(centre.y - fs * 0.5f), fs, RAYWHITE);
    };

    {        float orbR = std::max(24.f, hc.statOrbR);

        Vector2 hpOrbCentre{ hc.hpOrbX, hc.hpOrbY };
        Vector2 mpOrbCentre{ hc.mpOrbX, hc.mpOrbY };
        if (_touchModeActive)
        {
            orbR *= 1.25f;
            const float centreX = kVirtualWidth * 0.5f;
            const float centreGap = orbR * 2.f + 48.f;
            const float maxY = kVirtualHeight - orbR - 10.f;
            hpOrbCentre.x = centreX - centreGap * 0.5f;
            mpOrbCentre.x = centreX + centreGap * 0.5f;
            hpOrbCentre.y = std::min(hpOrbCentre.y, maxY);
            mpOrbCentre.y = std::min(mpOrbCentre.y, maxY);
        }

        float maxHp = _player.GetMaxHealthValue();
        float curHp = _player.GetHealthValue();
        float hpPct = (maxHp > 0.f) ? (curHp / maxHp) : 0.f;
        Color hpFill = (hpPct > 0.30f) ? Color{190, 25, 30, 235} : Color{255, 45, 40, 245};
        if (hpPct <= 0.30f)
        {
            float pulse = (sinf((float)GetTime() * (2.f * PI / 3.f)) + 1.f) * 0.5f;
            DrawCircleV(hpOrbCentre, orbR + 16.f, Fade(RED, 0.22f * pulse));
        }
        // Orb fill slides toward the true value instead of snapping.
        float orbK = std::min(1.f, GetFrameTime() * 8.f);
        _displayHpPct += (hpPct - _displayHpPct) * orbK;
        drawOrb(hpOrbCentre, orbR, _displayHpPct,
            hpFill, TextFormat("HP %.0f", curHp));

        int curMana = _player.GetMana();
        int maxMana = _player.GetMaxMana();
        float manaPct = (maxMana > 0) ? (float)curMana / (float)maxMana : 0.f;
        _displayManaPct += (manaPct - _displayManaPct) * orbK;
        drawOrb(mpOrbCentre, orbR, _displayManaPct,
            Color{245, 205, 45, 235}, TextFormat("MP %d", curMana));
    }

    if (_currentRoomType == RoomType::Elite && _eliteMechanic >= 0)
    {
        const char* shortLabel = EliteModifierShortName(_eliteMechanic);
        int mw = MeasureText(shortLabel, 20);
        drawLabelBox(shortLabel, (float)(kVirtualWidth - mw - (int)hc.actOffsetX), 58.f, 20,
                     GetEliteModifierColor(_eliteMechanic));
    }

    // Ascension badge (top-right, only on a modified difficulty run).
    if (_ascensionTier > 0)
    {
        const char* asc = TextFormat("Ascension %d", _ascensionTier);
        int aw = MeasureText(asc, 22);
        drawLabelBox(asc, (float)(kVirtualWidth - aw - (int)hc.actOffsetX), 96.f, 22, Color{ 255, 130, 90, 255 });
    }

    if (_currentRoomType == RoomType::Elite && _eliteEnrageWarningTimer > 0.f)
    {
        const float sw = (float)kVirtualWidth;
        const float sh = (float)kVirtualHeight;
        float alpha = 1.f;
        if (_eliteEnrageWarningTimer > kEliteEnrageWarningDuration - 0.5f)
            alpha = (kEliteEnrageWarningDuration - _eliteEnrageWarningTimer) / 0.5f;
        else if (_eliteEnrageWarningTimer < 0.5f)
            alpha = _eliteEnrageWarningTimer / 0.5f;
        alpha = std::max(0.f, std::min(1.f, alpha));

        const Color modifierColor = GetEliteModifierColor(_eliteMechanic);
        const char* line1 = "ELITE ENCOUNTER";
        const char* line2 = EliteModifierCondition(_eliteMechanic);
        const int sz1 = 48, sz2 = 28;
        const float bannerH = 120.f;
        const float bannerY = sh * 0.38f;
        DrawRectangle(0, (int)bannerY, (int)sw, (int)bannerH, Fade(Color{20,0,0,220}, alpha));
        DrawRectangle(0, (int)bannerY, (int)sw, 3, Fade(modifierColor, alpha));
        DrawRectangle(0, (int)(bannerY + bannerH - 3), (int)sw, 3, Fade(modifierColor, alpha));
        DrawText(line1, (int)(sw/2.f - MeasureText(line1,sz1)/2.f), (int)(bannerY+14.f), sz1, Fade(modifierColor,alpha));
        DrawText(line2, (int)(sw/2.f - MeasureText(line2,sz2)/2.f), (int)(bannerY+14.f+sz1+8.f), sz2, Fade(Color{255,220,150,255},alpha));
    }

    if (_debug.IsActive())
        DrawDebugToggleTab();

    if (_touchModeActive)
    {
        DrawTouchAbilityArc();
        _touch.Draw(kVirtualWidth, kVirtualHeight);

        // Pause button
        Rectangle pauseRec{ (float)_windowWidth - hc.touchPauseW - hc.touchPausePad,
                             hc.touchPausePad, hc.touchPauseW, hc.touchPauseH };
        DrawRectangleRounded(pauseRec, 0.22f, 6, Fade(BLACK, 0.55f));
        DrawRectangleRoundedLines(pauseRec, 0.22f, 6, Fade(WHITE, 0.40f));
        int pw = MeasureText("II", 26);
        DrawText("II",
            (int)(pauseRec.x + pauseRec.width / 2.f - pw / 2.f),
            (int)(pauseRec.y + pauseRec.height / 2.f - 13.f),
            26, RAYWHITE);
    }
    else
    {
        DrawAbilityBar();
    }

    // -- HUD debug editor --------------------------------------------------
#if MO_DEV_TOOLS
    if (IsKeyPressed(KEY_NINE))
        _hudEditorActive = !_hudEditorActive;
#endif

    if (_hudEditorActive)
    {
        constexpr int kN = 54;
        const char* varNames[kN] = {
            "0  Bar Width",       "1  Bar Height",      "2  Bar Gap",
            "3  Bar Top Pad",     "4  Bar Label Font",
            "5  Gold X",          "6  Gold Y",          "7  Gold Font",
            "8  Enemies X",       "9  Enemies Y",       "10 Enemies Font",
            "11 Act Offset X",    "12 Act Y",            "13 Act Font",
            "14 Mini X",          "15 Mini Y",           "16 Mini Width",
            "17 Dot Boss",        "18 Dot Elite",        "19 Dot Enemy",
            "20 Dot Prop",        "21 Dot Pickup",       "22 Dot Player",
            "23 Slot Size",       "24 Slot Gap",         "25 Slot Bot Pad",
            "26 Key Label Font",  "27 Name Font",
            "28 Joy Radius",      "29 Atk Radius",       "30 Dash Radius",
            "31 Atk Pad Right",   "32 Atk Pad Bot",      "33 Dash Offset",
            "34 Pause Width",     "35 Pause Height",     "36 Pause Pad",
            "37 Atk Label Font",  "38 Dash Label Font",
            "39 Touch Slot Size", "40 Touch Slot Gap",
            "41 Slot Right Pad",  "42 Slot Y Offset",
            "43 EXP Bar Height",  "44 EXP Bar Gap",      "45 EXP Label Font",
            "46 HP Orb X",        "47 HP Orb Y",         "48 MP Orb X",
            "49 MP Orb Y",        "50 HP/MP Orb Size",
            "51 Armour X",        "52 Armour Y",        "53 Armour Size",
        };
        float* vars[kN] = {
            &_hudCfg.barW,        &_hudCfg.barH,        &_hudCfg.barGap,
            &_hudCfg.barTopPad,   &_hudCfg.barLabelFs,
            &_hudCfg.goldX,       &_hudCfg.goldY,       &_hudCfg.goldFs,
            &_hudCfg.enemiesX,    &_hudCfg.enemiesY,    &_hudCfg.enemiesFs,
            &_hudCfg.actOffsetX,  &_hudCfg.actY,        &_hudCfg.actFs,
            &_hudCfg.miniX,       &_hudCfg.miniY,       &_hudCfg.miniW,
            &_hudCfg.miniDotBoss, &_hudCfg.miniDotElite,&_hudCfg.miniDotEnemy,
            &_hudCfg.miniDotProp, &_hudCfg.miniDotPickup,&_hudCfg.miniDotPlayer,
            &_hudCfg.slotSz,      &_hudCfg.slotGap,     &_hudCfg.slotBotPad,
            &_hudCfg.slotKeyFs,   &_hudCfg.slotNameFs,
            &_hudCfg.touchJoyR,   &_hudCfg.touchAtkR,   &_hudCfg.touchDashR,
            &_hudCfg.touchAtkPadR,&_hudCfg.touchAtkPadB,&_hudCfg.touchDashOffset,
            &_hudCfg.touchPauseW, &_hudCfg.touchPauseH, &_hudCfg.touchPausePad,
            &_hudCfg.touchAtkFs,  &_hudCfg.touchDashFs,
            &_hudCfg.touchSlotSz, &_hudCfg.touchSlotGap,
            &_hudCfg.touchSlotRightPad, &_hudCfg.touchSlotYOff,
            &_hudCfg.expBarH, &_hudCfg.expBarGap, &_hudCfg.expLabelFs,
            &_hudCfg.hpOrbX, &_hudCfg.hpOrbY, &_hudCfg.mpOrbX,
            &_hudCfg.mpOrbY, &_hudCfg.statOrbR,
            &_hudCfg.armourX, &_hudCfg.armourY, &_hudCfg.armourSize,
        };

        if (IsKeyPressed(KEY_UP))
            _hudEditorSelIdx = (_hudEditorSelIdx - 1 + kN) % kN;
        if (IsKeyPressed(KEY_DOWN))
            _hudEditorSelIdx = (_hudEditorSelIdx + 1) % kN;
        if (IsKeyDown(KEY_RIGHT)) *vars[_hudEditorSelIdx] += 1.f;
        if (IsKeyDown(KEY_LEFT))  *vars[_hudEditorSelIdx] -= 1.f;

        if (IsKeyPressed(KEY_S))
        {
            TraceLog(LOG_INFO, "=== HUD Editor Export ===");
            TraceLog(LOG_INFO, "barW=%g barH=%g barGap=%g barTopPad=%g barLabelFs=%g",
                hc.barW, hc.barH, hc.barGap, hc.barTopPad, hc.barLabelFs);
            TraceLog(LOG_INFO, "goldX=%g goldY=%g goldFs=%g", hc.goldX, hc.goldY, hc.goldFs);
            TraceLog(LOG_INFO, "enemiesX=%g enemiesY=%g enemiesFs=%g", hc.enemiesX, hc.enemiesY, hc.enemiesFs);
            TraceLog(LOG_INFO, "actOffsetX=%g actY=%g actFs=%g", hc.actOffsetX, hc.actY, hc.actFs);
            TraceLog(LOG_INFO, "miniX=%g miniY=%g miniW=%g", hc.miniX, hc.miniY, hc.miniW);
            TraceLog(LOG_INFO, "miniDots: boss=%g elite=%g enemy=%g prop=%g pickup=%g player=%g",
                hc.miniDotBoss, hc.miniDotElite, hc.miniDotEnemy, hc.miniDotProp, hc.miniDotPickup, hc.miniDotPlayer);
            TraceLog(LOG_INFO, "slotSz=%g slotGap=%g slotBotPad=%g slotKeyFs=%g slotNameFs=%g",
                hc.slotSz, hc.slotGap, hc.slotBotPad, hc.slotKeyFs, hc.slotNameFs);
            TraceLog(LOG_INFO, "touchJoyR=%g atkR=%g dashR=%g atkPadR=%g atkPadB=%g dashOff=%g",
                hc.touchJoyR, hc.touchAtkR, hc.touchDashR, hc.touchAtkPadR, hc.touchAtkPadB, hc.touchDashOffset);
            TraceLog(LOG_INFO, "pauseW=%g pauseH=%g pausePad=%g atkFs=%g dashFs=%g",
                hc.touchPauseW, hc.touchPauseH, hc.touchPausePad, hc.touchAtkFs, hc.touchDashFs);
            TraceLog(LOG_INFO, "touchSlotSz=%g touchSlotGap=%g touchSlotRightPad=%g touchSlotYOff=%g",
                hc.touchSlotSz, hc.touchSlotGap, hc.touchSlotRightPad, hc.touchSlotYOff);
            TraceLog(LOG_INFO, "expBarH=%g expBarGap=%g expLabelFs=%g",
                hc.expBarH, hc.expBarGap, hc.expLabelFs);
            TraceLog(LOG_INFO, "hpOrbX=%g hpOrbY=%g mpOrbX=%g mpOrbY=%g statOrbR=%g",
                hc.hpOrbX, hc.hpOrbY, hc.mpOrbX, hc.mpOrbY, hc.statOrbR);
            TraceLog(LOG_INFO, "armourX=%g armourY=%g armourSize=%g",
                hc.armourX, hc.armourY, hc.armourSize);
            TraceLog(LOG_INFO, "touchSlotOffsets: [0]=(%.1f,%.1f) [1]=(%.1f,%.1f) [2]=(%.1f,%.1f) [3]=(%.1f,%.1f)",
                _touchSlotOffset[0].x, _touchSlotOffset[0].y,
                _touchSlotOffset[1].x, _touchSlotOffset[1].y,
                _touchSlotOffset[2].x, _touchSlotOffset[2].y,
                _touchSlotOffset[3].x, _touchSlotOffset[3].y);
        }

        // Two-column panel
        constexpr int   kHalf = (kN + 1) / 2;  // 20
        constexpr float colW  = 310.f;
        constexpr float colGap= 20.f;
        constexpr float rowH  = 30.f;
        const float panW = colW * 2.f + colGap;
        const float panH = 40.f + kHalf * rowH;
        const float sw   = (float)kVirtualWidth;
        const float panX = sw * 0.5f - panW * 0.5f;
        const float panY = 10.f;

        DrawRectangle((int)panX, (int)panY, (int)panW, (int)panH, Fade(BLACK, 0.82f));
        DrawRectangleLines((int)panX, (int)panY, (int)panW, (int)panH, DARKGRAY);
        DrawLine((int)(panX+colW+colGap*0.5f),(int)(panY+36.f),
                 (int)(panX+colW+colGap*0.5f),(int)(panY+panH-4.f), DARKGRAY);
        DrawText("HUD EDITOR  [9] close", (int)(panX+8.f),(int)(panY+6.f),11,GRAY);
        DrawText("[UP/DOWN] sel  [L/R] nudge  [S] export  |  drag touch slots with mouse",
            (int)(panX+8.f),(int)(panY+20.f),10,DARKGRAY);

        for (int i = 0; i < kN; i++)
        {
            int   col  = i / kHalf;
            int   row  = i % kHalf;
            float cx   = panX + col * (colW + colGap);
            float ry   = panY + 38.f + row * rowH;
            bool  sel  = (i == _hudEditorSelIdx);
            Color col_c= sel ? YELLOW : WHITE;

            if (sel)
                DrawText("->", (int)(cx+4.f), (int)(ry+rowH*0.5f-7.f), 13, YELLOW);
            DrawText(varNames[i], (int)(cx+26.f), (int)(ry+rowH*0.5f-7.f), 13, col_c);

            const char* valStr = TextFormat("%.1f", *vars[i]);
            int valW = MeasureText(valStr, 13);
            DrawText(valStr, (int)(cx+colW-valW-6.f), (int)(ry+rowH*0.5f-7.f), 13, col_c);
        }
    }

    }
    return;

    // -- Shared bottom-bar layout constants (mirrored in DrawAbilityBar) -------
    static constexpr float kBarW   = 400.f;
    static constexpr float kBarH   = 28.f;
    static constexpr float kBarGap = 8.f;
    static constexpr float kBotPad = 12.f;

    const float expBarY  = (float)kVirtualHeight - kBotPad - kBarH;
    const float manaBarY = expBarY - kBarGap - kBarH;
    const float hpBarY   = manaBarY - kBarGap - kBarH;
    const float barX     = (float)kVirtualWidth / 2.f - kBarW / 2.f;

    // -- Top banner ------------------------------------------------------------
    DrawRectangle(0, 0, kVirtualWidth, kVirtualHeight / 8, Fade(BLACK, 0.6f));

    DrawText(TextFormat("Time: %.1f", _gameTimer), 85 + kVirtualWidth / 2 - 150, 60, 30, RAYWHITE);
    DrawText(("Gold: " + std::to_string(_player.GetGold())).c_str(), 20, 10, 30, GOLD);
    DrawText(("Enemies Left: " + std::to_string(GetActiveEnemyCount())).c_str(), 20, 60, 30, RAYWHITE);

    // -- Wave display - top right ----------------------------------------------
    {
        bool isBoss = (_wave > 0 && _wave % 5 == 0);
        const char* waveLabel = isBoss
            ? TextFormat("Wave %d  - BOSS", _wave)
            : TextFormat("Wave %d", _wave);
        int waveLabelW = MeasureText(waveLabel, 32);
        DrawText(waveLabel, kVirtualWidth - waveLabelW - 20, 20, 32,
            isBoss ? ORANGE : RAYWHITE);
    }

    // -- HP bar (bottom, above EXP) --------------------------------------------
    {
        float maxHp = _player.GetMaxHealthValue();
        float curHp = _player.GetHealthValue();
        float hpPct = (maxHp > 0.f) ? (curHp / maxHp) : 0.f;

        Color fillColor = (hpPct > 0.70f) ? GREEN :
                          (hpPct > 0.30f) ? YELLOW : RED;

        if (hpPct <= 0.30f)
        {
            float pulse    = (sinf((float)GetTime() * (2.f * PI / 3.f)) + 1.f) * 0.5f;
            float exps[]   = { 4.f, 9.f, 15.f };
            float alphas[] = { 0.45f, 0.28f, 0.13f };
            for (int i = 0; i < 3; i++)
            {
                float e = exps[i];
                DrawRectangleRounded(
                    { barX - e, hpBarY - e, kBarW + e * 2.f, kBarH + e * 2.f },
                    0.35f, 6, Fade(RED, alphas[i] * pulse));
            }
        }

        DrawRectangleRounded({ barX, hpBarY, kBarW, kBarH }, 0.3f, 6, Fade(BLACK, 0.75f));
        DrawRectangleRounded({ barX, hpBarY, kBarW * hpPct, kBarH }, 0.3f, 6, fillColor);
        DrawRectangleRoundedLines({ barX, hpBarY, kBarW, kBarH }, 0.3f, 6, Fade(WHITE, 0.25f));

        const char* hpLabel = TextFormat("HP  %.0f / %.0f", curHp, maxHp);
        int labelW = MeasureText(hpLabel, 18);
        DrawText(hpLabel,
            (int)(barX + kBarW / 2.f - labelW / 2.f),
            (int)(hpBarY + kBarH / 2.f - 9.f),
            18, BLACK);
    }

    // -- Mana bar (middle) -----------------------------------------------------
    {
        int   curMana  = _player.GetMana();
        int   maxMana  = _player.GetMaxMana();
        float manaPct  = (maxMana > 0) ? (float)curMana / (float)maxMana : 0.f;

        static const Color kManaFill = { 60, 120, 255, 230 };

        DrawRectangleRounded({ barX, manaBarY, kBarW, kBarH }, 0.3f, 6, Fade(BLACK, 0.75f));
        DrawRectangleRounded({ barX, manaBarY, kBarW * manaPct, kBarH }, 0.3f, 6, kManaFill);
        DrawRectangleRoundedLines({ barX, manaBarY, kBarW, kBarH }, 0.3f, 6, Fade(WHITE, 0.25f));

        const char* manaLabel = TextFormat("MP  %d / %d", curMana, maxMana);
        int manaLabelW = MeasureText(manaLabel, 18);
        DrawText(manaLabel,
            (int)(barX + kBarW / 2.f - manaLabelW / 2.f),
            (int)(manaBarY + kBarH / 2.f - 9.f),
            18, BLACK);
    }

    // -- EXP bar (very bottom) -------------------------------------------------
    {
        int   level    = _player.GetLevel();
        int   exp      = _player.GetExp();
        int   expToNext= _player.GetExpToNext();
        int   maxLevel = _player.GetMaxLevel();
        float expPct   = (level < maxLevel && expToNext > 0) ? (float)exp / (float)expToNext : 1.f;

        static const Color kExpFill = { 255, 210, 0, 230 };

        DrawRectangleRounded({ barX, expBarY, kBarW, kBarH }, 0.3f, 6, Fade(BLACK, 0.75f));
        if (level < maxLevel)
            DrawRectangleRounded({ barX, expBarY, kBarW * expPct, kBarH }, 0.3f, 6, kExpFill);
        DrawRectangleRoundedLines({ barX, expBarY, kBarW, kBarH }, 0.3f, 6, Fade(WHITE, 0.25f));

        const char* levelText = (level < maxLevel)
            ? TextFormat("Lv.%d  %d/%d EXP", level, exp, expToNext)
            : "Lv.MAX";
        int textW = MeasureText(levelText, 18);
        DrawText(levelText,
            (int)(barX + kBarW / 2.f - textW / 2.f),
            (int)(expBarY + kBarH / 2.f - 9.f),
            18, kExpFill);
    }

    DrawAbilityBar();

    if (_bossWarningTimer > 0.f)
    {
        const char* warning = "DON'T GET TOO CLOSE";
        int warningSize = 34;
        int warningWidth = MeasureText(warning, warningSize);
        DrawText(warning,
            kVirtualWidth / 2 - warningWidth / 2,
            96,
            warningSize,
            ORANGE);
    }
}

void Engine::DrawAbilityBar()
{
    {
    const HUDConfig& hc = _hudCfg;
    const int   totalSlots = _player.GetMaxAbilitySlots();
    const float slotSize   = hc.slotSz;
    const float slotGap    = hc.slotGap;
    const float slotY      = (float)kVirtualHeight - hc.slotBotPad - slotSize;
    const float totalW     = totalSlots * slotSize + (totalSlots - 1) * slotGap;
    const float startX     = kVirtualWidth / 2.f - totalW / 2.f;
    const int   keyFs      = (int)hc.slotKeyFs;
    const int   nameFs     = (int)hc.slotNameFs;


    {
        int curExp = _player.GetExp();
        int maxExp = _player.GetExpToNext();
        float expPct = (maxExp > 0) ? std::min((float)curExp / (float)maxExp, 1.f) : 0.f;
        const float expBarW = totalW;
        const float expBarX = startX;
        const float expBarY = slotY - hc.expBarGap - hc.expBarH;
        static const Color kExpFill = { 160, 60, 255, 220 };

        DrawRectangleRounded({ expBarX, expBarY, expBarW, hc.expBarH }, 0.3f, 4, Fade(BLACK, 0.65f));
        DrawRectangleRounded({ expBarX, expBarY, expBarW * expPct, hc.expBarH }, 0.3f, 4, kExpFill);
        DrawRectangleRoundedLines({ expBarX, expBarY, expBarW, hc.expBarH }, 0.3f, 4, Fade(WHITE, 0.22f));

        int eFs = (int)hc.expLabelFs;
        const char* expLabel = (_player.GetLevel() >= _player.GetMaxLevel())
            ? TextFormat("LVL MAX")
            : TextFormat("LVL %d   EXP  %d / %d", _player.GetLevel(), curExp, maxExp);
        int eLW = MeasureText(expLabel, eFs);
        DrawText(expLabel,
            (int)(expBarX + expBarW / 2.f - eLW / 2.f),
            (int)(expBarY + hc.expBarH / 2.f - eFs / 2.f),
            eFs, WHITE);

        const int armour = _player.GetArmour();
        const int maxArmour = _player.GetMaxArmour();
        const float armourIconH = std::max(1.f, hc.armourSize);
        const float armourScale = (_upgradeDefenseTex.height > 0)
            ? armourIconH / (float)_upgradeDefenseTex.height : 1.f;
        const float armourIconW = (_upgradeDefenseTex.width > 0)
            ? _upgradeDefenseTex.width * armourScale : armourIconH;
        const float armourGap = armourIconH * 0.35f;
        const float armourRight = maxArmour > 0
            ? hc.armourX + maxArmour * armourIconW + (maxArmour - 1) * armourGap
            : hc.armourX;

        if (_upgradeDefenseTex.id != 0)
        {
            float iconX = hc.armourX;
            const float iconY = hc.armourY;

            for (int i = 0; i < maxArmour; i++)
            {
                Color tint = (i < armour) ? WHITE : Fade(WHITE, 0.38f);
                DrawTextureEx(_upgradeDefenseTex, { iconX, iconY }, 0.f, armourScale, tint);
                iconX += armourIconW + armourGap;
            }
        }

        bool showMechanic = true;
        float progress = 0.f;
        Color classColor = WHITE;
        const char* mechanicLabel = "";
        bool primed = false;

        switch (_player.GetClass())
        {
        case PlayerClass::Warrior:
            progress = std::clamp(_player.GetRagePercent(), 0.f, 1.f);
            classColor = Color{ 255, 120, 30, 255 };
            mechanicLabel = TextFormat("RAGE  %d%%", (int)std::round(progress * 100.f));
            primed = progress >= 1.f;
            break;
        case PlayerClass::Rogue:
        {
            const int current = _player.GetComboPoints();
            const int maximum = std::max(1, _player.GetMaxComboPoints());
            progress = (float)current / (float)maximum;
            classColor = Color{ 235, 65, 85, 255 };
            mechanicLabel = TextFormat("COMBO  %d / %d", current, maximum);
            primed = current >= maximum;
            break;
        }
        case PlayerClass::Hunter:
        {
            const int required = std::max(1, _player.GetHunterMarkEvery());
            const int current = std::clamp(_hunterShotsSinceMark, 0, required);
            progress = (float)current / (float)required;
            classColor = Color{ 95, 210, 105, 255 };
            mechanicLabel = TextFormat("MARK  %d / %d", current, required);
            primed = current == required - 1;
            break;
        }
        case PlayerClass::Paladin:
            progress = std::clamp(_player.GetFaithPercent(), 0.f, 1.f);
            classColor = Color{ 240, 205, 85, 255 };
            mechanicLabel = TextFormat("FAITH  %d%%", (int)std::round(progress * 100.f));
            primed = progress >= 1.f;
            break;
        default:
            showMechanic = false;
            break;
        }

        const float mechanicH = std::max(28.f, armourIconH);
        const float mechanicY = expBarY - mechanicH - 10.f;
        const float mechanicX = armourRight + 18.f;
        const float mechanicRight = expBarX + expBarW;
        const float mechanicW = mechanicRight - mechanicX;
        if (showMechanic && mechanicW >= 180.f)
        {
            Color pulseColor = classColor;
            if (primed)
            {
                const float pulse = sinf((float)GetTime() * 8.f) * 0.5f + 0.5f;
                pulseColor = ColorLerp(classColor, WHITE, pulse * 0.35f);
            }

            Rectangle mechanicRect{ mechanicX, mechanicY, mechanicW, mechanicH };
            DrawRectangleRounded(mechanicRect, 0.22f, 6, Color{ 8, 10, 14, 235 });
            if (progress > 0.f)
                DrawRectangleRounded({ mechanicX, mechanicY, mechanicW * progress, mechanicH },
                                     0.22f, 6, Fade(pulseColor, 0.88f));
            DrawRectangleRoundedLines(mechanicRect, 0.22f, 6, pulseColor);

            const int mechanicFs = 18;
            const int labelW = MeasureText(mechanicLabel, mechanicFs);
            const int labelX = (int)(mechanicX + mechanicW * 0.5f - labelW * 0.5f);
            const int labelY = (int)(mechanicY + mechanicH * 0.5f - mechanicFs * 0.5f);
            DrawText(mechanicLabel, labelX - 2, labelY, mechanicFs, BLACK);
            DrawText(mechanicLabel, labelX + 2, labelY, mechanicFs, BLACK);
            DrawText(mechanicLabel, labelX, labelY - 2, mechanicFs, BLACK);
            DrawText(mechanicLabel, labelX, labelY + 2, mechanicFs, BLACK);
            DrawText(mechanicLabel, labelX, labelY, mechanicFs, WHITE);
        }
    }
    Vector2 mouse = GetVirtualMousePos();

    for (int i = 0; i < totalSlots; i++)
    {
        AbilityType ability = _player.GetLearnedAbility(i);
        bool isEmpty = (ability == AbilityType::None);
        bool onCooldown = !isEmpty && _player.IsSlotOnCooldown(i);
        bool canCast = !isEmpty && !onCooldown
                    && _player.GetMana() >= _player.GetAbilityCost(ability);   // includes Overload surcharge
        float x = startX + i * (slotSize + slotGap);
        Rectangle slot{ x, slotY, slotSize, slotSize };
        bool hovered = !isEmpty && CheckCollisionPointRec(mouse, slot);

        Color bgColor     = isEmpty ? Fade(BLACK, 0.30f) : (hovered ? Fade(BLACK, 0.80f) : Fade(BLACK, 0.55f));
        Color borderColor = isEmpty ? Fade(WHITE, 0.12f) :
            hovered ? Fade(GOLD, 0.70f) :
            canCast ? Fade(LIGHTGRAY, 0.35f) : Fade(RED, 0.40f);
        DrawRectangleRounded(slot, 0.18f, 6, bgColor);
        DrawRectangleRoundedLines(slot, 0.18f, 6, borderColor);

        if (isEmpty)
        {
            DrawText(PromptAbilitySlot(GetPromptModeForUi(), i),
                (int)(x + 6.f), (int)(slotY + 6.f), keyFs, Fade(WHITE, 0.25f));
            continue;
        }

        DrawText(PromptAbilitySlot(GetPromptModeForUi(), i),
            (int)(x + 6.f), (int)(slotY + 6.f), keyFs, Fade(WHITE, 0.6f));

        if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            BeginAbilityInput(i);
            if (_pendingAbilityAim.active && _pendingAbilityAim.slot == i)
                _pendingAbilityAim.commitOnMouseRelease = true;

        const Texture2D* iconTex = GetAbilityIcon(ability);
        if (!iconTex || iconTex->id == 0) iconTex = &_abilityIconFireTex;

        Color iconTint    = canCast ? WHITE : Fade(WHITE, 0.35f);
        float maxIconSize = slotSize * 0.55f;
        float iconScale   = std::min(maxIconSize / (float)iconTex->width, maxIconSize / (float)iconTex->height);
        float iw = iconTex->width  * iconScale;
        float ih = iconTex->height * iconScale;
        float cx = x + slotSize * 0.5f;
        float cy = slotY + slotSize * 0.42f;
        DrawTextureEx(*iconTex, { cx - iw * 0.5f, cy - ih * 0.5f }, 0.f, iconScale, iconTint);

        // Cooldown sweep: a dark fill that drains downward as the slot recovers,
        // with the seconds remaining in the middle of the slot.
        if (onCooldown)
        {
            float fraction = _player.GetSlotCooldownFraction(i);
            float sweepH   = slotSize * fraction;
            DrawRectangleRec({ x, slotY, slotSize, sweepH }, Fade(BLACK, 0.62f));
            const char* cdText = TextFormat("%.1f", _player.GetSlotCooldownRemaining(i));
            int cdFs = 26;
            DrawText(cdText,
                (int)(cx - MeasureText(cdText, cdFs) * 0.5f),
                (int)(slotY + slotSize * 0.42f - cdFs * 0.5f),
                cdFs, RAYWHITE);
        }

        const char* abilityName = GetAbilityName(ability);
        int nameW = MeasureText(abilityName, nameFs);
        DrawText(abilityName,
            (int)(x + slotSize / 2.f - nameW / 2.f),
            (int)(slotY + slotSize - nameFs - 6.f),
            nameFs, canCast ? RAYWHITE : Fade(GRAY, 0.6f));

        int abilityLv = _player.GetAbilityLevel(ability);
        if (abilityLv > 1)
        {
            const char* badge = TextFormat("Lv%d", abilityLv);
            int badgeW = MeasureText(badge, nameFs);
            Color badgeColor = (abilityLv >= 3) ? GOLD : Fade(SKYBLUE, 0.9f);
            DrawText(badge,
                (int)(x + slotSize - badgeW - 4.f),
                (int)(slotY + slotSize - nameFs - 4.f),
                nameFs, badgeColor);
        }
    }

    }
}

void Engine::DrawWaveIntro()
{
    if (!_waveStarting)
        return;

    DrawRectangle(0, kVirtualHeight / 2 - 80, kVirtualWidth, 160, Fade(BLACK, 0.7f));

    int sw = kVirtualWidth;
    int sh = kVirtualHeight;
    int fontSize = 60;
    int midY     = sh / 2;

    // -- Room type label (center, primary) ------------------------------------
    const char* roomLabel = nullptr;
    Color       roomColor = YELLOW;

    switch (_currentRoomType)
    {
    case RoomType::Boss:
        roomLabel = "- Boss Area -";
        roomColor = RED;
        break;
    case RoomType::Elite:
        roomLabel = "Elite Area";
        roomColor = Color{ 255, 140, 0, 255 };  // orange
        break;
    case RoomType::Rest:
        roomLabel = "Rest Area";
        roomColor = Color{ 100, 230, 120, 255 }; // green
        break;
    case RoomType::Treasure:
        roomLabel = "Treasure Area";
        roomColor = GOLD;
        break;
    case RoomType::Store:
        roomLabel = "Shop Area";
        roomColor = Color{ 220, 200, 60, 255 };
        break;
    default:  // Standard
        roomLabel = "Standard Area";
        roomColor = YELLOW;
        break;
    }

    // Very first room of the run - just show the biome name as the intro.
    if (_wave == 1 && _currentAct == 1)
    {
        const char* biomeName = GetBiomeName(_currentBiome);
        int bw = MeasureText(biomeName, fontSize);
        DrawText(biomeName, sw / 2 - bw / 2, midY - 30, fontSize, GOLD);
        return;
    }

    // New act intro (room 1 of any act after act 1)
    bool isFirstRoomOfNewAct = (_currentRoom == 1 && _currentAct > 1);
    int roomLabelY = (isFirstRoomOfNewAct || _currentRoomType == RoomType::Boss) ? midY - 55 : midY - 30;

    // Act label line (shown on new acts and boss rooms)
    if (isFirstRoomOfNewAct)
    {
        std::string actStr = "Act " + std::to_string(_currentAct) + " - " + GetBiomeName(GetBiomeForAct(_currentAct));
        int aw = MeasureText(actStr.c_str(), 42);
        DrawText(actStr.c_str(), sw / 2 - aw / 2, roomLabelY - 52, 42, LIGHTGRAY);
    }

    // Room label
    int rw = MeasureText(roomLabel, fontSize);
    DrawText(roomLabel, sw / 2 - rw / 2, roomLabelY, fontSize, roomColor);

    if (_currentRoomType == RoomType::Boss)
    {
        const char* actLine = (_message).c_str();
        int aw = MeasureText(actLine, 36);
        DrawText(actLine, sw / 2 - aw / 2, roomLabelY + fontSize + 10, 36, LIGHTGRAY);
    }
}

void Engine::GenerateStartingAbilityOptions()
{
    // Build the starter pool from the ACTIVE class's non-ultimate abilities that
    // are unlocked (ultimates are earned later, never offered as a starter).
    UpgradeType candidates[kAllAbilityCount];
    int optionCount = 0;
    for (int i = 0; i < kAllAbilityCount; i++)
    {
        AbilityType ability = kAllAbilities[i];
        if (IsUltimateAbility(ability)) continue;
        if (!_meta.IsAbilityUnlocked(ability) || !_player.ClassAllows(ability)) continue;
        candidates[optionCount++] = LearnTypeForAbility(ability);
    }

    if (optionCount == 0)
    {
        // Safety net: no valid starter for this class — skip the picker entirely.
        for (int i = 0; i < 6; i++) _startingAbilitySelected[i] = true;
        _startingAbilityPickCount = 0;
        return;
    }

    if (optionCount > 6) optionCount = 6;   // _startingAbilityOptions holds 6
    for (int i = 0; i < optionCount; i++)
        _startingAbilityOptions[i] = candidates[i];

    // Fill the remainder by repeating from the unlocked set so all 6 slots stay
    // valid (only the first 3 are ever displayed).
    for (int i = optionCount; i < 6; i++)
        _startingAbilityOptions[i] = _startingAbilityOptions[i % optionCount];

    for (int i = 0; i < optionCount; i++)
    {
        int j = GetRandomValue(i, optionCount - 1);
        UpgradeType tmp = _startingAbilityOptions[i];
        _startingAbilityOptions[i] = _startingAbilityOptions[j];
        _startingAbilityOptions[j] = tmp;
    }
    for (int i = 0; i < 6; i++)
        _startingAbilitySelected[i] = false;
    _startingAbilityPickCount = 0;
}

void Engine::GenerateLevelUpOptions(LevelUpOfferContext context)
{
    _levelUpOfferContext = context;

    if (context == LevelUpOfferContext::NormalLevel)
    {
        // Power Choices prioritize the abilities this run is actually using.
        // Up to two cards improve learned abilities; remaining cards are modest
        // defensive/utility options, never a mandatory flat damage increase.
        UpgradeType abilityPool[kAllAbilityCount];
        int abilityCount = 0;
        for (int i = 0; i < kAllAbilityCount; i++)
        {
            AbilityType ability = kAllAbilities[i];
            if (_player.HasLearnedAbility(ability) && _player.CanUpgradeAbility(ability))
                abilityPool[abilityCount++] = UpgradeTypeForAbility(ability);
        }
        for (int i = 0; i < abilityCount; i++)
        {
            int j = GetRandomValue(i, abilityCount - 1);
            UpgradeType tmp = abilityPool[i]; abilityPool[i] = abilityPool[j]; abilityPool[j] = tmp;
        }

        // Utility pool is class-aware: Attack Range only matters to the three
        // melee classes — Mage / Hunter / Warlock never see it.
        PlayerClass cls = _player.GetClass();
        bool isMelee = (cls == PlayerClass::Warrior || cls == PlayerClass::Paladin
                        || cls == PlayerClass::Rogue);
        UpgradeType utilityPool[6];
        int utilityCount = 0;
        if (isMelee) utilityPool[utilityCount++] = UpgradeType::AttackRange;
        utilityPool[utilityCount++] = UpgradeType::MaxHealth;
        utilityPool[utilityCount++] = UpgradeType::MaxMana;
        utilityPool[utilityCount++] = UpgradeType::Defense;
        utilityPool[utilityCount++] = UpgradeType::MoveSpeed;
        utilityPool[utilityCount++] = UpgradeType::ManaFlow;
        for (int i = 0; i < utilityCount; i++)
        {
            int j = GetRandomValue(i, utilityCount - 1);
            UpgradeType tmp = utilityPool[i]; utilityPool[i] = utilityPool[j]; utilityPool[j] = tmp;
        }

        // Class card pool: 8 build-shaping cards per class, plus the generic
        // build rares (Class Attunement for gain-resource classes, Overload
        // for everyone). These make Power Choices speak the class's language.
        UpgradeType specialPool[10];
        int specialCount = 0;
        switch (cls)
        {
        case PlayerClass::Mage:
            specialPool[specialCount++] = UpgradeType::MagePyromancy;
            specialPool[specialCount++] = UpgradeType::MageInfernalMastery;
            specialPool[specialCount++] = UpgradeType::MageCryomancy;
            specialPool[specialCount++] = UpgradeType::MageGlacialMastery;
            specialPool[specialCount++] = UpgradeType::MageStormAttunement;
            specialPool[specialCount++] = UpgradeType::MageTempestMastery;
            specialPool[specialCount++] = UpgradeType::MageComboResonance;
            specialPool[specialCount++] = UpgradeType::MageArcaneHaste;
            break;
        case PlayerClass::Warrior:
            specialPool[specialCount++] = UpgradeType::WarriorSmolderingFury;
            specialPool[specialCount++] = UpgradeType::WarriorFuriousMight;
            specialPool[specialCount++] = UpgradeType::WarriorBattleTrance;
            specialPool[specialCount++] = UpgradeType::WarriorUnbreakable;
            specialPool[specialCount++] = UpgradeType::WarriorColossus;
            specialPool[specialCount++] = UpgradeType::WarriorWarlordsReach;
            specialPool[specialCount++] = UpgradeType::WarriorBattleMeditation;
            specialPool[specialCount++] = UpgradeType::WarriorWeaponMaster;
            break;
        case PlayerClass::Hunter:
            specialPool[specialCount++] = UpgradeType::HunterPredatorsRhythm;
            specialPool[specialCount++] = UpgradeType::HunterQuarry;
            specialPool[specialCount++] = UpgradeType::HunterApexPredator;
            specialPool[specialCount++] = UpgradeType::HunterFletcher;
            specialPool[specialCount++] = UpgradeType::HunterTrappersCunning;
            specialPool[specialCount++] = UpgradeType::HunterSwiftQuiver;
            specialPool[specialCount++] = UpgradeType::HunterSurvivalist;
            specialPool[specialCount++] = UpgradeType::HunterFocusedBreathing;
            break;
        case PlayerClass::Rogue:
            specialPool[specialCount++] = UpgradeType::RogueDeepReserves;
            specialPool[specialCount++] = UpgradeType::RogueRuthlessFinisher;
            specialPool[specialCount++] = UpgradeType::RogueExsanguinate;
            specialPool[specialCount++] = UpgradeType::RogueToxinExpert;
            specialPool[specialCount++] = UpgradeType::RogueMasterPoisoner;
            specialPool[specialCount++] = UpgradeType::RogueFleetFootwork;
            specialPool[specialCount++] = UpgradeType::RogueShadowConditioning;
            specialPool[specialCount++] = UpgradeType::RogueOpportunist;
            break;
        case PlayerClass::Paladin:
            specialPool[specialCount++] = UpgradeType::PaladinHolyMight;
            specialPool[specialCount++] = UpgradeType::PaladinDivineWrath;
            specialPool[specialCount++] = UpgradeType::PaladinZealotsFury;
            specialPool[specialCount++] = UpgradeType::PaladinMirroredAegis;
            specialPool[specialCount++] = UpgradeType::PaladinDevotion;
            specialPool[specialCount++] = UpgradeType::PaladinCrusadersVitality;
            specialPool[specialCount++] = UpgradeType::PaladinSanctuary;
            specialPool[specialCount++] = UpgradeType::PaladinDivineConduit;
            break;
        case PlayerClass::Warlock:
            specialPool[specialCount++] = UpgradeType::WarlockGrimHarvest;
            specialPool[specialCount++] = UpgradeType::WarlockDarkPact;
            specialPool[specialCount++] = UpgradeType::WarlockSoulBargain;
            specialPool[specialCount++] = UpgradeType::WarlockLingeringMalice;
            specialPool[specialCount++] = UpgradeType::WarlockVoidAttunement;
            specialPool[specialCount++] = UpgradeType::WarlockCorruptedVitality;
            specialPool[specialCount++] = UpgradeType::WarlockSoulConduit;
            specialPool[specialCount++] = UpgradeType::WarlockOccultPower;
            break;
        default: break;
        }
        if (isMelee) specialPool[specialCount++] = UpgradeType::ClassAttunement;
        specialPool[specialCount++] = UpgradeType::Overload;
        for (int i = 0; i < specialCount; i++)
        {
            int j = GetRandomValue(i, specialCount - 1);
            UpgradeType tmp = specialPool[i]; specialPool[i] = specialPool[j]; specialPool[j] = tmp;
        }

        int optionCount = 0;
        int abilityOffers = std::min(2, abilityCount);
        for (int i = 0; i < abilityOffers; i++)
            _levelUpOptions[optionCount++] = abilityPool[i];
        // Each remaining slot: 55% chance of a class/build card so Power
        // Choices regularly ask a class question; utilities fill the rest.
        int sIdx = 0, uIdx = 0;
        while (optionCount < 3)
        {
            if (sIdx < specialCount && GetRandomValue(0, 99) < 55)
                _levelUpOptions[optionCount++] = specialPool[sIdx++];
            else if (uIdx < utilityCount)
                _levelUpOptions[optionCount++] = utilityPool[uIdx++];
            else if (sIdx < specialCount)
                _levelUpOptions[optionCount++] = specialPool[sIdx++];
            else
                break;
        }

        for (int i = 0; i < 3; i++)
        {
            int j = GetRandomValue(i, 2);
            UpgradeType tmp = _levelUpOptions[i]; _levelUpOptions[i] = _levelUpOptions[j]; _levelUpOptions[j] = tmp;
        }

        _showUltimateRow   = false;
        _ultimateRowPicked = false;
        _regularRowPicked  = false;
        return;
    }

    // Separate pools by rarity. Ability learn/upgrade cards remain on the boss
    // reward screen; these offers are purely the RPG stat-growth layer.
    UpgradeType commonPool[6] = {
        UpgradeType::AttackPower, UpgradeType::AttackRange,
        UpgradeType::MaxHealth,   UpgradeType::MaxMana,
        UpgradeType::Defense,     UpgradeType::MoveSpeed
    };
    UpgradeType rarePool[6] = {
        UpgradeType::IronConstitution, UpgradeType::SwiftFeet, UpgradeType::Ferocity,
        UpgradeType::ArcaneMind,       UpgradeType::IronSkin,  UpgradeType::BladeEdge
    };
    UpgradeType epicPool[5] = {
        UpgradeType::WarGod,    UpgradeType::Resilience, UpgradeType::BladeStorm,
        UpgradeType::Juggernaut, UpgradeType::ArcaneColossus
    };

    auto shufflePool = [](UpgradeType* pool, int size) {
        for (int i = 0; i < size; i++)
        {
            int j = GetRandomValue(i, size - 1);
            UpgradeType tmp = pool[i];
            pool[i] = pool[j];
            pool[j] = tmp;
        }
    };
    shufflePool(commonPool, 6);
    shufflePool(rarePool,   6);
    shufflePool(epicPool,   5);

    const int level = _player.GetLevel();
    const int roomProgress = ((_currentAct - 1) * 5) + std::max(0, _currentRoom - 1);

    int commonW = 0;
    int rareW   = 0;
    int epicW   = 0;

    switch (context)
    {
    case LevelUpOfferContext::TreasureBasic:
        commonW = 100;
        break;

    case LevelUpOfferContext::EliteReward:
        rareW = std::max(55, 82 - roomProgress * 3);
        epicW = std::min(45, 18 + roomProgress * 3 + std::max(0, level - 5) * 2);
        break;

    case LevelUpOfferContext::StoreStock:
        commonW = std::max(30, 78 - roomProgress * 4);
        rareW   = std::min(55, 22 + roomProgress * 3);
        epicW   = (_currentAct >= 3) ? std::min(15, 5 + (_currentAct - 3) * 5) : 0;
        break;

    case LevelUpOfferContext::NormalLevel:
    default:
        commonW = std::max(18, 72 - level * 5 - roomProgress * 3);
        rareW   = std::min(57, 20 + level * 4 + roomProgress * 2);
        epicW   = std::max(6, 8 + std::max(0, level - 4) * 2 + roomProgress);
        break;
    }

    int cIdx = 0, rIdx = 0, eIdx = 0;
    for (int i = 0; i < 3; i++)
    {
        int total = commonW + rareW + epicW;
        int roll  = (total > 0) ? GetRandomValue(0, total - 1) : 0;

        UpgradeType picked = UpgradeType::AttackPower;
        if (roll < commonW && cIdx < 6)                     picked = commonPool[cIdx++];
        else if (roll < commonW + rareW && rIdx < 6)       picked = rarePool[rIdx++];
        else if (eIdx < 5)                                 picked = epicPool[eIdx++];
        else if (rIdx < 6)                                 picked = rarePool[rIdx++];
        else if (cIdx < 6)                                 picked = commonPool[cIdx++];

        _levelUpOptions[i] = picked;
    }

    // Ability row is only used for the run-start choice right now.
    _showUltimateRow   = false;
    _ultimateRowPicked = false;
    _regularRowPicked  = false;
}

// =============================================================================
// 5th-wave ability choice screen
// =============================================================================

void Engine::GenerateAbilityChoiceOptions()
{
    _abilityChoiceOptionCount = 0;
    _abilityChoiceSwapPending = false;

    UpgradeType pool[kAllAbilityCount];
    int poolSize = 0;

    for (int i = 0; i < kAllAbilityCount; i++)
    {
        AbilityType ability = kAllAbilities[i];
        // Meta + class gate: locked or off-class abilities never appear.
        if (!_meta.IsAbilityUnlocked(ability) || !_player.ClassAllows(ability))
            continue;

        if (!_player.HasLearnedAbility(ability))
            pool[poolSize++] = LearnTypeForAbility(ability);
        else if (_player.CanUpgradeAbility(ability))
            pool[poolSize++] = UpgradeTypeForAbility(ability);
    }

    // Fisher-Yates shuffle then pick up to 3
    for (int i = 0; i < poolSize; i++)
    {
        int j = GetRandomValue(i, poolSize - 1);
        UpgradeType tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
    }

    _abilityChoiceOptionCount = (poolSize < 3) ? poolSize : 3;
    for (int i = 0; i < _abilityChoiceOptionCount; i++)
        _abilityChoiceOptions[i] = pool[i];
}

// =============================================================================
// Treasure chest mixed reward - 3 cards, each randomly an ability or stat upgrade
// =============================================================================

void Engine::GenerateTreasureChestOptions()
{
    // -- Build ability pool (unlearned abilities or upgradable ones) ------------
    UpgradeType abilityPool[kAllAbilityCount];
    int abilityPoolSize = 0;
    for (int i = 0; i < kAllAbilityCount; i++)
    {
        AbilityType ability = kAllAbilities[i];
        // Meta + class gate: locked or off-class abilities never appear.
        if (!_meta.IsAbilityUnlocked(ability) || !_player.ClassAllows(ability))
            continue;

        if (!_player.HasLearnedAbility(ability))
            abilityPool[abilityPoolSize++] = LearnTypeForAbility(ability);
        else if (_player.CanUpgradeAbility(ability))
            abilityPool[abilityPoolSize++] = UpgradeTypeForAbility(ability);
    }
    for (int i = 0; i < abilityPoolSize; i++)
    {
        int j = GetRandomValue(i, abilityPoolSize - 1);
        UpgradeType tmp = abilityPool[i]; abilityPool[i] = abilityPool[j]; abilityPool[j] = tmp;
    }

    // -- Build stat upgrade pool (common + rare) --------------------------------
    UpgradeType statPool[12] = {
        UpgradeType::AttackPower,      UpgradeType::AttackRange,   UpgradeType::MaxHealth,
        UpgradeType::MaxMana,          UpgradeType::Defense,       UpgradeType::MoveSpeed,
        UpgradeType::IronConstitution, UpgradeType::SwiftFeet,     UpgradeType::Ferocity,
        UpgradeType::ArcaneMind,       UpgradeType::IronSkin,      UpgradeType::BladeEdge
    };
    for (int i = 0; i < 12; i++)
    {
        int j = GetRandomValue(i, 11);
        UpgradeType tmp = statPool[i]; statPool[i] = statPool[j]; statPool[j] = tmp;
    }

    _levelUpOfferContext  = LevelUpOfferContext::TreasureChest;
    _showUltimateRow      = false;
    _ultimateRowPicked    = false;
    _regularRowPicked     = false;

    // -- Fill 3 slots - each slot randomly an ability or a stat upgrade ---------
    int aIdx = 0;
    int sIdx = 0;
    for (int i = 0; i < 3; i++)
    {
        bool canAbility = (aIdx < abilityPoolSize);
        bool canStat    = (sIdx < 12);
        bool pickAbility = canAbility && (!canStat || GetRandomValue(0, 1) == 0);

        if (pickAbility)
            _levelUpOptions[i] = abilityPool[aIdx++];
        else if (canStat)
            _levelUpOptions[i] = statPool[sIdx++];
        else
            _levelUpOptions[i] = UpgradeType::AttackPower; // should never reach
    }
}

void Engine::DrawAbilityChoice()
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.68f));

    bool ready = (_abilityChoiceOpenTimer <= 0.f);
    Vector2 mouse = GetVirtualMousePos();

    // Helper: map Learn*/Upgrade* ? the underlying AbilityType
    auto upgradeToAbility = [](UpgradeType ut) -> AbilityType
    {
        int base = (int)ut;
        if (base >= (int)UpgradeType::UpgradeFireSpread)
            return (AbilityType)(base - (int)UpgradeType::UpgradeFireSpread);
        if (base >= (int)UpgradeType::LearnFireSpread)
            return (AbilityType)(base - (int)UpgradeType::LearnFireSpread);
        return AbilityType::None;
    };

    auto elementColor = [](AbilityType ab) -> Color
    {
        if (ab == AbilityType::FireSpread  || ab == AbilityType::FireBolt  || ab == AbilityType::FireUltimate)
            return Color{255, 110,  20, 255};
        if (ab == AbilityType::IceSpread   || ab == AbilityType::IceBolt   || ab == AbilityType::IceUltimate)
            return Color{100, 210, 255, 255};
        return Color{255, 220,  30, 255};  // electric
    };

    // -- Swap sub-mode ---------------------------------------------------------
    if (_abilityChoiceSwapPending)
    {
        const char* title = "Replace which ability?";
        int tSz = 42;
        DrawText(title, (int)(sw/2.f - MeasureText(title, tSz)/2.f), (int)(sh*0.06f), tSz, GOLD);

        const float cardW = 210.f, cardH = 260.f, cardGap = 28.f;
        int count = _player.GetLearnedCount();
        float totalW = count * (cardW + cardGap) - cardGap;
        float sx = sw/2.f - totalW/2.f;

        for (int i = 0; i < count; i++)
        {
            float cx = sx + i * (cardW + cardGap);
            Rectangle card{cx, sh/2.f - cardH/2.f, cardW, cardH};
            bool hov = (ready && CheckCollisionPointRec(mouse, card)) ||
                       (_gamepad.isActive && i == _abilityChoiceGpCursor);

            DrawRectangleRounded(card, 0.12f, 8, hov ? Color{80,20,20,235} : Color{40,15,15,210});
            DrawRectangleRoundedLines(card, 0.12f, 8, hov ? RED : Color{180,55,55,160});

            AbilityType ab = _player.GetLearnedAbility(i);
            Color ec = elementColor(ab);
            // Small element dot
            DrawCircleV({cx + cardW/2.f, card.y + cardH*0.30f}, 22.f, Fade(ec, 0.3f));
            DrawCircleV({cx + cardW/2.f, card.y + cardH*0.30f}, 14.f, Fade(ec, 0.7f));

            const char* abName = GetAbilityName(ab);
            int nSz = 20;
            DrawText(abName, (int)(cx + cardW/2.f - MeasureText(abName, nSz)/2.f),
                     (int)(card.y + cardH*0.56f), nSz, hov ? GOLD : RAYWHITE);

            int lv = _player.GetAbilityLevel(ab);
            char lvStr[16]; snprintf(lvStr, sizeof(lvStr), "Lv %d", lv);
            int lvSz = 17;
            DrawText(lvStr, (int)(cx + cardW/2.f - MeasureText(lvStr, lvSz)/2.f),
                     (int)(card.y + cardH*0.70f), lvSz, Color{255,200,80,255});

            bool swapCardActivated = (ready && hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) ||
                                     (_gamepad.isActive && ready && i == _abilityChoiceGpCursor && _gamepad.menuConfirmPressed);
            if (swapCardActivated)
            {
                _player.RemoveAbilityAtSlot(i);
                _player.ApplyUpgrade(_abilityChoiceSwapTarget);
                _abilityChoiceSwapPending = false;
                if (_pendingRoomChoice)
                {
                    _pendingRoomChoice = false;
                    if (_bossesDefeated >= 2)
                        _gameState = GameState::DemoEnd;
                    else
                    {
                        _roomClearPending = true;
                        _gameState = GameState::Play;
                    }
                }
                else
                    _gameState = GameState::Play;
            }
        }

        // Cancel button
        Rectangle cancelBtn{sw/2.f - 90.f, sh*0.82f, 180.f, 44.f};
        bool cHov = ready && CheckCollisionPointRec(mouse, cancelBtn);
        DrawRectangleRounded(cancelBtn, 0.4f, 8, cHov ? Color{55,55,55,230} : Color{28,28,28,200});
        DrawRectangleRoundedLines(cancelBtn, 0.4f, 8, cHov ? WHITE : GRAY);
        const char* cTxt = "Cancel";
        int cSz = 22;
        DrawText(cTxt, (int)(cancelBtn.x + cancelBtn.width/2.f - MeasureText(cTxt,cSz)/2.f),
                 (int)(cancelBtn.y + cancelBtn.height/2.f - cSz/2.f), cSz, RAYWHITE);
        if ((ready && cHov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) ||
            (_gamepad.isActive && ready && _gamepad.backPressed))
            _abilityChoiceSwapPending = false;
        return;
    }

    // -- Main ability choice ----------------------------------------------------
    const char* title = "Ability Upgrade!";
    int tSz = 46;
    DrawText(title, (int)(sw/2.f - MeasureText(title, tSz)/2.f), (int)(sh*0.06f), tSz, GOLD);

    const char* sub = "Choose an ability to learn or upgrade:";
    int subSz = 24;
    DrawText(sub, (int)(sw/2.f - MeasureText(sub, subSz)/2.f),
             (int)(sh*0.06f + tSz + 8.f), subSz, Color{200,200,200,200});

    const float cardW = 280.f, cardH = 360.f, cardGap = 40.f;
    int count = _abilityChoiceOptionCount;
    float totalW = count * (cardW + cardGap) - cardGap;
    float sx = sw/2.f - totalW/2.f;
    float cardY = sh/2.f - cardH/2.f - 10.f;

    for (int i = 0; i < count; i++)
    {
        float cx = sx + i * (cardW + cardGap);
        Rectangle card{cx, cardY, cardW, cardH};
        bool hov = (ready && CheckCollisionPointRec(mouse, card)) ||
                   (_gamepad.isActive && i == _abilityChoiceGpCursor);

        UpgradeType opt = _abilityChoiceOptions[i];
        bool isLearn   = ((int)opt >= (int)UpgradeType::LearnFireSpread &&
                          (int)opt <= (int)UpgradeType::LearnElectricUltimate);
        AbilityType ab = upgradeToAbility(opt);
        Color ec       = elementColor(ab);

        // Card background tinted by element
        Color bgN = Color{(uint8_t)(ec.r/7), (uint8_t)(ec.g/7), (uint8_t)(ec.b/7), 210};
        Color bgH = Color{(uint8_t)(ec.r/4), (uint8_t)(ec.g/4), (uint8_t)(ec.b/4), 235};
        DrawRectangleRounded(card, 0.12f, 8, hov ? bgH : bgN);
        DrawRectangleRoundedLines(card, 0.12f, 8, hov ? ec : Color{ec.r,ec.g,ec.b,120});

        // Tag: NEW / UPGRADE
        const char* tag = isLearn ? "NEW" : "UPGRADE";
        Color tagColor  = isLearn ? Color{100,255,120,255} : Color{255,200,50,255};
        int tagSz = 17;
        DrawText(tag, (int)(cx + cardW/2.f - MeasureText(tag, tagSz)/2.f),
                 (int)(cardY + 12.f), tagSz, tagColor);

        // Element icon circle
        float circleY = cardY + cardH * 0.30f;
        DrawCircleV({cx + cardW/2.f, circleY}, 42.f, Fade(ec, 0.20f));
        DrawCircleV({cx + cardW/2.f, circleY}, 28.f, Fade(ec, 0.55f));

        // Ability name
        const char* abName = GetAbilityName(ab);
        int nSz = 26;
        DrawText(abName, (int)(cx + cardW/2.f - MeasureText(abName, nSz)/2.f),
                 (int)(cardY + cardH*0.54f), nSz, hov ? GOLD : RAYWHITE);

        // Level info / description
        if (!isLearn)
        {
            int lv = _player.GetAbilityLevel(ab);
            char lvStr[32]; snprintf(lvStr, sizeof(lvStr), "Lv %d  \xE2\x86\x92  Lv %d", lv, lv + 1);
            int lvSz = 19;
            DrawText(lvStr, (int)(cx + cardW/2.f - MeasureText(lvStr, lvSz)/2.f),
                     (int)(cardY + cardH*0.66f), lvSz, Color{255,200,80,255});
            const char* upDesc = "+1 spell damage";
            int udSz = 17;
            DrawText(upDesc, (int)(cx + cardW/2.f - MeasureText(upDesc, udSz)/2.f),
                     (int)(cardY + cardH*0.76f), udSz, LIGHTGRAY);
        }
        else
        {
            // Split ability description across two lines
            const char* abDesc = GetAbilityDesc(ab);
            std::string ds = abDesc;
            int nl = (int)ds.find('\n');
            int dSz = 19;
            if (nl != (int)std::string::npos)
            {
                std::string l1 = ds.substr(0, nl), l2 = ds.substr(nl + 1);
                DrawText(l1.c_str(), (int)(cx + cardW/2.f - MeasureText(l1.c_str(),dSz)/2.f),
                         (int)(cardY + cardH*0.66f), dSz, LIGHTGRAY);
                DrawText(l2.c_str(), (int)(cx + cardW/2.f - MeasureText(l2.c_str(),dSz)/2.f),
                         (int)(cardY + cardH*0.66f + dSz + 4), dSz, LIGHTGRAY);
            }
            else
                DrawText(abDesc, (int)(cx + cardW/2.f - MeasureText(abDesc,dSz)/2.f),
                         (int)(cardY + cardH*0.66f), dSz, LIGHTGRAY);
        }

        bool abilCardActivated = (ready && hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) ||
                                 (_gamepad.isActive && ready && i == _abilityChoiceGpCursor && _gamepad.menuConfirmPressed);
        if (abilCardActivated)
        {
            if (isLearn && _player.GetLearnedCount() >= _player.GetMaxAbilitySlots())
            {
                // No free slot - enter swap mode
                _abilityChoiceSwapPending = true;
                _abilityChoiceSwapTarget  = opt;
            }
            else
            {
                _player.ApplyUpgrade(opt);
                _abilityChoiceSwapPending = false;
                if (_pendingRoomChoice)
                {
                    _pendingRoomChoice = false;
                    if (_bossesDefeated >= 2)
                        _gameState = GameState::DemoEnd;
                    else
                    {
                        _roomClearPending = true;
                        _gameState = GameState::Play;
                    }
                }
                else
                    _gameState = GameState::Play;
            }
        }
    }

    if (count == 0)
    {
        const char* msg = "All abilities are at max level!";
        int mSz = 28;
        DrawText(msg, (int)(sw/2.f - MeasureText(msg, mSz)/2.f), (int)(sh/2.f - mSz/2.f), mSz, LIGHTGRAY);
    }

    // Continue button
    const float contScale = _touchModeActive ? 1.3f : 1.f;
    Rectangle contBtn{sw/2.f - (220.f * contScale) / 2.f, sh*0.87f, 220.f * contScale, 52.f * contScale};
    bool contHov = ready && CheckCollisionPointRec(mouse, contBtn);
    DrawRectangleRounded(contBtn, 0.4f, 8, contHov ? Color{35,60,35,235} : Color{18,35,18,200});
    DrawRectangleRoundedLines(contBtn, 0.4f, 8, contHov ? Color{100,210,100,255} : Color{55,120,55,160});
    const char* contTxt = "Continue";
    int contSz = _touchModeActive ? 31 : 24;
    DrawText(contTxt, (int)(contBtn.x + contBtn.width/2.f - MeasureText(contTxt, contSz)/2.f),
             (int)(contBtn.y + contBtn.height/2.f - contSz/2.f), contSz,
             contHov ? Color{160,255,160,255} : RAYWHITE);
    if (ready && contHov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        if (_pendingRoomChoice)
        {
            _pendingRoomChoice = false;
            if (_bossesDefeated >= 2)
                _gameState = GameState::DemoEnd;
            else
            {
                _roomClearPending = true;
                _gameState = GameState::Play;
            }
        }
        else
            _gameState = GameState::Play;
    }
}

// -- DrawDemoEnd ---------------------------------------------------------------
void Engine::DrawDemoEnd()
{
    DemoEndRenderContext ctx{};
    ctx.touchModeActive = _touchModeActive;
    ctx.demoCompleted = _demoCompleted;
    ctx.enemiesKilled = _enemiesKilled;
    ctx.bossesDefeated = _bossesDefeated;
    ctx.playerGold = _player.GetGold();
    ctx.playerLevel = _player.GetLevel();
    _overlayRenderer.DrawDemoEnd(ctx);
}

// -- UpdateExpTally ------------------------------------------------------------
// Drains _pendingExp into the player at 50 EXP/sec with NO interruptions.
// Once the bar is full the player presses Continue, then level-up choices
// fire (one per level gained).  Skip input drains the remainder instantly.
void Engine::UpdateExpTally(float dt)
{
    bool skip = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_M)
             || (_touchModeActive && IsMouseButtonPressed(MOUSE_LEFT_BUTTON));

    if (_expTallyDone)
    {
        if (skip)
        {
            _tallyChoiceChaining = false;
            if (_currentRoomType == RoomType::Elite && !_eliteRewardGranted)
            {
                _eliteRewardGranted = true;
                GenerateLevelUpOptions(LevelUpOfferContext::EliteReward);
                _levelUpReturnState = GameState::ExpTally;
                _levelUpOpenTimer   = 0.25f;
                _gameState          = GameState::LevelUpChoice;
            }
            else if (_currentRoomType == RoomType::Boss && _lastAbilityChoiceWave != _wave)
            {
                _lastAbilityChoiceWave  = _wave;
                GenerateAbilityChoiceOptions();
                _abilityChoiceOpenTimer = 0.5f;
                _pendingRoomChoice      = true;
                _gameState = GameState::AbilityChoice;
            }
            else
            {
                _roomClearPending = true;
                _gameState = GameState::Play;
            }
        }
        return;
    }

    // -- Bar still draining ----------------------------------------------------

    if (skip)
    {
        // Instantly drain all remaining EXP - no level-up interruption during drain.
        if (_pendingExp > 0.f)
        {
            _player.AddExp((int)_pendingExp);
            _pendingExp    = 0.f;
            _expTallyAccum = 0.f;
        }
        _tallyLevelUpsRemaining = _player.GetLevel() - _tallyStartLevel;
        _expTallyDone = true;
        return;
    }

    // Animated drain - 50 EXP per second, no interruptions.
    static constexpr float kDrainRate = 50.f;
    float drain = std::min(kDrainRate * dt, _pendingExp);
    _pendingExp    -= drain;
    _expTallyAccum += drain;

    int wholeExp = (int)_expTallyAccum;
    if (wholeExp > 0)
    {
        _expTallyAccum -= (float)wholeExp;
        _player.AddExp(wholeExp);   // level-ups handled silently inside AddExp
    }

    if (_pendingExp <= 0.f)
    {
        _pendingExp             = 0.f;
        _expTallyAccum          = 0.f;
        _tallyLevelUpsRemaining = _player.GetLevel() - _tallyStartLevel;
        _expTallyDone           = true;
    }
}

// -- DrawExpTally --------------------------------------------------------------
// Dark overlay drawn on top of the game world showing EXP bar filling and
// the player's current level.  Dismiss hint appears once the bar is full.
void Engine::DrawExpTally()
{
    ExpTallyRenderContext ctx{};
    ctx.player = &_player;
    ctx.tallyStartLevel = _tallyStartLevel;
    ctx.pendingExp = _pendingExp;
    ctx.expTallyDone = _expTallyDone;
    ctx.tallyLevelUpsRemaining = _tallyLevelUpsRemaining;
    ctx.tallyChoiceChaining = _tallyChoiceChaining;
    ctx.touchModeActive = _touchModeActive;
    _overlayRenderer.DrawExpTally(ctx);
}

// -- DrawMap -------------------------------------------------------------------
// Full-screen Slay-the-Spire-style act map.  Shows the entire node graph for
// the current act; available nodes are highlighted and clickable.
void Engine::DrawMap()
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    bool ready     = (_mapOpenTimer <= 0.f);
    Vector2 mouse  = GetVirtualMousePos();

    // -- Background --------------------------------------------------------
    Biome actBiome = GetBiomeForAct(_currentAct);
    Color bgDark = Color{ 78, 62, 40, 255 };
    Color bgLight = Color{ 110, 88, 58, 255 };
    if (actBiome == Biome::Forest)
    {
        bgDark  = Color{ 30, 74, 42, 255 };
        bgLight = Color{ 48, 112, 66, 255 };
    }
    DrawScrollingCheckerboard(sw, sh, bgDark, bgLight, 18.f, 10.f);

    // -- Header ? centred over the node graph area -------------------------
    // Graph spans 30?76% of the screen; journey panel occupies 78?97%.
    const float mapCentreX = sw * _mapHeaderX;
    std::string header = "Act " + std::to_string(_currentAct)
                       + "  -  " + GetBiomeName(actBiome);
    int hSz = (int)_mapHeaderFs;
    DrawText(header.c_str(),
        (int)(mapCentreX - MeasureText(header.c_str(), hSz) / 2.f), (int)_mapHeaderY, hSz, Color{255, 214, 102, 255});
    const char* sub = "Choose your path";
    const float subCentreX = sw * _mapSubX;
    const int subFs = (int)_mapSubFs;
    DrawText(sub,
        (int)(subCentreX - MeasureText(sub, subFs) / 2.f), (int)_mapSubY, subFs,
        Color{206, 242, 255, 210});

    if (_actMap.empty()) return;

    // -- Helpers -----------------------------------------------------------
    auto nodeColor = [](RoomType rt) -> Color {
        switch (rt) {
        case RoomType::Boss:     return Color{220,  40,  40, 255};
        case RoomType::Elite:    return Color{230, 130,  20, 255};
        case RoomType::Rest:     return Color{ 60, 190,  90, 255};
        case RoomType::Treasure: return Color{220, 190,  30, 255};
        case RoomType::Store:    return Color{ 80, 190, 230, 255};
        default:                 return Color{ 80, 130, 220, 255};
        }
    };
    auto roomIcon = [&](RoomType rt) -> Texture2D* {
        switch (rt) {
        case RoomType::Standard: return &_mapIconNormal;
        case RoomType::Elite:    return &_mapIconElite;
        case RoomType::Rest:     return &_mapIconRest;
        case RoomType::Store:    return &_mapIconShop;
        case RoomType::Treasure: return &_mapIconTreasure;
        case RoomType::Boss:     return &_mapIconBoss;
        default:                 return nullptr;
        }
    };
    auto nodeDesc = [](RoomType rt) -> const char* {
        switch (rt) {
        case RoomType::Boss:     return "Act finale  -  boss area";
        case RoomType::Elite:    return "Miniboss area  -  rarer reward";
        case RoomType::Rest:     return "Safe area  -  heal pickups appear";
        case RoomType::Treasure: return "Reward area  -  free upgrade card";
        case RoomType::Store:    return "Shop area  -  placeholder for now";
        default:                 return "Combat area  -  standard area reward";
        }
    };

    // -- Left panel: stats + legend ----------------------------------------
    {
        const float pX  = 48.f;
        const float pY  = 98.f;
        const float pH  = sh - pY - 98.f;
        const float pW  = 490.f;
        const float pad = 30.f;
        const float legendIndent = 20.f;
        const float titleSize = 32.f;
        const float statFont = 28.f;
        const float baseStatRowH = 44.f;
        const float legendIconSize = 56.f;
        const float legendLabelSize = 28.f;
        const float legendDescSize = 21.f;
        const float baseLegendRowH = 80.f;
        const float legendGap = 22.f;
        const float titleToRuleGap = 8.f;
        const float ruleToRowsGap = 16.f;
        const float sectionGapBase = 22.f;

        // Pre-calculate content height so the panel can grow to roughly match
        // the graph height without clipping smaller screens.
        int statRows = 5;  // Level, HP, MP, Attack, Defense
        if (_player.GetLevel() < _player.GetMaxLevel()) statRows++;  // EXP row
        float contentH = pad
                       + titleSize
                       + titleToRuleGap
                       + ruleToRowsGap
                       + statRows * baseStatRowH
                       + sectionGapBase
                       + titleSize
                       + titleToRuleGap
                       + ruleToRowsGap
                       + 6.f * baseLegendRowH
                       + pad;
        float panelH = std::max(contentH, pH);
        float extraSpace = panelH - contentH;
        float statRowH = baseStatRowH + extraSpace * 0.12f / (float)statRows;
        float legendRowH = baseLegendRowH + extraSpace * 0.88f / 6.f;
        float sectionGap = sectionGapBase + extraSpace * 0.08f;

        // Panel background
        DrawRectangleRounded({ pX, pY, pW, panelH }, 0.05f, 8,
            Fade(Color{12, 71, 84, 255}, 0.92f));
        DrawRectangleRoundedLines({ pX, pY, pW, panelH }, 0.05f, 8,
            Fade(Color{130, 235, 255, 255}, 0.30f));

        float cy         = pY + pad;
        float rightEdge  = pX + pW - pad;

        // -- Stats ------------------------------------------------------
        DrawText("PLAYER STATS", (int)(pX + pad), (int)cy, (int)titleSize, Color{255, 214, 102, 255});
        cy += titleSize + titleToRuleGap;
        DrawLineEx({ pX + pad, cy }, { rightEdge, cy }, 1.f, Fade(Color{130, 235, 255, 255}, 0.55f));
        cy += ruleToRowsGap;

        // stat row: label left, coloured value right
        auto statRow = [&](const char* lbl, const char* val, Color valCol = RAYWHITE)
        {
            DrawText(lbl, (int)(pX + pad), (int)cy, (int)statFont, Color{188, 228, 238, 228});
            int vw = MeasureText(val, (int)statFont);
            DrawText(val, (int)(rightEdge - vw), (int)cy, (int)statFont, valCol);
            cy += statRowH;
        };

        statRow("Level",   TextFormat("%d / %d", _player.GetLevel(), _player.GetMaxLevel()));

        float hpPct = (_player.GetMaxHealthValue() > 0.f)
            ? _player.GetHealthValue() / _player.GetMaxHealthValue() : 0.f;
        Color hpCol = hpPct > 0.6f ? GREEN : hpPct > 0.30f ? YELLOW : RED;
        statRow("HP",
            TextFormat("%d / %d",
                (int)std::ceil(_player.GetHealthValue()),
                (int)std::ceil(_player.GetMaxHealthValue())), hpCol);

        statRow("MP",
            TextFormat("%d / %d", _player.GetMana(), _player.GetMaxMana()),
            Color{100, 160, 255, 255});

        statRow("Attack", TextFormat("%d", (int)std::ceil((float)_player.GetMeleeDamage())));
        statRow("Armour", TextFormat("%d / %d", _player.GetArmour(), _player.GetMaxArmour()),
            Color{ 120, 180, 255, 255 });

        if (_player.GetLevel() < _player.GetMaxLevel())
            statRow("EXP",
                TextFormat("%d / %d", _player.GetExp(), _player.GetExpToNext()),
                Color{220, 190, 50, 220});

        // -- Section divider --------------------------------------------
        cy += sectionGap;
        DrawLineEx({ pX + pad, cy }, { rightEdge, cy }, 1.f, Fade(Color{130, 235, 255, 255}, 0.24f));
        cy += 18.f;

        // -- Legend -----------------------------------------------------
        DrawText("LEGEND", (int)(pX + pad), (int)cy, (int)titleSize, Color{255, 214, 102, 255});
        cy += titleSize + titleToRuleGap;
        DrawLineEx({ pX + pad, cy }, { rightEdge, cy }, 1.f, Fade(Color{130, 235, 255, 255}, 0.55f));
        cy += ruleToRowsGap;

        const float iconX = pX + pad + legendIndent;
        const float txtX  = iconX + legendIconSize + legendGap;

        auto legendRow = [&](Texture2D* icon, Color circleCol,
                             const char* name, const char* desc)
        {
            float iconY = cy + (legendRowH - legendIconSize) * 0.5f;
            if (icon && icon->id > 0)
            {
                Rectangle src = { 0, 0, (float)icon->width, (float)icon->height };
                DrawTexturePro(*icon, src, { iconX, iconY, legendIconSize, legendIconSize }, {}, 0.f, WHITE);
            }
            else
            {
                // Coloured circle for rooms without a PNG icon (Rest, Boss)
                Vector2 c = { iconX + legendIconSize * 0.5f, iconY + legendIconSize * 0.5f };
                DrawCircleV(c, legendIconSize * 0.44f, Fade(circleCol, 0.65f));
                DrawCircleLines((int)c.x, (int)c.y, legendIconSize * 0.44f, circleCol);
            }
            float labelY = cy + 3.f;
            float descY  = cy + legendLabelSize + 7.f;
            DrawText(name, (int)txtX, (int)labelY, (int)legendLabelSize, RAYWHITE);
            DrawText(desc, (int)txtX, (int)descY, (int)legendDescSize, Color{188, 228, 238, 215});
            cy += legendRowH;
        };

        legendRow(&_mapIconNormal,   { 80,130,220,255 }, "NORMAL AREA",   "Standard combat area");
        legendRow(&_mapIconElite,    {230,130, 20,255 }, "ELITE AREA",    "Miniboss area");
        legendRow(&_mapIconRest,     { 60,190, 90,255 }, "REST AREA",     "Safe area with food");
        legendRow(&_mapIconTreasure, {220,190, 30,255 }, "TREASURE AREA", "Free upgrade card");
        legendRow(&_mapIconShop,     { 80,190,230,255 }, "SHOP AREA",     "Placeholder shop area");
        legendRow(&_mapIconBoss,     {220, 40, 40,255 }, "BOSS AREA",     "Act finale");
    }

    // -- Right panel: journey history --------------------------------------
    {
        const float jX  = sw * _mapJourneyX;
        const float jY  = 98.f;
        const float jW  = sw - jX - 48.f;
        const float jH  = sh - jY - 98.f;
        const float pad = _mapPad;

        DrawRectangleRounded({ jX, jY, jW, jH }, 0.05f, 8,
            Fade(Color{12, 71, 84, 255}, 0.92f));
        DrawRectangleRoundedLines({ jX, jY, jW, jH }, 0.05f, 8,
            Fade(Color{130, 235, 255, 255}, 0.30f));

        float cy = jY + pad;
        DrawText("JOURNEY", (int)(jX + pad), (int)cy, (int)_mapTitleFs, Color{255, 214, 102, 255});
        cy += _mapTitleFs + 6.f;
        DrawLineEx({ jX + pad, cy }, { jX + jW - pad, cy }, 1.f,
            Fade(Color{130, 235, 255, 255}, 0.55f));
        cy += 14.f;

        // Visited-room tiles ? one per completed node, stamped with the room
        // icon and crossed with a red diagonal slash.
        int drawn = 0;
        for (const auto& node : _actMap)
            if (node.completed) drawn++;

        int maxJourneySlots = 0;
        for (const auto& node : _actMap)
            maxJourneySlots = std::max(maxJourneySlots, node.row + 1);
        maxJourneySlots = std::max(1, maxJourneySlots);

        const float baseGap = _mapSqGap;
        const float availW = jW - pad * 2.f;
        float fitSqSz = _mapSqSz;
        fitSqSz = std::min(fitSqSz, (availW - baseGap * (maxJourneySlots - 1)) / (float)maxJourneySlots);
        fitSqSz = std::max(20.f, fitSqSz);

        float fitGap = (maxJourneySlots > 1)
            ? std::min(baseGap, std::max(4.f, (availW - fitSqSz * maxJourneySlots) / (float)(maxJourneySlots - 1)))
            : 0.f;
        const float historyRowH = fitSqSz;
        const float historyStartX = jX + pad;

        if (drawn == 0)
        {
            DrawText("No rooms cleared yet",
                (int)historyStartX, (int)(cy + historyRowH * 0.5f - _mapNoRoomsFs * 0.5f),
                (int)_mapNoRoomsFs, RAYWHITE);
        }
        else
        {
            float sx = historyStartX;

            for (const auto& node : _actMap)
            {
                if (!node.completed) continue;

                Color col = nodeColor(node.type);
                Rectangle tile{ sx, cy, fitSqSz, fitSqSz };
                DrawRectangleRounded(tile, 0.25f, 4, Fade(BLACK, 0.92f));
                DrawRectangleRoundedLines(tile, 0.25f, 4, Fade(col, 0.95f));

                if (Texture2D* icon = roomIcon(node.type); icon && icon->id > 0)
                {
                    const float iconPad = std::max(4.f, fitSqSz * 0.12f);
                    Rectangle src = { 0.f, 0.f, (float)icon->width, (float)icon->height };
                    Rectangle dst = {
                        sx + iconPad,
                        cy + iconPad,
                        fitSqSz - iconPad * 2.f,
                        fitSqSz - iconPad * 2.f
                    };
                    DrawTexturePro(*icon, src, dst, {}, 0.f, WHITE);
                }

                const float slashPad = std::max(3.f, fitSqSz * 0.10f);
                DrawLineEx(
                    { sx + slashPad,               cy + fitSqSz - slashPad },
                    { sx + fitSqSz - slashPad,     cy + slashPad },
                    std::max(3.f, fitSqSz * 0.08f),
                    Color{ 215, 55, 55, 255 });

                sx += fitSqSz + fitGap;
            }
        }

        cy += historyRowH + 12.f;

        cy += 16.f;
        DrawText(TextFormat("Gold:  %d", _player.GetGold()),
            (int)(jX + pad), (int)cy, (int)_mapRoomsFs, Color{255, 214, 102, 220});

        // -- Biome progress diamonds ? size-aware zone that lifts upward as
        // the diamonds get larger so the divider and label make room ---------
        {
            const float minDivY = cy + _mapRoomsFs + 20.f;
            const float zoneBot = jY + jH - pad;
            const float wantedHalf = _mapDiamondSz;
            const float wantedSlot = wantedHalf * 2.35f;
            const float wantedZoneH = wantedSlot * (float)kTotalActs;
            const float wantedHeadH = _mapBiomeTitleFs + 22.f;
            const float divY = std::max(minDivY, zoneBot - wantedZoneH - wantedHeadH);
            const float zoneTop = divY + wantedHeadH;
            const float zoneH   = std::max(1.f, zoneBot - zoneTop);

            DrawLineEx({ jX + pad, divY }, { jX + jW - pad, divY }, 1.f,
                Fade(Color{130, 235, 255, 255}, 0.30f));
            DrawText("BIOMES", (int)(jX + pad), (int)(divY + 4.f), (int)_mapBiomeTitleFs,
                Color{255, 214, 102, 200});

            const float slot   = zoneH / (float)kTotalActs;
            const float halfW  = (jW - pad * 2.f) * 0.42f;
            const float half   = std::min(_mapDiamondSz, std::min(halfW, slot * 0.45f));
            const float dcx    = jX + jW * 0.5f;

            auto drawDiamond = [&](float cx, float dcy, float h, const char* label, int state)
            {
                if (state != 0)
                {
                    Color fill = (state == 1)
                        ? Color{255, 185, 30, 230}
                        : Color{12,  12,  20, 220};
                    DrawTriangle({cx, dcy - h}, {cx + h, dcy}, {cx - h, dcy}, fill);
                    DrawTriangle({cx - h, dcy}, {cx + h, dcy}, {cx, dcy + h}, fill);
                }

                float thick  = (state == 1) ? 2.5f : 1.5f;
                Color border = (state == 1)
                    ? Color{255, 230, 120, 255}
                    : Color{130, 130, 140, 200};

                DrawLineEx({cx,     dcy - h}, {cx + h, dcy    }, thick, border);
                DrawLineEx({cx + h, dcy    }, {cx,     dcy + h}, thick, border);
                DrawLineEx({cx,     dcy + h}, {cx - h, dcy    }, thick, border);
                DrawLineEx({cx - h, dcy    }, {cx,     dcy - h}, thick, border);

                int fs = (int)(h * _mapBiomeLabelFs);
                fs = std::max(9, std::min(fs, 24));
                Color tc = (state == 0) ? Color{110, 110, 120, 180}
                         : (state == 1) ? Color{255, 245, 190, 255}
                                        : Color{ 70,  70,  85, 200};
                int tw2 = MeasureText(label, fs);
                DrawText(label, (int)(cx - tw2 / 2.f), (int)(dcy - fs / 2.f), fs, tc);
            };

            for (int i = 0; i < kTotalActs; i++)
            {
                int   act   = kTotalActs - i;
                float dcy   = zoneTop + (i + 0.5f) * slot;
                int   state = (act < _currentAct)  ? 0
                            : (act == _currentAct) ? 1
                                                   : 2;
                const char* name = ((int)_biomeSequence.size() >= act)
                    ? GetBiomeName(_biomeSequence[act - 1])
                    : "?";
                drawDiamond(dcx, dcy, half, name, state);
            }
        }
    }

    // -- Connection lines --------------------------------------------------
    for (int i = 0; i < (int)_actMap.size(); i++)
    {
        const MapNode& n = _actMap[i];
        for (int nextIdx : n.nextNodes)
        {
            if (nextIdx < 0 || nextIdx >= (int)_actMap.size()) continue;
            const MapNode& m = _actMap[nextIdx];
            Color lc = n.completed ? Color{70, 136, 152, 185} : Color{120, 214, 234, 120};
            DrawLineEx(n.drawPos, m.drawPos, 3.5f, lc);
        }
    }

    // -- Nodes -------------------------------------------------------------
    const float kIconSz   = _mapIconSz;
    const float kIconHalf = kIconSz * 0.5f;
    int hoveredIdx = -1;

    for (int i = 0; i < (int)_actMap.size(); i++)
    {
        const MapNode& n  = _actMap[i];
        Color   ec        = nodeColor(n.type);
        Texture2D* icon   = roomIcon(n.type);
        bool    hasIcon   = (icon && icon->id > 0);

        Rectangle hitRect = { n.drawPos.x - kIconHalf - 4.f,
                               n.drawPos.y - kIconHalf - 4.f,
                               kIconSz + 8.f, kIconSz + 8.f };
        bool hov = ready && !n.completed && CheckCollisionPointRec(mouse, hitRect);
        if (hov) hoveredIdx = i;

        // Glow halo behind available nodes
        if (n.available)
        {
            float glow = 0.18f + 0.10f * sinf((float)GetTime() * 3.0f);
            DrawCircleV(n.drawPos, kIconHalf + 14.f, Fade(ec, glow));
            if (hov)
                DrawCircleV(n.drawPos, kIconHalf + 8.f, Fade(ec, 0.35f));
        }

        if (hasIcon)
        {
            Rectangle src = { 0, 0, (float)icon->width, (float)icon->height };
            Rectangle dst = { n.drawPos.x - kIconHalf, n.drawPos.y - kIconHalf,
                               kIconSz, kIconSz };
            Color tint = n.completed ? DARKGRAY
                       : n.available ? WHITE
                       :               Fade(WHITE, 0.35f);
            DrawTexturePro(*icon, src, dst, {}, 0.f, tint);
        }
        else
        {
            float r = kIconHalf - 2.f;
            float alpha = n.completed ? 0.28f : n.available ? 0.72f : 0.18f;
            DrawCircleV(n.drawPos, r, Fade(ec, alpha));
            DrawCircleLines((int)n.drawPos.x, (int)n.drawPos.y, r,
                n.available ? ec : Fade(ec, 0.45f));
            if (n.available && hov)
                DrawCircleLines((int)n.drawPos.x, (int)n.drawPos.y, r + 4.f,
                    Fade(ec, 0.55f));
        }

        // Click
        if (ready && n.available && !n.completed &&
            CheckCollisionPointRec(mouse, hitRect) &&
            IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        {
            EnterMapRoom(i);
        }
    }

    // -- Keyboard-selected node ring ---------------------------------------
    if (_mapKeySelectedIdx >= 0 && _mapKeySelectedIdx < (int)_actMap.size())
    {
        const MapNode& kn = _actMap[_mapKeySelectedIdx];
        if (kn.available && !kn.completed)
        {
            DrawCircleLines((int)kn.drawPos.x, (int)kn.drawPos.y,
                kIconHalf + 9.f,  WHITE);
            DrawCircleLines((int)kn.drawPos.x, (int)kn.drawPos.y,
                kIconHalf + 14.f, Fade(WHITE, 0.35f));
        }
    }

    // -- Current-node indicator (yellow ring on last completed node) -------
    if (_currentMapNodeIdx >= 0 && _currentMapNodeIdx < (int)_actMap.size())
    {
        const MapNode& cur = _actMap[_currentMapNodeIdx];
        if (cur.completed)
            DrawCircleLines((int)cur.drawPos.x, (int)cur.drawPos.y,
                kIconHalf + 11.f, Fade(YELLOW, 0.55f));
    }

    // -- Hovered node description ------------------------------------------
    if (hoveredIdx >= 0)
    {
        const char* dsc = nodeDesc(_actMap[hoveredIdx].type);
        int dSz = 24;
        DrawText(dsc,
            (int)(mapCentreX - MeasureText(dsc, dSz) / 2.f),
            (int)(sh - 90.f), dSz, Color{206, 242, 255, 230});
    }

    // -- Footer hint -------------------------------------------------------
    const char* hint = _touchModeActive
        ? "Tap a highlighted node to enter"
        : "Click node  or  A/D  to select  -  Enter / Space  to confirm";
    int ftSz = (int)_mapHintFs;
    DrawText(hint,
        (int)(mapCentreX - MeasureText(hint, ftSz) / 2.f),
        (int)(sh - _mapHintY), ftSz, Color{173, 223, 236, 185});

    // -- Map right-panel debug editor --------------------------------------
#if MO_DEV_TOOLS
    if (IsKeyPressed(KEY_NINE))
        _mapEditorActive = !_mapEditorActive;
#endif

    if (_mapEditorActive)
    {
        constexpr int kN = 19;
        const char* varNames[kN] = {
            "0  Journey Panel X",
            "1  Padding",
            "2  Title Font",
            "3  Rooms/Gold Font",
            "4  No Rooms Font",
            "5  Biome Title Font",
            "6  Diamond Label Fs",
            "7  Square Size",
            "8  Square Gap",
            "9  Icon Size",
            "10 Diamond Size",
            "11 Act X",
            "12 Act Y",
            "13 Act Font",
            "14 Path X",
            "15 Path Y",
            "16 Path Font",
            "17 Hint Y",
            "18 Hint Font",
        };
        float* vars[kN] = {
            &_mapJourneyX, &_mapPad, &_mapTitleFs, &_mapRoomsFs, &_mapNoRoomsFs,
            &_mapBiomeTitleFs, &_mapBiomeLabelFs, &_mapSqSz, &_mapSqGap, &_mapIconSz,
            &_mapDiamondSz, &_mapHeaderX, &_mapHeaderY, &_mapHeaderFs,
            &_mapSubX, &_mapSubY, &_mapSubFs, &_mapHintY, &_mapHintFs
        };

        if (IsKeyPressed(KEY_UP))
            _mapEditorSelIdx = (_mapEditorSelIdx - 1 + kN) % kN;
        if (IsKeyPressed(KEY_DOWN))
            _mapEditorSelIdx = (_mapEditorSelIdx + 1) % kN;

        // Fractional X/font fraction controls use finer steps.
        float step = (_mapEditorSelIdx == 0 || _mapEditorSelIdx == 6
                   || _mapEditorSelIdx == 11 || _mapEditorSelIdx == 14) ? 0.005f : 1.f;
        if (IsKeyDown(KEY_RIGHT)) *vars[_mapEditorSelIdx] += step;
        if (IsKeyDown(KEY_LEFT))  *vars[_mapEditorSelIdx] -= step;

        if (IsKeyPressed(KEY_S))
        {
            TraceLog(LOG_INFO, "=== Map Right-Panel Editor Export ===");
            TraceLog(LOG_INFO, "_mapJourneyX     = %.3ff;", _mapJourneyX);
            TraceLog(LOG_INFO, "_mapPad          = %.2ff;", _mapPad);
            TraceLog(LOG_INFO, "_mapTitleFs      = %.2ff;", _mapTitleFs);
            TraceLog(LOG_INFO, "_mapRoomsFs      = %.2ff;", _mapRoomsFs);
            TraceLog(LOG_INFO, "_mapNoRoomsFs    = %.2ff;", _mapNoRoomsFs);
            TraceLog(LOG_INFO, "_mapBiomeTitleFs = %.2ff;", _mapBiomeTitleFs);
            TraceLog(LOG_INFO, "_mapBiomeLabelFs = %.3ff;", _mapBiomeLabelFs);
            TraceLog(LOG_INFO, "_mapSqSz         = %.2ff;", _mapSqSz);
            TraceLog(LOG_INFO, "_mapSqGap        = %.2ff;", _mapSqGap);
            TraceLog(LOG_INFO, "_mapIconSz       = %.2ff;", _mapIconSz);
            TraceLog(LOG_INFO, "_mapDiamondSz    = %.2ff;", _mapDiamondSz);
            TraceLog(LOG_INFO, "_mapHeaderX      = %.3ff;", _mapHeaderX);
            TraceLog(LOG_INFO, "_mapHeaderY      = %.2ff;", _mapHeaderY);
            TraceLog(LOG_INFO, "_mapHeaderFs     = %.2ff;", _mapHeaderFs);
            TraceLog(LOG_INFO, "_mapSubX         = %.3ff;", _mapSubX);
            TraceLog(LOG_INFO, "_mapSubY         = %.2ff;", _mapSubY);
            TraceLog(LOG_INFO, "_mapSubFs        = %.2ff;", _mapSubFs);
            TraceLog(LOG_INFO, "_mapHintY        = %.2ff;", _mapHintY);
            TraceLog(LOG_INFO, "_mapHintFs       = %.2ff;", _mapHintFs);
        }

        // -- Panel draw ----------------------------------------------------
        constexpr float rowH = 30.f;
        constexpr float panW = 290.f;
        const float panH = 40.f + kN * rowH;
        const float panX = 10.f;
        const float panY = 10.f;

        DrawRectangle((int)panX, (int)panY, (int)panW, (int)panH, Fade(BLACK, 0.82f));
        DrawRectangleLines((int)panX, (int)panY, (int)panW, (int)panH, DARKGRAY);
        DrawText("MAP EDITOR  [9] close", (int)(panX + 8.f), (int)(panY + 6.f), 11, GRAY);
        DrawText("[UP/DOWN] sel  [L/R] nudge  [S] export",
            (int)(panX + 8.f), (int)(panY + 20.f), 10, DARKGRAY);

        for (int i = 0; i < kN; i++)
        {
            float ry  = panY + 38.f + i * rowH;
            bool  sel = (i == _mapEditorSelIdx);
            Color col = sel ? YELLOW : WHITE;

            if (sel)
                DrawText("->", (int)(panX + 4.f), (int)(ry + rowH * 0.5f - 7.f), 13, YELLOW);
            DrawText(varNames[i], (int)(panX + 26.f), (int)(ry + rowH * 0.5f - 7.f), 13, col);

            const char* valStr = TextFormat("%.3f", *vars[i]);
            int valW = MeasureText(valStr, 13);
            DrawText(valStr, (int)(panX + panW - valW - 6.f),
                (int)(ry + rowH * 0.5f - 7.f), 13, col);
        }
    }
}

void Engine::DrawStartingAbilityChoice()
{
    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.68f));

    const char* title = "Zeph: Choose your starting ability";
    int titleSz = 44;
    DrawText(title, (int)(sw * 0.5f - MeasureText(title, titleSz) * 0.5f), (int)(sh * 0.055f), titleSz, GOLD);

    const char* quote = "The dungeon is unforgiving. Choose the magic that suits you best.";
    int quoteSz = 24;
    DrawText(quote, (int)(sw * 0.5f - MeasureText(quote, quoteSz) * 0.5f), (int)(sh * 0.115f), quoteSz, Fade(RAYWHITE, 0.70f));

    auto abilityInfo = [&](UpgradeType type, const char*& name, const char*& desc, Texture2D*& icon, Color& elementColor)
    {
        switch (type)
        {
        case UpgradeType::LearnFireSpread:
            name = "Flame Wall"; desc = "Place a burning wall\nthat controls space"; icon = &_abilityIconFireTex; elementColor = Color{255, 110, 20, 255}; break;
        case UpgradeType::LearnIceSpread:
            name = "Frost Nova"; desc = "Defensive frost burst\nslows and freezes"; icon = &_abilityIconIceTex; elementColor = Color{100, 210, 255, 255}; break;
        case UpgradeType::LearnElectricSpread:
            name = "Lightning Blink"; desc = "Aim a damaging blink\nshock at arrival"; icon = &_abilityIconElectricTex; elementColor = Color{255, 220, 30, 255}; break;
        case UpgradeType::LearnFireBolt:
            name = "Fireball"; desc = "Aimed projectile\nexplodes and burns"; icon = &_abilityIconFireTex; elementColor = Color{255, 110, 20, 255}; break;
        case UpgradeType::LearnIceBolt:
            name = "Ice Lance"; desc = "Pierces enemies\nshatters chilled foes"; icon = &_abilityIconIceTex; elementColor = Color{100, 210, 255, 255}; break;
        case UpgradeType::LearnElectricBolt:
            name = "Chain Lightning"; desc = "Aimed first strike\nchains through foes"; icon = &_abilityIconElectricTex; elementColor = Color{255, 220, 30, 255}; break;
        default:
        {
            // Class abilities (Warrior kit etc.): pull name/desc from the shared
            // ability metadata via the learn/upgrade ⇄ ability mapping.
            AbilityType ab = AbilityForLearnType(type);
            if (ab == AbilityType::None) ab = AbilityForUpgradeType(type);
            if (ab != AbilityType::None)
            {
                name = GetAbilityName(ab);
                desc = GetAbilityDesc(ab);
                icon = GetAbilityIcon(ab);
                elementColor = Color{ 230, 120, 60, 255 };   // warrior steel-orange
            }
            else
            {
                name = "Ability"; desc = ""; icon = nullptr; elementColor = WHITE;
            }
            break;
        }
        }
    };

    auto drawMultilineCentered = [](const char* text, float centerX, float startY, int fontSize, Color color)
    {
        std::string s = text;
        std::size_t start = 0;
        int line = 0;
        while (start <= s.size())
        {
            std::size_t end = s.find('\n', start);
            std::string part = (end == std::string::npos) ? s.substr(start) : s.substr(start, end - start);
            int w = MeasureText(part.c_str(), fontSize);
            DrawText(part.c_str(), (int)(centerX - w * 0.5f), (int)(startY + line * (fontSize + 5)), fontSize, color);
            if (end == std::string::npos) break;
            start = end + 1;
            line++;
        }
    };

    Vector2 mouse = GetVirtualMousePos();
    bool ready = (_levelUpOpenTimer <= 0.f);
    const float cardW = 260.f;
    const float cardH = 275.f;
    const float gapX = 34.f;
    const float gapY = 26.f;
    const float startX = sw * 0.5f - (cardW * 3.f + gapX * 2.f) * 0.5f;
    const float startY = sh * 0.18f;

    for (int i = 0; i < 3; i++)
    {
        Rectangle card{ startX + i * (cardW + gapX), startY, cardW, cardH };
        bool selected = _startingAbilitySelected[i];
        bool hovered = (ready && CheckCollisionPointRec(mouse, card)) ||
                       (_gamepad.isActive && i == _levelUpGpCursor);

        Color bg = selected ? Color{45, 36, 10, 235} : (hovered ? Color{42, 42, 50, 230} : Color{22, 22, 25, 218});
        Color border = selected ? GOLD : (hovered ? Color{200, 200, 200, 220} : Color{120, 120, 120, 130});
        DrawRectangleRounded(card, 0.12f, 8, bg);
        DrawRectangleRoundedLines(card, 0.12f, 8, border);

        const char* name = "";
        const char* desc = "";
        Texture2D* icon = nullptr;
        Color elementColor = WHITE;
        abilityInfo(_startingAbilityOptions[i], name, desc, icon, elementColor);

        Vector2 center{ card.x + card.width * 0.5f, card.y + 78.f };
        bool isBolt = _startingAbilityOptions[i] == UpgradeType::LearnFireBolt ||
                      _startingAbilityOptions[i] == UpgradeType::LearnIceBolt ||
                      _startingAbilityOptions[i] == UpgradeType::LearnElectricBolt;
        if (isBolt)
        {
            DrawLineEx({ center.x - 50.f, center.y }, { center.x + 48.f, center.y }, 4.f, elementColor);
            DrawTriangle({ center.x + 62.f, center.y }, { center.x + 42.f, center.y - 14.f }, { center.x + 42.f, center.y + 14.f }, elementColor);
        }
        else
        {
            const float angles[8] = { 0.f, 45.f, 90.f, 135.f, 180.f, 225.f, 270.f, 315.f };
            for (float deg : angles)
            {
                float r = deg * DEG2RAD;
                Vector2 a{ center.x + cosf(r) * 20.f, center.y + sinf(r) * 20.f };
                Vector2 b{ center.x + cosf(r) * 58.f, center.y + sinf(r) * 58.f };
                DrawLineEx(a, b, 2.2f, elementColor);
            }
        }
        if (icon && icon->id != 0)
        {
            float target = (icon == &_abilityIconFireTex) ? 42.f : 62.f;
            float scale = std::min(target / (float)icon->width, target / (float)icon->height);
            DrawTextureEx(*icon, { center.x - icon->width * scale * 0.5f, center.y - icon->height * scale * 0.5f }, 0.f, scale, WHITE);
        }

        int nameSz = 25;
        DrawText(name, (int)(card.x + card.width * 0.5f - MeasureText(name, nameSz) * 0.5f), (int)(card.y + 150.f), nameSz, selected ? GOLD : RAYWHITE);
        drawMultilineCentered(desc, card.x + card.width * 0.5f, card.y + 192.f, 19, LIGHTGRAY);

        if (selected)
        {
            const char* mark = "SELECTED";
            int fs = 17;
            DrawText(mark, (int)(card.x + card.width * 0.5f - MeasureText(mark, fs) * 0.5f), (int)(card.y + card.height - 30.f), fs, GOLD);
        }

        bool startCardActivated = (ready && hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) ||
                                   (_gamepad.isActive && ready && i == _levelUpGpCursor && _gamepad.menuConfirmPressed);
        if (startCardActivated)
        {
            if (_startingAbilitySelected[i])
            {
                _startingAbilitySelected[i] = false;
                _startingAbilityPickCount--;
            }
            else if (_startingAbilityPickCount < 1)
            {
                _startingAbilitySelected[i] = true;
                _startingAbilityPickCount++;
            }
            else
            {
                // Switch selection to the newly activated card
                for (int j = 0; j < 3; j++) _startingAbilitySelected[j] = false;
                _startingAbilitySelected[i] = true;
            }
        }
    }

    Rectangle confirm{ sw * 0.5f - 150.f, sh - 104.f, 300.f, 58.f };
    bool canConfirm = (_startingAbilityPickCount == 1);
    bool confirmHover = canConfirm && ready && CheckCollisionPointRec(mouse, confirm);
    DrawRectangleRounded(confirm, 0.25f, 8, canConfirm ? (confirmHover ? Color{210, 165, 45, 255} : GOLD) : Color{80, 80, 80, 210});
    const char* cText = canConfirm ? "Start with this ability" : "Choose 1 ability";
    int cFs = 26;
    DrawText(cText, (int)(confirm.x + confirm.width * 0.5f - MeasureText(cText, cFs) * 0.5f), (int)(confirm.y + confirm.height * 0.5f - cFs * 0.5f), cFs, canConfirm ? BLACK : LIGHTGRAY);

    // Gamepad: pressing A on an already-selected card confirms (no need to navigate to the button)
    bool gpConfirmStarting = _gamepad.isActive && ready && canConfirm &&
                             _gamepad.menuConfirmPressed && _startingAbilitySelected[_levelUpGpCursor];
    if ((canConfirm && ready && confirmHover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) || gpConfirmStarting)
    {
        for (int i = 0; i < 3; i++)
            if (_startingAbilitySelected[i])
                _player.ApplyUpgrade(_startingAbilityOptions[i]);

        _starterAbilityGiftClaimed = true;
        _awaitingStartingAbility = false;
        _startingAbilityPickCount = 0;

        if (_cutscene.IsActive() && _cutscene.WantsAbilitySelect())
            _cutscene.OnAbilitySelected();

        if (_currentRoomType == RoomType::Store)
            SetStoreDoorTiles(TileType::DoorOpen);

        _gameState = _levelUpReturnState;
    }
}
void Engine::DrawLevelUpChoice()
{
    if (_awaitingStartingAbility)
    {
        DrawStartingAbilityChoice();
        return;
    }
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    // Dim overlay
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.65f));

    // Determine which rows are still visible
    bool showUlt = _showUltimateRow && !_ultimateRowPicked && !_awaitingStartingAbility;
    bool showReg = !_regularRowPicked;

    // Title
    const char* title;
    if (_awaitingStartingAbility)
        title = "Choose your starting ability:";
    else if (_levelUpOfferContext == LevelUpOfferContext::TreasureChest)
        title = "Treasure Chest  -  Choose your reward:";
    else if (_levelUpOfferContext == LevelUpOfferContext::TreasureBasic)
        title = "Treasure Room  -  Choose a basic upgrade:";
    else if (_levelUpOfferContext == LevelUpOfferContext::EliteReward)
        title = "Elite Reward  -  Choose a rarer upgrade:";
    else if (_levelUpOfferContext == LevelUpOfferContext::StoreStock)
        title = "Shop Stock  -  Choose one item:";
    else if (showUlt && showReg)
        title = "LEVEL UP!  Choose an Ultimate AND an Upgrade:";
    else if (showUlt)
        title = "Now choose your Ultimate:";
    else if (showReg)
        title = "POWER CHOICE  -  Shape your build:";
    else
        title = "LEVEL UP!";

    int titleSz = 44;
    int titleW  = MeasureText(title, titleSz);
    DrawText(title, (int)(sw / 2.f - titleW / 2.f), (int)(sh * 0.06f), titleSz, GOLD);

    // Card layout
    const float cardW   = 280.f;
    const float cardH   = 360.f;
    const float cardGap = 40.f;
    const float totalW  = 3.f * cardW + 2.f * cardGap;
    const float startX  = sw / 2.f - totalW / 2.f;

    // Row Y positions - stack both rows when both visible, otherwise center the lone row
    float ultRowY, regRowY;
    if (showUlt && showReg)
    {
        float totalH = cardH * 2.f + 50.f;
        float topY   = sh / 2.f - totalH / 2.f;
        ultRowY = topY;
        regRowY = topY + cardH + 50.f;
    }
    else
    {
        ultRowY = sh / 2.f - cardH / 2.f;
        regRowY = sh / 2.f - cardH / 2.f;
    }

    // Use cardY to keep the rest of the code below working for the regular row
    const float cardY = regRowY;

    auto roundedStat = [](float value) -> int
    {
        return (int)std::ceil(value);
    };
    auto pctString = [&](float value) -> std::string
    {
        return std::to_string(roundedStat(value * 100.f)) + "%";
    };
    auto float1String = [](float value) -> std::string
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        return std::string(buffer);
    };
    auto linePreview = [&](const char* label, float currentValue, float newValue, const char* suffix = "") -> std::string
    {
        return std::string(label) + " "
            + std::to_string(roundedStat(currentValue)) + suffix
            + " -> "
            + std::to_string(roundedStat(newValue)) + suffix;
    };
    auto getUpgradeInfo = [&](UpgradeType type, const char*& name, std::string& desc, Texture2D*& icon)
    {
        desc.clear();
        switch (type)
        {
        case UpgradeType::AttackPower:
            name = "Attack Power";
            desc = linePreview("Atk", _player.GetAttackPowerValue(), _player.GetAttackPowerValue() + 0.5f);
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::AttackRange:
            name = "Attack Range";
            desc = "Range " + float1String(_player.GetAttackRangeMultiplierValue()) + "x -> "
                + float1String(_player.GetAttackRangeMultiplierValue() + 0.08f) + "x";
            icon = &_upgradeAttackRangeTex;
            break;
        case UpgradeType::MaxHealth:
            name = "Max Health";
            desc = linePreview("HP", _player.GetMaxHealthValue(), _player.GetMaxHealthValue() + 2.f)
                + "\nDoes not heal";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::MaxMana:
            name = "Max Mana";
            desc = linePreview("Mana", (float)_player.GetMaxMana(), (float)(_player.GetMaxMana() + 4))
                + "\nDoes not refill";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::Defense:
            name = "Armour";
            desc = "Armour " + std::to_string(_player.GetArmour()) + " -> "
                + std::to_string(std::min(_player.GetArmour() + 1, _player.GetMaxArmour()))
                + " / " + std::to_string(_player.GetMaxArmour())
                + "\nAbsorbs 1 hit";
            icon = &_upgradeDefenseTex;
            break;
        case UpgradeType::MoveSpeed:
            name = "Move Speed";
            desc = linePreview("Speed", _player.GetMoveSpeedValue(), _player.GetMoveSpeedValue() + 12.f);
            icon = &_upgradeMoveSpeedTex;
            break;
        // -- Rare --------------------------------------------------------------
        case UpgradeType::IronConstitution:
            name = "Iron Constitution";
            desc = linePreview("HP", _player.GetMaxHealthValue(),
                std::ceil(_player.GetMaxHealthValue() * 1.15f))
                + "\nHeals the gained HP";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::SwiftFeet:
            name = "Swift Feet";
            desc = linePreview("Speed", _player.GetMoveSpeedValue(), _player.GetMoveSpeedValue() * 1.08f);
            icon = &_upgradeMoveSpeedTex;
            break;
        case UpgradeType::Ferocity:
            name = "Ferocity";
            desc = linePreview("Atk", _player.GetAttackPowerValue(),
                _player.GetAttackPowerValue() + std::max(1.0f, _player.GetAttackPowerValue() * 0.10f));
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::ArcaneMind:
            name = "Arcane Mind";
            desc = linePreview("Mana", (float)_player.GetMaxMana(), (float)(_player.GetMaxMana() + 8))
                + "\nRegen +10%";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::IronSkin:
            name = "Iron Skin";
            desc = "Armour " + std::to_string(_player.GetArmour()) + " -> "
                + std::to_string(std::min(_player.GetArmour() + 2, _player.GetMaxArmour()))
                + " / " + std::to_string(_player.GetMaxArmour())
                + "\nAbsorbs 2 hits";
            icon = &_upgradeDefenseTex;
            break;
        case UpgradeType::BladeEdge:
            name = "Blade Edge";
            desc = linePreview("Atk", _player.GetAttackPowerValue(), _player.GetAttackPowerValue() + 1.f)
                + "\nRange " + float1String(_player.GetAttackRangeMultiplierValue()) + "x -> "
                + float1String(_player.GetAttackRangeMultiplierValue() + 0.10f) + "x";
            icon = &_upgradeAttackRangeTex;
            break;
        // -- Epic --------------------------------------------------------------
        case UpgradeType::WarGod:
            name = "War God";
            desc = linePreview("Atk", _player.GetAttackPowerValue(),
                _player.GetAttackPowerValue() + std::max(1.75f, _player.GetAttackPowerValue() * 0.15f))
                + "\nRange " + float1String(_player.GetAttackRangeMultiplierValue()) + "x -> "
                + float1String(_player.GetAttackRangeMultiplierValue() + 0.12f) + "x";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::Resilience:
            name = "Resilience";
            desc = linePreview("HP", _player.GetMaxHealthValue(),
                std::ceil(_player.GetMaxHealthValue() * 1.18f))
                + "\nHeals the gained HP";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::BladeStorm:
            name = "Blade Storm";
            desc = linePreview("Atk", _player.GetAttackPowerValue(), _player.GetAttackPowerValue() + 1.5f)
                + "\nSpeed " + std::to_string(roundedStat(_player.GetMoveSpeedValue())) + " -> "
                + std::to_string(roundedStat(_player.GetMoveSpeedValue() * 1.08f));
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::Juggernaut:
            name = "Juggernaut";
            desc = linePreview("HP", _player.GetMaxHealthValue(),
                std::ceil(_player.GetMaxHealthValue() * 1.12f))
                + "\nArmour " + std::to_string(_player.GetArmour()) + " -> "
                + std::to_string(std::min(_player.GetArmour() + 2, _player.GetMaxArmour()))
                + " / " + std::to_string(_player.GetMaxArmour());
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::ArcaneColossus:
            name = "Arcane Colossus";
            desc = linePreview("Mana", (float)_player.GetMaxMana(), (float)(_player.GetMaxMana() + 10))
                + "\nAtk " + std::to_string(roundedStat(_player.GetAttackPowerValue()))
                + " -> " + std::to_string(roundedStat(_player.GetAttackPowerValue() + 1.5f))
                + "\nRegen +15%";
            icon = &_upgradeMagicTex;
            break;
        // -- Power Choice additions ---------------------------------------------
        case UpgradeType::ManaFlow:
            name = "Mana Flow";
            desc = linePreview("Mana", (float)_player.GetMaxMana(), (float)(_player.GetMaxMana() + 1))
                + "\nRegen +15%";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::ClassAttunement:
        {
            name = "Class Attunement";
            const char* resourceName = "Resource";
            switch (_player.GetClass())
            {
            case PlayerClass::Warrior: resourceName = "Rage";         break;
            case PlayerClass::Paladin: resourceName = "Faith";        break;
            case PlayerClass::Rogue:   resourceName = "Combo Points"; break;
            default: break;
            }
            desc = std::string(resourceName) + " gain +25%";
            icon = &_upgradeMagicTex;
            break;
        }
        case UpgradeType::Overload:
            name = "Overload";
            desc = "Abilities +15% damage\nBUT casts cost +1 mana";
            icon = &_upgradeAttackPowerTex;
            break;
        // -- Class-specific Power Choice cards --------------------------------
        case UpgradeType::MagePyromancy:
            name = "Pyromancy";
            desc = "Fire spells deal\n+20% damage";
            icon = &_abilityIconFireTex;
            break;
        case UpgradeType::MageInfernalMastery:
            name = "Infernal Mastery";
            desc = "Fire spells deal\n+35% damage";
            icon = &_abilityIconFireTex;
            break;
        case UpgradeType::MageCryomancy:
            name = "Cryomancy";
            desc = "Ice spells deal\n+20% damage";
            icon = &_abilityIconIceTex;
            break;
        case UpgradeType::MageGlacialMastery:
            name = "Glacial Mastery";
            desc = "Ice spells deal\n+35% damage";
            icon = &_abilityIconIceTex;
            break;
        case UpgradeType::MageStormAttunement:
            name = "Storm Attunement";
            desc = "Electric spells deal\n+20% damage";
            icon = &_abilityIconElectricTex;
            break;
        case UpgradeType::MageTempestMastery:
            name = "Tempest Mastery";
            desc = "Electric spells deal\n+35% damage";
            icon = &_abilityIconElectricTex;
            break;
        case UpgradeType::MageComboResonance:
            name = "Combo Resonance";
            desc = "Elemental combo reactions\ndeal DOUBLE bonus damage";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::MageArcaneHaste:
            name = "Arcane Haste";
            desc = "Mana regen +20%";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::WarriorSmolderingFury:
            name = "Smoldering Fury";
            desc = "Rage decays 50% slower\nout of combat";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::WarriorFuriousMight:
            name = "Furious Might";
            desc = "Full-Rage damage bonus\n+50% -> +70%";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::WarriorBattleTrance:
            name = "Battle Trance";
            desc = "Rage gain +25% AND\nfull-Rage bonus +10%";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::WarriorUnbreakable:
            name = "Unbreakable";
            desc = "+1 armour, +1 max HP";
            icon = &_upgradeDefenseTex;
            break;
        case UpgradeType::WarriorColossus:
            name = "Colossus";
            desc = "+2 max HP, +0.5 attack";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::WarriorWarlordsReach:
            name = "Warlord's Reach";
            desc = "Attack range +0.10x";
            icon = &_upgradeAttackRangeTex;
            break;
        case UpgradeType::WarriorBattleMeditation:
            name = "Battle Meditation";
            desc = "Heal 1 HP when a\nroom is cleared";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::WarriorWeaponMaster:
            name = "Weapon Master";
            desc = "Class abilities +8% damage";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::HunterPredatorsRhythm:
            name = "Predator's Rhythm";
            desc = "Every 2nd shot marks prey\n(instead of every 3rd)";
            icon = &_upgradeAttackRangeTex;
            break;
        case UpgradeType::HunterQuarry:
            name = "Hunter's Quarry";
            desc = "Marked enemies take\n+15% more damage";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::HunterApexPredator:
            name = "Apex Predator";
            desc = "Marked enemies take\n+30% more damage";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::HunterFletcher:
            name = "Fletcher";
            desc = "+0.5 attack power";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::HunterTrappersCunning:
            name = "Trapper's Cunning";
            desc = "Class abilities +10% damage";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::HunterSwiftQuiver:
            name = "Swift Quiver";
            desc = "+12 move speed";
            icon = &_upgradeMoveSpeedTex;
            break;
        case UpgradeType::HunterSurvivalist:
            name = "Survivalist";
            desc = "+1 max HP, +1 armour";
            icon = &_upgradeDefenseTex;
            break;
        case UpgradeType::HunterFocusedBreathing:
            name = "Focused Breathing";
            desc = "Mana regen +15%";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::RogueDeepReserves:
            name = "Deep Reserves";
            desc = "+1 max combo point\n(up to 7)";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::RogueRuthlessFinisher:
            name = "Ruthless Finisher";
            desc = "Eviscerate +5% damage\nper combo point";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::RogueExsanguinate:
            name = "Exsanguinate";
            desc = "Eviscerate +10% damage\nper combo point";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::RogueToxinExpert:
            name = "Toxin Expert";
            desc = "Poison ticks +50% damage";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::RogueMasterPoisoner:
            name = "Master Poisoner";
            desc = "Poison ticks +100% damage";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::RogueFleetFootwork:
            name = "Fleet Footwork";
            desc = "+12 move speed";
            icon = &_upgradeMoveSpeedTex;
            break;
        case UpgradeType::RogueShadowConditioning:
            name = "Shadow Conditioning";
            desc = "+1 max HP";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::RogueOpportunist:
            name = "Opportunist";
            desc = "Combo gain +25% AND\nEviscerate +5%/point";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::PaladinHolyMight:
            name = "Holy Might";
            desc = "Holy abilities +10% damage";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::PaladinDivineWrath:
            name = "Divine Wrath";
            desc = "Holy abilities +18% damage";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::PaladinZealotsFury:
            name = "Zealot's Fury";
            desc = "Retribution +6% damage\nper stack (base 12%)";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::PaladinMirroredAegis:
            name = "Mirrored Aegis";
            desc = "Reflected damage\n50% stronger";
            icon = &_upgradeDefenseTex;
            break;
        case UpgradeType::PaladinDevotion:
            name = "Devotion";
            desc = "Faith gain +15%";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::PaladinCrusadersVitality:
            name = "Crusader's Vitality";
            desc = "+2 max HP";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::PaladinSanctuary:
            name = "Sanctuary";
            desc = "+2 armour AND heal 1 HP\non room clear";
            icon = &_upgradeDefenseTex;
            break;
        case UpgradeType::PaladinDivineConduit:
            name = "Divine Conduit";
            desc = "+2 max mana,\nmana regen +15%";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::WarlockGrimHarvest:
            name = "Grim Harvest";
            desc = "Lifesteal 50% stronger";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::WarlockDarkPact:
            name = "Dark Pact";
            desc = "Cursed enemies take\n+10% more damage";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::WarlockSoulBargain:
            name = "Soul Bargain";
            desc = "Cursed enemies take\n+25% more damage";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::WarlockLingeringMalice:
            name = "Lingering Malice";
            desc = "Curses last +50% longer";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::WarlockVoidAttunement:
            name = "Void Attunement";
            desc = "Mana regen +20%";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::WarlockCorruptedVitality:
            name = "Corrupted Vitality";
            desc = "+2 max HP";
            icon = &_upgradeHealthTex;
            break;
        case UpgradeType::WarlockSoulConduit:
            name = "Soul Conduit";
            desc = "Lifesteal +50% AND\ncurses +25% longer";
            icon = &_upgradeMagicTex;
            break;
        case UpgradeType::WarlockOccultPower:
            name = "Occult Power";
            desc = "Class abilities +8% damage";
            icon = &_upgradeAttackPowerTex;
            break;
        case UpgradeType::LearnFireSpread:
            name = "Flame Wall";
            desc = "Place a burning wall\nthat controls space";
            icon = &_abilityIconFireTex;
            break;
        case UpgradeType::LearnIceSpread:
            name = "Frost Nova";
            desc = "Defensive frost burst\nslows and freezes";
            icon = &_abilityIconIceTex;
            break;
        case UpgradeType::LearnElectricSpread:
            name = "Lightning Blink";
            desc = "Aim a damaging blink\nshock at arrival";
            icon = &_abilityIconElectricTex;
            break;
        case UpgradeType::LearnFireBolt:
            name = "Fireball";
            desc = "Aimed projectile\nexplodes and burns";
            icon = &_abilityIconFireTex;
            break;
        case UpgradeType::LearnIceBolt:
            name = "Ice Lance";
            desc = "Pierces enemies\nshatters chilled foes";
            icon = &_abilityIconIceTex;
            break;
        case UpgradeType::LearnElectricBolt:
            name = "Chain Lightning";
            desc = "Aimed first strike\nchains through foes";
            icon = &_abilityIconElectricTex;
            break;
        case UpgradeType::LearnFireUltimate:
            name = "Meteor";
            desc = "Targeted impact leaves\nburning ground, all MP";
            icon = &_abilityIconFireTex;
            break;
        case UpgradeType::LearnIceUltimate:
            name = "Blizzard";
            desc = "Targeted storm slows\nand freezes, all MP";
            icon = &_abilityIconIceTex;
            break;
        case UpgradeType::LearnElectricUltimate:
            name = "Thunderstorm";
            desc = "Forward-moving storm\nstrikes enemies, all MP";
            icon = &_abilityIconElectricTex;
            break;
        default:
        {
            // Learned ability upgrades show their real name and level change.
            AbilityType ab = AbilityForLearnType(type);
            if (ab == AbilityType::None) ab = AbilityForUpgradeType(type);
            if (ab != AbilityType::None)
            {
                name = GetAbilityName(ab);
                if (AbilityForUpgradeType(type) != AbilityType::None)
                {
                    int currentLevel = _player.GetAbilityLevel(ab);
                    desc = "Level " + std::to_string(currentLevel) + " -> "
                        + std::to_string(std::min(3, currentLevel + 1))
                        + "\nPower and effects improve";
                }
                else
                    desc = GetAbilityDesc(ab);
                icon = GetAbilityIcon(ab);
                if (!icon) icon = &_upgradeAttackPowerTex;
            }
            else
            {
                name = "???";
                desc = "";
                icon = &_upgradeAttackPowerTex;
            }
            break;
        }
        }
    };

    auto drawMultilineCentered = [&](const std::string& text, float centerX, float startY, int fontSize, Color color)
    {
        std::size_t start = 0;
        int lineIndex = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            std::string line = (end == std::string::npos) ? text.substr(start) : text.substr(start, end - start);
            int lineW = MeasureText(line.c_str(), fontSize);
            DrawText(line.c_str(), (int)(centerX - lineW / 2.f), (int)(startY + lineIndex * (fontSize + 4)), fontSize, color);
            if (end == std::string::npos)
                break;
            start = end + 1;
            lineIndex++;
        }
    };
    Vector2 mouse = GetVirtualMousePos();
    bool ready = (_levelUpOpenTimer <= 0.f);

    // -- Ultimate row (level 3 only, top) -------------------------------------
    if (showUlt)
    {
        // Row label
        int lblSz = 22;
        const char* lbl = "- Elemental Ultimate -";
        DrawText(lbl, (int)(sw / 2.f - MeasureText(lbl, lblSz) / 2.f),
                 (int)(ultRowY - lblSz - 8.f), lblSz, Color{255,200,80,200});

        for (int i = 0; i < 3; i++)
        {
            float x = startX + i * (cardW + cardGap);
            Rectangle card{ x, ultRowY, cardW, cardH };
            bool hovered = (ready && CheckCollisionPointRec(mouse, card)) ||
                           (_gamepad.isActive && _levelUpGpRow == 0 && i == _levelUpGpCursor);

            // Gold-tinted card background to distinguish from regular row
            Color bgColor = hovered ? Color{55, 40, 10, 230} : Color{35, 25, 5, 215};
            DrawRectangleRounded(card, 0.12f, 8, bgColor);
            DrawRectangleRoundedLines(card, 0.12f, 8,
                hovered ? Color{255,180,0,255} : Color{200,140,30,160});

            const char* name = ""; std::string desc; Texture2D* icon = nullptr;
            getUpgradeInfo(_levelUpUltimateOptions[i], name, desc, icon);

            // Icon / pattern area
            {
                const float iconAreaSize = cardW * 0.52f;
                const float previewCX    = x + cardW / 2.f;
                const float previewCY    = ultRowY + cardH * 0.10f + iconAreaSize / 2.f;

                UpgradeType opt = _levelUpUltimateOptions[i];
                Color ec =
                    (opt == UpgradeType::LearnFireUltimate)     ? Color{255,110, 20,255} :
                    (opt == UpgradeType::LearnIceUltimate)      ? Color{100,210,255,255} :
                                                                  Color{255,220, 30,255};
                Color ecDim = {ec.r, ec.g, ec.b, 80};
                const float outerR = iconAreaSize * 0.44f;
                DrawCircleLinesV({previewCX, previewCY}, outerR, ecDim);
                const float dotAngles[10] = {0.f,36.f,72.f,108.f,144.f,180.f,216.f,252.f,288.f,324.f};
                const float dotDists[10]  = {0.55f,0.80f,0.65f,0.90f,0.50f,0.75f,0.85f,0.60f,0.95f,0.70f};
                for (int d = 0; d < 10; d++)
                {
                    float rad  = dotAngles[d] * DEG2RAD;
                    float dist = outerR * dotDists[d];
                    Vector2 dp = {previewCX + cosf(rad)*dist, previewCY + sinf(rad)*dist};
                    DrawCircleV(dp, 5.f, ecDim);
                    DrawCircleV(dp, 3.f, ec);
                }
                if (icon && icon->id != 0)
                {
                    float iconTarget = (opt == UpgradeType::LearnFireUltimate) ? 44.f : 72.f;
                    float iconScale  = std::min(iconTarget/(float)icon->width, iconTarget/(float)icon->height);
                    float iw = icon->width * iconScale; float ih = icon->height * iconScale;
                    DrawTextureEx(*icon, {previewCX - iw*0.5f, previewCY - ih*0.5f}, 0.f, iconScale, WHITE);
                }
            }

            // Name
            int nameSz = 26;
            int nameW  = MeasureText(name, nameSz);
            DrawText(name, (int)(x + cardW/2.f - nameW/2.f),
                     (int)(ultRowY + cardH*0.58f), nameSz, hovered ? GOLD : RAYWHITE);

            drawMultilineCentered(desc, x + cardW / 2.f, ultRowY + cardH * 0.72f, 20, LIGHTGRAY);

            bool ultActivated = (ready && hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) ||
                                 (_gamepad.isActive && ready && _levelUpGpRow == 0 && i == _levelUpGpCursor && _gamepad.menuConfirmPressed);
            if (ultActivated)
            {
                _player.ApplyUpgrade(_levelUpUltimateOptions[i]);
                _ultimateRowPicked = true;
                if (_regularRowPicked)
                {
                    if (_levelUpReturnState == GameState::Map) _mapOpenTimer = 0.4f;
                    _gameState = _levelUpReturnState;
                }
            }
        }
    }

    // -- Regular row ----------------------------------------------------------
    if (showReg)
    {
        // Row label when both rows are visible
        if (showUlt)
        {
            int lblSz = 22;
            const char* lbl = "- Upgrade -";
            DrawText(lbl, (int)(sw / 2.f - MeasureText(lbl, lblSz) / 2.f),
                     (int)(regRowY - lblSz - 8.f), lblSz, Color{200,200,200,180});
        }

    for (int i = 0; i < 3; i++)
    {
        float x = startX + i * (cardW + cardGap);
        Rectangle card{ x, cardY, cardW, cardH };

        bool hovered = (ready && CheckCollisionPointRec(mouse, card)) ||
                       (_gamepad.isActive && _levelUpGpRow == 1 && i == _levelUpGpCursor);

        // Rarity-based colors
        UpgradeRarity rarity = _player.GetUpgradeRarity(_levelUpOptions[i]);
        Color bgNormal, bgHover, borderNormal, borderHover, stripColor;
        Color nameColor;
        const char* rarityLabel;
        switch (rarity)
        {
        case UpgradeRarity::Common:
            bgNormal = Color{22,22,25,210}; bgHover = Color{42,42,50,230};
            borderNormal = Color{120,120,120,130}; borderHover = Color{200,200,200,220};
            stripColor = Color{80,80,80,200}; nameColor = RAYWHITE; rarityLabel = "COMMON"; break;
        case UpgradeRarity::Rare:
            bgNormal = Color{15,20,42,210}; bgHover = Color{25,40,85,230};
            borderNormal = Color{55,100,200,150}; borderHover = Color{100,160,255,255};
            stripColor = Color{35,75,180,210}; nameColor = Color{160,210,255,255}; rarityLabel = "RARE"; break;
        default: // Epic
            bgNormal = Color{35,18,5,210}; bgHover = Color{70,35,10,230};
            borderNormal = Color{200,100,20,150}; borderHover = Color{255,160,50,255};
            stripColor = Color{180,85,15,220}; nameColor = Color{255,185,80,255}; rarityLabel = "EPIC"; break;
        }
        Color bgColor = hovered ? bgHover : bgNormal;
        DrawRectangleRounded(card, 0.12f, 8, bgColor);
        DrawRectangleRoundedLines(card, 0.12f, 8, hovered ? borderHover : borderNormal);

        const char* name = "";
        std::string desc;
        Texture2D* icon  = nullptr;
        getUpgradeInfo(_levelUpOptions[i], name, desc, icon);

        // Icon area - ability unlocks get a drawn attack-pattern preview so the
        // player can immediately see "burst vs single shot". Stat upgrades keep
        // their regular texture icon.
        {
            const float iconAreaSize = cardW * 0.52f;
            const float previewCX    = x + cardW / 2.f;
            const float previewCY    = cardY + cardH * 0.10f + iconAreaSize / 2.f;

            UpgradeType opt = _levelUpOptions[i];
            bool isSpreadAbility   = (opt == UpgradeType::LearnFireSpread  ||
                                      opt == UpgradeType::LearnIceSpread   ||
                                      opt == UpgradeType::LearnElectricSpread);
            bool isBoltAbility     = (opt == UpgradeType::LearnFireBolt    ||
                                      opt == UpgradeType::LearnIceBolt     ||
                                      opt == UpgradeType::LearnElectricBolt);
            bool isUltimateAbility = (opt == UpgradeType::LearnFireUltimate    ||
                                      opt == UpgradeType::LearnIceUltimate     ||
                                      opt == UpgradeType::LearnElectricUltimate);

            if (isSpreadAbility || isBoltAbility || isUltimateAbility)
            {
                // Pick element colour
                Color ec =
                    (opt == UpgradeType::LearnFireSpread    || opt == UpgradeType::LearnFireBolt    || opt == UpgradeType::LearnFireUltimate)     ? Color{255, 110,  20, 255} :
                    (opt == UpgradeType::LearnIceSpread     || opt == UpgradeType::LearnIceBolt     || opt == UpgradeType::LearnIceUltimate)      ? Color{100, 210, 255, 255} :
                                                                                                                                                    Color{255, 220,  30, 255};
                Color ecDim = { ec.r, ec.g, ec.b, 80 };

                if (isSpreadAbility)
                {
                    // 8 radiating arrows from a centre point
                    const float armLen  = iconAreaSize * 0.40f;
                    const float headLen = 10.f;
                    const float headW   = 6.f;
                    const float angles[8] = { 0.f, 45.f, 90.f, 135.f, 180.f, 225.f, 270.f, 315.f };

                    for (float deg : angles)
                    {
                        float rad  = deg * DEG2RAD;
                        float cx2  = cosf(rad);
                        float cy2  = sinf(rad);
                        Vector2 shaft0 = { previewCX + cx2 * 22.f, previewCY + cy2 * 22.f };
                        Vector2 tip    = { previewCX + cx2 * armLen, previewCY + cy2 * armLen };

                        DrawLineEx(shaft0, tip, 2.2f, ec);

                        Vector2 perpL = { -cy2, cx2 };
                        Vector2 hb1 = { tip.x - cx2 * headLen + perpL.x * headW,
                                        tip.y - cy2 * headLen + perpL.y * headW };
                        Vector2 hb2 = { tip.x - cx2 * headLen - perpL.x * headW,
                                        tip.y - cy2 * headLen - perpL.y * headW };
                        DrawTriangle(tip, hb1, hb2, ec);
                    }
                }
                else if (isBoltAbility)
                {
                    // Single large arrow aimed to the right
                    const float armLen  = iconAreaSize * 0.38f;
                    const float headLen = 18.f;
                    const float headW   = 11.f;

                    Vector2 shaftStart = { previewCX - armLen * 0.55f, previewCY };
                    Vector2 tip        = { previewCX + armLen,          previewCY };
                    DrawLineEx(shaftStart, tip, 4.5f, ec);

                    Vector2 hb1 = { tip.x - headLen, tip.y - headW };
                    Vector2 hb2 = { tip.x - headLen, tip.y + headW };
                    DrawTriangle(tip, hb1, hb2, ec);
                }
                else // ultimate
                {
                    // Scattered burst: outer ring + 10 dots at irregular distances
                    // to suggest "explosions everywhere". Positions are deterministic
                    // so they don't flicker each frame.
                    const float outerR = iconAreaSize * 0.44f;
                    DrawCircleLinesV({ previewCX, previewCY }, outerR, ecDim);

                    const float dotAngles[10] = { 0.f, 36.f, 72.f, 108.f, 144.f, 180.f, 216.f, 252.f, 288.f, 324.f };
                    const float dotDists[10]  = { 0.55f, 0.80f, 0.65f, 0.90f, 0.50f, 0.75f, 0.85f, 0.60f, 0.95f, 0.70f };

                    for (int d = 0; d < 10; d++)
                    {
                        float rad  = dotAngles[d] * DEG2RAD;
                        float dist = outerR * dotDists[d];
                        Vector2 dp = { previewCX + cosf(rad) * dist, previewCY + sinf(rad) * dist };
                        DrawCircleV(dp, 5.f, ecDim);
                        DrawCircleV(dp, 3.f, ec);
                    }
                }

                // Icon centred on top of the arrow pattern
                if (icon && icon->id != 0)
                {
                    // Fire icon reads clearly at a smaller size; ice and electric
                    // sprites are visually smaller so give them more room.
                    float iconTarget =
                        (opt == UpgradeType::LearnIceSpread      || opt == UpgradeType::LearnIceBolt      || opt == UpgradeType::LearnIceUltimate      ||
                         opt == UpgradeType::LearnElectricSpread || opt == UpgradeType::LearnElectricBolt || opt == UpgradeType::LearnElectricUltimate)
                        ? 72.f : 44.f;
                    float iconScale = std::min(iconTarget / (float)icon->width,
                                               iconTarget / (float)icon->height);
                    float iw = icon->width  * iconScale;
                    float ih = icon->height * iconScale;
                    DrawTextureEx(*icon, { previewCX - iw * 0.5f, previewCY - ih * 0.5f },
                                  0.f, iconScale, WHITE);
                }
            }
            else if (icon && icon->id != 0)
            {
                // Stat upgrade - draw the regular texture icon
                float iconScale = std::min(iconAreaSize / (float)icon->width,
                                           iconAreaSize / (float)icon->height);
                float iconW = icon->width  * iconScale;
                float iconH = icon->height * iconScale;
                float iconX = previewCX - iconW / 2.f;
                float iconY = cardY + cardH * 0.10f + (iconAreaSize - iconH) / 2.f;
                DrawTextureEx(*icon, { iconX, iconY }, 0.f, iconScale, WHITE);
            }
        }

        // Name (rarity-tinted when not hovered)
        int nameSz = 26;
        int nameW  = MeasureText(name, nameSz);
        DrawText(name,
            (int)(x + cardW / 2.f - nameW / 2.f),
            (int)(cardY + cardH * 0.58f),
            nameSz, hovered ? GOLD : nameColor);

        drawMultilineCentered(desc, x + cardW / 2.f, cardY + cardH * 0.72f, 20, LIGHTGRAY);

        // Rarity strip at card bottom
        {
            const float stripH = 26.f;
            Rectangle stripRect{ x + 2.f, cardY + cardH - stripH - 2.f, cardW - 4.f, stripH };
            DrawRectangleRec(stripRect, stripColor);
            int rarSz = 15;
            DrawText(rarityLabel,
                (int)(x + cardW / 2.f - MeasureText(rarityLabel, rarSz) / 2.f),
                (int)(cardY + cardH - stripH - 2.f + (stripH - rarSz) / 2.f),
                rarSz, WHITE);
        }

        // Click or gamepad A to select - blocked until open timer expires
        bool regActivated = (ready && hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) ||
                            (_gamepad.isActive && ready && _levelUpGpRow == 1 && i == _levelUpGpCursor && _gamepad.menuConfirmPressed);
        if (regActivated)
        {
            _player.ApplyUpgrade(_levelUpOptions[i]);
            _awaitingStartingAbility = false;
            _regularRowPicked = true;
            if (!_showUltimateRow || _ultimateRowPicked)
            {
                if (_levelUpReturnState == GameState::Map) _mapOpenTimer = 0.4f;
                _gameState = _levelUpReturnState;
            }
        }
    }
    } // end showReg

    // Hint / loading cue
    const char* hint;
    if (_levelUpOpenTimer > 0.f)
        hint = "Loading...";
    else if (showUlt && showReg)
        hint = "Choose one from each row";
    else
        hint = "Click a card to choose";

    Color hintColor = (_levelUpOpenTimer > 0.f) ? Fade(GRAY, 0.6f) : Fade(LIGHTGRAY, 0.7f);
    float hintRefY  = showReg ? (regRowY + cardH + 28.f) : (ultRowY + cardH + 28.f);
    int hintW = MeasureText(hint, 22);
    DrawText(hint, (int)(sw / 2.f - hintW / 2.f), (int)hintRefY, 22, hintColor);
}

// Apply an attack's saved projectile tuning (size / collision / speed / lifetime)
// to a spread projectile. Falls back to the legacy AttackEditor box radius, and
// leaves the projectile's own defaults when nothing is tuned.
static void ApplySpreadTuning(SpreadProjectile& p, const AttackTuning* t)
{
    if (!t) return;
    if (t->hasProjectile)
    {
        if (t->projRadius   > 0.f) p.SetRadius(t->projRadius);
        if (t->projScale    > 0.f) p.SetVisualScale(t->projScale);
        if (t->projSpeed    > 0.f) p.SetSpeed(t->projSpeed);
        if (t->projLifetime > 0.f) p.SetLifetime(t->projLifetime);
    }
    else if (t->hasBox && t->w > 0.f)
    {
        p.SetRadius(t->w * 0.5f);   // legacy AttackEditor box → circle radius
    }
}

void Engine::HandlePlayerMeleeDamage()
{
    if (!_player.CanApplyMeleeDamage())
        return;

    // Ranged classes (Mage/Warlock/Hunter) fire a projectile instead of swinging.
    if (_player.UsesRangedBasic())
    {
        AbilityType element; Color tint; bool arrow = false;
        switch (_player.GetClass())
        {
        case PlayerClass::Mage:    element = AbilityType::FireSpread; tint = Color{ 150, 205, 255, 255 }; break; // arcane orb
        case PlayerClass::Warlock: element = AbilityType::FireSpread; tint = Color{ 190, 100, 225, 255 }; break; // dark orb
        case PlayerClass::Hunter:  element = AbilityType::FireSpread; tint = WHITE; arrow = true;         break; // physical arrow
        default:                   element = AbilityType::FireSpread; tint = WHITE; break;
        }
        const AttackTuning* bt = AttackTuningStore::Get(AttackTuningKeyForBasic((int)_player.GetClass()));
        Vector2 borigin = (bt && bt->hasFirePoint)
                        ? _player.GetCastOrigin(bt->fireForward, bt->fireHeight)
                        : _player.GetCastOrigin();
        SpreadProjectile shot;
        shot.InitBasic(borigin, _player.GetFacingDirection(), element, tint, arrow);
        ApplySpreadTuning(shot, bt);
        _spreadProjectiles.push_back(shot);
        _player.ConsumeMeleeDamageFrame();
        return;
    }

    bool hitAny = false;
    Rectangle attackRec = _player.GetAttackCollisionRec();

    // Melee swings chip destructible room hazards (totems / torches) too.
    if (_roomHazards.DamageHazardsInRect(attackRec, 1) > 0)
        hitAny = true;

    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive())
            continue;
        if (!enemy->IsAlive())
            continue;

        if (CheckCollisionRecs(attackRec, enemy->GetHitCollisionRec()))
        {
            bool crit = false;
            int base = (int)std::round(_player.GetMeleeDamage() * _player.GetClassDamageMult());
            int dmg  = ScalePlayerHit(*enemy, std::max(1, base), crit);
            const float healthBefore = enemy->GetHealthValue();
            enemy->TakeDamage(dmg, _player.GetWorldPos());
            Enemy::HitBlockReason blocked = enemy->ConsumeHitBlock();
            if (blocked != Enemy::HitBlockReason::None)
            {
                ShowBlockedHitFeedback(*enemy, blocked);
                continue;
            }
            const int actualDamage = RegisterHitFx(*enemy, healthBefore, crit, false, false, 1u);
            if (actualDamage <= 0)
                continue;
            ApplyPlayerLifesteal(actualDamage);
            _player.AddRage(8.f);         // Warrior: every landed swing stokes Rage (no-op for others)
            _player.AddComboPoints(1);    // Rogue: quick strikes bank combo points (no-op for others)
            _player.AddFaith(4.f);        // Paladin: righteous blows build Faith (no-op for others)
            _vfx.SpawnHitEffect(Character::CastType::None, enemy->GetWorldPos(), _player.GetFacingDirection());
            hitAny = true;
        }
    }

    _player.ConsumeMeleeDamageFrame();

    if (hitAny)
        TriggerScreenShake(6.f, 0.07f);
}

Rectangle Engine::GetTreasureChestRect() const
{
    // Generous pickup zone (~2.5 tiles) — walking anywhere near the chest opens
    // it; paired with a body-overlap check instead of a single-point test.
    const Vector2 chest = GetTreasureChestWorldPos();
    return { chest.x - 90.f, chest.y - 90.f, 180.f, 180.f };
}

Vector2 Engine::GetTreasureChestWorldPos() const
{
    int col = _dungeonRoomLayout.treasureChestCol;
    int row = _dungeonRoomLayout.treasureChestRow;
    if (col < 0 || col >= RoomLayout::kCols ||
        row < 0 || row >= RoomLayout::kRows)
    {
        col = RoomLayout::kCols / 2;
        row = RoomLayout::kRows / 2;
    }
    const float cellW = (float)kVirtualWidth / (float)RoomLayout::kCols;
    const float cellH = (float)kVirtualHeight / (float)RoomLayout::kRows;
    return { (col + 0.5f) * cellW, (row + 0.5f) * cellH };
}

void Engine::SetTreasureChestTile(TileType type)
{
    int col = _dungeonRoomLayout.treasureChestCol;
    int row = _dungeonRoomLayout.treasureChestRow;
    if (col < 0 || col >= RoomLayout::kCols ||
        row < 0 || row >= RoomLayout::kRows)
    {
        col = RoomLayout::kCols / 2;
        row = RoomLayout::kRows / 2;
    }
    _dungeonRoomLayout.tiles[row][col] = type;
}

void Engine::OpenTreasureChest()
{
    // Switch tile sprite to open chest immediately.
    SetTreasureChestTile(TileType::ChestOpen);

    _vfx.SpawnHitEffect(Character::CastType::None, GetTreasureChestWorldPos(),
                        _player.GetFacingDirection());
    TriggerScreenShake(5.f, 0.12f);
    StopSound(_pickupSound);
    PlaySound(_pickupSound);

    GenerateTreasureChestOptions();
    _levelUpReturnState = GameState::DungeonRun;
    _levelUpOpenTimer   = 0.3f;
    _gameState          = GameState::LevelUpChoice;
}

Engine::AbilityAimProfile Engine::GetAbilityAimProfile(AbilityType ability) const
{
    AbilityAimProfile profile{};
    switch (ability)
    {
    case AbilityType::FireSpread:      profile = { AbilityAimMode::GroundTarget, 560.f, 0.f, 420.f, 90.f }; break;
    case AbilityType::IceSpread:       profile = { AbilityAimMode::Instant, 0.f, 210.f, 0.f, 0.f }; break;
    case AbilityType::ElectricSpread:  profile = { AbilityAimMode::Direction, 430.f, 95.f, 430.f, 100.f }; break;
    case AbilityType::FireBolt:        profile = { AbilityAimMode::Direction, 900.f, 135.f, 0.f, 0.f }; break;
    case AbilityType::IceBolt:         profile = { AbilityAimMode::Direction, 1050.f, 65.f, 0.f, 0.f }; break;
    case AbilityType::ElectricBolt:    profile = { AbilityAimMode::Direction, 800.f, 0.f, 0.f, 0.f }; break;
    case AbilityType::FireUltimate:    profile = { AbilityAimMode::GroundTarget, 800.f, 250.f, 0.f, 0.f }; break;
    case AbilityType::IceUltimate:     profile = { AbilityAimMode::GroundTarget, 720.f, 300.f, 0.f, 0.f }; break;
    case AbilityType::ElectricUltimate:profile = { AbilityAimMode::Direction, 850.f, 230.f, 0.f, 0.f }; break;

    case AbilityType::WarCleave:       profile = { AbilityAimMode::Direction, 210.f, 0.f, 210.f, 200.f, 1.f }; break;
    case AbilityType::ThrowingAxe:     profile = { AbilityAimMode::Direction, 520.f, 0.f, 520.f, 90.f, 1.f }; break;
    case AbilityType::Rend:            profile = { AbilityAimMode::Direction, 270.f, 0.f, 150.f, 140.f, 1.f }; break;
    case AbilityType::ShieldBash:      profile = { AbilityAimMode::Direction, 240.f, 0.f, 150.f, 150.f, 1.f }; break;
    case AbilityType::Earthshatter:    profile = { AbilityAimMode::Direction, 640.f, 0.f, 640.f, 130.f, 1.f }; break;

    case AbilityType::FanOfKnives:     profile = { AbilityAimMode::Direction, 360.f, 0.f, 360.f, 240.f, 0.85f }; break;
    case AbilityType::Shadowstep:      profile = { AbilityAimMode::Direction, 300.f, 0.f, 300.f, 110.f, 0.85f }; break;
    case AbilityType::PoisonVial:      profile = { AbilityAimMode::GroundTarget, 500.f, 130.f, 0.f, 0.f, 0.85f }; break;
    case AbilityType::Backstab:        profile = { AbilityAimMode::Direction, 150.f, 0.f, 150.f, 120.f, 0.85f }; break;
    case AbilityType::Eviscerate:      profile = { AbilityAimMode::Direction, 210.f, 0.f, 210.f, 130.f, 0.85f }; break;
    case AbilityType::RainOfBlades:    profile = { AbilityAimMode::GroundTarget, 680.f, 220.f, 0.f, 0.f, 0.85f }; break;

    case AbilityType::PiercingShot:    profile = { AbilityAimMode::Direction, 600.f, 0.f, 600.f, 70.f, 0.35f }; break;
    case AbilityType::Multishot:       profile = { AbilityAimMode::Direction, 380.f, 0.f, 380.f, 260.f, 0.35f }; break;
    case AbilityType::FrostTrap:       profile = { AbilityAimMode::GroundTarget, 500.f, 175.f, 0.f, 0.f, 0.30f }; break;
    case AbilityType::ExplosiveArrow:  profile = { AbilityAimMode::GroundTarget, 500.f, 195.f, 0.f, 0.f, 0.30f }; break;
    case AbilityType::Roll:            profile = { AbilityAimMode::Direction, 240.f, 0.f, 240.f, 100.f, 0.40f }; break;
    case AbilityType::Volley:          profile = { AbilityAimMode::Direction, 660.f, 0.f, 660.f, 84.f, 0.35f }; break;
    case AbilityType::ArrowStorm:      profile = { AbilityAimMode::GroundTarget, 700.f, 360.f, 0.f, 0.f, 0.30f }; break;
    case AbilityType::PiercingBarrage: profile = { AbilityAimMode::Direction, 900.f, 0.f, 900.f, 150.f, 0.30f }; break;

    case AbilityType::Smite:           profile = { AbilityAimMode::Direction, 210.f, 0.f, 210.f, 200.f, 0.75f }; break;
    case AbilityType::Consecrate:      profile = { AbilityAimMode::GroundTarget, 420.f, 170.f, 0.f, 0.f, 0.70f }; break;
    case AbilityType::HolyBolt:        profile = { AbilityAimMode::Direction, 560.f, 0.f, 560.f, 80.f, 0.70f }; break;
    case AbilityType::HammerThrow:     profile = { AbilityAimMode::Direction, 480.f, 0.f, 480.f, 110.f, 0.70f }; break;
    case AbilityType::HammerOfJustice: profile = { AbilityAimMode::Direction, 660.f, 0.f, 660.f, 150.f, 0.70f }; break;

    case AbilityType::ShadowBolt:      profile = { AbilityAimMode::Direction, 560.f, 0.f, 560.f, 80.f, 0.70f }; break;
    case AbilityType::DrainLife:       profile = { AbilityAimMode::Direction, 220.f, 0.f, 220.f, 130.f, 0.75f }; break;
    case AbilityType::Curse:           profile = { AbilityAimMode::Direction, 260.f, 0.f, 260.f, 200.f, 0.70f }; break;
    case AbilityType::CorruptionPool:  profile = { AbilityAimMode::GroundTarget, 500.f, 150.f, 0.f, 0.f, 0.65f }; break;
    case AbilityType::ShadowNova:      profile = { AbilityAimMode::Direction, 680.f, 0.f, 680.f, 200.f, 0.65f }; break;
    default: break;
    }
    if (const AttackTuning* tuning = AttackTuningStore::Get(AttackTuningKeyForAbility(ability));
        tuning && tuning->hasAbility)
    {
        profile.range = tuning->aimRange;
        profile.radius = tuning->areaRadius;
        profile.length = tuning->effectLength;
        profile.width = tuning->effectWidth;
    }
    return profile;
}

Vector2 Engine::GetCurrentAimWorldPoint() const
{
    Vector2 player = _player.GetWorldPos();
    AbilityAimProfile profile = GetAbilityAimProfile(_pendingAbilityAim.ability);

    if (_gamepad.isActive && _inputPromptMode == InputPromptMode::Gamepad)
    {
        Vector2 dir = Vector2Length(_gamepad.aimDir) > GamepadInput::kDeadZone
            ? Vector2Normalize(_gamepad.aimDir) : _pendingAbilityAim.direction;
        return Vector2Add(player, Vector2Scale(dir, profile.range));
    }

    Vector2 mouse = GetVirtualMousePos();
    return Vector2{ mouse.x + _cameraPos.x - kVirtualWidth * 0.5f,
                    mouse.y + _cameraPos.y - kVirtualHeight * 0.5f };
}

void Engine::BeginAbilityInput(int slot)
{
    if (!_player.CanBeginAbilityCast(slot))
        return;

    AbilityType ability = _player.GetLearnedAbility(slot);
    AbilityAimProfile profile = GetAbilityAimProfile(ability);
    if (profile.mode == AbilityAimMode::Instant)
    {
        _committedAimAbility = ability;
        _committedAimDirection = _player.GetFacingDirection();
        _committedAimTarget = _player.GetWorldPos();
        _player.TriggerAbilityCast(slot);
        return;
    }

    if (_pendingAbilityAim.active)
    {
        if (_pendingAbilityAim.slot == slot && (_settingsMgr.Get().abilityAimToggle || _touchModeActive))
        {
            CommitAbilityAim();
            return;
        }
        CancelAbilityAim();
    }

    _pendingAbilityAim.active = true;
    _pendingAbilityAim.slot = slot;
    _pendingAbilityAim.ability = ability;
    _pendingAbilityAim.direction = _player.GetFacingDirection();
    _pendingAbilityAim.target = Vector2Add(_player.GetWorldPos(),
        Vector2Scale(_pendingAbilityAim.direction, profile.range));
}

void Engine::CancelAbilityAim()
{
    _pendingAbilityAim = PendingAbilityAim{};
    _player.SetAbilityAimMoveScale(1.f);
}

void Engine::CommitAbilityAim()
{
    if (!_pendingAbilityAim.active)
        return;
    if (_player.CanBeginAbilityCast(_pendingAbilityAim.slot))
    {
        _committedAimAbility = _pendingAbilityAim.ability;
        _committedAimDirection = _pendingAbilityAim.direction;
        _committedAimTarget = _pendingAbilityAim.target;
        _player.TriggerAbilityCast(_pendingAbilityAim.slot);
    }
    CancelAbilityAim();
}

void Engine::UpdateAbilityAiming()
{
    if (_pendingAbilityAim.active)
    {
        AbilityAimProfile profile = GetAbilityAimProfile(_pendingAbilityAim.ability);
        _player.SetAbilityAimMoveScale(profile.moveScale);
        Vector2 player = _player.GetWorldPos();
        Vector2 rawTarget = GetCurrentAimWorldPoint();
        Vector2 delta = Vector2Subtract(rawTarget, player);
        if (Vector2Length(delta) > 1.f)
            _pendingAbilityAim.direction = Vector2Normalize(delta);
        float distance = std::min(profile.range, Vector2Length(delta));
        _pendingAbilityAim.target = Vector2Add(player, Vector2Scale(_pendingAbilityAim.direction, distance));

        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
        {
            CancelAbilityAim();
            return;
        }
        if (_pendingAbilityAim.commitOnMouseRelease && IsMouseButtonReleased(MOUSE_LEFT_BUTTON))
        {
            CommitAbilityAim();
            return;
        }
    }

    for (int slot = 0; slot < _player.GetLearnedCount(); ++slot)
    {
        KeyboardKey key = _player.GetAbilityKey(slot);
        bool pressed = (key != KEY_NULL && IsKeyPressed(key)) ||
                       (_gamepad.isActive && _gamepad.abilityPressed[slot]);
        bool released = (key != KEY_NULL && IsKeyReleased(key)) ||
                        (_gamepad.isActive && _gamepad.abilityReleased[slot]);

        if (pressed)
        {
            BeginAbilityInput(slot);
            return;
        }
        if (_pendingAbilityAim.active && _pendingAbilityAim.slot == slot &&
            !_settingsMgr.Get().abilityAimToggle && released)
        {
            CommitAbilityAim();
            return;
        }
    }
}

void Engine::DrawAbilityAimPreview() const
{
    if (!_pendingAbilityAim.active)
        return;

    AbilityAimProfile profile = GetAbilityAimProfile(_pendingAbilityAim.ability);
    auto toScreen = [&](Vector2 world)
    {
        return Vector2{ world.x - _cameraPos.x + kVirtualWidth * 0.5f + _shakeOffset.x,
                        world.y - _cameraPos.y + kVirtualHeight * 0.5f + _shakeOffset.y };
    };

    Vector2 start = toScreen(_player.GetWorldPos());
    Vector2 target = toScreen(_pendingAbilityAim.target);
    Vector2 dir = _pendingAbilityAim.direction;
    Vector2 side{ -dir.y, dir.x };
    Color element = (_pendingAbilityAim.ability == AbilityType::FireSpread ||
                     _pendingAbilityAim.ability == AbilityType::FireBolt ||
                     _pendingAbilityAim.ability == AbilityType::FireUltimate)
                  ? Color{ 255, 110, 45, 255 }
                  : (_pendingAbilityAim.ability == AbilityType::IceSpread ||
                     _pendingAbilityAim.ability == AbilityType::IceBolt ||
                     _pendingAbilityAim.ability == AbilityType::IceUltimate)
                  ? Color{ 100, 220, 255, 255 }
                  : ((int)_pendingAbilityAim.ability >= (int)AbilityType::WarCleave &&
                     (int)_pendingAbilityAim.ability <= (int)AbilityType::Earthshatter)
                  ? Color{ 255, 135, 70, 255 }
                  : ((int)_pendingAbilityAim.ability >= (int)AbilityType::FanOfKnives &&
                     (int)_pendingAbilityAim.ability <= (int)AbilityType::RainOfBlades)
                  ? Color{ 205, 120, 255, 255 }
                  : ((int)_pendingAbilityAim.ability >= (int)AbilityType::PiercingShot &&
                     (int)_pendingAbilityAim.ability <= (int)AbilityType::PiercingBarrage)
                  ? Color{ 100, 240, 175, 255 }
                  : ((int)_pendingAbilityAim.ability >= (int)AbilityType::Smite &&
                     (int)_pendingAbilityAim.ability <= (int)AbilityType::HammerOfJustice)
                  ? Color{ 255, 225, 110, 255 }
                  : ((int)_pendingAbilityAim.ability >= (int)AbilityType::ShadowBolt &&
                     (int)_pendingAbilityAim.ability <= (int)AbilityType::ShadowNova)
                  ? Color{ 205, 90, 240, 255 }
                  : Color{ 255, 230, 70, 255 };

    DrawLineEx(start, target, 4.f, Fade(element, 0.72f));
    Vector2 arrowBase = Vector2Subtract(target, Vector2Scale(dir, 28.f));
    DrawTriangle(target,
                 Vector2Add(arrowBase, Vector2Scale(side, 13.f)),
                 Vector2Subtract(arrowBase, Vector2Scale(side, 13.f)),
                 Fade(element, 0.88f));

    if (profile.mode == AbilityAimMode::GroundTarget)
    {
        if (_pendingAbilityAim.ability == AbilityType::FireSpread)
        {
            Vector2 a = Vector2Subtract(target, Vector2Scale(side, profile.length * 0.5f));
            Vector2 b = Vector2Add(target, Vector2Scale(side, profile.length * 0.5f));
            DrawLineEx(a, b, profile.width, Fade(element, 0.13f));
            DrawLineEx(a, b, 3.f, Fade(element, 0.9f));
        }
        else
        {
            DrawCircleV(target, profile.radius, Fade(element, 0.10f));
            DrawCircleLinesV(target, profile.radius, Fade(element, 0.9f));
        }
    }
    else if (profile.length > 0.f && profile.width > 0.f)
    {
        Vector2 end = Vector2Add(start, Vector2Scale(dir, profile.length));
        Vector2 halfSide = Vector2Scale(side, profile.width * 0.5f);
        DrawLineEx(start, end, profile.width, Fade(element, 0.10f));
        DrawLineEx(Vector2Add(start, halfSide), Vector2Add(end, halfSide), 2.f, Fade(element, 0.8f));
        DrawLineEx(Vector2Subtract(start, halfSide), Vector2Subtract(end, halfSide), 2.f, Fade(element, 0.8f));
        if (profile.radius > 0.f)
            DrawCircleLinesV(end, profile.radius, Fade(element, 0.85f));
    }

    const char* mode = _settingsMgr.Get().abilityAimToggle ? "PRESS AGAIN TO CAST" : "RELEASE TO CAST";
    DrawText(mode, (int)target.x - MeasureText(mode, 18) / 2, (int)target.y + 24, 18, element);
}

void Engine::HandlePlayerCastRequest()
{
    Character::CastType castType = _player.ConsumeCastRequest();

    AbilityType mageAbility = AbilityType::None;
    switch (castType)
    {
    case Character::CastType::FireSpread:      mageAbility = AbilityType::FireSpread; break;
    case Character::CastType::IceSpread:       mageAbility = AbilityType::IceSpread; break;
    case Character::CastType::ElectricSpread:  mageAbility = AbilityType::ElectricSpread; break;
    case Character::CastType::FireBolt:        mageAbility = AbilityType::FireBolt; break;
    case Character::CastType::IceBolt:         mageAbility = AbilityType::IceBolt; break;
    case Character::CastType::ElectricBolt:    mageAbility = AbilityType::ElectricBolt; break;
    case Character::CastType::FireUltimate:    mageAbility = AbilityType::FireUltimate; break;
    case Character::CastType::IceUltimate:     mageAbility = AbilityType::IceUltimate; break;
    case Character::CastType::ElectricUltimate:mageAbility = AbilityType::ElectricUltimate; break;
    default: break;
    }
    if (mageAbility != AbilityType::None)
    {
        Vector2 direction = (_committedAimAbility == mageAbility)
            ? _committedAimDirection : _player.GetFacingDirection();
        Vector2 target = (_committedAimAbility == mageAbility)
            ? _committedAimTarget
            : Vector2Add(_player.GetWorldPos(), Vector2Scale(direction, GetAbilityAimProfile(mageAbility).range));
        CastMageSpell(mageAbility, direction, target);
        SfxBank::Get().PlayAbilityCast(mageAbility);   // element-specific cast
        _committedAimAbility = AbilityType::None;
    }

    // Non-elemental class abilities (Warrior kit, and future classes) dispatch
    // through their own handler rather than the elemental CastType path.
    AbilityType classAbility = _player.ConsumeClassAbility();
    if (classAbility != AbilityType::None)
    {
        SfxBank::Get().PlayAbilityCast(classAbility);   // per-ability signature
        HandleClassAbilityCast(classAbility);
        _committedAimAbility = AbilityType::None;
    }
}
void Engine::SpawnSpreadBurst(AbilityType element)
{
    static const std::array<Vector2, 8> directions{
        Vector2{  1.f,  0.f },
        Vector2{ -1.f,  0.f },
        Vector2{  0.f,  1.f },
        Vector2{  0.f, -1.f },
        Vector2{  1.f,  1.f },
        Vector2{  1.f, -1.f },
        Vector2{ -1.f,  1.f },
        Vector2{ -1.f, -1.f }
    };

    const AttackTuning* t = AttackTuningStore::Get(AttackTuningKeyForAbility(element));
    Vector2 origin = (t && t->hasFirePoint)
                   ? _player.GetCastOrigin(t->fireForward, t->fireHeight)
                   : _player.GetCastOrigin();

    for (const Vector2& dir : directions)
    {
        SpreadProjectile projectile;
        projectile.Init(origin, dir, element);
        ApplySpreadTuning(projectile, t);
        _spreadProjectiles.push_back(projectile);
    }
}

void Engine::SpawnBolt(AbilityType element)
{
    const AttackTuning* t = AttackTuningStore::Get(AttackTuningKeyForAbility(element));
    Vector2 origin = (t && t->hasFirePoint)
                   ? _player.GetCastOrigin(t->fireForward, t->fireHeight)
                   : _player.GetCastOrigin();
    SpreadProjectile projectile;
    projectile.Init(origin, _player.GetFacingDirection(), element);
    ApplySpreadTuning(projectile, t);
    _spreadProjectiles.push_back(projectile);
}

bool Engine::DamageMageEnemy(Enemy& enemy, int damage, AbilityType element,
                             bool burn, bool chill, bool shock)
{
    const bool overload = (element == AbilityType::FireSpread || element == AbilityType::FireBolt ||
                           element == AbilityType::FireUltimate) && enemy.IsCharged();
    int combo = ResolveElementalCombo(enemy, element, &_vfx);
    combo = (int)roundf(combo * _player.GetComboBonusMult());
    bool crit = false;
    int dealt = ScalePlayerHit(enemy, std::max(1, damage), crit);
    const float healthBefore = enemy.GetHealthValue();
    enemy.TakeDamage(dealt, _player.GetWorldPos());
    Enemy::HitBlockReason blocked = enemy.ConsumeHitBlock();
    if (blocked != Enemy::HitBlockReason::None)
    {
        ShowBlockedHitFeedback(enemy, blocked);
        return false;
    }
    const std::uint32_t attackId = 100u + static_cast<std::uint32_t>(element);
    if (RegisterHitFx(enemy, healthBefore, crit, false, false, attackId) <= 0)
        return false;
    if (combo > 0 && enemy.IsAlive())
    {
        const float comboHealthBefore = enemy.GetHealthValue();
        enemy.TakeDamage(combo, _player.GetWorldPos());
        RegisterHitFx(enemy, comboHealthBefore, false, false, false, attackId + 1000u);
    }
    if (burn)
        enemy.ApplyBurn(0.45f, std::max(1, _player.GetBoltBurnDamage(element)), _player.GetWorldPos());
    if (chill)
    {
        if (enemy.IsBoss()) enemy.ApplySlow(0.94f, 1.25f);
        else                enemy.ApplySlow(0.72f, 1.5f);
    }
    if (shock)
        enemy.ApplyElectricCharge();
    if (overload && enemy.IsAlive())
    {
        Vector2 away = Vector2Subtract(enemy.GetWorldPos(), _player.GetWorldPos());
        enemy.StartForcedPush(away, enemy.IsBoss() ? 45.f : 150.f);
    }
    return true;
}

void Engine::DamageMageArea(Vector2 centre, float radius, int damage, AbilityType element,
                            bool burn, bool chill, bool shock)
{
    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive() || !enemy->IsAlive())
            continue;
        Rectangle hit = enemy->GetHitCollisionRec();
        Vector2 closest{ std::clamp(centre.x, hit.x, hit.x + hit.width),
                         std::clamp(centre.y, hit.y, hit.y + hit.height) };
        if (Vector2Distance(centre, closest) > radius)
            continue;

        DamageMageEnemy(*enemy, damage, element, burn, chill, shock);
    }
}

void Engine::CastMageSpell(AbilityType ability, Vector2 direction, Vector2 target)
{
    if (Vector2Length(direction) < 0.01f)
        direction = _player.GetFacingDirection();
    direction = Vector2Normalize(direction);
    Vector2 playerPos = _player.GetWorldPos();
    const AttackTuning* tuning = AttackTuningStore::Get(AttackTuningKeyForAbility(ability));
    AbilityAimProfile profile = GetAbilityAimProfile(ability);
    auto durationOr = [&](float fallback) { return (tuning && tuning->hasAbility) ? tuning->effectDuration : fallback; };
    auto tickOr = [&](float fallback) { return (tuning && tuning->hasAbility) ? tuning->tickInterval : fallback; };

    StopSound(_fireballCastSound);
    PlaySound(_fireballCastSound);

    if (ability == AbilityType::FireBolt || ability == AbilityType::IceBolt || ability == AbilityType::ElectricBolt)
    {
        Vector2 origin = (tuning && tuning->hasFirePoint)
            ? _player.GetCastOrigin(tuning->fireForward, tuning->fireHeight)
            : _player.GetCastOrigin();
        SpreadProjectile projectile;
        projectile.Init(origin, direction, ability);
        ApplySpreadTuning(projectile, tuning);
        if (ability == AbilityType::IceBolt)
            projectile.SetPiercingHits(5);
        _spreadProjectiles.push_back(projectile);

        Character::CastType castFx = ability == AbilityType::FireBolt ? Character::CastType::FireSpread
                                   : ability == AbilityType::IceBolt ? Character::CastType::IceSpread
                                                                     : Character::CastType::ElectricSpread;
        _vfx.SpawnCastEffect(castFx, origin, direction);
        return;
    }

    MageSpellField field;
    field.ability = ability;
    field.direction = direction;

    switch (ability)
    {
    case AbilityType::FireSpread: // Flame Wall
        field.pos = target;
        field.duration = durationOr(5.f);
        field.tickInterval = tickOr(0.45f);
        field.length = profile.length;
        field.width = profile.width;
        break;

    case AbilityType::IceSpread: // Frost Nova
        DamageMageArea(playerPos, profile.radius, _player.GetSpreadHitDamage(ability), ability, false, true, false);
        for (auto& enemy : _enemies)
            if (enemy->IsActive() && enemy->IsAlive() && !enemy->IsBoss() &&
                Vector2Distance(enemy->GetWorldPos(), playerPos) <= profile.radius)
                enemy->ApplyFreeze(0.75f);
        field.pos = playerPos;
        field.duration = 0.7f;
        field.radius = profile.radius;
        field.impacted = true;
        break;

    case AbilityType::ElectricSpread: // Lightning Blink
    {
        const float maxDistance = (tuning && tuning->hasAbility) ? tuning->moveDistance : profile.range;
        Vector2 start = playerPos;
        Vector2 destination = start;
        Rectangle body = _player.GetCollisionRec();
        Vector2 bodyOffset{ body.x - start.x, body.y - start.y };
        for (float d = 24.f; d <= maxDistance; d += 24.f)
        {
            Vector2 candidate = Vector2Add(start, Vector2Scale(direction, d));
            Rectangle candidateBody{ candidate.x + bodyOffset.x, candidate.y + bodyOffset.y, body.width, body.height };
            bool blocked = false;
            for (const auto& prop : _props)
                if (CheckCollisionRecs(candidateBody, prop.GetCollisionRec())) { blocked = true; break; }
            if (_nav.GetCellSize() > 0.f)
            {
                int col = (int)(candidate.x / _nav.GetCellSize());
                int row = (int)(candidate.y / _nav.GetCellSize());
                blocked = blocked || _nav.IsCellBlocked(col, row);
            }
            if (blocked) break;
            destination = candidate;
        }

        auto pointSegmentDistance = [](Vector2 p, Vector2 a, Vector2 b)
        {
            Vector2 ab = Vector2Subtract(b, a);
            float lenSq = Vector2DotProduct(ab, ab);
            float t = lenSq > 0.001f ? Vector2DotProduct(Vector2Subtract(p, a), ab) / lenSq : 0.f;
            t = std::clamp(t, 0.f, 1.f);
            return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, t)));
        };
        for (auto& enemy : _enemies)
        {
            if (!enemy->IsActive() || !enemy->IsAlive()) continue;
            if (pointSegmentDistance(enemy->GetWorldPos(), start, destination) > 85.f) continue;
            DamageMageEnemy(*enemy, _player.GetSpreadHitDamage(ability), ability, false, false, true);
        }
        _player.SetWorldPos(destination);
        _player.GrantInvulnerability(0.12f);
        DamageMageArea(destination, 110.f, _player.GetSpreadHitDamage(ability), ability, false, false, true);
        field.pos = start;
        field.strikePos = destination;
        field.duration = 0.38f;
        field.impacted = true;
        break;
    }

    case AbilityType::FireUltimate: // Meteor
        field.pos = target;
        field.duration = durationOr(4.8f);
        field.windup = 0.9f;
        field.radius = profile.radius;
        field.tickInterval = tickOr(0.55f);
        TriggerScreenFlash(Color{ 255, 120, 40, 255 }, 0.12f);
        break;

    case AbilityType::IceUltimate: // Blizzard
        field.pos = target;
        field.duration = durationOr(7.f);
        field.radius = profile.radius;
        field.tickInterval = tickOr(0.55f);
        field.impacted = true;
        break;

    case AbilityType::ElectricUltimate: // Thunderstorm
        field.pos = Vector2Add(playerPos, Vector2Scale(direction, 100.f));
        field.strikePos = field.pos;
        field.duration = durationOr(7.f);
        field.radius = profile.radius;
        field.tickInterval = tickOr(0.42f);
        field.impacted = true;
        break;

    default:
        return;
    }
    _mageSpellFields.push_back(field);
}

void Engine::UpdateMageSpells(float dt)
{
    auto distanceToSegment = [](Vector2 p, Vector2 a, Vector2 b)
    {
        Vector2 ab = Vector2Subtract(b, a);
        float lenSq = Vector2DotProduct(ab, ab);
        float t = lenSq > 0.001f ? Vector2DotProduct(Vector2Subtract(p, a), ab) / lenSq : 0.f;
        t = std::clamp(t, 0.f, 1.f);
        return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, t)));
    };

    for (MageSpellField& field : _mageSpellFields)
    {
        field.timer += dt;
        field.strikeFlash = std::max(0.f, field.strikeFlash - dt);

        if (field.ability == AbilityType::FireUltimate && !field.impacted)
        {
            if (field.timer < field.windup)
                continue;
            field.impacted = true;
            field.tickAccum = 0.f;
            DamageMageArea(field.pos, field.radius, _player.GetUltimateHitDamage(field.ability),
                           field.ability, true, false, false);
            TriggerScreenShake(13.f, 0.32f);
            StopSound(_explosionSound); PlaySound(_explosionSound);
        }

        if (!field.impacted)
            continue;

        if (field.ability == AbilityType::ElectricUltimate)
        {
            Vector2 candidate = Vector2Add(field.pos, Vector2Scale(field.direction, 88.f * dt));
            bool blocked = false;
            if (_nav.GetCellSize() > 0.f)
            {
                int col = (int)(candidate.x / _nav.GetCellSize());
                int row = (int)(candidate.y / _nav.GetCellSize());
                blocked = _nav.IsCellBlocked(col, row);
            }
            if (!blocked)
                field.pos = candidate; // the storm only advances; it never reverses
        }

        field.tickAccum += dt;
        while (field.tickAccum >= field.tickInterval)
        {
            field.tickAccum -= field.tickInterval;
            ++field.tickCount;

            if (field.ability == AbilityType::FireSpread)
            {
                Vector2 across{ -field.direction.y, field.direction.x };
                Vector2 a = Vector2Subtract(field.pos, Vector2Scale(across, field.length * 0.5f));
                Vector2 b = Vector2Add(field.pos, Vector2Scale(across, field.length * 0.5f));
                for (auto& enemy : _enemies)
                    if (enemy->IsActive() && enemy->IsAlive() &&
                        distanceToSegment(enemy->GetWorldPos(), a, b) <= field.width * 0.5f)
                        DamageMageEnemy(*enemy, _player.GetSpreadHitDamage(field.ability),
                                        field.ability, true, false, false);
            }
            else if (field.ability == AbilityType::FireUltimate)
            {
                DamageMageArea(field.pos, field.radius * 0.82f,
                               std::max(1, _player.GetUltimateHitDamage(field.ability) / 3),
                               field.ability, true, false, false);
            }
            else if (field.ability == AbilityType::IceUltimate)
            {
                DamageMageArea(field.pos, field.radius,
                               std::max(1, _player.GetUltimateHitDamage(field.ability) / 3),
                               field.ability, false, true, false);
                if ((field.tickCount % 4) == 0)
                    for (auto& enemy : _enemies)
                        if (enemy->IsActive() && enemy->IsAlive() && !enemy->IsBoss() &&
                            Vector2Distance(enemy->GetWorldPos(), field.pos) <= field.radius)
                            enemy->ApplyFreeze(0.65f);
            }
            else if (field.ability == AbilityType::ElectricUltimate)
            {
                // Strike positions are random only inside the storm's forward
                // half, so its weather front never appears to travel backwards.
                Vector2 side{ -field.direction.y, field.direction.x };
                float forward = (float)GetRandomValue(20, (int)field.radius);
                float lateral = (float)GetRandomValue(-(int)field.radius, (int)field.radius);
                field.strikePos = Vector2Add(field.pos,
                    Vector2Add(Vector2Scale(field.direction, forward), Vector2Scale(side, lateral * 0.65f)));
                field.strikeFlash = 0.22f;
                DamageMageArea(field.strikePos, 95.f,
                               std::max(1, _player.GetUltimateHitDamage(field.ability) / 2),
                               field.ability, false, false, true);
                TriggerScreenShake(2.5f, 0.06f);
            }
        }
    }

    _mageSpellFields.erase(
        std::remove_if(_mageSpellFields.begin(), _mageSpellFields.end(),
            [](const MageSpellField& field) { return field.timer >= field.duration; }),
        _mageSpellFields.end());
}

void Engine::DrawMageSpells(Vector2 cameraRef) const
{
    auto screenPos = [&](Vector2 world)
    {
        Vector2 p = Vector2Subtract(world, cameraRef);
        p.x += kVirtualWidth * 0.5f;
        p.y += kVirtualHeight * 0.5f;
        return p;
    };
    auto drawSpellFrame = [&](AbilityType element, Vector2 world, float size, float alpha, float rotation = 0.f)
    {
        const Texture2D& tex = SpreadProjectile::GetAnimTexture(element);
        int fw = SpreadProjectile::GetFrameWFor(element);
        int fh = SpreadProjectile::GetFrameHFor(element);
        int count = std::max(1, SpreadProjectile::GetFrameCountFor(element));
        int frame = (int)((float)GetTime() * 16.f) % count;
        Rectangle src = GetAnimationFrameRect(tex, fw, fh, frame);
        Vector2 p = screenPos(world);
        Rectangle dst{ p.x, p.y, size, size };
        DrawTexturePro(tex, src, dst, Vector2{ size * 0.5f, size * 0.5f }, rotation,
                       Fade(WHITE, std::clamp(alpha, 0.f, 1.f)));
    };

    for (const MageSpellField& field : _mageSpellFields)
    {
        float remainingFade = std::clamp((field.duration - field.timer) * 3.f, 0.f, 1.f);
        if (field.ability == AbilityType::FireSpread)
        {
            Vector2 side{ -field.direction.y, field.direction.x };
            for (int i = -3; i <= 3; ++i)
                drawSpellFrame(AbilityType::FireUltimate,
                    Vector2Add(field.pos, Vector2Scale(side, field.length * (float)i / 7.f)),
                    field.width * 1.7f, remainingFade, atan2f(side.y, side.x) * RAD2DEG);
        }
        else if (field.ability == AbilityType::IceSpread)
        {
            float grow = std::clamp(field.timer / 0.28f, 0.f, 1.f);
            drawSpellFrame(AbilityType::IceUltimate, field.pos, field.radius * 2.f * grow, remainingFade);
        }
        else if (field.ability == AbilityType::ElectricSpread)
        {
            Vector2 delta = Vector2Subtract(field.strikePos, field.pos);
            float dist = Vector2Length(delta);
            Vector2 dir = dist > 0.f ? Vector2Scale(delta, 1.f / dist) : field.direction;
            int pieces = std::max(1, (int)(dist / 90.f));
            for (int i = 0; i <= pieces; ++i)
                drawSpellFrame(AbilityType::ElectricUltimate,
                    Vector2Add(field.pos, Vector2Scale(dir, dist * (float)i / pieces)),
                    115.f, remainingFade, atan2f(dir.y, dir.x) * RAD2DEG);
        }
        else if (field.ability == AbilityType::FireBolt)
        {
            float grow = 0.75f + 0.25f * std::clamp(field.timer / 0.16f, 0.f, 1.f);
            drawSpellFrame(AbilityType::FireUltimate, field.pos, field.radius * 2.f * grow, remainingFade);
        }
        else if (field.ability == AbilityType::ElectricBolt)
        {
            Vector2 delta = Vector2Subtract(field.strikePos, field.pos);
            float dist = Vector2Length(delta);
            Vector2 dir = dist > 0.f ? Vector2Scale(delta, 1.f / dist) : Vector2{ 1.f, 0.f };
            int pieces = std::max(1, (int)(dist / 70.f));
            for (int i = 0; i <= pieces; ++i)
                drawSpellFrame(AbilityType::ElectricUltimate,
                    Vector2Add(field.pos, Vector2Scale(dir, dist * (float)i / pieces)),
                    90.f, remainingFade, atan2f(dir.y, dir.x) * RAD2DEG);
        }
        else if (field.ability == AbilityType::FireUltimate)
        {
            if (!field.impacted)
            {
                Vector2 p = screenPos(field.pos);
                float pulse = 0.55f + 0.35f * sinf(field.timer * 15.f);
                DrawCircleLinesV(p, field.radius, Fade(ORANGE, pulse)); // warning only
                DrawCircleLinesV(p, field.radius * 0.72f, Fade(GOLD, pulse * 0.7f));
            }
            else
                drawSpellFrame(AbilityType::FireUltimate, field.pos, field.radius * 2.1f, remainingFade);
        }
        else if (field.ability == AbilityType::IceUltimate)
        {
            static const Vector2 offsets[] = { {0,0}, {-150,-80}, {145,-65}, {-100,130}, {125,120} };
            for (Vector2 off : offsets)
                drawSpellFrame(AbilityType::IceUltimate, Vector2Add(field.pos, off), 210.f, remainingFade);
        }
        else if (field.ability == AbilityType::ElectricUltimate)
        {
            drawSpellFrame(AbilityType::ElectricUltimate, field.pos, field.radius * 1.2f, remainingFade);
            if (field.strikeFlash > 0.f)
                drawSpellFrame(AbilityType::ElectricUltimate, field.strikePos, 210.f,
                               field.strikeFlash / 0.22f);
        }
    }
}

void Engine::SpawnUltimateBurst(AbilityType element)
{
    // Big elemental screen flash on the ult cast.
    Color flash = (element == AbilityType::IceUltimate)      ? Color{ 130, 210, 255, 255 }
                : (element == AbilityType::ElectricUltimate) ? Color{ 255, 240, 130, 255 }
                :                                              Color{ 255, 150, 70, 255 };
    TriggerScreenFlash(flash, 0.30f);

    // Fill the entire visible screen with blasts. Positions are computed in
    // screen space then converted to world space so they always cover the
    // full 1920x1080 view regardless of where the player is.
    const int   blastCount = 25;
    const float lifetime   = _ultCinematicDuration;
    const float margin     = 80.f;   // keep icons away from the very edge

    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    Vector2 playerPos = _player.GetWorldPos();

    for (int i = 0; i < blastCount; i++)
    {
        float sx = (float)GetRandomValue((int)margin, (int)(sw - margin));
        float sy = (float)GetRandomValue((int)margin, (int)(sh - margin));

        // Convert screen-space position to world-space so DrawUltimateBlasts
        // maps it back to the same screen pixel via the worldOffset calculation.
        Vector2 pos = {
            playerPos.x + sx - sw * 0.5f,
            playerPos.y + sy - sh * 0.5f
        };

        // Angle each blast outward from the screen center so they look like
        // they're radiating away from the player.
        float rotation = atan2f(sy - sh * 0.5f, sx - sw * 0.5f) * RAD2DEG;

        UltimateBlast blast;
        blast.worldPos  = pos;
        blast.element   = element;
        blast.lifetime  = lifetime;
        blast.timer     = lifetime;
        blast.rotation  = rotation;
        _ultimateBlasts.push_back(blast);
    }

    TriggerScreenShake(6.f, 0.3f);
}

void Engine::UpdateUltimateBlasts(float dt)
{
    // Purely visual during the cinematic phase - damage is applied all at once
    // by ApplyUltimateImpact at the Impact phase transition.
    for (auto& blast : _ultimateBlasts)
        blast.timer -= dt;

    _ultimateBlasts.erase(
        std::remove_if(_ultimateBlasts.begin(), _ultimateBlasts.end(),
            [](const UltimateBlast& b) { return b.timer <= 0.f; }),
        _ultimateBlasts.end());
}

// ═════════════════════════════════════════════════════════════════════════════
// CLASS ABILITIES — Warrior kit (and the shared dispatch for future classes)
// Each ability applies its damage/effects instantly on cast; the WarriorVfx list
// only animates the swing. Damage scales with the player's attack power, the
// ability's learned level (1–3), and any active self-buff (War Cry / Rampage).
// ═════════════════════════════════════════════════════════════════════════════
int Engine::DamageEnemiesInRect(Rectangle worldRect, int damage, float knockback,
                                float stunSeconds, float bleedSeconds, int bleedDmgPerTick,
                                bool ignoreShield)
{
    (void)knockback;   // knockback comes from TakeDamage using the player position
    int hitCount = 0;
    Vector2 playerPos = _player.GetWorldPos();

    // Every damaging ability rect also chips destructible room hazards, so
    // "disable the totem" works with the player's whole kit, not just melee.
    _roomHazards.DamageHazardsInRect(worldRect, damage);

    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive() || !enemy->IsAlive())
            continue;
        if (!CheckCollisionRecs(worldRect, enemy->GetHitCollisionRec()))
            continue;

        bool crit = false;
        int dmg = ScalePlayerHit(*enemy, std::max(1, damage), crit);
        const float healthBefore = enemy->GetHealthValue();
        // ignoreShield lets the Hunter's Puncture Shot bypass a Shieldbearer's
        // frontal block (and any future block); normal hits respect the shield.
        if (ignoreShield) enemy->TakeDamageUnblockable(dmg, playerPos);
        else              enemy->TakeDamage(dmg, playerPos);

        // Hit denied by a shield / i-frames? Show the reason instead of a
        // phantom damage number, and skip lifesteal + status (nothing landed).
        Enemy::HitBlockReason blk = enemy->ConsumeHitBlock();
        if (blk != Enemy::HitBlockReason::None)
        {
            ShowBlockedHitFeedback(*enemy, blk);
            continue;
        }

        const int actualDamage = RegisterHitFx(*enemy, healthBefore, crit, false, false, 2u);
        if (actualDamage <= 0)
            continue;
        ApplyPlayerLifesteal(actualDamage);
        _player.AddRage(4.f);         // Warrior: ability hits build Rage per enemy struck (no-op for others)
        _player.AddComboPoints(1);    // Rogue: ability hits bank combo per enemy struck (no-op for others)
        _player.AddFaith(3.f);        // Paladin: holy hits build Faith per enemy struck (no-op for others)

        if (stunSeconds  > 0.f) enemy->ApplyFreeze(stunSeconds);          // stun ≈ frozen hold
        if (bleedSeconds > 0.f) enemy->ApplyBleed(bleedDmgPerTick, bleedSeconds);  // real bleed DoT (shared status)
        hitCount++;
    }
    return hitCount;
}

// ── Class-ability status stamps ──────────────────────────────────────────────
// Apply a shared status to every enemy overlapping a world rect. Class abilities
// call these right after their DamageEnemiesInRect so each class delivers its
// signature effect (Warrior armor-break, Rogue poison, Hunter mark, etc.).
static bool OverlapsDirectedBox(Rectangle hit, Vector2 origin, Vector2 direction,
                                float length, float width)
{
    if (Vector2Length(direction) < 0.001f)
        direction = Vector2{ 1.f, 0.f };
    direction = Vector2Normalize(direction);
    Vector2 side{ -direction.y, direction.x };
    Vector2 centre{ hit.x + hit.width * 0.5f, hit.y + hit.height * 0.5f };
    Vector2 delta = Vector2Subtract(centre, origin);

    const float along = Vector2DotProduct(delta, direction);
    const float lateral = fabsf(Vector2DotProduct(delta, side));
    const float alongExtent = fabsf(direction.x) * hit.width * 0.5f +
                              fabsf(direction.y) * hit.height * 0.5f;
    const float sideExtent = fabsf(side.x) * hit.width * 0.5f +
                             fabsf(side.y) * hit.height * 0.5f;
    return along + alongExtent >= 0.f && along - alongExtent <= length &&
           lateral <= width * 0.5f + sideExtent;
}

static Rectangle DirectedBoxBounds(Vector2 origin, Vector2 direction, float length, float width)
{
    if (Vector2Length(direction) < 0.001f)
        direction = Vector2{ 1.f, 0.f };
    direction = Vector2Normalize(direction);
    Vector2 side = Vector2Scale(Vector2{ -direction.y, direction.x }, width * 0.5f);
    Vector2 end = Vector2Add(origin, Vector2Scale(direction, length));
    Vector2 points[] = { Vector2Add(origin, side), Vector2Subtract(origin, side),
                         Vector2Add(end, side), Vector2Subtract(end, side) };
    float minX = points[0].x, maxX = points[0].x;
    float minY = points[0].y, maxY = points[0].y;
    for (int i = 1; i < 4; ++i)
    {
        minX = std::min(minX, points[i].x); maxX = std::max(maxX, points[i].x);
        minY = std::min(minY, points[i].y); maxY = std::max(maxY, points[i].y);
    }
    return Rectangle{ minX, minY, maxX - minX, maxY - minY };
}

int Engine::DamageEnemiesInDirectedBox(Vector2 origin, Vector2 direction, float length, float width,
                                       int damage, float knockback, float stunSeconds,
                                       float bleedSeconds, int bleedDmgPerTick, bool ignoreShield)
{
    (void)knockback;
    int hitCount = 0;
    Vector2 playerPos = _player.GetWorldPos();
    _roomHazards.DamageHazardsInRect(DirectedBoxBounds(origin, direction, length, width), damage);

    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive() || !enemy->IsAlive() ||
            !OverlapsDirectedBox(enemy->GetHitCollisionRec(), origin, direction, length, width))
            continue;

        bool crit = false;
        int dmg = ScalePlayerHit(*enemy, std::max(1, damage), crit);
        const float healthBefore = enemy->GetHealthValue();
        if (ignoreShield) enemy->TakeDamageUnblockable(dmg, playerPos);
        else              enemy->TakeDamage(dmg, playerPos);

        Enemy::HitBlockReason blocked = enemy->ConsumeHitBlock();
        if (blocked != Enemy::HitBlockReason::None)
        {
            ShowBlockedHitFeedback(*enemy, blocked);
            continue;
        }

        const int actualDamage = RegisterHitFx(*enemy, healthBefore, crit, false, false, 3u);
        if (actualDamage <= 0)
            continue;
        ApplyPlayerLifesteal(actualDamage);
        _player.AddRage(4.f);
        _player.AddComboPoints(1);
        _player.AddFaith(3.f);
        if (stunSeconds > 0.f) enemy->ApplyFreeze(stunSeconds);
        if (bleedSeconds > 0.f) enemy->ApplyBleed(bleedDmgPerTick, bleedSeconds);
        ++hitCount;
    }
    return hitCount;
}

template <typename Effect>
static void ApplyInDirectedBox(std::vector<std::unique_ptr<Enemy>>& enemies,
                               Vector2 origin, Vector2 direction, float length, float width,
                               Effect effect)
{
    for (auto& enemy : enemies)
        if (enemy->IsActive() && enemy->IsAlive() &&
            OverlapsDirectedBox(enemy->GetHitCollisionRec(), origin, direction, length, width))
            effect(*enemy);
}

static void ApplyPoisonInRect(std::vector<std::unique_ptr<Enemy>>& es, Rectangle r, int dmgPerTick, float sec)
{
    for (auto& e : es)
        if (e->IsActive() && e->IsAlive() && CheckCollisionRecs(r, e->GetHitCollisionRec()))
            e->ApplyPoison(dmgPerTick, sec);
}
static void ApplyVulnInRect(std::vector<std::unique_ptr<Enemy>>& es, Rectangle r, float mult, float sec)
{
    for (auto& e : es)
        if (e->IsActive() && e->IsAlive() && CheckCollisionRecs(r, e->GetHitCollisionRec()))
            e->ApplyVulnerability(mult, sec);
}
static void ApplySlowInRect(std::vector<std::unique_ptr<Enemy>>& es, Rectangle r, float mult, float sec)
{
    for (auto& e : es)
        if (e->IsActive() && e->IsAlive() && CheckCollisionRecs(r, e->GetHitCollisionRec()))
            e->ApplySlow(mult, sec);
}
static void ApplyMarkInRect(std::vector<std::unique_ptr<Enemy>>& es, Rectangle r, float sec)
{
    for (auto& e : es)
        if (e->IsActive() && e->IsAlive() && CheckCollisionRecs(r, e->GetHitCollisionRec()))
            e->ApplyMark(sec);
}
// Warlock curse tag — cursed enemies take bonus Warlock damage (ScalePlayerHit).
static void ApplyCurseInRect(std::vector<std::unique_ptr<Enemy>>& es, Rectangle r, float sec)
{
    for (auto& e : es)
        if (e->IsActive() && e->IsAlive() && CheckCollisionRecs(r, e->GetHitCollisionRec()))
            e->ApplyCurse(sec);
}
// Burn (fire DoT) via the existing burn queue — stack a few delayed bursts so the
// target visibly "catches fire" instead of taking one hit.
static void ApplyBurnInRect(std::vector<std::unique_ptr<Enemy>>& es, Rectangle r, int dmgPerTick, int ticks, Vector2 src)
{
    for (auto& e : es)
        if (e->IsActive() && e->IsAlive() && CheckCollisionRecs(r, e->GetHitCollisionRec()))
            for (int i = 1; i <= ticks; ++i)
                e->ApplyBurn(0.4f * i, dmgPerTick, src);
}

void Engine::ApplyPlayerLifesteal(int damageDealt)
{
    if (!_player.IsLifestealActive() || damageDealt <= 0)
        return;
    // Sustain rework: lifesteal heals at most kLifestealCapPerRoom HP per room.
    // Lifesteal is a % of damage dealt, so without a cap every damage upgrade
    // silently buys more healing — this bounds that loop without touching the
    // class-identity grants (Rampage / Blade Dance / Drain Life / Demon Form).
    if (_roomLifestealHealed >= Balance::Sustain::kLifestealCapPerRoom)
        return;
    _lifestealAccum += damageDealt * _player.GetLifestealFraction();
    if (_lifestealAccum >= 1.f)
    {
        int heal = (int)_lifestealAccum;
        _lifestealAccum -= (float)heal;
        heal = std::min(heal, Balance::Sustain::kLifestealCapPerRoom - _roomLifestealHealed);
        _player.Heal(heal);
        _roomLifestealHealed += heal;
    }
}

void Engine::HandleClassAbilityCast(AbilityType ability)
{
    Vector2 playerPos = _player.GetWorldPos();
    Vector2 facing = (_committedAimAbility == ability)
        ? _committedAimDirection : _player.GetFacingDirection();
    if (Vector2Length(facing) < 0.001f)
        facing = _player.GetFacingDirection();
    facing = Vector2Normalize(facing);
    float facingSign = fabsf(facing.x) > 0.05f
        ? (facing.x >= 0.f ? 1.f : -1.f)
        : (_player.GetFacingDirection().x >= 0.f ? 1.f : -1.f);
    Vector2 castTarget = (_committedAimAbility == ability)
        ? _committedAimTarget : Vector2Add(playerPos, Vector2Scale(facing, 200.f));
    AbilityAimProfile aimProfile = GetAbilityAimProfile(ability);
    float   atk   = _player.GetAttackPowerValue();
    int     level = std::max(1, _player.GetAbilityLevel(ability));
    float   buff  = _player.GetClassDamageMult();

    // Warrior Rage: abilities hit harder as the bar fills (base +50% at full;
    // the Furious Might class card raises the ceiling).
    buff *= 1.f + _player.GetRageFullBonus() * _player.GetRagePercent();

    // Paladin Faith: abilities hit up to +40% harder at full Faith — conviction
    // built by holding ground pays out through holy power (see Character::AddFaith).
    buff *= 1.f + 0.4f * _player.GetFaithPercent();

    // Overload Power Choice: permanent ability damage bought at +1 mana cost —
    // the tradeoff card, so it multiplies here rather than adding attack power.
    buff *= _player.GetAbilityPowerMult();

    // Helper to scale a base value by ability level and the active damage buff.
    auto dmgVal = [&](float base) {
        float levelMult = 1.f + Balance::AbilityDamage::kClassLevelStep * (level - 1);
        return std::max(1, (int)roundf(base * levelMult * buff));
    };
    // Class-card dials: poison tick potency (Toxin Expert) and curse window
    // length (Lingering Malice). Both default to 1.0 for unaffected classes.
    auto poisonVal = [&](float base) {
        return std::max(1, (int)roundf(dmgVal(base) * _player.GetPoisonPotency()));
    };
    const float curseDur = _player.GetCurseDurationMult();

    // Attack Editor override: if a hitbox was saved for this ability, use it
    // (centred at the box offset from the player, mirrored by facing). No file =
    // the original hardcoded rect below, so gameplay is unchanged until tuned.
    const AttackTuning* atkTune = AttackTuningStore::Get(AttackTuningKeyForAbility(ability));
    auto tunedOr = [&](Rectangle def) -> Rectangle {
        if (atkTune && atkTune->hasBox)
        {
            float cx = playerPos.x + facingSign * atkTune->x;
            float cy = playerPos.y + atkTune->y;
            return Rectangle{ cx - atkTune->w * 0.5f, cy - atkTune->h * 0.5f, atkTune->w, atkTune->h };
        }
        return def;
    };

    // Builds a forward rectangle starting at the player, extending in facing dir.
    auto forwardRect = [&](float length, float height) -> Rectangle {
        float x = (facingSign > 0.f) ? playerPos.x : playerPos.x - length;
        return tunedOr(Rectangle{ x, playerPos.y - height * 0.5f, length, height });
    };
    auto radialRect = [&](float radius) -> Rectangle {
        return tunedOr(Rectangle{ playerPos.x - radius, playerPos.y - radius, radius * 2.f, radius * 2.f });
    };
    auto directedLength = [&](float fallback) {
        return aimProfile.length > 0.f ? aimProfile.length : fallback;
    };
    auto directedWidth = [&](float fallback) {
        return aimProfile.width > 0.f ? aimProfile.width : fallback;
    };
    auto directedGeometry = [&](float fallbackLength, float fallbackWidth,
                                Vector2& origin, float& length, float& width) {
        length = directedLength(fallbackLength);
        width = directedWidth(fallbackWidth);
        origin = playerPos;
        // Legacy authored boxes remain authoritative until the newer Ability
        // Lab geometry is explicitly saved for this move. Rotate their local
        // forward/side offsets with the aim direction instead of discarding them.
        if (atkTune && atkTune->hasBox && !atkTune->hasAbility)
        {
            Vector2 side{ -facing.y, facing.x };
            length = atkTune->w;
            width = atkTune->h;
            origin = Vector2Add(playerPos,
                Vector2Add(Vector2Scale(facing, atkTune->x - length * 0.5f),
                           Vector2Scale(side, atkTune->y)));
        }
    };
    auto damageDirected = [&](float length, float width, int damage, float knockback,
                              float stun, float bleed, int bleedTick, bool ignoreShield = false) {
        Vector2 origin{}; float actualLength = 0.f, actualWidth = 0.f;
        directedGeometry(length, width, origin, actualLength, actualWidth);
        return DamageEnemiesInDirectedBox(origin, facing, actualLength, actualWidth, damage,
                                          knockback, stun, bleed, bleedTick, ignoreShield);
    };
    auto applyDirected = [&](float length, float width, auto effect) {
        Vector2 origin{}; float actualLength = 0.f, actualWidth = 0.f;
        directedGeometry(length, width, origin, actualLength, actualWidth);
        ApplyInDirectedBox(_enemies, origin, facing, actualLength, actualWidth, effect);
    };
    auto targetArea = [&](float fallbackRadius) {
        float radius = aimProfile.radius > 0.f ? aimProfile.radius : fallbackRadius;
        return Rectangle{ castTarget.x - radius, castTarget.y - radius, radius * 2.f, radius * 2.f };
    };
    // Animator hookups for the remaining Ability Lab rows: self-centred blast
    // radius, zone/trap lifetime, and zone tick cadence. Untuned = the same
    // hardcoded fallbacks as before.
    auto tunedRadius = [&](float fallbackRadius) {
        return aimProfile.radius > 0.f ? aimProfile.radius : fallbackRadius;
    };
    auto durationOr = [&](float fallbackDuration) {
        return (atkTune && atkTune->hasAbility) ? atkTune->effectDuration : fallbackDuration;
    };
    auto tickOr = [&](float fallbackInterval) {
        return (atkTune && atkTune->hasAbility) ? atkTune->tickInterval : fallbackInterval;
    };

    // Whether this ability has a real owned FX sheet (FX_<Ability>.png). If so,
    // SpawnAbilityFx (called at the end) plays it and the prototype shape below is
    // suppressed — the raylib shapes are now only a FALLBACK for abilities lacking art.
    const bool hasFxSheet = ((int)ability >= 0 && (int)ability < (int)AbilityType::Count &&
                             _abilityFx[(int)ability].id != 0 && _abilityFxFrames[(int)ability] > 0);

    auto pushVfx = [&](WarriorVfxKind kind, float lifetime, float radius, Color tint) {
        if (hasFxSheet) return;   // owned sprite FX replaces the prototype shape
        WarriorVfx v; v.kind = kind; v.pos = playerPos; v.dir = facing;
        v.timer = 0.f; v.lifetime = lifetime; v.radius = radius; v.tint = tint;
        _warriorVfx.push_back(v);
    };
    // Thrown/fired projectile that flies the ability's icon sprite forward.
    auto pushShot = [&](float lifetime, float dist, AbilityType a, bool spin) {
        WarriorVfx v; v.kind = WarriorVfxKind::Axe; v.pos = playerPos; v.dir = facing;
        v.timer = 0.f; v.lifetime = lifetime; v.radius = dist; v.tint = WHITE;
        v.sprite = GetAbilityIcon(a); v.spin = spin;
        _warriorVfx.push_back(v);
    };

    StopSound(_fireballCastSound);
    PlaySound(_fireballCastSound);

    switch (ability)
    {
    case AbilityType::WarCleave:
    {
        int hits = damageDirected(210.f, 200.f, dmgVal(4.f + atk * 0.8f), 1.f, 0.f, 0.f, 0);
        applyDirected(210.f, 200.f, [](Enemy& enemy) { enemy.ApplyVulnerability(1.25f, 4.f); });
        pushVfx(WarriorVfxKind::Wave, 0.28f, 210.f, Color{ 200, 235, 255, 255 });
        if (hits) TriggerScreenShake(6.f, 0.10f);
        break;
    }
    case AbilityType::Whirlwind:
    {
        float whirlRadius = tunedRadius(170.f);
        DamageEnemiesInRect(radialRect(whirlRadius), dmgVal(3.f + atk * 0.6f), 1.f, 0.f, 2.5f, dmgVal(0.6f));   // spinning blades bleed
        pushVfx(WarriorVfxKind::Whirl, 0.35f, whirlRadius, Color{ 235, 235, 245, 255 });
        TriggerScreenShake(5.f, 0.12f);
        break;
    }
    case AbilityType::ThrowingAxe:
    {
        damageDirected(520.f, 90.f, dmgVal(5.f + atk * 1.0f), 1.f, 0.f, 3.f, dmgVal(0.8f));
        pushShot(0.4f, 520.f, AbilityType::ThrowingAxe, true);
        break;
    }
    case AbilityType::Rend:
    {
        float moveDistance = (atkTune && atkTune->hasAbility) ? atkTune->moveDistance : 120.f;
        _player.MoveAlongDirection(facing, moveDistance);
        playerPos = _player.GetWorldPos();
        damageDirected(150.f, 140.f, dmgVal(3.f + atk * 0.6f), 1.f, 0.f, 3.f, dmgVal(1.f));
        pushVfx(WarriorVfxKind::Wave, 0.25f, 150.f, Color{ 220, 60, 60, 255 });
        break;
    }
    case AbilityType::ShieldBash:
    {
        float moveDistance = (atkTune && atkTune->hasAbility) ? atkTune->moveDistance : 90.f;
        _player.MoveAlongDirection(facing, moveDistance);
        playerPos = _player.GetWorldPos();
        damageDirected(150.f, 150.f, dmgVal(2.f + atk * 0.4f), 1.f, 1.3f, 0.f, 0);
        pushVfx(WarriorVfxKind::Bash, 0.25f, 150.f, Color{ 255, 240, 190, 255 });
        TriggerScreenShake(7.f, 0.10f);
        break;
    }
    case AbilityType::WarCry:
    {
        // Identity: the roar is a RAGE battery + toughness, not just flat damage —
        // +30 Rage, one armour slot (absorbs the next hit), and the damage buff.
        _player.GrantDamageBuff(1.4f, 6.f);
        _player.AddRage(30.f);
        _player.AddArmour(1);
        _player.Heal(2);
        _vfx.SpawnFloatingLabel(playerPos, "RAGE!", Color{ 255, 140, 40, 255 }, 1.4f);
        pushVfx(WarriorVfxKind::Cry, 0.6f, 160.f, Color{ 255, 180, 60, 255 });
        TriggerScreenShake(4.f, 0.15f);
        break;
    }
    case AbilityType::GroundSlam:
    {
        // Identity payoff: the quake CONSUMES all Rage — up to +45% radius at a
        // full bar (the damage itself already scaled with Rage via `buff` above,
        // so the radius is the spend reward and avoids double-dipping).
        float rageFraction = _player.ConsumeAllRage();
        float slamRadius   = tunedRadius(340.f) * (1.f + 0.45f * rageFraction);
        DamageEnemiesInRect(radialRect(slamRadius), dmgVal(7.f + atk * 1.5f), 1.f, 1.5f, 0.f, 0);
        ApplyVulnInRect(_enemies, radialRect(slamRadius), 1.3f, 5.f);   // quake cracks armor
        pushVfx(WarriorVfxKind::Slam, 0.5f, slamRadius, Color{ 235, 200, 120, 255 });
        StopSound(_explosionSound);
        PlaySound(_explosionSound);
        TriggerScreenShake(14.f, 0.4f);
        break;
    }
    case AbilityType::Rampage:
    {
        _player.GrantDamageBuff(2.0f, 7.f);
        _player.GrantLifesteal(0.25f, 7.f);
        pushVfx(WarriorVfxKind::Cry, 0.7f, 180.f, Color{ 255, 80, 80, 255 });
        TriggerScreenShake(8.f, 0.25f);
        break;
    }
    case AbilityType::Earthshatter:
    {
        damageDirected(640.f, 130.f, dmgVal(10.f + atk * 2.0f), 1.f, 0.6f, 0.f, 0);
        applyDirected(640.f, 130.f, [](Enemy& enemy) { enemy.ApplyVulnerability(1.35f, 5.f); });
        pushVfx(WarriorVfxKind::Spikes, 0.55f, 640.f, Color{ 180, 130, 70, 255 });
        StopSound(_explosionSound);
        PlaySound(_explosionSound);
        TriggerScreenShake(12.f, 0.35f);
        break;
    }

    // ── ROGUE ────────────────────────────────────────────────────────────────
    case AbilityType::FanOfKnives:
    {
        damageDirected(360.f, 240.f, dmgVal(4.f + atk * 0.7f), 1.f, 0.f, 0.f, 0);
        int poison = poisonVal(1.f);
        applyDirected(360.f, 240.f, [=](Enemy& enemy) { enemy.ApplyPoison(poison, 4.f); });
        pushVfx(WarriorVfxKind::Fan, 0.3f, 360.f, Color{ 220, 225, 235, 255 });
        break;
    }
    case AbilityType::Shadowstep:
    {
        // Cut everything in the corridor ahead, then blink to the far end.
        damageDirected(300.f, 110.f, dmgVal(3.f + atk * 0.6f), 1.f, 0.f, 2.5f, dmgVal(0.7f));
        pushVfx(WarriorVfxKind::Teleport, 0.3f, 40.f, Color{ 150, 90, 220, 255 });   // puff at origin
        float moveDistance = (atkTune && atkTune->hasAbility) ? atkTune->moveDistance : 300.f;
        _player.MoveAlongDirection(facing, moveDistance);
        WarriorVfx dst; dst.kind = WarriorVfxKind::Teleport; dst.pos = _player.GetWorldPos();
        dst.dir = facing; dst.lifetime = 0.3f; dst.radius = 40.f; dst.tint = Color{ 150, 90, 220, 255 };
        _warriorVfx.push_back(dst);
        break;
    }
    case AbilityType::PoisonVial:
    {
        // Lobbed to a spot ahead; leaves a pool that ticks enemies over time.
        Vector2 spot = castTarget;
        WarriorVfx pool; pool.kind = WarriorVfxKind::PoisonZone; pool.pos = spot; pool.dir = facing;
        pool.lifetime = durationOr(3.5f); pool.radius = tunedRadius(130.f);
        pool.tint = Color{ 120, 210, 90, 255 };
        pool.tickDamage = dmgVal(1.f + atk * 0.2f); pool.tickInterval = tickOr(0.4f);
        // Reuse the owned animated poison-pool decal for the complete hazard
        // lifetime. An fxStrip bypasses PoisonZone's prototype Raylib circles;
        // SpawnAbilityFx still supplies the separate vial-impact eruption.
        const int poolFx = (int)BossFx::PoisonPool;
        if (poolFx < (int)_bossFx.size() && _bossFx[poolFx].id != 0 && _bossFxFrames[poolFx] > 0)
        {
            pool.fxStrip = &_bossFx[poolFx];
            pool.fxFrames = _bossFxFrames[poolFx];
            pool.fxScale = (pool.radius * 2.f) / 64.f;
            pool.fxLoop = true;
            pool.fxFrameTime = 1.f / 12.f;
        }
        _warriorVfx.push_back(pool);
        ApplyPoisonInRect(_enemies, targetArea(130.f), poisonVal(1.f), 5.f);
        break;
    }
    case AbilityType::Backstab:
    {
        // A real backstab now: every target in the strike box takes a solid
        // base hit, but the big multiplier, bleed, and expose land ONLY when
        // the player is actually inside the target's rear cone. Positioning
        // (and the new enemy facing commitment) is what unlocks the payoff.
        int struck = 0;
        int rearStrikes = 0;
        for (auto& enemy : _enemies)
        {
            if (!enemy->IsActive() || !enemy->IsAlive())
                continue;
            Vector2 strikeOrigin{}; float strikeLength = 0.f, strikeWidth = 0.f;
            directedGeometry(150.f, 120.f, strikeOrigin, strikeLength, strikeWidth);
            if (!OverlapsDirectedBox(enemy->GetHitCollisionRec(), strikeOrigin, facing,
                                     strikeLength, strikeWidth))
                continue;

            bool rearHit = enemy->IsPositionBehind(playerPos, Balance::Facing::kRearConeDot);
            int baseDamage = dmgVal(4.f + atk * 0.9f);
            int damage = rearHit
                ? std::max(1, (int)roundf(baseDamage * Balance::Facing::kBackstabRearMult))
                : baseDamage;

            bool crit = false;
            damage = ScalePlayerHit(*enemy, std::max(1, damage), crit);
            const float healthBefore = enemy->GetHealthValue();
            enemy->TakeDamage(damage, playerPos);

            Enemy::HitBlockReason blocked = enemy->ConsumeHitBlock();
            if (blocked != Enemy::HitBlockReason::None)
            {
                ShowBlockedHitFeedback(*enemy, blocked);
                if (_debug.IsActive())
                    TraceLog(LOG_INFO, "BACKSTAB blocked (%s side)", rearHit ? "rear?!" : "front");
                continue;
            }

            const int actualDamage = RegisterHitFx(*enemy, healthBefore, crit, rearHit, false, 4u);
            if (actualDamage <= 0)
                continue;
            ApplyPlayerLifesteal(actualDamage);
            _player.AddRage(4.f);
            _player.AddComboPoints(1);
            _player.AddFaith(3.f);
            if (rearHit)
            {
                enemy->ApplyBleed(dmgVal(1.f), 3.f);        // deep wound bleeds
                enemy->ApplyVulnerability(1.3f, 4.f);       // exposes the victim
                rearStrikes++;
            }
            struck++;
            if (_debug.IsActive())
                TraceLog(LOG_INFO, "BACKSTAB %s: dmg %d", rearHit ? "REAR success" : "front/side (base dmg)", damage);
        }
        // Identity: a TRUE backstab banks two extra combo points on top of the
        // per-hit point, setting up Eviscerate's payoff.
        if (rearStrikes > 0) _player.AddComboPoints(2);
        pushVfx(WarriorVfxKind::Flurry, 0.22f, 150.f, Color{ 255, 90, 90, 255 });
        TriggerScreenShake(6.f, 0.10f);
        break;
    }
    case AbilityType::SmokeBomb:
    {
        float smokeRadius = tunedRadius(240.f);
        DamageEnemiesInRect(radialRect(smokeRadius), 0, 0.f, 1.2f, 0.f, 0);   // slow nearby (stun≈hold)
        ApplySlowInRect(_enemies, radialRect(smokeRadius), 0.5f, 3.5f);       // lingering blinding smoke slows
        _player.GrantDamageBuff(1.6f, 5.f);                            // ambush from the smoke
        pushVfx(WarriorVfxKind::Smoke, 0.6f, smokeRadius, Color{ 150, 150, 160, 255 });
        break;
    }
    case AbilityType::Eviscerate:
    {
        // Identity payoff: the FINISHER spends every banked combo point for up to
        // +75% damage at 5 points (see Character::AddComboPoints). Spent BEFORE
        // the hit so the flurry that lands doesn't refill what it just consumed.
        int   combo      = _player.ConsumeAllComboPoints();
        float finishMult = 1.f + _player.GetEvisceratePerCombo() * combo;
        if (combo >= 3)
            _vfx.SpawnFloatingLabel(playerPos, TextFormat("FINISHER x%d", combo),
                                    Color{ 235, 235, 245, 255 }, 1.3f);
        damageDirected(210.f, 130.f,
            std::max(1, (int)roundf(dmgVal(5.f + atk * 1.1f) * finishMult)),
            1.f, 0.f, 3.f, dmgVal(0.9f));   // flurry of cuts bleeds
        pushVfx(WarriorVfxKind::Flurry, 0.3f, 210.f, Color{ 235, 235, 245, 255 });
        break;
    }
    case AbilityType::DeathMark:
    {
        float markRadius = tunedRadius(1200.f);
        DamageEnemiesInRect(radialRect(markRadius), dmgVal(9.f + atk * 1.8f), 1.f, 0.f, 0.f, 0);
        ApplyMarkInRect(_enemies, radialRect(markRadius), 6.f);   // marks the whole screen for execution
        ApplyVulnInRect(_enemies, radialRect(markRadius), 1.3f, 6.f);
        pushVfx(WarriorVfxKind::Marks, 0.6f, markRadius, Color{ 255, 70, 70, 255 });
        StopSound(_explosionSound);
        PlaySound(_explosionSound);
        TriggerScreenShake(12.f, 0.35f);
        break;
    }
    case AbilityType::BladeDance:
    {
        float danceRadius = tunedRadius(230.f);
        DamageEnemiesInRect(radialRect(danceRadius), dmgVal(6.f + atk * 1.2f), 1.f, 0.f, 0.f, 0);
        _player.GrantDamageBuff(1.8f, 6.f);
        _player.GrantLifesteal(0.3f, 6.f);
        pushVfx(WarriorVfxKind::Whirl, 0.5f, danceRadius, Color{ 255, 120, 120, 255 });
        TriggerScreenShake(8.f, 0.25f);
        break;
    }
    case AbilityType::RainOfBlades:
    {
        Rectangle area = targetArea(220.f);
        DamageEnemiesInRect(area, dmgVal(9.f + atk * 1.7f), 1.f, 0.f, 0.f, 0);
        WarriorVfx barrage; barrage.kind = WarriorVfxKind::Barrage; barrage.pos = castTarget;
        barrage.dir = facing; barrage.lifetime = 0.6f; barrage.radius = aimProfile.radius;
        barrage.tint = Color{ 220, 225, 240, 255 }; _warriorVfx.push_back(barrage);
        StopSound(_explosionSound);
        PlaySound(_explosionSound);
        TriggerScreenShake(11.f, 0.35f);
        break;
    }

    // ── HUNTER ───────────────────────────────────────────────────────────────
    case AbilityType::PiercingShot:
    {
        damageDirected(600.f, 70.f, dmgVal(5.f + atk * 1.1f), 1.f, 0.f, 2.5f, dmgVal(0.8f));
        applyDirected(600.f, 70.f, [](Enemy& enemy) { enemy.ApplyMark(6.f); });
        break;   // comet FX added by SpawnAbilityFx
    }
    case AbilityType::Multishot:
    {
        damageDirected(380.f, 260.f, dmgVal(3.f + atk * 0.6f), 1.f, 0.f, 0.f, 0);
        int poison = poisonVal(1.f);
        applyDirected(380.f, 260.f, [=](Enemy& enemy) { enemy.ApplyPoison(poison, 4.f); });
        pushVfx(WarriorVfxKind::Fan, 0.3f, 380.f, Color{ 140, 235, 210, 255 });
        break;
    }
    case AbilityType::FrostTrap:   // Hunter FREEZING TRAP — armed, snaps to freeze
    {
        // Drop an armed trap at your feet. It arms after a beat, then snaps when a
        // foe steps on it: a light burst plus a hard freeze on everything nearby.
        WarriorVfx trap; trap.kind = WarriorVfxKind::Trap; trap.pos = castTarget; trap.dir = facing;
        trap.isTrap = true; trap.armDelay = 0.5f; trap.lifetime = durationOr(15.f);   // sits armed up to 15s
        trap.radius = tunedRadius(175.f);
        trap.triggerRadius = 72.f;      // enemy contact distance that trips it
        trap.trapDamage = dmgVal(2.f + atk * 0.3f);
        trap.trapFreeze = 2.2f;         // hard freeze duration on snap
        trap.tint = Color{ 120, 210, 255, 255 };
        _warriorVfx.push_back(trap);
        break;
    }
    case AbilityType::ExplosiveArrow:   // Hunter EXPLOSIVE TRAP — armed, snaps for AoE
    {
        // Armed trap at your feet: when a foe trips it, a big blast + knockback.
        WarriorVfx trap; trap.kind = WarriorVfxKind::Trap; trap.pos = castTarget; trap.dir = facing;
        trap.isTrap = true; trap.armDelay = 0.5f; trap.lifetime = durationOr(15.f);
        trap.radius = tunedRadius(195.f);
        trap.triggerRadius = 72.f;
        trap.trapDamage = dmgVal(7.f + atk * 1.3f);
        trap.trapKnockback = 1.5f;
        trap.tint = Color{ 255, 150, 60, 255 };
        _warriorVfx.push_back(trap);
        break;
    }
    case AbilityType::Roll:
    {
        float moveDistance = (atkTune && atkTune->hasAbility) ? atkTune->moveDistance : 240.f;
        _player.MoveAlongDirection(facing, moveDistance);
        playerPos = _player.GetWorldPos();
        _player.GrantDamageBuff(1.4f, 3.f);   // brief "deadeye" after repositioning
        pushVfx(WarriorVfxKind::Teleport, 0.3f, 40.f, Color{ 200, 235, 220, 255 });
        break;
    }
    case AbilityType::Volley:   // Hunter PUNCTURE SHOT — shield-piercing heavy arrow
    {
        // A heavy armour-piercing arrow: ignores a Shieldbearer's frontal block and
        // hits harder than a normal shot, so shielded foes can't wall you out.
        damageDirected(660.f, 84.f, dmgVal(6.f + atk * 1.4f), 1.f, 0.f, 0.f, 0, true);
        applyDirected(660.f, 84.f, [](Enemy& enemy) {
            enemy.ApplyVulnerability(1.4f, 5.f);
            enemy.ApplyMark(5.f);
        });
        pushVfx(WarriorVfxKind::Spikes, 0.4f, 660.f, Color{ 235, 245, 205, 255 });
        break;
    }
    case AbilityType::ArrowStorm:
    {
        Rectangle area = targetArea(360.f);
        DamageEnemiesInRect(area, dmgVal(8.f + atk * 1.6f), 1.f, 0.f, 0.f, 0);
        ApplyPoisonInRect(_enemies, area, poisonVal(1.f), 4.f);
        WarriorVfx barrage; barrage.kind = WarriorVfxKind::Barrage; barrage.pos = castTarget;
        barrage.dir = facing; barrage.lifetime = 0.6f; barrage.radius = aimProfile.radius;
        barrage.tint = Color{ 140, 235, 210, 255 }; _warriorVfx.push_back(barrage);
        StopSound(_explosionSound); PlaySound(_explosionSound);
        TriggerScreenShake(11.f, 0.35f);
        break;
    }
    case AbilityType::Deadeye:
    {
        _player.GrantDamageBuff(2.2f, 8.f);
        pushVfx(WarriorVfxKind::Cry, 0.7f, 180.f, Color{ 150, 255, 220, 255 });
        TriggerScreenShake(6.f, 0.2f);
        break;
    }
    case AbilityType::PiercingBarrage:
    {
        damageDirected(900.f, 150.f, dmgVal(10.f + atk * 1.9f), 1.f, 0.f, 3.f, dmgVal(1.f));
        applyDirected(900.f, 150.f, [](Enemy& enemy) { enemy.ApplyMark(5.f); });
        pushVfx(WarriorVfxKind::Spikes, 0.55f, 900.f, Color{ 140, 235, 210, 255 });
        StopSound(_explosionSound); PlaySound(_explosionSound);
        TriggerScreenShake(12.f, 0.35f);
        break;
    }

    // ── PALADIN (holy) ─────────────────────────────────────────────────────────
    case AbilityType::Smite:
    {
        damageDirected(210.f, 200.f, dmgVal(4.f + atk * 0.9f), 1.f, 0.f, 0.f, 0);
        applyDirected(210.f, 200.f, [](Enemy& enemy) { enemy.ApplyMark(5.f); });
        pushVfx(WarriorVfxKind::Wave, 0.28f, 210.f, Color{ 255, 235, 150, 255 });
        break;
    }
    case AbilityType::Consecrate:
    {
        WarriorVfx z; z.kind = WarriorVfxKind::PoisonZone; z.pos = castTarget; z.dir = facing;
        z.lifetime = durationOr(4.f); z.radius = tunedRadius(170.f);
        z.tint = Color{ 255, 225, 130, 255 };
        z.tickDamage = dmgVal(1.f + atk * 0.2f); z.tickInterval = tickOr(0.45f);
        // The holy ring is the zone itself: loop it for the complete damage
        // duration instead of drawing a separate prototype circle beneath it.
        const int fxIdx = (int)AbilityType::Consecrate;
        if (_abilityFx[fxIdx].id != 0 && _abilityFxFrames[fxIdx] > 0)
        {
            z.fxStrip = &_abilityFx[fxIdx];
            z.fxFrames = _abilityFxFrames[fxIdx];
            z.fxScale = 5.3f;
            z.fxLoop = true;
            z.fxFrameTime = 1.f / 20.f;
        }
        _warriorVfx.push_back(z);
        break;
    }
    case AbilityType::ShieldOfFaith:   // "Aegis" — reflect + smite buff (no heal)
    {
        // Identity: the vow is also a FAITH battery — +30 Faith and one armour
        // slot alongside the reflect, mirroring how War Cry feeds Rage.
        _player.GrantDamageBuff(1.5f, 6.f);
        _player.GrantReflect(0.5f, 6.f);
        _player.AddFaith(30.f);
        _player.AddArmour(1);
        _vfx.SpawnFloatingLabel(playerPos, "FAITH!", Color{ 255, 235, 150, 255 }, 1.4f);
        pushVfx(WarriorVfxKind::Cry, 0.6f, 160.f, Color{ 255, 235, 150, 255 });
        break;
    }
    case AbilityType::HolyBolt:
    {
        damageDirected(560.f, 80.f, dmgVal(5.f + atk * 1.0f), 1.f, 0.f, 0.f, 0);
        applyDirected(560.f, 80.f, [](Enemy& enemy) { enemy.ApplyMark(5.f); });
        break;   // comet FX added by SpawnAbilityFx
    }
    case AbilityType::HammerThrow:
    {
        damageDirected(480.f, 110.f, dmgVal(5.f + atk * 1.0f), 1.f, 1.2f, 0.f, 0);
        pushShot(0.42f, 480.f, AbilityType::HammerThrow, true);
        break;
    }
    case AbilityType::LayOnHands:   // repurposed "Vengeful Ward" — strong reflect + full retribution (no heal)
    {
        _player.GrantReflect(0.6f, 6.f);
        _player.AddRetribution(5);
        pushVfx(WarriorVfxKind::Cry, 0.6f, 150.f, Color{ 255, 245, 190, 255 });
        break;
    }
    case AbilityType::DivineStorm:
    {
        // Identity payoff: the nova CONSUMES all Faith — up to +45% radius at a
        // full bar (damage already scaled with Faith via `buff` above, so the
        // radius is the spend reward and avoids double-dipping — same pattern
        // as the Warrior's Rage-fuelled Ground Slam).
        float faithFraction = _player.ConsumeAllFaith();
        float novaRadius    = tunedRadius(320.f) * (1.f + 0.45f * faithFraction);
        int hits = DamageEnemiesInRect(radialRect(novaRadius), dmgVal(7.f + atk * 1.4f), 1.f, 0.8f, 0.f, 0);   // holy stun
        _player.AddRetribution(hits);   // wrath builds per foe struck (no heal)
        ApplyMarkInRect(_enemies, radialRect(novaRadius), 5.f);
        pushVfx(WarriorVfxKind::Slam, 0.5f, novaRadius, Color{ 255, 235, 150, 255 });
        StopSound(_explosionSound); PlaySound(_explosionSound);
        TriggerScreenShake(12.f, 0.35f);
        break;
    }
    case AbilityType::AvengingWrath:   // pure wrath — big damage buff + reflect, no heal/lifesteal
    {
        _player.GrantDamageBuff(1.9f, 7.f);
        _player.GrantReflect(0.5f, 7.f);
        pushVfx(WarriorVfxKind::Cry, 0.7f, 180.f, Color{ 255, 240, 170, 255 });
        TriggerScreenShake(8.f, 0.25f);
        break;
    }
    case AbilityType::HammerOfJustice:
    {
        damageDirected(660.f, 150.f, dmgVal(10.f + atk * 1.9f), 1.f, 1.3f, 0.f, 0);
        applyDirected(660.f, 150.f, [](Enemy& enemy) { enemy.ApplyMark(6.f); });
        pushVfx(WarriorVfxKind::Spikes, 0.55f, 660.f, Color{ 255, 235, 150, 255 });
        StopSound(_explosionSound); PlaySound(_explosionSound);
        TriggerScreenShake(13.f, 0.4f);
        break;
    }

    // ── WARLOCK (dark) ──────────────────────────────────────────────────────────
    case AbilityType::ShadowBolt:
    {
        damageDirected(560.f, 80.f, dmgVal(5.f + atk * 1.0f), 1.f, 0.f, 0.f, 0);
        applyDirected(560.f, 80.f, [=](Enemy& enemy) {
            enemy.ApplyVulnerability(1.25f, 4.f);
            enemy.ApplyCurse(5.f * curseDur);
        });
        break;   // comet FX added by SpawnAbilityFx
    }
    case AbilityType::DrainLife:
    {
        _player.GrantLifesteal(0.6f, 0.3f);   // this strike heals you
        damageDirected(220.f, 130.f, dmgVal(4.f + atk * 0.9f), 1.f, 0.f, 0.f, 0);
        applyDirected(220.f, 130.f, [](Enemy& enemy) { enemy.ApplyVulnerability(1.2f, 3.f); });
        pushVfx(WarriorVfxKind::Wave, 0.3f, 220.f, Color{ 170, 90, 210, 255 });
        break;
    }
    case AbilityType::Curse:
    {
        damageDirected(260.f, 200.f, dmgVal(2.f + atk * 0.4f), 1.f, 0.f, 0.f, 0);
        int poison = poisonVal(1.f);
        applyDirected(260.f, 200.f, [=](Enemy& enemy) {
            enemy.ApplyVulnerability(1.4f, 5.f);
            enemy.ApplyPoison(poison, 5.f);
            enemy.ApplyCurse(7.f * curseDur);
        });
        pushVfx(WarriorVfxKind::Wave, 0.35f, 260.f, Color{ 150, 70, 190, 255 });
        break;
    }
    case AbilityType::CorruptionPool:
    {
        Vector2 spot = castTarget;
        WarriorVfx z; z.kind = WarriorVfxKind::PoisonZone; z.pos = spot; z.dir = facing;
        z.lifetime = durationOr(4.f); z.radius = tunedRadius(150.f);
        z.tint = Color{ 150, 70, 190, 255 };
        z.tickDamage = dmgVal(1.f + atk * 0.25f); z.tickInterval = tickOr(0.4f);
        _warriorVfx.push_back(z);
        Rectangle area = targetArea(150.f);
        ApplyPoisonInRect(_enemies, area, poisonVal(1.f), 5.f);
        ApplyCurseInRect(_enemies, area, 5.f * curseDur);
        break;
    }
    case AbilityType::Hellfire:
    {
        float hellRadius = tunedRadius(260.f);
        DamageEnemiesInRect(radialRect(hellRadius), dmgVal(5.f + atk * 1.1f), 1.f, 0.f, 0.f, 0);
        ApplyBurnInRect(_enemies, radialRect(hellRadius), dmgVal(1.f), 3, playerPos);   // hellish flames linger
        pushVfx(WarriorVfxKind::Slam, 0.45f, hellRadius, Color{ 200, 60, 160, 255 });
        StopSound(_explosionSound); PlaySound(_explosionSound);
        TriggerScreenShake(8.f, 0.2f);
        break;
    }
    case AbilityType::SoulSiphon:
    {
        float siphonRadius = tunedRadius(230.f);
        _player.GrantLifesteal(0.4f, 0.3f);
        DamageEnemiesInRect(radialRect(siphonRadius), dmgVal(4.f + atk * 0.9f), 1.f, 0.f, 0.f, 0);
        ApplySlowInRect(_enemies, radialRect(siphonRadius), 0.6f, 3.f);   // siphoning dread slows them
        _player.GrantLifesteal(0.25f, 5.f);   // lingering siphon buff afterward
        pushVfx(WarriorVfxKind::Whirl, 0.5f, siphonRadius, Color{ 170, 90, 210, 255 });
        break;
    }
    case AbilityType::Cataclysm:
    {
        float cataclysmRadius = tunedRadius(1200.f);
        DamageEnemiesInRect(radialRect(cataclysmRadius), dmgVal(9.f + atk * 1.8f), 1.f, 0.f, 0.f, 0);
        ApplyVulnInRect(_enemies, radialRect(cataclysmRadius), 1.35f, 6.f);                 // screen-wide curse
        ApplyCurseInRect(_enemies, radialRect(cataclysmRadius), 6.f * curseDur);                       // everything on screen is primed
        ApplyBurnInRect(_enemies, radialRect(cataclysmRadius), dmgVal(1.f), 3, playerPos);  // + dark fire
        pushVfx(WarriorVfxKind::Marks, 0.6f, cataclysmRadius, Color{ 180, 70, 210, 255 });
        StopSound(_explosionSound); PlaySound(_explosionSound);
        TriggerScreenShake(13.f, 0.4f);
        break;
    }
    case AbilityType::DemonForm:
    {
        _player.GrantDamageBuff(2.0f, 8.f);
        _player.GrantLifesteal(0.35f, 8.f);
        pushVfx(WarriorVfxKind::Cry, 0.7f, 190.f, Color{ 200, 60, 160, 255 });
        TriggerScreenShake(8.f, 0.25f);
        break;
    }
    case AbilityType::ShadowNova:
    {
        damageDirected(680.f, 200.f, dmgVal(9.f + atk * 1.7f), 1.f, 0.f, 0.f, 0);
        int poison = poisonVal(1.f);
        applyDirected(680.f, 200.f, [=](Enemy& enemy) {
            enemy.ApplyVulnerability(1.4f, 6.f);
            enemy.ApplyCurse(6.f * curseDur);
            enemy.ApplyPoison(poison, 5.f);
        });
        pushVfx(WarriorVfxKind::Barrage, 0.6f, 680.f, Color{ 170, 90, 210, 255 });
        StopSound(_explosionSound); PlaySound(_explosionSound);
        TriggerScreenShake(12.f, 0.35f);
        break;
    }

    default:
        break;
    }

    // Overlay the ability's dedicated animated FX (uses the final player position,
    // which lunges above have already updated).
    SpawnAbilityFx(ability, playerPos, facing, facingSign);
}

void Engine::SpawnBossFx(Vector2 worldPos, int fxId)
{
    // Boss impact moments get a weighty impact sound (plays even if the sprite
    // for this fxId is missing). SpawnBossFx is the shared call site for every
    // boss slam/strike/eruption, so this covers them all in one place.
    SfxBank::Get().Play(SfxId::BossImpact, 0.7f);
    if (fxId < 0 || fxId >= (int)_bossFx.size()) return;
    if (_bossFx[fxId].id == 0 || _bossFxFrames[fxId] <= 0) return;
    // Bosses are big — scale the impact sprite up so it reads at boss scale.
    _vfx.SpawnSpriteFx(&_bossFx[fxId], worldPos, _bossFxFrames[fxId], 7.f, 1.f / 24.f);
}

Enemy* Engine::FindEncounterDebugTarget()
{
    if (_eliteMinibossPtr != nullptr && _eliteMinibossPtr->IsActive() &&
        _eliteMinibossPtr->IsAlive())
        return _eliteMinibossPtr;

    for (const auto& enemy : _enemies)
        if (enemy && enemy->IsActive() && enemy->IsAlive() && enemy->IsBoss())
            return enemy.get();
    return nullptr;
}

const Enemy* Engine::FindEncounterDebugTarget() const
{
    if (_eliteMinibossPtr != nullptr && _eliteMinibossPtr->IsActive() &&
        _eliteMinibossPtr->IsAlive())
        return _eliteMinibossPtr;

    for (const auto& enemy : _enemies)
        if (enemy && enemy->IsActive() && enemy->IsAlive() && enemy->IsBoss())
            return enemy.get();
    return nullptr;
}

// Debug-only encounter readout: type, phase, signature state, cast/hit
// counters, live zone count and drop telemetry. Never shown in normal play.
void Engine::DrawEliteSignatureTelemetry() const
{
    const Enemy* encounter = FindEncounterDebugTarget();
    if (encounter == nullptr)
        return;

    static const char* kArchetypeNames[] = { "Ogre", "Infernal", "Bonechill", "Stormclub", "Venomfang", "None" };
    const bool isBoss = encounter->IsBoss();
    const int archetypeIndex = std::clamp((int)encounter->GetEliteArchetype(), 0, 5);
    const char* typeName = isBoss ? encounter->GetTuningName() : kArchetypeNames[archetypeIndex];
    if (typeName == nullptr) typeName = isBoss ? "Boss" : "None";
    const char* modifierName = isBoss ? "None" : EliteModifierName(_eliteMechanic);
    const EliteSignatureTelemetry telemetry = encounter->GetEliteSignatureTelemetry();

    const int panelX = 20;
    int lineY = (int)(kVirtualHeight * 0.5f) + 40;
    auto line = [&](const char* text, Color color)
    {
        DrawText(text, panelX + 1, lineY + 1, 18, Fade(BLACK, 0.7f));
        DrawText(text, panelX, lineY, 18, color);
        lineY += 22;
    };
    line(isBoss ? "BOSS TELEMETRY" : "ELITE TELEMETRY", Color{ 255, 210, 150, 255 });
    line(TextFormat("Type: %s  |  Modifier: %s", typeName, modifierName), RAYWHITE);
    line(TextFormat("State: %s  |  Phase: %d", encounter->GetEliteSignatureStateName(),
                    telemetry.phase + 1), RAYWHITE);
    line(TextFormat("Casts: %d  |  Hits: %d", telemetry.casts, telemetry.hits), RAYWHITE);
    line(TextFormat("Zones: %d/%d  |  Dropped z:%d e:%d",
                    _combatDirector.GetActiveEliteZoneCount(),
                    Balance::Elite::kSignatureZoneCapacity,
                    _combatDirector.GetDroppedEliteZoneCount(),
                    telemetry.droppedEvents),
         (_combatDirector.GetDroppedEliteZoneCount() + telemetry.droppedEvents > 0)
             ? Color{ 255, 120, 100, 255 } : RAYWHITE);
    if (!isBoss)
        line(TextFormat("Guard linked: %s", encounter->IsEliteGuardLinked() ? "YES (60%% DR)" : "no"), RAYWHITE);
}

// Elite signature art: one-shot animated impact with caller-chosen scale/tint.
// Reuses the owned FX_Boss* strips so active elite attacks are real animation.
void Engine::SpawnEliteFx(Vector2 worldPos, int fxId, float scale, Color tint)
{
    if (fxId < 0 || fxId >= (int)_bossFx.size()) return;
    if (_bossFx[fxId].id == 0 || _bossFxFrames[fxId] <= 0) return;
    _vfx.SpawnSpriteFx(&_bossFx[fxId], worldPos, _bossFxFrames[fxId], scale, 1.f / 24.f, tint);
}

// Elite lingering hazard art: a looping animated decal for the patch lifetime
// (flame patches, poison trail). The gameplay zone owns collision/damage.
void Engine::SpawnEliteHazardFx(Vector2 worldPos, int fxId, float scale, float duration, Color tint)
{
    if (fxId < 0 || fxId >= (int)_bossFx.size()) return;
    if (_bossFx[fxId].id == 0 || _bossFxFrames[fxId] <= 0) return;
    _vfx.SpawnHazardDecal(&_bossFx[fxId], worldPos, _bossFxFrames[fxId], scale, duration, tint);
}

void Engine::SpawnAbilityFx(AbilityType a, Vector2 playerPos, Vector2 facing, float facingSign)
{
    int idx = (int)a;
    if (idx < 0 || idx >= (int)AbilityType::Count) return;
    if (_abilityFx[idx].id == 0 || _abilityFxFrames[idx] <= 0) return;

    const AttackTuning* fxTuning = AttackTuningStore::Get(AttackTuningKeyForAbility(a));
    auto authoredFxPos = [&](Vector2 anchor) -> Vector2
    {
        if (fxTuning && fxTuning->hasFxOffset)
            return Vector2Add(Vector2Add(anchor, Vector2Scale(facing, fxTuning->fxForward)),
                              Vector2{ 0.f, fxTuning->fxHeight });
        return anchor;
    };

    // Multi-projectile abilities fan out several small flying FX so the visual
    // matches the name (a cone of knives, a volley of arrows, a storm of blades).
    switch (a)
    {
    case AbilityType::FanOfKnives: case AbilityType::Multishot:
    {
        const int   n     = 5;
        const float dist  = 460.f;
        const float spread = 190.f;   // total vertical fan (px over the flight)
        for (int i = 0; i < n; i++)
        {
            float frac = (n > 1) ? ((float)i / (n - 1) - 0.5f) : 0.f;   // -0.5..0.5
            WarriorVfx v;
            v.pos = authoredFxPos(playerPos); v.dir = facing; v.timer = 0.f; v.lifetime = 0.45f;
            v.radius = dist; v.fxStrip = &_abilityFx[idx]; v.fxFrames = _abilityFxFrames[idx];
            v.fxScale = 2.4f; v.vy = frac * spread; v.spin = true;
            _warriorVfx.push_back(v);
        }
        return;
    }
    default: break;
    }

    // Placement: 0 = none (weapon-icon abilities keep their flying sprite),
    //            1 = self (centred on player), 2 = forward (in front),
    //            3 = zone (at the thrown/impact point), 4 = travel (flies forward).
    int   mode  = 2;      // default: forward
    float scale = 4.5f;
    float dist  = 0.f;
    float life  = 0.45f;

    switch (a)
    {
    // Weapon throws keep their spinning icon — no FX overlay.
    case AbilityType::ThrowingAxe:
    case AbilityType::HammerThrow:
    case AbilityType::ExplosiveArrow:   // Hunter traps draw their own armed marker
    case AbilityType::FrostTrap:
    case AbilityType::Consecrate:       // persistent zone owns and loops this FX
        return;

    // Flying bolts — travel their comet/arrow FX forward.
    case AbilityType::PiercingShot: case AbilityType::HolyBolt: case AbilityType::ShadowBolt:
        mode = 4; scale = 2.8f; dist = 560.f; life = 0.42f; break;

    // Big radial ultimates / self buffs — large burst centred on the player.
    case AbilityType::GroundSlam:   case AbilityType::Rampage:     case AbilityType::Cataclysm:
    case AbilityType::DivineStorm:  case AbilityType::Hellfire:
    case AbilityType::DeathMark:    case AbilityType::DemonForm:   case AbilityType::AvengingWrath:
    case AbilityType::BladeDance:   case AbilityType::SoulSiphon:  case AbilityType::Whirlwind:
    case AbilityType::Deadeye:      case AbilityType::WarCry:      case AbilityType::ShieldOfFaith:
    case AbilityType::LayOnHands:   case AbilityType::SmokeBomb:
        mode = 1; scale = 8.f; life = 0.6f; break;

    // Lingering ground zones — FX sits at the thrown/impact point.
    // (FrostTrap / ExplosiveArrow are Hunter traps now — handled by the early
    //  return above; they draw their own armed marker, no forward FX overlay.)
    case AbilityType::PoisonVial:   case AbilityType::CorruptionPool:
    case AbilityType::RainOfBlades: case AbilityType::ArrowStorm:
        mode = 3; scale = 5.5f; life = 0.7f; break;

    default:  // forward melee / cone abilities
        mode = 2; scale = 4.5f; life = 0.45f; break;
    }

    Vector2 pos = playerPos;
    if (mode == 2) pos = Vector2Add(playerPos, Vector2Scale(facing, 130.f));
    else if (mode == 3 && _committedAimAbility == a) pos = _committedAimTarget;
    else if (mode == 3) pos = Vector2Add(playerPos, Vector2Scale(facing, 200.f));
    if (fxTuning && fxTuning->hasFxOffset && mode != 3)
        pos = authoredFxPos(playerPos);

    WarriorVfx v;
    v.kind = WarriorVfxKind::Wave;   // unused when fxStrip is set
    v.pos = pos; v.dir = facing; v.timer = 0.f; v.lifetime = life;
    v.radius = (mode == 4) ? dist : 0.f;
    v.fxStrip = &_abilityFx[idx];
    v.fxFrames = _abilityFxFrames[idx];
    v.fxScale = scale;
    v.spin = (mode == 2 || mode == 4);
    _warriorVfx.push_back(v);
}

void Engine::UpdateWarriorEffects(float dt)
{
    for (auto& v : _warriorVfx)
    {
        v.timer += dt;

        // Hunter traps: sit dormant, arm after armDelay, then snap the instant an
        // enemy enters the trigger radius — one burst (+ optional freeze), then a
        // brief detonation visual before removal. Untriggered traps expire at their
        // long lifetime.
        if (v.isTrap)
        {
            if (!v.triggered && v.timer >= v.armDelay)
            {
                for (auto& enemy : _enemies)
                {
                    if (!enemy->IsActive() || !enemy->IsAlive())
                        continue;
                    if (Vector2Distance(enemy->GetWorldPos(), v.pos) > v.triggerRadius)
                        continue;

                    // Snap: burst damage + optional hard freeze to everything in radius.
                    Rectangle zone{ v.pos.x - v.radius, v.pos.y - v.radius,
                                    v.radius * 2.f, v.radius * 2.f };
                    DamageEnemiesInRect(zone, v.trapDamage, v.trapKnockback, v.trapFreeze, 0.f, 0);
                    v.triggered = true;
                    v.timer = 0.f;
                    v.lifetime = 0.45f;   // brief detonation flash, then removed
                    TriggerScreenShake(v.trapFreeze > 0.f ? 5.f : 9.f, 0.2f);
                    StopSound(_explosionSound); PlaySound(_explosionSound);
                    break;
                }
            }
            continue;   // traps never use the lingering tick-zone path below
        }

        // Lingering damage zones (Rogue poison pool) tick enemies inside them.
        if (v.tickDamage > 0)
        {
            v.tickAccum += dt;
            if (v.tickAccum >= v.tickInterval)
            {
                v.tickAccum -= v.tickInterval;
                Rectangle zone{ v.pos.x - v.radius, v.pos.y - v.radius, v.radius * 2.f, v.radius * 2.f };
                DamageEnemiesInRect(zone, v.tickDamage, 0.f, 0.f, 0.f, 0);
            }
        }
    }
    _warriorVfx.erase(
        std::remove_if(_warriorVfx.begin(), _warriorVfx.end(),
                       [](const WarriorVfx& v) { return v.timer >= v.lifetime; }),
        _warriorVfx.end());
}

// Collect the currently-ticking player damage zones (Consecrate, Poison Vial,
// Corruption Pool...) into the shared list enemies steer around. Traps are
// excluded on purpose: their whole design is being stepped on.
void Engine::RebuildEnemyHazardZones()
{
    _enemyHazardZones.clear();
    for (const WarriorVfx& v : _warriorVfx)
        if (v.tickDamage > 0 && !v.isTrap)
            _enemyHazardZones.push_back({ v.pos, v.radius });

    // Pits read as hazard zones so enemies steer around them on their own — but
    // they can still be knocked in (knockback ignores this avoidance steering).
    if (_dungeonRoomLayout.handcrafted)
    {
        const float cellW = (float)kVirtualWidth  / (float)RoomLayout::kCols;
        const float cellH = (float)kVirtualHeight / (float)RoomLayout::kRows;
        const float radius = std::min(cellW, cellH) * 0.62f;
        for (int r = 0; r < RoomLayout::kRows; ++r)
            for (int c = 0; c < RoomLayout::kCols; ++c)
                if (_dungeonRoomLayout.fall[r][c])
                    _enemyHazardZones.push_back({ { (c + 0.5f) * cellW, (r + 0.5f) * cellH }, radius });

        // Fall Rects may be smaller or wider than one tile. Sample each one
        // into cell-sized circles so steering follows the editor-authored shape.
        for (const Rectangle& rect : _dungeonRoomLayout.fallRects)
        {
            const int cols = std::max(1, (int)std::ceil(rect.width));
            const int rows = std::max(1, (int)std::ceil(rect.height));
            const float stepW = rect.width / (float)cols;
            const float stepH = rect.height / (float)rows;
            const float sampleRadius = 0.62f * std::max(stepW * cellW, stepH * cellH);
            for (int row = 0; row < rows; ++row)
                for (int col = 0; col < cols; ++col)
                {
                    const Vector2 centre{
                        (rect.x + (col + 0.5f) * stepW) * cellW,
                        (rect.y + (row + 0.5f) * stepH) * cellH
                    };
                    _enemyHazardZones.push_back({ centre, sampleRadius });
                }
        }
    }

}

Vector2 Engine::PitPullDirection(Vector2 feet, float cellW, float cellH) const
{
    return RoomFallPullDirection(_dungeonRoomLayout, feet, cellW, cellH);
}

void Engine::SpawnFallSurfaceFx(Vector2 worldPos)
{
    switch (_dungeonRoomLayout.fallSurface)
    {
    case FallSurface::Water:
        // The blue animated impact reads as a compact splash without adding
        // prototype circles back into environmental feedback.
        _vfx.SpawnHitEffect(Character::CastType::IceSpread, worldPos,
                            { 0.f, -1.f }, 1.25f);
        break;
    case FallSurface::Lava:
        // Flame_Explosion is a 64px strip; its first eight frames contain the
        // complete burst used elsewhere by the game.
        _vfx.SpawnSpriteFx(&_roomClearExplosionTex, worldPos, 8, 2.85f,
                           1.f / 28.f, Color{ 255, 185, 95, 255 });
        break;
    case FallSurface::Void:
        break;
    }
}

void Engine::UpdateEnemyPitfalls(float cellW, float cellH)
{
    if (!_dungeonRoomLayout.handcrafted) return;
    for (auto& enemy : _enemies)
    {
        if (!enemy || !enemy->IsActive()) continue;
        if (enemy->IsBoss() || enemy->IsEliteMiniboss()) continue;   // immune to pits

        if (enemy->IsPitFalling())
        {
            if (enemy->PitFallComplete())
                enemy->FinishPitFall();
            continue;
        }

        if (enemy->IsDying()) continue;
        const Rectangle b = enemy->GetCollisionRec();
        const Vector2 feet{ b.x + b.width * 0.5f, b.y + b.height * 0.85f };
        if (!IsRoomFallPoint(_dungeonRoomLayout, feet, cellW, cellH)) continue;

        const Vector2 pull = PitPullDirection(feet, cellW, cellH);
        const float sinkDistance = std::min(cellW, cellH) * 0.55f;
        const Vector2 target = Vector2Length(pull) > 0.01f
            ? Vector2Add(enemy->GetWorldPos(), Vector2Scale(pull, sinkDistance))
            : enemy->GetWorldPos();
        SpawnFallSurfaceFx(feet);
        enemy->BeginPitFall(target, FallSurfaceTint(_dungeonRoomLayout.fallSurface));
    }
}

void Engine::DrawWarriorEffects(Vector2 camRef)
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    for (const auto& v : _warriorVfx)
    {
        float t     = v.timer / v.lifetime;          // 0..1 progress
        float fade  = 1.f - t;
        Vector2 c   = Vector2Subtract(v.pos, camRef);
        c.x += sw * 0.5f;
        c.y += sh * 0.5f;
        Vector2 moveDir = Vector2Length(v.dir) > 0.001f ? Vector2Normalize(v.dir) : Vector2{ 1.f, 0.f };
        Vector2 moveSide{ -moveDir.y, moveDir.x };

        // Animated FX strip (slashes, novas, comets...) — plays over the lifetime.
        if (v.fxStrip && v.fxStrip->id != 0 && v.fxFrames > 0)
        {
            // Looping strips are persistent zones: their radius is gameplay
            // coverage, not travel distance, so they remain on their world anchor.
            float travel = v.fxLoop ? 0.f : v.radius * t;
            Vector2 pc = Vector2Add(c, Vector2Add(Vector2Scale(moveDir, travel),
                                                  Vector2Scale(moveSide, v.vy * t)));
            int frame = 0;
            if (v.fxLoop)
            {
                const float frameTime = std::max(0.01f, v.fxFrameTime);
                frame = ((int)(v.timer / frameTime)) % v.fxFrames;
            }
            else
            {
                frame = (int)(t * v.fxFrames);
                if (frame >= v.fxFrames) frame = v.fxFrames - 1;
            }
            float cell = 64.f;
            Rectangle src{ frame * cell, 0.f, cell, cell };
            Rectangle dst{ pc.x, pc.y, cell * v.fxScale, cell * v.fxScale };
            float rotation = v.spin ? atan2f(moveDir.y, moveDir.x) * RAD2DEG : 0.f;
            DrawTexturePro(*v.fxStrip, src, dst, Vector2{ dst.width * 0.5f, dst.height * 0.5f }, rotation,
                           Fade(v.tint, 0.85f + 0.15f * fade));
            continue;
        }

        // Flying icon-sprite projectile (thrown axe, bolt, arrow...).
        if (v.sprite && v.sprite->id != 0)
        {
            float travel = v.radius * t;
            Vector2 pc = Vector2Add(c, Vector2Add(Vector2Scale(moveDir, travel),
                                                  Vector2Scale(moveSide, v.vy * t)));
            float scale = 3.6f;
            float rot = v.spin ? (v.timer * 900.f) : atan2f(moveDir.y, moveDir.x) * RAD2DEG;
            Rectangle src{ 0.f, 0.f, (float)v.sprite->width, (float)v.sprite->height };
            Rectangle dst{ pc.x, pc.y, v.sprite->width * scale, v.sprite->height * scale };
            DrawTexturePro(*v.sprite, src, dst, Vector2{ dst.width * 0.5f, dst.height * 0.5f }, rot, Fade(WHITE, 0.5f + 0.5f * fade));
            continue;
        }

        switch (v.kind)
        {
        case WarriorVfxKind::Whirl:
        {
            // Expanding double ring sweeping around the player.
            float r = v.radius * (0.5f + 0.5f * t);
            DrawCircleLines((int)c.x, (int)c.y, r, Fade(v.tint, fade));
            DrawCircleLines((int)c.x, (int)c.y, r * 0.7f, Fade(v.tint, fade * 0.7f));
            float a = t * 12.f;
            for (int i = 0; i < 3; i++)
            {
                float ang = a + i * (2.0944f);   // 120° apart
                DrawLineEx(c, { c.x + cosf(ang) * r, c.y + sinf(ang) * r }, 3.f, Fade(v.tint, fade));
            }
            break;
        }
        case WarriorVfxKind::Slam:
        {
            float r = v.radius * t;
            DrawCircleLines((int)c.x, (int)c.y, r, Fade(v.tint, fade));
            DrawCircleLines((int)c.x, (int)c.y, r * 0.85f, Fade(v.tint, fade * 0.6f));
            DrawCircle((int)c.x, (int)c.y, 26.f * fade, Fade(v.tint, fade * 0.5f));
            break;
        }
        case WarriorVfxKind::Trap:
        {
            if (v.triggered)
            {
                // Detonation: expanding shockwave over the brief post-snap lifetime.
                float r = v.radius * t;
                DrawCircleLines((int)c.x, (int)c.y, r,        Fade(v.tint, fade));
                DrawCircleLines((int)c.x, (int)c.y, r * 0.7f, Fade(v.tint, fade * 0.6f));
                DrawCircle((int)c.x, (int)c.y, 26.f * fade,   Fade(v.tint, fade * 0.5f));
            }
            else
            {
                // Dormant/armed marker: a small trap on the ground. Faint while
                // arming, then a steady pulsing ring with prongs once it's live.
                bool  live  = v.timer >= v.armDelay;
                float pulse = 0.5f + 0.5f * sinf(v.timer * 7.f);
                Color mc    = live ? v.tint : Fade(v.tint, 0.45f);
                float rr    = v.triggerRadius;
                DrawCircleLines((int)c.x, (int)c.y, rr,        Fade(mc, live ? 0.4f + 0.5f * pulse : 0.4f));
                DrawCircleLines((int)c.x, (int)c.y, rr * 0.45f, Fade(mc, 0.45f));
                for (int i = 0; i < 4; i++)   // four little jaws around the centre
                {
                    float a = i * 1.5708f + v.timer * 0.6f;
                    DrawLineEx({ c.x + cosf(a) * rr * 0.45f, c.y + sinf(a) * rr * 0.45f },
                               { c.x + cosf(a) * rr * 0.8f,  c.y + sinf(a) * rr * 0.8f },
                               2.f, Fade(mc, 0.75f));
                }
            }
            break;
        }
        case WarriorVfxKind::Wave:
        {
            // Crescent arc travelling forward.
            float travel = v.radius * t;
            Vector2 wc = Vector2Add(c, Vector2Scale(moveDir, travel));
            DrawCircleLines((int)wc.x, (int)wc.y, 60.f, Fade(v.tint, fade));
            DrawLineEx(Vector2Subtract(wc, Vector2Scale(moveSide, 55.f)),
                       Vector2Add(wc, Vector2Scale(moveSide, 55.f)), 5.f, Fade(v.tint, fade));
            break;
        }
        case WarriorVfxKind::Axe:
        {
            float travel = v.radius * t;
            Vector2 ac = Vector2Add(c, Vector2Scale(moveDir, travel));
            float spin = t * 40.f;
            for (int i = 0; i < 2; i++)
            {
                float ang = spin + i * 3.14159f;
                Vector2 tip{ ac.x + cosf(ang) * 22.f, ac.y + sinf(ang) * 22.f };
                DrawLineEx(ac, tip, 6.f, Fade(v.tint, 0.9f));
            }
            DrawCircle((int)ac.x, (int)ac.y, 6.f, Fade(v.tint, 0.9f));
            break;
        }
        case WarriorVfxKind::Bash:
        {
            float travel = v.radius * t;
            Vector2 bc = Vector2Add(c, Vector2Scale(moveDir, travel));
            DrawCircle((int)bc.x, (int)bc.y, 30.f * (1.f - t * 0.5f), Fade(v.tint, fade));
            break;
        }
        case WarriorVfxKind::Cry:
        {
            float r = v.radius * (0.3f + 0.7f * t);
            DrawCircleLines((int)c.x, (int)c.y, r, Fade(v.tint, fade));
            break;
        }
        case WarriorVfxKind::Spikes:
        {
            // A row of erupting triangles marching forward from the player.
            int count = 9;
            for (int i = 0; i < count; i++)
            {
                float frac = (float)i / (count - 1);
                if (frac > t * 1.2f) break;   // spikes rise in sequence
                float dist = frac * v.radius;
                Vector2 base = Vector2Add(c, Vector2Scale(moveDir, dist));
                float h  = 50.f * fade;
                DrawTriangle(
                    Vector2Add(base, Vector2Scale(moveDir, h)),
                    Vector2Add(Vector2Subtract(base, Vector2Scale(moveSide, 14.f)), Vector2Scale(moveDir, -18.f)),
                    Vector2Add(Vector2Add(base, Vector2Scale(moveSide, 14.f)), Vector2Scale(moveDir, -18.f)),
                    Fade(v.tint, fade));
            }
            break;
        }
        case WarriorVfxKind::Fan:
        {
            // Spray of dagger streaks in a forward cone.
            float travel = v.radius * t;
            for (int i = -3; i <= 3; i++)
            {
                float spread = i * 0.16f;                 // fan angle
                float baseAngle = atan2f(moveDir.y, moveDir.x);
                float dx = cosf(baseAngle + spread);
                float dy = sinf(baseAngle + spread);
                Vector2 tip{ c.x + dx * travel, c.y + dy * travel };
                Vector2 tail{ c.x + dx * (travel - 26.f), c.y + dy * (travel - 26.f) };
                DrawLineEx(tail, tip, 3.f, Fade(v.tint, fade));
            }
            break;
        }
        case WarriorVfxKind::Teleport:
        {
            // Imploding/expanding smoke puff.
            float r = v.radius * (0.4f + t);
            DrawCircleLines((int)c.x, (int)c.y, r, Fade(v.tint, fade));
            DrawCircle((int)c.x, (int)c.y, 10.f * fade, Fade(v.tint, fade * 0.6f));
            break;
        }
        case WarriorVfxKind::Smoke:
        {
            // Growing translucent cloud with a few billowing puffs.
            float r = v.radius * (0.5f + 0.5f * t);
            DrawCircle((int)c.x, (int)c.y, r, Fade(v.tint, fade * 0.35f));
            for (int i = 0; i < 5; i++)
            {
                float ang = i * 1.2566f + t * 2.f;
                float pr = r * 0.6f;
                DrawCircle((int)(c.x + cosf(ang) * pr), (int)(c.y + sinf(ang) * pr),
                           22.f * fade, Fade(v.tint, fade * 0.4f));
            }
            break;
        }
        case WarriorVfxKind::PoisonZone:
        {
            // Bubbling green pool for its whole lifetime.
            DrawCircle((int)c.x, (int)c.y, v.radius, Fade(v.tint, 0.25f * fade + 0.10f));
            DrawCircleLines((int)c.x, (int)c.y, v.radius, Fade(v.tint, 0.6f));
            for (int i = 0; i < 6; i++)
            {
                float ang = i * 1.047f + v.timer * 3.f;
                float pr = v.radius * (0.3f + 0.5f * fabsf(sinf(v.timer * 2.f + i)));
                DrawCircle((int)(c.x + cosf(ang) * pr), (int)(c.y + sinf(ang) * pr),
                           6.f, Fade(v.tint, 0.7f));
            }
            break;
        }
        case WarriorVfxKind::Marks:
        {
            // Expanding red execution ring with target reticles.
            float r = v.radius * t;
            DrawCircleLines((int)c.x, (int)c.y, r, Fade(v.tint, fade));
            for (float deg = 0.f; deg < 360.f; deg += 45.f)
            {
                float rad = deg * DEG2RAD;
                Vector2 p{ c.x + cosf(rad) * r * 0.6f, c.y + sinf(rad) * r * 0.6f };
                DrawLineEx({ p.x - 10.f, p.y }, { p.x + 10.f, p.y }, 2.f, Fade(v.tint, fade));
                DrawLineEx({ p.x, p.y - 10.f }, { p.x, p.y + 10.f }, 2.f, Fade(v.tint, fade));
            }
            break;
        }
        case WarriorVfxKind::Barrage:
        {
            // Daggers rain around the chosen target area.
            float radius = std::max(80.f, v.radius);
            for (int i = 0; i < 14; i++)
            {
                float frac = (float)i / 13.f;
                float bx = c.x + (frac * 2.f - 1.f) * radius;
                float phase = fmodf(t * 2.f + frac * 1.3f, 1.f);
                float by = c.y - radius + phase * radius * 2.f;
                DrawLineEx({ bx, by - 16.f }, { bx, by + 16.f }, 3.f, Fade(v.tint, fade));
            }
            break;
        }
        case WarriorVfxKind::Flurry:
        {
            // Quick criss-cross slashes in the strike direction.
            float travel = v.radius * (0.3f + 0.7f * t);
            Vector2 fc = Vector2Add(c, Vector2Scale(moveDir, travel * 0.5f));
            for (int i = 0; i < 3; i++)
            {
                float off = (i - 1) * 18.f;
                Vector2 centre = Vector2Add(fc, Vector2Scale(moveSide, off));
                DrawLineEx(Vector2Add(Vector2Scale(moveDir, -30.f), Vector2Add(centre, Vector2Scale(moveSide, -20.f))),
                           Vector2Add(Vector2Scale(moveDir, 30.f), Vector2Add(centre, Vector2Scale(moveSide, 20.f))),
                           3.f, Fade(v.tint, fade));
            }
            break;
        }
        }
    }
}

void Engine::TriggerUltimateSequence(AbilityType element)
{
    _ultimatePhase       = UltimatePhase::WindUp;
    _ultimatePhaseTimer  = 0.f;
    _ultimateCircleAngle = 0.f;
    _ultimateElement     = element;
    _ultimateBlasts.clear();
    StopSound(_fireballCastSound);
    PlaySound(_fireballCastSound);
}

void Engine::UpdateUltimateSequence(float dt)
{
    _ultimatePhaseTimer  += dt;
    _ultimateCircleAngle += dt * 2.2f;

    switch (_ultimatePhase)
    {
    case UltimatePhase::WindUp:
        if (_ultimatePhaseTimer >= _ultWindUpDuration)
        {
            _ultimatePhase      = UltimatePhase::Cinematic;
            _ultimatePhaseTimer = 0.f;
            SpawnUltimateBurst(_ultimateElement);
        }
        break;

    case UltimatePhase::Cinematic:
        UpdateUltimateBlasts(dt);
        if (_ultimatePhaseTimer >= _ultCinematicDuration)
        {
            _ultimatePhase      = UltimatePhase::Impact;
            _ultimatePhaseTimer = 0.f;
            ApplyUltimateImpact();
            _ultimateBlasts.clear();
            TriggerScreenShake(14.f, 0.45f);
            StopSound(_explosionSound);
            PlaySound(_explosionSound);
        }
        break;

    case UltimatePhase::Impact:
        if (_ultimatePhaseTimer >= _ultImpactDuration)
        {
            _ultimatePhase      = UltimatePhase::Release;
            _ultimatePhaseTimer = 0.f;
        }
        break;

    case UltimatePhase::Release:
        if (_ultimatePhaseTimer >= _ultReleaseDuration)
        {
            _ultimatePhase      = UltimatePhase::None;
            _ultimatePhaseTimer = 0.f;
        }
        break;

    default:
        break;
    }
}

namespace
{
    struct JuiceParam { const char* name; float step; float lo; float hi; bool isBool; bool isInteger; };
    const JuiceParam kJuiceParams[] = {
        { "Crit slow dur",       0.05f, 0.f,   1.5f,  false, false },
        { "Crit slow scale",     0.05f, 0.05f, 1.f,   false, false },
        { "Boss slow dur",       0.1f,  0.f,   3.f,   false, false },
        { "Boss slow scale",     0.05f, 0.05f, 1.f,   false, false },
        { "Kill hit-stop",       0.01f, 0.f,   0.3f,  false, false },
        { "Shake mult",          0.1f,  0.f,   3.f,   false, false },
        { "Crit focus dark",     0.05f, 0.f,   1.f,   false, false },
        { "FORCE CRIT",          0.f,   0.f,   1.f,   true,  false },
        { "Damage numbers",      0.f,   0.f,   1.f,   true,  false },
        { "Minimum font",        1.f,   12.f,  48.f,  false, true  },
        { "Maximum font",        1.f,   16.f,  72.f,  false, true  },
        { "Damage reference",    1.f,   1.f,   100.f, false, false },
        { "Rise speed",          5.f,   10.f,  240.f, false, false },
        { "Horizontal drift",    2.f,   0.f,   100.f, false, false },
        { "Lifetime",            0.05f, 0.25f, 2.f,   false, false },
        { "Outline",             0.25f, 0.f,   5.f,   false, false },
        { "Merge window",        0.01f, 0.f,   0.5f,  false, false },
        { "Visible cap",         1.f,   4.f,   96.f,  false, true  },
        { "Freeze number anim",  0.f,   0.f,   1.f,   true,  false },
    };
    constexpr int kJuiceParamCount = (int)(sizeof(kJuiceParams) / sizeof(kJuiceParams[0]));
}

void Engine::UpdateJuicePanel()
{
    float* juiceValues[] = { &_juiceCritSlowDur, &_juiceCritSlowScale,
                             &_juiceBossSlowDur, &_juiceBossSlowScale, &_juiceKillHitStop,
                             &_juiceShakeMult, &_juiceCritFocus };
    DamageNumberSettings& numbers = _damageNumbers.GetSettings();

    if (IsKeyPressed(KEY_ESCAPE)) { _juicePanelOpen = false; return; }
    if (IsKeyPressed(KEY_DOWN)) _juiceSel = (_juiceSel + 1) % kJuiceParamCount;
    if (IsKeyPressed(KEY_UP))   _juiceSel = (_juiceSel + kJuiceParamCount - 1) % kJuiceParamCount;

    _juiceNudgeCd -= GetFrameTime();
    int dir = 0;
    if      (IsKeyPressed(KEY_RIGHT)) dir = 1;
    else if (IsKeyPressed(KEY_LEFT))  dir = -1;
    else if (_juiceNudgeCd <= 0.f)
    {
        if      (IsKeyDown(KEY_RIGHT)) dir = 1;
        else if (IsKeyDown(KEY_LEFT))  dir = -1;
    }

    const JuiceParam& p = kJuiceParams[_juiceSel];
    if (p.isBool)
    {
        if (dir != 0 || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE))
        {
            if (_juiceSel == 7) _juiceForceCrit = !_juiceForceCrit;
            else if (_juiceSel == 8) numbers.enabled = !numbers.enabled;
            else if (_juiceSel == 18) numbers.freezeAnimation = !numbers.freezeAnimation;
        }
    }
    else if (dir != 0)
    {
        float value = 0.f;
        if (_juiceSel < 7) value = *juiceValues[_juiceSel];
        else switch (_juiceSel)
        {
        case 9:  value = (float)numbers.minFontSize; break;
        case 10: value = (float)numbers.maxFontSize; break;
        case 11: value = numbers.damageReference; break;
        case 12: value = numbers.riseSpeed; break;
        case 13: value = numbers.horizontalDrift; break;
        case 14: value = numbers.lifetime; break;
        case 15: value = numbers.outlineOffset; break;
        case 16: value = numbers.mergeWindow; break;
        case 17: value = (float)numbers.visibleCap; break;
        }
        value = std::clamp(value + dir * p.step, p.lo, p.hi);
        if (_juiceSel < 7) *juiceValues[_juiceSel] = value;
        else switch (_juiceSel)
        {
        case 9:  numbers.minFontSize = (int)value; break;
        case 10: numbers.maxFontSize = (int)value; break;
        case 11: numbers.damageReference = value; break;
        case 12: numbers.riseSpeed = value; break;
        case 13: numbers.horizontalDrift = value; break;
        case 14: numbers.lifetime = value; break;
        case 15: numbers.outlineOffset = value; break;
        case 16: numbers.mergeWindow = value; break;
        case 17: numbers.visibleCap = (int)value; break;
        }
        numbers.maxFontSize = std::max(numbers.maxFontSize, numbers.minFontSize);
        _juiceNudgeCd = 0.07f;
    }
}

void Engine::DrawJuicePanel()
{
    const DamageNumberSettings& numbers = _damageNumbers.GetSettings();
    const DamageNumberStats& stats = _damageNumbers.GetStats();
    float vals[] = { _juiceCritSlowDur, _juiceCritSlowScale,
                     _juiceBossSlowDur, _juiceBossSlowScale, _juiceKillHitStop,
                     _juiceShakeMult, _juiceCritFocus, _juiceForceCrit ? 1.f : 0.f,
                     numbers.enabled ? 1.f : 0.f, (float)numbers.minFontSize,
                     (float)numbers.maxFontSize, numbers.damageReference, numbers.riseSpeed,
                     numbers.horizontalDrift, numbers.lifetime, numbers.outlineOffset,
                     numbers.mergeWindow, (float)numbers.visibleCap,
                     numbers.freezeAnimation ? 1.f : 0.f };
    const float sw = (float)kVirtualWidth;
    float pw = 520.f, ph = 82.f + kJuiceParamCount * 28.f, px = sw - pw - 30.f, py = 70.f;
    DrawRectangleRounded({ px, py, pw, ph }, 0.04f, 6, Fade(Color{ 18, 16, 26, 255 }, 0.94f));
    DrawRectangleRoundedLines({ px, py, pw, ph }, 0.04f, 6, Color{ 235, 210, 140, 255 });
    DrawText("JUICE TUNING", (int)(px + 18.f), (int)(py + 12.f), 24, Color{ 235, 210, 140, 255 });

    for (int i = 0; i < kJuiceParamCount; i++)
    {
        float y = py + 44.f + i * 28.f;
        bool sel = (i == _juiceSel);
        if (sel) DrawRectangle((int)(px + 6.f), (int)y - 2, (int)pw - 12, 30, Fade(GOLD, 0.16f));
        DrawText(kJuiceParams[i].name, (int)(px + 18.f), (int)y, 19, sel ? GOLD : RAYWHITE);
        const char* valTxt = kJuiceParams[i].isBool
            ? (vals[i] > 0.5f ? "ON" : "off")
            : (kJuiceParams[i].isInteger ? TextFormat("%.0f", vals[i]) : TextFormat("%.2f", vals[i]));
        DrawText(valTxt, (int)(px + pw - 110.f), (int)y, 19, sel ? GOLD : Color{ 180, 210, 255, 255 });
    }
    DrawText(TextFormat("POOL %d/%d  visible %d  merged %llu  suppressed %llu  high %d",
                        stats.active, stats.capacity, stats.visible, stats.merged,
                        stats.suppressed, stats.highWater),
             (int)(px + 18.f), (int)(py + ph - 48.f), 16, Color{ 120, 220, 190, 255 });
    DrawText("Up/Down: pick   Left/Right: adjust   J/ESC: close",
             (int)(px + 18.f), (int)(py + ph - 24.f), 16, Color{ 165, 160, 185, 255 });
}

void Engine::DrawScreenFx()
{
    // Crit focus — a soft dark spotlight centred on the hit (no blur; pixel-safe).
    if (_critFocusTimer > 0.f)
    {
        float t = _critFocusTimer / _critFocusDur;                 // 1 -> 0
        float dark = _juiceCritFocus * t;
        float radius = kVirtualHeight * 0.9f;
        DrawCircleGradient((int)_critFocusScreenPos.x, (int)_critFocusScreenPos.y,
                           radius, Fade(BLACK, 0.f), Fade(BLACK, dark));
    }
    // Screen flash (level-up / ultimate).
    if (_screenFlashTimer > 0.f && _screenFlashDur > 0.f)
    {
        float a = (_screenFlashTimer / _screenFlashDur) * 0.45f;
        DrawRectangle(0, 0, kVirtualWidth, kVirtualHeight, Fade(_screenFlashColor, a));
    }
    // Player hurt — red vignette hugging the four screen edges, clear in the
    // middle (four gradient bands fading from red at the border to transparent
    // inward; overlapping corners read a touch darker, as a vignette should).
    if (_playerHurtTimer > 0.f)
    {
        float a = (_playerHurtTimer / 0.4f) * 0.6f;
        Color edge  = Fade(Color{ 200, 20, 20, 255 }, a);
        Color clear = Fade(Color{ 200, 20, 20, 255 }, 0.f);
        const int bandX = (int)(kVirtualWidth  * 0.20f);
        const int bandY = (int)(kVirtualHeight * 0.22f);
        DrawRectangleGradientV(0, 0, kVirtualWidth, bandY, edge, clear);                      // top
        DrawRectangleGradientV(0, kVirtualHeight - bandY, kVirtualWidth, bandY, clear, edge); // bottom
        DrawRectangleGradientH(0, 0, bandX, kVirtualHeight, edge, clear);                     // left
        DrawRectangleGradientH(kVirtualWidth - bandX, 0, bandX, kVirtualHeight, clear, edge); // right
    }
    // Player death — heavy dark-red wash over the whole screen during the beat.
    if (_deathVignette > 0.f)
    {
        float a = std::min(0.6f, _deathVignette * 0.55f);
        DrawRectangle(0, 0, kVirtualWidth, kVirtualHeight, Fade(Color{ 90, 0, 0, 255 }, a));
    }

    if (_juicePanelOpen) DrawJuicePanel();
}

int Engine::RegisterHitFx(Enemy& enemy, float healthBefore, bool crit,
                          bool backstab, bool damageOverTime, std::uint32_t attackId)
{
    const int damage = CalculateActualDamage(healthBefore, enemy.GetHealthValue());
    if (damage <= 0)
        return 0;

    const bool killed = !enemy.IsAlive();
    const bool isBoss = enemy.IsBoss();
    DamageNumberEvent event{};
    event.targetId = enemy.GetCombatId();
    event.attackId = attackId;
    event.worldPos = enemy.GetWorldPos();
    event.finalDamage = damage;
    event.outcome = crit ? DamageNumberOutcome::Critical : DamageNumberOutcome::Normal;
    event.backstab = backstab;
    event.killingBlow = killed;
    event.elite = enemy.IsEliteMiniboss();
    event.boss = isBoss;
    event.damageOverTime = damageOverTime;
    _damageNumbers.Submit(event);

    // Guard Links reduced this hit: show a restrained GUARDED tag beside the
    // real (post-reduction) number so the player learns the attack WORKED but
    // is inefficient until the guards die — never a silent or fake denial.
    if (enemy.ConsumeGuardReducedFlag())
        _vfx.SpawnFloatingLabel(Vector2{ enemy.GetWorldPos().x, enemy.GetWorldPos().y - 40.f },
                                "GUARDED", Color{ 180, 100, 255, 255 }, 0.8f);

    // Hit direction — away from the player, so melee/ranged both shove the target
    // outward. Reused for the directional sparks and the recoil below.
    Vector2 hitDir = Vector2Subtract(enemy.GetWorldPos(), _player.GetWorldPos());

    const Color impactColor = crit ? Color{ 255, 205, 45, 255 }
                                   : Color{ 230, 45, 50, 255 };
    // Sparks spray in a cone along the blow (a cut), except a kill bursts radially
    // for a satisfying "pop". Wider cone on crits so big hits feel splashier.
    if (killed)
    {
        _vfx.SpawnImpactBurst(enemy.GetWorldPos(), impactColor, 12, 360.f);
        // Extra fast white gibs so the kill flashes/pops (pairs with the death-pop
        // scale in Enemy::DrawEnemy).
        _vfx.SpawnImpactBurst(enemy.GetWorldPos(), Color{ 255, 255, 255, 255 }, 8, 520.f);
    }
    else
        _vfx.SpawnImpactBurst(enemy.GetWorldPos(), impactColor,
                              crit ? 9 : 4, 240.f, hitDir, crit ? 0.9f : 0.6f);

    // Recoil: shove non-boss enemies away from the blow so hits have weight. Bosses
    // stay planted (a shovable boss feels wrong); elites take a reduced shove.
    if (!killed && !isBoss)
    {
        const float baseSpeed = crit ? 520.f : 340.f;
        const float speed = baseSpeed * (enemy.IsEliteMiniboss() ? 0.45f : 1.f);
        enemy.ApplyHitKnockback(hitDir, speed);
    }

    if (killed)
    {
        TriggerScreenShake(isBoss ? 9.f : 6.5f, isBoss ? 0.35f : 0.18f);
        // In the boss room the cinematic slow-mo is deferred to the moment the
        // LAST enemy falls (room clear) so it doesn't fire early while adds live.
        bool inBossRoom = (_gameState == GameState::DungeonRun &&
                           _dungeonRoomIdx == _dungeonGen.GetBossIndex());
        if (isBoss && !inBossRoom) TriggerSlowMo(_juiceBossSlowDur, _juiceBossSlowScale); // elite death elsewhere
        else                       RequestHitStop(_juiceKillHitStop);                     // snappy kill; boss-room slow-mo comes at clear
    }
    else if (crit)
    {
        TriggerScreenShake(3.f, 0.09f);
        TriggerSlowMo(_juiceCritSlowDur, _juiceCritSlowScale);   // crit slowdown (instead of extra push)
        RequestHitStop(_juiceHitStopMax);                        // crits get the full non-kill freeze too
        // Focus spotlight on the hit — screen pos via the same camera transform
        // the world objects use.
        _critFocusTimer     = _critFocusDur = 0.3f;
        const Vector2 enemyPos = enemy.GetWorldPos();
        _critFocusScreenPos = Vector2{
            enemyPos.x - (_cameraPos.x - _shakeOffset.x) + kVirtualWidth  * 0.5f,
            enemyPos.y - (_cameraPos.y - _shakeOffset.y) + kVirtualHeight * 0.5f };
    }
    else
    {
        // Normal landed hit — a short, damage-scaled freeze so the strike bites.
        // frac = fraction of the target's max HP this hit removed; a big chunk of a
        // small enemy freezes near the cap, chip damage sits near the floor, and a
        // huge-HP boss barely stutters (frac ~ 0).
        const float maxHp = enemy.GetMaxHealthValue();
        const float frac  = (maxHp > 0.f) ? std::min(1.f, (float)damage / maxHp) : 0.f;
        const float hitStop = _juiceHitStopMin + frac * (_juiceHitStopMax - _juiceHitStopMin);
        RequestHitStop(hitStop);
    }
    return damage;
}

void Engine::ShowBlockedHitFeedback(Enemy& enemy, Enemy::HitBlockReason reason)
{
    Color labelColor{ 170, 200, 255, 255 };
    DamageNumberOutcome outcome = DamageNumberOutcome::Blocked;
    if (reason == Enemy::HitBlockReason::Blocked)
        outcome = DamageNumberOutcome::Blocked;
    else if (reason == Enemy::HitBlockReason::Immune)
    {
        outcome = DamageNumberOutcome::Immune;
        labelColor = Color{ 190, 190, 210, 255 };
    }
    DamageNumberEvent event{};
    event.targetId = enemy.GetCombatId();
    event.worldPos = enemy.GetWorldPos();
    event.outcome = outcome;
    _damageNumbers.Submit(event);
    // A few pale sparks sell the denial without implying damage was dealt.
    _vfx.SpawnImpactBurst(enemy.GetWorldPos(), labelColor, 5, 200.f);
}

void Engine::ShowBossCallout(Vector2 enemyPos, const char* text)
{
    if (text == nullptr) return;
    // Big and warm so a phase change reads as an event; raised above the boss so
    // it clears the health bar. Rises/fades like any floating label.
    const Color kCalloutColor{ 255, 140, 40, 255 };
    _vfx.SpawnFloatingLabel(Vector2{ enemyPos.x, enemyPos.y - 70.f }, text, kCalloutColor, 1.8f);
}

void Engine::ApplyPendingReflect()
{
    float dmg; Vector2 pos;
    if (!_player.ConsumeReflect(dmg, pos) || dmg < 1.f) return;

    Enemy* best = nullptr; float bestD = 1e18f;
    for (auto& e : _enemies)
    {
        if (!e->IsActive() || !e->IsAlive()) continue;
        Vector2 ep = e->GetWorldPos();
        float d = (ep.x - pos.x) * (ep.x - pos.x) + (ep.y - pos.y) * (ep.y - pos.y);
        if (d < bestD) { bestD = d; best = e.get(); }
    }
    if (best)
    {
        int r = (int)lroundf(dmg);
        const float healthBefore = best->GetHealthValue();
        best->TakeDamage(r, _player.GetWorldPos());
        Enemy::HitBlockReason blocked = best->ConsumeHitBlock();
        if (blocked != Enemy::HitBlockReason::None)
            ShowBlockedHitFeedback(*best, blocked);
        else
            RegisterHitFx(*best, healthBefore, false, false, false, 5u);
    }
}

void Engine::ApplyUltimateImpact()
{
    // Hit every enemy currently visible on screen (with a small margin beyond edges).
    // Attack Editor override: a saved box replaces the screen-wide default so the
    // damage region can be matched to the blast FX.
    float halfW = kVirtualWidth  * 0.55f;
    float halfH = kVirtualHeight * 0.55f;
    Vector2 playerPos = _player.GetWorldPos();
    Vector2 centreOff{ 0.f, 0.f };
    const AttackTuning* ultTune = AttackTuningStore::Get(AttackTuningKeyForAbility(_ultimateElement));
    if (ultTune && ultTune->hasBox)
    {
        halfW = ultTune->w * 0.5f;
        halfH = ultTune->h * 0.5f;
        centreOff = { ultTune->x, ultTune->y };
    }

    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive() || !enemy->IsAlive())
            continue;

        Vector2 delta = Vector2Subtract(enemy->GetWorldPos(), Vector2Add(playerPos, centreOff));
        if (fabsf(delta.x) > halfW || fabsf(delta.y) > halfH)
            continue;

        int baseDmg = enemy->AsMolarbeast() ? std::min(3, _player.GetUltimateHitDamage(_ultimateElement)) : _player.GetUltimateHitDamage(_ultimateElement);
        bool ultCrit = false;
        int dmg = ScalePlayerHit(*enemy, baseDmg, ultCrit);
        const float healthBefore = enemy->GetHealthValue();
        enemy->TakeDamage(dmg, playerPos);

        Enemy::HitBlockReason blocked = enemy->ConsumeHitBlock();
        if (blocked != Enemy::HitBlockReason::None)
        {
            ShowBlockedHitFeedback(*enemy, blocked);
            continue;
        }
        RegisterHitFx(*enemy, healthBefore, ultCrit, false, false,
                      200u + static_cast<std::uint32_t>(_ultimateElement));

        if (_ultimateElement == AbilityType::FireUltimate)
            enemy->ApplyBurn(0.5f, _player.GetBoltBurnDamage(_ultimateElement), playerPos);
        else if (_ultimateElement == AbilityType::IceUltimate)
            enemy->ApplyFreeze(3.f);
        else if (_ultimateElement == AbilityType::ElectricUltimate)
            enemy->ApplyElectricCharge();
    }
}

void Engine::DrawUltimateSequence()
{
    if (_ultimatePhase == UltimatePhase::None)
        return;

    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    const float cx = sw * 0.5f;
    const float cy = sh * 0.5f;

    Color ec =
        (_ultimateElement == AbilityType::FireUltimate)     ? Color{255, 110,  20, 255} :
        (_ultimateElement == AbilityType::IceUltimate)      ? Color{ 80, 200, 255, 255} :
                                                               Color{255, 220,   0, 255};

    // -- Dark overlay (ramps up during wind-up, holds, fades on release) -------
    if (_ultimatePhase != UltimatePhase::Impact)
    {
        float alpha =
            (_ultimatePhase == UltimatePhase::WindUp)
                ? (_ultimatePhaseTimer / _ultWindUpDuration) * 0.55f :
            (_ultimatePhase == UltimatePhase::Release)
                ? 0.55f * (1.f - _ultimatePhaseTimer / _ultReleaseDuration)
                : 0.55f;
        DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, alpha));
    }

    // -- Impact flash ---------------------------------------------------------
    if (_ultimatePhase == UltimatePhase::Impact)
    {
        float t = 1.f - (_ultimatePhaseTimer / _ultImpactDuration);
        DrawRectangle(0, 0, (int)sw, (int)sh, Fade(WHITE, t * 0.85f));
        // Expanding ring in element colour
        Color ringCol = { ec.r, ec.g, ec.b, (unsigned char)(t * 210.f) };
        DrawCircleLinesV({ cx, cy }, (1.f - t) * sw * 0.9f, ringCol);
    }

    // -- Magic circle (wind-up + cinematic) -----------------------------------
    if (_ultimatePhase == UltimatePhase::WindUp || _ultimatePhase == UltimatePhase::Cinematic)
    {
        float progress = (_ultimatePhase == UltimatePhase::WindUp)
            ? _ultimatePhaseTimer / _ultWindUpDuration : 1.f;

        const float radii[3]  = { 75.f, 120.f, 165.f };
        const float speeds[3] = { 1.0f, -0.65f, 0.45f };

        for (int r = 0; r < 3; r++)
        {
            float radius = radii[r] * progress;
            float angle  = _ultimateCircleAngle * speeds[r];
            unsigned char a = (unsigned char)(190.f * progress);
            Color ringCol = { ec.r, ec.g, ec.b, a };

            DrawCircleLinesV({ cx, cy }, radius, ringCol);

            // 4 glowing markers rotating around each ring
            for (int m = 0; m < 4; m++)
            {
                float ma = angle + m * (PI * 0.5f);
                Vector2 mp = { cx + cosf(ma) * radius, cy + sinf(ma) * radius };
                DrawCircleV(mp, 5.f * progress, ringCol);
            }
        }

        // Soft glow around the player's screen position
        DrawCircleV({ cx, cy }, 48.f * progress,
            { ec.r, ec.g, ec.b, (unsigned char)(85.f * progress) });
    }
}

void Engine::DrawUltimateBlasts(Vector2 worldOffset)
{
    for (const auto& blast : _ultimateBlasts)
    {
        if (blast.timer <= 0.f)
            continue;

        // Progress 0?1 as blast ages
        float progress = 1.f - (blast.timer / blast.lifetime);

        // Pulse envelope: pop in fast, hold, fade out
        float pulse;
        if      (progress < 0.3f)  pulse = progress / 0.3f;
        else if (progress < 0.75f) pulse = 1.f;
        else                       pulse = 1.f - (progress - 0.75f) / 0.25f;

        if (pulse <= 0.f) continue;

        // Animated sprite sheet - ultimate types use their own 64x64 sheets
        const Texture2D& tex = SpreadProjectile::GetAnimTexture(blast.element);

        const float fw        = (float)SpreadProjectile::GetFrameWFor(blast.element);
        const float fh        = (float)SpreadProjectile::GetFrameHFor(blast.element);
        const int   frameCount = SpreadProjectile::GetFrameCountFor(blast.element);

        float elapsed = blast.lifetime - blast.timer;
        int   frame   = (int)(elapsed / (1.f / 16.f)) % frameCount;

        Rectangle src = GetAnimationFrameRect(tex, (int)fw, (int)fh, frame);

        const float maxSize = 200.f;
        float size = maxSize * pulse;

        Vector2 screenPos = {
            blast.worldPos.x + worldOffset.x + kVirtualWidth  * 0.5f,
            blast.worldPos.y + worldOffset.y + kVirtualHeight * 0.5f
        };

        // Fixed facing direction per blast - no spinning
        float rotation = blast.rotation;

        Rectangle dest     = { screenPos.x, screenPos.y, size, size };
        Vector2   origin   = { size * 0.5f, size * 0.5f };
        unsigned char alpha = (unsigned char)(pulse * 255.f);

        // Soft glow behind - slightly larger, low alpha
        float     glowSize   = size * 1.65f;
        Rectangle glowDest   = { screenPos.x, screenPos.y, glowSize, glowSize };
        Vector2   glowOrigin = { glowSize * 0.5f, glowSize * 0.5f };
        DrawTexturePro(tex, src, glowDest, glowOrigin, rotation,
            { 255, 255, 255, (unsigned char)(pulse * 55.f) });

        // Main animated frame
        DrawTexturePro(tex, src, dest, origin, rotation, { 255, 255, 255, alpha });
    }
}


void Engine::UpdateSpreadProjectiles(float dt)
{
    for (auto& projectile : _spreadProjectiles)
    {
        if (!projectile.IsActive())
            continue;

        projectile.Update(dt);

        if (!projectile.IsActive())
            continue;

        // Helper: map element -> CastType so hit effects use the right sprite
        auto elementToCastType = [](AbilityType el) -> Character::CastType
        {
            if (el == AbilityType::IceSpread     || el == AbilityType::IceBolt)      return Character::CastType::IceSpread;
            if (el == AbilityType::ElectricSpread|| el == AbilityType::ElectricBolt) return Character::CastType::ElectricSpread;
            return Character::CastType::FireSpread;
        };

        if (_gameState == GameState::DungeonRun)
        {
            Rectangle projRec = projectile.GetCollisionRec();
            bool blocked = false;

            float cellW = (float)kVirtualWidth / (float)RoomLayout::kCols;
            float cellH = (float)kVirtualHeight / (float)RoomLayout::kRows;

            for (int r = 0; r < RoomLayout::kRows && !blocked; r++)
            {
                for (int c = 0; c < RoomLayout::kCols; c++)
                {
                    TileType t = _dungeonRoomLayout.tiles[r][c];
                    if ((!_dungeonRoomLayout.solid[r][c] || RoomPlacementClearsAtDoor(
                            {(float)c,(float)r,1.f,1.f},_dungeonRoomLayout)) &&
                        (t == TileType::Floor || t == TileType::FloorVariant ||
                        t == TileType::DoorOpen || t == TileType::Void ||
                        t == TileType::None))
                    {
                        continue;
                    }

                    Rectangle wallRect{ c * cellW, r * cellH, cellW, cellH };
                    if (CheckCollisionRecs(projRec, wallRect))
                    {
                        blocked = true;
                        break;
                    }
                }
            }

            float pxScaleX = cellW / 16.f;
            float pxScaleY = cellH / 16.f;
            for (const SpritePlacement& prop : _dungeonRoomLayout.props)
            {
                if (blocked) break;
                const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,prop,_tileDefs);
                if (defs==nullptr || prop.defIdx<0 || prop.defIdx>=(int)defs->props.size()) continue;
                const Rectangle& coll = defs->props[prop.defIdx].collision;
                Rectangle propRect{
                    prop.col * cellW + coll.x * pxScaleX,
                    prop.row * cellH + coll.y * pxScaleY,
                    coll.width  * pxScaleX,
                    coll.height * pxScaleY
                };

                if (CheckCollisionRecs(projRec, propRect))
                    blocked = true;
            }

            if (blocked)
            {
                _vfx.SpawnHitEffect(elementToCastType(projectile.GetElement()),
                                    projectile.GetWorldPos(), projectile.GetDirection());
                projectile.Destroy();
                continue;
            }
        }
        else
        {
            // Map-boundary wall check - same margins as player collision.
            float mapW = _map.width  * _mapScale;
            float mapH = _map.height * _mapScale;
            Vector2 p  = projectile.GetWorldPos();
            if (p.x < 76.f || p.x > mapW - 96.f ||
                p.y < 42.f || p.y > mapH - 320.f)
            {
                _vfx.SpawnHitEffect(elementToCastType(projectile.GetElement()),
                                    projectile.GetWorldPos(), projectile.GetDirection());
                projectile.Destroy();
                continue;
            }

            for (auto& prop : _props)
            {
                if (CheckCollisionRecs(projectile.GetCollisionRec(), prop.GetCollisionRec()))
                {
                    _vfx.SpawnHitEffect(elementToCastType(projectile.GetElement()),
                                        projectile.GetWorldPos(), projectile.GetDirection());
                    projectile.Destroy();
                    break;
                }
            }

            if (!projectile.IsActive())
                continue;
        }

        // Player projectiles chip destructible room hazards and burst on them,
        // so ranged classes can disable totems/torches like melee classes can.
        if (_roomHazards.DamageHazardsInRect(projectile.GetCollisionRec(), 1) > 0)
        {
            _vfx.SpawnHitEffect(elementToCastType(projectile.GetElement()),
                                projectile.GetWorldPos(), projectile.GetDirection());
            projectile.Destroy();
            continue;
        }

        for (auto& enemy : _enemies)
        {
            if (!enemy->IsActive() || !enemy->IsAlive())
                continue;

            if (!CheckCollisionRecs(projectile.GetCollisionRec(), enemy->GetHitCollisionRec()))
                continue;
            if (projectile.HasHit(enemy.get()))
                continue;

            AbilityType element = projectile.GetElement();

            // Class basic-attack shot: small fixed damage, no element status, one hit.
            if (projectile.IsBasic())
            {
                bool basicCrit = false;
                int base = std::max(1, (int)std::round(_player.GetMeleeDamage() * 0.6f * _player.GetClassDamageMult()));
                int dmg = ScalePlayerHit(*enemy, base, basicCrit);
                const float healthBefore = enemy->GetHealthValue();
                enemy->TakeDamage(dmg, _player.GetWorldPos());
                Enemy::HitBlockReason blocked = enemy->ConsumeHitBlock();
                if (blocked != Enemy::HitBlockReason::None)
                {
                    ShowBlockedHitFeedback(*enemy, blocked);
                    _vfx.SpawnHitEffect(elementToCastType(element), projectile.GetWorldPos(), projectile.GetDirection());
                    projectile.Destroy();
                    break;
                }
                const int actualDamage = RegisterHitFx(*enemy, healthBefore, basicCrit, false, false, 6u);
                if (actualDamage <= 0)
                    continue;
                ApplyPlayerLifesteal(actualDamage);

                // Hunter class identity: every 3rd landed shot MARKS the target,
                // feeding the marked-prey damage bonus (see ScalePlayerHit). This
                // makes sustained focus fire on one enemy the Hunter's brain.
                if (_player.GetClass() == PlayerClass::Hunter)
                {
                    _hunterShotsSinceMark++;
                    if (_hunterShotsSinceMark >= _player.GetHunterMarkEvery())   // Predator's Rhythm lowers this
                    {
                        _hunterShotsSinceMark = 0;
                        enemy->ApplyMark(5.f);
                        _vfx.SpawnFloatingLabel(enemy->GetWorldPos(), "MARKED",
                                                 Color{ 255, 90, 90, 255 });
                    }
                }

                _vfx.SpawnHitEffect(elementToCastType(element), projectile.GetWorldPos(), projectile.GetDirection());
                projectile.Destroy();
                break;
            }

            int baseDamage = enemy->AsMolarbeast() != nullptr
                ? 2 : _player.GetBoltHitDamage(element);
            bool wasSlowed = enemy->IsSlowed();
            bool landed = DamageMageEnemy(*enemy, baseDamage, element,
                                          element == AbilityType::FireBolt,
                                          element == AbilityType::IceBolt,
                                          element == AbilityType::ElectricBolt);

            Character::CastType hitEffectType = element == AbilityType::FireBolt ? Character::CastType::FireSpread
                                                    : element == AbilityType::IceBolt ? Character::CastType::IceSpread
                                                                                      : Character::CastType::ElectricSpread;
            if (landed)
                SfxBank::Get().PlayProjectileImpact(element, 0.5f);   // element-specific hit (self-throttled)
            if (landed && element == AbilityType::FireBolt)
            {
                // Fireball's identity is the impact burst, not merely a red bolt.
                for (auto& nearby : _enemies)
                {
                    if (nearby.get() == enemy.get() || !nearby->IsActive() || !nearby->IsAlive()) continue;
                    if (Vector2Distance(nearby->GetWorldPos(), projectile.GetWorldPos()) <= 135.f)
                        DamageMageEnemy(*nearby, std::max(1, baseDamage / 2), element, true, false, false);
                }
                MageSpellField burst;
                burst.ability = AbilityType::FireBolt;
                burst.pos = projectile.GetWorldPos();
                burst.duration = 0.42f;
                burst.radius = 135.f;
                burst.impacted = true;
                _mageSpellFields.push_back(burst);
            }
            else if (landed && element == AbilityType::IceBolt && wasSlowed && !enemy->IsBoss())
            {
                enemy->ApplyFreeze(1.15f); // a second frost setup completes the freeze
                _vfx.SpawnFloatingLabel(enemy->GetWorldPos(), "FROZEN", SKYBLUE, 1.15f);
            }
            else if (landed && element == AbilityType::ElectricBolt)
            {
                const AttackTuning* chainTune = AttackTuningStore::Get(AttackTuningKeyForAbility(element));
                int maxJumps = (chainTune && chainTune->hasAbility)
                    ? std::max(1, (int)roundf(chainTune->maxTargets) - 1) : 4;
                float chainRange = (chainTune && chainTune->hasAbility) ? chainTune->chainRange : 280.f;
                std::vector<Enemy*> chained{ enemy.get() };
                Enemy* current = enemy.get();
                for (int jump = 0; jump < maxJumps; ++jump)
                {
                    Enemy* next = nullptr;
                    float best = chainRange;
                    for (auto& candidate : _enemies)
                    {
                        if (!candidate->IsActive() || !candidate->IsAlive()) continue;
                        if (std::find(chained.begin(), chained.end(), candidate.get()) != chained.end()) continue;
                        float distance = Vector2Distance(current->GetWorldPos(), candidate->GetWorldPos());
                        if (distance < best) { best = distance; next = candidate.get(); }
                    }
                    if (!next) break;
                    int chainDamage = std::max(1, baseDamage - 1 - jump / 2);
                    DamageMageEnemy(*next, chainDamage, element, false, false, true);
                    MageSpellField arc;
                    arc.ability = AbilityType::ElectricBolt;
                    arc.pos = current->GetWorldPos();
                    arc.strikePos = next->GetWorldPos();
                    arc.duration = 0.24f;
                    arc.impacted = true;
                    _mageSpellFields.push_back(arc);
                    chained.push_back(next);
                    current = next;
                }
            }

            _vfx.SpawnHitEffect(hitEffectType, projectile.GetWorldPos(), projectile.GetDirection());
            bool keepFlying = landed && (element == AbilityType::IceBolt) && projectile.RegisterHit(enemy.get());
            if (!keepFlying)
                projectile.Destroy();
            TriggerScreenShake(4.f, 0.05f);
            StopSound(_explosionSound);
            PlaySound(_explosionSound);
            if (!keepFlying) break;
        }
    }

    _spreadProjectiles.erase(
        std::remove_if(_spreadProjectiles.begin(), _spreadProjectiles.end(),
            [](const SpreadProjectile& p) { return !p.IsActive(); }),
        _spreadProjectiles.end());
}

void Engine::SpawnEnemyDrop(Vector2 worldPos, bool isOgre, bool isBoss)
{
    // Boss: scatter a large gold reward at the arena centre.
    if (isBoss)
    {
        // DungeonRun world == screen, so use screen centre.
        // Overworld uses the map texture centre.
        Vector2 centre;
        if (_gameState == GameState::DungeonRun)
            centre = { (float)kVirtualWidth * 0.5f, (float)kVirtualHeight * 0.5f };
        else
        {
            const float mapW = _map.width  * _mapScale;
            const float mapH = _map.height * _mapScale;
            centre = { mapW * 0.5f, mapH * 0.5f };
        }
        auto spawnGold = [&](GoldDenomination denom, float ox, float oy)
        {
            auto g = std::make_unique<GoldPickup>();
            g->Init(Vector2{ centre.x + ox, centre.y + oy }, denom);
            _pickups.push_back(std::move(g));
        };
        // 8- Ten + 6- Five + 3- Single = ~113g jackpot
        spawnGold(GoldDenomination::Ten,      0.f,    0.f);
        spawnGold(GoldDenomination::Ten,    -55.f,  -60.f);
        spawnGold(GoldDenomination::Ten,     55.f,  -60.f);
        spawnGold(GoldDenomination::Ten,   -110.f,    0.f);
        spawnGold(GoldDenomination::Ten,    110.f,    0.f);
        spawnGold(GoldDenomination::Ten,    -55.f,   60.f);
        spawnGold(GoldDenomination::Ten,     55.f,   60.f);
        spawnGold(GoldDenomination::Ten,      0.f,   90.f);
        spawnGold(GoldDenomination::Five,  -140.f,  -40.f);
        spawnGold(GoldDenomination::Five,   140.f,  -40.f);
        spawnGold(GoldDenomination::Five,  -140.f,   40.f);
        spawnGold(GoldDenomination::Five,   140.f,   40.f);
        spawnGold(GoldDenomination::Five,     0.f, -100.f);
        spawnGold(GoldDenomination::Five,     0.f,  130.f);
        spawnGold(GoldDenomination::Single, -80.f, -110.f);
        spawnGold(GoldDenomination::Single,  80.f, -110.f);
        spawnGold(GoldDenomination::Single,   0.f, -150.f);

        // Echoes: bosses guarantee a big haul (1x Ten + 2x Five = 20 cells).
        auto spawnCell = [&](CellDenomination denom, float ox, float oy)
        {
            auto c = std::make_unique<CellPickup>();
            c->Init(Vector2{ centre.x + ox, centre.y + oy }, denom);
            _pickups.push_back(std::move(c));
        };
        spawnCell(CellDenomination::Ten,    0.f,  -55.f);
        spawnCell(CellDenomination::Five, -90.f,   80.f);
        spawnCell(CellDenomination::Five,  90.f,   80.f);
        return;
    }

    // Support adds during an active boss fight give no drops.
    if (IsBossFightActive())
        return;

    // Find a valid drop position (avoid spawning inside a prop).
    Vector2 dropPos = worldPos;
    if (!IsSpawnPositionValid(dropPos))
    {
        float mapW = _map.width * _mapScale;
        float mapH = _map.height * _mapScale;
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            Vector2 candidate{
                worldPos.x + (float)GetRandomValue(-180, 180),
                worldPos.y + (float)GetRandomValue(-180, 180)
            };
            candidate.x = (float)ClampInt((int)candidate.x, 100, (int)mapW - 100);
            candidate.y = (float)ClampInt((int)candidate.y, 100, (int)mapH - 100);
            if (IsSpawnPositionValid(candidate)) { dropPos = candidate; break; }
        }
    }

    // Always drop one gold coin. Ogres are weighted toward higher denominations.
    // Ogre:    20% Single | 50% Five | 30% Ten
    // Regular: 50% Single | 35% Five | 15% Ten
    int roll = GetRandomValue(1, 100);
    GoldDenomination denom;
    if (isOgre)
        denom = (roll <= 20) ? GoldDenomination::Single
              : (roll <= 70) ? GoldDenomination::Five
              :                GoldDenomination::Ten;
    else
        denom = (roll <= 50) ? GoldDenomination::Single
              : (roll <= 85) ? GoldDenomination::Five
              :                GoldDenomination::Ten;

    // Pity: force at least Five if the last 5 drops were all Singles.
    if (denom == GoldDenomination::Single)
    {
        if (_goldDroughtCounter >= 5)
        {
            denom = GoldDenomination::Five;
            _goldDroughtCounter = 0;
        }
        else
        {
            _goldDroughtCounter++;
        }
    }
    else
    {
        _goldDroughtCounter = 0;
    }

    auto g = std::make_unique<GoldPickup>();
    g->Init(dropPos, denom);
    _pickups.push_back(std::move(g));

    // Gilded / Cursed room affixes scatter extra gold piles of the same size.
    int extraGoldPiles = (int)lroundf(GetRoomAffixDef(_currentRoomAffix).goldMult) - 1;
    for (int e = 0; e < extraGoldPiles; e++)
    {
        auto eg = std::make_unique<GoldPickup>();
        eg->Init(Vector2{ dropPos.x + (float)GetRandomValue(-42, 42),
                          dropPos.y + (float)GetRandomValue(-42, 42) }, denom);
        _pickups.push_back(std::move(eg));
    }

    // Echoes (meta progression currency, Dead Cells style):
    // Ogres/elites always drop a Five; regular enemies drop a Single 30% of the time.
    if (isOgre)
    {
        auto c = std::make_unique<CellPickup>();
        c->Init(Vector2{ dropPos.x + 30.f, dropPos.y - 20.f }, CellDenomination::Five);
        _pickups.push_back(std::move(c));
    }
    else if (GetRandomValue(1, 100) <= 30)
    {
        auto c = std::make_unique<CellPickup>();
        c->Init(Vector2{ dropPos.x + 26.f, dropPos.y - 14.f }, CellDenomination::Single);
        _pickups.push_back(std::move(c));
    }

    // Rare bonus heal drop — Scavenger raises it; ascension lowers it; the Cursed
    // room affix trades danger for a better chance at healing. Sustain rework:
    // total heal drops (chance + timed) are CAPPED per room so drop-chance
    // stacking can't turn a long fight into a healing fountain.
    int healChance = kEnemyDropChancePercent + _player.GetHealDropBonusPercent() - _ascensionMods.healDropPenaltyPct;
    if (GetRoomAffixDef(_currentRoomAffix).bonusLoot) healChance += 25;
    if (_roomHealDrops < Balance::Sustain::kMaxHealDropsPerRoom
        && GetRandomValue(1, 100) <= healChance)
    {
        auto p = std::make_unique<HealPickup>();
        p->Init(dropPos);
        _pickups.push_back(std::move(p));
        _roomHealDrops++;
    }
}

void Engine::SpawnTimedPickup()
{
    // Boss fights are self-contained - no timed pickups during them.
    // The timer keeps ticking so a pickup is not immediately ready when
    // the boss dies; it resets naturally after kDefaultTimedPickupInterval.
    if (IsBossFightActive())
        return;

    // Sustain rework: timed heals share the per-room cap with kill drops.
    if (_roomHealDrops >= Balance::Sustain::kMaxHealDropsPerRoom)
        return;
    _roomHealDrops++;

    // Timed pickups are heals only. Mana gems removed from the timed pool -
    // passive regen covers mana recovery between waves.
    float mapW = _map.width  * _mapScale;
    float mapH = _map.height * _mapScale;

    Vector2 pos{};
    int attempts = 0;
    do
    {
        pos = Vector2{
            (float)GetRandomValue(300, (int)mapW - 300),
            (float)GetRandomValue(300, (int)mapH - 300)
        };
        attempts++;
    } while (!IsSpawnPositionValid(pos) && attempts < 40);

    auto p = std::make_unique<HealPickup>();
    p->Init(pos);
    p->SetTimerSpawned(true);
    _pickups.push_back(std::move(p));
}

// =============================================================================
// Meta progression — Echoes, Poe's Altar, permanent unlocks
// =============================================================================

Vector2 Engine::GetPoeAltarPos() const
{
    // West of Zeph (who stands at sw*0.5, sh*0.42) so both prompts never overlap.
    return { (float)kVirtualWidth * 0.30f, (float)kVirtualHeight * 0.42f };
}

void Engine::HandlePlayerDeathMetaPenalty()
{
    if (_deathPenaltyApplied)
        return;
    _deathPenaltyApplied = true;

    // One-wallet meta loop (locked 2026-07-09): it's a roguelite, so DEATH WIPES
    // ALL carried gold. Only unlocked village buildings + cosmetics persist. No
    // gold carryover survives a death (supersedes the old Dead Cells retention).
    _meta.SetGoldCarryover(0);
    _player.SetGold(0);
    _player.TakeCells();
    _meta.Save();   // persist bestiary kills tallied this run
}

// ── Death -> Poe revive cutscene ───────────────────────────────────────────────
// Zelda-style: the fallen mystic stays a lone lit sprite on black while the gold
// + echoes it carried stream away and fade, then Poe (spectral phantom, purple
// frame) offers a way back and asks what the player will become. Flows straight
// into ClassSelect (class + look), which starts the next run.
static constexpr float kDeathReviveLossDur = 1.8f;   // loss/fade beat length
static constexpr float kDeathReviveTypeDur = 2.4f;   // typewriter reveal length
void Engine::BeginDeathRevive()
{
    _deathRevivePhase    = 0;
    _deathReviveTimer    = 0.f;
    _deathReviveTypeT    = 0.f;
    _deathReviveWorldPos = _player.GetWorldPos();

    // Motes: gold coins + purple echo motes drifting up and away from the body.
    // Count scales with what was lost so a rich death sprays more.
    _deathMotes.clear();
    int goldMotes = 10 + std::min(22, _deathReviveLostGold / 20);
    int echoMotes =  5 + std::min(14, _deathReviveLostCells / 2);
    auto spawn = [&](bool gold)
    {
        DeathMote m;
        float ang = (float)GetRandomValue(-1200, 1200) / 1000.f;   // spread around straight up
        float spd = (float)GetRandomValue(60, 190);
        m.pos = { _deathReviveWorldPos.x + (float)GetRandomValue(-14, 14),
                  _deathReviveWorldPos.y + (float)GetRandomValue(-24,  4) };
        m.vel = { sinf(ang) * spd, -fabsf(cosf(ang)) * spd - 40.f };   // mostly upward
        m.maxLife = (float)GetRandomValue(90, 160) / 100.f;
        m.life    = m.maxLife;
        m.gold    = gold;
        _deathMotes.push_back(m);
    };
    for (int i = 0; i < goldMotes; i++) spawn(true);
    for (int i = 0; i < echoMotes; i++) spawn(false);

    _gameState = GameState::DeathRevive;
}

void Engine::UpdateDeathRevive()
{
    const float dt = GetFrameTime();
    _gamepad.Update(_gamepadBindingsEdit);
    _deathReviveTimer += dt;

    if (MO_DEV_TOOLS && _firstDeathRevive && _demoCompleted && IsKeyPressed(KEY_F8))
    {
        _firstDeathRevive = false;
        _prologue.Complete();
        _prologueActive = false;
        _firstVillageVisit = false;
        _villageIntroDialogueActive = false;
        _villageIntroDialogueLine = 0;
        _meta.SetOnboardingComplete();
        EnterVillage();
        _fadeInTimer = 1.f;
        _fadeInDuration = 1.f;
        return;
    }

    if (_deathRevivePhase == 0)
    {
        for (auto& m : _deathMotes)
        {
            m.pos.x += m.vel.x * dt;
            m.pos.y += m.vel.y * dt;
            m.vel.x *= (1.f - 1.4f * dt);   // ease horizontal drift
            m.vel.y -= 30.f * dt;           // keep rising
            m.life  -= dt;
        }
        if (_deathReviveTimer >= kDeathReviveLossDur)
        {
            _deathRevivePhase = 1;
            _deathReviveTimer = 0.f;
            _deathReviveTypeT = 0.f;
        }
        return;
    }

    // Phase 1 — Poe dialogue.
    _deathReviveTypeT += dt;
    bool fullyShown = _deathReviveTypeT >= kDeathReviveTypeDur;

    // ESC / back abandons the run to the main menu (keeps a quit path).
    if (IsKeyPressed(KEY_ESCAPE) || (_gamepad.isActive && _gamepad.backPressed))
    {
        ResetRunState();
        _menu.Init();
        _gameState = GameState::Menu;
        return;
    }

    bool advance = IsKeyPressed(KEY_E) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                   IsMouseButtonPressed(MOUSE_LEFT_BUTTON) ||
                   (_gamepad.isActive && _gamepad.menuConfirmPressed);
    if (advance)
    {
        if (!fullyShown) { _deathReviveTypeT = kDeathReviveTypeDur; return; }
        if (_firstDeathRevive && _deathReviveDialogueLine < GetFirstPoeDialogueLineCount() - 1)
        {
            ++_deathReviveDialogueLine;
            _deathReviveTypeT = 0.f;
            _deathReviveTimer = 0.f;
            return;
        }

        if (_firstDeathRevive)
        {
            _firstDeathRevive = false;
            _prologue.Complete();
            _prologueActive = false;
            _firstVillageVisit = true;
            _villageIntroDialogueActive = false;
            _villageIntroDialogueLine = 0;
        }
        EnterVillage();
        _fadeInTimer = 1.f;
        _fadeInDuration = 1.f;
    }
}

void Engine::DrawDeathRevive()
{
    const float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;
    DrawRectangle(0, 0, (int)sw, (int)sh, BLACK);

    if (_firstDeathRevive && _demoCompleted)
        DrawText("[F8] Skip Onboarding", (int)sw - 260, 22, 20, Fade(RAYWHITE, 0.75f));

    if (_deathRevivePhase == 0)
    {
        // Lone lit sprite where the mystic fell (DungeonRun camera is centred).
        _player.DrawPlayer(_cameraPos);

        for (const auto& m : _deathMotes)
        {
            if (m.life <= 0.f) continue;
            float a = m.life / m.maxLife;                 // 1 -> 0
            Vector2 scr = { m.pos.x - _cameraPos.x + sw * 0.5f,
                            m.pos.y - _cameraPos.y + sh * 0.5f };
            if (m.gold)
            {
                DrawCircleV(scr, 4.f * a + 1.5f, Color{ 255, 210, 90, (unsigned char)(230 * a) });
                DrawCircleLinesV(scr, 4.f * a + 1.5f, Fade(Color{ 255, 240, 180, 255 }, a));
            }
            else
            {
                DrawCircleV(scr, 3.5f * a + 1.f, Color{ 190, 130, 240, (unsigned char)(220 * a) });
            }
        }

        // Fade the whole beat to black over its last stretch, into Poe's dialogue.
        float fade = (_deathReviveTimer - kDeathReviveLossDur * 0.55f) /
                     (kDeathReviveLossDur * 0.45f);
        if (fade > 0.f) DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, std::min(fade, 1.f)));
        return;
    }

    // Phase 1 — Poe (spectral phantom) + purple dialogue frame.
    if (_cellsMerchantTex.id != 0)
    {
        Texture2D poe = _cellsMerchantTex;
        float bob = sinf(_deathReviveTimer * 2.2f) * 10.f;
        int fw = (poe.height > 0 && poe.width > poe.height) ? poe.height : poe.width;   // first frame of the idle sheet
        Rectangle src = { 0, 0, (float)fw, (float)poe.height };
        float scale = sh * 0.34f / (float)poe.height;
        float dw = fw * scale, dh = poe.height * scale;
        Rectangle dst = { sw * 0.30f - dw * 0.5f, sh * 0.40f - dh * 0.5f + bob, dw, dh };
        DrawCircle((int)(sw * 0.30f), (int)(sh * 0.40f), dh * 0.55f, Fade(Color{ 120, 70, 200, 255 }, 0.20f));
        DrawTexturePro(poe, src, dst, { 0, 0 }, 0.f, Color{ 205, 180, 255, 235 });
    }

    Rectangle panel = { sw * 0.15f, sh * 0.66f, sw * 0.70f, sh * 0.26f };
    DrawRectangleRounded(panel, 0.10f, 10, Color{ 26, 16, 44, 240 });
    DrawRectangleRoundedLinesEx(panel, 0.10f, 10, 3.f, Color{ 168, 116, 232, 255 });
    DrawText("Poe", (int)(panel.x + 30.f), (int)(panel.y + 20.f), 30, Color{ 206, 176, 255, 255 });

    // Typed body text with simple word-wrap.
    const char* revivalLine = _firstDeathRevive
        ? GetFirstPoeDialogueLine(_deathReviveDialogueLine)
        : kPoeRevivalLine;
    int shown = (int)((float)strlen(revivalLine) *
                      std::min(_deathReviveTypeT / kDeathReviveTypeDur, 1.f));
    std::string vis(revivalLine, revivalLine + shown);
    {
        const int   fs   = 26;
        const float maxW = panel.width - 60.f;
        const Color col  = { 232, 224, 245, 255 };
        float x = panel.x + 30.f, y = panel.y + 66.f;
        std::string word, line;
        for (size_t i = 0; i <= vis.size(); i++)
        {
            char ch = (i < vis.size()) ? vis[i] : ' ';
            if (ch == ' ')
            {
                std::string trial = line.empty() ? word : line + " " + word;
                if (!line.empty() && MeasureText(trial.c_str(), fs) > (int)maxW)
                {
                    DrawText(line.c_str(), (int)x, (int)y, fs, col);
                    y += fs + 8.f;
                    line = word;
                }
                else line = trial;
                word.clear();
            }
            else word += ch;
        }
        if (!line.empty()) DrawText(line.c_str(), (int)x, (int)y, fs, col);
    }

    // Continue prompt once fully revealed (blinks).
    if (_deathReviveTypeT >= kDeathReviveTypeDur && fmodf(_deathReviveTimer, 1.0f) < 0.6f)
    {
        const char* p  = "Press  E  to rise";
        int fs = 22, tw = MeasureText(p, fs);
        DrawText(p, (int)(panel.x + panel.width - tw - 30.f),
                 (int)(panel.y + panel.height - fs - 16.f), fs, Color{ 190, 160, 230, 220 });
    }
}

void Engine::UpdateMetaShop(float dt)
{
    const int unlockCount = (int)MetaUnlockType::Count;

    // Brief input lock so the E/A press that opened the screen isn't consumed.
    if (_metaShopOpenTimer > 0.f)
    {
        _metaShopOpenTimer -= dt;
        return;
    }

    _gamepad.Update(_gamepadBindingsEdit);

    // -- Leave ---------------------------------------------------------------
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_E) ||
        (_gamepad.isActive && _gamepad.backPressed))
    {
        _gameState = _metaShopReturnState;
        return;
    }

    // -- Cursor navigation (keyboard arrows + d-pad), 6 cards per row ---------
    const int columns = 6;
    _metaShopNavCooldown -= dt;
    auto moveCursor = [&](int dCol, int dRow)
    {
        int col = _metaShopCursor % columns;
        int row = _metaShopCursor / columns;
        col += dCol;
        row += dRow;
        int rowCount = (unlockCount + columns - 1) / columns;
        if (row < 0) row = 0;
        if (row >= rowCount) row = rowCount - 1;
        if (col < 0) col = 0;
        if (col >= columns) col = columns - 1;
        int idx = row * columns + col;
        if (idx >= unlockCount) idx = unlockCount - 1;
        _metaShopCursor = idx;
        _metaShopNavCooldown = 0.16f;
    };

    bool navReady = _metaShopNavCooldown <= 0.f;
    bool gpLeft   = _gamepad.isActive && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT);
    bool gpRight  = _gamepad.isActive && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
    bool gpUp     = _gamepad.isActive && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_UP);
    bool gpDown   = _gamepad.isActive && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN);
    if (navReady && (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A) || gpLeft))  moveCursor(-1, 0);
    if (navReady && (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || gpRight)) moveCursor( 1, 0);
    if (navReady && (IsKeyPressed(KEY_UP)    || IsKeyPressed(KEY_W) || gpUp))    moveCursor(0, -1);
    if (navReady && (IsKeyPressed(KEY_DOWN)  || IsKeyPressed(KEY_S) || gpDown))  moveCursor(0,  1);

    // -- Mouse hover moves the cursor; click / Enter / A purchases ------------
    // Card layout must match DrawMetaShop exactly.
    const float sw = (float)kVirtualWidth;
    const float cardW = 272.f, cardH = 150.f, cardGap = 14.f;
    const float gridW = columns * cardW + (columns - 1) * cardGap;
    const float gridX = (sw - gridW) * 0.5f;
    const float gridY = 280.f;

    Vector2 mouse = GetVirtualMousePos();
    bool purchasePressed = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                           (_gamepad.isActive && _gamepad.menuConfirmPressed);

    for (int i = 0; i < unlockCount; i++)
    {
        int col = i % columns;
        int row = i / columns;
        Rectangle card{ gridX + col * (cardW + cardGap), gridY + row * (cardH + cardGap), cardW, cardH };
        if (CheckCollisionPointRec(mouse, card))
        {
            _metaShopCursor = i;
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
                purchasePressed = true;
        }
    }

    if (purchasePressed)
    {
        MetaUnlockType selected = (MetaUnlockType)_metaShopCursor;
        if (_meta.Purchase(selected))
        {
            if (_audioInitialised)
                PlaySound(_pickupSound);
        }
    }
}

void Engine::DrawMetaShop()
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    // Dark mystical backdrop — same scrolling pattern as other full screens,
    // tinted purple so the altar feels different from Zeph's shop.
    DrawScrollingCheckerboard(sw, sh, Color{ 26, 16, 34, 255 }, Color{ 34, 22, 46, 255 }, 18.f, 12.f);

    const Texture2D& cellTexture = CellPickup::GetMediumTexture();

    // -- Title + banked cell balance -------------------------------------------
    const char* title = "ECHOES OF THE FALLEN";
    int titleFontSize = 74;
    DrawText(title, (int)(sw * 0.5f - MeasureText(title, titleFontSize) * 0.5f), 70, titleFontSize, Color{ 255, 170, 230, 255 });

    const char* subtitle = "Permanent unlocks. Death cannot take these from you.";
    int subFontSize = 28;
    DrawText(subtitle, (int)(sw * 0.5f - MeasureText(subtitle, subFontSize) * 0.5f), 158, subFontSize, Color{ 190, 160, 200, 255 });

    // Poe's voice — a rotating, melancholy greeting from the keeper of Echoes.
    const char* poeLine = kPoeGreetings[_poeGreetingIdx % kPoeGreetingCount];
    int poeFs = 20;
    const char* poePrefix = "Poe: ";
    int poeLineW = MeasureText(poePrefix, poeFs) + MeasureText(poeLine, poeFs);
    int poeX = (int)(sw * 0.5f - poeLineW * 0.5f);
    DrawText(poePrefix, poeX, 190, poeFs, Color{ 214, 196, 255, 255 });
    DrawText(poeLine, poeX + MeasureText(poePrefix, poeFs), 190, poeFs, Color{ 178, 170, 198, 235 });

    // Banked balance with the cell orb icon
    const char* balanceText = TextFormat("%d", _meta.GetBankedCells());
    int balanceFontSize = 52;
    float balanceWidth = (float)MeasureText(balanceText, balanceFontSize);
    float orbSize = 52.f;
    float balanceX = sw * 0.5f - (balanceWidth + orbSize + 14.f) * 0.5f;
    Rectangle orbSrc{ 0.f, 0.f, (float)cellTexture.width, (float)cellTexture.height };
    Rectangle orbDst{ balanceX, 212.f, orbSize, orbSize };
    DrawTexturePro(cellTexture, orbSrc, orbDst, Vector2{ 0.f, 0.f }, 0.f, WHITE);
    DrawText(balanceText, (int)(balanceX + orbSize + 14.f), 214, balanceFontSize, Color{ 255, 120, 210, 255 });

    // -- Unlock cards -----------------------------------------------------------
    const int columns = 6;
    const int unlockCount = (int)MetaUnlockType::Count;
    const float cardW = 272.f, cardH = 150.f, cardGap = 14.f;
    const float gridW = columns * cardW + (columns - 1) * cardGap;
    const float gridX = (sw - gridW) * 0.5f;
    const float gridY = 280.f;

    for (int i = 0; i < unlockCount; i++)
    {
        MetaUnlockType type = (MetaUnlockType)i;
        const MetaUnlockInfo& info = GetMetaUnlockInfo(type);
        bool owned        = _meta.IsUnlocked(type);
        bool prereqOwned  = _meta.IsUnlocked(info.prerequisite);
        bool affordable   = _meta.GetBankedCells() >= info.cost;
        bool purchasable  = _meta.CanPurchase(type);
        bool cursorHere   = (_metaShopCursor == i);

        int col = i % columns;
        int row = i / columns;
        Rectangle card{ gridX + col * (cardW + cardGap), gridY + row * (cardH + cardGap), cardW, cardH };

        // Card body
        Color bodyColor = owned       ? Color{ 30, 62, 40, 235 }
                        : purchasable ? Color{ 52, 34, 66, 235 }
                        :               Color{ 34, 30, 40, 235 };
        DrawRectangleRounded(card, 0.14f, 6, bodyColor);

        // Border: gold = cursor, green = owned, purple = purchasable, grey = locked
        Color borderColor = cursorHere  ? GOLD
                          : owned       ? Color{ 90, 200, 120, 255 }
                          : purchasable ? Color{ 200, 110, 220, 255 }
                          :               Color{ 80, 75, 90, 255 };
        if (cursorHere)
        {
            // Double outline so the cursor card pops without a thickness parameter.
            Rectangle outerCard{ card.x - 3.f, card.y - 3.f, card.width + 6.f, card.height + 6.f };
            DrawRectangleRoundedLines(outerCard, 0.14f, 6, borderColor);
        }
        DrawRectangleRoundedLines(card, 0.14f, 6, borderColor);

        // Name
        Color nameColor = (owned || purchasable) ? RAYWHITE : Color{ 140, 130, 150, 255 };
        DrawText(info.name, (int)(card.x + 16.f), (int)(card.y + 14.f), 26, nameColor);

        // Description (already contains \n line breaks)
        DrawText(info.description, (int)(card.x + 16.f), (int)(card.y + 52.f), 20,
            (owned || purchasable) ? Color{ 200, 195, 210, 255 } : Color{ 120, 112, 130, 255 });

        // Bottom row: OWNED tag, price, or prerequisite hint
        if (owned)
        {
            DrawText("OWNED", (int)(card.x + 16.f), (int)(card.y + cardH - 40.f), 24, Color{ 90, 200, 120, 255 });
        }
        else if (!prereqOwned)
        {
            const char* prereqText = TextFormat("Requires %s", GetMetaUnlockInfo(info.prerequisite).name);
            DrawText(prereqText, (int)(card.x + 16.f), (int)(card.y + cardH - 38.f), 18, Color{ 170, 120, 120, 255 });
        }
        else
        {
            float priceOrbSize = 26.f;
            Rectangle priceOrbDst{ card.x + 16.f, card.y + cardH - 42.f, priceOrbSize, priceOrbSize };
            DrawTexturePro(cellTexture, orbSrc, priceOrbDst, Vector2{ 0.f, 0.f }, 0.f, WHITE);
            const char* priceText = TextFormat("%d", info.cost);
            DrawText(priceText, (int)(priceOrbDst.x + priceOrbSize + 10.f), (int)(card.y + cardH - 40.f), 24,
                affordable ? Color{ 255, 120, 210, 255 } : Color{ 200, 80, 80, 255 });
        }
    }

    // -- Footer hints -------------------------------------------------------------
    const char* hint = _gamepad.isActive
        ? "D-Pad: Navigate    A: Unlock    B: Back"
        : "Arrows: Navigate    Enter / Click: Unlock    ESC / E: Back";
    int hintFontSize = 24;
    DrawText(hint, (int)(sw * 0.5f - MeasureText(hint, hintFontSize) * 0.5f), (int)(sh - 60.f), hintFontSize, Color{ 170, 160, 180, 255 });

    const char* lifetimeText = TextFormat("Lifetime Echoes banked: %d", _meta.GetLifetimeCells());
    DrawText(lifetimeText, 40, (int)(sh - 60.f), 22, Color{ 130, 120, 140, 255 });
}

// =============================================================================
// Cursed Shrine — risk/reward blessing+curse pacts (#19)
// =============================================================================
// A wager tier: pay `cost` gold to make the whole upcoming biome tougher
// (enemyHp / enemyDmg mults) in exchange for a `reward` mult on all gold, XP
// and Echoes earned until the next shop.
struct WagerTier
{
    const char* name;
    int         cost;      // gold to activate
    float       enemyHp;   // enemy HP mult for the biome
    float       enemyDmg;  // enemy damage mult for the biome
    float       reward;    // gold / XP / Echo gain mult while active
    const char* risk;      // difficulty line (two lines via \n)
    const char* payout;    // reward line
    Color       tint;
};

static const WagerTier kWagerTiers[] = {
    { "Cursed", 40,  1.40f, 1.30f, 1.60f, "+40% enemy HP\n+30% enemy damage",   "+60% gold, XP\n& Echoes",  Color{ 235, 180, 90, 255 } },
    { "Damned", 120, 1.90f, 1.70f, 2.40f, "+90% enemy HP\n+70% enemy damage",   "+140% gold, XP\n& Echoes", Color{ 235, 120, 90, 255 } },
    { "Doomed", 300, 2.60f, 2.20f, 3.60f, "+160% enemy HP\n+120% enemy damage", "+260% gold, XP\n& Echoes", Color{ 230, 70, 90, 255 } },
};
static const int kWagerTierCount = (int)(sizeof(kWagerTiers) / sizeof(kWagerTiers[0]));

Vector2 Engine::GetCurseShrinePos() const
{
    // East of Zeph, mirroring Poe's Altar on the west.
    return { (float)kVirtualWidth * 0.70f, (float)kVirtualHeight * 0.42f };
}

// ── Generic ChoiceCardScreen ────────────────────────────────────────────────
// Extracted verbatim from the Cursed Wager UI (below) so every decision room
// reuses the same debugged keyboard/gamepad/mouse picker. Layout constants are
// unchanged from the original wager, so the wager still renders pixel-identically.
void Engine::OpenChoiceCards(ChoiceCardScreen& s)
{
    s.cursor    = 0;
    s.openTimer = 0.25f;
    s.denyFlash = 0.f;
    s.result    = -1;
}

void Engine::UpdateChoiceCards(ChoiceCardScreen& s)
{
    s.result = -1;
    const int n = (int)s.cards.size();
    if (n <= 0) return;

    if (s.openTimer > 0.f) { s.openTimer -= GetFrameTime(); return; }
    if (s.denyFlash > 0.f) s.denyFlash -= GetFrameTime();

    _gamepad.Update(_gamepadBindingsEdit);

    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    const float cardW = 420.f, cardH = 460.f, gap = 60.f;
    const float totalW = n * cardW + (n - 1) * gap;
    const float startX = (sw - totalW) * 0.5f;
    const float cardY  = sh * 0.5f - cardH * 0.5f + 40.f;

    if (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A)) s.cursor = (s.cursor + n - 1) % n;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) s.cursor = (s.cursor + 1) % n;
    if (_gamepad.isActive)
    {
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT))  s.cursor = (s.cursor + n - 1) % n;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) s.cursor = (s.cursor + 1) % n;
    }

    Vector2 mouse = GetVirtualMousePos();
    bool confirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                   (_gamepad.isActive && _gamepad.menuConfirmPressed);

    for (int i = 0; i < n; i++)
    {
        Rectangle card{ startX + i * (cardW + gap), cardY, cardW, cardH };
        if (CheckCollisionPointRec(mouse, card))
        {
            s.cursor = i;
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) confirm = true;
        }
    }

    // Walk away without choosing (Esc / gamepad B).
    if (IsKeyPressed(KEY_ESCAPE) ||
        (_gamepad.isActive && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)))
    {
        s.result = -2;
        return;
    }

    if (confirm)
    {
        const ChoiceCard& c = s.cards[s.cursor];
        if (!c.enabled || _player.GetGold() < c.costGold)
        {
            s.denyFlash = 0.6f;   // can't afford / unavailable — flash and stay
            return;
        }
        s.result = s.cursor;      // caller applies the effect + deducts cost
    }
}

void Engine::DrawChoiceCards(const ChoiceCardScreen& s)
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    const int   n  = (int)s.cards.size();

    DrawScrollingCheckerboard(sw, sh, s.bgColorA, s.bgColorB, 18.f, 12.f);
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.35f));

    DrawText(s.title.c_str(), (int)(sw * 0.5f - MeasureText(s.title.c_str(), 70) * 0.5f), 56, 70, s.titleColor);
    DrawText(s.subtitle.c_str(), (int)(sw * 0.5f - MeasureText(s.subtitle.c_str(), 26) * 0.5f), 146, 26, Color{ 200, 160, 175, 255 });

    // Show the player's purse so affordability is obvious (flashes red on denial).
    if (s.showPurse)
    {
        const char* goldLine = TextFormat("Your gold: %d", _player.GetGold());
        Color goldCol = (s.denyFlash > 0.f) ? Color{ 245, 90, 90, 255 } : Color{ 255, 210, 120, 255 };
        DrawText(goldLine, (int)(sw * 0.5f - MeasureText(goldLine, 30) * 0.5f), 190, 30, goldCol);
    }

    const float cardW = 420.f, cardH = 460.f, gap = 60.f;
    const float totalW = n * cardW + (n - 1) * gap;
    const float startX = (sw - totalW) * 0.5f;
    const float cardY  = sh * 0.5f - cardH * 0.5f + 40.f;

    for (int i = 0; i < n; i++)
    {
        const ChoiceCard& c = s.cards[i];
        Rectangle card{ startX + i * (cardW + gap), cardY, cardW, cardH };
        bool sel    = (i == s.cursor);
        bool afford = c.enabled && _player.GetGold() >= c.costGold;

        DrawRectangleRounded(card, 0.06f, 8, sel ? Color{ 60, 30, 42, 245 } : Color{ 42, 24, 32, 235 });
        if (sel)
        {
            Rectangle outer{ card.x - 4.f, card.y - 4.f, card.width + 8.f, card.height + 8.f };
            DrawRectangleRoundedLines(outer, 0.06f, 8, GOLD);
        }
        DrawRectangleRoundedLines(card, 0.06f, 8, sel ? GOLD : Color{ 110, 70, 84, 255 });

        // Optional colored accent stripe across the top of the card.
        if (c.accentBar)
            DrawRectangleRounded({ card.x + 16.f, card.y + 12.f, card.width - 32.f, 6.f },
                                 0.9f, 4, Fade(c.tint, sel ? 0.95f : 0.55f));

        DrawText(c.title.c_str(), (int)(card.x + card.width * 0.5f - MeasureText(c.title.c_str(), 40) * 0.5f),
                 (int)(card.y + 26.f), 40, sel ? c.tint : RAYWHITE);

        // Optional flavor quote under the title.
        if (!c.flavor.empty())
            DrawText(c.flavor.c_str(), (int)(card.x + card.width * 0.5f - MeasureText(c.flavor.c_str(), 20) * 0.5f),
                     (int)(card.y + 86.f), 20, Fade(sel ? c.tint : Color{ 200, 190, 200, 255 }, 0.9f));

        // Cost (free cards draw no cost line).
        if (c.costGold > 0)
        {
            const char* costTxt = TextFormat("%d gold", c.costGold);
            Color costCol = afford ? Color{ 255, 210, 120, 255 } : Color{ 150, 90, 90, 255 };
            DrawText(costTxt, (int)(card.x + card.width * 0.5f - MeasureText(costTxt, 30) * 0.5f),
                     (int)(card.y + 82.f), 30, costCol);
        }

        // Downside block (RISK / CURSE / COST).
        DrawText(c.downsideHeader.c_str(), (int)(card.x + 30.f), (int)(card.y + 150.f), 24, Color{ 245, 100, 110, 255 });
        DrawText(c.downsideText.c_str(),   (int)(card.x + 30.f), (int)(card.y + 186.f), 26, Color{ 240, 200, 205, 255 });

        // Upside block (REWARD).
        DrawText(c.upsideHeader.c_str(), (int)(card.x + 30.f), (int)(card.y + 300.f), 24, Color{ 120, 235, 150, 255 });
        DrawText(c.upsideText.c_str(),   (int)(card.x + 30.f), (int)(card.y + 336.f), 26, Color{ 205, 240, 210, 255 });

        if (sel)
        {
            const char* pick = !c.enabled ? c.disabledReason.c_str()
                             : afford      ? s.confirmVerb.c_str()
                                           : "NOT ENOUGH GOLD";
            DrawText(pick, (int)(card.x + card.width * 0.5f - MeasureText(pick, 26) * 0.5f),
                     (int)(card.y + card.height - 48.f), 26, afford ? GOLD : Color{ 235, 90, 90, 255 });
        }
    }

    const char* hint = _gamepad.isActive
        ? TextFormat("D-Pad: Choose    A: %s    B: Leave", s.confirmHint.c_str())
        : TextFormat("Arrows / Click: Choose    Enter / Click: %s    Esc: Leave", s.confirmHint.c_str());
    DrawText(hint, (int)(sw * 0.5f - MeasureText(hint, 24) * 0.5f), (int)(sh - 54.f), 24, Color{ 190, 160, 175, 255 });
}

// ── Cursed Wager — first consumer of ChoiceCardScreen ───────────────────────
void Engine::OpenCurseShrine()
{
    _wagerScreen = ChoiceCardScreen{};
    _wagerScreen.title       = "CURSED WAGER";
    _wagerScreen.subtitle    = "Pay in gold to curse the whole biome. Survive it for far richer spoils.";
    _wagerScreen.titleColor  = Color{ 235, 90, 120, 255 };
    _wagerScreen.confirmVerb = "WAGER";   // pick label
    _wagerScreen.confirmHint = "Wager";   // bottom hint verb
    _wagerScreen.showPurse   = true;

    for (int i = 0; i < kWagerTierCount; i++)
    {
        const WagerTier& w = kWagerTiers[i];
        ChoiceCard c;
        c.title        = w.name;
        c.costGold     = w.cost;
        c.downsideText = w.risk;    // headers default to RISK / REWARD
        c.upsideText   = w.payout;
        c.tint         = w.tint;
        _wagerScreen.cards.push_back(c);
    }

    OpenChoiceCards(_wagerScreen);
    _gameState = GameState::CurseShrine;
}

void Engine::UpdateCurseShrine()
{
    UpdateChoiceCards(_wagerScreen);

    if (_wagerScreen.result == -2)          // walked away
    {
        _gameState = GameState::DungeonRun;
        return;
    }
    if (_wagerScreen.result >= 0)           // wager confirmed (already affordable)
    {
        const WagerTier& w = kWagerTiers[_wagerScreen.result];
        _player.AddGold(-w.cost);
        _wagerTier = _wagerScreen.result + 1;        // curses this biome's enemies
        _player.SetWagerRewardMult(w.reward);        // richer gold / XP / Echoes
        _curseShrineUsed = true;                     // one wager per biome
        if (_audioInitialised) SfxBank::Get().Play(SfxId::UIConfirm, 0.7f);
        _gameState = GameState::DungeonRun;
    }
}

void Engine::DrawCurseShrine()
{
    DrawChoiceCards(_wagerScreen);
}

// =============================================================================
// Decision rooms + Risk Shrine (room-events P1/P2)
// =============================================================================
// Risk Shrine contracts: danger in the NEXT combat room for reward in that room.
// Effects use only the existing HP/damage + gold/XP levers so every downside has
// an in-world tell (tougher enemies) and no new enemy plumbing is needed.
struct RiskContract
{
    const char* name;
    const char* flavor;    // short evocative quote under the title
    const char* risk;      // downside text (\n allowed)
    const char* reward;    // upside text
    float enemyHpMult;
    float enemyDmgMult;
    float goldMult;
    float xpMult;
    Color tint;
};
static const RiskContract kRiskContracts[] = {
    { "Greed",  "\"Fortune favors the reckless.\"", "Enemies strike +30% harder\nin the next room", "Double all GOLD\nin the next room", 1.00f, 1.30f, 2.0f, 1.0f, Color{ 235, 205, 90, 255 } },
    { "Hunger", "\"Feed on the danger.\"",          "Enemies are +35% tougher\nin the next room",   "Double all XP\nin the next room",   1.35f, 1.00f, 1.0f, 2.0f, Color{ 140, 220, 120, 255 } },
    { "Pride",  "\"Prove you fear nothing.\"",      "Enemies +30% tougher\nAND deadlier",           "Double GOLD\nand +50% XP",          1.30f, 1.30f, 2.0f, 1.5f, Color{ 230, 120, 110, 255 } },
};
static const int kRiskContractCount = (int)(sizeof(kRiskContracts) / sizeof(kRiskContracts[0]));

// A decision room's shrine sits at room centre (player spawns at the bottom).
Vector2 Engine::GetDecisionShrinePos() const
{
    return { (float)kVirtualWidth * 0.5f, (float)kVirtualHeight * 0.42f };
}

// Rolled once per dungeon, right after _dungeonRoomStates is cleared on a fresh
// run. Tags 1 guaranteed + 1 (~50%) Standard rooms as Risk Shrines. Done at
// generation time so it never rerolls on re-entry and seeded runs stay identical.
void Engine::RollDungeonRoomSpecials()
{
    const auto& rooms = _dungeonGen.GetRooms();
    int startIdx = _dungeonGen.GetStartIndex();
    int bossIdx  = _dungeonGen.GetBossIndex();
    int keyIdx   = _dungeonGen.GetKeyIndex();   // KEY room stays type Standard — must skip it

    // Collect eligible Standard rooms. Skip start/boss/key so a decision room
    // never overwrites the gem room or a structural room.
    std::vector<int> candidates;
    for (int i = 0; i < (int)rooms.size(); i++)
        if (i != startIdx && i != bossIdx && i != keyIdx &&
            rooms[i].type == RoomType::Standard)
            candidates.push_back(i);

    for (int i = (int)candidates.size() - 1; i > 0; i--)
        std::swap(candidates[i], candidates[GetRandomValue(0, i)]);

    int count = candidates.empty() ? 0 : (1 + (GetRandomValue(0, 1)));   // 1, sometimes 2
    count = std::min(count, (int)candidates.size());

    for (int n = 0; n < count; n++)
    {
        DungeonRoomState& st = _dungeonRoomStates[candidates[n]];
        st.special = RoomSpecialType::RiskShrine;
        // Offer all three contracts for now (with only 3 authored, no sub-roll).
        st.specialOptions[0] = 0;
        st.specialOptions[1] = 1;
        st.specialOptions[2] = 2;
        st.specialClaimed = false;
        st.specialChoice  = -1;
    }
}

void Engine::OpenDecisionRoom()
{
    DungeonRoomState& st = _dungeonRoomStates[_dungeonRoomIdx];
    _decisionScreen = ChoiceCardScreen{};
    _decisionScreen.showPurse = false;                 // Risk Shrine contracts are free

    if (st.special == RoomSpecialType::RiskShrine)
    {
        _decisionScreen.title       = "RISK SHRINE";
        _decisionScreen.subtitle    = "Bind a contract: greater danger ahead, greater spoils. Or walk on.";
        _decisionScreen.titleColor  = Color{ 235, 150, 90, 255 };
        _decisionScreen.confirmVerb = "BIND";
        _decisionScreen.confirmHint = "Bind";

        bool atCap = AcceptedModifierCount() >= kMaxAcceptedModifiers;
        for (int i = 0; i < 3; i++)
        {
            int ci = st.specialOptions[i];
            if (ci < 0 || ci >= kRiskContractCount) continue;
            const RiskContract& rc = kRiskContracts[ci];
            ChoiceCard c;
            c.title        = rc.name;
            c.flavor       = rc.flavor;
            c.accentBar    = true;
            c.downsideText = rc.risk;
            c.upsideText   = rc.reward;
            c.tint         = rc.tint;
            if (atCap) { c.enabled = false; c.disabledReason = "TOO MANY BURDENS"; }
            _decisionScreen.cards.push_back(c);
        }
    }

    OpenChoiceCards(_decisionScreen);
    _gameState = GameState::DecisionRoom;
}

void Engine::UpdateDecisionRoom()
{
    UpdateChoiceCards(_decisionScreen);

    if (_decisionScreen.result == -2)      // walked away — shrine stays available
    {
        _gameState = GameState::DungeonRun;
        return;
    }
    if (_decisionScreen.result >= 0)
    {
        DungeonRoomState& st = _dungeonRoomStates[_dungeonRoomIdx];
        if (st.special == RoomSpecialType::RiskShrine)
        {
            int ci = st.specialOptions[_decisionScreen.result];
            if (ci >= 0 && ci < kRiskContractCount)
            {
                const RiskContract& rc = kRiskContracts[ci];
                RunModifier m;
                m.label        = rc.name;
                m.enemyHpMult  = rc.enemyHpMult;
                m.enemyDmgMult = rc.enemyDmgMult;
                m.goldMult     = rc.goldMult;
                m.xpMult       = rc.xpMult;
                m.tint         = rc.tint;
                m.active       = false;      // arms for the next combat room
                m.roomsRemaining = 1;
                _runModifiers.push_back(m);
            }
        }
        st.specialClaimed = true;
        st.specialChoice  = _decisionScreen.result;
        if (_audioInitialised) SfxBank::Get().Play(SfxId::UIConfirm, 0.7f);
        _gameState = GameState::DungeonRun;
    }
}

void Engine::DrawDecisionRoom()
{
    DrawChoiceCards(_decisionScreen);
}

// On entering a fresh combat room: arm→active, and fold the product of active
// modifiers into the enemy-scaling + reward levers used while this room runs.
void Engine::ActivatePendingModifiers()
{
    float hp = 1.f, dmg = 1.f, gold = 1.f, xp = 1.f;
    for (RunModifier& m : _runModifiers)
    {
        m.active = true;
        hp   *= m.enemyHpMult;
        dmg  *= m.enemyDmgMult;
        gold *= m.goldMult;
        xp   *= m.xpMult;
    }
    _roomModHpMult  = hp;
    _roomModDmgMult = dmg;
    _player.SetContractGoldMult(gold);
    _player.SetContractXpMult(xp);
    _player.TakeContractBonusGold();   // zero the accumulators for a clean count
    _player.TakeContractBonusXp();
}

// On combat-room clear: bank the bonus, resolve/expire finished contracts, show a
// toast, and clear the room levers (A2 result toast + A6 duration tick).
void Engine::ResolveActiveModifiersOnRoomClear()
{
    if (_runModifiers.empty()) return;

    int bonusGold = _player.TakeContractBonusGold();
    int bonusXp   = _player.TakeContractBonusXp();

    std::string lastName;
    std::string shopContractResult;
    bool resolvedAny = false;
    for (auto it = _runModifiers.begin(); it != _runModifiers.end(); )
    {
        if (it->active && --it->roomsRemaining <= 0)
        {
            lastName = it->label;
            resolvedAny = true;

            if (it->shopContract != ShopContractType::Count)
            {
                bool succeeded = false;
                switch (it->shopContract)
                {
                case ShopContractType::Untouched:
                    succeeded = _player.GetTelemDamageTaken() <= 0.01f;
                    break;
                case ShopContractType::ArcaneRestraint:
                    succeeded = _player.GetTelemAbilityCasts() <= 2;
                    break;
                case ShopContractType::AgainstTheClock:
                {
                    float limit = (_currentRoomType == RoomType::Boss) ? 150.f
                                : (_currentRoomType == RoomType::Elite) ? 75.f : 50.f;
                    succeeded = (float)(GetTime() - _roomEnterTime) <= limit;
                    break;
                }
                default: break;
                }

                if (succeeded)
                {
                    if (it->shopContract == ShopContractType::Untouched)
                    {
                        AbilityType target = AbilityType::None;
                        int lowestLevel = 999;
                        for (AbilityType ability : kAllAbilities)
                        {
                            if (IsUltimateAbility(ability) || !_player.HasLearnedAbility(ability) ||
                                !_player.CanUpgradeAbility(ability)) continue;
                            int level = _player.GetAbilityLevel(ability);
                            if (level < lowestLevel) { lowestLevel = level; target = ability; }
                        }
                        if (target != AbilityType::None)
                            _player.UpgradeAbility(target);
                        else
                            _player.GrantShopWard();
                    }
                    else if (it->shopContract == ShopContractType::ArcaneRestraint)
                    {
                        _player.GrantShopWard();
                        _player.GrantShopFreeCast(2);
                    }
                    else if (it->shopContract == ShopContractType::AgainstTheClock)
                    {
                        ++_pendingRelicChoices;
                    }
                    shopContractResult = it->label + " COMPLETE - REWARD EARNED";
                }
                else
                {
                    shopContractResult = it->label + " FAILED - NO EXTRA PENALTY";
                }
            }
            it = _runModifiers.erase(it);
        }
        else ++it;
    }

    if (!shopContractResult.empty())
    {
        _contractToastText  = shopContractResult;
        _contractToastTimer = 4.f;
    }
    else if (resolvedAny)
    {
        _contractToastText  = TextFormat("%s HONORED   +%d gold   +%d XP", lastName.c_str(), bonusGold, bonusXp);
        _contractToastTimer = 4.f;
    }

    // The contracted room is done — recompute levers from whatever remains active.
    _roomModHpMult = 1.f;
    _roomModDmgMult = 1.f;
    _player.SetContractGoldMult(1.f);
    _player.SetContractXpMult(1.f);
}

void Engine::ClearRunModifiers()
{
    _runModifiers.clear();
    _roomModHpMult  = 1.f;
    _roomModDmgMult = 1.f;
    _player.SetContractGoldMult(1.f);
    _player.SetContractXpMult(1.f);
    _contractToastTimer = 0.f;
}

// =============================================================================
// Bestiary — a catalogue of every foe with lifetime kill counts (#20)
// =============================================================================
static const char* kBestiaryCatalogue[] = {
    // Regular foes
    "Grunt", "Slime", "Skeleton Archer", "Flame Wisp", "Sporeling", "Shieldbearer",
    "Phantom", "Bomber Imp", "Warchief", "Living Blade", "Toxic Vermin",
    // Bosses / elites
    "Cyclops", "Ogre", "Molarbeast", "Abyss Slime", "Pumpkin Jack", "Minotaur",
    "Werewolf", "Chomp Bug", "Osiris", "Titan Guard", "Ancient Bear",
};
static const int kBestiaryCatalogueCount = (int)(sizeof(kBestiaryCatalogue) / sizeof(kBestiaryCatalogue[0]));

void Engine::UpdateBestiary()
{
    _gamepad.Update(_gamepadBindingsEdit);
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_B) || IsKeyPressed(KEY_ENTER) ||
        (_gamepad.isActive && (_gamepad.backPressed || _gamepad.menuConfirmPressed)))
    {
        _gameState = _bestiaryReturnState;
        if (_bestiaryReturnState == GameState::Menu) _menu.Init();
    }
}

void Engine::DrawBestiary()
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    DrawScrollingCheckerboard(sw, sh, Color{ 18, 22, 30, 255 }, Color{ 24, 28, 38, 255 }, 16.f, 11.f);

    const char* title = "BESTIARY";
    DrawText(title, (int)(sw * 0.5f - MeasureText(title, 70) * 0.5f), 54, 70, RAYWHITE);

    const auto& kills = _meta.GetBestiary();
    int totalKinds = 0, discovered = 0, totalKills = 0;

    const int cols = 4;
    const float cardW = 400.f, cardH = 110.f, gapX = 30.f, gapY = 18.f;
    const float gridW = cols * cardW + (cols - 1) * gapX;
    const float startX = (sw - gridW) * 0.5f;
    const float startY = 168.f;

    for (int i = 0; i < kBestiaryCatalogueCount; i++)
    {
        const char* name = kBestiaryCatalogue[i];
        auto it = kills.find(name);
        int count = (it != kills.end()) ? it->second : 0;
        bool known = count > 0;
        totalKinds++;
        if (known) { discovered++; totalKills += count; }

        int col = i % cols, row = i / cols;
        Rectangle card{ startX + col * (cardW + gapX), startY + row * (cardH + gapY), cardW, cardH };

        DrawRectangleRounded(card, 0.10f, 6, known ? Color{ 34, 42, 54, 235 } : Color{ 26, 28, 34, 220 });
        DrawRectangleRoundedLines(card, 0.10f, 6, known ? Color{ 90, 130, 170, 255 } : Color{ 70, 74, 84, 255 });

        if (known)
        {
            DrawText(name, (int)(card.x + 20.f), (int)(card.y + 20.f), 30, RAYWHITE);
            DrawText(TextFormat("Slain: %d", count), (int)(card.x + 20.f), (int)(card.y + 64.f), 26, Color{ 150, 220, 170, 255 });
        }
        else
        {
            DrawText("? ? ?", (int)(card.x + 20.f), (int)(card.y + 20.f), 30, Color{ 120, 124, 134, 255 });
            DrawText("Not yet encountered", (int)(card.x + 20.f), (int)(card.y + 66.f), 22, Color{ 100, 104, 114, 255 });
        }
    }

    const char* summary = TextFormat("Discovered %d / %d      Total kills: %d", discovered, totalKinds, totalKills);
    DrawText(summary, (int)(sw * 0.5f - MeasureText(summary, 28) * 0.5f), (int)(sh - 96.f), 28, Color{ 200, 200, 215, 255 });
    const char* hint = "ESC / B: Back";
    DrawText(hint, (int)(sw * 0.5f - MeasureText(hint, 24) * 0.5f), (int)(sh - 56.f), 24, Color{ 160, 164, 178, 255 });
}

// =============================================================================
// Relic choice — pick 1 of 3 relics after an elite/boss kill (#22)
// =============================================================================
void Engine::OpenRelicChoice()
{
    // Build a weighted pool of unowned relics (commons more likely), then draw
    // up to 3 distinct ones — without mutating player state.
    std::vector<RelicType> pool;
    std::vector<int>       weights;
    for (int i = 0; i < (int)RelicType::Count; i++)
    {
        RelicType t = (RelicType)i;
        if (_player.HasRelic(t)) continue;
        int w = (GetRelicInfo(t).rarity == RelicRarity::Common) ? 6
              : (GetRelicInfo(t).rarity == RelicRarity::Rare)   ? 3 : 1;
        pool.push_back(t);
        weights.push_back(w);
    }

    int found = 0;
    RelicType picked[3] = { RelicType::Count, RelicType::Count, RelicType::Count };
    while (found < 3 && !pool.empty())
    {
        int total = 0; for (int w : weights) total += w;
        int roll = GetRandomValue(1, total);
        int idx = 0;
        for (size_t k = 0; k < pool.size(); k++) { roll -= weights[k]; if (roll <= 0) { idx = (int)k; break; } }
        picked[found++] = pool[idx];
        pool.erase(pool.begin() + idx);
        weights.erase(weights.begin() + idx);
    }

    if (found == 0)   // nothing left to offer; clear the debt
    {
        _pendingRelicChoices = 0;
        return;
    }
    for (int i = 0; i < 3; i++) _relicChoices[i] = (i < found) ? picked[i] : RelicType::Count;
    _relicChoiceCursor    = 0;
    _relicChoiceOpenTimer = 0.25f;
    _gameState            = GameState::RelicChoice;
}

void Engine::UpdateRelicChoice()
{
    if (_relicChoiceOpenTimer > 0.f) { _relicChoiceOpenTimer -= GetFrameTime(); return; }
    _gamepad.Update(_gamepadBindingsEdit);

    int count = 0;
    for (int i = 0; i < 3; i++) if (_relicChoices[i] != RelicType::Count) count++;
    if (count <= 0) { _pendingRelicChoices = 0; _gameState = GameState::DungeonRun; return; }

    const float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;
    const float cardW = 380.f, cardH = 440.f, gap = 50.f;
    const float totalW = count * cardW + (count - 1) * gap;
    const float startX = (sw - totalW) * 0.5f;
    const float cardY  = sh * 0.5f - cardH * 0.5f + 20.f;

    if (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A)) _relicChoiceCursor = (_relicChoiceCursor + count - 1) % count;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) _relicChoiceCursor = (_relicChoiceCursor + 1) % count;
    if (_gamepad.isActive)
    {
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT))  _relicChoiceCursor = (_relicChoiceCursor + count - 1) % count;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) _relicChoiceCursor = (_relicChoiceCursor + 1) % count;
    }

    Vector2 mouse = GetVirtualMousePos();
    bool confirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                   (_gamepad.isActive && _gamepad.menuConfirmPressed);
    for (int i = 0; i < count; i++)
    {
        Rectangle card{ startX + i * (cardW + gap), cardY, cardW, cardH };
        if (CheckCollisionPointRec(mouse, card))
        {
            _relicChoiceCursor = i;
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) confirm = true;
        }
    }

    if (confirm)
    {
        GrantRelic(_relicChoices[_relicChoiceCursor]);
        if (_audioInitialised) PlaySound(_pickupSound);
        _pendingRelicChoices--;
        _gameState = GameState::DungeonRun;
    }
}

void Engine::DrawRelicChoice()
{
    const float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;
    DrawScrollingCheckerboard(sw, sh, Color{ 18, 20, 32, 255 }, Color{ 26, 28, 42, 255 }, 16.f, 11.f);
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.4f));

    const char* title = "CHOOSE A RELIC";
    DrawText(title, (int)(sw * 0.5f - MeasureText(title, 66) * 0.5f), 68, 66, Color{ 235, 210, 140, 255 });
    const char* sub = "A spoil of victory. Pick one to shape your build.";
    DrawText(sub, (int)(sw * 0.5f - MeasureText(sub, 26) * 0.5f), 156, 26, Color{ 190, 190, 210, 230 });

    int count = 0;
    for (int i = 0; i < 3; i++) if (_relicChoices[i] != RelicType::Count) count++;
    const float cardW = 380.f, cardH = 440.f, gap = 50.f;
    const float totalW = count * cardW + (count - 1) * gap;
    const float startX = (sw - totalW) * 0.5f;
    const float cardY  = sh * 0.5f - cardH * 0.5f + 20.f;

    for (int i = 0; i < count; i++)
    {
        const RelicInfo& info = GetRelicInfo(_relicChoices[i]);
        Rectangle card{ startX + i * (cardW + gap), cardY, cardW, cardH };
        bool sel = (i == _relicChoiceCursor);

        Color rar = (info.rarity == RelicRarity::Epic)   ? Color{ 200, 120, 240, 255 }
                  : (info.rarity == RelicRarity::Rare)   ? Color{ 90, 170, 255, 255 }
                  :                                        Color{ 150, 210, 150, 255 };

        DrawRectangleRounded(card, 0.06f, 8, sel ? Color{ 46, 42, 58, 245 } : Color{ 34, 32, 44, 235 });
        if (sel)
        {
            Rectangle outer{ card.x - 4.f, card.y - 4.f, card.width + 8.f, card.height + 8.f };
            DrawRectangleRoundedLines(outer, 0.06f, 8, GOLD);
        }
        DrawRectangleRoundedLines(card, 0.06f, 8, sel ? GOLD : rar);

        DrawText(info.name, (int)(card.x + card.width * 0.5f - MeasureText(info.name, 34) * 0.5f),
                 (int)(card.y + 28.f), 34, sel ? GOLD : RAYWHITE);

        // Rarity + archetype badge
        const char* rarTxt = GetRelicRarityName(info.rarity);
        DrawText(rarTxt, (int)(card.x + card.width * 0.5f - MeasureText(rarTxt, 22) * 0.5f),
                 (int)(card.y + 78.f), 22, rar);
        const char* arch = GetRelicArchetypeName(info.archetype);
        DrawText(arch, (int)(card.x + card.width * 0.5f - MeasureText(arch, 18) * 0.5f),
                 (int)(card.y + 110.f), 18, Color{ 160, 160, 180, 220 });

        // Relic icon on a rarity-colored halo (falls back to a gem if missing).
        Vector2 gem{ card.x + card.width * 0.5f, card.y + 200.f };
        DrawCircleV(gem, 50.f, Fade(rar, 0.22f));
        DrawCircleLines((int)gem.x, (int)gem.y, 50.f, Fade(rar, 0.7f));
        const Texture2D* ricon = GetRelicIcon(_relicChoices[i]);
        if (ricon && ricon->id != 0)
        {
            float s = 5.0f;
            Rectangle src{ 0.f, 0.f, (float)ricon->width, (float)ricon->height };
            Rectangle dst{ gem.x - ricon->width * s * 0.5f, gem.y - ricon->height * s * 0.5f, ricon->width * s, ricon->height * s };
            DrawTexturePro(*ricon, src, dst, Vector2{}, 0.f, WHITE);
        }
        else
        {
            DrawPoly(gem, 6, 38.f, (float)GetTime() * 20.f, rar);
        }

        // Description (wrap on \n already present in relic text).
        int dy = (int)(card.y + 280.f);
        std::string desc = info.description;
        std::size_t start = 0;
        while (start <= desc.size())
        {
            std::size_t nl = desc.find('\n', start);
            std::string ln = (nl == std::string::npos) ? desc.substr(start) : desc.substr(start, nl - start);
            DrawText(ln.c_str(), (int)(card.x + card.width * 0.5f - MeasureText(ln.c_str(), 21) * 0.5f), dy, 21, Color{ 205, 205, 220, 235 });
            dy += 28;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }

        if (sel)
        {
            const char* take = "TAKE";
            DrawText(take, (int)(card.x + card.width * 0.5f - MeasureText(take, 26) * 0.5f),
                     (int)(card.y + card.height - 48.f), 26, GOLD);
        }
    }

    const char* hint = _gamepad.isActive ? "D-Pad: Choose    A: Take relic"
                                         : "Arrows / Click: Choose    Enter / Click: Take relic";
    DrawText(hint, (int)(sw * 0.5f - MeasureText(hint, 24) * 0.5f), (int)(sh - 56.f), 24, Color{ 185, 185, 205, 220 });
}

void Engine::DrawHowToPlay()
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    const float dt = GetFrameTime();

    // -- Font sizes -----------------------------------------------------------
    const int titleSz  = (int)(sh * 0.062f);
    const int headerSz = (int)(sh * 0.034f);
    const int labelSz  = (int)(sh * 0.027f);
    const int descSz   = (int)(sh * 0.022f);
    const int tabSz    = (int)(sh * 0.026f);

    // -- Slide animation ------------------------------------------------------
    _htpSlideOffset *= (1.f - std::min(dt * 14.f, 1.f));

    // -- Background -----------------------------------------------------------
    DrawScrollingCheckerboard(sw, sh,
        Color{ 96, 34, 86, 255 },
        Color{ 132, 54, 116, 255 },
        22.f, 12.f);

    // -- Title bar ------------------------------------------------------------
    const float titleBarH = sh * 0.095f;
    DrawRectangle(0, 0, (int)sw, (int)titleBarH, Fade(Color{ 50, 14, 56, 255 }, 0.88f));
    const char* title = "HOW TO PLAY";
    int titleW = MeasureText(title, titleSz);
    for (int ox = -2; ox <= 2; ox += 2)
        for (int oy = -2; oy <= 2; oy += 2)
            if (ox || oy)
                DrawText(title, (int)(sw / 2.f - titleW / 2.f) + ox,
                    (int)(titleBarH / 2.f - titleSz / 2.f) + oy, titleSz, BLACK);
    DrawText(title, (int)(sw / 2.f - titleW / 2.f),
        (int)(titleBarH / 2.f - titleSz / 2.f), titleSz, Color{ 255, 194, 92, 255 });

    // -- Tab bar --------------------------------------------------------------
    const char* tabLabels[] = { "BASICS", "ELEMENTS", "THE WORLD", "TOUCH" };
    const int   tabCount    = 4;
    const float tabBarY     = titleBarH + sh * 0.008f;
    const float tabBarH     = sh * 0.063f;
    const float tabW        = sw * 0.175f;
    const float tabGap      = sw * 0.012f;
    const float tabsTotal   = tabCount * tabW + (tabCount - 1) * tabGap;
    const float tabStartX   = sw / 2.f - tabsTotal / 2.f;

    for (int i = 0; i < tabCount; i++)
    {
        float tx = tabStartX + i * (tabW + tabGap);
        Rectangle tabRect = { tx, tabBarY, tabW, tabBarH };
        bool isActive  = (_htpTab == i);
        bool tabHov    = CheckCollisionPointRec(GetVirtualMousePos(), tabRect);

        Color bgCol  = isActive  ? Color{ 185, 130, 30, 240 }
                     : tabHov    ? Color{ 100, 50,  90, 200 }
                     :             Color{ 58,  22,  56, 200 };
        Color edgeCol = isActive ? Color{ 255, 210, 80,  255 }
                      :            Fade(Color{ 200, 140, 220, 255 }, 0.45f);

        DrawRectangleRounded(tabRect, 0.22f, 6, bgCol);
        DrawRectangleRoundedLines(tabRect, 0.22f, 6, edgeCol);
        if (isActive)
            DrawRectangle((int)(tx + tabW * 0.1f), (int)(tabBarY + tabBarH - 4.f),
                          (int)(tabW * 0.8f), 4, Color{ 255, 210, 80, 255 });

        int lw = MeasureText(tabLabels[i], tabSz);
        Color textCol = isActive ? Color{ 20, 10, 5, 255 } : Color{ 220, 185, 240, 255 };
        DrawText(tabLabels[i],
            (int)(tx + tabW / 2.f - lw / 2.f),
            (int)(tabBarY + tabBarH / 2.f - tabSz / 2.f),
            tabSz, textCol);

        if (tabHov && !isActive && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        {
            _htpSlideOffset = (i > _htpTab ? 1.f : -1.f) * sw * 0.12f;
            _htpTab = i;
            StopSound(_buttonPressSound); PlaySound(_buttonPressSound);
        }
    }

    // -- Content panel --------------------------------------------------------
    const float panelX = sw * 0.04f;
    const float panelY = tabBarY + tabBarH + sh * 0.010f;
    const float panelW = sw * 0.92f;
    const float panelH = sh * 0.755f;
    Rectangle panelRect = { panelX, panelY, panelW, panelH };

    if (_htpBorderTex.id != 0)
        DrawNineSliceEx(_htpBorderTex, 62.8f, 15.3f, 19.5f, 18.1f, 16.f, panelRect, Fade(WHITE, 0.88f));
    else
        DrawRectangleRounded(panelRect, 0.04f, 8, Fade(Color{ 45, 12, 52, 255 }, 0.88f));

    // Content origin (shifted by slide offset)
    const float cx = panelX + sw * 0.03f + _htpSlideOffset;
    const float cy = panelY + sh * 0.025f;
    const float cw = panelW - sw * 0.06f;

    BeginScissorMode((int)panelX + 2, (int)panelY + 2, (int)panelW - 4, (int)panelH - 4);

    // ------------------------------------------------------------------------
    // TAB 0 - BASICS
    // ------------------------------------------------------------------------
    if (_htpTab == 0)
    {
        const float colW   = cw * 0.45f;
        const float midGap = cw * 0.10f;
        const float leftX  = cx;
        const float rightX = cx + colW + midGap;
        const float divX   = cx + colW + midGap / 2.f;

        const float basicsHeaderY = cy + sh * 0.022f;
        InputPromptMode promptMode = GetPromptModeForUi();
        std::string abilitySlots = PromptAbilitySlots(promptMode);
        std::string meleeLine = std::string(PromptAttack(promptMode)) + " to swing your sword.";
        std::string abilityLine = PromptAbilityAction(promptMode);

        DrawText((promptMode == InputPromptMode::Gamepad) ? "CONTROLLER & MOVEMENT" : (promptMode == InputPromptMode::Touch) ? "TOUCH & MOVEMENT" : "KEYBOARD & MOVEMENT", (int)leftX,  (int)basicsHeaderY, headerSz, Color{ 255, 194, 92, 255 });
        DrawText("ACTIONS & ABILITIES",  (int)rightX, (int)basicsHeaderY, headerSz, Color{ 255, 194, 92, 255 });
        DrawLineEx({ divX, basicsHeaderY + headerSz + 4.f }, { divX, panelY + panelH - sh * 0.03f },
            1.5f, Fade(Color{ 220, 160, 240, 255 }, 0.35f));

        // Keyboard controls
        struct KBEntry { const char* key; const char* desc; };
        KBEntry kb[] = {
            { PromptMove(promptMode),  "Move"                       },
            { PromptDash(promptMode),  "Dash  (brief invincibility)" },
            { PromptAttack(promptMode), "Melee attack"                },
            { abilitySlots.c_str(), "Use ability in that slot"    },
            { (promptMode == InputPromptMode::Gamepad) ? "LB/RB" : (promptMode == InputPromptMode::Touch) ? "Tap Slots" : "Scroll Wheel", "Cycle active ability"        },
            { PromptPause(promptMode), "Pause / unpause"             },
        };
        float rowY = basicsHeaderY + headerSz + sh * 0.022f;
        for (auto& k : kb)
        {
            int kw = MeasureText(k.key, labelSz);
            float badgeH = (float)labelSz + 10.f;
            DrawRectangleRounded({ leftX, rowY - 4.f, (float)kw + 18.f, badgeH },
                0.28f, 4, Fade(Color{ 70, 18, 66, 255 }, 0.80f));
            DrawRectangleRoundedLines({ leftX, rowY - 4.f, (float)kw + 18.f, badgeH },
                0.28f, 4, Fade(Color{ 255, 182, 236, 255 }, 0.55f));
            DrawText(k.key,  (int)leftX + 9, (int)rowY, labelSz, Color{ 255, 194, 92, 255 });
            DrawText(k.desc, (int)(leftX + kw + 30.f), (int)rowY, descSz, Color{ 20, 15, 25, 255 });
            rowY += sh * 0.072f;
        }

        // EXP & Gold
        rowY += sh * 0.010f;
        DrawText("EXP & GOLD", (int)leftX, (int)rowY, headerSz, Color{ 255, 194, 92, 255 });
        rowY += headerSz + sh * 0.012f;
        const char* expLines[] = {
            "Kill enemies to earn EXP and level up.",
            "Choose 1 of 3 upgrade cards each level.",
            "Enemies drop Gold coins when defeated.",
            "Spend Gold at Zeph's shop between biomes.",
        };
        for (auto& line : expLines)
        {
            DrawText(line, (int)leftX, (int)rowY, descSz, Color{ 20, 15, 25, 255 });
            rowY += descSz + sh * 0.008f;
        }

        // Actions column
        struct ActionEntry { const char* name; const char* line1; const char* line2; };
        ActionEntry acts[] = {
            { "DASH",
              "Quick burst of movement in any direction.",
              "You are invincible for its full duration." },
            { "MELEE",
              meleeLine.c_str(),
              "Hits all enemies in a forward arc." },
            { "ABILITIES",
              abilityLine.c_str(),
              "Each ability costs Mana to activate." },
            { "MANA",
              "Blue gems drop from defeated enemies.",
              "Pick them up to fuel your abilities." },
        };
        float rowR = basicsHeaderY + headerSz + sh * 0.022f;
        for (auto& a : acts)
        {
            DrawText(a.name, (int)rightX, (int)rowR, labelSz, Color{ 255, 194, 92, 255 });
            rowR += labelSz + sh * 0.005f;
            DrawText(a.line1, (int)rightX, (int)rowR, descSz, Color{ 20, 15, 25, 255 });
            rowR += descSz + sh * 0.003f;
            DrawText(a.line2, (int)rightX, (int)rowR, descSz, Color{ 20, 15, 25, 255 });
            rowR += descSz + sh * 0.030f;
        }
    }

    // ------------------------------------------------------------------------
    // TAB 1 - ELEMENTS
    // ------------------------------------------------------------------------
    else if (_htpTab == 1)
    {
        const char* elemTitle = "THE MAGIC SYSTEM";
        DrawText(elemTitle,
            (int)(cx + cw / 2.f - MeasureText(elemTitle, headerSz) / 2.f),
            (int)cy, headerSz, Color{ 255, 194, 92, 255 });

        struct ElemEntry {
            const char* name;
            const char* effect;
            const char* desc1;
            const char* desc2;
            Color       nameCol;
            Texture2D*  icon;
        };
        ElemEntry elems[] = {
            { "FIRE",
              "BURN  -  Damage over time",
              "Enemies ignite and take periodic damage",
              "for several seconds after being hit.",
              Color{ 255, 120, 40, 255 },
              &_abilityIconFireTex },
            { "ICE",
              "FREEZE  -  Stuns the enemy",
              "Frozen enemies cannot move or attack.",
              "Break the freeze with a melee hit for bonus damage.",
              Color{ 100, 210, 255, 255 },
              &_abilityIconIceTex },
            { "ELECTRIC",
              "SHOCK  -  Amplifies melee damage",
              "Shocked enemies take greatly increased damage",
              "from your next melee strike.",
              Color{ 220, 220, 50, 255 },
              &_abilityIconElectricTex },
        };

        const float iconSz     = sh * 0.095f;
        const float rowSpacing = sh * 0.220f;
        float elemY = cy + headerSz + sh * 0.040f;

        for (auto& el : elems)
        {
            Rectangle iconRect = { cx, elemY, iconSz, iconSz };
            DrawRectangleRounded(iconRect, 0.22f, 6, Fade(el.nameCol, 0.18f));
            DrawRectangleRoundedLines(iconRect, 0.22f, 6, Fade(el.nameCol, 0.65f));
            if (el.icon->id != 0)
                DrawTexturePro(*el.icon,
                    { 0.f, 0.f, (float)el.icon->width, (float)el.icon->height },
                    { cx + iconSz * 0.1f, elemY + iconSz * 0.1f, iconSz * 0.8f, iconSz * 0.8f },
                    {}, 0.f, WHITE);
            else
                DrawCircleV({ cx + iconSz / 2.f, elemY + iconSz / 2.f },
                    iconSz * 0.35f, Fade(el.nameCol, 0.75f));

            float textX = cx + iconSz + sw * 0.025f;
            float textY = elemY;
            DrawText(el.name, (int)textX, (int)textY, headerSz, el.nameCol);
            textY += headerSz + sh * 0.006f;

            int effW = MeasureText(el.effect, labelSz);
            DrawRectangleRounded({ textX, textY - 2.f, (float)effW + 16.f, (float)labelSz + 10.f },
                0.3f, 4, Fade(el.nameCol, 0.22f));
            DrawRectangleRoundedLines({ textX, textY - 2.f, (float)effW + 16.f, (float)labelSz + 10.f },
                0.3f, 4, Fade(el.nameCol, 0.55f));
            DrawText(el.effect, (int)textX + 8, (int)textY, labelSz, el.nameCol);
            textY += labelSz + sh * 0.016f;
            DrawText(el.desc1, (int)textX, (int)textY, descSz, Color{ 210, 178, 220, 255 });
            textY += descSz + sh * 0.005f;
            DrawText(el.desc2, (int)textX, (int)textY, descSz, Color{ 170, 140, 185, 255 });

            elemY += rowSpacing;
        }
    }

    // ------------------------------------------------------------------------
    // TAB 2 - THE WORLD
    // ------------------------------------------------------------------------
    else if (_htpTab == 2)
    {
        const float colW   = cw * 0.45f;
        const float midGap = cw * 0.10f;
        const float leftX  = cx;
        const float rightX = cx + colW + midGap;
        const float divX   = cx + colW + midGap / 2.f;

        const float worldHeaderY = cy + sh * 0.022f;
        DrawText("MAP LEGEND",  (int)leftX,  (int)worldHeaderY, headerSz, Color{ 255, 194, 92, 255 });
        DrawText("ZEPH'S SHOP", (int)rightX, (int)worldHeaderY, headerSz, Color{ 255, 194, 92, 255 });
        DrawLineEx({ divX, worldHeaderY + headerSz + 4.f }, { divX, panelY + panelH - sh * 0.03f },
            1.5f, Fade(Color{ 220, 160, 240, 255 }, 0.35f));

        struct MapEntry { Texture2D* tex; Color fallback; const char* name; const char* desc; };
        MapEntry mapIcons[] = {
            { &_mapIconNormal,   GRAY,   "NORMAL ROOM", "Clear all enemies to proceed."         },
            { &_mapIconElite,    ORANGE, "ELITE ROOM",  "Tougher enemies, better drops."        },
            { &_mapIconShop,     GOLD,   "SHOP",        "Buy upgrades and potions from Zeph."   },
            { &_mapIconTreasure, YELLOW, "TREASURE",    "Find a powerful item for free."        },
            { &_mapIconBoss,     RED,    "BOSS",        "Defeat the boss to complete the biome."},
            { &_mapIconRest,     GREEN,  "REST SITE",   "Recover HP between battles."           },
        };
        const float mapIconSz = sh * 0.052f;
        float mapY = worldHeaderY + headerSz + sh * 0.022f;
        for (auto& mi : mapIcons)
        {
            if (mi.tex->id != 0)
                DrawTexturePro(*mi.tex,
                    { 0.f, 0.f, (float)mi.tex->width, (float)mi.tex->height },
                    { leftX, mapY, mapIconSz, mapIconSz }, {}, 0.f, WHITE);
            else
            {
                DrawRectangleRounded({ leftX, mapY, mapIconSz, mapIconSz }, 0.28f, 4, Fade(mi.fallback, 0.65f));
                DrawRectangleRoundedLines({ leftX, mapY, mapIconSz, mapIconSz }, 0.28f, 4, BLACK);
            }
            float tx = leftX + mapIconSz + sw * 0.015f;
            DrawText(mi.name, (int)tx, (int)mapY,                   labelSz, Color{ 255, 194, 92, 255 });
            DrawText(mi.desc, (int)tx, (int)(mapY + labelSz + 3.f), descSz,  Color{ 20, 15, 25, 255 });
            mapY += sh * 0.095f;
        }

        // Shop info
        struct ShopLine { const char* text; bool isHeader; };
        ShopLine shopLines[] = {
            { "Between biomes you visit Zeph's Shop.",         false },
            { "",                                              false },
            { "UPGRADES",                                      true  },
            { "Buy powerful passive boosts for your run.",     false },
            { "Epic-tier items offer rare, powerful effects.", false },
            { "",                                              false },
            { "REROLL",                                        true  },
            { "Pay gold to refresh the item selection.",       false },
            { "Cost increases with each reroll.",              false },
            { "",                                              false },
            { "POTIONS",                                       true  },
            { "Health Potion  -  restore HP instantly.",       false },
            { "Mana Potion  -  restore Mana instantly.",       false },
            { "",                                              false },
            { "DAILY DEAL",                                    true  },
            { "One item each visit is 25% off.",               false },
            { "Look for the gold price tag.",                  false },
        };
        float shopY = worldHeaderY + headerSz + sh * 0.022f;
        for (auto& sl : shopLines)
        {
            if (sl.text[0] == '\0') { shopY += descSz * 0.5f; continue; }
            Color col = sl.isHeader ? Color{ 255, 194, 92, 255 } : Color{ 20, 15, 25, 255 };
            int   sz  = sl.isHeader ? labelSz : descSz;
            DrawText(sl.text, (int)rightX, (int)shopY, sz, col);
            shopY += sz + sh * 0.007f;
        }
    }

    // ------------------------------------------------------------------------
    // TAB 3 - TOUCH CONTROLS
    // ------------------------------------------------------------------------
    else if (_htpTab == 3)
    {
        const char* touchTitle = "TOUCH CONTROLS";
        DrawText(touchTitle,
            (int)(cx + cw / 2.f - MeasureText(touchTitle, headerSz) / 2.f),
            (int)cy, headerSz, Color{ 255, 194, 92, 255 });

        // Screen split diagram
        const float diagW = cw * 0.78f;
        const float diagH = sh * 0.285f;
        const float diagX = cx + cw / 2.f - diagW / 2.f;
        const float diagY = cy + headerSz + sh * 0.025f;
        const float halfW = diagW / 2.f;

        DrawRectangleRounded({ diagX, diagY, diagW, diagH }, 0.06f, 8, Fade(Color{ 30, 10, 36, 255 }, 0.90f));
        DrawRectangleRoundedLines({ diagX, diagY, diagW, diagH }, 0.06f, 8, Fade(Color{ 220, 160, 240, 255 }, 0.55f));

        // Left half - movement
        DrawRectangleRounded({ diagX, diagY, halfW, diagH }, 0.06f, 8, Fade(Color{ 40, 80, 120, 255 }, 0.35f));
        float jsX = diagX + halfW / 2.f;
        float jsY = diagY + diagH / 2.f;
        DrawCircleV({ jsX, jsY }, diagH * 0.28f, Fade(Color{ 80, 130, 200, 255 }, 0.30f));
        DrawCircleLinesV({ jsX, jsY }, diagH * 0.28f, Fade(Color{ 120, 180, 255, 255 }, 0.65f));
        DrawCircleV({ jsX, jsY }, diagH * 0.11f, Fade(Color{ 160, 210, 255, 255 }, 0.80f));
        DrawText("MOVE",
            (int)(jsX - MeasureText("MOVE", descSz) / 2.f),
            (int)(diagY + diagH - descSz - sh * 0.015f),
            descSz, Color{ 150, 200, 255, 255 });

        DrawLineEx({ diagX + halfW, diagY + sh * 0.015f },
            { diagX + halfW, diagY + diagH - sh * 0.015f }, 1.5f, Fade(WHITE, 0.30f));

        // Right half - buttons
        DrawRectangleRounded({ diagX + halfW, diagY, halfW, diagH }, 0.06f, 8, Fade(Color{ 100, 40, 80, 255 }, 0.35f));
        float btnR  = diagH * 0.18f;
        float b1X   = diagX + halfW + halfW * 0.32f;
        float b2X   = diagX + halfW + halfW * 0.70f;
        float btY   = diagY + diagH * 0.42f;

        DrawCircleV({ b1X, btY }, btnR, Fade(Color{ 200, 80, 80, 255 }, 0.55f));
        DrawCircleLinesV({ b1X, btY }, btnR, Fade(WHITE, 0.50f));
        DrawText("ATK",
            (int)(b1X - MeasureText("ATK", descSz) / 2.f),
            (int)(btY - descSz / 2.f), descSz, WHITE);

        DrawCircleV({ b2X, btY }, btnR, Fade(Color{ 80, 120, 220, 255 }, 0.55f));
        DrawCircleLinesV({ b2X, btY }, btnR, Fade(WHITE, 0.50f));
        DrawText("DASH",
            (int)(b2X - MeasureText("DASH", descSz) / 2.f),
            (int)(btY - descSz / 2.f), descSz, WHITE);

        DrawText("COMBAT",
            (int)(diagX + halfW + halfW / 2.f - MeasureText("COMBAT", descSz) / 2.f),
            (int)(diagY + diagH - descSz - sh * 0.015f),
            descSz, Color{ 255, 150, 200, 255 });

        // Tips
        struct TipEntry { const char* label; const char* desc; };
        TipEntry tips[] = {
            { "JOYSTICK:",  "Drag anywhere on the left half to move your character." },
            { "ATTACK:",    "Tap the ATK button to swing your sword."                },
            { "DASH:",      "Tap DASH for a quick invincible burst of speed."        },
            { "ABILITIES:", "Tap any ability icon at the bottom of the screen."      },
            { "PAUSE:",     "Tap the pause icon in the top-right corner."            },
        };
        const float tipsX   = cx + cw / 2.f - cw * 0.35f;
        float       tipY    = diagY + diagH + sh * 0.030f;
        for (auto& tip : tips)
        {
            int lw = MeasureText(tip.label, labelSz);
            DrawText(tip.label, (int)tipsX,                         (int)tipY, labelSz, Color{ 255, 194, 92, 255 });
            DrawText(tip.desc,  (int)(tipsX + lw + sw * 0.012f),   (int)tipY, descSz,  Color{ 20, 15, 25, 255 });
            tipY += sh * 0.068f;
        }
    }

    EndScissorMode();

    // -- Back button ----------------------------------------------------------
    const float btnW = sw * 0.14f;
    const float btnH = sh * 0.055f;
    const float btnX = sw / 2.f - btnW / 2.f;
    const float btnY = sh - btnH - sh * 0.016f;
    Rectangle backBtn{ btnX, btnY, btnW, btnH };
    bool hovered = CheckCollisionPointRec(GetVirtualMousePos(), backBtn);

    DrawRectangleRounded(backBtn, 0.3f, 6, hovered ? Color{ 196, 86, 165, 240 } : Color{ 142, 58, 132, 228 });
    DrawRectangleRoundedLines(backBtn, 0.3f, 6, Fade(Color{ 255, 194, 92, 255 }, 0.68f));

    const char* backLabel   = (_howToPlayFrom == GameState::Pause) ? "Resume Game" : "< Back";
    int         backLabelSz = (int)(sh * 0.030f);
    int         backW       = MeasureText(backLabel, backLabelSz);
    DrawText(backLabel,
        (int)(btnX + btnW / 2.f - backW / 2.f),
        (int)(btnY + btnH / 2.f - backLabelSz / 2.f),
        backLabelSz, Color{ 255, 243, 214, 255 });

    if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        StopSound(_buttonPressSound); PlaySound(_buttonPressSound);
        if (_howToPlayFrom == GameState::Menu)
            _menu.Init();
        _runState.ReturnFromHowToPlay();
    }
}


Vector2 Engine::GetRandomPropPosition()
{
    float mapW = _map.width * _mapScale;
    float mapH = _map.height * _mapScale;
    Vector2 playerStart{ mapW * 0.5f, mapH * 0.5f };

    // One full player unit of clearance from every map edge.
    // Player sprite is 32 px wide at draw scale 6 ? 192 world-space pixels.
    // The bottom margin is larger because prop positions are top-left corners,
    // so tall props (trees) would otherwise hang off the bottom edge.
    const float margin       = 32.f * 6.f;         // 192 px - sides and top
    const float bottomMargin = margin * 1.5f;       // 288 px - bottom
    float minX = margin;
    float maxX = mapW - margin;
    float minY = margin;
    float maxY = mapH - bottomMargin;

    float minSpacing = 308.f;
    float playerSafeRadius = 320.f;
    int attempts = 0;
    const int maxAttempts = 50;

    while (attempts < maxAttempts)
    {
        Vector2 pos{};
        pos.x = (float)GetRandomValue((int)minX, (int)maxX);
        pos.y = (float)GetRandomValue((int)minY, (int)maxY);

        bool tooClose = false;

        if (Vector2Distance(pos, playerStart) < playerSafeRadius)
            tooClose = true;

        for (auto& prop : _props)
        {
            if (Vector2Distance(pos, prop.GetWorldPos()) < minSpacing)
            {
                tooClose = true;
                break;
            }
        }

        if (!tooClose)
            return pos;

        attempts++;
    }

    Vector2 fallback{};
    fallback.x = (float)GetRandomValue((int)minX, (int)maxX);
    fallback.y = (float)GetRandomValue((int)minY, (int)maxY);
    return fallback;
}

const char* Engine::GetBiomeName(Biome biome) const
{
    switch (biome)
    {
    case Biome::AncientCastle:  return "Ancient Castle";
    case Biome::Caverns:        return "Caverns";
    case Biome::DemonsInsides:  return "Demons Insides";
    case Biome::DreamRealm:     return "Dream Realm";
    case Biome::Forest:         return "Forest";
    case Biome::Graveyard:      return "Graveyard";
    case Biome::Jungle:         return "Jungle";
    case Biome::LostCity:       return "Lost City";
    case Biome::TheSanctuary:   return "The Sanctuary";
    case Biome::Wastelands:     return "Wastelands";
    default:                    return "???";
    }
}

void Engine::LoadDungeonVisualVariants(Biome biome)
{
    _dungeonVisualVariants.clear();
    const std::string biomeName = GetBiomeName(biome);
    const std::string configName = "biomevariants_" + biomeName + ".txt";
    std::ifstream input(AssetPath(configName.c_str()));
    std::string tag;
    while (input >> tag)
    {
        if (!tag.empty() && tag[0] == '#')
        {
            std::string ignored;
            std::getline(input, ignored);
            continue;
        }
        if (tag != "variant")
        {
            std::string ignored;
            std::getline(input, ignored);
            continue;
        }

        DungeonVisualVariant variant;
        input >> std::quoted(variant.name)
              >> std::quoted(variant.sheetStem)
              >> std::quoted(variant.mapperStem)
              >> variant.weight >> variant.minWingRooms >> variant.maxWingRooms;
        if (!input.fail() && !variant.name.empty() && !variant.sheetStem.empty())
        {
            variant.weight = std::max(1, variant.weight);
            variant.minWingRooms = std::max(1, variant.minWingRooms);
            variant.maxWingRooms = std::max(variant.minWingRooms, variant.maxWingRooms);
            _dungeonVisualVariants.push_back(std::move(variant));
        }
    }

    // Existing biomes need no migration. Without a config, their old mapper and
    // sheet become the one default variant and render exactly as before.
    if (_dungeonVisualVariants.empty())
        _dungeonVisualVariants.push_back({ biomeName, biomeName, biomeName, 1, 12, 12 });
}

void Engine::AssignDungeonVisualVariantWings()
{
    const auto& rooms = _dungeonGen.GetRooms();
    _dungeonRoomVisualVariants.assign(rooms.size(), -1);
    if (rooms.empty() || _dungeonVisualVariants.empty())
        return;

    auto chooseVariant = [&](int avoid) {
        (void)avoid;
        int total = 0;
        for (int i = 0; i < (int)_dungeonVisualVariants.size(); ++i)
            total += _dungeonVisualVariants[i].weight;
        int roll = GetRandomValue(1, std::max(1, total));
        for (int i = 0; i < (int)_dungeonVisualVariants.size(); ++i)
        {
            roll -= _dungeonVisualVariants[i].weight;
            if (roll <= 0) return i;
        }
        return 0;
    };

    int assigned = 0;
    bool firstWing = true;
    while (assigned < (int)rooms.size())
    {
        int seed = -1;
        if (firstWing)
            seed = _dungeonGen.GetStartIndex();
        if (seed < 0 || seed >= (int)rooms.size() || _dungeonRoomVisualVariants[seed] >= 0)
            for (int i = 0; i < (int)rooms.size(); ++i)
                if (_dungeonRoomVisualVariants[i] < 0) { seed = i; break; }
        if (seed < 0) break;

        int neighbourVariant = -1;
        const int dirs[4][2] = { {-1,0}, {1,0}, {0,-1}, {0,1} };
        for (const auto& dir : dirs)
        {
            int neighbour = _dungeonGen.GetNeighborIndex(seed, dir[0], dir[1]);
            if (neighbour >= 0 && _dungeonRoomVisualVariants[neighbour] >= 0)
            {
                neighbourVariant = _dungeonRoomVisualVariants[neighbour];
                break;
            }
        }

        int variantIdx = firstWing ? 0 : chooseVariant(neighbourVariant);
        const DungeonVisualVariant& variant = _dungeonVisualVariants[variantIdx];
        int wingSize = GetRandomValue(variant.minWingRooms, variant.maxWingRooms);
        std::queue<int> frontier;
        frontier.push(seed);
        int wingAssigned = 0;
        while (!frontier.empty() && wingAssigned < wingSize)
        {
            int roomIdx = frontier.front();
            frontier.pop();
            if (roomIdx < 0 || roomIdx >= (int)rooms.size() || _dungeonRoomVisualVariants[roomIdx] >= 0)
                continue;
            _dungeonRoomVisualVariants[roomIdx] = variantIdx;
            ++wingAssigned;
            ++assigned;
            for (const auto& dir : dirs)
            {
                int neighbour = _dungeonGen.GetNeighborIndex(roomIdx, dir[0], dir[1]);
                if (neighbour >= 0 && _dungeonRoomVisualVariants[neighbour] < 0)
                    frontier.push(neighbour);
            }
        }
        firstWing = false;
    }
}

int Engine::GetDungeonVisualVariantForRoom(int roomIdx) const
{
    if (roomIdx >= 0 && roomIdx < (int)_dungeonRoomVisualVariants.size())
        return std::clamp(_dungeonRoomVisualVariants[roomIdx], 0,
                          std::max(0, (int)_dungeonVisualVariants.size() - 1));
    return 0;
}

void Engine::LoadDungeonVisualVariantAssets(int variantIdx, TileDefSet& defs, TileRenderer& renderer)
{
    if (_dungeonVisualVariants.empty()) return;
    variantIdx = std::clamp(variantIdx, 0, (int)_dungeonVisualVariants.size() - 1);
    const DungeonVisualVariant& variant = _dungeonVisualVariants[variantIdx];
    std::string txtFile = "tilemapper_" + variant.mapperStem + ".txt";
    std::string sheetPath = AssetPath((std::string(kTilesheetFolder) + "/" + variant.sheetStem + ".png").c_str());
    std::string groundPath = AssetPath((std::string(kTilesheetFolder) + "/Ground TIles.png").c_str());
    std::string sharedRewardPath = AssetPath((std::string(kTilesheetFolder) + "/Caverns.png").c_str());
    defs = {};
    defs.LoadFromFile(AssetPath(txtFile.c_str()).c_str());
    renderer.Init(sheetPath.c_str(), groundPath.c_str(), sharedRewardPath.c_str(), defs);
    renderer.LoadRoomAssetCatalog(_roomAssetCatalog);
}

// Loads visual-variant data and initializes the first connected room wing.
// Gameplay still uses the original Biome enum; these variants are art only.
void Engine::LoadTilesetForBiome(Biome biome)
{
    LoadDungeonVisualVariants(biome);
    AssignDungeonVisualVariantWings();
    int roomIdx = _dungeonRoomIdx >= 0 ? _dungeonRoomIdx : _dungeonGen.GetStartIndex();
    _currentDungeonVisualVariant = GetDungeonVisualVariantForRoom(roomIdx);
    LoadDungeonVisualVariantAssets(_currentDungeonVisualVariant, _tileDefs, _tileRenderer);
}

void Engine::PopulatePropsForBiome(Biome biome)
{
    _props.clear();

    // Boss and Store rooms are open arenas - no props
    if (_currentRoomType == RoomType::Boss || _currentRoomType == RoomType::Store)
        return;

    if (biome == Biome::Forest)
    {
        // Forest props are all solid blockers, so a mixed random rotation is
        // enough to make each pass through the biome read differently without
        // changing the existing prop/pathfinding systems.
        int propCount = GetRandomValue(4, 6);
        for (int i = 0; i < propCount; ++i)
        {
            Vector2 pos = GetRandomPropPosition();
            int choice = GetRandomValue(0, 3);
            if (choice == 0)
            {
                _props.push_back(Prop{ pos, _treeTex, 1, 0, 0, 4.8f });
                _props.back().SetCollisionTopFraction(0.25f);
                _props.back().SetCollisionSideFraction(0.30f);
            }
            else if (choice == 1)
            {
                _props.push_back(Prop{ pos, _smallTreeTex, 1, 0, 0, 4.8f });
                _props.back().SetCollisionTopFraction(0.25f);
                _props.back().SetCollisionSideFraction(0.30f);
            }
            else if (choice == 2)
            {
                _props.push_back(Prop{ pos, _rockTex, 1, 0, 0, 4.f });
            }
            else
            {
                _props.push_back(Prop{ pos, _bigRockTex, 1, 0, 0, 3.2f });
            }
        }
        return;
    }

    // Dungeon keeps the original authored mix of pillars and torches.
    int propCount = GetRandomValue(7, 10);
    int pillarCount = propCount - 3;

    for (int i = 0; i < pillarCount; ++i)
    {
        Vector2 pos = GetRandomPropPosition();
        _props.push_back(Prop{ pos, _pillarTex });
    }
    for (int i = 0; i < 2; ++i)
    {
        Vector2 pos = GetRandomPropPosition();
        _props.push_back(Prop{ pos, _torchTex, 8, 32, 29, 2.5f });
    }

    Vector2 pos = GetRandomPropPosition();
    _props.push_back(Prop{ pos, _pillarTorchTex, 8, 32, 52, 3.f, 1.f / 10.f, 17 });
}

void Engine::ApplyBiome(Biome biome)
{
    // The active map swaps between dungeon and forest. ForestMap is authored at
    // half the pixel dimensions of the dungeon map, so it uses 2x the scale to
    // preserve the same playable world size and camera feel.
    _nav.CancelAndReset();

    if (_map.id != 0)
        UnloadTexture(_map);

    // Forest-family biomes use the forest map; all others use the dungeon map.
    // New biome maps can be wired in here when the art is ready.
    bool useForestMap = (biome == Biome::Forest || biome == Biome::Jungle);
    if (useForestMap)
        _map = LoadTexture(AssetPath("ForestLevel/ForestMap.png").c_str());
    else
        _map = LoadTexture(AssetPath("TileSet/Map.png").c_str());

    // Compute the map scale for the current screen size.
    // WorldConfig derives the correct scale from mode + texture dimensions,
    // so the world always fits properly on 720p, 1080p, 4K, and phones.
    _worldConfig.Recalculate(_map, kVirtualWidth, kVirtualHeight);
    _mapScale = _worldConfig.GetScale();

    _currentBiome = biome;
    PopulatePropsForBiome(biome);
    {
        std::vector<Rectangle> propRects;
        propRects.reserve(_props.size() + 1);
        for (auto& prop : _props)
            propRects.push_back(prop.GetCollisionRec());
        // Block the bottom boundary zone so A* never routes waypoints into it.
        {
            const float navMapW = _map.width  * _mapScale;
            const float navMapH = _map.height * _mapScale;
            const float navBot  = (_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle) ? 160.f : 220.f;
            propRects.push_back({ 0.f, navMapH - navBot, navMapW, navBot });
        }
        _nav.Rebuild(_map.width * _mapScale, _map.height * _mapScale, propRects);
    }

    // Biome swaps happen between waves, so recentering the player gives them
    // a clean neutral start in the new arena before enemies spawn in.
    _player.SetWorldPos(Vector2{ _map.width * _mapScale * 0.5f, _map.height * _mapScale * 0.5f });
}

void Engine::UpdateBiomeTransition(float dt)
{
    if (!_biomeTransitionActive)
        return;

    float totalDuration = kBiomeFadeOutDuration + kBiomeFadeInDuration;
    float previousElapsed = totalDuration - _biomeTransitionTimer;
    _biomeTransitionTimer = std::max(0.f, _biomeTransitionTimer - dt);
    float currentElapsed = totalDuration - _biomeTransitionTimer;

    if (!_biomeTransitionSwapped &&
        previousElapsed < kBiomeFadeOutDuration &&
        currentElapsed >= kBiomeFadeOutDuration)
    {
        ApplyBiome(_pendingBiome);
        _nav.RefreshSync(_player.GetFeetWorldPos());
        _biomeTransitionSwapped = true;
    }

    if (_biomeTransitionTimer <= 0.f)
    {
        _biomeTransitionActive = false;
        _biomeTransitionSwapped = false;
    }
}

float Engine::GetBiomeTransitionAlpha() const
{
    if (!_biomeTransitionActive)
        return 0.f;

    float totalDuration = kBiomeFadeOutDuration + kBiomeFadeInDuration;
    float elapsed = totalDuration - _biomeTransitionTimer;
    if (elapsed < kBiomeFadeOutDuration)
        return elapsed / kBiomeFadeOutDuration;

    float fadeInElapsed = elapsed - kBiomeFadeOutDuration;
    return 1.f - std::min(1.f, fadeInElapsed / kBiomeFadeInDuration);
}

void Engine::RespawnOutOfBoundsEnemies()
{
    float mapW = _map.width  * _mapScale;
    float mapH = _map.height * _mapScale;
    const float hardMargin = 60.f;   // enemy is truly outside if beyond this

    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive())
            continue;
        if (enemy->IsDying() || !enemy->IsAlive())
            continue;

        Vector2 pos = enemy->GetWorldPos();
        bool outOfBounds = pos.x < hardMargin || pos.y < hardMargin ||
                           pos.x > mapW - hardMargin || pos.y > mapH - hardMargin;

        if (!outOfBounds)
            continue;

        // Find a valid position away from props, other enemies, and the player
        Vector2 newPos{};
        bool found = false;

        for (int attempt = 0; attempt < 50; ++attempt)
        {
            newPos.x = (float)GetRandomValue(300, (int)(mapW - 300));
            newPos.y = (float)GetRandomValue(300, (int)(mapH - 300));

            if (!IsSpawnPositionValid(newPos))
                continue;

            // Also keep a safe distance from the player
            if (Vector2Distance(newPos, _player.GetWorldPos()) < 300.f)
                continue;

            found = true;
            break;
        }

        if (found)
            enemy->Teleport(newPos);
    }
}

bool Engine::IsSpawnPositionValid(Vector2 pos)
{
    const float safeDistance = 200.f;
    const float playerSafeDistance = 260.f;

    // Reject if the spawn cell or any neighbour is blocked by a prop on the nav grid
    int col = (int)(pos.x / _nav.GetCellSize());
    int row = (int)(pos.y / _nav.GetCellSize());
    for (int dc = -1; dc <= 1; dc++)
        for (int dr = -1; dr <= 1; dr++)
            if (_nav.IsCellBlocked(col + dc, row + dr))
                return false;

    // Check against each prop's actual collision rect (inflated by safeDistance)
    // rather than distance from the top-left corner, so tall props like trees
    // correctly reject positions near their lower half.
    for (auto& prop : _props)
    {
        Rectangle rec = prop.GetCollisionRec();
        Rectangle inflated = {
            rec.x - safeDistance,
            rec.y - safeDistance,
            rec.width  + safeDistance * 2.f,
            rec.height + safeDistance * 2.f
        };
        if (CheckCollisionPointRec(pos, inflated))
            return false;
    }

    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive())
            continue;
        if (Vector2Distance(pos, enemy->GetWorldPos()) < safeDistance)
            return false;
    }

    // Keep enemy, boss, and timed pickup spawns away from the player so
    // nothing materializes directly on top of them.
    if (Vector2Distance(pos, _player.GetWorldPos()) < playerSafeDistance)
        return false;

    // Reject tiles that are completely cut off from the player by walls.
    // HasReachablePath checks the flow-field cost - max_int means the BFS
    // wave never reached this cell, so no path exists.
    if (!_nav.HasReachablePath(pos))
        return false;

    return true;
}

void Engine::ResetRunState()
{
    // Class and appearance are player choices, not run-scoped progression.
    // Preserve both before Character::Init reloads all sprite sheets.
    const PlayerClass selectedClass = _player.GetClass();
    const std::string selectedAppearance = _player.GetAppearance();

    _player.SetDashAllowedWhileCombatLocked(false);
    _nav.CancelAndReset();
    ResetMusicState();
    _wave          = 0;
    _enemiesKilled = 0;
    _bossesDefeated = 0;
    _gameTimer = 0.f;
    _playerDying = false;
    _awaitingStartingAbility = false;
    _starterAbilityGiftClaimed = false;
    _startingAbilityPickCount = 0;
    for (int i = 0; i < 6; i++)
        _startingAbilitySelected[i] = false;
    _waveStarting        = true;
    _wave1LevelUpDone    = false;
    _bossWarningTimer    = 0.f;
    _biomeTransitionActive = false;
    _biomeTransitionSwapped = false;
    _biomeTransitionTimer = 0.f;
    _ultimatePhase       = UltimatePhase::None;
    _ultimatePhaseTimer  = 0.f;
    _ultimateCircleAngle = 0.f;
    _warriorVfx.clear();
    _mageSpellFields.clear();
    CancelAbilityAim();
    _lifestealAccum      = 0.f;
    _showUltimateRow     = false;
    _ultimateRowPicked   = false;
    _regularRowPicked    = false;
    _lastAbilityChoiceWave    = -1;
    _abilityChoiceSwapPending = false;
    _abilityChoiceOptionCount = 0;
    _levelUpOfferContext      = LevelUpOfferContext::NormalLevel;
    _eliteRewardGranted       = false;
    _debug.Deactivate();

    // -- Room / act progression reset --------------------------------------
    _currentAct        = 1;
    _currentRoom       = 0;
    _currentRoomType   = RoomType::Standard;
    _pendingRoomChoice  = false;
    _roomClearPending   = false;
    _roomClearTimer     = 0.f;
    _pendingExp             = 0.f;
    _expTallyAccum          = 0.f;
    _expTallyDone           = false;
    _tallyStartLevel        = 1;
    _tallyLevelUpsRemaining = 0;
    _tallyChoiceChaining    = false;
    _currentMapNodeIdx  = -1;
    _mapKeySelectedIdx  = -1;
    _mapOpenTimer       = 0.f;
    _actMap.clear();

    _isMainGameRun = false;

    // World map run state
    _worldZone = 0;
    _worldCompletedBiomes.clear();
    _worldChosenNodeIndices.clear();
    _worldMap.Reset();
    _runSessionData.Reset();
    _worldMapPreparedZone = -1;

    // Generate a random sequence of kTotalActs biomes, no two consecutive duplicates.
    {
        static constexpr Biome kAllBiomes[] = {
            Biome::Caverns, Biome::Forest
        };
        static constexpr int kBiomeCount = (int)(sizeof(kAllBiomes) / sizeof(kAllBiomes[0]));
        _biomeSequence.clear();
        Biome last = (Biome)-1;
        for (int i = 0; i < kTotalActs; i++)
        {
            Biome pick;
            do { pick = kAllBiomes[GetRandomValue(0, kBiomeCount - 1)]; }
            while (pick == last);
            _biomeSequence.push_back(pick);
            last = pick;
        }
        _startBiomeDungeon = (_biomeSequence[0] == Biome::Caverns);  // keep fallback in sync
    }

    _spreadProjectiles.clear();
    _ultimateBlasts.clear();
    _lavaBalls.clear();
    _enemyProjectiles.clear();
    _poisonClouds.clear();
    _cyclopsLasers.clear();
    _pickups.clear();
    SpawnWarlockMinion();   // Warlock's imp (clears + respawns; no-op for other classes)
    _lastPlayerLevel = _player.GetLevel();   // avoid a false level-up flash on run start
    _lastPlayerHp    = _player.GetHealthValue();
    _slowMoTimer = 0.f; _critFocusTimer = 0.f; _screenFlashTimer = 0.f;
    _playerHurtTimer = 0.f; _deathVignette = 0.f;
    _displayGold = (float)_player.GetGold();  _displayCells = (float)_player.GetCells();
    _displayHpPct = 1.f; _displayManaPct = 1.f;
    _vfx.Clear();
    _damageNumbers.Clear();
    _player.SetClass(selectedClass);
    _player.SetAppearance(selectedAppearance.c_str());
    _player.Init();

    // Meta progression: permanent unlocks + gold retained from the last death.
    _player.ApplyMetaBonuses(
        _meta.GetStartingGoldBonus() + _meta.TakeGoldCarryover(),
        _meta.GetVitalityBonus(),
        _meta.GetManaRegenMultiplier(),
        _meta.HasFifthAbilitySlot(),
        _meta.HasSixthAbilitySlot(),
        _meta.GetStartingArmourBonus());

    _player.SetCellGainMultiplier(_meta.GetCellGainMultiplier());   // Echo Surge

    // Heirloom: begin the run already holding one random relic.
    if (_meta.HasStartingRelic())
        GrantRelic(RollRandomRelic());

    // Second Wind: one free revive per run.
    _secondWindAvailable = _meta.HasSecondWind();

    // Cursed Shrine / Wager: reset run-wide modifiers + the per-biome wager.
    _runPlayerDamageMult = 1.f;
    _runEnemyHealthMult  = 1.f;
    _runEnemyDamageMult  = 1.f;
    _wagerTier           = 0;
    _player.SetWagerRewardMult(1.f);
    _curseShrineUsed     = false;
    _nearCurseShrine     = false;
    _wagerAccessGranted  = false;   // earned only by choosing "push onward" after a boss
    ClearRunModifiers();            // death/new run drops all Risk Shrine contracts
    _pendingRelicChoices = 0;

    _deathPenaltyApplied   = false;
    _nearPoeAltar       = false;
    _cellsBankedToastTimer = 0.f;

    // Lock in the chosen ascension difficulty for this whole run and fold its
    // cumulative modifiers into the run-wide multipliers (reset to 1.0 above).
    // Boss HP and the heal-drop penalty are read from _ascensionMods at their
    // own hook sites.
    _ascensionTier     = _meta.GetSelectedAscension();
    _ascensionRecorded = false;
    _ascensionMods       = GetAscensionModifiers(_ascensionTier);
    _runEnemyHealthMult  *= _ascensionMods.enemyHpMult;
    _runEnemyDamageMult  *= _ascensionMods.enemyDmgMult;
    _runPlayerDamageMult *= _ascensionMods.playerDmgMult;

    _currentBiome = GetBiomeForAct(1);   // no initial biome transition
    _pendingBiome = _currentBiome;
    ApplyBiome(_currentBiome);
    _bossCyclopsSupport = {};
    _bossOgreSupport = {};

    for (auto& enemy : _enemies)
    {
        enemy->SetActive(false);
        enemy->Teleport(Vector2{ -5000.f, -5000.f });
    }

    _pickupSpawnTimer = kDefaultTimedPickupInterval;

    _nav.RefreshSync(_player.GetFeetWorldPos());
}

int Engine::GetActiveEnemyCount() const
{
    return _combatDirector.GetActiveEnemyCount(_enemies);
}

bool Engine::IsBossFightActive() const
{
    return _combatDirector.IsBossFightActive(_enemies);
}

bool Engine::TryGetFarSpawnPosition(Vector2& pos, float minPlayerDistance)
{
    float mapW = _map.width * _mapScale;
    float mapH = _map.height * _mapScale;

    for (int attempt = 0; attempt < 60; ++attempt)
    {
        Vector2 candidate{
            (float)GetRandomValue(300, (int)mapW - 300),
            (float)GetRandomValue(300, (int)mapH - 300)
        };

        if (!IsSpawnPositionValid(candidate))
            continue;
        if (Vector2Distance(candidate, _player.GetWorldPos()) < minPlayerDistance)
            continue;

        pos = candidate;
        return true;
    }

    return false;
}

void Engine::SpawnBossSupportAdds()
{
    BossSupportContext ctx{};
    ctx.bossCyclopsSupport = &_bossCyclopsSupport;
    ctx.bossOgreSupport = &_bossOgreSupport;
    ctx.tryGetFarSpawnPosition = [&](Vector2& pos, float minPlayerDistance) { return TryGetFarSpawnPosition(pos, minPlayerDistance); };
    ctx.spawnCyclops = [&](Vector2 pos) { return SpawnCyclops(pos); };
    ctx.spawnOgre = [&](Vector2 pos) { return SpawnOgre(pos); };
    ctx.isBossFightActive = [&]() { return IsBossFightActive(); };
    _combatDirector.SpawnBossSupportAdds(ctx);
}

void Engine::ClearBossSupportAdds()
{
    _combatDirector.ClearBossSupportAdds(_bossCyclopsSupport, _bossOgreSupport);
}

void Engine::UpdateBossSupportRespawns(float dt)
{
    BossSupportContext ctx{};
    ctx.bossCyclopsSupport = &_bossCyclopsSupport;
    ctx.bossOgreSupport = &_bossOgreSupport;
    ctx.tryGetFarSpawnPosition = [&](Vector2& pos, float minPlayerDistance) { return TryGetFarSpawnPosition(pos, minPlayerDistance); };
    ctx.spawnCyclops = [&](Vector2 pos) { return SpawnCyclops(pos); };
    ctx.spawnOgre = [&](Vector2 pos) { return SpawnOgre(pos); };
    ctx.isBossFightActive = [&]() { return IsBossFightActive(); };
    _combatDirector.UpdateBossSupportRespawns(ctx, dt);
}

bool Engine::TryGetPooledEnemySpawn(Vector2 pos)
{
    return SpawnBasicEnemy(pos) != nullptr;
}

Enemy* Engine::SpawnBasicEnemy(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;
        // Only reuse plain grunts — every specialised type has its own pool.
        // All tunable types carry a tuning name; the legacy specials don't.
        if (enemy->GetTuningName() != nullptr)
            continue;
        if (enemy->AsCyclops() != nullptr)
            continue;
        if (enemy->AsOgre() != nullptr)
            continue;
        if (enemy->AsMolarbeast() != nullptr)
            continue;
        if (enemy->IsBoss())
            continue;

        enemy->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*enemy);
        return enemy.get();
    }

    auto enemy = std::make_unique<Enemy>(pos);
    enemy->Init();
    ConfigureSpawnedEnemy(*enemy);
    Enemy* enemyPtr = enemy.get();
    _enemies.push_back(std::move(enemy));
    return enemyPtr;
}

bool Engine::TryGetPooledCyclopsSpawn(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        Cyclops* cyclops = enemy->AsCyclops();
        if (cyclops == nullptr)
            continue;

        cyclops->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*cyclops);
        return true;
    }

    return false;
}

int Engine::GetCyclopsSpawnCountForWave(int wave) const
{
    // Superseded by the curated wave table in SpawnEnemies - kept for
    // any legacy call sites that may still reference it.
    (void)wave;
    return 0;
}

bool Engine::TryGetPooledOgreSpawn(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        Ogre* ogre = enemy->AsOgre();
        if (ogre == nullptr)
            continue;

        ogre->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*ogre);
        return true;
    }

    return false;
}

bool Engine::TryGetPooledMolarbeastSpawn(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        Molarbeast* molarbeast = enemy->AsMolarbeast();
        if (molarbeast == nullptr)
            continue;

        molarbeast->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*molarbeast);
        return true;
    }

    return false;
}

int Engine::GetOgreSpawnCountForWave(int wave) const
{
    // Superseded by the curated wave table in SpawnEnemies - kept for
    // any legacy call sites that may still reference it.
    (void)wave;
    return 0;
}

int Engine::GetEnemyPowerLevelForWave(int wave) const
{
    // Power level advances every kRoomsPerPowerLevel rooms — roughly one tier per
    // world zone — so enemies visibly toughen up as the run deepens.
    // rooms 1-5: power 1   rooms 6-10: power 2   rooms 11-15: power 3   etc.
    if (wave <= 0)
        return 1;

    return 1 + ((wave - 1) / Balance::Curve::kRoomsPerPowerLevel);
}

void Engine::ConfigureSpawnedEnemy(Enemy& enemy)
{
    // All enemy types share the same spawn-tuning path:
    // 1. SetWaveScale - fixed base stats + behavioural timing for this room.
    // 2. ApplyEnemyPowerLevel - single global multiplier, advances every 10 rooms.
    // 3. SetTarget - restore player pointer so pooled enemies rejoin the run.
    // _wave = total rooms entered this run, used here for scaling only.
    if (_gameState == GameState::DungeonRun)
    {
        const float cellW = (float)kVirtualWidth / (float)RoomLayout::kCols;
        const float cellH = (float)kVirtualHeight / (float)RoomLayout::kRows;
        Vector2 safePos{};
        if (TryFindDungeonEnemySpawnPosition(
                &enemy, enemy.GetWorldPos(), cellW, cellH, 0.f, safePos))
        {
            enemy.Teleport(safePos);
        }
        else
        {
            // Malformed rooms with no usable floor must not create an enemy
            // inside a wall or pit. Leave the pooled object safely inactive.
            enemy.SetActive(false);
            enemy.Teleport({ -5000.f, -5000.f });
            return;
        }
    }

    enemy.SetCombatId(_nextCombatTargetId++);
    enemy.SetWaveScale(_wave);
    enemy.ApplyEnemyPowerLevel(GetEnemyPowerLevelForWave(_wave));

    // Run-wide difficulty multipliers (Ascension + Cursed Shrine are both folded
    // into these) stack on top of the per-wave power level.
    if (_runEnemyHealthMult != 1.f || _runEnemyDamageMult != 1.f)
        enemy.ApplyDifficultyScaling(_runEnemyHealthMult, _runEnemyDamageMult);

    // Ascension "Apex Predators" — bosses get extra health on top of everything.
    if (enemy.IsBoss() && _ascensionMods.bossHpMult != 1.f)
        enemy.ApplyDifficultyScaling(_ascensionMods.bossHpMult, 1.f);

    // Room affix — Swarm weakens each body, Cursed empowers them.
    const RoomAffixDef& affix = GetRoomAffixDef(_currentRoomAffix);
    if (affix.enemyHpMult != 1.f || affix.enemyDmgMult != 1.f)
        enemy.ApplyDifficultyScaling(affix.enemyHpMult, affix.enemyDmgMult);

    // Cursed Wager — the tier chosen at Zeph's shrine makes every enemy in this
    // biome much tougher (traded for bonus gold/XP/Echoes on the reward side).
    if (_wagerTier > 0)
    {
        const WagerTier& wager = kWagerTiers[_wagerTier - 1];
        enemy.ApplyDifficultyScaling(wager.enemyHp, wager.enemyDmg);
    }

    // Risk Shrine contract — a decision-room bargain that makes just THIS combat
    // room harder in exchange for bonus gold/XP (set on room entry, reset on clear).
    if (_roomModHpMult != 1.f || _roomModDmgMult != 1.f)
        enemy.ApplyDifficultyScaling(_roomModHpMult, _roomModDmgMult);

    // Colour-variant tier by world zone — later zones spawn recoloured,
    // visibly tougher versions (their stats already scale via power level).
    // Zones 0-1 = tier 0, zones 2-3 = tier 1, zone 4 = tier 2, zone 5 = tier 3.
    int variantTier = (_worldZone <= 1) ? 0 : (_worldZone <= 3) ? 1 : (_worldZone == 4) ? 2 : 3;
    enemy.SetVariantTier(variantTier);

    enemy.SetTarget(&_player);
    // Give every enemy its own pointer to the shared nav grid so it can
    // extract waypoints on its own staggered timer (see Enemy::HandleMovement).
    enemy.SetNavigationGrid(&_nav);
}

Enemy* Engine::SpawnCyclops(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        Cyclops* cyclops = enemy->AsCyclops();
        if (cyclops == nullptr)
            continue;

        cyclops->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*cyclops);
        return cyclops;
    }

    // No pooled instance - create a new one.
    auto cyclops = std::make_unique<Cyclops>(pos);
    cyclops->Init();
    ConfigureSpawnedEnemy(*cyclops);
    Enemy* cyclopsPtr = cyclops.get();
    _enemies.push_back(std::move(cyclops));
    return cyclopsPtr;
}

Enemy* Engine::SpawnOgre(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        Ogre* ogre = enemy->AsOgre();
        if (ogre == nullptr)
            continue;

        ogre->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*ogre);
        return ogre;
    }

    auto ogre = std::make_unique<Ogre>(pos);
    ogre->Init();
    ConfigureSpawnedEnemy(*ogre);
    Enemy* ogrePtr = ogre.get();
    _enemies.push_back(std::move(ogre));
    return ogrePtr;
}

void Engine::SpawnMolarbeast(Vector2 pos)
{
    if (TryGetPooledMolarbeastSpawn(pos))
    {
        _bossWarningTimer = 4.f;
        return;
    }

    auto molarbeast = std::make_unique<Molarbeast>(pos);
    molarbeast->Init();
    ConfigureSpawnedEnemy(*molarbeast);
    _enemies.push_back(std::move(molarbeast));
    _bossWarningTimer = 4.f;
}

Enemy* Engine::SpawnSkeletonArcher(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        SkeletonArcher* archer = enemy->AsSkeletonArcher();
        if (archer == nullptr)
            continue;

        archer->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*archer);
        return archer;
    }

    auto archer = std::make_unique<SkeletonArcher>(pos);
    archer->Init();
    ConfigureSpawnedEnemy(*archer);
    Enemy* archerPtr = archer.get();
    _enemies.push_back(std::move(archer));
    return archerPtr;
}

Enemy* Engine::SpawnFlameWisp(Vector2 pos)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        FlameWisp* wisp = enemy->AsFlameWisp();
        if (wisp == nullptr)
            continue;

        wisp->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*wisp);
        return wisp;
    }

    auto wisp = std::make_unique<FlameWisp>(pos);
    wisp->Init();
    ConfigureSpawnedEnemy(*wisp);
    Enemy* wispPtr = wisp.get();
    _enemies.push_back(std::move(wisp));
    return wispPtr;
}

Enemy* Engine::SpawnSlime(Vector2 pos, SlimeSize size)
{
    for (auto& enemy : _enemies)
    {
        if (enemy->IsActive())
            continue;

        SlimeEnemy* slime = enemy->AsSlime();
        if (slime == nullptr || slime->GetSize() != size)
            continue;

        slime->ResetForSpawn(pos);
        ConfigureSpawnedEnemy(*slime);
        return slime;
    }

    auto slime = std::make_unique<SlimeEnemy>(pos, size);
    slime->Init();
    ConfigureSpawnedEnemy(*slime);
    Enemy* slimePtr = slime.get();
    _enemies.push_back(std::move(slime));
    return slimePtr;
}

// Shared pooled-spawn body for the wave-2 grunt types — identical flow to the
// hand-written spawners above, parameterised on the concrete class.
template <typename EnemyType>
static Enemy* SpawnPooledType(std::vector<std::unique_ptr<Enemy>>& enemies, Vector2 pos,
    EnemyType* (Enemy::*asType)(), const std::function<void(Enemy&)>& configure)
{
    for (auto& enemy : enemies)
    {
        if (enemy->IsActive())
            continue;

        EnemyType* match = (enemy.get()->*asType)();
        if (match == nullptr)
            continue;

        match->ResetForSpawn(pos);
        configure(*match);
        return match;
    }

    auto fresh = std::make_unique<EnemyType>(pos);
    fresh->Init();
    configure(*fresh);
    Enemy* freshPtr = fresh.get();
    enemies.push_back(std::move(fresh));
    return freshPtr;
}

Enemy* Engine::SpawnSporeling(Vector2 pos)
{
    return SpawnPooledType<Sporeling>(_enemies, pos, &Enemy::AsSporeling,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnShieldbearer(Vector2 pos)
{
    return SpawnPooledType<Shieldbearer>(_enemies, pos, &Enemy::AsShieldbearer,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnPhantom(Vector2 pos)
{
    return SpawnPooledType<Phantom>(_enemies, pos, &Enemy::AsPhantom,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnBomberImp(Vector2 pos)
{
    return SpawnPooledType<BomberImp>(_enemies, pos, &Enemy::AsBomberImp,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnWarchief(Vector2 pos)
{
    return SpawnPooledType<Warchief>(_enemies, pos, &Enemy::AsWarchief,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnLivingBlade(Vector2 pos)
{
    return SpawnPooledType<LivingBlade>(_enemies, pos, &Enemy::AsLivingBlade,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnInfernal(Vector2 pos)
{
    return SpawnPooledType<Infernal>(_enemies, pos, &Enemy::AsInfernal,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnBonechill(Vector2 pos)
{
    return SpawnPooledType<Bonechill>(_enemies, pos, &Enemy::AsBonechill,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnStormclub(Vector2 pos)
{
    return SpawnPooledType<Stormclub>(_enemies, pos, &Enemy::AsStormclub,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

Enemy* Engine::SpawnVenomfang(Vector2 pos)
{
    return SpawnPooledType<Venomfang>(_enemies, pos, &Enemy::AsVenomfang,
        [&](Enemy& e) { ConfigureSpawnedEnemy(e); });
}

// Curated elite-bruiser pool. The elite room used to hardcode an Ogre; it now
// rolls one of five bruisers, each with its own identity — Ogre (charge +
// throws), Infernal (fire/burn), Bonechill (ice/slow, immovable), Stormclub
// (storm/knockback, leaping smash), and Venomfang (poison, hit-and-run).
Enemy* Engine::SpawnEliteMiniboss(Vector2 pos)
{
    enum { kOgre, kInfernal, kBonechill, kStormclub, kVenomfang, kEliteTypeCount };
    // Debug panel can pin the elite type for QA; -1 = normal random roll.
    const int forcedType = _debug.GetForcedEliteType();
    const int typeRoll = (forcedType >= 0 && forcedType < kEliteTypeCount)
        ? forcedType : GetRandomValue(0, kEliteTypeCount - 1);
    Enemy* miniboss = nullptr;
    switch (typeRoll)
    {
    case kInfernal:  miniboss = SpawnInfernal(pos);  break;
    case kBonechill: miniboss = SpawnBonechill(pos); break;
    case kStormclub: miniboss = SpawnStormclub(pos); break;
    case kVenomfang: miniboss = SpawnVenomfang(pos); break;
    default:         miniboss = SpawnOgre(pos);      break;
    }
    if (miniboss) miniboss->SetIsEliteMiniboss(true);
    return miniboss;
}

// =============================================================================
// Boss selection per biome — every domain now has its own signature boss.
// =============================================================================
// =============================================================================
// Class select + main-run start
// =============================================================================
int Engine::ComputeDailySeed() const
{
    time_t now = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    lt = *localtime(&now);
#endif
    return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}

void Engine::StartMainRun()
{
    _debug.Deactivate();
    _useHandcraftedDungeonRooms = true;
    _lastHandcraftedRoomId.clear();
    _forcedHandcraftedRoomId.clear();
    _handcraftedDungeonRoomIds.clear();
    // One-wallet meta loop: gold persists from the village INTO the run (only a
    // death zeroes it, in HandlePlayerDeathMetaPenalty). Capture the village
    // wallet before ResetRunState reseeds it, then restore it afterwards so the
    // hauled/leftover gold rides into the run instead of being reset.
    int walletGold = _player.GetGold();
    ResetRunState();   // calls _player.Init() which loads the chosen class sprites
    _player.SetGold(walletGold);
    _isMainGameRun = true;
    _runSessionData.Begin();
    _player.LearnAbility(StarterAbilityForClass(_player.GetClass()));

    // Daily runs use a fixed seed so everyone shares the same dungeon that day;
    // normal runs reseed from the clock so each one is different.
    if (_isDailyRun)
        SetRandomSeed((unsigned int)_dailySeed);
    else
        SetRandomSeed((unsigned int)time(nullptr));

    _currentBiome = Biome::Forest;
    _pendingBiome = _currentBiome;
    RefreshHandcraftedRooms();
    if (!GenerateHandcraftedDungeon(_currentBiome, _handcraftedDungeonRoomIds))
    {
        _message = "Forest room library cannot form a complete dungeon yet";
        TraceLog(LOG_ERROR, "%s", _message.c_str());
        EnterVillage();
        return;
    }
    LoadTilesetForBiome(_currentBiome);

    int startIdx = _dungeonGen.GetStartIndex();
    EnterDungeonRoom(startIdx, DungeonDoorSide::None, GetDungeonEntranceSpawnPos(), true);

    _fadeInTimer = 1.0f;
    _fadeInDuration = 1.0f;
}

void Engine::StartSelectedGameMode()
{
    if (ShouldPlayPrologue(_prologueEntryMode))
    {
        StartPrologue();
        return;
    }

    // Continue skips onboarding, so initialize the sprite sheets here using
    // the class and appearance that were just confirmed on hero select.
    const int walletGold = _player.GetGold();
    _player.Init();
    _player.SetGold(walletGold);
    _pendingNewRunFromVillage = true;
    _firstVillageVisit = false;
    EnterVillage();
}

void Engine::StartPrologue()
{
    int walletGold = _player.GetGold();
    _useHandcraftedDungeonRooms = false;
    _lastHandcraftedRoomId.clear();
    _forcedHandcraftedRoomId.clear();
    ResetRunState();
    _player.SetGold(walletGold);
    _isMainGameRun = false;
    _prologueActive = true;
    _firstDeathRevive = false;
    _firstVillageVisit = true;
    _pendingNewRunFromVillage = true;
    _prologue.Begin();
    _prologueLastHealth = _player.GetHealthValue();

    _player.LearnAbility(StarterAbilityForClass(_player.GetClass()));

    _currentBiome = Biome::Forest;
    _pendingBiome = _currentBiome;
    LoadTilesetForBiome(_currentBiome);
    _dungeonGen.GeneratePrologue();
    const float cellW = (float)kVirtualWidth / (float)RoomLayout::kCols;
    EnterDungeonRoom(0, DungeonDoorSide::None,
                     Vector2{ cellW * 2.f, (float)kVirtualHeight * 0.5f }, true);
    _message = "Learn the basics";
    _fadeInTimer = 0.8f;
    _fadeInDuration = 0.8f;
}

const Texture2D* Engine::GetRelicIcon(RelicType type) const
{
    if (type == RelicType::Count) return nullptr;
    int a = (int)GetRelicInfo(type).archetype;
    if (a >= 0 && a < 7 && _relicIcons[a].id != 0) return &_relicIcons[a];
    return nullptr;
}

Texture2D* Engine::GetAbilityIcon(AbilityType type)
{
    // Mage elemental abilities keep using the element pickup textures.
    if (IsElementalAbility(type))
    {
        if (type == AbilityType::IceSpread || type == AbilityType::IceBolt || type == AbilityType::IceUltimate)
            return &_abilityIconIceTex;
        if (type == AbilityType::ElectricSpread || type == AbilityType::ElectricBolt || type == AbilityType::ElectricUltimate)
            return &_abilityIconElectricTex;
        return &_abilityIconFireTex;
    }
    int idx = (int)type;
    if (idx >= 0 && idx < (int)AbilityType::Count && _abilityIcons[idx].id != 0)
        return &_abilityIcons[idx];
    return nullptr;
}

void Engine::ReloadAppearancePortrait()
{
    if (_appearancePortrait.id != 0) UnloadTexture(_appearancePortrait);
    const char* prefix = GetAppearancePrefix(_appearanceCursor);
    _appearancePortrait = LoadTexture(AssetPath(TextFormat("Hero/%s_Idle.png", prefix)).c_str());
}

void Engine::UpdateClassSelect()
{
    _gamepad.Update(_gamepadBindingsEdit);

    if (IsKeyPressed(KEY_ESCAPE) ||
        (_gamepad.isActive && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)))
    {
        // Backing out returns to wherever class select was opened from (the
        // main menu, or the village gate).
        _gameState = _classSelectReturnState;
        if (_gameState == GameState::Menu) _menu.Init();
        return;
    }

    const int     count    = (int)PlayerClass::Count;
    const int     appCount = GetAppearanceCount();
    const Vector2 mouse    = GetVirtualMousePos();
    const bool    click    = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    // ---- Shared carousel geometry (MUST match DrawClassSelect) ----------------
    const float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;
    const float centerW = 470.f, centerH = 540.f;
    const float centerX = sw * 0.5f - centerW * 0.5f;
    const float centerY = sh * 0.30f;
    const float sideW = 300.f, sideH = 440.f;
    const float sideY = centerY + (centerH - sideH) * 0.5f;
    const float leftPeekX  = centerX - sideW * 0.66f;
    const float rightPeekX = centerX + centerW - sideW * 0.34f;
    const float portraitCX = centerX + centerW * 0.5f;
    const float portraitCY = centerY + 176.f;
    Rectangle centerCard{ centerX, centerY, centerW, centerH };
    Rectangle leftPeek  { leftPeekX,  sideY, sideW, sideH };
    Rectangle rightPeek { rightPeekX, sideY, sideW, sideH };
    Rectangle bigLeft   { 50.f,       centerY + centerH * 0.5f - 48.f, 72.f, 96.f };
    Rectangle bigRight  { sw - 122.f, centerY + centerH * 0.5f - 48.f, 72.f, 96.f };
    Rectangle lookPrev  { centerX + 38.f,           portraitCY - 22.f, 46.f, 60.f };
    Rectangle lookNext  { centerX + centerW - 84.f, portraitCY - 22.f, 46.f, 60.f };

    // ---- Appearance ("look") cycling -- independent of class ------------------
    // Sprite lives on the TRIGGERS (LT/RT) so it can never be confused with the
    // class controls (D-pad / bumpers). Two separate button pairs on purpose.
    bool appPrev = IsKeyPressed(KEY_Q);
    bool appNext = IsKeyPressed(KEY_E);
    if (_gamepad.isActive)
    {
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_TRIGGER_2))  appPrev = true;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_2)) appNext = true;
    }
    bool clickedLook = false;
    if (click && CheckCollisionPointRec(mouse, lookPrev)) { appPrev = true; clickedLook = true; }
    if (click && CheckCollisionPointRec(mouse, lookNext)) { appNext = true; clickedLook = true; }
    if (appPrev) { _appearanceCursor = (_appearanceCursor + appCount - 1) % appCount; ReloadAppearancePortrait(); }
    if (appNext) { _appearanceCursor = (_appearanceCursor + 1) % appCount;            ReloadAppearancePortrait(); }

    // ---- Class cycling --------------------------------------------------------
    // Class lives on the D-pad AND the bumpers (LB/RB) — never the triggers.
    bool clsPrev = IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A);
    bool clsNext = IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D);
    if (_gamepad.isActive)
    {
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT)  || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_TRIGGER_1))  clsPrev = true;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) clsNext = true;
    }
    bool clickedNav = false;
    if (click && !clickedLook && (CheckCollisionPointRec(mouse, bigLeft)  || CheckCollisionPointRec(mouse, leftPeek)))  { clsPrev = true; clickedNav = true; }
    if (click && !clickedLook && (CheckCollisionPointRec(mouse, bigRight) || CheckCollisionPointRec(mouse, rightPeek))) { clsNext = true; clickedNav = true; }
    if (clsPrev) _classSelectCursor = (_classSelectCursor + count - 1) % count;
    if (clsNext) _classSelectCursor = (_classSelectCursor + 1) % count;

    // ---- Confirm --------------------------------------------------------------
    bool confirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                   (_gamepad.isActive && _gamepad.menuConfirmPressed);
    if (click && !clickedLook && !clickedNav && CheckCollisionPointRec(mouse, centerCard))
        confirm = true;

    if (confirm)
    {
        ClassUnlockProfile profile{
            _meta.IsRogueUnlocked(),
            _meta.IsWarlockUnlocked(),
            _meta.HasCompletedGame()
        };
        PlayerClass selectedClass = (PlayerClass)_classSelectCursor;
        ClassUnlockStatus status = GetClassUnlockStatus(selectedClass, profile);
        if (!status.unlocked)
        {
            StopSound(_buttonPressSound);
            PlaySound(_buttonPressSound);
            return;
        }
        _player.SetAppearance(GetAppearancePrefix(_appearanceCursor));
        _player.SetClass(selectedClass);
        StartSelectedGameMode();
    }
}

void Engine::DrawClassSelect()
{
    const int   count = (int)PlayerClass::Count;
    const float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;

    DrawScrollingCheckerboard(sw, sh, Color{ 26, 22, 34, 255 }, Color{ 34, 28, 44, 255 }, 16.f, 11.f);

    const char* title = "CHOOSE YOUR HERO";
    int titleFs = 60;
    DrawText(title, (int)(sw * 0.5f - MeasureText(title, titleFs) * 0.5f), 40, titleFs, RAYWHITE);

    // ---- Shared carousel geometry (MUST match UpdateClassSelect) --------------
    const float centerW = 470.f, centerH = 540.f;
    const float centerX = sw * 0.5f - centerW * 0.5f;
    const float centerY = sh * 0.30f;
    const float sideW = 300.f, sideH = 440.f;
    const float sideY = centerY + (centerH - sideH) * 0.5f;
    const float leftPeekX  = centerX - sideW * 0.66f;
    const float rightPeekX = centerX + centerW - sideW * 0.34f;
    const float portraitCX = centerX + centerW * 0.5f;
    const float portraitCY = centerY + 176.f;

    const int prevIdx = (_classSelectCursor + count - 1) % count;
    const int nextIdx = (_classSelectCursor + 1) % count;
    Vector2 mouse = GetVirtualMousePos();

    // Draws the shared appearance sprite (first idle frame) centred at (cx,cy).
    auto drawHero = [&](float cx, float cy, float scale, Color tint) {
        const Texture2D& tex = _appearancePortrait;
        if (tex.id == 0) return;
        float frameW = 32.f;
        Rectangle src{ 0.f, 0.f, frameW, (float)tex.height };
        Rectangle dst{ cx - frameW * scale * 0.5f, cy - tex.height * scale * 0.5f,
                       frameW * scale, tex.height * scale };
        DrawTexturePro(tex, src, dst, Vector2{}, 0.f, tint);
    };

    // ---- Dim neighbour peek cards (cover-flow) --------------------------------
    auto drawPeek = [&](float px, int idx) {
        Rectangle r{ px, sideY, sideW, sideH };
        const PlayerClassInfo& pinfo = GetPlayerClassInfo((PlayerClass)idx);
        DrawRectangleRounded(r, 0.08f, 8, Color{ 30, 27, 40, 220 });
        DrawRectangleRoundedLines(r, 0.08f, 8, Color{ 70, 64, 84, 255 });
        int fs = 26;
        DrawText(pinfo.name, (int)(r.x + r.width * 0.5f - MeasureText(pinfo.name, fs) * 0.5f),
                 (int)(r.y + 18.f), fs, Color{ 150, 144, 164, 255 });
        // NO hero sprite on the peeks — the sprite belongs ONLY to the focused
        // class card, so changing your look never looks tied to picking a class.
        int pfs = 17;
        DrawText(pinfo.playstyle, (int)(r.x + r.width * 0.5f - MeasureText(pinfo.playstyle, pfs) * 0.5f),
                 (int)(r.y + r.height * 0.5f - 10.f), pfs, Color{ 122, 118, 138, 255 });
    };
    drawPeek(leftPeekX,  prevIdx);
    drawPeek(rightPeekX, nextIdx);

    // ---- Big class arrows -----------------------------------------------------
    Rectangle bigLeft { 50.f,       centerY + centerH * 0.5f - 48.f, 72.f, 96.f };
    Rectangle bigRight{ sw - 122.f, centerY + centerH * 0.5f - 48.f, 72.f, 96.f };
    Color blC = CheckCollisionPointRec(mouse, bigLeft)  ? GOLD : Color{ 200, 195, 210, 255 };
    Color brC = CheckCollisionPointRec(mouse, bigRight) ? GOLD : Color{ 200, 195, 210, 255 };
    DrawTriangle({ bigLeft.x + bigLeft.width, bigLeft.y },
                 { bigLeft.x, bigLeft.y + bigLeft.height * 0.5f },
                 { bigLeft.x + bigLeft.width, bigLeft.y + bigLeft.height }, blC);
    DrawTriangle({ bigRight.x, bigRight.y },
                 { bigRight.x, bigRight.y + bigRight.height },
                 { bigRight.x + bigRight.width, bigRight.y + bigRight.height * 0.5f }, brC);

    // ---- Center focused class card --------------------------------------------
    const PlayerClassInfo& info = GetPlayerClassInfo((PlayerClass)_classSelectCursor);
    ClassUnlockProfile unlockProfile{
        _meta.IsRogueUnlocked(), _meta.IsWarlockUnlocked(), _meta.HasCompletedGame()
    };
    ClassUnlockStatus unlockStatus = GetClassUnlockStatus((PlayerClass)_classSelectCursor, unlockProfile);
    Rectangle centerCard{ centerX, centerY, centerW, centerH };
    DrawRectangleRounded(centerCard, 0.06f, 10, Color{ 54, 48, 70, 255 });
    DrawRectangleRoundedLines({ centerCard.x - 4.f, centerCard.y - 4.f, centerCard.width + 8.f, centerCard.height + 8.f }, 0.06f, 10, GOLD);
    DrawRectangleRoundedLines(centerCard, 0.06f, 10, GOLD);

    int nameFs = 44;
    DrawText(info.name, (int)(portraitCX - MeasureText(info.name, nameFs) * 0.5f),
             (int)(centerY + 20.f), nameFs, GOLD);

    // Hero (appearance) sub-panel INSIDE the class card. The class card selects
    // the CLASS; this labelled panel is explicitly where you pick your HERO look.
    Rectangle heroPanel{ centerX + 26.f, centerY + 78.f, centerW - 52.f, 224.f };
    DrawRectangleRounded(heroPanel, 0.10f, 8, Color{ 34, 30, 46, 235 });
    DrawRectangleRoundedLines(heroPanel, 0.10f, 8, Color{ 100, 92, 120, 255 });
    const char* heroLabel = "SELECT HERO";
    DrawText(heroLabel, (int)(portraitCX - MeasureText(heroLabel, 18) * 0.5f),
             (int)(heroPanel.y + 9.f), 18, Color{ 176, 168, 192, 255 });

    drawHero(portraitCX, portraitCY, 4.6f, WHITE);

    // Prominent hero arrows flanking the portrait (clearly the way to change look).
    Rectangle lookPrev{ centerX + 38.f,           portraitCY - 22.f, 46.f, 60.f };
    Rectangle lookNext{ centerX + centerW - 84.f, portraitCY - 22.f, 46.f, 60.f };
    Color lpC = CheckCollisionPointRec(mouse, lookPrev) ? GOLD : Color{ 224, 214, 150, 255 };
    Color lnC = CheckCollisionPointRec(mouse, lookNext) ? GOLD : Color{ 224, 214, 150, 255 };
    DrawTriangle({ lookPrev.x + lookPrev.width, lookPrev.y },
                 { lookPrev.x, lookPrev.y + lookPrev.height * 0.5f },
                 { lookPrev.x + lookPrev.width, lookPrev.y + lookPrev.height }, lpC);
    DrawTriangle({ lookNext.x, lookNext.y },
                 { lookNext.x, lookNext.y + lookNext.height },
                 { lookNext.x + lookNext.width, lookNext.y + lookNext.height * 0.5f }, lnC);
    const char* aName = GetAppearanceName(_appearanceCursor);
    DrawText(aName, (int)(portraitCX - MeasureText(aName, 22) * 0.5f),
             (int)(heroPanel.y + heroPanel.height - 30.f), 22, RAYWHITE);

    float tx = centerX + 30.f;
    DrawText(info.playstyle, (int)tx, (int)(centerY + 320.f), 22, Color{ 205, 200, 216, 255 });
    DrawText(TextFormat("HP %d    MP %d", info.baseHealth, info.baseMana),
             (int)tx, (int)(centerY + 360.f), 21, Color{ 170, 210, 170, 255 });
    DrawText(TextFormat("ATK %.0f    SPD %.0f", info.baseAttackPower, info.baseMoveSpeed),
             (int)tx, (int)(centerY + 390.f), 21, Color{ 214, 192, 150, 255 });
    DrawText(info.description, (int)tx, (int)(centerY + 428.f), 18, Color{ 168, 162, 182, 255 });

    // Class-mechanic explainer: what the class's resource is and that Rage /
    // Faith / Combo are bonuses ON TOP of MP, not replacements for it.
    DrawText(TextFormat("RESOURCE: %s", info.resourceName),
             (int)tx, (int)(centerY + 456.f), 18, GOLD);
    DrawText(info.resourceDesc, (int)tx, (int)(centerY + 478.f), 15, Color{ 190, 184, 202, 255 });

    if (!unlockStatus.unlocked)
    {
        DrawRectangleRounded(centerCard, 0.06f, 10, Fade(BLACK, 0.58f));
        const char* locked = "LOCKED";
        int lockedFs = 48;
        DrawText(locked, (int)(portraitCX - MeasureText(locked, lockedFs) * 0.5f),
                 (int)(centerY + 210.f), lockedFs, Color{ 255, 120, 120, 255 });
        int reasonFs = 24;
        DrawText(unlockStatus.reason,
                 (int)(portraitCX - MeasureText(unlockStatus.reason, reasonFs) * 0.5f),
                 (int)(centerY + 276.f), reasonFs, RAYWHITE);
    }

    // ---- Position pips + name -------------------------------------------------
    float pipY   = centerY + centerH + 34.f;
    float pipGap = 26.f;
    float pipStart = sw * 0.5f - (count - 1) * pipGap * 0.5f;
    for (int i = 0; i < count; i++)
    {
        bool on = (i == _classSelectCursor);
        DrawCircle((int)(pipStart + i * pipGap), (int)pipY, on ? 7.f : 5.f,
                   on ? GOLD : Color{ 90, 84, 104, 255 });
    }
    const char* pos = TextFormat("%s  -  %d / %d", info.name, _classSelectCursor + 1, count);
    DrawText(pos, (int)(sw * 0.5f - MeasureText(pos, 22) * 0.5f), (int)(pipY + 20.f), 22, Color{ 200, 195, 212, 255 });

    const char* hint = _gamepad.isActive
        ? "D-Pad / LB-RB: Class      LT-RT: Change Hero      A: Confirm      B: Back"
        : "A/D or Arrows: Class      Q/E: Change Hero      Enter: Confirm      ESC: Back";
    int hintFs = 24;
    DrawText(hint, (int)(sw * 0.5f - MeasureText(hint, hintFs) * 0.5f), (int)(sh - 52.f), hintFs, Color{ 175, 168, 188, 255 });
}

void Engine::SpawnBossForBiome(Vector2 pos)
{
    auto configure = [&](Enemy& e) { ConfigureSpawnedEnemy(e); };

    switch (_currentBiome)
    {
    case Biome::Caverns:
        SpawnMolarbeast(pos);
        return;   // SpawnMolarbeast already sets the warning timer

    case Biome::Forest:
        SpawnPooledType<Werewolf>(_enemies, pos, &Enemy::AsWerewolf, configure);
        break;

    case Biome::Jungle:
        SpawnPooledType<ChompBug>(_enemies, pos, &Enemy::AsChompBug, configure);
        break;

    case Biome::Graveyard:
        SpawnPooledType<PumpkinJack>(_enemies, pos, &Enemy::AsPumpkinJack, configure);
        break;

    case Biome::AncientCastle:
        SpawnPooledType<Minotaur>(_enemies, pos, &Enemy::AsMinotaur, configure);
        break;

    case Biome::LostCity:
        SpawnPooledType<Osiris>(_enemies, pos, &Enemy::AsOsiris, configure);
        break;

    case Biome::TheSanctuary:
        SpawnPooledType<TitanGuard>(_enemies, pos, &Enemy::AsTitanGuard, configure);
        break;

    case Biome::Wastelands:
        SpawnPooledType<ToxicVermin>(_enemies, pos, &Enemy::AsToxicVermin, configure);
        break;

    case Biome::DreamRealm:
        SpawnPooledType<AncientBear>(_enemies, pos, &Enemy::AsAncientBear, configure);
        break;

    case Biome::DemonsInsides:
    default:
        SpawnPooledType<AbyssSlime>(_enemies, pos, &Enemy::AsAbyssSlime, configure);
        break;
    }

    _bossWarningTimer = 4.f;
}

void Engine::SaveDungeonRoomEnemyState()
{
    if (_dungeonRoomIdx < 0)
        return;

    auto& state = _dungeonRoomStates[_dungeonRoomIdx];
    if (state.cleared)
    {
        state.enemiesInitialized = true;
        state.survivors.clear();
        return;
    }

    state.enemiesInitialized = true;
    state.survivors.clear();
    for (auto& enemy : _enemies)
    {
        if (!enemy || !enemy->IsActive())
            continue;

        DungeonEnemySnapshot snapshot{};
        snapshot.type = GetDungeonSnapshotType(*enemy);
        snapshot.pos = enemy->GetWorldPos();
        state.survivors.push_back(snapshot);
    }
}

bool Engine::RestoreDungeonRoomEnemyState(int roomIdx)
{
    auto it = _dungeonRoomStates.find(roomIdx);
    if (it == _dungeonRoomStates.end() || !it->second.enemiesInitialized)
        return false;

    DungeonRoomState& state = it->second;
    if (state.cleared)
    {
        _dungeonEnemiesSpawned = false;
        return true;
    }

    ResetEliteRoomRuntime();
    bool anySpawned = false;
    Enemy* restoredElite = nullptr;

    for (const DungeonEnemySnapshot& snapshot : state.survivors)
    {
        Enemy* spawned = SpawnDungeonSnapshotEnemy(snapshot);
        if (spawned)
        {
            anySpawned = true;
            // The elite buff is re-applied inside SpawnDungeonSnapshotEnemy for
            // any "Elite_<base>" survivor; just re-point the room's elite handle.
            if (spawned->IsEliteMiniboss())
                restoredElite = spawned;
        }
        else if (snapshot.type == "Molarbeast")
        {
            anySpawned = true;
        }
    }

    // Re-entry runs the SAME setup path as a fresh room. The stored modifier is
    // reused (GetCompatibleEliteMechanicForRoom keeps it), so the challenge never
    // rerolls; Guard Links recounts the surviving guards.
    if (_currentRoomType == RoomType::Elite && restoredElite)
        InitializeEliteRoomRuntime(restoredElite, roomIdx,
                                   (float)kVirtualWidth, (float)kVirtualHeight);

    _dungeonEnemiesSpawned = anySpawned;
    if (!anySpawned)
    {
        state.cleared = true;
        state.survivors.clear();
        ApplyDungeonRoomDoorState(_dungeonRoomLayout, roomIdx, _dungeonEntryDoorSide);
    }
    return true;
}

std::string Engine::GetDungeonSnapshotType(Enemy& enemy) const
{
    // Elites are stored as "Elite_<base>" so the real elite type (Ogre, Infernal,
    // ...) survives a room re-entry instead of always restoring as an Ogre.
    auto baseName = [&]() -> std::string {
    if (enemy.AsCyclops()) return "Cyclops";
    if (enemy.AsOgre()) return "Ogre";
    if (enemy.AsMolarbeast()) return "Molarbeast";
    if (enemy.AsSkeletonArcher()) return "SkeletonArcher";
    if (enemy.AsFlameWisp()) return "FlameWisp";
    if (SlimeEnemy* slime = enemy.AsSlime())
        return slime->GetSize() == SlimeSize::Small ? "SlimeSmall" : "SlimeBig";
    if (enemy.AsSporeling()) return "Sporeling";
    if (enemy.AsShieldbearer()) return "Shieldbearer";
    if (enemy.AsPhantom()) return "Phantom";
    if (enemy.AsBomberImp()) return "BomberImp";
    if (enemy.AsWarchief()) return "Warchief";
    if (enemy.AsLivingBlade()) return "LivingBlade";
    if (enemy.AsWerewolf()) return "Werewolf";
    if (enemy.AsChompBug()) return "ChompBug";
    if (enemy.AsOsiris()) return "Osiris";
    if (enemy.AsTitanGuard()) return "TitanGuard";
    if (enemy.AsToxicVermin()) return "ToxicVermin";
    if (enemy.AsAncientBear()) return "AncientBear";
    if (enemy.AsAbyssSlime()) return "AbyssSlime";
    if (enemy.AsPumpkinJack()) return "PumpkinJack";
    if (enemy.AsMinotaur()) return "Minotaur";
    if (enemy.AsInfernal()) return "Infernal";
    if (enemy.AsBonechill()) return "Bonechill";
    if (enemy.AsStormclub()) return "Stormclub";
    if (enemy.AsVenomfang()) return "Venomfang";
    return "Basic";
    };
    std::string base = baseName();
    return enemy.IsEliteMiniboss() ? ("Elite_" + base) : base;
}

Enemy* Engine::SpawnDungeonSnapshotEnemy(const DungeonEnemySnapshot& snapshot)
{
    // Elites are stored as "Elite_<base>": spawn the base type, then re-apply the
    // elite buff so the actual elite type is preserved across a room re-entry.
    if (snapshot.type.rfind("Elite_", 0) == 0)
    {
        DungeonEnemySnapshot base = snapshot;
        base.type = snapshot.type.substr(6);
        Enemy* elite = SpawnDungeonSnapshotEnemy(base);
        if (elite) elite->SetIsEliteMiniboss(true);
        return elite;
    }

    const Vector2 pos = snapshot.pos;
    if (snapshot.type == "Basic") return SpawnBasicEnemy(pos);
    if (snapshot.type == "Infernal") return SpawnInfernal(pos);
    if (snapshot.type == "Bonechill") return SpawnBonechill(pos);
    if (snapshot.type == "Stormclub") return SpawnStormclub(pos);
    if (snapshot.type == "Venomfang") return SpawnVenomfang(pos);
    if (snapshot.type == "Cyclops") return SpawnCyclops(pos);
    if (snapshot.type == "Ogre") return SpawnOgre(pos);
    if (snapshot.type == "EliteOgre") return SpawnOgre(pos);
    if (snapshot.type == "SkeletonArcher") return SpawnSkeletonArcher(pos);
    if (snapshot.type == "FlameWisp") return SpawnFlameWisp(pos);
    if (snapshot.type == "SlimeSmall") return SpawnSlime(pos, SlimeSize::Small);
    if (snapshot.type == "SlimeBig") return SpawnSlime(pos, SlimeSize::Big);
    if (snapshot.type == "Sporeling") return SpawnSporeling(pos);
    if (snapshot.type == "Shieldbearer") return SpawnShieldbearer(pos);
    if (snapshot.type == "Phantom") return SpawnPhantom(pos);
    if (snapshot.type == "BomberImp") return SpawnBomberImp(pos);
    if (snapshot.type == "Warchief") return SpawnWarchief(pos);
    if (snapshot.type == "LivingBlade") return SpawnLivingBlade(pos);
    if (snapshot.type == "Molarbeast") { SpawnMolarbeast(pos); return nullptr; }

    auto configure = [&](Enemy& e) { ConfigureSpawnedEnemy(e); };
    if (snapshot.type == "Werewolf") return SpawnPooledType<Werewolf>(_enemies, pos, &Enemy::AsWerewolf, configure);
    if (snapshot.type == "ChompBug") return SpawnPooledType<ChompBug>(_enemies, pos, &Enemy::AsChompBug, configure);
    if (snapshot.type == "Osiris") return SpawnPooledType<Osiris>(_enemies, pos, &Enemy::AsOsiris, configure);
    if (snapshot.type == "TitanGuard") return SpawnPooledType<TitanGuard>(_enemies, pos, &Enemy::AsTitanGuard, configure);
    if (snapshot.type == "ToxicVermin") return SpawnPooledType<ToxicVermin>(_enemies, pos, &Enemy::AsToxicVermin, configure);
    if (snapshot.type == "AncientBear") return SpawnPooledType<AncientBear>(_enemies, pos, &Enemy::AsAncientBear, configure);
    if (snapshot.type == "AbyssSlime") return SpawnPooledType<AbyssSlime>(_enemies, pos, &Enemy::AsAbyssSlime, configure);
    if (snapshot.type == "PumpkinJack") return SpawnPooledType<PumpkinJack>(_enemies, pos, &Enemy::AsPumpkinJack, configure);
    if (snapshot.type == "Minotaur") return SpawnPooledType<Minotaur>(_enemies, pos, &Enemy::AsMinotaur, configure);

    return SpawnBasicEnemy(pos);
}
// =============================================================================
// Debug-panel direct spawns — index order mirrors NewEnemyItems/NewBossItems
// in DebugPanel.cpp. Used only from the debug panel for playtesting.
// =============================================================================
void Engine::DebugSpawnLoot()
{
    Vector2 c = _player.GetWorldPos();
    for (int i = 0; i < 8; i++)
    {
        auto g = std::make_unique<GoldPickup>();
        GoldDenomination d = (i % 3 == 0) ? GoldDenomination::Ten
                           : (i % 3 == 1) ? GoldDenomination::Five : GoldDenomination::Single;
        g->Init(Vector2{ c.x + (float)GetRandomValue(-170, 170), c.y + (float)GetRandomValue(-170, 170) }, d);
        _pickups.push_back(std::move(g));
    }
    for (int i = 0; i < 4; i++)
    {
        auto ce = std::make_unique<CellPickup>();
        ce->Init(Vector2{ c.x + (float)GetRandomValue(-150, 150), c.y + (float)GetRandomValue(-150, 150) },
                 CellDenomination::Single);
        _pickups.push_back(std::move(ce));
    }
}

void Engine::DebugSpawnNewEnemy(int index, Vector2 pos)
{
    switch (index)
    {
    case 0: SpawnSkeletonArcher(pos);        break;
    case 1: SpawnFlameWisp(pos);             break;
    case 2: SpawnSlime(pos, SlimeSize::Big); break;
    case 3: SpawnSporeling(pos);             break;
    case 4: SpawnShieldbearer(pos);          break;
    case 5: SpawnPhantom(pos);               break;
    case 6: SpawnBomberImp(pos);             break;
    case 7: SpawnWarchief(pos);              break;
    case 8: SpawnLivingBlade(pos);           break;
    case 9:  SpawnInfernal(pos);             break;
    case 10: SpawnBonechill(pos);            break;
    case 11: SpawnStormclub(pos);            break;
    case 12: SpawnVenomfang(pos);            break;
    default: break;
    }
}

void Engine::DebugSpawnNewBoss(int index, Vector2 pos)
{
    auto configure = [&](Enemy& e) { ConfigureSpawnedEnemy(e); };
    switch (index)
    {
    case 0: SpawnPooledType<Werewolf>(_enemies, pos, &Enemy::AsWerewolf, configure);       break;
    case 1: SpawnPooledType<ChompBug>(_enemies, pos, &Enemy::AsChompBug, configure);       break;
    case 2: SpawnPooledType<Osiris>(_enemies, pos, &Enemy::AsOsiris, configure);           break;
    case 3: SpawnPooledType<TitanGuard>(_enemies, pos, &Enemy::AsTitanGuard, configure);   break;
    case 4: SpawnPooledType<ToxicVermin>(_enemies, pos, &Enemy::AsToxicVermin, configure); break;
    case 5: SpawnPooledType<AncientBear>(_enemies, pos, &Enemy::AsAncientBear, configure); break;
    case 6: SpawnPooledType<AbyssSlime>(_enemies, pos, &Enemy::AsAbyssSlime, configure);   break;
    case 7: SpawnPooledType<PumpkinJack>(_enemies, pos, &Enemy::AsPumpkinJack, configure); break;
    case 8: SpawnPooledType<Minotaur>(_enemies, pos, &Enemy::AsMinotaur, configure);       break;
    default: break;
    }
    _bossWarningTimer = 4.f;
}

// =============================================================================
// Enemy projectiles (arrows + fire bolts) — flight, player hit, bounds cull.
// =============================================================================
void Engine::UpdateEnemyProjectiles(float dt)
{
    for (auto& projectile : _enemyProjectiles)
    {
        if (!projectile.IsActive())
            continue;

        projectile.Update(dt);

        // Cull once fully off the room (rooms are exactly one virtual screen).
        Vector2 pos = projectile.GetWorldPos();
        if (pos.x < -120.f || pos.x > (float)kVirtualWidth  + 120.f ||
            pos.y < -120.f || pos.y > (float)kVirtualHeight + 120.f)
        {
            projectile.Destroy();
            continue;
        }

        if (CheckCollisionRecs(projectile.GetCollisionRec(), _player.GetCollisionRec()))
        {
            _player.TakeDamage(projectile.GetDamage(), projectile.GetWorldPos());
            if (projectile.GetKind() == EnemyProjectileKind::FireBolt)
                _vfx.SpawnHitEffect(Character::CastType::FireBolt, projectile.GetWorldPos(), Vector2{ 0.f, -1.f });
            projectile.Destroy();
        }
    }

    _enemyProjectiles.erase(
        std::remove_if(_enemyProjectiles.begin(), _enemyProjectiles.end(),
            [](const EnemyProjectile& p) { return !p.IsActive(); }),
        _enemyProjectiles.end());
}

void Engine::DrawEnemyProjectiles(Vector2 worldOffset) const
{
    for (const auto& projectile : _enemyProjectiles)
        projectile.Draw(worldOffset);
}

// =============================================================================
// Poison clouds — lingering ground hazards from Sporeling deaths and the
// Toxic Vermin boss. Standing inside one chips the player's health.
// =============================================================================
void Engine::SpawnPoisonCloud(Vector2 pos, float radius)
{
    PoisonCloud cloud;
    cloud.pos    = pos;
    cloud.timer  = 5.5f;
    cloud.radius = radius;
    _poisonClouds.push_back(cloud);

    // Persistent visual is now an animated poison-pool sprite (FX_BossPoisonPool)
    // instead of prototype Raylib ellipses. The cloud above keeps its own circular
    // collision/damage; this decal only draws it, and lives exactly as long.
    const int fx = (int)BossFx::PoisonPool;
    if (fx < (int)_bossFx.size() && _bossFx[fx].id != 0 && _bossFxFrames[fx] > 0)
    {
        const float decalScale = (2.f * radius) / 64.f;   // 64px cell → cover the radius
        _vfx.SpawnHazardDecal(&_bossFx[fx], pos, _bossFxFrames[fx], decalScale, cloud.timer,
                              Color{ 220, 255, 200, 235 });
    }
}

void Engine::UpdatePoisonClouds(float dt)
{
    if (_poisonDamageCooldown > 0.f)
        _poisonDamageCooldown -= dt;

    for (int i = (int)_poisonClouds.size() - 1; i >= 0; --i)
    {
        _poisonClouds[i].timer -= dt;
        if (_poisonClouds[i].timer <= 0.f)
        {
            _poisonClouds.erase(_poisonClouds.begin() + i);
            continue;
        }

        if (_poisonDamageCooldown <= 0.f && _player.IsAlive() &&
            Vector2Distance(_poisonClouds[i].pos, _player.GetFeetWorldPos()) < _poisonClouds[i].radius)
        {
            _player.TakeFractionalDamage(0.25f, _poisonClouds[i].pos);
            _poisonDamageCooldown = 0.7f;
        }
    }
}

void Engine::SpawnWarlockMinion()
{
    _warlockMinions.clear();
    if (_player.GetClass() != PlayerClass::Warlock) return;
    WarlockMinion m;
    m.pos = Vector2Add(_player.GetWorldPos(), Vector2{ -60.f, -40.f });
    _warlockMinions.push_back(m);
}

void Engine::UpdateWarlockMinions(float dt)
{
    if (_warlockMinions.empty()) return;
    const float kSpeed = 320.f, kAttackRange = 80.f, kLeash = 1100.f, kCooldown = 0.7f;
    const int   kBaseDamage = 4;   // atk-independent; ScalePlayerHit adds relic scaling
    Vector2 playerPos = _player.GetWorldPos();

    for (auto& m : _warlockMinions)
    {
        m.bob += dt;
        m.frameTimer += dt;
        if (m.frameTimer >= 1.f / 10.f) { m.frameTimer = 0.f; m.frame++; }
        if (m.attackCooldown > 0.f) m.attackCooldown -= dt;

        // Nearest active enemy within leash of the player.
        Enemy* target = nullptr; float bestD = kLeash * kLeash;
        for (auto& e : _enemies)
        {
            if (!e->IsActive() || !e->IsAlive()) continue;
            Vector2 ep = e->GetWorldPos();
            float dp = (ep.x - playerPos.x) * (ep.x - playerPos.x) + (ep.y - playerPos.y) * (ep.y - playerPos.y);
            if (dp > kLeash * kLeash) continue;
            float dm = (ep.x - m.pos.x) * (ep.x - m.pos.x) + (ep.y - m.pos.y) * (ep.y - m.pos.y);
            if (dm < bestD) { bestD = dm; target = e.get(); }
        }

        Vector2 goal  = target ? target->GetWorldPos() : Vector2Add(playerPos, Vector2{ -60.f, -40.f });
        Vector2 delta = Vector2Subtract(goal, m.pos);
        float   dist  = Vector2Length(delta);
        if (fabsf(delta.x) > 2.f) m.facing = (delta.x >= 0.f) ? 1.f : -1.f;

        if (target && dist <= kAttackRange)
        {
            if (m.attackCooldown <= 0.f)
            {
                bool crit = false;
                int dmg = ScalePlayerHit(*target, kBaseDamage, crit);
                const float healthBefore = target->GetHealthValue();
                target->TakeDamage(dmg, m.pos);
                Enemy::HitBlockReason blocked = target->ConsumeHitBlock();
                if (blocked != Enemy::HitBlockReason::None)
                    ShowBlockedHitFeedback(*target, blocked);
                else
                    RegisterHitFx(*target, healthBefore, crit, false, false, 7u);
                m.attackCooldown = kCooldown;
            }
        }
        else if (dist > 4.f)
        {
            Vector2 dir = Vector2Scale(delta, 1.f / dist);
            m.pos = Vector2Add(m.pos, Vector2Scale(dir, kSpeed * dt));
        }
    }
}

void Engine::DrawWarlockMinions(Vector2 worldOffset) const
{
    if (_minionTex.id == 0 || _warlockMinions.empty()) return;
    int frameH = _minionTex.height;
    int frames = (frameH > 0) ? (_minionTex.width / frameH) : 1;   // square frames
    if (frames < 1) frames = 1;
    float fw = (float)_minionTex.width / (float)frames;

    for (const auto& m : _warlockMinions)
    {
        int   fr    = (frames > 1) ? (m.frame % frames) : 0;
        float bobY  = sinf(m.bob * 3.f) * 6.f;
        float scale = 2.6f;
        Vector2 screen{ m.pos.x + worldOffset.x + kVirtualWidth  / 2.f,
                        m.pos.y + worldOffset.y + kVirtualHeight / 2.f + bobY };
        // Soft demonic aura so the ally reads clearly.
        DrawCircleV(Vector2{ screen.x, screen.y + frameH * scale * 0.32f }, 24.f, Fade(Color{ 150, 80, 220, 255 }, 0.20f));
        Rectangle src{ fr * fw, 0.f, fw * m.facing, (float)frameH };   // negative w flips
        Rectangle dst{ screen.x, screen.y, fw * scale, frameH * scale };
        DrawTexturePro(_minionTex, src, dst, Vector2{ dst.width * 0.5f, dst.height * 0.5f }, 0.f, Color{ 210, 170, 255, 255 });
    }
}

void Engine::DrawPoisonClouds(Vector2 worldOffset) const
{
    // The poison pool's visual is now a looping FX_BossPoisonPool decal spawned in
    // SpawnPoisonCloud (drawn by VFXManager), so there is no prototype Raylib shape
    // to render here anymore. Kept as a hook in case a future overlay (contact
    // shadow, edge glow) wants to layer on top of the sprite.
    (void)worldOffset;
}

// =============================================================================
// Relics — damage scaling, on-kill effects, granting, HUD strip.
// =============================================================================
int Engine::ScalePlayerHit(const Enemy& target, int baseDamage, bool& outCrit) const
{
    float hpFraction = (target.GetMaxHealthValue() > 0.f)
        ? (target.GetHealthValue() / target.GetMaxHealthValue()) : 1.f;
    // Cursed Shrine blessing/curse modifies all outgoing player damage.
    int scaledBase = std::max(1, (int)lroundf(baseDamage * _runPlayerDamageMult));
    int dmg = _player.ScaleOutgoingDamage(target.IsFrozen(), target.IsCharged(),
        target.IsBurning(), hpFraction, scaledBase, outCrit);
    if (_juiceForceCrit && !outCrit) { outCrit = true; dmg *= 2; }   // debug: force crit

    // ── Universal status-combo reactions (NOT relic-gated) ──────────────────────
    // These make applying statuses inherently rewarding and create cross-class
    // synergy (a Mage's freeze lets a Warrior shatter; any class can execute a
    // Paladin/Rogue-marked target). Relic bonuses (Permafrost/Executioner/…) stack
    // on top via ScaleOutgoingDamage above.
    float comboMult = 1.f;
    if (target.IsFrozen())                             comboMult += 0.25f;   // Shatter frozen foes
    if (target.IsMarked())
        comboMult += (hpFraction <= 0.30f) ? 1.20f : 0.12f;                  // Execute the marked & wounded
    // Hunter class identity: marked prey takes +30% from ALL Hunter damage on top
    // of the universal mark bonus — marking priority targets is the class's brain.
    if (target.IsMarked() && _player.GetClass() == PlayerClass::Hunter)
        comboMult += _player.GetMarkedBonus();   // Hunter's Quarry cards raise this
    // Warlock class identity: cursed targets take +25% from ALL Warlock damage —
    // curse first, then collect. Delayed but powerful is the class's brain.
    if (target.IsCursed() && _player.GetClass() == PlayerClass::Warlock)
        comboMult += _player.GetCursedBonus();   // Dark Pact cards raise this
    dmg = (int)lroundf(dmg * comboMult);

    // Vulnerability / armor break: a marked-vulnerable target takes extra damage.
    dmg = std::max(1, (int)lroundf(dmg * target.GetIncomingDamageMult()));
    return dmg;
}

void Engine::ApplyRelicOnKill(Vector2 pos, bool wasBurning, bool wasFrozen,
                              bool wasCharged, bool /*eliteOrBoss*/)
{
    // Wildfire — a burning corpse erupts, damaging + igniting nearby enemies.
    if (wasBurning && _player.WantsWildfire())
    {
        const float radius = 200.f;
        _vfx.SpawnHitEffect(Character::CastType::FireSpread, pos, Vector2{ 0.f, -1.f });
        for (auto& e : _enemies)
        {
            if (!e->IsActive() || e->IsDying()) continue;
            if (Vector2Distance(e->GetWorldPos(), pos) < radius)
            {
                const float healthBefore = e->GetHealthValue();
                e->TakeDamage(3, pos);
                Enemy::HitBlockReason blocked = e->ConsumeHitBlock();
                if (blocked != Enemy::HitBlockReason::None)
                    ShowBlockedHitFeedback(*e, blocked);
                else if (RegisterHitFx(*e, healthBefore, false, false, false, 3001u) > 0)
                    e->ApplyBurn(0.5f, 2, pos);
            }
        }
    }

    // Shatter Strike — a frozen kill freezes the dead enemy's neighbours.
    if (wasFrozen && _player.WantsShatterStrike())
    {
        const float radius = 220.f;
        for (auto& e : _enemies)
        {
            if (!e->IsActive() || e->IsDying()) continue;
            if (Vector2Distance(e->GetWorldPos(), pos) < radius)
                e->ApplyFreeze(2.5f);
        }
    }

    // Storm's Reach — a charged kill shocks + stuns nearby enemies.
    if (wasCharged && _player.WantsStormsReach())
    {
        const float radius = 220.f;
        for (auto& e : _enemies)
        {
            if (!e->IsActive() || e->IsDying()) continue;
            if (Vector2Distance(e->GetWorldPos(), pos) < radius)
            {
                const float healthBefore = e->GetHealthValue();
                e->TakeDamage(2, pos);
                Enemy::HitBlockReason blocked = e->ConsumeHitBlock();
                if (blocked != Enemy::HitBlockReason::None)
                    ShowBlockedHitFeedback(*e, blocked);
                else if (RegisterHitFx(*e, healthBefore, false, false, false, 3002u) > 0)
                    e->ApplyElectricCharge();
            }
        }
    }
}

RelicType Engine::RollRandomRelic() const
{
    // Gather unowned relics, weighted by rarity (commons more likely).
    std::vector<RelicType> pool;
    std::vector<int>       weights;
    for (int i = 0; i < (int)RelicType::Count; i++)
    {
        RelicType type = (RelicType)i;
        if (_player.HasRelic(type))
            continue;
        int w = 1;
        switch (GetRelicInfo(type).rarity)
        {
        case RelicRarity::Common: w = 6; break;
        case RelicRarity::Rare:   w = 3; break;
        case RelicRarity::Epic:   w = 1; break;
        }
        pool.push_back(type);
        weights.push_back(w);
    }
    if (pool.empty())
        return RelicType::Count;   // player owns everything

    int total = 0;
    for (int w : weights) total += w;
    int roll = GetRandomValue(1, total);
    for (size_t i = 0; i < pool.size(); i++)
    {
        roll -= weights[i];
        if (roll <= 0)
            return pool[i];
    }
    return pool.back();
}

void Engine::GrantRelic(RelicType type)
{
    if (type == RelicType::Count || _player.HasRelic(type))
        return;
    _player.AddRelic(type);
    _relicToastName  = GetRelicInfo(type).name;
    _relicToastTimer = 4.f;
    // Power-gain moment.
    TriggerScreenFlash(Color{ 200, 160, 255, 255 }, 0.30f);
    RequestHitStop(0.06f);
    if (_audioInitialised)
        SfxBank::Get().Play(SfxId::AbilityLearn, 0.7f);   // relic-gain fanfare
}

void Engine::DrawOwnedRelics() const
{
    int count = _player.GetRelicCount();
    if (count <= 0)
        return;

    // Compact vertical strip of coloured relic pips on the left edge, below
    // the resource labels. Archetype colour + first letter keeps it readable
    // without dedicated relic art.
    const float startX = 24.f;
    const float startY = 150.f;
    const float size   = 34.f;
    const float gap    = 6.f;

    for (int i = 0; i < count; i++)
    {
        RelicType type = _player.GetRelicAt(i);
        const RelicInfo& info = GetRelicInfo(type);
        Color archColor;
        switch (info.archetype)
        {
        case RelicArchetype::Fire:     archColor = Color{ 230, 90,  40, 255 }; break;
        case RelicArchetype::Ice:      archColor = Color{ 90, 180, 235, 255 }; break;
        case RelicArchetype::Electric: archColor = Color{ 235, 210, 60, 255 }; break;
        case RelicArchetype::Offense:  archColor = Color{ 210, 70,  90, 255 }; break;
        case RelicArchetype::Defense:  archColor = Color{ 120, 190, 120, 255 }; break;
        case RelicArchetype::Economy:  archColor = Color{ 220, 180, 70, 255 }; break;
        default:                       archColor = Color{ 170, 150, 220, 255 }; break;
        }

        float y = startY + i * (size + gap);
        Rectangle pip{ startX, y, size, size };
        DrawRectangleRounded(pip, 0.28f, 5, Fade(archColor, 0.85f));
        DrawRectangleRoundedLines(pip, 0.28f, 5, Fade(WHITE, 0.35f));
        const Texture2D* ricon = GetRelicIcon(type);
        if (ricon && ricon->id != 0)
        {
            float s = (size - 8.f) / (float)ricon->width;
            Rectangle src{ 0.f, 0.f, (float)ricon->width, (float)ricon->height };
            Rectangle dst{ startX + 4.f, y + 4.f, ricon->width * s, ricon->height * s };
            DrawTexturePro(*ricon, src, dst, Vector2{}, 0.f, WHITE);
        }
        else
        {
            char letter[2] = { info.name[0], '\0' };
            DrawText(letter, (int)(startX + size * 0.5f - 6.f), (int)(y + 6.f), 22, RAYWHITE);
        }
    }
}

// =============================================================================
std::vector<Vector2> Engine::GetCyclopsLaserEndpoints(const CyclopsLaserProjectile& laser) const
{
    std::vector<Vector2> endpoints;
    endpoints.reserve(laser.GetBeamCount());

    const Vector2 start = laser.GetWorldPos();
    const float maxLength = laser.GetCurrentBeamLength();

    for (int i = 0; i < laser.GetBeamCount(); ++i)
    {
        Vector2 dir = laser.GetBeamDirection(i);
        if (Vector2LengthSqr(dir) < 0.0001f)
            continue;

        Vector2 farEnd = Vector2Add(start, Vector2Scale(dir, maxLength));
        Vector2 bestEnd = farEnd;
        float bestDist = maxLength;

        for (const auto& prop : _props)
        {
            Rectangle rect = prop.GetCollisionRec();
            const Vector2 corners[4] = {
                { rect.x, rect.y },
                { rect.x + rect.width, rect.y },
                { rect.x + rect.width, rect.y + rect.height },
                { rect.x, rect.y + rect.height }
            };

            for (int edge = 0; edge < 4; ++edge)
            {
                Vector2 hit{};
                Vector2 edgeStart = corners[edge];
                Vector2 edgeEnd = corners[(edge + 1) % 4];
                if (!CheckCollisionLines(start, farEnd, edgeStart, edgeEnd, &hit))
                    continue;

                float hitDist = Vector2Distance(start, hit);
                if (hitDist < bestDist)
                {
                    bestDist = hitDist;
                    bestEnd = hit;
                }
            }
        }

        endpoints.push_back(bestEnd);
    }

    return endpoints;
}

bool Engine::SegmentHitsRect(Vector2 start, Vector2 end, float thickness, const Rectangle& rect) const
{
    Rectangle expanded{
        rect.x - thickness * 0.5f,
        rect.y - thickness * 0.5f,
        rect.width + thickness,
        rect.height + thickness
    };

    if (CheckCollisionPointRec(start, expanded) || CheckCollisionPointRec(end, expanded))
        return true;

    const Vector2 corners[4] = {
        { expanded.x, expanded.y },
        { expanded.x + expanded.width, expanded.y },
        { expanded.x + expanded.width, expanded.y + expanded.height },
        { expanded.x, expanded.y + expanded.height }
    };

    for (int edge = 0; edge < 4; ++edge)
    {
        Vector2 hit{};
        if (CheckCollisionLines(start, end, corners[edge], corners[(edge + 1) % 4], &hit))
            return true;
    }

    return false;
}

void Engine::DrawCyclopsLasers(Vector2 worldOffset)
{
    for (const auto& laser : _cyclopsLasers)
    {
        if (!laser.IsActive())
            continue;

        std::vector<Vector2> endpoints = GetCyclopsLaserEndpoints(laser);
        laser.Draw(worldOffset, endpoints.data(), (int)endpoints.size());
    }
}

void Engine::UpdateCyclopsLasers(float dt)
{
    for (auto& laser : _cyclopsLasers)
    {
        if (!laser.IsActive())
            continue;

        laser.Update(dt);
        if (!laser.IsActive())
            continue;

        if (_player.IsAlive() && laser.CanHitPlayer())
        {
            std::vector<Vector2> endpoints = GetCyclopsLaserEndpoints(laser);
            for (const Vector2& end : endpoints)
            {
                if (!SegmentHitsRect(laser.GetWorldPos(), end, laser.GetBeamWidth(), _player.GetCollisionRec()))
                    continue;

                int laserDmg = laser.GetDamage();
                const float healthBefore = _player.GetHealthValue();
                _player.TakeDamage(laserDmg, laser.GetWorldPos());
                DamageNumberEvent incoming{};
                incoming.targetId = 0;
                incoming.attackId = 9001u;
                incoming.worldPos = _player.GetWorldPos();
                incoming.finalDamage = CalculateActualDamage(healthBefore, _player.GetHealthValue());
                incoming.outcome = DamageNumberOutcome::Incoming;
                if (incoming.finalDamage > 0) _damageNumbers.Submit(incoming);
                laser.OnHitPlayer();
                TriggerScreenShake(2.5f, 0.07f);
                break;
            }
        }
    }

    _cyclopsLasers.erase(
        std::remove_if(_cyclopsLasers.begin(), _cyclopsLasers.end(),
            [](const CyclopsLaserProjectile& l) { return !l.IsActive(); }),
        _cyclopsLasers.end());
}

void Engine::UpdateLavaBallProjectiles(float dt)
{
    // In DungeonRun mode _map is not loaded, so fall back to the virtual
    // canvas size with one tile-cell of clearance on each side.
    const float cellW = (float)kVirtualWidth  / (float)RoomLayout::kCols;
    const float cellH = (float)kVirtualHeight / (float)RoomLayout::kRows;
    const bool hasTileMap = (_map.width > 0 && _map.height > 0);
    const float mapW        = hasTileMap ? _map.width  * _mapScale : (float)kVirtualWidth;
    const float mapH        = hasTileMap ? _map.height * _mapScale : (float)kVirtualHeight;
    const float marginLeft   = hasTileMap ? 76.f  : cellW;
    const float marginRight  = hasTileMap ? 96.f  : cellW;
    const float marginTop    = hasTileMap ? 42.f  : cellH;
    const float marginBottom = hasTileMap ? 320.f : cellH;

    for (auto& projectile : _lavaBalls)
    {
        if (!projectile.IsActive())
            continue;

        projectile.Update(dt);
        if (!projectile.IsActive())
            continue;

        Rectangle collisionRec = projectile.GetCollisionRec();

        // Arena wall and prop checks only matter while the ball is travelling.
        if (projectile.IsFlying())
        {
            bool hitArenaWall =
                collisionRec.x < marginLeft ||
                collisionRec.x + collisionRec.width > mapW - marginRight ||
                collisionRec.y < marginTop ||
                collisionRec.y + collisionRec.height > mapH - marginBottom;

            if (hitArenaWall)
            {
                projectile.BeginHit();
                StopSound(_lavaBallImpactSound);
                PlaySound(_lavaBallImpactSound);
                continue;
            }

            bool hitProp = false;
            for (const auto& prop : _props)
            {
                if (CheckCollisionRecs(collisionRec, prop.GetCollisionRec()))
                {
                    hitProp = true;
                    break;
                }
            }
            if (hitProp)
            {
                projectile.BeginHit();
                StopSound(_lavaBallImpactSound);
                PlaySound(_lavaBallImpactSound);
                continue;
            }
        }

        // Player collision - only register once per projectile (HasHitPlayer guard).
        if (_player.IsAlive() &&
            !projectile.HasHitPlayer() &&
            CheckCollisionRecs(collisionRec, _player.GetCollisionRec()))
        {
            static constexpr int kLavaBallDamage = 2;
            const float healthBefore = _player.GetHealthValue();
            _player.TakeDamage(kLavaBallDamage, projectile.GetWorldPos());
            DamageNumberEvent incoming{};
            incoming.targetId = 0;
            incoming.attackId = 9002u;
            incoming.worldPos = _player.GetWorldPos();
            incoming.finalDamage = CalculateActualDamage(healthBefore, _player.GetHealthValue());
            incoming.outcome = DamageNumberOutcome::Incoming;
            if (incoming.finalDamage > 0) _damageNumbers.Submit(incoming);
            projectile.OnPlayerHit();
            if (projectile.IsFlying())
            {
                projectile.BeginHit();
                StopSound(_lavaBallImpactSound);
                PlaySound(_lavaBallImpactSound);
            }
        }
    }

    _lavaBalls.erase(
        std::remove_if(_lavaBalls.begin(), _lavaBalls.end(),
            [](const LavaBallProjectile& projectile) { return !projectile.IsActive(); }),
        _lavaBalls.end());
}

void Engine::SaveKeybindings()
{
    const KeyBindings& b  = _player.GetBindings();
    const GamepadBindings& g = _gamepadBindingsEdit;
    FILE* f = fopen("keybindings.cfg", "w");
    if (!f) return;
    // Keyboard / Mouse
    std::fprintf(f, "moveUp %d\n",    (int)b.moveUp);
    std::fprintf(f, "moveDown %d\n",  (int)b.moveDown);
    std::fprintf(f, "moveLeft %d\n",  (int)b.moveLeft);
    std::fprintf(f, "moveRight %d\n", (int)b.moveRight);
    std::fprintf(f, "dash %d\n",      (int)b.dash);
    std::fprintf(f, "attack %d\n",    (int)b.attack);
    std::fprintf(f, "ability0 %d\n",  (int)b.ability[0]);
    std::fprintf(f, "ability1 %d\n",  (int)b.ability[1]);
    std::fprintf(f, "ability2 %d\n",  (int)b.ability[2]);
    std::fprintf(f, "ability3 %d\n",  (int)b.ability[3]);
    // Gamepad
    std::fprintf(f, "gp_attack %d\n",    (int)g.attack);
    std::fprintf(f, "gp_dash %d\n",      (int)g.dash);
    std::fprintf(f, "gp_ability0 %d\n",  (int)g.ability[0]);
    std::fprintf(f, "gp_ability1 %d\n",  (int)g.ability[1]);
    std::fprintf(f, "gp_ability2 %d\n",  (int)g.ability[2]);
    std::fprintf(f, "gp_ability3 %d\n",  (int)g.ability[3]);
    std::fprintf(f, "gp_pause %d\n",     (int)g.pause);
    std::fclose(f);
}

void Engine::LoadKeybindings()
{
    FILE* f = fopen("keybindings.cfg", "r");
    if (!f) return;
    KeyBindings     b;
    GamepadBindings g;
    char key[32];
    int  value;
    while (fscanf(f, "%31s %d", key, &value) == 2)
    {
        if      (std::strcmp(key, "moveUp")     == 0) b.moveUp       = (KeyboardKey)value;
        else if (std::strcmp(key, "moveDown")   == 0) b.moveDown     = (KeyboardKey)value;
        else if (std::strcmp(key, "moveLeft")   == 0) b.moveLeft     = (KeyboardKey)value;
        else if (std::strcmp(key, "moveRight")  == 0) b.moveRight    = (KeyboardKey)value;
        else if (std::strcmp(key, "dash")       == 0) b.dash         = (KeyboardKey)value;
        else if (std::strcmp(key, "attack")     == 0) b.attack       = (KeyboardKey)value;
        else if (std::strcmp(key, "ability0")   == 0) b.ability[0]   = (KeyboardKey)value;
        else if (std::strcmp(key, "ability1")   == 0) b.ability[1]   = (KeyboardKey)value;
        else if (std::strcmp(key, "ability2")   == 0) b.ability[2]   = (KeyboardKey)value;
        else if (std::strcmp(key, "ability3")   == 0) b.ability[3]   = (KeyboardKey)value;
        else if (std::strcmp(key, "gp_attack")  == 0) g.attack       = (GamepadButton)value;
        else if (std::strcmp(key, "gp_dash")    == 0) g.dash         = (GamepadButton)value;
        else if (std::strcmp(key, "gp_ability0")== 0) g.ability[0]   = (GamepadButton)value;
        else if (std::strcmp(key, "gp_ability1")== 0) g.ability[1]   = (GamepadButton)value;
        else if (std::strcmp(key, "gp_ability2")== 0) g.ability[2]   = (GamepadButton)value;
        else if (std::strcmp(key, "gp_ability3")== 0) g.ability[3]   = (GamepadButton)value;
        else if (std::strcmp(key, "gp_pause")   == 0) g.pause        = (GamepadButton)value;
    }
    std::fclose(f);
    _player.SetBindings(b);
    _gamepadBindingsEdit = g;
}

// -----------------------------------------------------------------------------
// Touch Controls
// -----------------------------------------------------------------------------

// Returns the screen-space centre of touch-mode ability arc button for `slot`.
// Buttons are arranged in a quarter-circle arc above the ATK button.
// Screen angles: 270- = straight up, 210- = upper-left.
// Free helper - computes the bounding rect for touch-mode ability slot `slot`.
// 4 square slots in a right-aligned row, above the ATK/DASH buttons.
// `offset` is a per-slot drag offset applied on top of the computed base position.
static Rectangle TouchAbilityRect(int slot, int screenW, int screenH,
                                   float btnBotPad, float btnRadius,
                                   float slotSz, float slotGap, float rightPad, float yOff,
                                   Vector2 offset)
{
    const float slotY  = (float)screenH - btnBotPad - btnRadius - yOff - slotSz;
    const float totalW = 4.f * slotSz + 3.f * slotGap;
    const float startX = (float)screenW - rightPad - totalW;
    return { startX + (float)slot * (slotSz + slotGap) + offset.x,
             slotY + offset.y, slotSz, slotSz };
}

Rectangle Engine::GetDebugToggleTabRect() const
{
    const float screenW = (float)kVirtualWidth;
    const float screenH = (float)kVirtualHeight;
    return Rectangle{ screenW - 54.f, screenH * 0.43f, 42.f, 132.f };
}

bool Engine::HandleDebugToggleTabInput()
{
    if (!_debug.IsActive())
        return false;

    const Rectangle debugTab = GetDebugToggleTabRect();
    const int touchCount = GetTouchPointCount();

    if (touchCount == 0)
    {
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            CheckCollisionPointRec(GetVirtualMousePos(), debugTab))
        {
            _debug.ToggleOpen();
            return true;
        }
        return false;
    }

    for (int i = 0; i < touchCount; ++i)
    {
        if (CheckCollisionPointRec(GetVirtualTouchPos(i), debugTab))
        {
            _debug.ToggleOpen();
            return true;
        }
    }

    return false;
}

// -- Hitbox debug editor -------------------------------------------------------

// -- Dungeon run combat helpers ------------------------------------------------------

std::vector<Rectangle> Engine::GetDungeonSpawnBlockers(float cellW, float cellH) const
{
    std::vector<Rectangle> blockers;
    blockers.reserve(_dungeonRoomLayout.props.size() +
                     _dungeonRoomLayout.animProps.size());
    const float pxScaleX = cellW / 16.f;
    const float pxScaleY = cellH / 16.f;

    auto addBlocker = [&](const SpritePlacement& prop, const Rectangle& collision)
    {
        if (collision.width <= 0.f || collision.height <= 0.f) return;
        blockers.push_back({
            prop.col * cellW + collision.x * pxScaleX,
            prop.row * cellH + collision.y * pxScaleY,
            collision.width * pxScaleX,
            collision.height * pxScaleY
        });
    };

    for (const SpritePlacement& prop : _dungeonRoomLayout.props)
    {
        const TileDefSet* defs = ResolveRoomDefinitions(
            _dungeonRoomLayout, prop, _tileDefs);
        if (defs == nullptr || prop.defIdx < 0 ||
            prop.defIdx >= (int)defs->props.size())
            continue;
        addBlocker(prop, defs->props[prop.defIdx].collision);
    }
    for (const SpritePlacement& prop : _dungeonRoomLayout.animProps)
    {
        const TileDefSet* defs = ResolveRoomDefinitions(
            _dungeonRoomLayout, prop, _tileDefs);
        if (defs == nullptr || prop.defIdx < 0 ||
            prop.defIdx >= (int)defs->animProps.size())
            continue;
        addBlocker(prop, defs->animProps[prop.defIdx].collision);
    }
    return blockers;
}

Rectangle Engine::GetDungeonEnemySpawnBody(const Enemy* enemy, Vector2 worldPos,
                                           float cellW, float cellH) const
{
    if (enemy != nullptr)
    {
        const Rectangle current = enemy->GetCollisionRec();
        const Vector2 currentPos = enemy->GetWorldPos();
        return {
            worldPos.x + current.x - currentPos.x,
            worldPos.y + current.y - currentPos.y,
            current.width,
            current.height
        };
    }

    // Candidate generation happens before an enemy type exists. A conservative
    // cell-sized footprint prevents the initial choice from hugging blockers;
    // ConfigureSpawnedEnemy validates the concrete enemy body afterward.
    const float width = cellW * 0.72f;
    const float height = cellH * 0.72f;
    return { worldPos.x - width * 0.5f, worldPos.y - height * 0.5f,
             width, height };
}

bool Engine::IsDungeonEnemySpawnPositionValid(
    const Enemy* enemy, Vector2 worldPos, float cellW, float cellH,
    const std::vector<Rectangle>& blockers) const
{
    return IsRoomSpawnAreaValid(
        _dungeonRoomLayout,
        GetDungeonEnemySpawnBody(enemy, worldPos, cellW, cellH),
        cellW, cellH, blockers);
}

bool Engine::TryFindDungeonEnemySpawnPosition(
    const Enemy* enemy, Vector2 desiredPos, float cellW, float cellH,
    float minPlayerDistance, Vector2& result) const
{
    const std::vector<Rectangle> blockers = GetDungeonSpawnBlockers(cellW, cellH);
    const Vector2 playerPos = _player.GetWorldPos();
    const float minDistanceSq = minPlayerDistance * minPlayerDistance;
    auto valid = [&](Vector2 candidate)
    {
        const float dx = candidate.x - playerPos.x;
        const float dy = candidate.y - playerPos.y;
        return dx * dx + dy * dy >= minDistanceSq &&
               IsDungeonEnemySpawnPositionValid(
                   enemy, candidate, cellW, cellH, blockers);
    };

    if (valid(desiredPos))
    {
        result = desiredPos;
        return true;
    }

    bool found = false;
    float bestDistanceSq = std::numeric_limits<float>::max();
    for (int row = 1; row < RoomLayout::kRows - 1; ++row)
    {
        for (int col = 1; col < RoomLayout::kCols - 1; ++col)
        {
            const Vector2 candidate{
                (col + 0.5f) * cellW,
                (row + 0.5f) * cellH
            };
            if (!valid(candidate)) continue;

            const float dx = candidate.x - desiredPos.x;
            const float dy = candidate.y - desiredPos.y;
            const float distanceSq = dx * dx + dy * dy;
            if (distanceSq < bestDistanceSq)
            {
                bestDistanceSq = distanceSq;
                result = candidate;
                found = true;
            }
        }
    }
    return found;
}

Vector2 Engine::GetDungeonSpawnPos(float cellW, float cellH) const
{
    Vector2 playerPos = _player.GetWorldPos();
    float minDist = cellW * 4.f;   // stay at least 4 cells from the player
    const std::vector<Rectangle> blockers = GetDungeonSpawnBlockers(cellW, cellH);

    for (int attempt = 0; attempt < 40; attempt++)
    {
        int col = GetRandomValue(2, RoomLayout::kCols - 3);
        int row = GetRandomValue(2, RoomLayout::kRows - 3);

        float x = (col + 0.5f) * cellW;
        float y = (row + 0.5f) * cellH;
        if (!IsDungeonEnemySpawnPositionValid(
                nullptr, { x, y }, cellW, cellH, blockers))
            continue;
        float dx = x - playerPos.x;
        float dy = y - playerPos.y;
        if (dx * dx + dy * dy < minDist * minDist) continue;

        return { x, y };
    }

    // Exhaustively find a valid floor instead of returning an unchecked corner.
    const Vector2 preferred{
        (float)kVirtualWidth * 0.75f,
        (float)kVirtualHeight * 0.5f
    };
    Vector2 result{};
    if (TryFindDungeonEnemySpawnPosition(
            nullptr, preferred, cellW, cellH, minDist, result) ||
        TryFindDungeonEnemySpawnPosition(
            nullptr, preferred, cellW, cellH, 0.f, result))
        return result;

    // A valid room always has floor. Keep this deterministic for malformed
    // content; ConfigureSpawnedEnemy will reject the position and deactivate it.
    return preferred;
}

Vector2 Engine::GetDungeonSpawnPosForRole(EnemyRole role, float cellW, float cellH) const
{
    // Grunts/chargers spawn anywhere valid.
    if (role == EnemyRole::Grunt || role == EnemyRole::Charger)
        return GetDungeonSpawnPos(cellW, cellH);

    // For tactical roles, sample several already-valid candidates and pick the one
    // that best matches the role's intent: ranged/support to the back, tanks at
    // mid-range, assassins off to a side. Every candidate comes from
    // GetDungeonSpawnPos, so validity/min-distance are guaranteed regardless.
    Vector2 player    = _player.GetWorldPos();
    Vector2 best      = GetDungeonSpawnPos(cellW, cellH);
    float   bestScore = -1e9f;
    for (int sample = 0; sample < 6; sample++)
    {
        Vector2 candidate = GetDungeonSpawnPos(cellW, cellH);
        float   dist      = Vector2Distance(candidate, player);
        float   score     = 0.f;
        switch (role)
        {
        case EnemyRole::Ranged: case EnemyRole::HeavyRanged: case EnemyRole::Zoner:
        case EnemyRole::Support: case EnemyRole::Summoner:
            score = dist; break;                                 // farthest = back line
        case EnemyRole::Tank:
            score = -fabsf(dist - cellW * 5.f); break;           // hold mid-range
        case EnemyRole::Assassin:
            score = fabsf(candidate.x - player.x)
                    - fabsf(candidate.y - player.y) * 0.5f; break;   // reward off-angle
        default:
            score = 0.f; break;
        }
        if (score > bestScore) { bestScore = score; best = candidate; }
    }
    return best;
}

void Engine::RollRoomAffix(int roomIdx, RoomType type)
{
    _currentRoomAffix = RoomAffix::None;

    // Only Standard combat rooms roll affixes, and only while still uncleared.
    if (type != RoomType::Standard) return;
    auto it = _dungeonRoomStates.find(roomIdx);
    if (it != _dungeonRoomStates.end() && it->second.cleared) return;
    if (GetRandomValue(1, 100) > kRoomAffixChancePercent) return;

    // Weighted pick among the real affixes (index 0 = None is skipped).
    int total = 0;
    for (int i = 1; i < (int)RoomAffix::Count; i++) total += kRoomAffixes[i].rollWeight;
    if (total <= 0) return;

    int roll = GetRandomValue(1, total);
    for (int i = 1; i < (int)RoomAffix::Count; i++)
    {
        roll -= kRoomAffixes[i].rollWeight;
        if (roll <= 0) { _currentRoomAffix = (RoomAffix)i; break; }
    }
    if (_currentRoomAffix != RoomAffix::None)
        _roomAffixBannerTimer = 3.5f;   // brief intro flash on the HUD banner
}

void Engine::DrawRoomAffixBanner()
{
    if (_currentRoomAffix == RoomAffix::None) return;
    if (_roomAffixBannerTimer > 0.f) _roomAffixBannerTimer -= GetFrameTime();

    const RoomAffixDef& a = GetRoomAffixDef(_currentRoomAffix);
    Color accent{ a.r, a.g, a.b, 255 };

    const float sw = (float)kVirtualWidth;
    const char* tag  = "ROOM AFFIX";
    int tagFs = 18, nameFs = 34, descFs = 20;
    int nameW = MeasureText(a.name, nameFs);
    int descW = MeasureText(a.description, descFs);
    float pillW = std::max(nameW, descW) + 90.f;
    float pillH = 104.f;
    float pillX = sw * 0.5f - pillW * 0.5f;
    float pillY = 92.f;

    Rectangle pill{ pillX, pillY, pillW, pillH };
    DrawRectangleRounded(pill, 0.4f, 10, Fade(Color{ 20, 18, 28, 255 }, 0.9f));
    DrawRectangleRoundedLines(pill, 0.4f, 10, accent);

    DrawText(tag,  (int)(sw * 0.5f - MeasureText(tag, tagFs) * 0.5f), (int)(pillY + 12.f), tagFs, Fade(accent, 0.85f));
    DrawText(a.name, (int)(sw * 0.5f - nameW * 0.5f), (int)(pillY + 34.f), nameFs, accent);
    DrawText(a.description, (int)(sw * 0.5f - descW * 0.5f), (int)(pillY + 76.f), descFs, Color{ 212, 212, 224, 230 });
}

// =============================================================================
// Dungeon HUD polish — intro banner, room badge, modifier stack, minimap.
// Rule: normal gameplay HUD only shows exceptions, decisions, danger, or
// actionable info. Standard rooms draw nothing at the top of the screen.
// =============================================================================

void Engine::QueueRoomIntroBanner()
{
    const auto& rooms = _dungeonGen.GetRooms();
    if (_dungeonRoomIdx < 0 || _dungeonRoomIdx >= (int)rooms.size()) return;

    _roomIntroTitle.clear();
    _roomIntroSub.clear();

    const DungeonRoom& room = rooms[_dungeonRoomIdx];
    RoomSpecialType special = RoomSpecialType::None;
    {
        auto it = _dungeonRoomStates.find(_dungeonRoomIdx);
        if (it != _dungeonRoomStates.end()) special = it->second.special;
    }
    if (special == RoomSpecialType::RiskShrine)
    {
        _roomIntroTitle = "RISK SHRINE";
        _roomIntroSub   = "Bind a contract, or walk on";
    }
    else if (_dungeonRoomIdx == _dungeonGen.GetBossIndex())
    {
        _roomIntroTitle = "BOSS ROOM";
        _roomIntroSub   = "No turning back";
    }
    else if (_dungeonRoomIdx == _dungeonGen.GetKeyIndex())
    {
        _roomIntroTitle = "KEY ROOM";
        _roomIntroSub   = "Claim the magic gem";
    }
    else switch (room.type)
    {
    case RoomType::Elite:
    {
        _roomIntroTitle = "ELITE ENCOUNTER";
        if (_eliteMechanic >= 0 && _eliteMechanic < (int)EliteModifier::Count)
            _roomIntroSub = std::string("Condition: ") + EliteModifierName(_eliteMechanic);
        break;
    }
    case RoomType::Treasure:
        _roomIntroTitle = "TREASURE ROOM";
        _roomIntroSub   = "Choose your reward";
        break;
    case RoomType::Rest:
        _roomIntroTitle = "REST ROOM";
        _roomIntroSub   = "A moment to breathe";
        break;
    case RoomType::Store:
        break;   // Zeph's shop has its own intro flow
    default:
        if (room.encounterProfile == EncounterProfile::Holdout)
        {
            _roomIntroTitle = "HOLDOUT";
            _roomIntroSub = "Survive until the enemy force withdraws";
        }
        else if (room.encounterProfile == EncounterProfile::Swarm)
        {
            _roomIntroTitle = "SWARM";
            _roomIntroSub = "Fragile enemies will arrive in waves";
        }
        // Ordinary Standard rooms remain silent unless an affix makes them an exception.
        else if (_currentRoomAffix != RoomAffix::None)
        {
            const RoomAffixDef& a = GetRoomAffixDef(_currentRoomAffix);
            _roomIntroTitle = a.name;
            _roomIntroSub   = a.description;
        }
        break;
    }

    if (!_roomIntroTitle.empty())
        _roomIntroTimer = kRoomIntroDuration;
}

void Engine::DrawRoomIntroBanner()
{
    if (_roomIntroTimer <= 0.f) return;
    _roomIntroTimer -= GetFrameTime();

    const float sw = (float)kVirtualWidth;
    float elapsed = kRoomIntroDuration - _roomIntroTimer;

    // Fade in over 0.25s, hold, fade out over the last 0.5s.
    float alpha = 1.f;
    if (elapsed < 0.25f)            alpha = elapsed / 0.25f;
    if (_roomIntroTimer < 0.5f)     alpha = _roomIntroTimer / 0.5f;
    // Slight settle: the banner drifts up a few px as it appears.
    float rise = (elapsed < 0.25f) ? (1.f - elapsed / 0.25f) * 10.f : 0.f;

    int   titleFs = 54, subFs = 26;
    int   titleW  = MeasureText(_roomIntroTitle.c_str(), titleFs);
    float bandY   = 110.f + rise;

    // Soft backing band so the text reads over any biome.
    float bandH = _roomIntroSub.empty() ? 84.f : 118.f;
    DrawRectangleGradientH((int)(sw * 0.20f), (int)(bandY - 14.f), (int)(sw * 0.30f), (int)bandH,
                           Fade(BLACK, 0.f), Fade(BLACK, 0.55f * alpha));
    DrawRectangleGradientH((int)(sw * 0.50f), (int)(bandY - 14.f), (int)(sw * 0.30f), (int)bandH,
                           Fade(BLACK, 0.55f * alpha), Fade(BLACK, 0.f));

    DrawText(_roomIntroTitle.c_str(), (int)(sw * 0.5f - titleW * 0.5f), (int)bandY,
             titleFs, Fade(GOLD, alpha));
    if (!_roomIntroSub.empty())
    {
        int subW = MeasureText(_roomIntroSub.c_str(), subFs);
        DrawText(_roomIntroSub.c_str(), (int)(sw * 0.5f - subW * 0.5f), (int)(bandY + 64.f),
                 subFs, Fade(Color{ 225, 225, 235, 255 }, alpha));
    }
}

void Engine::DrawRoomBadge() const
{
    if (_roomIntroTimer > 0.f) return;   // never double up with the intro banner

    const auto& rooms = _dungeonGen.GetRooms();
    if (_dungeonRoomIdx < 0 || _dungeonRoomIdx >= (int)rooms.size()) return;

    auto stateIt = _dungeonRoomStates.find(_dungeonRoomIdx);
    bool cleared = (stateIt != _dungeonRoomStates.end()) && stateIt->second.cleared;

    // Only rooms with a live objective/identity keep a badge after the intro.
    std::string label; Color accent{ 255, 205, 100, 255 };
    if (_dungeonRoomIdx == _dungeonGen.GetBossIndex() && !cleared)
    {
        label = "BOSS"; accent = Color{ 235, 80, 80, 255 };
    }
    else if (_dungeonRoomIdx == _dungeonGen.GetKeyIndex() && !_magicGemCollected)
    {
        label = "KEY ROOM"; accent = Color{ 190, 130, 255, 255 };
    }
    else if (rooms[_dungeonRoomIdx].type == RoomType::Elite && !cleared)
    {
        label = (_eliteMechanic >= 0 && _eliteMechanic < (int)EliteModifier::Count)
              ? std::string("ELITE - ") + EliteModifierShortName(_eliteMechanic) : "ELITE";
        accent = Color{ 255, 150, 70, 255 };
    }
    else if (rooms[_dungeonRoomIdx].type == RoomType::Treasure && _treasureChestSpawned && !_treasureChestBroken)
    {
        label = "TREASURE"; accent = GOLD;
    }
    if (label.empty()) return;

    const float sw = (float)kVirtualWidth;
    int   fs = 20;
    int   tw = MeasureText(label.c_str(), fs);
    float pillW = tw + 36.f, pillH = 34.f;
    Rectangle pill{ sw * 0.5f - pillW * 0.5f, 10.f, pillW, pillH };
    DrawRectangleRounded(pill, 0.5f, 8, Fade(Color{ 18, 16, 26, 255 }, 0.85f));
    DrawRectangleRoundedLines(pill, 0.5f, 8, Fade(accent, 0.9f));
    DrawText(label.c_str(), (int)(pill.x + 18.f), (int)(pill.y + 7.f), fs, accent);
}

void Engine::DrawModifierStack() const
{
    // Collect every active run/room modifier into compact pills. Nothing
    // active = nothing drawn. New systems (curses, contracts, omens) add
    // entries here so danger info lives in ONE place.
    struct ModifierPill { std::string text; Color accent; };
    std::vector<ModifierPill> pills;

    if (_currentRoomAffix != RoomAffix::None)
    {
        const RoomAffixDef& a = GetRoomAffixDef(_currentRoomAffix);
        pills.push_back({ a.name, Color{ a.r, a.g, a.b, 255 } });
    }
    if (_wagerTier > 0)
    {
        const char* tiers[3] = { "WAGER I", "WAGER II", "WAGER III" };
        pills.push_back({ tiers[std::min(_wagerTier - 1, 2)], Color{ 235, 120, 90, 255 } });
    }
    // Risk Shrine contracts — armed (waiting for the next combat room) or active.
    for (const RunModifier& m : _runModifiers)
        pills.push_back({ m.active ? m.label : (m.label + " (armed)"), m.tint });
    if (pills.empty()) return;

    // Below the corner minimap, right-aligned.
    const float sw = (float)kVirtualWidth;
    const float mapH = DungeonGen::kGridSize * 30.f + 30.f;   // matches minimap footprint
    float y = 8.f + mapH + 10.f;
    for (const ModifierPill& pill : pills)
    {
        int   fs = 18;
        int   tw = MeasureText(pill.text.c_str(), fs);
        float pw = tw + 28.f, ph = 30.f;
        Rectangle rect{ sw - 10.f - pw, y, pw, ph };
        DrawRectangleRounded(rect, 0.5f, 8, Fade(Color{ 18, 16, 26, 255 }, 0.8f));
        DrawRectangleRoundedLines(rect, 0.5f, 8, Fade(pill.accent, 0.85f));
        DrawText(pill.text.c_str(), (int)(rect.x + 14.f), (int)(rect.y + 6.f), fs, pill.accent);
        y += ph + 8.f;
    }
}

void Engine::DrawDungeonMiniMap(float originX, float originY, float cellPx, bool overlay) const
{
    const auto& mmRooms = _dungeonGen.GetRooms();
    if (mmRooms.empty()) return;

    const float pad = cellPx * 0.5f;
    const float sq  = cellPx * 0.62f;
    const float mmW = DungeonGen::kGridSize * cellPx + pad * 2.f;

    DrawRectangleRounded({ originX, originY, mmW, mmW }, 0.08f, 4,
                         Fade(Color{ 25, 25, 35, 255 }, overlay ? 0.92f : 0.61f));
    DrawRectangleRoundedLines({ originX, originY, mmW, mmW }, 0.08f, 4,
                              Color{ 200, 200, 220, 45 });

    int  bossIdx  = _dungeonGen.GetBossIndex();
    int  startIdx = _dungeonGen.GetStartIndex();
    int  keyIdx   = _dungeonGen.GetKeyIndex();
    bool cartographer = _meta.HasCartographer();
    float pulse = sinf((float)GetTime() * (2.f * PI / 3.f)) * 0.5f + 0.5f;

    // Connections behind the rooms.
    for (const auto& room : mmRooms)
    {
        float cx = originX + pad + (room.col + 0.5f) * cellPx;
        float cy = originY + pad + (room.row + 0.5f) * cellPx;
        if (room.hasEast)
            DrawLineEx({ cx, cy }, { cx + cellPx, cy }, 1.4f, Color{ 185, 185, 205, 65 });
        if (room.hasSouth)
            DrawLineEx({ cx, cy }, { cx, cy + cellPx }, 1.4f, Color{ 185, 185, 205, 65 });
    }

    for (int i = 0; i < (int)mmRooms.size(); i++)
    {
        const DungeonRoom& room = mmRooms[i];
        float rx = originX + pad + room.col * cellPx + (cellPx - sq) * 0.5f;
        float ry = originY + pad + room.row * cellPx + (cellPx - sq) * 0.5f;
        Rectangle cellRect{ rx, ry, sq, sq };

        auto stateIt  = _dungeonRoomStates.find(i);
        bool visited  = (stateIt != _dungeonRoomStates.end()) && stateIt->second.visited;
        bool cleared  = (stateIt != _dungeonRoomStates.end()) && stateIt->second.cleared;
        // Boss and shop are structural knowledge — always shown. Everything
        // else is revealed by visiting (the map remembers) or Cartographer's Echo.
        bool typeKnown = visited || cartographer || i == _dungeonRoomIdx ||
                         i == bossIdx || i == startIdx;

        // Base square (dimmer once cleared — spent rooms fade into the background).
        Color base = cleared ? Color{ 150, 150, 165, 70 } : Color{ 210, 210, 225, 115 };
        DrawRectangleRounded(cellRect, 0.3f, 4, base);

        // Room content: icon for known special rooms, "?" for unknown ones.
        const Texture2D* icon = nullptr;
        if (!typeKnown)
        {
            int qFs = (int)(cellPx * 0.55f);
            DrawText("?", (int)(rx + sq * 0.5f - MeasureText("?", qFs) * 0.5f),
                     (int)(ry + sq * 0.5f - qFs * 0.5f), qFs, Color{ 235, 220, 160, 200 });
        }
        else if (i == bossIdx)                          icon = &_mapIconBoss;
        else if (i == startIdx)                         icon = nullptr;
        else if (room.type == RoomType::Elite)          icon = &_mapIconElite;
        else if (room.type == RoomType::Treasure)       icon = &_mapIconTreasure;
        else if (room.type == RoomType::Rest)           icon = &_mapIconRest;
        else if (room.type == RoomType::Store)          icon = &_mapIconShop;
        // Standard rooms stay a calm plain square — no icon noise.

        if (icon && icon->id != 0)
        {
            float iconSize = cellPx * 0.82f;
            Rectangle src{ 0.f, 0.f, (float)icon->width, (float)icon->height };
            Rectangle dst{ rx + sq * 0.5f - iconSize * 0.5f, ry + sq * 0.5f - iconSize * 0.5f,
                           iconSize, iconSize };
            DrawTexturePro(*icon, src, dst, Vector2{ 0.f, 0.f }, 0.f, Fade(WHITE, 0.95f));
        }

        // Decision room (Risk Shrine, …): amber "!" once the cell is known
        // (undiscovered ones just show the generic "?" above — mystery intact).
        {
            auto ds = _dungeonRoomStates.find(i);
            if (typeKnown && ds != _dungeonRoomStates.end() &&
                ds->second.special != RoomSpecialType::None)
            {
                int dFs = (int)(cellPx * 0.55f);
                Color mc = ds->second.specialClaimed ? Color{ 150, 130, 90, 220 }
                                                      : Color{ 255, 190, 90, 255 };
                DrawText("!", (int)(rx + sq * 0.5f - MeasureText("!", dFs) * 0.5f),
                         (int)(ry + sq * 0.5f - dFs * 0.5f), dFs, mc);
            }
        }

        // Key room marker (once known): a gold K over the square.
        if (typeKnown && i == keyIdx && !_magicGemCollected)
        {
            int kFs = (int)(cellPx * 0.5f);
            DrawText("K", (int)(rx + sq * 0.5f - MeasureText("K", kFs) * 0.5f),
                     (int)(ry + sq * 0.5f - kFs * 0.5f), kFs, Color{ 190, 130, 255, 255 });
        }

        // Current room: pulsing gold outline (the icon stays visible underneath).
        if (i == _dungeonRoomIdx)
        {
            Rectangle outline{ rx - 3.f, ry - 3.f, sq + 6.f, sq + 6.f };
            DrawRectangleRoundedLines(outline, 0.3f, 4,
                Fade(GOLD, 0.55f + 0.45f * pulse));
        }
    }

    if (overlay)
    {
        const char* title = "DUNGEON MAP";
        DrawText(title, (int)(originX + mmW * 0.5f - MeasureText(title, 30) * 0.5f),
                 (int)(originY - 46.f), 30, GOLD);
        const char* legend = _meta.HasCartographer()
            ? "Cartographer's Echo: all rooms revealed"
            : "? = unexplored    visit rooms to reveal them";
        DrawText(legend, (int)(originX + mmW * 0.5f - MeasureText(legend, 20) * 0.5f),
                 (int)(originY + mmW + 12.f), 20, Color{ 200, 200, 215, 220 });
    }
}

// Spawn one grunt from the mixed-type table by typeId. Shared by the room
// opening and by reinforcement waves so both paths place roles identically.
// typeIds: 0 shadow, 1 archer, 2 slime, 3 wisp, 4 sporeling, 5 shieldbearer,
//          6 phantom, 7 bomber, 8 warchief, 9 blade
Enemy* Engine::SpawnDungeonGrunt(const EncounterSpawnEntry& entry, Vector2 pos, float cellW, float cellH)
{
    const int typeId = EncounterTypeId(entry.kind);
    Enemy* spawned = nullptr;
    switch (typeId)
    {
    case 1: spawned = SpawnSkeletonArcher(pos);        break;
    case 2: spawned = SpawnSlime(pos, SlimeSize::Big); break;
    case 3: spawned = SpawnFlameWisp(pos);             break;
    case 4: spawned = SpawnSporeling(pos);             break;
    case 5: spawned = SpawnShieldbearer(pos);          break;
    case 6: spawned = SpawnPhantom(pos);               break;
    case 7: spawned = SpawnBomberImp(pos);             break;
    case 8: spawned = SpawnWarchief(pos);              break;
    case 9: spawned = SpawnLivingBlade(pos);           break;
    default: spawned = SpawnBasicEnemy(pos);           break;
    }

    // Role-based placement: nudge specialised roles to sensible spots
    // (ranged/support to the back, tanks mid, assassins off-angle). The
    // target is always a valid GetDungeonSpawnPos candidate, so this can't
    // put an enemy in a wall. Grunts keep their original spread.
    if (spawned != nullptr)
    {
        EnemyRole role = spawned->GetEncounterRole();
        if (role != EnemyRole::Grunt && role != EnemyRole::Charger)
        {
            const Vector2 rolePos = GetDungeonSpawnPosForRole(role, cellW, cellH);
            Vector2 safeRolePos{};
            if (TryFindDungeonEnemySpawnPosition(
                    spawned, rolePos, cellW, cellH, 0.f, safeRolePos))
                spawned->Teleport(safeRolePos);
        }
        if (entry.swarmProfile)
            spawned->ApplyDifficultyScaling(0.75f, 0.85f);
    }
    return spawned;
}

// Purple-gray tint shared by the Circle-phase warning shapes
// (DrawPendingEnemySpawns) and the Circle->Smoke transition puff
// (AdvanceDungeonPendingSpawns's SpawnSmokeBurst call below) so the whole
// reinforcement telegraph reads as one consistent motif (design: "a filled
// translucent purple raylib circle" / "existing smoke particles burst at the
// position").
constexpr Color kSpawnTelegraphColor{ 170, 110, 210, 255 };

// Revalidates a reserved reinforcement position immediately before
// instantiation, using the exact same blocker + minimum-player-distance
// checks GetDungeonSpawnPos already applies at reservation time (design:
// "Reserved positions observe existing blocker checks and minimum player
// distance" / "A reserved spawn is revalidated immediately before
// instantiation. If invalid, it returns to the reinforcement queue.").
bool Engine::IsDungeonReinforcementPosStillValid(Vector2 pos, float cellW, float cellH) const
{
    const std::vector<Rectangle> blockers = GetDungeonSpawnBlockers(cellW, cellH);
    const Vector2 playerPos = _player.GetWorldPos();
    const float minDist = cellW * 4.f;   // matches GetDungeonSpawnPos's own minimum distance
    const float dx = pos.x - playerPos.x;
    const float dy = pos.y - playerPos.y;
    if (dx * dx + dy * dy < minDist * minDist) return false;
    return IsDungeonEnemySpawnPositionValid(nullptr, pos, cellW, cellH, blockers);
}

// Reserves 1-2 new pending reinforcement spawns from the front of
// _dungeonReinforcements when the room is below its simultaneous body
// target (ReinforcementPacing::SimultaneousBodyTarget) and no telegraph
// batch is already pending (design: "A new batch begins only when the
// active body count is below the room's simultaneous target and no spawn
// telegraph batch is already pending" / "Each batch reserves valid spawn
// positions before its telegraph begins"). Never calls SpawnDungeonGrunt —
// entries only instantiate once their telegraph completes (see
// AdvanceDungeonPendingSpawns).
void Engine::TryReserveDungeonReinforcementBatch(float cellW, float cellH)
{
    if (_dungeonReinforcements.empty()) return;
    if (!_pendingEnemySpawns.empty()) return;   // a batch's telegraph is already in flight

    const int target = SimultaneousBodyTarget(_roomCombatCapacity.band);
    const int active = GetActiveEnemyCount();
    const int batchSize = ReinforcementBatchSize(
        active, (int)_pendingEnemySpawns.size(), target, (int)_dungeonReinforcements.size());
    if (batchSize <= 0) return;

    for (int n = 0; n < batchSize; ++n)
    {
        const EncounterSpawnEntry entry = _dungeonReinforcements.front();
        _dungeonReinforcements.pop_front();

        PendingEnemySpawn spawn{};
        spawn.entry = entry;
        spawn.worldPos = GetDungeonSpawnPosForRole(entry.role, cellW, cellH);
        spawn.phase = PendingSpawnPhase::Circle;
        spawn.timer = 0.f;
        spawn.smokeEmitted = false;
        _pendingEnemySpawns.push_back(spawn);
    }
}

// Ticks every pending reinforcement's telegraph timer (Circle -> Smoke ->
// Ready). Entries that reach Ready are revalidated against the same
// blocker/min-distance checks used at reservation time: if still valid they
// are instantiated via SpawnDungeonGrunt and removed from the pending
// collection; if now invalid they are returned to the FRONT of the
// reinforcement queue and dropped from pending (design: "Failed reservations
// stay queued and retry later; they do not spawn at an unsafe fallback").
void Engine::AdvanceDungeonPendingSpawns(float dt, float cellW, float cellH)
{
    for (size_t i = 0; i < _pendingEnemySpawns.size(); )
    {
        PendingEnemySpawn& spawn = _pendingEnemySpawns[i];
        const bool wasEmitted = spawn.smokeEmitted;
        AdvancePendingSpawn(spawn, dt);

        // Fire the smoke puff exactly once, right on the Circle->Smoke
        // transition frame (smokeEmitted flips false->true exactly once —
        // see ReinforcementPacing.h). Guarding on the flip itself (not on
        // phase==Smoke) means a large single-frame dt that walks the whole
        // state machine through in one call still only emits once.
        if (!wasEmitted && spawn.smokeEmitted)
            _vfx.SpawnSmokeBurst(spawn.worldPos, kSpawnTelegraphColor);

        if (spawn.phase != PendingSpawnPhase::Ready)
        {
            ++i;
            continue;
        }

        if (IsDungeonReinforcementPosStillValid(spawn.worldPos, cellW, cellH))
        {
            Enemy* spawned = SpawnDungeonGrunt(spawn.entry, spawn.worldPos, cellW, cellH);
            // Short arrival/orientation delay: the enemy exists but may not
            // move or attack for a brief beat after the telegraph resolves
            // (design: "State and Failure Handling" / "Spawn Telegraph and VFX").
            if (spawned != nullptr)
                spawned->BeginArrivalDelay(Balance::Rhythm::kSpawnArrivalDelaySeconds);
        }
        else
            _dungeonReinforcements.push_front(spawn.entry);

        _pendingEnemySpawns.erase(_pendingEnemySpawns.begin() + (long)i);
    }
}

// Draws every pending reinforcement's telegraph. Deterministic on
// spawn.timer (never GetTime()/wall-clock), so hit-stop/slow-mo/pause freeze
// this exactly like every other piece of UpdateDungeonRun. worldOffset is
// taken as a parameter rather than recomputed, matching the sibling
// _vfx.Draw(worldOffset, ...)/proj.Draw(worldOffset) calls in the same
// dungeon draw block this is called from.
//   Circle phase:  pulsing filled circle + two expanding/fading outline
//                  rings (design: "a filled translucent purple raylib circle
//                  pulses at the exact spawn position, with one or two
//                  expanding outline rings").
//   Smoke phase:   the circle contracts and brightens ahead of the spawn
//                  (design: "the circle contracts and brightens").
//   Ready phase:   nothing to draw — AdvanceDungeonPendingSpawns already
//                  removes Ready entries from _pendingEnemySpawns the same
//                  frame they resolve, so this is a defensive no-op, not the
//                  expected path.
void Engine::DrawPendingEnemySpawns(Vector2 worldOffset) const
{
    for (const auto& spawn : _pendingEnemySpawns)
    {
        if (spawn.phase == PendingSpawnPhase::Ready)
            continue;

        Vector2 screenPos = Vector2Add(spawn.worldPos, worldOffset);
        screenPos.x += kVirtualWidth  / 2.f;
        screenPos.y += kVirtualHeight / 2.f;

        if (spawn.phase == PendingSpawnPhase::Circle)
        {
            const float elapsed = spawn.timer;   // 0 .. kSpawnCircleSeconds

            // Filled circle gently breathes in size/alpha.
            const float breathe = 0.5f + 0.5f * sinf(elapsed * 6.5f);
            DrawCircleV(screenPos, 16.f + 4.f * breathe,
                        Fade(kSpawnTelegraphColor, 0.28f + 0.18f * breathe));

            // Two expanding, fading outline rings, offset half a cycle apart
            // so a new ring is always mid-expansion (a continuous pulse
            // rather than a single repeating ring).
            constexpr float kRingPeriod    = 0.45f;
            constexpr float kRingMaxRadius = 46.f;
            for (int ring = 0; ring < 2; ++ring)
            {
                const float phaseOffset = (ring == 0) ? 0.f : kRingPeriod * 0.5f;
                const float ringT = fmodf(elapsed + phaseOffset, kRingPeriod) / kRingPeriod;
                const float ringRadius = ringT * kRingMaxRadius;
                const float ringAlpha  = 1.f - ringT;
                DrawCircleLinesV(screenPos, ringRadius,
                                 Fade(kSpawnTelegraphColor, ringAlpha * 0.85f));
            }
        }
        else   // Smoke phase
        {
            const float smokeElapsed = spawn.timer - Balance::Rhythm::kSpawnCircleSeconds;
            const float t = std::clamp(smokeElapsed / Balance::Rhythm::kSpawnSmokeSeconds, 0.f, 1.f);
            const float radius = 20.f - 14.f * t;                 // contracts toward a point
            const Color color  = Fade(kSpawnTelegraphColor, 0.4f + 0.6f * t);  // brightens
            DrawCircleV(screenPos, radius, color);
        }
    }
}

void Engine::SpawnDungeonRoomEnemies()
{
    if (_dungeonRoomIdx < 0) return;

    DungeonRoomState& roomState = _dungeonRoomStates[_dungeonRoomIdx];

    // This is a map-editor playtest preference, not a room-clear action. Keep
    // the guard at the shared spawn boundary so room transitions and delayed
    // encounter setup cannot silently turn enemies back on. Normal runs never
    // enter this branch because _editorPlaytestActive is false.
    if (_editorPlaytestActive && !_editorPlaytestEnemiesOn)
    {
        _dungeonEnemiesSpawned = false;
        _dungeonReinforcements.clear();
        _pendingEnemySpawns.clear();
        ApplyDungeonRoomDoorState(
            _dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
        return;
    }

    // Rooms that have already been cleared don't respawn enemies — but lava
    // stays: terrain doesn't despawn just because the fight is over. Turret
    // hazards stay quiet in cleared rooms (backtracking shouldn't be shot at).
    if (roomState.cleared)
    {
        _dungeonEnemiesSpawned = false;
        RestoreDungeonRoomHazardState(_dungeonRoomIdx, /*lavaOnly=*/true);
        return;
    }

    // Re-entering a half-cleared room restores only the enemies that were
    // still alive, plus every hazard that wasn't destroyed.
    if (roomState.enemiesInitialized)
    {
        RestoreDungeonRoomEnemyState(_dungeonRoomIdx);
        RestoreDungeonRoomHazardState(_dungeonRoomIdx, /*lavaOnly=*/false);
        return;
    }

    float sw    = (float)kVirtualWidth;
    float sh    = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;

    const auto& rooms = _dungeonGen.GetRooms();
    if (_dungeonRoomIdx >= (int)rooms.size()) return;

    int i       = _dungeonRoomIdx;
    int startIdx = _dungeonGen.GetStartIndex();
    int bossIdx  = _dungeonGen.GetBossIndex();
    RoomType type = rooms[i].type;

    if (rooms[i].encounterProfile == EncounterProfile::Holdout)
    {
        if (roomState.holdoutTimeRemaining < 0.f)
            roomState.holdoutTimeRemaining = 30.f;
        _roomObjectiveTimer = roomState.holdoutTimeRemaining;
        _roomObjectiveComplete = _roomObjectiveTimer <= 0.f;
    }

    if (_prologueActive)
    {
        _roomHazards.ClearRoom();
        _dungeonReinforcements.clear();
        _pendingEnemySpawns.clear();
        if (i < 2)
        {
            const int count = (i == 0) ? 2 : 4;
            for (int n = 0; n < count; ++n)
            {
                float y = sh * (0.34f + 0.14f * (float)(n % 3));
                SpawnBasicEnemy({ sw * (0.62f + 0.08f * (float)(n / 3)), y });
            }
        }
        else
        {
            constexpr int kLastStandArchers = 14;
            Vector2 center{ sw * 0.5f, sh * 0.5f };
            for (int n = 0; n < kLastStandArchers; ++n)
            {
                float angle = (2.f * PI * n) / (float)kLastStandArchers;
                Vector2 position{
                    center.x + cosf(angle) * sw * 0.40f,
                    center.y + sinf(angle) * sh * 0.34f
                };
                Enemy* spawned = SpawnSkeletonArcher(position);
                SkeletonArcher* archer = spawned != nullptr ? spawned->AsSkeletonArcher() : nullptr;
                if (archer != nullptr) archer->EnableRelentlessFire();
            }
        }
        roomState.enemiesInitialized = true;
        _dungeonEnemiesSpawned = true;
        roomState.hazardsInitialized = true;
        return;
    }

    // An entrance that rolled empty is pre-cleared and remains peaceful.
    if ((i == startIdx && rooms[i].startsEmpty && !_prologueActive) || type == RoomType::Rest)
    {
        roomState.cleared = true;
        roomState.enemiesInitialized = true;
        roomState.survivors.clear();
        return;
    }

    auto spawnAt = [&](auto spawnFn, int count = 1) {
        for (int n = 0; n < count; n++)
            spawnFn(GetDungeonSpawnPos(cellW, cellH));
    };

    // Count cleared rooms as a progression tier (0 = early, 1 = mid, 2 = late).
    int clearedRooms = 0;
    for (const auto& [idx, state] : _dungeonRoomStates)
        if (state.cleared) clearedRooms++;
    int tier = (clearedRooms <= 2) ? 0 : (clearedRooms <= 5) ? 1 : 2;
    _roomEncounterTier = tier;

    if (i == bossIdx)
    {
        // Spawn the boss at the centre-top of the room so it's always in bounds
        // regardless of player entry point. Avoids wall-clip from random placement.
        // The boss class is chosen per biome (Molarbeast, Abyss Slime,
        // Pumpkin Jack, or Minotaur).
        float bossX = sw * 0.5f;
        float bossY = sh * 0.28f;
        SpawnBossForBiome({ bossX, bossY });
        // Boss rooms retain crowd pressure, but use mostly cheap adds so the
        // boss pattern remains the encounter's readable centrepiece.
        const int addCount = GetRandomValue(4 + tier * 2, 6 + tier * 2);
        const int heavyAdds = tier >= 1 ? 1 : 0;
        spawnAt([&](Vector2 p){ SpawnBasicEnemy(p); }, addCount - heavyAdds);
        if (heavyAdds > 0)
            spawnAt([&](Vector2 p){ SpawnCyclops(p); }, heavyAdds);
    }
    else if (type == RoomType::Treasure)
    {
        // Small combat encounter - players must fight before the chest appears.
        spawnAt([&](Vector2 p){ SpawnBasicEnemy(p); }, GetRandomValue(1, tier == 0 ? 2 : 3));
        if (tier >= 1 || GetRandomValue(0, 1) == 0)
            SpawnCyclops(GetDungeonSpawnPos(cellW, cellH));
    }
    else if (type == RoomType::Elite)
    {
        // Spawn the miniboss (a curated bruiser: Ogre / Infernal / ...) plus its
        // guards, then run the ONE shared elite-room setup path.
        Enemy* miniboss = SpawnEliteMiniboss(GetDungeonSpawnPos(cellW, cellH));
        spawnAt([&](Vector2 p){ SpawnBasicEnemy(p); }, tier == 0 ? 1 : 2);
        InitializeEliteRoomRuntime(miniboss, i, sw, sh);
    }
    else  // Standard
    {
        // Roguelite pacing: rooms get genuinely crowded as the run deepens.
        // Body counts + the pressure budget live in Balance::Pressure; anything
        // beyond the opening cap arrives later as reinforcement waves.
        const int tierIdx = std::clamp(tier, 0, 2);

        // Later world zones add one extra body on top of the room-tier count.
        const int zoneBonus = _worldZone / 2;   // zones 0-1 = 0, 2-3 = +1, 4-5 = +2

        // Demon Insides: the gauntlet biome — significantly harder rooms.
        // Each grunt slot rolls its enemy type from a weighted table. Early
        // rooms lean on the familiar shadow grunt; later rooms mix in every
        // specialised type. The Warchief is rare and capped at one per room.
        // ── Environmental hazard roll (first slice of Phase 6 integration) ───
        // Standard combat rooms only, frequency by tier. The hazard's pressure
        // cost comes straight out of the enemy budget below, so hazard rooms
        // field fewer bodies. Lava fits the hot/underground biomes; totems go
        // anywhere; torches mount on the east/west walls outside the door band.
        {
            int frequencyRoll = GetRandomValue(0, 999);
            if (frequencyRoll < (int)(Balance::Hazards::kRoomFrequencyByTier[tierIdx] * 1000.f))
            {
                std::vector<Vector2> forbiddenSpots = {
                    { sw * 0.5f, cellH },        // north door
                    { sw * 0.5f, sh - cellH },   // south door
                    { cellW, sh * 0.5f },        // west door
                    { sw - cellW, sh * 0.5f },   // east door
                    _player.GetWorldPos()        // entry position
                };
                // Lava belongs where the ground already feels dangerous —
                // Lava pools are authored by hand now (Fall zones + an animated
                // lava decor you can fall into), so the procedural hazard director
                // only rolls the two turret types. RoomHazardType::LavaPool support
                // is kept intact for the debug preview and any hand-placed use.
                int typeRoll = GetRandomValue(1, 100);
                RoomHazardType hazardType = (typeRoll <= 60) ? RoomHazardType::FireTotem
                                                             : RoomHazardType::FireballTorch;

                bool placed = false;
                if (hazardType == RoomHazardType::FireballTorch)
                {
                    bool onWestWall = GetRandomValue(0, 1) == 0;
                    float laneOffset = Balance::Hazards::kTorchDoorBandHalf + (float)GetRandomValue(0, 160);
                    float laneY = sh * 0.5f + ((GetRandomValue(0, 1) == 0) ? -laneOffset : laneOffset);
                    Vector2 torchPos{ onWestWall ? cellW * 2.f : sw - cellW * 2.f, laneY };
                    placed = _roomHazards.TryPlaceWallTorch(torchPos, { onWestWall ? 1.f : -1.f, 0.f },
                                                            forbiddenSpots, Balance::Hazards::kMinDistFromDoorway);
                }
                else
                {
                    Rectangle roomBounds{ 0.f, 0.f, sw, sh };
                    for (int attempt = 0; attempt < 12 && !placed; attempt++)
                        placed = _roomHazards.TryPlaceHazard(hazardType, GetDungeonSpawnPos(cellW, cellH),
                                                             forbiddenSpots, Balance::Hazards::kMinDistFromEntry,
                                                             roomBounds);
                }
                TraceLog(LOG_INFO, "HAZARD roll: type %d %s (biome %d, tier %d)",
                         (int)hazardType, placed ? "PLACED" : "failed all attempts",
                         (int)_currentBiome, tierIdx);
            }
        }

        // ── Pressure budget (Balance::Pressure) ──────────────────────────────
        // Roll the whole composition first, costing each body by how much
        // simultaneous danger it adds. When a roll would bust the cap it
        // downgrades to a plain grunt (cost 1) or stops. Cost per typeId:
        // grunts/sporelings 1, specialists (ranged/tank/assassin/zoner) 2,
        // Warchief (support elite) 3 — mirrors the EnemyRole cost model.
        // Hazards spend from the same budget (fewer bodies in hazard rooms).
        // Opening slice vs reinforcement waves: never open the fight with more
        // bodies than the tier's readable cap — the surplus streams in later.
        EncounterRequest encounterRequest{};
        encounterRequest.tier = tierIdx;
        encounterRequest.seed = static_cast<std::uint32_t>(GetRandomValue(1, 0x7ffffffe));
        encounterRequest.hazardPressure = _roomHazards.TotalPressureCost();
        encounterRequest.populationBonus = zoneBonus + (_currentBiome == Biome::DemonsInsides ? 2 : 0);
        encounterRequest.profile = rooms[i].encounterProfile;
        encounterRequest.capacity = _roomCombatCapacity;
        encounterRequest.learnedAbilityCount = _player.GetLearnedCount();
        encounterRequest.swarm = _currentRoomAffix == RoomAffix::Swarm ||
                                 rooms[i].encounterProfile == EncounterProfile::Swarm;
        EncounterPlan encounter = EncounterPlanner::Build(encounterRequest);
        _roomPressureSpent = encounter.debug.totalPressure;
        _roomPressureCapDbg = encounter.debug.pressureCap;
        _dungeonOpeningCap = encounter.debug.openingBodyCap;
        const int openingCount = static_cast<int>(encounter.opening.size());
        // Task 4 (Reinforcement Pacing): never open the fight with more bodies
        // on screen than the room's simultaneous target, even when the
        // planner's own opening slice is larger — anything bumped out streams
        // in later as reinforcements (through the normal telegraphed
        // reservation path) alongside whatever the planner already queued.
        // Total planned population is unchanged: nothing here is dropped,
        // only deferred (openingCount + reinforcements.size() stays constant).
        const int immediateOpeningCount = std::min(openingCount, SimultaneousBodyTarget(_roomCombatCapacity.band));
        std::deque<EncounterSpawnEntry> overflowReinforcements(
            encounter.opening.begin() + immediateOpeningCount, encounter.opening.end());
        overflowReinforcements.insert(overflowReinforcements.end(),
            encounter.reinforcements.begin(), encounter.reinforcements.end());
        _dungeonReinforcements = std::move(overflowReinforcements);
        _dungeonReinforcementTimer = Balance::Pressure::kReinforceInterval;
        if (_debug.IsActive())
            TraceLog(LOG_INFO, "ROOM PRESSURE tier %d: %d/%d (bodies %d, opening %d->%d, reinforcements %d)",
                     tierIdx, encounter.debug.totalPressure, encounter.debug.pressureCap,
                     encounter.debug.plannedPopulation, openingCount, immediateOpeningCount,
                     (int)_dungeonReinforcements.size());

        // Ancient Castle: spawn in a tight cluster so enemies charge together.
        if (_currentBiome == Biome::AncientCastle && immediateOpeningCount > 1)
        {
            Vector2 clusterCenter = GetDungeonSpawnPos(cellW, cellH);
            SpawnDungeonGrunt(encounter.opening[0], clusterCenter, cellW, cellH);
            for (int n = 1; n < immediateOpeningCount; n++)
            {
                float   angle = (float)GetRandomValue(0, 628) / 100.f;
                float   dist  = (float)GetRandomValue(30, 100);
                Vector2 clusterPos{
                    std::clamp(clusterCenter.x + cosf(angle) * dist, 80.f, sw - 80.f),
                    std::clamp(clusterCenter.y + sinf(angle) * dist, 80.f, sh - 80.f)
                };
                SpawnDungeonGrunt(encounter.opening[n], clusterPos, cellW, cellH);
            }
        }
        else
        {
            for (int n = 0; n < immediateOpeningCount; n++)
                SpawnDungeonGrunt(encounter.opening[n],
                                  GetDungeonSpawnPos(cellW, cellH), cellW, cellH);
        }

        // Heavy enemies are selected by EncounterPlanner. Spawning an extra
        // Cyclops here bypassed the room's geometry and specialist caps.
    }

    // Graveyard: every non-boss enemy gets a one-time revive.
    if (_currentBiome == Biome::Graveyard)
    {
        for (auto& enemy : _enemies)
        {
            if (!enemy->IsActive() || enemy->IsBoss()) continue;
            enemy->SetGraveReviveAvailable(true);
        }
    }

    // Zeph Risk Bargains land on the first COMBAT room that actually spawns
    // after purchase (so a rest room between doesn't dodge the debt). Pressure
    // debt = one extra enemy + 10% faster attacks for everyone in the room;
    // the wager additionally flags the room for a payout on clear.
    _shopWagerRoomActive = _player.ConsumeShopWager();
    if (_player.ConsumeShopPressureDebt())
    {
        SpawnBasicEnemy(GetDungeonSpawnPos(cellW, cellH));
        for (auto& enemy : _enemies)
            if (enemy->IsActive())
                enemy->QuickenAttacks(0.9f);
    }

    roomState.enemiesInitialized = true;
    _dungeonEnemiesSpawned = true;
    SaveDungeonRoomEnemyState();
    SaveDungeonRoomHazardState();
    roomState.hazardsInitialized = true;
}

// ── Hazard persistence ────────────────────────────────────────────────────────
// Snapshot the current room's hazards (position, health, destroyed flag) so
// leaving and re-entering a room brings back the same layout instead of a
// fresh roll — destroyed stays destroyed, damaged stays damaged.
void Engine::SaveDungeonRoomHazardState()
{
    if (_dungeonRoomIdx < 0) return;
    DungeonRoomState& roomState = _dungeonRoomStates[_dungeonRoomIdx];
    roomState.hazards.clear();
    for (const RoomHazard& hazard : _roomHazards.Hazards())
    {
        DungeonHazardSnapshot snap;
        snap.type      = (int)hazard.type;
        snap.pos       = hazard.pos;
        snap.fireDir   = hazard.fireDir;
        snap.health    = hazard.health;
        snap.destroyed = (hazard.state == RoomHazardState::Destroyed);
        roomState.hazards.push_back(snap);
    }
}

void Engine::RestoreDungeonRoomHazardState(int roomIdx, bool lavaOnly)
{
    auto stateIt = _dungeonRoomStates.find(roomIdx);
    if (stateIt == _dungeonRoomStates.end()) return;
    for (const DungeonHazardSnapshot& snap : stateIt->second.hazards)
    {
        if (snap.destroyed) continue;
        RoomHazardType type = (RoomHazardType)snap.type;
        if (lavaOnly && type != RoomHazardType::LavaPool) continue;
        _roomHazards.RestoreHazard(type, snap.pos, snap.fireDir, snap.health);
    }
}

void Engine::ClearDungeonEnemies()
{
    for (auto& e : _enemies)
    {
        e->SetActive(false);
        e->Teleport({ -5000.f, -5000.f });
    }
    _spreadProjectiles.clear();
    _cyclopsLasers.clear();
    _lavaBalls.clear();
    _enemyProjectiles.clear();
    _poisonClouds.clear();
    _warriorVfx.clear();
    _pickups.clear();
    _vfx.Clear();
    _damageNumbers.Clear();
    _pendingExp    = 0.f;
    _dungeonEnemiesSpawned = false;
    _treasureChestSpawned  = false;
    _treasureChestBroken   = false;
    _eliteRewardGranted    = false;
    ResetEliteRoomRuntime();
}

void Engine::ResetEliteRoomRuntime()
{
    if (_eliteMinibossPtr)
    {
        _eliteMinibossPtr->SetInvulnerable(false);
        _eliteMinibossPtr->SetLeapFrozen(false);
        _eliteMinibossPtr->SetEliteGuardLinked(false);
        _eliteMinibossPtr->ClearEliteEvents();
    }
    _eliteMechanic           = -1;
    _eliteMinibossPtr        = nullptr;
    _eliteCageRadius         = 0.f;
    _eliteCageDamageTimer    = 0.f;
    _eliteEnrageWarningTimer = 0.f;
    _eliteHazardSpawnTimer   = 0.f;
    // Every reset path also clears the bounded attack-zone pool so no elite
    // damage/warning survives a room exit, restart, or snapshot restore.
    _combatDirector.ClearEliteRuntime();
}

int Engine::GetCompatibleEliteMechanicForRoom(int roomIdx, EliteArchetype archetype)
{
    DungeonRoomState& state = _dungeonRoomStates[roomIdx];
    const int forced = _debug.GetForcedEliteMechanic();

    // A stored modifier survives re-entry/snapshot restore — the room's
    // challenge never rerolls — but only while it stays compatible with the
    // elite that actually lives in the room.
    if (state.eliteMechanic >= 0 && state.eliteMechanic < (int)EliteModifier::Count &&
        forced < 0 &&
        IsEliteModifierCompatible(archetype, (EliteModifier)state.eliteMechanic))
        return state.eliteMechanic;

    const std::uint32_t seed = (std::uint32_t)GetRandomValue(1, 0x7ffffffe);
    state.eliteMechanic = (int)ChooseEliteModifier(archetype, seed, forced);
    return state.eliteMechanic;
}

void Engine::InitializeEliteRoomRuntime(Enemy* elite, int roomIdx,
                                        float worldWidth, float worldHeight)
{
    ResetEliteRoomRuntime();
    _eliteMinibossPtr = elite;
    if (!elite)
        return;

    _eliteMechanic = GetCompatibleEliteMechanicForRoom(roomIdx, elite->GetEliteArchetype());
    _eliteEnrageWarningTimer = kEliteEnrageWarningDuration;

    switch (_eliteMechanic)
    {
    case 0:   // Cage — stay inside the ring
        _eliteCageCenter      = { worldWidth * 0.5f, worldHeight * 0.5f };
        _eliteCageRadius      = kEliteCageRadius;
        _eliteCageDamageTimer = kEliteCageDamageInterval;
        break;
    case 1:   // Guard Links — 60% reduction while any guard lives (never immune)
    {
        bool anyGuardAlive = false;
        for (const auto& enemy : _enemies)
        {
            if (enemy.get() == elite) continue;
            if (enemy && enemy->IsActive() && enemy->IsAlive() && !enemy->IsDying())
            { anyGuardAlive = true; break; }
        }
        elite->SetEliteGuardLinked(anyGuardAlive);
        break;
    }
    case 2:   // Permanent Enrage
        elite->ApplyEnrage();
        break;
    case 3:   // Arena Pressure — themed hazard volleys on the shared budget
        _eliteHazardSpawnTimer = (float)GetRandomValue(
            (int)(kHazardVolleyMinInterval * 100.f),
            (int)(kHazardVolleyMaxInterval * 100.f)) / 100.f;
        break;
    }
}


Engine::DungeonDoorSide Engine::OppositeDungeonDoorSide(int dr, int dc) const
{
    if (dr < 0) return DungeonDoorSide::South;
    if (dr > 0) return DungeonDoorSide::North;
    if (dc < 0) return DungeonDoorSide::East;
    if (dc > 0) return DungeonDoorSide::West;
    return DungeonDoorSide::None;
}

// Map-editor playtest: flip between "enemies encountered" (doors lock) and
// "room clear" (all doors open) live, so the designer can test door
// locking/unlocking without leaving the room.
void Engine::SetPlaytestEnemies(bool enemiesOn)
{
    _editorPlaytestEnemiesOn = enemiesOn;
    if (_dungeonRoomIdx < 0) return;
    DungeonRoomState& roomState = _dungeonRoomStates[_dungeonRoomIdx];

    // Clear whatever is currently alive first. Reset the elite-mechanic state too:
    // _eliteMinibossPtr is a raw pointer INTO _enemies, so clearing the vector
    // would leave it dangling and UpdateEliteMechanics would read freed memory.
    for (auto& enemy : _enemies) { enemy->SetActive(false); enemy->Teleport({ -5000.f, -5000.f }); }
    _enemies.clear();
    _dungeonReinforcements.clear();
    _pendingEnemySpawns.clear();
    _roomClearPending = false;
    ResetEliteRoomRuntime();

    if (enemiesOn)
    {
        roomState.cleared = false;
        roomState.enemiesInitialized = false;
        _dungeonEnemiesSpawned = false;
        SpawnDungeonRoomEnemies();     // fresh wave → doors lock
    }
    else
    {
        // Door state reads the editor-only suppression flag. Do not permanently
        // clear the room, or switching enemies back on could not restore it.
        _dungeonEnemiesSpawned = false;
    }
    ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
}

void Engine::ApplyDungeonRoomDoorState(RoomLayout& layout, int roomIdx, DungeonDoorSide entryDoorSide) const
{
    const auto& rooms = _dungeonGen.GetRooms();
    if (roomIdx < 0 || roomIdx >= (int)rooms.size()) return;

    const DungeonRoom& room = rooms[roomIdx];
    auto stateIt = _dungeonRoomStates.find(roomIdx);
    bool cleared = (stateIt != _dungeonRoomStates.end() && stateIt->second.cleared);
    // Decision rooms (Risk Shrine, …) are peaceful — always walkable-through,
    // even on first entry (independent of when the cleared flag gets set).
    bool hasSpecial = (stateIt != _dungeonRoomStates.end() &&
                       stateIt->second.special != RoomSpecialType::None);
    bool alwaysOpen = (_editorPlaytestActive && !_editorPlaytestEnemiesOn)
        || cleared
        || hasSpecial
        || (room.startsEmpty && !_prologueActive)
        || room.type == RoomType::Rest
        || room.type == RoomType::Treasure
        || room.type == RoomType::Store;

    layout.roomCleared = alwaysOpen;

    int doorStartC = GetDungeonDoorStartCol();
    int doorStartR = GetDungeonDoorStartRow();

    auto shouldOpen = [&](DungeonDoorSide side) {
        return alwaysOpen || side == entryDoorSide;
    };

    layout.doorZoneOpen[(int)RoomWallSide::Top] = shouldOpen(DungeonDoorSide::North);
    layout.doorZoneOpen[(int)RoomWallSide::Bottom] = shouldOpen(DungeonDoorSide::South);
    layout.doorZoneOpen[(int)RoomWallSide::Left] = shouldOpen(DungeonDoorSide::West);
    layout.doorZoneOpen[(int)RoomWallSide::Right] = shouldOpen(DungeonDoorSide::East);

    // Handcrafted rooms express doors entirely through their authored Door Zones
    // plus the doorZoneOpen state above: the renderer hides the wall inside an
    // open zone and collision clears it, revealing the authored ground beneath.
    // Never rewrite their border tile art — the designer's continuous wall stays.
    if (layout.handcrafted)
    {
        if (room.type == RoomType::Boss)
        {
            const bool bossOpen = cleared ||
                (_editorPlaytestActive && !_editorPlaytestEnemiesOn);
            layout.roomCleared = bossOpen;
            for (bool& open : layout.doorZoneOpen) open = bossOpen;
        }
        return;
    }

    auto setNorth = [&](bool open) {
        for (int dc = 0; dc < kDungeonDoorSpanCols; dc++)
            layout.tiles[0][doorStartC + dc] = open ? TileType::Floor : TileType::WallTopFace;
    };
    auto setSouth = [&](bool open) {
        for (int dc = 0; dc < kDungeonDoorSpanCols; dc++)
            layout.tiles[RoomLayout::kRows - 1][doorStartC + dc] = open ? TileType::Floor : TileType::WallBottom;
    };
    auto setWest = [&](bool open) {
        for (int dr = 0; dr < kDungeonDoorSpanRows; dr++)
            layout.tiles[doorStartR + dr][0] = open ? TileType::Floor : TileType::WallLeft;
    };
    auto setEast = [&](bool open) {
        for (int dr = 0; dr < kDungeonDoorSpanRows; dr++)
            layout.tiles[doorStartR + dr][RoomLayout::kCols - 1] = open ? TileType::Floor : TileType::WallRight;
    };

    if (room.type == RoomType::Boss)
    {
        const bool editorOpen = _editorPlaytestActive && !_editorPlaytestEnemiesOn;
        layout.roomCleared = cleared || editorOpen;
        for (bool& open : layout.doorZoneOpen) open = cleared || editorOpen;
        if (room.hasNorth) setNorth(editorOpen);
        if (room.hasSouth) setSouth(editorOpen);
        if (room.hasWest)  setWest(editorOpen);
        if (room.hasEast)  setEast(editorOpen);
        return;
    }

    if (room.hasNorth) setNorth(shouldOpen(DungeonDoorSide::North));
    if (room.hasSouth) setSouth(shouldOpen(DungeonDoorSide::South));
    if (room.hasWest)  setWest(shouldOpen(DungeonDoorSide::West));
    if (room.hasEast)  setEast(shouldOpen(DungeonDoorSide::East));
}

bool Engine::IsDungeonDoorOpen(DungeonDoorSide side) const
{
    // Handcrafted rooms carry no automatic door tile art, so their open state
    // lives in the authored Door Zones rather than the border tiles.
    if (_dungeonRoomLayout.handcrafted)
    {
        switch (side)
        {
        case DungeonDoorSide::North: return _dungeonRoomLayout.doorZoneOpen[(int)RoomWallSide::Top];
        case DungeonDoorSide::South: return _dungeonRoomLayout.doorZoneOpen[(int)RoomWallSide::Bottom];
        case DungeonDoorSide::West:  return _dungeonRoomLayout.doorZoneOpen[(int)RoomWallSide::Left];
        case DungeonDoorSide::East:  return _dungeonRoomLayout.doorZoneOpen[(int)RoomWallSide::Right];
        default: return false;
        }
    }

    int doorStartC = GetDungeonDoorStartCol();
    int doorStartR = GetDungeonDoorStartRow();
    TileType t = TileType::None;

    switch (side)
    {
    case DungeonDoorSide::North: t = _dungeonRoomLayout.tiles[0][doorStartC + kDungeonDoorSpanCols / 2]; break;
    case DungeonDoorSide::South: t = _dungeonRoomLayout.tiles[RoomLayout::kRows - 1][doorStartC + kDungeonDoorSpanCols / 2]; break;
    case DungeonDoorSide::West:  t = _dungeonRoomLayout.tiles[doorStartR + kDungeonDoorSpanRows / 2][0]; break;
    case DungeonDoorSide::East:  t = _dungeonRoomLayout.tiles[doorStartR + kDungeonDoorSpanRows / 2][RoomLayout::kCols - 1]; break;
    default: return false;
    }

    return t == TileType::Floor || t == TileType::FloorVariant || t == TileType::DoorOpen;
}

void Engine::SpawnDungeonDoorOpenEffects()
{
    const auto& rooms = _dungeonGen.GetRooms();
    if (_dungeonRoomIdx < 0 || _dungeonRoomIdx >= (int)rooms.size()) return;

    const DungeonRoom& room = rooms[_dungeonRoomIdx];
    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;
    int doorStartC = GetDungeonDoorStartCol();
    int doorStartR = GetDungeonDoorStartRow();
    size_t effectStartCount = _dungeonClearEffects.size();

    auto addDoorEffect = [&](DungeonDoorSide side, bool exists) {
        if (!exists || side == _dungeonEntryDoorSide) return;
        Vector2 pos{};
        Rectangle glow{};
        switch (side)
        {
        case DungeonDoorSide::North:
            pos = { (doorStartC + kDungeonDoorSpanCols * 0.5f) * cellW, 0.65f * cellH };
            glow = { doorStartC * cellW, 0.f, (float)kDungeonDoorSpanCols * cellW, 0.85f * cellH };
            break;
        case DungeonDoorSide::South:
            pos = { (doorStartC + kDungeonDoorSpanCols * 0.5f) * cellW, (RoomLayout::kRows - 0.65f) * cellH };
            glow = { doorStartC * cellW, (RoomLayout::kRows - 0.85f) * cellH, (float)kDungeonDoorSpanCols * cellW, 0.85f * cellH };
            break;
        case DungeonDoorSide::West:
            pos = { 0.65f * cellW, (doorStartR + kDungeonDoorSpanRows * 0.5f) * cellH };
            glow = { 0.f, doorStartR * cellH, 0.85f * cellW, (float)kDungeonDoorSpanRows * cellH };
            break;
        case DungeonDoorSide::East:
            pos = { (RoomLayout::kCols - 0.65f) * cellW, (doorStartR + kDungeonDoorSpanRows * 0.5f) * cellH };
            glow = { (RoomLayout::kCols - 0.85f) * cellW, doorStartR * cellH, 0.85f * cellW, (float)kDungeonDoorSpanRows * cellH };
            break;
        default: return;
        }
        _dungeonClearEffects.push_back({ pos, 0.f, glow, true });
    };

    addDoorEffect(DungeonDoorSide::North, room.hasNorth);
    addDoorEffect(DungeonDoorSide::South, room.hasSouth);
    addDoorEffect(DungeonDoorSide::West,  room.hasWest);
    addDoorEffect(DungeonDoorSide::East,  room.hasEast);

    if (_dungeonRoomIdx == _dungeonGen.GetBossIndex())
    {
        Rectangle exit = GetDungeonBossExitTrigger();
        if (exit.width > 0.f && exit.height > 0.f)
            _dungeonClearEffects.push_back({ { exit.x + exit.width * 0.5f, exit.y + exit.height * 0.5f }, 0.f, exit, true });
    }
    if (_dungeonClearEffects.size() == effectStartCount)
        _dungeonClearEffects.push_back({ { sw * 0.5f, sh * 0.5f }, 0.f, {}, false });

    if (_roomClearExplosionSound.frameCount > 0)
    {
        StopSound(_roomClearExplosionSound);
        PlaySound(_roomClearExplosionSound);
    }
}

void Engine::UpdateDungeonClearEffects(float dt)
{
    static constexpr float kDuration = 1.75f;
    for (auto& effect : _dungeonClearEffects)
        effect.timer += dt;
    _dungeonClearEffects.erase(
        std::remove_if(_dungeonClearEffects.begin(), _dungeonClearEffects.end(),
            [](const DungeonClearEffect& effect) { return effect.timer >= kDuration; }),
        _dungeonClearEffects.end());
}

void Engine::DrawDungeonClearEffects() const
{
    static constexpr int kFrameW = 64;
    static constexpr int kFrameH = 64;
    static constexpr int kFrameCount = 24;
    static constexpr float kDuration = 1.75f;
    static constexpr float kScale = 4.0f;

    for (const DungeonClearEffect& effect : _dungeonClearEffects)
    {
        float pct = std::clamp(effect.timer / kDuration, 0.f, 0.999f);
        if (effect.hasGlow)
        {
            float fadeOut = 1.f - pct;
            float pulse = 0.65f + 0.35f * sinf(effect.timer * 13.f);
            Rectangle outer{
                effect.glowRect.x - 8.f,
                effect.glowRect.y - 8.f,
                effect.glowRect.width + 16.f,
                effect.glowRect.height + 16.f
            };
            Color glow = Color{ 255, 204, 82, 255 };
            DrawRectangleRounded(outer, 0.20f, 8, Fade(glow, 0.16f * fadeOut * pulse));
            DrawRectangleRounded(effect.glowRect, 0.20f, 8, Fade(glow, 0.30f * fadeOut * pulse));
            DrawRectangleRoundedLines(effect.glowRect, 0.20f, 8, Fade(WHITE, 0.55f * fadeOut * pulse));
        }

        if (_roomClearExplosionTex.id == 0)
            continue;

        int frame = std::min((int)(pct * kFrameCount), kFrameCount - 1);
        Rectangle src = GetAnimationFrameRect(_roomClearExplosionTex, kFrameW, kFrameH, frame);
        Rectangle dst{
            effect.worldPos.x,
            effect.worldPos.y,
            kFrameW * kScale,
            kFrameH * kScale
        };
        DrawTexturePro(_roomClearExplosionTex, src, dst,
            { dst.width * 0.5f, dst.height * 0.5f }, 0.f, WHITE);
    }
}
// Set the north door tiles of the Store room to doorType.
// Used to lock the door at cutscene start and unlock it after ability selection.
void Engine::SetStoreDoorTiles(TileType doorType)
{
    if (_currentRoomType != RoomType::Store) return;

    TileType placedType = (doorType == TileType::DoorOpen) ? TileType::Floor : doorType;
    int doorStartC = GetDungeonDoorStartCol();
    for (int dc = 0; dc < kDungeonDoorSpanCols; dc++)
        _dungeonRoomLayout.tiles[0][doorStartC + dc] = placedType;

    RebuildDungeonNav();
}

void Engine::ApplyDungeonBossExitTiles(TileType doorType)
{
    int bossIdx = _dungeonGen.GetBossIndex();
    if (bossIdx < 0) return;
    const auto& rooms = _dungeonGen.GetRooms();
    if (bossIdx >= (int)rooms.size()) return;
    const DungeonRoom& boss = rooms[bossIdx];

    int doorStartC = GetDungeonDoorStartCol();
    int doorStartR = GetDungeonDoorStartRow();

    // Place the exit on the first wall that has no existing dungeon connection.
    if (!boss.hasSouth)
    {
        for (int dc = 0; dc < kDungeonDoorSpanCols; dc++)
            _dungeonRoomLayout.tiles[RoomLayout::kRows - 1][doorStartC + dc] = doorType;
    }
    else if (!boss.hasNorth)
    {
        for (int dc = 0; dc < kDungeonDoorSpanCols; dc++)
            _dungeonRoomLayout.tiles[0][doorStartC + dc] = doorType;
    }
    else if (!boss.hasEast)
    {
        for (int dr = 0; dr < kDungeonDoorSpanRows; dr++)
            _dungeonRoomLayout.tiles[doorStartR + dr][RoomLayout::kCols - 1] = doorType;
    }
    else
    {
        for (int dr = 0; dr < kDungeonDoorSpanRows; dr++)
            _dungeonRoomLayout.tiles[doorStartR + dr][0] = doorType;
    }
}

Rectangle Engine::GetDungeonBossExitTrigger() const
{
    int bossIdx = _dungeonGen.GetBossIndex();
    if (bossIdx < 0) return {};
    const auto& rooms = _dungeonGen.GetRooms();
    if (bossIdx >= (int)rooms.size()) return {};
    const DungeonRoom& boss = rooms[bossIdx];

    float sw    = (float)kVirtualWidth;
    float sh    = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;
    int doorStartC = GetDungeonDoorStartCol();
    int doorStartR = GetDungeonDoorStartRow();

    if (!boss.hasSouth)
        return { doorStartC * cellW, (RoomLayout::kRows - 2) * cellH, (float)kDungeonDoorSpanCols * cellW, cellH };
    if (!boss.hasNorth)
        return { doorStartC * cellW, cellH, (float)kDungeonDoorSpanCols * cellW, cellH };
    if (!boss.hasEast)
        return { (RoomLayout::kCols - 2) * cellW, doorStartR * cellH, cellW, (float)kDungeonDoorSpanRows * cellH };
    return { cellW, doorStartR * cellH, cellW, (float)kDungeonDoorSpanRows * cellH };
}

void Engine::RebuildDungeonNav()
{
    float sw    = (float)kVirtualWidth;
    float sh    = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;
    float pxSX  = cellW / 16.f;
    float pxSY  = cellH / 16.f;

    std::vector<Rectangle> solids;

    // Wall tiles - every non-floor, non-void tile blocks a full cell.
    for (int r = 0; r < RoomLayout::kRows; r++)
    {
        for (int c = 0; c < RoomLayout::kCols; c++)
        {
            TileType t = _dungeonRoomLayout.tiles[r][c];
            if ((!_dungeonRoomLayout.solid[r][c] || RoomPlacementClearsAtDoor(
                    {(float)c,(float)r,1.f,1.f},_dungeonRoomLayout)) &&
                (t == TileType::Floor        || t == TileType::FloorVariant ||
                t == TileType::DoorOpen     || t == TileType::Void         ||
                t == TileType::None))
                continue;
            solids.push_back({ c * cellW, r * cellH, cellW, cellH });
        }
    }

    // Props use their TileMapper collision AABBs. Some props are wider/taller
    // than one tile, so blocking only the placement cell lets A* route through
    // part of the physical collider.
    for (const SpritePlacement& p : _dungeonRoomLayout.props)
    {
        const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,p,_tileDefs);
        if (defs==nullptr || p.defIdx<0 || p.defIdx>=(int)defs->props.size()) continue;
        const Rectangle& coll = defs->props[p.defIdx].collision;
        solids.push_back({
            p.col * cellW + coll.x * pxSX,
            p.row * cellH + coll.y * pxSY,
            coll.width * pxSX,
            coll.height * pxSY });
    }
    for (const SpritePlacement& p : _dungeonRoomLayout.animProps)
    {
        const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,p,_tileDefs);
        if (defs==nullptr || p.defIdx<0 || p.defIdx>=(int)defs->animProps.size()) continue;
        const Rectangle& coll = defs->animProps[p.defIdx].collision;
        solids.push_back({
            p.col * cellW + coll.x * pxSX,
            p.row * cellH + coll.y * pxSY,
            coll.width * pxSX,
            coll.height * pxSY });
    }

    _nav.CancelAndReset();
    _nav.Rebuild(sw, sh, solids);
    _nav.RefreshSync(_player.GetFeetWorldPos());
}

void Engine::ResolveDungeonEnemyCollisions()
{
    float sw    = (float)kVirtualWidth;
    float sh    = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;
    float pxSX  = cellW / 16.f;
    float pxSY  = cellH / 16.f;

    for (auto& e : _enemies)
    {
        if (!e->IsActive()) continue;

        // -- Wall tiles - only check the 5-5 neighbourhood around the enemy ----
        Vector2 ePos = e->GetWorldPos();
        int ec = std::max(0, std::min((int)(ePos.x / cellW), RoomLayout::kCols - 1));
        int er = std::max(0, std::min((int)(ePos.y / cellH), RoomLayout::kRows - 1));

        bool rushStopHandled = false;
        for (int r = std::max(0, er - 2); r <= std::min(RoomLayout::kRows - 1, er + 2) && !rushStopHandled; r++)
        {
            for (int c = std::max(0, ec - 2); c <= std::min(RoomLayout::kCols - 1, ec + 2); c++)
            {
                TileType t = _dungeonRoomLayout.tiles[r][c];
                if ((!_dungeonRoomLayout.solid[r][c] || RoomPlacementClearsAtDoor(
                        {(float)c,(float)r,1.f,1.f},_dungeonRoomLayout)) &&
                    (t == TileType::Floor        || t == TileType::FloorVariant ||
                    t == TileType::DoorOpen     || t == TileType::Void         ||
                    t == TileType::None))
                    continue;

                Rectangle wallRect{ c * cellW, r * cellH, cellW, cellH };
                Rectangle eRect = e->GetCollisionRec();
                if (!CheckCollisionRecs(eRect, wallRect)) continue;

                // Rushing ogre / dashing molarbeast: undo movement and signal impact.
                // The position is already valid after UndoMovement, so skip the push-out.
                if (Ogre* ogre = e->AsOgre())
                {
                    if (ogre->IsRushing()) { ogre->OnRushBlocked(); rushStopHandled = true; break; }
                }
                else if (Molarbeast* mb = e->AsMolarbeast())
                {
                    if (mb->IsDashing()) { mb->OnDashBlocked(); rushStopHandled = true; break; }

                    Vector2 mtv{};
                    if (CheckCapsuleRect(mb->GetCapsule(), wallRect, mtv))
                    {
                        if (e->IsBeingForcedPushed())
                        {
                            e->OnForcedPushCollision();
                        }
                        else
                        {
                            Vector2 p = e->GetWorldPos();
                            e->Teleport({ p.x + mtv.x, p.y + mtv.y });
                        }
                    }
                    continue;
                }

                if (e->IsBeingForcedPushed())
                {
                    e->OnForcedPushCollision();
                }
                else
                {
                    float oL = (eRect.x + eRect.width)  - wallRect.x;
                    float oR = (wallRect.x + wallRect.width) - eRect.x;
                    float oT = (eRect.y + eRect.height) - wallRect.y;
                    float oB = (wallRect.y + wallRect.height) - eRect.y;
                    float rX = (oL < oR) ? -oL : oR;
                    float rY = (oT < oB) ? -oT : oB;
                    Vector2 p = e->GetWorldPos();
                    if (std::abs(rX) < std::abs(rY)) p.x += rX; else p.y += rY;
                    e->Teleport(p);
                }
            }
        }

        // -- Props - precise collision rect ------------------------------------
        if (!rushStopHandled)
        {
            auto resolveEnemyVsPropRect = [&](Rectangle propRect) {
                if (Molarbeast* mb = e->AsMolarbeast())
                {
                    Vector2 mtv{};
                    if (!CheckCapsuleRect(mb->GetCapsule(), propRect, mtv)) return;
                    if (mb->IsDashing()) { mb->OnDashBlocked(); return; }
                    if (e->IsBeingForcedPushed()) { e->OnForcedPushCollision(); return; }

                    Vector2 p = e->GetWorldPos();
                    e->Teleport({ p.x + mtv.x, p.y + mtv.y });
                    return;
                }

                Rectangle eRect = e->GetCollisionRec();
                if (!CheckCollisionRecs(eRect, propRect)) return;

                if (Ogre* ogre = e->AsOgre())
                    { if (ogre->IsRushing()) { ogre->OnRushBlocked(); return; } }

                if (e->IsBeingForcedPushed()) { e->OnForcedPushCollision(); return; }

                float oL = (eRect.x + eRect.width)  - propRect.x;
                float oR = (propRect.x + propRect.width)  - eRect.x;
                float oT = (eRect.y + eRect.height) - propRect.y;
                float oB = (propRect.y + propRect.height) - eRect.y;
                float rX = (oL < oR) ? -oL : oR;
                float rY = (oT < oB) ? -oT : oB;
                Vector2 p = e->GetWorldPos();
                if (std::abs(rX) < std::abs(rY)) p.x += rX; else p.y += rY;
                e->Teleport(p);
            };

            for (const SpritePlacement& prop : _dungeonRoomLayout.props)
            {
                const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,prop,_tileDefs);
                if(defs==nullptr||prop.defIdx<0||prop.defIdx>=(int)defs->props.size()) continue;
                const Rectangle& coll = defs->props[prop.defIdx].collision;
                resolveEnemyVsPropRect({
                    prop.col * cellW + coll.x * pxSX,
                    prop.row * cellH + coll.y * pxSY,
                    coll.width  * pxSX, coll.height * pxSY });
            }
            for (const SpritePlacement& prop : _dungeonRoomLayout.animProps)
            {
                const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,prop,_tileDefs);
                if(defs==nullptr||prop.defIdx<0||prop.defIdx>=(int)defs->animProps.size()) continue;
                const Rectangle& coll = defs->animProps[prop.defIdx].collision;
                resolveEnemyVsPropRect({
                    prop.col * cellW + coll.x * pxSX,
                    prop.row * cellH + coll.y * pxSY,
                    coll.width  * pxSX, coll.height * pxSY });
            }
        }
    }
}

Rectangle Engine::GetDungeonRoomRect(int roomIdx) const
{
    const auto& rooms = _dungeonGen.GetRooms();
    if (roomIdx < 0 || roomIdx >= (int)rooms.size())
        return {};

    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;
    float margin = 90.f;
    float gridPx = std::min((sw - margin * 2.f) / DungeonGen::kGridSize,
                            (sh - margin * 2.f - 80.f) / DungeonGen::kGridSize);
    float startX = sw * 0.5f - (gridPx * DungeonGen::kGridSize) * 0.5f;
    float startY = margin;

    const DungeonRoom& room = rooms[roomIdx];
    float pad = gridPx * 0.12f;
    return Rectangle{
        startX + room.col * gridPx + pad,
        startY + room.row * gridPx + pad,
        gridPx - pad * 2.f,
        gridPx - pad * 2.f
    };
}

Engine::DungeonDoorSide Engine::GetBossBarrierSide() const
{
    if (_bossBarrierUnlocked || _dungeonRoomIdx < 0)
        return DungeonDoorSide::None;

    int bossIdx = _dungeonGen.GetBossIndex();
    if (bossIdx < 0)
        return DungeonDoorSide::None;

    if (_dungeonGen.GetNeighborIndex(_dungeonRoomIdx, -1, 0) == bossIdx) return DungeonDoorSide::North;
    if (_dungeonGen.GetNeighborIndex(_dungeonRoomIdx, +1, 0) == bossIdx) return DungeonDoorSide::South;
    if (_dungeonGen.GetNeighborIndex(_dungeonRoomIdx, 0, -1) == bossIdx) return DungeonDoorSide::West;
    if (_dungeonGen.GetNeighborIndex(_dungeonRoomIdx, 0, +1) == bossIdx) return DungeonDoorSide::East;
    return DungeonDoorSide::None;
}

Rectangle Engine::GetBossBarrierRect(DungeonDoorSide side) const
{
    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;
    float cellW = sw / (float)RoomLayout::kCols;
    float cellH = sh / (float)RoomLayout::kRows;
    int doorStartC = GetDungeonDoorStartCol();
    int doorStartR = GetDungeonDoorStartRow();

    switch (side)
    {
    case DungeonDoorSide::North: return { doorStartC * cellW, 0.f, (float)kDungeonDoorSpanCols * cellW, cellH };
    case DungeonDoorSide::South: return { doorStartC * cellW, (RoomLayout::kRows - 1) * cellH, (float)kDungeonDoorSpanCols * cellW, cellH };
    case DungeonDoorSide::West:  return { 0.f, doorStartR * cellH, cellW, (float)kDungeonDoorSpanRows * cellH };
    case DungeonDoorSide::East:  return { (RoomLayout::kCols - 1) * cellW, doorStartR * cellH, cellW, (float)kDungeonDoorSpanRows * cellH };
    default: return {};
    }
}

Vector2 Engine::GetMagicGemWorldPos() const
{
    return { (float)kVirtualWidth * 0.5f, (float)kVirtualHeight * 0.5f };
}

void Engine::UpdateDungeonMagicGemAndBarrier(float dt)
{
    _magicGemAnimTimer += dt;
    while (_magicGemAnimTimer >= 0.10f)
    {
        _magicGemAnimTimer -= 0.10f;
        _magicGemFrame = (_magicGemFrame + 1) % 8;
    }

    _bossBarrierAnimTimer += dt;
    while (_bossBarrierAnimTimer >= 0.08f)
    {
        _bossBarrierAnimTimer -= 0.08f;
        _bossBarrierFrame = (_bossBarrierFrame + 1) % 24;
    }

    if (_bossBarrierMessageTimer > 0.f)
        _bossBarrierMessageTimer = std::max(0.f, _bossBarrierMessageTimer - dt);

    int keyIdx = _dungeonGen.GetKeyIndex();
    if (_dungeonRoomIdx == keyIdx && _magicGemSpawned && !_magicGemCollected)
    {
        Vector2 gemPos = GetMagicGemWorldPos();
        Rectangle gemRec{ gemPos.x - 28.f, gemPos.y - 28.f, 56.f, 56.f };
        if (CheckCollisionRecs(_player.GetCollisionRec(), gemRec))
        {
            _magicGemCollected = true;
            _magicGemSpawned = false;
            _hasMagicGem = true;
            _message = "Magic gem collected";
        }
    }

    DungeonDoorSide barrierSide = GetBossBarrierSide();
    if (barrierSide != DungeonDoorSide::None)
    {
        Rectangle barrierRec = GetBossBarrierRect(barrierSide);
        Rectangle playerRec = _player.GetCollisionRec();
        if (CheckCollisionRecs(playerRec, barrierRec))
        {
            if (_hasMagicGem)
            {
                _hasMagicGem = false;
                _bossBarrierUnlocked = true;
                _bossBarrierMessageTimer = 0.f;
                _message = "The magic gem shattered the barrier";
            }
            else
            {
                _bossBarrierMessageTimer = 2.2f;
                _message = "you need a a magic gem to get past the barrier";

                Vector2 p = _player.GetWorldPos();
                switch (barrierSide)
                {
                case DungeonDoorSide::North:
                    p.y = barrierRec.y + barrierRec.height + playerRec.height * 0.5f + 2.f;
                    break;
                case DungeonDoorSide::South:
                    p.y = barrierRec.y - playerRec.height * 0.5f - 2.f;
                    break;
                case DungeonDoorSide::West:
                    p.x = barrierRec.x + barrierRec.width + playerRec.width * 0.5f + 2.f;
                    break;
                case DungeonDoorSide::East:
                    p.x = barrierRec.x - playerRec.width * 0.5f - 2.f;
                    break;
                default:
                    break;
                }
                _player.SetWorldPos(p);
                if (_player.IsBeingForcedPushed())
                    _player.OnForcedPushCollision();
            }
        }
    }
}
void Engine::DrawDungeonMagicGemAndBarrier() const
{
    if (_magicGemTex.id > 0 && _dungeonRoomIdx == _dungeonGen.GetKeyIndex() && _magicGemSpawned && !_magicGemCollected)
    {
        Vector2 gemPos = GetMagicGemWorldPos();
        Rectangle src{ (float)(_magicGemFrame * 16), 0.f, 16.f, 16.f };
        Rectangle dst{ gemPos.x - 32.f, gemPos.y - 32.f, 64.f, 64.f };
        DrawTexturePro(_magicGemTex, src, dst, {}, 0.f, WHITE);
    }

    DungeonDoorSide side = GetBossBarrierSide();
    if (_bossBarrierTex.id > 0 && side != DungeonDoorSide::None)
    {
        int frame = _bossBarrierFrame % 24;
        int col = frame % 6;
        int row = frame / 6;
        Rectangle src{ (float)(col * 32), (float)(row * 32), 32.f, 32.f };
        Rectangle dst = GetBossBarrierRect(side);
        DrawTexturePro(_bossBarrierTex, src, dst, {}, 0.f, WHITE);
    }
}

void Engine::DrawMagicGemHudIcon() const
{
    if (!_hasMagicGem || _magicGemTex.id == 0)
        return;

    float sw = (float)kVirtualWidth;
    Rectangle src{ 0.f, 0.f, 16.f, 16.f };
    Rectangle dst{ sw - 76.f, 22.f, 48.f, 48.f };
    DrawTexturePro(_magicGemTex, src, dst, {}, 0.f, WHITE);
    DrawText("x1", (int)(dst.x + dst.width + 4.f), (int)(dst.y + 18.f), 18, GOLD);
}
// -- Settings ------------------------------------------------------------------

void Engine::ApplySfxVolume()
{
    if (!_audioInitialised) return;
    float sfx = _settingsMgr.Get().sfxVolume;
    SetSoundVolume(_buttonPressSound,        0.35f * sfx);
    SetSoundVolume(_pickupSound,             0.45f * sfx);
    SetSoundVolume(_lavaBallImpactSound,     0.45f * sfx);
    SetSoundVolume(_roomClearExplosionSound, 0.60f * sfx);
    SetSoundVolume(_explosionSound,          1.0f  * sfx);
    SetSoundVolume(_fireballCastSound,       1.0f  * sfx);
    SfxBank::Get().SetVolumeScale(sfx);   // categorized SFX library master volume
}

// -- Helpers used by the settings screen --------------------------------------
namespace
{
    float DrawSettingsSlider(const char* label, float value, float trackX, float trackY,
                             float trackW, bool isBeingDragged,
                             Vector2 mouse, bool mouseDown, bool mouseReleased)
    {
        const float trackH  = 10.f;
        const float handleR = 18.f;
        const float labelFs = 34.f;
        const float pctFs   = 30.f;

        DrawText(label, (int)(trackX - 320.f), (int)(trackY + trackH * 0.5f - labelFs * 0.5f),
                 (int)labelFs, Color{220, 240, 255, 230});

        DrawRectangleRounded({ trackX, trackY, trackW, trackH }, 1.f, 4, Color{40, 60, 75, 220});
        DrawRectangleRounded({ trackX, trackY, trackW * value, trackH }, 1.f, 4, Color{130, 235, 255, 220});

        float hx = trackX + trackW * value;
        float hy = trackY + trackH * 0.5f;
        bool  hovered = (fabsf(mouse.x - hx) < handleR + 8.f && fabsf(mouse.y - hy) < handleR + 8.f);
        DrawCircleV({ hx, hy }, handleR,
                    (hovered || isBeingDragged) ? Color{255,255,255,255} : Color{180,220,240,255});

        int pct = (int)(value * 100.f + 0.5f);
        DrawText(TextFormat("%d%%", pct),
                 (int)(trackX + trackW + 20.f),
                 (int)(trackY + trackH * 0.5f - pctFs * 0.5f),
                 (int)pctFs, Color{220, 240, 255, 200});

        bool overTrack = (mouse.x >= trackX - handleR && mouse.x <= trackX + trackW + handleR
                       && mouse.y >= trackY - handleR && mouse.y <= trackY + handleR * 2.f);
        if (mouseDown && (isBeingDragged || overTrack))
        {
            float raw = (mouse.x - trackX) / trackW;
            value = raw < 0.f ? 0.f : raw > 1.f ? 1.f : raw;
        }
        return value;
    }

    int DrawOptionRow(const char* label, const char* const* options, int count,
                      int activeIdx, float rowX, float rowY, float btnW, float btnH,
                      Vector2 mouse, bool mouseClicked)
    {
        const float labelFs = 34.f;
        DrawText(label, (int)(rowX - 320.f), (int)(rowY + btnH * 0.5f - labelFs * 0.5f),
                 (int)labelFs, Color{220, 240, 255, 230});

        for (int i = 0; i < count; ++i)
        {
            float bx = rowX + i * (btnW + 16.f);
            Rectangle btn = { bx, rowY, btnW, btnH };
            bool isActive  = (i == activeIdx);
            bool isHovered = CheckCollisionPointRec(mouse, btn);

            Color fill   = isActive  ? Color{130, 235, 255, 200}
                         : isHovered ? Color{60,  110, 140, 200}
                                     : Color{30,   55,  70, 200};
            Color border = isActive  ? Color{130, 235, 255, 255}
                                     : Color{70,  120, 150, 160};
            DrawRectangleRounded(btn, 0.2f, 6, fill);
            DrawRectangleRoundedLines(btn, 0.2f, 6, border);

            const float fs = 28.f;
            int tw = MeasureText(options[i], (int)fs);
            DrawText(options[i],
                     (int)(bx + btnW * 0.5f - tw * 0.5f),
                     (int)(rowY + btnH * 0.5f - fs * 0.5f),
                     (int)fs, isActive ? BLACK : RAYWHITE);

            if (mouseClicked && isHovered)
                activeIdx = i;
        }
        return activeIdx;
    }
}

void Engine::UpdateSettings(float dt)
{
    (void)dt;

    GameSettings& s     = _settingsMgr.Get();
    Vector2       mouse = GetVirtualMousePos();
    bool mouseDown      = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    bool mousePressed   = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
    bool mouseReleased  = IsMouseButtonReleased(MOUSE_LEFT_BUTTON);

    if (GetTouchPointCount() > 0)
    {
        mouse        = GetVirtualTouchPos(0);
        mouseDown    = true;
        mousePressed = true;
    }

    const float sw     = (float)kVirtualWidth;
    const float sh     = (float)kVirtualHeight;
    const float panelW = 1100.f;
    const float panelH = 880.f;
    const float panelX = sw * 0.5f - panelW * 0.5f;
    const float panelY = sh * 0.5f - panelH * 0.5f;

    // ESC: cancel an active rebind first; if none, exit settings
    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (_settingsRebindSlot >= 0)
            _settingsRebindSlot = -1;
        else if (_settingsGpRebindSlot >= 0)
            _settingsGpRebindSlot = -1;
        else
        {
            _player.SetBindings(_keybindingsEdit);
            SaveKeybindings();
            _settingsMgr.Save();
            _settingsMgr.ApplyWindow();
            _settingsMgr.ApplyVolumes(_audio);
            ApplySfxVolume();
            _gameState            = _stateBeforeSettings;
            _settingsDragSlider   = -1;
        }
        return;
    }

    // -- Gamepad cursor navigation ---------------------------------------------
    if (IsGamepadAvailable(GamepadInput::kGamepad))
    {
        float axisX = GetGamepadAxisMovement(GamepadInput::kGamepad, GAMEPAD_AXIS_LEFT_X);
        float axisY = GetGamepadAxisMovement(GamepadInput::kGamepad, GAMEPAD_AXIS_LEFT_Y);
        _settingsGpCooldown -= dt;
        constexpr float kGpCooldown = 0.18f;
        bool gpLeft  = IsGamepadButtonPressed(GamepadInput::kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT)  || (axisX < -0.5f && _settingsGpCooldown <= 0.f);
        bool gpRight = IsGamepadButtonPressed(GamepadInput::kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || (axisX >  0.5f && _settingsGpCooldown <= 0.f);
        bool gpUp    = IsGamepadButtonPressed(GamepadInput::kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP)    || (axisY < -0.5f && _settingsGpCooldown <= 0.f);
        bool gpDown  = IsGamepadButtonPressed(GamepadInput::kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN)  || (axisY >  0.5f && _settingsGpCooldown <= 0.f);
        bool gpA     = IsGamepadButtonPressed(GamepadInput::kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        bool gpB     = IsGamepadButtonPressed(GamepadInput::kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);

        // B cancels rebind first; if no rebind active it exits settings
        if (gpB)
        {
            if (_settingsRebindSlot >= 0)   { _settingsRebindSlot   = -1; return; }
            if (_settingsGpRebindSlot >= 0) { _settingsGpRebindSlot = -1; return; }
            _player.SetBindings(_keybindingsEdit);
            SaveKeybindings();
            _settingsMgr.Save();
            _settingsMgr.ApplyWindow();
            _settingsMgr.ApplyVolumes(_audio);
            ApplySfxVolume();
            _gameState          = _stateBeforeSettings;
            _settingsDragSlider = -1;
            return;
        }

        // Don't navigate while waiting for a key/button input during rebind
        if (_settingsRebindSlot < 0 && _settingsGpRebindSlot < 0)
        {
            // Helper: sync the cursor column with the actual current display option
            auto syncDisplayCol = [&]()
            {
                if (_settingsTab != 0) { _settingsGpContentCol = 0; return; }
                if (_settingsGpContentRow == 0)
                    _settingsGpContentCol = (int)s.windowMode;
                else if (_settingsGpContentRow == 1)
                    _settingsGpContentCol = (s.windowedWidth == 1280) ? 0 : (s.windowedWidth == 2560) ? 2 : 1;
                else
                    _settingsGpContentCol = s.vsync ? 0 : 1;
            };

            // Row count per tab for content section
            int maxContentRows = (_settingsTab == 0) ? 3
                               : (_settingsTab == 1) ? 3
                               : (_keybindSubTab == 0) ? 10 : 7;

            switch (_settingsGpSection)
            {
            case 0: // Main tab strip (Display / Audio / Keybindings)
                if (gpLeft)  { _settingsGpTabCursor = (_settingsGpTabCursor + 2) % 3; _settingsGpCooldown = kGpCooldown; }
                if (gpRight) { _settingsGpTabCursor = (_settingsGpTabCursor + 1) % 3; _settingsGpCooldown = kGpCooldown; }
                if (gpA)
                {
                    _settingsTab          = _settingsGpTabCursor;
                    _settingsRebindSlot   = -1;
                    _settingsGpRebindSlot = -1;
                    _settingsGpSection    = 2;
                    _settingsGpContentRow = 0;
                    syncDisplayCol();
                }
                if (gpDown)
                {
                    _settingsGpSection    = (_settingsTab == 2) ? 1 : 2;
                    _settingsGpContentRow = 0;
                    syncDisplayCol();
                    _settingsGpCooldown = kGpCooldown;
                }
                break;

            case 1: // Keybindings sub-tab strip (M&K / Gamepad)
                if (gpLeft || gpRight) { _settingsGpSubCursor ^= 1; _settingsGpCooldown = kGpCooldown; }
                if (gpA)
                {
                    _keybindSubTab        = _settingsGpSubCursor;
                    _settingsRebindSlot   = -1;
                    _settingsGpRebindSlot = -1;
                    _settingsGpSection    = 2;
                    _settingsGpContentRow = 0;
                }
                if (gpUp)   { _settingsGpSection = 0; _settingsGpTabCursor = _settingsTab; _settingsGpCooldown = kGpCooldown; }
                if (gpDown) { _settingsGpSection = 2; _settingsGpContentRow = 0; _settingsGpCooldown = kGpCooldown; }
                break;

            case 2: // Content rows
                if (gpUp)
                {
                    if (_settingsGpContentRow > 0)
                    {
                        _settingsGpContentRow--;
                        syncDisplayCol();
                        _settingsGpCooldown = kGpCooldown;
                    }
                    else if (_settingsTab == 2)
                    {
                        // Keybind: go back to sub-tab strip, sync cursor with active sub-tab
                        _settingsGpSection    = 1;
                        _settingsGpSubCursor  = _keybindSubTab;
                        _settingsGpCooldown   = kGpCooldown;
                    }
                    else
                    {
                        // Other tabs: go to main tab strip, sync cursor with active tab
                        _settingsGpSection   = 0;
                        _settingsGpTabCursor = _settingsTab;
                        _settingsGpCooldown  = kGpCooldown;
                    }
                }
                if (gpDown)
                {
                    if (_settingsGpContentRow < maxContentRows - 1)
                    {
                        _settingsGpContentRow++;
                        syncDisplayCol();
                        _settingsGpCooldown = kGpCooldown;
                    }
                    else { _settingsGpSection = 3; _settingsGpCooldown = kGpCooldown; }
                }

                if (_settingsTab == 0) // Display options: left/right changes selection
                {
                    static const int kDisplayColCount[3] = { 3, 3, 2 };
                    int maxCols = kDisplayColCount[_settingsGpContentRow];
                    if (gpLeft  && _settingsGpContentCol > 0)           { _settingsGpContentCol--; _settingsGpCooldown = kGpCooldown; }
                    if (gpRight && _settingsGpContentCol < maxCols - 1) { _settingsGpContentCol++; _settingsGpCooldown = kGpCooldown; }
                    if (gpLeft || gpRight)
                    {
                        int col = _settingsGpContentCol;
                        if      (_settingsGpContentRow == 0) { s.windowMode = (GameSettings::WindowMode)col; _settingsMgr.ApplyWindow(); }
                        else if (_settingsGpContentRow == 1 && s.windowMode == GameSettings::WindowMode::Windowed)
                        {
                            static const int kResW[] = { 1280, 1920, 2560 };
                            static const int kResH[] = { 720,  1080, 1440 };
                            s.windowedWidth  = kResW[col];
                            s.windowedHeight = kResH[col];
                            _settingsMgr.ApplyWindow();
                        }
                        else if (_settingsGpContentRow == 2) { s.vsync = (col == 0); _settingsMgr.ApplyWindow(); }
                    }
                }
                else if (_settingsTab == 1) // Audio sliders: left/right adjusts value
                {
                    float* sliders[3] = { &s.masterVolume, &s.musicVolume, &s.sfxVolume };
                    if (_settingsGpContentRow >= 0 && _settingsGpContentRow <= 2)
                    {
                        if (gpLeft)  { *sliders[_settingsGpContentRow] = std::max(0.f, *sliders[_settingsGpContentRow] - 0.05f); _settingsGpCooldown = kGpCooldown; }
                        if (gpRight) { *sliders[_settingsGpContentRow] = std::min(1.f, *sliders[_settingsGpContentRow] + 0.05f); _settingsGpCooldown = kGpCooldown; }
                        if (gpLeft || gpRight)
                        {
                            SetMasterVolume(s.masterVolume);
                            _audio.SetMusicVolumeScale(s.musicVolume);
                            _audio.SetSfxVolumeScale(s.sfxVolume);
                            ApplySfxVolume();
                        }
                    }
                }
                else // Keybindings: A starts rebind for focused row
                {
                    if (gpA)
                    {
                        if (_keybindSubTab == 0) _settingsRebindSlot   = _settingsGpContentRow;
                        else                     _settingsGpRebindSlot = _settingsGpContentRow;
                        // Return immediately so the scan loop below doesn't see this same A press
                        return;
                    }
                }
                break;

            case 3: // Back button
                if (gpA)
                {
                    _player.SetBindings(_keybindingsEdit);
                    SaveKeybindings();
                    _settingsMgr.Save();
                    _settingsMgr.ApplyWindow();
                    _settingsMgr.ApplyVolumes(_audio);
                    ApplySfxVolume();
                    _gameState          = _stateBeforeSettings;
                    _settingsDragSlider = -1;
                    _settingsRebindSlot   = -1;
                    _settingsGpRebindSlot = -1;
                    return;
                }
                if (gpUp) { _settingsGpSection = 2; _settingsGpContentRow = maxContentRows - 1; _settingsGpCooldown = kGpCooldown; }
                break;
            }
        }
    }

    // Tab strip
    const float tabY   = panelY + 85.f;
    const float tabH   = 52.f;
    const float tabGap = 14.f;
    const float kTabW[3] = { 195.f, 175.f, 225.f };
    float tabX = panelX + 40.f;
    for (int tabIdx = 0; tabIdx < 3; tabIdx++)
    {
        if (mousePressed && CheckCollisionPointRec(mouse, { tabX, tabY, kTabW[tabIdx], tabH }))
        {
            _settingsTab          = tabIdx;
            _settingsRebindSlot   = -1;
            _settingsGpRebindSlot = -1;
        }
        tabX += kTabW[tabIdx] + tabGap;
    }

    // Back button - sits below the nine-slice border PNG (borderPad=24 + 8 gap)
    float backBtnX = panelX + panelW * 0.5f - 140.f;
    float backBtnY = panelY + panelH + 32.f;
    if (mousePressed && CheckCollisionPointRec(mouse, { backBtnX, backBtnY, 280.f, 55.f }))
    {
        _player.SetBindings(_keybindingsEdit);
        SaveKeybindings();
        _settingsMgr.Save();
        _settingsMgr.ApplyWindow();
        _settingsMgr.ApplyVolumes(_audio);
        ApplySfxVolume();
        _gameState          = _stateBeforeSettings;
        _settingsDragSlider = -1;
        _settingsRebindSlot   = -1;
        _settingsGpRebindSlot = -1;
        return;
    }

    const float contentX   = panelX + 400.f;
    const float contentY   = panelY + 175.f;
    const float rowSpacing = 110.f;

    if (_settingsTab == 0)
    {
        // -- Display ---------------------------------------------------------
        static const char* kModes[] = { "Fullscreen", "Borderless", "Windowed" };
        int modeIdx = (int)s.windowMode;
        int newMode = DrawOptionRow("Window Mode", kModes, 3, modeIdx,
                                    contentX, contentY, 210.f, 56.f, mouse, mousePressed);
        if (newMode != modeIdx)
        {
            s.windowMode = (GameSettings::WindowMode)newMode;
            _settingsMgr.ApplyWindow();
        }

        if (s.windowMode == GameSettings::WindowMode::Windowed)
        {
            static const char* kRes[]  = { "1280x720", "1920x1080", "2560x1440" };
            static const int   kResW[] = { 1280, 1920, 2560 };
            static const int   kResH[] = { 720,  1080, 1440 };
            int curRes = 1;
            if (s.windowedWidth == 1280)       curRes = 0;
            else if (s.windowedWidth == 2560)  curRes = 2;
            int newRes = DrawOptionRow("Resolution", kRes, 3, curRes,
                                       contentX, contentY + rowSpacing, 210.f, 56.f, mouse, mousePressed);
            if (newRes != curRes)
            {
                s.windowedWidth  = kResW[newRes];
                s.windowedHeight = kResH[newRes];
                _settingsMgr.ApplyWindow();
            }
        }

        static const char* kVSync[] = { "On", "Off" };
        int vsyncIdx = s.vsync ? 0 : 1;
        int newVSync = DrawOptionRow("VSync", kVSync, 2, vsyncIdx,
                                     contentX, contentY + rowSpacing * 2.f, 210.f, 56.f, mouse, mousePressed);
        if (newVSync != vsyncIdx)
        {
            s.vsync = (newVSync == 0);
            _settingsMgr.ApplyWindow();
        }
    }
    else if (_settingsTab == 1)
    {
        // -- Audio ------------------------------------------------------------
        const float trackX = contentX;
        const float trackW = 600.f;

        auto handleSlider = [&](int sliderIdx, float sliderY, float& value)
        {
            bool dragging  = (_settingsDragSlider == sliderIdx);
            bool overTrack = (mouse.x >= trackX - 26.f && mouse.x <= trackX + trackW + 26.f
                           && mouse.y >= sliderY - 26.f && mouse.y <= sliderY + 46.f);
            if (mousePressed && overTrack) { _settingsDragSlider = sliderIdx; dragging = true; }
            if (mouseReleased)              _settingsDragSlider = -1;
            if (mouseDown && dragging)
            {
                float rawValue = (mouse.x - trackX) / trackW;
                value = rawValue < 0.f ? 0.f : rawValue > 1.f ? 1.f : rawValue;
            }
            return dragging;
        };

        handleSlider(0, contentY,                  s.masterVolume);
        handleSlider(1, contentY + rowSpacing,     s.musicVolume);
        handleSlider(2, contentY + rowSpacing * 2, s.sfxVolume);

        if (_settingsDragSlider >= 0)
        {
            SetMasterVolume(s.masterVolume);
            _audio.SetMusicVolumeScale(s.musicVolume);
            _audio.SetSfxVolumeScale(s.sfxVolume);
            ApplySfxVolume();
        }
    }
    else
    {
        // -- Keybindings ------------------------------------------------------

        Rectangle aimModeToggle{ panelX + 620.f, contentY + 8.f, 420.f, 40.f };
        if (mousePressed && CheckCollisionPointRec(mouse, aimModeToggle))
        {
            s.abilityAimToggle = !s.abilityAimToggle;
            _settingsMgr.Save();
        }
        if (_settingsRebindSlot < 0 && _settingsGpRebindSlot < 0 &&
            IsGamepadAvailable(GamepadInput::kGamepad) &&
            IsGamepadButtonPressed(GamepadInput::kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
        {
            s.abilityAimToggle = !s.abilityAimToggle;
            _settingsMgr.Save();
        }

        // Sub-tab click (M&K vs Gamepad) - same geometry as DrawSettingsKeybindings
        const float subTabH   = 40.f;
        const float subTabW   = 160.f;
        const float subTabGap = 10.f;
        const float subTabY   = contentY + 8.f;
        if (mousePressed)
        {
            if (CheckCollisionPointRec(mouse, { panelX + 40.f, subTabY, subTabW, subTabH }))
            {
                _keybindSubTab        = 0;
                _settingsRebindSlot   = -1;
                _settingsGpRebindSlot = -1;
            }
            else if (CheckCollisionPointRec(mouse, { panelX + 40.f + subTabW + subTabGap, subTabY, subTabW, subTabH }))
            {
                _keybindSubTab        = 1;
                _settingsRebindSlot   = -1;
                _settingsGpRebindSlot = -1;
            }
        }

        // Rows start below the sub-tab strip
        const float rowsStartY = contentY + subTabH + 22.f;

        if (_keybindSubTab == 0)
        {
            // -- M&K sub-tab --------------------------------------------------
            if (_settingsRebindSlot >= 0)
            {
                for (int k = 32; k <= 348; k++)
                {
                    if (IsKeyPressed((KeyboardKey)k))
                    {
                        switch (_settingsRebindSlot)
                        {
                        case 0: _keybindingsEdit.moveUp     = (KeyboardKey)k; break;
                        case 1: _keybindingsEdit.moveDown   = (KeyboardKey)k; break;
                        case 2: _keybindingsEdit.moveLeft   = (KeyboardKey)k; break;
                        case 3: _keybindingsEdit.moveRight  = (KeyboardKey)k; break;
                        case 4: _keybindingsEdit.dash       = (KeyboardKey)k; break;
                        case 5: _keybindingsEdit.attack     = (KeyboardKey)k; break;
                        case 6: _keybindingsEdit.ability[0] = (KeyboardKey)k; break;
                        case 7: _keybindingsEdit.ability[1] = (KeyboardKey)k; break;
                        case 8: _keybindingsEdit.ability[2] = (KeyboardKey)k; break;
                        case 9: _keybindingsEdit.ability[3] = (KeyboardKey)k; break;
                        default: break;
                        }
                        _settingsRebindSlot = -1;
                        break;
                    }
                }
                return;
            }

            // Badge click detection (matches DrawSettingsKeybindings M&K layout)
            const float rowX   = panelX + 40.f;
            const float rowW   = panelW - 80.f;
            const float rowH   = 42.f;
            const float rowGap = 7.f;
            const float labelH = 24.f;
            const float grpGap = 18.f;
            const float badgeW = 150.f;
            const float badgeH = rowH * 0.72f;
            const float badgeX = rowX + rowW - badgeW - 16.f;
            static const bool kMKGroupStart[10] = { true, false, false, false, true, false, true, false, false, false };
            float curY = rowsStartY;
            for (int slotIdx = 0; slotIdx < 10; slotIdx++)
            {
                if (kMKGroupStart[slotIdx]) { if (slotIdx > 0) curY += grpGap; curY += labelH + 4.f; }
                float badgeY = curY + rowH * 0.5f - badgeH * 0.5f;
                if (mousePressed && CheckCollisionPointRec(mouse, { badgeX, badgeY, badgeW, badgeH }))
                    _settingsRebindSlot = slotIdx;
                curY += rowH + rowGap;
            }
        }
        else
        {
            // -- Gamepad sub-tab ----------------------------------------------
            if (_settingsGpRebindSlot >= 0)
            {
                // Scan for any gamepad button press and assign it
                if (IsGamepadAvailable(GamepadInput::kGamepad))
                {
                    for (int b = 1; b <= 17; b++)
                    {
                        if (IsGamepadButtonPressed(GamepadInput::kGamepad, (GamepadButton)b))
                        {
                            GamepadButton pressed = (GamepadButton)b;
                            switch (_settingsGpRebindSlot)
                            {
                            case 0: _gamepadBindingsEdit.attack      = pressed; break;
                            case 1: _gamepadBindingsEdit.dash        = pressed; break;
                            case 2: _gamepadBindingsEdit.pause       = pressed; break;
                            case 3: _gamepadBindingsEdit.ability[0]  = pressed; break;
                            case 4: _gamepadBindingsEdit.ability[1]  = pressed; break;
                            case 5: _gamepadBindingsEdit.ability[2]  = pressed; break;
                            case 6: _gamepadBindingsEdit.ability[3]  = pressed; break;
                            default: break;
                            }
                            _settingsGpRebindSlot = -1;
                            SaveKeybindings();
                            break;
                        }
                    }
                }
                return;
            }

            // Badge click detection (matches DrawSettingsKeybindings Gamepad layout)
            const float rowX   = panelX + 40.f;
            const float rowW   = panelW - 80.f;
            const float rowH   = 42.f;
            const float rowGap = 7.f;
            const float labelH = 24.f;
            const float grpGap = 18.f;
            const float badgeW = 150.f;
            const float badgeH = rowH * 0.72f;
            const float badgeX = rowX + rowW - badgeW - 16.f;
            // 7 slots: Attack, Dash, Pause  |  Ability 1-4
            static const bool kGPGroupStart[7] = { true, false, false, true, false, false, false };
            float curY = rowsStartY;
            for (int slotIdx = 0; slotIdx < 7; slotIdx++)
            {
                if (kGPGroupStart[slotIdx]) { if (slotIdx > 0) curY += grpGap; curY += labelH + 4.f; }
                float badgeY = curY + rowH * 0.5f - badgeH * 0.5f;
                if (mousePressed && CheckCollisionPointRec(mouse, { badgeX, badgeY, badgeW, badgeH }))
                    _settingsGpRebindSlot = slotIdx;
                curY += rowH + rowGap;
            }
        }
    }
}

void Engine::DrawSettings() const
{
    const GameSettings& s  = _settingsMgr.Get();
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    // Scrolling checkerboard background - blue-navy tones
    DrawScrollingCheckerboard(sw, sh,
        Color{14, 28, 52, 255}, Color{20, 42, 76, 255}, 20.f, 11.f);

    // Panel layout
    const float panelW = 1100.f;
    const float panelH = 880.f;
    const float panelX = sw * 0.5f - panelW * 0.5f;
    const float panelY = sh * 0.5f - panelH * 0.5f;

    // Nine-slice border slightly outside the panel
    if (_settingsBorderTex.id != 0)
    {
        const float borderPad = 24.f;
        DrawNineSlice(_settingsBorderTex, 18.f, 22.f,
            { panelX - borderPad, panelY - borderPad,
              panelW + borderPad * 2.f, panelH + borderPad * 2.f }, WHITE);
    }

    // Dark panel interior
    DrawRectangleRounded({ panelX, panelY, panelW, panelH },
                          0.02f, 8, Fade(Color{8, 14, 26, 255}, 0.97f));

    // Title
    const char* title = "SETTINGS";
    const int titleSz = 58;
    int titleW = MeasureText(title, titleSz);
    DrawText(title,
             (int)(panelX + panelW * 0.5f - titleW * 0.5f),
             (int)(panelY + 22.f),
             titleSz, Color{255, 214, 102, 255});

    // Tab strip
    const float tabY   = panelY + 85.f;
    const float tabH   = 52.f;
    const float tabGap = 14.f;
    const float kTabW[3]           = { 195.f, 175.f, 225.f };
    static const char* kTabLabels[3] = { "DISPLAY", "AUDIO", "KEYBINDINGS" };
    bool gpAvail = IsGamepadAvailable(0);
    float tabX = panelX + 40.f;
    for (int tabIdx = 0; tabIdx < 3; tabIdx++)
    {
        bool   active     = (_settingsTab == tabIdx);
        bool   gpCursor   = gpAvail && (_settingsGpSection == 0) && (_settingsGpTabCursor == tabIdx);
        Color  tabFill    = active ? Color{130, 235, 255, 200} : Color{30, 55, 70, 180};
        Color  tabBorder  = active ? Color{130, 235, 255, 255} : Color{70, 120, 150, 140};
        if (gpCursor && !active)
            DrawRectangleRoundedLines({ tabX - 3.f, tabY - 3.f, kTabW[tabIdx] + 6.f, tabH + 6.f },
                                      0.25f, 6, Color{255, 200, 0, 255});
        DrawRectangleRounded    ({ tabX, tabY, kTabW[tabIdx], tabH }, 0.25f, 6, tabFill);
        DrawRectangleRoundedLines({ tabX, tabY, kTabW[tabIdx], tabH }, 0.25f, 6, tabBorder);
        int tabFs = 30;
        int tabTw = MeasureText(kTabLabels[tabIdx], tabFs);
        DrawText(kTabLabels[tabIdx],
                 (int)(tabX + kTabW[tabIdx] * 0.5f - tabTw * 0.5f),
                 (int)(tabY + tabH * 0.5f - tabFs * 0.5f),
                 tabFs, active ? BLACK : RAYWHITE);
        tabX += kTabW[tabIdx] + tabGap;
    }

    // Divider below tabs
    float divY = tabY + tabH + 12.f;
    DrawLineEx({ panelX + 20.f, divY }, { panelX + panelW - 20.f, divY }, 1.f,
               Fade(Color{130, 235, 255, 255}, 0.30f));

    const float contentX   = panelX + 400.f;
    const float contentY   = panelY + 175.f;
    const float rowSpacing = 110.f;

    Vector2 mouse = GetVirtualMousePos();
    if (GetTouchPointCount() > 0) mouse = GetVirtualTouchPos(0);

    if (_settingsTab == 0)
    {
        // Display tab
        static const char* kModes[] = { "Fullscreen", "Borderless", "Windowed" };
        DrawOptionRow("Window Mode", kModes, 3, (int)s.windowMode,
                      contentX, contentY, 210.f, 56.f, mouse, false);

        if (s.windowMode == GameSettings::WindowMode::Windowed)
        {
            static const char* kRes[] = { "1280x720", "1920x1080", "2560x1440" };
            int curRes = 1;
            if (s.windowedWidth == 1280)       curRes = 0;
            else if (s.windowedWidth == 2560)  curRes = 2;
            DrawOptionRow("Resolution", kRes, 3, curRes,
                          contentX, contentY + rowSpacing, 210.f, 56.f, mouse, false);
        }

        static const char* kVSync[] = { "On", "Off" };
        DrawOptionRow("VSync", kVSync, 2, s.vsync ? 0 : 1,
                      contentX, contentY + rowSpacing * 2.f, 210.f, 56.f, mouse, false);
    }
    else if (_settingsTab == 1)
    {
        // Audio tab
        const float trackX = contentX;
        const float trackW = 600.f;
        bool mouseDown = IsMouseButtonDown(MOUSE_LEFT_BUTTON) || (GetTouchPointCount() > 0);
        bool mouseRel  = IsMouseButtonReleased(MOUSE_LEFT_BUTTON);
        bool gpInAudio = gpAvail && (_settingsGpSection == 2);
        if (gpInAudio && _settingsGpContentRow == 0)
            DrawRectangleRoundedLines({ trackX - 330.f, contentY - 24.f, trackX + trackW + 80.f - (trackX - 330.f), 52.f },
                                      0.1f, 4, Color{255, 200, 0, 160});
        DrawSettingsSlider("Master Volume", s.masterVolume,
                           trackX, contentY, trackW,
                           (_settingsDragSlider == 0) || (gpInAudio && _settingsGpContentRow == 0), mouse, mouseDown, mouseRel);
        if (gpInAudio && _settingsGpContentRow == 1)
            DrawRectangleRoundedLines({ trackX - 330.f, contentY + rowSpacing - 24.f, trackX + trackW + 80.f - (trackX - 330.f), 52.f },
                                      0.1f, 4, Color{255, 200, 0, 160});
        DrawSettingsSlider("Music Volume", s.musicVolume,
                           trackX, contentY + rowSpacing, trackW,
                           (_settingsDragSlider == 1) || (gpInAudio && _settingsGpContentRow == 1), mouse, mouseDown, mouseRel);
        if (gpInAudio && _settingsGpContentRow == 2)
            DrawRectangleRoundedLines({ trackX - 330.f, contentY + rowSpacing * 2.f - 24.f, trackX + trackW + 80.f - (trackX - 330.f), 52.f },
                                      0.1f, 4, Color{255, 200, 0, 160});
        DrawSettingsSlider("SFX Volume", s.sfxVolume,
                           trackX, contentY + rowSpacing * 2.f, trackW,
                           (_settingsDragSlider == 2) || (gpInAudio && _settingsGpContentRow == 2), mouse, mouseDown, mouseRel);
    }
    else
    {
        // Keybindings tab
        DrawSettingsKeybindings(contentY, panelX, panelW, mouse);
    }

    // Back button - sits below the nine-slice border PNG (borderPad=24 + 8 gap)
    float backX = panelX + panelW * 0.5f - 140.f;
    float backY = panelY + panelH + 32.f;
    Rectangle backBtn = { backX, backY, 280.f, 55.f };
    bool backHov    = CheckCollisionPointRec(mouse, backBtn);
    bool gpBackFocus = IsGamepadAvailable(0) && (_settingsGpSection == 3);
    if (gpBackFocus)
        DrawRectangleRoundedLines({ backX - 4.f, backY - 4.f, 288.f, 63.f }, 0.25f, 6, Color{255, 200, 0, 255});
    DrawRectangleRounded(backBtn, 0.25f, 6,
        (backHov || gpBackFocus) ? Color{60, 110, 140, 240} : Color{20, 40, 55, 200});
    DrawRectangleRoundedLines(backBtn, 0.25f, 6, Color{130, 235, 255, 160});
    int bfs = 32;
    int bw  = MeasureText("BACK", bfs);
    DrawText("BACK",
             (int)(backX + 140.f - bw * 0.5f),
             (int)(backY + 27.5f - bfs * 0.5f),
             bfs, RAYWHITE);
}

void Engine::DrawSettingsKeybindings(float contentY, float panelX, float panelW, Vector2 mouse) const
{
    const float rowX   = panelX + 40.f;
    const float rowW   = panelW - 80.f;
    const float rowH   = 42.f;
    const float rowGap = 7.f;
    const float labelH = 24.f;
    const float grpGap = 18.f;
    const float badgeW = 150.f;
    const float badgeH = rowH * 0.72f;
    const float badgeX = rowX + rowW - badgeW - 16.f;

    // -- Sub-tab strip (M&K / GAMEPAD) ----------------------------------------
    const float subTabH   = 40.f;
    const float subTabW   = 160.f;
    const float subTabGap = 10.f;
    const float subTabY   = contentY + 8.f;

    static const char* kSubTabs[2] = { "M&K", "GAMEPAD" };
    float stX = panelX + 40.f;
    bool gpSubAvail = IsGamepadAvailable(0);
    for (int st = 0; st < 2; st++)
    {
        bool active      = (_keybindSubTab == st);
        bool gpSubCursor = gpSubAvail && (_settingsGpSection == 1) && (_settingsGpSubCursor == st);
        Color fill       = active ? Color{130, 235, 255, 200} : Color{30, 55, 70, 180};
        Color border     = active ? Color{130, 235, 255, 255} : Color{70, 120, 150, 140};
        if (gpSubCursor && !active)
            DrawRectangleRoundedLines({ stX - 3.f, subTabY - 3.f, subTabW + 6.f, subTabH + 6.f },
                                      0.25f, 6, Color{255, 200, 0, 255});
        DrawRectangleRounded     ({ stX, subTabY, subTabW, subTabH }, 0.25f, 6, fill);
        DrawRectangleRoundedLines({ stX, subTabY, subTabW, subTabH }, 0.25f, 6, border);
        int stFs = 28;
        int stTw = MeasureText(kSubTabs[st], stFs);
        DrawText(kSubTabs[st],
                 (int)(stX + subTabW * 0.5f - stTw * 0.5f),
                 (int)(subTabY + subTabH * 0.5f - stFs * 0.5f),
                 stFs, active ? BLACK : RAYWHITE);
        stX += subTabW + subTabGap;
    }

    const GameSettings& settings = _settingsMgr.Get();
    Rectangle aimModeToggle{ panelX + 620.f, subTabY, 420.f, subTabH };
    bool aimHovered = CheckCollisionPointRec(mouse, aimModeToggle);
    DrawRectangleRounded(aimModeToggle, 0.2f, 6,
        aimHovered ? Color{ 65, 105, 125, 230 } : Color{ 30, 55, 70, 200 });
    DrawRectangleRoundedLines(aimModeToggle, 0.2f, 6, Color{ 130, 235, 255, 180 });
    const char* aimMode = settings.abilityAimToggle ? "AIM: PRESS / PRESS" : "AIM: HOLD / RELEASE";
    DrawText(aimMode, (int)aimModeToggle.x + 14, (int)aimModeToggle.y + 8, 22, RAYWHITE);
    DrawText("Y", (int)(aimModeToggle.x + aimModeToggle.width - 34.f), (int)aimModeToggle.y + 8, 22, GOLD);

    // Divider below sub-tabs
    float subDivY = subTabY + subTabH + 8.f;
    DrawLineEx({ rowX, subDivY }, { rowX + rowW, subDivY }, 1.f,
               Fade(Color{130, 235, 255, 255}, 0.25f));

    // Rows start below the sub-tab strip
    const float rowsStartY = contentY + subTabH + 22.f;

    if (_keybindSubTab == 0)
    {
        // -- M&K --------------------------------------------------------------
        struct SlotDef { const char* groupLabel; const char* name; };
        static const SlotDef mkSlots[10] = {
            { "MOVEMENT",  "Move Up"    },
            { nullptr,     "Move Down"  },
            { nullptr,     "Move Left"  },
            { nullptr,     "Move Right" },
            { "ACTIONS",   "Dash"       },
            { nullptr,     "Attack"     },
            { "ABILITIES", "Ability 1"  },
            { nullptr,     "Ability 2"  },
            { nullptr,     "Ability 3"  },
            { nullptr,     "Ability 4"  },
        };
        auto getMKKey = [&](int i) -> KeyboardKey {
            switch (i) {
            case 0: return _keybindingsEdit.moveUp;
            case 1: return _keybindingsEdit.moveDown;
            case 2: return _keybindingsEdit.moveLeft;
            case 3: return _keybindingsEdit.moveRight;
            case 4: return _keybindingsEdit.dash;
            case 5: return _keybindingsEdit.attack;
            case 6: return _keybindingsEdit.ability[0];
            case 7: return _keybindingsEdit.ability[1];
            case 8: return _keybindingsEdit.ability[2];
            default: return _keybindingsEdit.ability[3];
            }
        };

        float curY = rowsStartY;
        for (int i = 0; i < 10; i++)
        {
            if (mkSlots[i].groupLabel)
            {
                if (i > 0) curY += grpGap;
                DrawText(mkSlots[i].groupLabel, (int)rowX, (int)curY, (int)labelH, Color{255, 214, 102, 255});
                curY += labelH + 4.f;
            }
            DrawRectangleRounded({ rowX, curY, rowW, rowH }, 0.2f, 4, Color{15, 40, 55, 180});
            const int nameFsz = 26;
            DrawText(mkSlots[i].name, (int)(rowX + 16.f),
                     (int)(curY + rowH * 0.5f - nameFsz * 0.5f),
                     nameFsz, Color{200, 225, 240, 220});

            bool  awaiting   = (_settingsRebindSlot == i);
            bool  gpRowFocus = IsGamepadAvailable(0) && (_settingsGpSection == 2) && (_settingsGpContentRow == i);
            float badgeY     = curY + rowH * 0.5f - badgeH * 0.5f;
            bool  hovered    = CheckCollisionPointRec(mouse, { badgeX, badgeY, badgeW, badgeH });
            if (gpRowFocus)
                DrawRectangleRoundedLines({ rowX - 2.f, curY - 2.f, rowW + 4.f, rowH + 4.f },
                                          0.2f, 4, Color{255, 200, 0, 200});
            Color badgeFill  = awaiting ? Color{255, 200, 60, 230} : (gpRowFocus || hovered) ? Color{80, 180, 80, 220} : Color{40, 130, 70, 200};
            Color badgeBorder= awaiting ? Color{255, 230, 100, 255} : Color{80, 200, 80, 180};
            DrawRectangleRounded     ({ badgeX, badgeY, badgeW, badgeH }, 0.3f, 6, badgeFill);
            DrawRectangleRoundedLines({ badgeX, badgeY, badgeW, badgeH }, 0.3f, 6, badgeBorder);
            const char* keyLabel = awaiting ? "PRESS KEY..." : GetKeyName(getMKKey(i));
            int keyFsz = awaiting ? 20 : 26;
            int keyTw  = MeasureText(keyLabel, keyFsz);
            DrawText(keyLabel, (int)(badgeX + badgeW * 0.5f - keyTw * 0.5f),
                     (int)(badgeY + badgeH * 0.5f - keyFsz * 0.5f),
                     keyFsz, awaiting ? BLACK : RAYWHITE);
            curY += rowH + rowGap;
        }

        const char* hint = (_settingsRebindSlot >= 0)
            ? "Press ESC / B to cancel  |  Press any key to assign"
            : "Click a badge or press A to rebind";
        int hintFsz = 22;
        int hintTw  = MeasureText(hint, hintFsz);
        DrawText(hint, (int)(rowX + rowW * 0.5f - hintTw * 0.5f),
                 (int)(rowsStartY + (rowH + rowGap) * 10 + grpGap * 3 + 12.f),
                 hintFsz, Color{140, 180, 200, 160});
    }
    else
    {
        // -- Gamepad -----------------------------------------------------------
        struct GpSlotDef { const char* groupLabel; const char* name; };
        static const GpSlotDef gpSlots[7] = {
            { "ACTIONS",   "Attack"    },
            { nullptr,     "Dash"      },
            { nullptr,     "Pause"     },
            { "ABILITIES", "Ability 1" },
            { nullptr,     "Ability 2" },
            { nullptr,     "Ability 3" },
            { nullptr,     "Ability 4" },
        };
        auto getGpButton = [&](int i) -> GamepadButton {
            switch (i) {
            case 0: return _gamepadBindingsEdit.attack;
            case 1: return _gamepadBindingsEdit.dash;
            case 2: return _gamepadBindingsEdit.pause;
            case 3: return _gamepadBindingsEdit.ability[0];
            case 4: return _gamepadBindingsEdit.ability[1];
            case 5: return _gamepadBindingsEdit.ability[2];
            default: return _gamepadBindingsEdit.ability[3];
            }
        };

        // Left-stick info row at the top
        float infoY = rowsStartY;
        DrawRectangleRounded({ rowX, infoY, rowW, rowH }, 0.2f, 4, Color{10, 30, 45, 160});
        const int infoFs = 24;
        DrawText("Move", (int)(rowX + 16.f), (int)(infoY + rowH * 0.5f - infoFs * 0.5f),
                 infoFs, Color{200, 225, 240, 180});
        const char* stickLabel = "Left Stick";
        int stickTw = MeasureText(stickLabel, infoFs);
        DrawText(stickLabel, (int)(badgeX + badgeW * 0.5f - stickTw * 0.5f),
                 (int)(infoY + rowH * 0.5f - infoFs * 0.5f),
                 infoFs, Color{160, 200, 220, 160});
        float curY = infoY + rowH + rowGap + 6.f;

        for (int i = 0; i < 7; i++)
        {
            if (gpSlots[i].groupLabel)
            {
                if (i > 0) curY += grpGap;
                DrawText(gpSlots[i].groupLabel, (int)rowX, (int)curY, (int)labelH, Color{255, 214, 102, 255});
                curY += labelH + 4.f;
            }
            DrawRectangleRounded({ rowX, curY, rowW, rowH }, 0.2f, 4, Color{15, 40, 55, 180});
            const int nameFsz = 26;
            DrawText(gpSlots[i].name, (int)(rowX + 16.f),
                     (int)(curY + rowH * 0.5f - nameFsz * 0.5f),
                     nameFsz, Color{200, 225, 240, 220});

            bool  awaiting   = (_settingsGpRebindSlot == i);
            bool  gpRowFocus = IsGamepadAvailable(0) && (_settingsGpSection == 2) && (_settingsGpContentRow == i);
            float badgeY     = curY + rowH * 0.5f - badgeH * 0.5f;
            bool  hovered    = CheckCollisionPointRec(mouse, { badgeX, badgeY, badgeW, badgeH });
            if (gpRowFocus)
                DrawRectangleRoundedLines({ rowX - 2.f, curY - 2.f, rowW + 4.f, rowH + 4.f },
                                          0.2f, 4, Color{255, 200, 0, 200});
            Color badgeFill  = awaiting ? Color{255, 200, 60, 230} : (gpRowFocus || hovered) ? Color{80, 180, 80, 220} : Color{40, 130, 70, 200};
            Color badgeBorder= awaiting ? Color{255, 230, 100, 255} : Color{80, 200, 80, 180};
            DrawRectangleRounded     ({ badgeX, badgeY, badgeW, badgeH }, 0.3f, 6, badgeFill);
            DrawRectangleRoundedLines({ badgeX, badgeY, badgeW, badgeH }, 0.3f, 6, badgeBorder);

            const char* btnLabel = awaiting ? "PRESS BUTTON..." : GetGamepadButtonName(getGpButton(i));
            int keyFsz = awaiting ? 18 : 22;
            int keyTw  = MeasureText(btnLabel, keyFsz);
            DrawText(btnLabel, (int)(badgeX + badgeW * 0.5f - keyTw * 0.5f),
                     (int)(badgeY + badgeH * 0.5f - keyFsz * 0.5f),
                     keyFsz, awaiting ? BLACK : RAYWHITE);
            curY += rowH + rowGap;
        }

        const char* hint = (_settingsGpRebindSlot >= 0)
            ? "Press ESC / B to cancel  |  Press any button to assign"
            : "Press A on a row to rebind  |  Left stick always moves";
        int hintFsz = 22;
        int hintTw  = MeasureText(hint, hintFsz);
        DrawText(hint, (int)(rowX + rowW * 0.5f - hintTw * 0.5f),
                 (int)(curY + 12.f), hintFsz, Color{140, 180, 200, 160});
    }
}

// -- Boss-clear choice ----------------------------------------------------------
// Fires after EVERY boss (from the boss exit trigger, in place of jumping
// straight to the world map). The player weighs safety against greed:
//   Return to village  -> bank the run: keep your hauled gold, spend it at the
//                         village build menu, then start a fresh run at the gate.
//   Push onward        -> double-or-nothing: full heal + bonus gold now, the
//                         cursed wager unlocks for the next shop, and this same
//                         character presses on to the next domain. Die and it's
//                         all gone.
void Engine::OpenBossChoice()
{
    if (_worldZone < 4 && _worldMapPreparedZone != _worldZone + 1)
    {
        _worldMap.Generate(_worldCompletedBiomes, _worldChosenNodeIndices,
                           _worldZone + 1, kVirtualWidth, kVirtualHeight);
        _worldMapPreparedZone = _worldZone + 1;
        _runSessionData.MarkWorldMapGenerated();
    }
    _bossChoiceCursor    = 0;      // default-highlight the safe option
    _bossChoiceOpenTimer = 0.30f;  // swallow the exit-tile input for a beat
    _gameState           = GameState::BossChoice;
}

// Shared card geometry (Update + Draw MUST agree).
static void BossChoiceCardRects(Rectangle& left, Rectangle& right)
{
    const float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;
    const float cardW = sw * 0.30f, cardH = sh * 0.46f, gap = sw * 0.05f;
    const float totalW = cardW * 2.f + gap;
    const float x0 = sw * 0.5f - totalW * 0.5f;
    const float y  = sh * 0.5f - cardH * 0.5f + sh * 0.04f;
    left  = { x0,                 y, cardW, cardH };
    right = { x0 + cardW + gap,   y, cardW, cardH };
}

void Engine::UpdateBossChoice()
{
    const float dt = GetFrameTime();
    if (_bossChoiceOpenTimer > 0.f) { _bossChoiceOpenTimer -= dt; return; }
    _gamepad.Update(_gamepadBindingsEdit);

    Rectangle left, right;
    BossChoiceCardRects(left, right);
    Vector2 mouse = GetVirtualMousePos();

    if (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A)) _bossChoiceCursor = 0;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) _bossChoiceCursor = 1;
    if (_gamepad.isActive)
    {
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT))  _bossChoiceCursor = 0;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) _bossChoiceCursor = 1;
    }
    if (CheckCollisionPointRec(mouse, left))  _bossChoiceCursor = 0;
    if (CheckCollisionPointRec(mouse, right)) _bossChoiceCursor = 1;

    bool clickL = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, left);
    bool clickR = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, right);
    bool confirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                   (_gamepad.isActive && _gamepad.menuConfirmPressed);

    if (clickL || (confirm && _bossChoiceCursor == 0))
    {
        StopSound(_buttonPressSound); PlaySound(_buttonPressSound);
        _runSessionData.PauseInVillage();
        EnterVillage();
        _fadeInTimer = 1.0f; _fadeInDuration = 1.0f;
        return;
    }
    if (clickR || (confirm && _bossChoiceCursor == 1))
    {
        // Push onward keeps the boss-fight damage and mana expenditure.
        StopSound(_buttonPressSound); PlaySound(_buttonPressSound);
        _player.AddGold(50 + _worldZone * 50);     // bravery bounty (tunable)
        _wagerAccessGranted = false;
        OpenWorldMap();
        return;
    }
}

void Engine::DrawBossChoice()
{
    const float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;
    DrawRectangle(0, 0, (int)sw, (int)sh, Color{ 10, 8, 16, 255 });

    const char* title = "The boss lies broken. What now?";
    int tFs = 46, tTw = MeasureText(title, tFs);
    DrawText(title, (int)(sw * 0.5f - tTw * 0.5f), (int)(sh * 0.16f), tFs, Color{ 235, 228, 245, 255 });

    Rectangle left, right;
    BossChoiceCardRects(left, right);

    struct CardInfo { Rectangle rec; const char* head; const char* sub; std::vector<const char*> lines; Color accent; };
    CardInfo cards[2] = {
        { left,  "Return to Village", "Safe",
          { "Pause this journey.", "Restore health and mana,", "visit Zeph, then resume", "the same world-map path.", "", "Your run remains active." },
          Color{ 110, 190, 130, 255 } },
        { right, "Push Onward", "Double or Nothing",
          { "Press deeper, same soul.", "Take a gold bounty,", "but restore no health", "and restore no mana.", "", "Fall now and lose it all." },
          Color{ 220, 90, 110, 255 } },
    };

    for (int i = 0; i < 2; i++)
    {
        const CardInfo& c = cards[i];
        bool sel = (_bossChoiceCursor == i);
        Color body = sel ? Color{ 34, 30, 48, 255 } : Color{ 22, 20, 32, 245 };
        DrawRectangleRounded(c.rec, 0.06f, 10, body);
        DrawRectangleRoundedLinesEx(c.rec, 0.06f, 10, sel ? 5.f : 2.f,
                                    sel ? c.accent : Color{ 70, 66, 86, 255 });

        int hFs = 34, hTw = MeasureText(c.head, hFs);
        DrawText(c.head, (int)(c.rec.x + c.rec.width * 0.5f - hTw * 0.5f), (int)(c.rec.y + 26.f), hFs, c.accent);
        int sFs = 20, sTw = MeasureText(c.sub, sFs);
        DrawText(c.sub, (int)(c.rec.x + c.rec.width * 0.5f - sTw * 0.5f), (int)(c.rec.y + 68.f), sFs, Color{ 190, 184, 205, 255 });

        float ly = c.rec.y + 120.f;
        for (const char* ln : c.lines)
        {
            int lFs = 22, lTw = MeasureText(ln, lFs);
            DrawText(ln, (int)(c.rec.x + c.rec.width * 0.5f - lTw * 0.5f), (int)ly, lFs, Color{ 210, 204, 222, 235 });
            ly += lFs + 10.f;
        }
    }

    const char* hint = "A / D  or  <  >  to choose      Enter / click to confirm";
    int hFs = 22, hTw = MeasureText(hint, hFs);
    DrawText(hint, (int)(sw * 0.5f - hTw * 0.5f), (int)(sh * 0.90f), hFs, Color{ 150, 145, 165, 220 });
}

// -- World Map ------------------------------------------------------------------

void Engine::OpenWorldMap()
{
    // Victory: the final domain (Demon's Insides, zone 5) boss has been cleared.
    // This is the true end of a full run — all six domains beaten.
    if (_worldZone >= 5)
    {
        _demoCompleted = true;
        _meta.RecordGameCompleted();
        _gameState     = GameState::DemoEnd;
        return;
    }

    // Zone 4 boss just cleared ? skip map, go straight to the final domain.
    if (_worldZone >= 4)
    {
        _worldZone = 5;
        _currentBiome = Biome::DemonsInsides;
        _useHandcraftedDungeonRooms = false;
        _handcraftedDungeonRoomIds.clear();
        LoadTilesetForBiome(_currentBiome);
        _dungeonGen.Generate();
        int startIdx = _dungeonGen.GetStartIndex();
        EnterDungeonRoom(startIdx, DungeonDoorSide::None, GetDungeonEntranceSpawnPos(), true);
        // FadingIn was already set by the fade handler - just reset the timer/alpha.
        _dungeonFadeState = DungeonFadeState::FadingIn;
        _dungeonFadeTimer = kDungeonFadeDuration;
        _dungeonFadeAlpha = 255.f;
        return;
    }

    if (_worldMapPreparedZone == _worldZone + 1)
    {
        _worldMap.Reopen();
    }
    else
    {
        _worldMap.Generate(_worldCompletedBiomes, _worldChosenNodeIndices,
                           _worldZone + 1, kVirtualWidth, kVirtualHeight);
        _worldMapPreparedZone = _worldZone + 1;
        _runSessionData.MarkWorldMapGenerated();
    }

    _gameState = GameState::WorldMap;
}

void Engine::UpdateWorldMap(float dt)
{
    // Allow pausing while on the map.
    if (IsKeyPressed(KEY_ESCAPE))
    {
        _stateBeforePause = GameState::WorldMap;
        _gameState = GameState::Pause;
        return;
    }
    if (IsGamepadAvailable(0) && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT))
    {
        _stateBeforePause = GameState::WorldMap;
        _gameState        = GameState::Pause;
        return;
    }

    // Debug editor toggle (KEY_NINE, same as the act-map editor).
    if (_debug.IsActive() && IsKeyPressed(KEY_NINE))
        _worldMap.ToggleEditor();

    if (_worldMap.IsEditorActive())
    {
        _worldMap.UpdateEditor();
        return;
    }

    _worldMap.SetPromptMode(GetPromptModeForUi());
    bool done = _worldMap.Update(dt);
    if (!done) return;

    // Player confirmed a biome - advance zone and load the new dungeon.
    Biome selectedBiome   = _worldMap.GetSelectedBiome();
    int   selectedTierIdx = _worldMap.GetSelectedTierIdx();

    _worldZone++;
    _worldCompletedBiomes.push_back(selectedBiome);
    _worldChosenNodeIndices.push_back(selectedTierIdx);
    _runSessionData.RecordMapChoice(selectedBiome, selectedTierIdx);
    _worldMapPreparedZone = -1;

    _currentBiome = selectedBiome;
    // Forest is the first complete authored library. Other regions retain their
    // existing rooms until their .mroom coverage is finished, then they can use
    // the same assignment path without changing runtime rendering again.
    _useHandcraftedDungeonRooms = _currentBiome == Biome::Forest;
    _handcraftedDungeonRoomIds.clear();
    LoadTilesetForBiome(_currentBiome);

    if (_useHandcraftedDungeonRooms)
    {
        RefreshHandcraftedRooms();
        if (!GenerateHandcraftedDungeon(_currentBiome, _handcraftedDungeonRoomIds))
        {
            _message = "This region's handcrafted room library is incomplete";
            _gameState = GameState::WorldMap;
            return;
        }
    }
    else
        _dungeonGen.Generate();
    int startIdx   = _dungeonGen.GetStartIndex();
    Vector2 spawnPos = GetDungeonEntranceSpawnPos();
    EnterDungeonRoom(startIdx, DungeonDoorSide::None, spawnPos, true);

    // Fade in to the newly generated biome entrance.
    _dungeonFadeState = DungeonFadeState::FadingIn;
    _dungeonFadeTimer = kDungeonFadeDuration;
    _dungeonFadeAlpha = 255.f;
}

void Engine::DrawWorldMap()
{
    _worldMap.Draw(_player);

    if (_worldMap.IsEditorActive())
        _worldMap.DrawEditor();
}

void Engine::InitBiomeModifierRoom()
{
    _wastelandHazards.clear();
    _wastelandHazardTimer = kWastelandHazardInterval;
    _lostCityBeams.clear();
    _sanctuaryZones.clear();
    _demonPulseTimer = 0.f;
    _player.ClearBiomeDebuffs();

    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    if (_currentBiome == Biome::LostCity)
    {
        LostCityBeam beam1{};
        beam1.center       = { sw * 0.28f, sh * 0.30f };
        beam1.angle        = 0.f;
        beam1.rotSpeed     = kLostCityBeamRotSpeed;
        beam1.length       = kLostCityBeamLength;
        beam1.damageCooldown = 0.f;

        LostCityBeam beam2{};
        beam2.center       = { sw * 0.72f, sh * 0.70f };
        beam2.angle        = 1.57f;
        beam2.rotSpeed     = -kLostCityBeamRotSpeed;
        beam2.length       = kLostCityBeamLength;
        beam2.damageCooldown = 0.f;

        _lostCityBeams.push_back(beam1);
        _lostCityBeams.push_back(beam2);
    }
    else if (_currentBiome == Biome::TheSanctuary)
    {
        // Build a list of prop world-space centers so zones don't land on top of them.
        const float cellW = sw / (float)RoomLayout::kCols;
        const float cellH = sh / (float)RoomLayout::kRows;
        std::vector<Vector2> propCenters;
        for (const auto& sp : _dungeonRoomLayout.props)
            propCenters.push_back({ sp.col * cellW + cellW * 0.5f, sp.row * cellH + cellH * 0.5f });
        for (const auto& sp : _dungeonRoomLayout.animProps)
            propCenters.push_back({ sp.col * cellW + cellW * 0.5f, sp.row * cellH + cellH * 0.5f });

        int zoneCount = GetRandomValue(2, 3);
        static constexpr float kZoneMargin = 160.f;
        for (int z = 0; z < zoneCount; z++)
        {
            for (int attempt = 0; attempt < 30; attempt++)
            {
                Vector2 candidate{
                    (float)GetRandomValue((int)kZoneMargin, (int)(sw - kZoneMargin)),
                    (float)GetRandomValue((int)kZoneMargin, (int)(sh - kZoneMargin))
                };
                // Skip if too close to another zone.
                bool overlaps = false;
                for (const auto& existing : _sanctuaryZones)
                    if (Vector2Distance(candidate, existing.pos) < kSanctuaryZoneRadius * 2.2f)
                    { overlaps = true; break; }
                if (overlaps) continue;
                // Skip if too close to any prop.
                for (const auto& pc : propCenters)
                    if (Vector2Distance(candidate, pc) < kSanctuaryZoneRadius * 0.9f)
                    { overlaps = true; break; }
                if (overlaps) continue;

                _sanctuaryZones.push_back({ candidate, kSanctuaryZoneRadius });
                break;
            }
        }
    }
}

void Engine::UpdateBiomeModifiers(float dt)
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    Vector2 playerWorldPos = _player.GetWorldPos();

    // Sanctuary debuffs are re-applied below when the player is inside a zone;
    // clearing them here lets them drop the moment the player steps out.
    _player.ClearBiomeDebuffs();

    switch (_currentBiome)
    {
    case Biome::Wastelands:
    {
        _wastelandHazardTimer -= dt;
        if (_wastelandHazardTimer <= 0.f)
        {
            _wastelandHazardTimer = kWastelandHazardInterval;
            Vector2 hazardPos{
                (float)GetRandomValue(150, (int)(sw - 150)),
                (float)GetRandomValue(150, (int)(sh - 150))
            };
            _wastelandHazards.push_back({ hazardPos, kWastelandWarningDuration, 0.f, 0.f, false });
        }

        for (auto& hazard : _wastelandHazards)
        {
            if (!hazard.exploded)
            {
                hazard.warningTimer -= dt;
                if (hazard.warningTimer <= 0.f)
                {
                    hazard.exploded    = true;
                    hazard.activeTimer = kWastelandActiveDuration;
                    StopSound(_explosionSound);
                    PlaySound(_explosionSound);
                }
            }
            else
            {
                hazard.activeTimer -= dt;
                if (hazard.dmgCooldown > 0.f) hazard.dmgCooldown -= dt;

                // Damage zone is active for the full sprite duration.
                if (hazard.dmgCooldown <= 0.f &&
                    Vector2Distance(_player.GetFeetWorldPos(), hazard.pos) <= kWastelandExplosionRadius)
                {
                    _player.TakeDamage(kWastelandDamage, hazard.pos);
                    hazard.dmgCooldown = 0.3f;
                }
            }
        }

        _wastelandHazards.erase(
            std::remove_if(_wastelandHazards.begin(), _wastelandHazards.end(),
                [](const WastelandHazard& h){ return h.exploded && h.activeTimer <= 0.f; }),
            _wastelandHazards.end());
        break;
    }

    case Biome::LostCity:
    {
        for (auto& beam : _lostCityBeams)
        {
            beam.angle += beam.rotSpeed * dt;
            if (beam.damageCooldown > 0.f) beam.damageCooldown -= dt;

            float   halfLen = beam.length * 0.5f;
            Vector2 beamDir{ cosf(beam.angle), sinf(beam.angle) };
            Vector2 beamA{ beam.center.x - beamDir.x * halfLen, beam.center.y - beamDir.y * halfLen };
            Vector2 beamB{ beam.center.x + beamDir.x * halfLen, beam.center.y + beamDir.y * halfLen };

            Vector2 ab = Vector2Subtract(beamB, beamA);
            Vector2 ap = Vector2Subtract(playerWorldPos, beamA);
            float   t  = Vector2DotProduct(ap, ab) / Vector2DotProduct(ab, ab);
            if (t >= 0.f && t <= 1.f)
            {
                float cross = ap.x * ab.y - ap.y * ab.x;
                float perp  = fabsf(cross) / Vector2Length(ab);
                if (perp < kLostCityBeamWidth * 0.5f && beam.damageCooldown <= 0.f)
                {
                    _player.TakeDamage(1, beam.center);
                    beam.damageCooldown = kLostCityBeamDmgCooldown;
                }
            }
        }
        break;
    }

    case Biome::TheSanctuary:
    {
        for (const auto& zone : _sanctuaryZones)
        {
            if (Vector2Distance(playerWorldPos, zone.pos) <= zone.radius)
            {
                _player.SetBiomeDashLocked(true);
                _player.SetBiomeSlowFactor(kSanctuarySlowFactor);
                break;
            }
        }
        break;
    }

    case Biome::DemonsInsides:
        _demonPulseTimer += dt;
        break;

    default:
        break;
    }
}

void Engine::DrawBiomeModifiers()
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    // World pos ? screen pos in dungeon run (room fills the screen, camera at center).
    auto worldToScreen = [&](Vector2 worldPos) -> Vector2
    {
        return { worldPos.x + _shakeOffset.x, worldPos.y + _shakeOffset.y };
    };

    switch (_currentBiome)
    {
    case Biome::Wastelands:
    {
        for (const auto& hazard : _wastelandHazards)
        {
            Vector2 sp  = worldToScreen(hazard.pos);
            int     cx  = (int)sp.x;
            int     cy  = (int)sp.y;

            if (!hazard.exploded)
            {
                float progress = 1.f - (hazard.warningTimer / kWastelandWarningDuration);
                float radius   = kWastelandExplosionRadius * (0.4f + 0.6f * progress);
                float pulse    = sinf((float)GetTime() * 12.f) * 0.3f + 0.7f;
                DrawCircleLines(cx, cy, radius, Fade(RED, pulse));
                DrawCircleLines(cx, cy, radius * 0.55f, Fade(Color{ 255, 120, 0, 255 }, pulse * 0.5f));
            }
            else if (_roomClearExplosionTex.id != 0)
            {
                // Play Flame_Explosion.png - only the first 8 frames contain fire content.
                // Sprite bottom is anchored at the circle center so flames fill the circle.
                static constexpr int   kExpFrameW = 64;
                static constexpr int   kExpFrameH = 64;
                static constexpr int   kExpFrames  = 16;
                float progress = 1.f - (hazard.activeTimer / kWastelandActiveDuration);
                int   frame    = std::min((int)(progress * kExpFrames), kExpFrames - 1);
                Rectangle src  = GetAnimationFrameRect(_roomClearExplosionTex, kExpFrameW, kExpFrameH, frame);
                float diam = kWastelandExplosionRadius * 2.f;
                // Bottom edge at sp.y so flame base sits at the circle center; flames rise upward.
                Rectangle dst{ sp.x - diam * 0.5f, sp.y - diam, diam, diam };
                DrawTexturePro(_roomClearExplosionTex, src, dst, Vector2{}, 0.f, WHITE);
            }
        }
        break;
    }

    case Biome::LostCity:
    {
        for (const auto& beam : _lostCityBeams)
        {
            float   halfLen = beam.length * 0.5f;
            Vector2 beamDir{ cosf(beam.angle), sinf(beam.angle) };
            Vector2 worldA{ beam.center.x - beamDir.x * halfLen, beam.center.y - beamDir.y * halfLen };
            Vector2 worldB{ beam.center.x + beamDir.x * halfLen, beam.center.y + beamDir.y * halfLen };

            Vector2 screenA = worldToScreen(worldA);
            Vector2 screenB = worldToScreen(worldB);
            Vector2 screenC = worldToScreen(beam.center);

            DrawLineEx(screenA, screenB, kLostCityBeamWidth + 6.f,
                Fade(Color{ 255, 220, 60, 255 }, 0.30f));
            DrawLineEx(screenA, screenB, kLostCityBeamWidth,
                Color{ 255, 230, 80, 230 });
            DrawCircle((int)screenC.x, (int)screenC.y, 7, Color{ 255, 200, 50, 200 });
        }
        break;
    }

    case Biome::TheSanctuary:
    {
        // Gentle breathing fill + bright border - the whole disc is visible, not just an edge ring.
        float fill  = sinf((float)GetTime() * 2.0f) * 0.08f + 0.22f;
        float border = 0.75f;
        for (const auto& zone : _sanctuaryZones)
        {
            Vector2 sp = worldToScreen(zone.pos);
            int cx = (int)sp.x;
            int cy = (int)sp.y;
            DrawCircle(cx, cy, (int)zone.radius, Fade(Color{ 50, 70, 220, 255 }, fill));
            DrawCircleLines(cx, cy, zone.radius, Fade(Color{ 110, 150, 255, 255 }, border));
        }
        break;
    }

    case Biome::DemonsInsides:
    {
        float pulse = sinf(_demonPulseTimer * 3.5f) * 0.5f + 0.5f;
        DrawRectangle(0, 0, (int)sw, (int)sh, Fade(RED, pulse * 0.10f));
        break;
    }

    case Biome::DreamRealm:
    {
        float t = (float)GetTime();

        // Pulsing dark purple overlay over the whole room.
        DrawRectangle(0, 0, (int)sw, (int)sh,
            Fade(Color{ 30, 0, 60, 255 }, 0.15f + 0.07f * sinf(t * 0.7f)));

        // Floating dream wisps - stateless, positions driven by time + per-wisp phase.
        constexpr int kWisps = 12;
        for (int w = 0; w < kWisps; w++)
        {
            float phase = w * 0.524f;   // 2p / 12
            float px = sw * (0.06f + 0.88f * ((w + 0.5f) / kWisps))
                       + cosf(t * 0.38f + phase) * sw * 0.06f;
            float py = sh * 0.5f + sinf(t * 0.28f + phase * 1.4f) * sh * 0.33f;
            px = std::clamp(px, 30.f, sw - 30.f);
            py = std::clamp(py, 30.f, sh - 30.f);
            float alpha = 0.28f + 0.18f * sinf(t * 2.2f + phase);
            DrawCircle((int)px, (int)py, 9, Fade(Color{ 210, 140, 255, 255 }, alpha));
            DrawCircle((int)px, (int)py, 4, Fade(WHITE, alpha * 0.55f));
        }
        break;
    }

    default:
        break;
    }
}

void Engine::UpdateDreamFlicker(float dt)
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;
    const float margin = 100.f;
    Vector2 playerPos = _player.GetWorldPos();

    for (auto& enemy : _enemies)
    {
        if (!enemy->IsActive() || enemy->IsDying() || enemy->IsBoss()) continue;

        enemy->TickFlicker(dt);

        // Complete a windup that just finished - teleport and restart cooldown.
        if (enemy->ConsumeFlickerComplete())
        {
            enemy->Teleport(enemy->GetFlickerTarget());
            enemy->SetFlickerCooldown((float)GetRandomValue(300, 600) / 100.f);
            continue;
        }

        if (enemy->IsFlickerInWindup()) continue;
        if (enemy->GetFlickerCooldown() > 0.f) continue;
        if (enemy->IsFrozen() || enemy->IsElectroStunned()) continue;

        float distToPlayer = Vector2Distance(enemy->GetWorldPos(), playerPos);

        // Per-type trigger condition.
        bool shouldFlicker = false;
        if (Ogre* ogre = enemy->AsOgre())
        {
            // Ogre only repositions when far away and not mid-rush.
            shouldFlicker = !ogre->IsRushing() && distToPlayer > 420.f;
        }
        else if (Cyclops* cyclops = enemy->AsCyclops())
        {
            // Cyclops blinks away when the player gets too close.
            (void)cyclops;
            shouldFlicker = distToPlayer < 280.f;
        }
        else
        {
            // Basic grunt: flicker sideways to surround the player.
            shouldFlicker = distToPlayer > 200.f;
        }

        if (!shouldFlicker)
        {
            // Not far enough / close enough yet - check again soon.
            enemy->SetFlickerCooldown(0.5f);
            continue;
        }

        // Find a valid destination: random angle, 100-200 px away from current pos.
        Vector2 enemyPos  = enemy->GetWorldPos();
        Vector2 blinkDest{};
        bool    foundDest = false;

        for (int attempt = 0; attempt < 14 && !foundDest; attempt++)
        {
            float angle = (float)GetRandomValue(0, 628) / 100.f;
            float dist  = (float)GetRandomValue(100, 200);
            Vector2 candidate{
                enemyPos.x + cosf(angle) * dist,
                enemyPos.y + sinf(angle) * dist
            };

            candidate.x = std::clamp(candidate.x, margin, sw - margin);
            candidate.y = std::clamp(candidate.y, margin, sh - margin);

            if (!IsSpawnPositionValid(candidate)) continue;

            // Must land far enough from the player.
            if (Vector2Distance(candidate, playerPos) < 130.f) continue;

            // Cyclops must end up farther from the player than it started.
            if (enemy->AsCyclops() &&
                Vector2Distance(candidate, playerPos) < distToPlayer * 0.85f)
                continue;

            blinkDest = candidate;
            foundDest = true;
        }

        if (!foundDest)
        {
            // No valid spot this attempt - try again in a moment.
            enemy->SetFlickerCooldown(0.8f);
            continue;
        }

        enemy->StartFlickerWindup(0.5f, blinkDest);
        // Freeze the enemy for the windup so it pauses before vanishing.
        enemy->ApplyFreeze(0.5f);
    }
}

void Engine::UpdateDungeonRun(float dt)
{
    if (_prologueActive)
    {
        // F8 is intentionally unused elsewhere. F12 unlocks developer tools;
        // together they provide a visible, test-only onboarding skip.
        if (MO_DEV_TOOLS && _demoCompleted && IsKeyPressed(KEY_F8))
        {
            ClearDungeonEnemies();
            _prologue.Complete();
            _prologueActive = false;
            _firstVillageVisit = false;
            _meta.SetOnboardingComplete();
            EnterVillage();
            return;
        }

        const KeyBindings& bindings = _player.GetBindings();
        const bool basicPressed = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) ||
            (bindings.attack != KEY_NULL && IsKeyPressed(bindings.attack)) ||
            (IsGamepadAvailable(0) && IsGamepadButtonPressed(0, _gamepadBindingsEdit.attack));
        const bool abilityPressed =
            (bindings.ability[0] != KEY_NULL && IsKeyPressed(bindings.ability[0])) ||
            (IsGamepadAvailable(0) && IsGamepadButtonPressed(0, _gamepadBindingsEdit.ability[0]));
        const bool dashPressed = IsKeyPressed(bindings.dash) ||
            (IsGamepadAvailable(0) && IsGamepadButtonPressed(0, _gamepadBindingsEdit.dash));
        ProloguePhase phaseBeforeInput = _prologue.GetPhase();
        _prologue.Update({ basicPressed, abilityPressed, dashPressed, false, false });
        if (phaseBeforeInput == ProloguePhase::Dash &&
            _prologue.GetPhase() == ProloguePhase::LastStand && _dungeonRoomIdx >= 0)
        {
            DungeonRoomState& state = _dungeonRoomStates[_dungeonRoomIdx];
            state.cleared = true;
            state.enemiesInitialized = true;
            state.survivors.clear();
            ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
            SpawnDungeonDoorOpenEffects();
            RebuildDungeonNav();
        }

        float healthNow = _player.GetHealthValue();
        if (_prologue.GetPhase() == ProloguePhase::LastStand &&
            healthNow < _prologueLastHealth - 0.01f)
        {
            _prologue.Update({ false, false, false, false, true });
            if (_prologue.IsScriptedDeathReady())
            {
                _firstDeathRevive = true;
                _deathReviveDialogueLine = 0;
                _deathReviveLostGold = 0;
                _deathReviveLostCells = 0;
                ClearDungeonEnemies();
                BeginDeathRevive();
                return;
            }
            _player.Revive();
            healthNow = _player.GetHealthValue();
        }
        else if (healthNow <= 0.f && _prologue.ShouldAutoRestoreOnLethalHit())
        {
            _player.Revive();
            healthNow = _player.GetHealthValue();
            _vfx.SpawnFloatingLabel(_player.GetWorldPos(), "TRY AGAIN", RAYWHITE, 1.2f);
        }
        _prologueLastHealth = healthNow;
    }

    // Hit-stop: freeze the simulation for a few frames so impacts land hard.
    if (_hitStopTimer > 0.f) { _hitStopTimer -= GetFrameTime(); return; }
    // Slow-mo (crit / boss death): scale the gameplay sim (timer ticks in Update).
    if (_slowMoTimer > 0.f) dt *= _slowMoScale;

    // -- Dungeon fade transition (Store enter / boss clear) --------------------
    if (_dungeonFadeState != DungeonFadeState::None)
    {
        _dungeonFadeTimer -= dt;
        float progress = 1.f - std::max(0.f, _dungeonFadeTimer) / kDungeonFadeDuration;

        if (_dungeonFadeState == DungeonFadeState::FadingOut)
        {
            _dungeonFadeAlpha = progress * 255.f;
            if (_dungeonFadeTimer <= 0.f)
            {
                _dungeonFadeAlpha = 255.f;
                if (_dungeonFadePendingAction)
                {
                    _dungeonFadePendingAction();
                    _dungeonFadePendingAction = nullptr;
                }
                _dungeonFadeState = DungeonFadeState::FadingIn;
                _dungeonFadeTimer = kDungeonFadeDuration;
            }
            return;  // freeze the old room while fading to black
        }
        else  // FadingIn
        {
            _dungeonFadeAlpha = (1.f - progress) * 255.f;
            if (_dungeonFadeTimer <= 0.f)
            {
                _dungeonFadeAlpha = 0.f;
                _dungeonFadeState = DungeonFadeState::None;
            }
            // gameplay runs normally during fade-in - don't return
        }
    }

    // -- Play view - player walks around the room ------------------------------
    if (_dungeonView == DungeonView::Play)
    {
        if (!_dungeonScrolling && IsKeyPressed(KEY_ESCAPE))
        {
            _stateBeforePause = GameState::DungeonRun;
            _gameState = GameState::Pause;
            return;
        }

        // Tick screen shake every frame so TriggerScreenShake has visible effect.
        if (_shakeTimer > 0.f)
        {
            _shakeTimer -= dt;
            float x = GetRandomValue(-100, 100) / 50.f * _shakeStrength;
            float y = GetRandomValue(-100, 100) / 50.f * _shakeStrength;
            _shakeOffset = { x, y };
        }
        else
        {
            _shakeOffset = Vector2Zero();
        }

        float sw = (float)kVirtualWidth;
        float sh = (float)kVirtualHeight;

        // -- Scroll animation -------------------------------------------------
        if (_dungeonScrolling)
        {
            _dungeonScrollT += dt / kDungeonScrollDur;
            if (_dungeonScrollT >= 1.f)
            {
                _dungeonScrollT = 1.f;
                EnterDungeonRoom(_dungeonScrollNextIdx, _dungeonScrollNextEntryDoorSide, _dungeonScrollSpawnPos, false);
            }
            _cameraPos = { sw * 0.5f, sh * 0.5f };
            return;
        }

        // -- Cutscene ---------------------------------------------------------
        if (_cutscene.IsActive())
        {
            // F11 - toggle dialogue box designer even while a cutscene is running
            if (_debug.IsActive() && IsKeyPressed(KEY_F11))
            {
                _isDlgEditorActive = !_isDlgEditorActive;
                _dlgEditorHandle   = -1;
                _dlgSpeakerFsDrag  = false;
                _dlgBodyFsDrag     = false;
                _dlgInsetLeftDrag  = false;
                _dlgInsetTopDrag   = false;
                if (_isDlgEditorActive)
                    _debug.SetOpen(false);
            }

            if (_isDlgEditorActive)
            {
                // Editor active - handle mouse drag but don't consume E/click for dialogue
                UpdateDialogueBoxEditor();
                return;
            }

            // Move player position via cutscene (MoveActor index 0)
            Vector2 playerPos = _player.GetWorldPos();
            _cutscene.Update(dt, &playerPos, nullptr);
            _player.SetWorldPos(playerPos);

            // Signal: open ability selection screen
            if (_cutscene.WantsAbilitySelect()
                && !_awaitingStartingAbility
                && !_starterAbilityGiftClaimed)
            {
                GenerateStartingAbilityOptions();
                _awaitingStartingAbility  = true;
                _levelUpReturnState       = GameState::DungeonRun;
                _levelUpOpenTimer         = 0.3f;
                _gameState                = GameState::LevelUpChoice;
            }

            // Signal: player just chose an ability - resume cutscene
            if (_cutscene.WantsAbilitySelect() && _starterAbilityGiftClaimed)
                _cutscene.OnAbilitySelected();

            // Signal: unlock north door
            if (_cutscene.WantsDoorUnlock())
            {
                _cutscene.ConsumeDoorUnlock();
                SetStoreDoorTiles(TileType::DoorOpen);
            }

            // Poll gamepad here so controller buttons work even though the
            // cutscene block returns early before the normal gamepad input block.
            _gamepad.Update(_gamepadBindingsEdit);
            bool gamepadAdvance = _gamepad.isActive &&
                (_gamepad.attackPressed || _gamepad.dashPressed);

            // Player advances dialogue with E, left-click, or a gamepad face button
            if (IsKeyPressed(KEY_E) || IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || gamepadAdvance)
                _cutscene.AdvanceOnInput();

            return;   // skip all normal dungeon input while cutscene is active
        }

        if (_debug.IsActive())
        {
            if (HandleDebugToggleTabInput())
                return;

            if (_debug.IsGodMode())
                _player.GrantInvulnerability(0.2f);

            DebugCommand cmd = _debug.Update();
            if (cmd.issued)
            {
                Vector2 spawnBase = _player.GetWorldPos();
                switch (cmd.action)
                {
                case DebugActionKind::GrantInvuln:
                    _player.GrantInvulnerability(5.f); break;
                case DebugActionKind::ClearEnemiesContinue:
                    for (auto& e : _enemies) { e->SetActive(false); e->Teleport(Vector2{ -5000.f, -5000.f }); }
                    _dungeonRoomStates[_dungeonRoomIdx].cleared = true;
                    _dungeonEnemiesSpawned = false;
                    ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
                    if (_dungeonRoomIdx == _dungeonGen.GetBossIndex())
                        ApplyDungeonBossExitTiles(TileType::DoorOpen);
                    SpawnDungeonDoorOpenEffects();
                    RebuildDungeonNav();
                    if (_currentRoomType == RoomType::Treasure)
                    {
                        _treasureChestSpawned = true;
                        _treasureChestBroken  = false;
                        SetTreasureChestTile(TileType::ChestClosed);
                    }
                    break;
                case DebugActionKind::RestartRoom:
                    DebugRestartDungeonRoomAs((RoomType)cmd.value); break;
                case DebugActionKind::SetEliteMechanic:
                    _debug.SetForcedEliteMechanic(cmd.value);
                    DebugRestartDungeonRoomAs(RoomType::Elite); break;
                case DebugActionKind::SetEliteType:
                    _debug.SetForcedEliteType(cmd.value);
                    DebugRestartDungeonRoomAs(RoomType::Elite); break;
                case DebugActionKind::ForceEliteSignature:
                    if (Enemy* encounter = FindEncounterDebugTarget())
                        encounter->DebugForceEliteSignature();
                    break;
                case DebugActionKind::ForceElitePhaseTwo:
                    if (Enemy* encounter = FindEncounterDebugTarget())
                        encounter->DebugForceElitePhaseTwo();
                    break;
                case DebugActionKind::SpawnGrunt:
                    SpawnBasicEnemy(Vector2Add(spawnBase, Vector2{ 220.f, 40.f }));
                    _dungeonEnemiesSpawned = true; break;
                case DebugActionKind::SpawnCyclops:
                    SpawnCyclops(Vector2Add(spawnBase, Vector2{ 260.f, -60.f }));
                    _dungeonEnemiesSpawned = true; break;
                case DebugActionKind::SpawnOgre:
                    SpawnOgre(Vector2Add(spawnBase, Vector2{ -240.f, 50.f }));
                    _dungeonEnemiesSpawned = true; break;
                case DebugActionKind::SpawnBoss:
                    SpawnMolarbeast(Vector2Add(spawnBase, Vector2{ 0.f, -260.f }));
                    _dungeonEnemiesSpawned = true; break;
                case DebugActionKind::SpawnNewEnemy:
                    DebugSpawnNewEnemy(cmd.value, Vector2Add(spawnBase, Vector2{ 260.f, -40.f }));
                    _dungeonEnemiesSpawned = true; break;
                case DebugActionKind::SpawnNewBoss:
                    DebugSpawnNewBoss(cmd.value, Vector2Add(spawnBase, Vector2{ 0.f, -300.f }));
                    _dungeonEnemiesSpawned = true; break;
                case DebugActionKind::GrantRandomRelic:
                    GrantRelic(RollRandomRelic()); break;
                case DebugActionKind::GrantAllRelics:
                    for (int i = 0; i < (int)RelicType::Count; i++) _player.AddRelic((RelicType)i); break;
                case DebugActionKind::UnlockAscension:
                    _meta.RecordAscensionCleared(_meta.GetMaxAscensionUnlocked());
                    _meta.SetSelectedAscension(_meta.GetMaxAscensionUnlocked()); break;
                case DebugActionKind::Heal:
                    _player.Heal(cmd.value); break;
                case DebugActionKind::RestoreMana:
                    _player.RestoreMana(cmd.value); break;
                case DebugActionKind::AddGold:
                    _player.AddGold(cmd.value); break;
                case DebugActionKind::SpawnLoot:
                    DebugSpawnLoot(); break;
                case DebugActionKind::ForceLevelUp:
                    _player.AddExp(60); break;
                case DebugActionKind::AddExp:
                    _player.AddExp(cmd.value); break;
                case DebugActionKind::TreasureCards:
                    GenerateLevelUpOptions(LevelUpOfferContext::TreasureBasic);
                    _levelUpReturnState = GameState::DungeonRun; _levelUpOpenTimer = 0.1f;
                    _gameState = GameState::LevelUpChoice; break;
                case DebugActionKind::EliteReward:
                    GenerateLevelUpOptions(LevelUpOfferContext::EliteReward);
                    _levelUpReturnState = GameState::DungeonRun; _levelUpOpenTimer = 0.1f;
                    _gameState = GameState::LevelUpChoice; break;
                case DebugActionKind::AbilityReward:
                    GenerateAbilityChoiceOptions(); _abilityChoiceOpenTimer = 0.1f;
                    _levelUpReturnState = GameState::DungeonRun; _pendingRoomChoice = false;
                    _gameState = GameState::AbilityChoice; break;
                case DebugActionKind::ApplyUpgrade:
                    _player.ApplyUpgrade((UpgradeType)cmd.value); break;
                default: break;
                }
            }
        }

        if (_debug.IsActive() && IsKeyPressed(KEY_ZERO))
        {
            _isHitboxEditorActive = !_isHitboxEditorActive;
            _hitboxSelectedEntity = nullptr;
            _hitboxEditAttack     = false;
            _hitboxNudgeAccum     = -kHitboxNudgeInitDelay;
            if (_isHitboxEditorActive)
                _debug.SetOpen(false);
        }
        if (_isHitboxEditorActive && !_debug.IsActive())
        {
            _isHitboxEditorActive = false;
            _hitboxSelectedEntity = nullptr;
        }
        if (_isHitboxEditorActive)
        {
            UpdateHitboxEditor();
            return;
        }

        // F11 - toggle dialogue box designer (debug only)
        if (_debug.IsActive() && IsKeyPressed(KEY_F11))
        {
            _isDlgEditorActive = !_isDlgEditorActive;
            _dlgEditorHandle   = -1;
            _dlgSpeakerFsDrag  = false;
            _dlgBodyFsDrag     = false;
            _dlgInsetLeftDrag  = false;
            _dlgInsetTopDrag   = false;
            if (_isDlgEditorActive)
            {
                _cutscene.GetLayout().ApplyScreenDefaults();
                _debug.SetOpen(false);
            }
        }
        if (_isDlgEditorActive && !_debug.IsActive())
            _isDlgEditorActive = false;

        if (_isDlgEditorActive)
        {
            UpdateDialogueBoxEditor();
            return;
        }
        // KEY_NINE - jump to world map with editor open (debug shortcut)
        if (_debug.IsActive() && IsKeyPressed(KEY_NINE))
        {
            OpenWorldMap();
            if (!_worldMap.IsEditorActive())
                _worldMap.ToggleEditor();
        }

        // -- Normal player update ----------------------------------------------
        if (_player.GetHealthValue() <= 0.f && !_playerDying)
        {
            _playerDying   = true;
            _gameOverTimer = _gameOverDelay;
        }
        if (_playerDying)
        {
            _gameOverTimer -= dt;
            if (_gameOverTimer <= 0.f)
            {
                _playerDying = false;
                // Capture what the fallen mystic carried BEFORE the wipe so the
                // loss animation shows the right gold + echoes streaming away,
                // then begin Poe's revive cutscene (replaces the GameOver menu).
                _deathReviveLostGold  = _player.GetGold();
                _deathReviveLostCells = _player.GetCells();
                HandlePlayerDeathMetaPenalty();   // one-wallet rule: wipes all gold + cells
                BeginDeathRevive();
            }
            _cameraPos = { sw * 0.5f, sh * 0.5f };
            return;
        }

        const bool ultActive = (_ultimatePhase != UltimatePhase::None);
        _player.SetCombatLocked(ultActive);
        _player.SetManaRegenPaused(ultActive);

        // During the ultimate cinematic freeze combat and advance the sequence.
        if (ultActive)
        {
            _player.ConsumeCastRequest();
            UpdateUltimateSequence(dt);
            return;
        }

        // Touch controls - must be set on player before Update() consumes them.
        _player.SetTouchModeEnabled(_touchModeActive);
        if (_touchModeActive)
            UpdateTouchControls();

        // Gamepad input for dungeon run
        _gamepad.Update(_gamepadBindingsEdit);
        if (_gamepad.isActive)
        {
            // Always push the direction (including zero) so releasing the stick
            // clears _touchMoveDir and the player stops instead of drifting.
            _player.SetTouchDirection(_gamepad.moveDir);
            if (_gamepad.attackPressed)  _player.SetTouchAttack();
            if (_gamepad.dashPressed)    _player.SetTouchDash();
            if (!_dungeonScrolling && _gamepad.pausePressed)
            {
                _stateBeforePause = GameState::DungeonRun;
                _gameState = GameState::Pause;
                return;
            }
        }

        UpdateAbilityAiming();
        const Vector2 playerPosBeforeUpdate = _player.GetWorldPos();
        _player.Update(dt);
        HandlePlayerCastRequest();
        UpdateDungeonMagicGemAndBarrier(dt);

        float cellW = sw / (float)RoomLayout::kCols;
        float cellH = sh / (float)RoomLayout::kRows;

        Vector2 pos = _player.GetWorldPos();
        const auto& rooms = _dungeonGen.GetRooms();
        const DungeonRoom& cur = rooms[_dungeonRoomIdx];

        // Door ranges in world (= screen) coords.
        int   doorStartC = GetDungeonDoorStartCol();
        float doorLeft   = doorStartC * cellW;
        float doorRight  = (doorStartC + kDungeonDoorSpanCols) * cellW;
        int   doorStartR = GetDungeonDoorStartRow();
        float doorTop    = doorStartR * cellH;
        float doorBot    = (doorStartR + kDungeonDoorSpanRows) * cellH;

        // Which walls have open doorways the player can walk through?
        // The player's BODY (collision rect) must fit inside the opening, and
        // travel only starts while the player is actively pushing toward that door.
        Rectangle playerBody = _player.GetCollisionRec();
        bool bodyInDoorX = playerBody.x > doorLeft && playerBody.x + playerBody.width  < doorRight;
        bool bodyInDoorY = playerBody.y > doorTop  && playerBody.y + playerBody.height < doorBot;
        Vector2 moveDir = _player.GetMoveDirection();
        bool canUseDoor = !_player.IsForceLocked() && Vector2LengthSqr(moveDir) > 0.01f;
        bool wantsNorth = canUseDoor && moveDir.y < -0.25f;
        bool wantsSouth = canUseDoor && moveDir.y >  0.25f;
        bool wantsWest  = canUseDoor && moveDir.x < -0.25f;
        bool wantsEast  = canUseDoor && moveDir.x >  0.25f;
        const bool hasNorthNeighbor = _dungeonGen.GetNeighborIndex(_dungeonRoomIdx, -1,  0) >= 0;
        const bool hasSouthNeighbor = _dungeonGen.GetNeighborIndex(_dungeonRoomIdx,  1,  0) >= 0;
        const bool hasWestNeighbor  = _dungeonGen.GetNeighborIndex(_dungeonRoomIdx,  0, -1) >= 0;
        const bool hasEastNeighbor  = _dungeonGen.GetNeighborIndex(_dungeonRoomIdx,  0,  1) >= 0;
        bool canPassNorth = cur.hasNorth && hasNorthNeighbor && IsDungeonDoorOpen(DungeonDoorSide::North) && bodyInDoorX && wantsNorth;
        bool canPassSouth = cur.hasSouth && hasSouthNeighbor && IsDungeonDoorOpen(DungeonDoorSide::South) && bodyInDoorX && wantsSouth;
        bool canPassWest  = cur.hasWest  && hasWestNeighbor  && IsDungeonDoorOpen(DungeonDoorSide::West)  && bodyInDoorY && wantsWest;
        bool canPassEast  = cur.hasEast  && hasEastNeighbor  && IsDungeonDoorOpen(DungeonDoorSide::East)  && bodyInDoorY && wantsEast;
        // Helper: begin a Zelda-style scroll into an adjacent room.
        // scrollVec describes which direction the CURRENT room slides offscreen.
        auto startScroll = [&](int dr, int dc, Vector2 scrollVec, Vector2 spawnPos)
        {
            int nextIdx = _dungeonGen.GetNeighborIndex(_dungeonRoomIdx, dr, dc);
            if (nextIdx < 0) return;

            if (nextIdx == _dungeonGen.GetBossIndex() && !_bossBarrierUnlocked)
            {
                if (_hasMagicGem)
                {
                    _hasMagicGem = false;
                    _bossBarrierUnlocked = true;
                    _message = "The magic gem shattered the barrier";
                }
                else
                {
                    _bossBarrierMessageTimer = 2.2f;
                    _message = "you need a a magic gem to get past the barrier";
                    Vector2 blocked = _player.GetWorldPos();
                    if (dr < 0) blocked.y = cellH + 4.f;
                    if (dr > 0) blocked.y = (RoomLayout::kRows - 2) * cellH - 4.f;
                    if (dc < 0) blocked.x = cellW + 4.f;
                    if (dc > 0) blocked.x = (RoomLayout::kCols - 1) * cellW - 4.f;
                    _player.SetWorldPos(blocked);
                    return;
                }
            }

            SaveDungeonRoomEnemyState();

            const DungeonRoom& next = rooms[nextIdx];
            int nextVisualVariant = GetDungeonVisualVariantForRoom(nextIdx);
            const TileDefSet* nextDefs = &_tileDefs;
            if (nextVisualVariant != _currentDungeonVisualVariant)
            {
                LoadDungeonVisualVariantAssets(nextVisualVariant, _dungeonScrollTileDefs,
                                               _dungeonScrollTileRenderer);
                nextDefs = &_dungeonScrollTileDefs;
            }
            else
            {
                _dungeonScrollTileRenderer.Unload();
            }
            _dungeonScrollNextLayout = BuildDungeonRoomLayout(
                nextIdx, *nextDefs, nextVisualVariant);
            _dungeonScrollNextEntryDoorSide = OppositeDungeonDoorSide(dr, dc);
            ApplyDungeonRoomDoorState(_dungeonScrollNextLayout, nextIdx, _dungeonScrollNextEntryDoorSide);
            _dungeonScrollNextIdx     = nextIdx;
            _dungeonScrollSpawnPos    = spawnPos;
            _dungeonScrollVec         = scrollVec;
            _dungeonScrollT           = 0.f;
            _dungeonScrolling         = true;
        };

        // Check door thresholds and trigger scroll.
        if (canPassNorth && pos.y < cellH)
        {
            if (_currentRoomType == RoomType::Store)
            {
                // Fade to black before entering the first dungeon room - feels more intentional
                // than a Zelda-style room scroll for this "entering the dungeon" moment.
                int nextIdx = _dungeonGen.GetNeighborIndex(_dungeonRoomIdx, -1, 0);
                if (nextIdx >= 0)
                {
                    Vector2 spawnPos{ sw * 0.5f, (RoomLayout::kRows - 2) * cellH };
                    SaveDungeonRoomEnemyState();
                    _dungeonFadePendingAction = [this, nextIdx, spawnPos]()
                    {
                        EnterDungeonRoom(nextIdx, DungeonDoorSide::South, spawnPos, false);
                    };
                    _dungeonFadeState = DungeonFadeState::FadingOut;
                    _dungeonFadeTimer = kDungeonFadeDuration;
                }
            }
            else
                startScroll(-1, 0, { 0.f, 1.f }, { pos.x, (RoomLayout::kRows - 2) * cellH });
        }
        else if (canPassSouth && pos.y > (RoomLayout::kRows - 1) * cellH)
        {
            // Never allow walking back south into the Store room once the player has left.
            int southNeighborIdx = _dungeonGen.GetNeighborIndex(_dungeonRoomIdx, +1, 0);
            if (southNeighborIdx != _dungeonGen.GetStartIndex())
                startScroll(+1, 0, { 0.f, -1.f }, { pos.x, cellH * 2.f });
        }
        else if (canPassWest  && pos.x < cellW)
            startScroll( 0, -1, { 1.f,  0.f }, { (RoomLayout::kCols - 2) * cellW, pos.y });
        else if (canPassEast  && pos.x > (RoomLayout::kCols - 1) * cellW)
            startScroll( 0, +1, {-1.f,  0.f }, { cellW * 2.f, pos.y });

        // Wall collision: solid walls clamp, door openings let the player through.
        // Clamps push the player's COLLISION RECT flush against each wall's
        // visible inner edge (old version clamped the centre point, letting the
        // sprite sink into the side walls and stop short of the top wall).
        if (!_dungeonScrolling)
        {
            Vector2 posBefore = pos;
            float wallNorthEdge = cellH;                                   // bottom of the top wall row
            float wallSouthEdge = (RoomLayout::kRows - 1) * cellH;         // top of the bottom wall row
            float wallWestEdge  = cellW;                                   // right edge of the left wall
            float wallEastEdge  = (RoomLayout::kCols - 1) * cellW;         // left edge of the right wall
            if (!canPassNorth && playerBody.y < wallNorthEdge)
                pos.y += wallNorthEdge - playerBody.y;
            if (!canPassSouth && playerBody.y + playerBody.height > wallSouthEdge)
                pos.y -= (playerBody.y + playerBody.height) - wallSouthEdge;
            if (!canPassWest  && playerBody.x < wallWestEdge)
                pos.x += wallWestEdge - playerBody.x;
            if (!canPassEast  && playerBody.x + playerBody.width > wallEastEdge)
                pos.x -= (playerBody.x + playerBody.width) - wallEastEdge;
            _player.SetWorldPos(pos);
            // End any forced push the moment the player touches a wall.
            if ((pos.x != posBefore.x || pos.y != posBefore.y) && _player.IsBeingForcedPushed())
                _player.OnForcedPushCollision();

            // Authored rooms may paint interior walls and void gaps anywhere on
            // the 28x16 grid. Resolve movement one axis at a time so the player
            // slides along those shapes instead of sticking at corners.
            if (_dungeonRoomLayout.handcrafted)
            {
                const Vector2 desiredPos = _player.GetWorldPos();
                const Vector2 resolvedPos = ResolveHandcraftedTileMovement(
                    _dungeonRoomLayout, playerPosBeforeUpdate, desiredPos,
                    _player.GetCollisionRec(), cellW, cellH);
                if (resolvedPos.x != desiredPos.x || resolvedPos.y != desiredPos.y)
                {
                    _player.SetWorldPos(resolvedPos);
                    if (_player.IsBeingForcedPushed()) _player.OnForcedPushCollision();
                }
            }

            // Prop collision - resolve player out of each prop's stored collision rect.
            float pxScaleX = cellW / 16.f;
            float pxScaleY = cellH / 16.f;
            auto resolvePlayerVsPropRect = [&](Rectangle propRect) {
                Rectangle playerRect = _player.GetCollisionRec();
                if (!CheckCollisionRecs(playerRect, propRect)) return;
                bool wasPushed = _player.IsBeingForcedPushed();
                float overlapL = (playerRect.x + playerRect.width)  - propRect.x;
                float overlapR = (propRect.x   + propRect.width)    - playerRect.x;
                float overlapT = (playerRect.y + playerRect.height) - propRect.y;
                float overlapB = (propRect.y   + propRect.height)   - playerRect.y;
                float resolveX = (overlapL < overlapR) ? -overlapL : overlapR;
                float resolveY = (overlapT < overlapB) ? -overlapT : overlapB;
                Vector2 p = _player.GetWorldPos();
                if (std::abs(resolveX) < std::abs(resolveY)) p.x += resolveX;
                else                                          p.y += resolveY;
                _player.SetWorldPos(p);
                if (wasPushed) _player.OnForcedPushCollision();
            };
            for (const SpritePlacement& prop : _dungeonRoomLayout.props)
            {
                const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,prop,_tileDefs);
                if(defs==nullptr||prop.defIdx<0||prop.defIdx>=(int)defs->props.size()) continue;
                const Rectangle& coll = defs->props[prop.defIdx].collision;
                resolvePlayerVsPropRect({
                    prop.col * cellW + coll.x * pxScaleX,
                    prop.row * cellH + coll.y * pxScaleY,
                    coll.width * pxScaleX, coll.height * pxScaleY });
            }
            for (const SpritePlacement& prop : _dungeonRoomLayout.animProps)
            {
                const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,prop,_tileDefs);
                if(defs==nullptr||prop.defIdx<0||prop.defIdx>=(int)defs->animProps.size()) continue;
                const Rectangle& coll = defs->animProps[prop.defIdx].collision;
                resolvePlayerVsPropRect({
                    prop.col * cellW + coll.x * pxScaleX,
                    prop.row * cellH + coll.y * pxScaleY,
                    coll.width * pxScaleX, coll.height * pxScaleY });
            }

            // ── Zelda-style pit fall ─────────────────────────────────────────
            // A pit pulls you toward its centre, but you get a short window to
            // scramble your feet back onto solid ground before it drags you down.
            // Falling costs 1 HP and drops you back at the edge you fell from.
            constexpr float kPitPullSpeed  = 235.f;   // px/s drag toward the pit
            constexpr float kPitDragWindow = 0.42f;   // seconds before you go down
            _dungeonFallRecoveryCooldown = std::max(0.f, _dungeonFallRecoveryCooldown - dt);
            if (_dungeonRoomLayout.handcrafted)
            {
                if (_player.IsPitFalling())
                {
                    if (_player.PitFallComplete())
                    {
                        _player.EndPitFall();
                        _player.TakePitfallDamage(1);
                        if (_player.GetHealthValue() > 0.f)
                        {
                            _player.SetWorldPos(_dungeonLastSafePos);
                            if (_dungeonRoomLayout.fallSurface == FallSurface::Lava)
                                _player.ApplyBurnTicks(0.55f, 2, 0.25f,
                                                       _dungeonLastSafePos);
                        }
                        _pitDragTimer = 0.f;
                        _dungeonFallRecoveryCooldown = 0.6f;
                        _message = FallSurfaceMessage(_dungeonRoomLayout.fallSurface);
                    }
                }
                else if (_dungeonFallRecoveryCooldown <= 0.f)
                {
                    const Rectangle body = _player.GetCollisionRec();
                    const Vector2 currentPos = _player.GetWorldPos();
                    const Vector2 frameMove = Vector2Subtract(currentPos, playerPosBeforeUpdate);
                    const Rectangle sweptBody{
                        body.x - std::max(0.f, frameMove.x),
                        body.y - std::max(0.f, frameMove.y),
                        body.width + std::abs(frameMove.x),
                        body.height + std::abs(frameMove.y)
                    };

                    // A dash cannot enter or tunnel through a fall zone. Ordinary
                    // walking and forced enemy pushes intentionally skip this
                    // branch, preserving the authored pit gameplay.
                    if (_player.IsDashing() && RoomBodyIntersectsFall(
                            _dungeonRoomLayout, sweptBody, cellW, cellH))
                    {
                        _player.SetWorldPos(playerPosBeforeUpdate);
                        _player.CancelDash();
                        _pitDragTimer = 0.f;
                        _dungeonLastSafePos = playerPosBeforeUpdate;
                    }
                    else
                    {
                        const Rectangle currentBody = _player.GetCollisionRec();
                        const Vector2 feet{ currentBody.x + currentBody.width * 0.5f,
                                            currentBody.y + currentBody.height * 0.88f };
                        if (IsRoomFallPoint(_dungeonRoomLayout, feet, cellW, cellH))
                        {
                            const Vector2 pull = PitPullDirection(feet, cellW, cellH);
                            if (Vector2Length(pull) > 0.01f)
                            {
                                const float pullSpeed =
                                    _dungeonRoomLayout.fallSurface == FallSurface::Water
                                    ? kPitPullSpeed * 2.f
                                    : kPitPullSpeed;
                                _player.SetWorldPos(Vector2Add(_player.GetWorldPos(),
                                    Vector2Scale(pull, pullSpeed * dt)));
                            }
                            _pitDragTimer += dt;
                            if (_pitDragTimer >= kPitDragWindow)
                            {
                                SpawnFallSurfaceFx(feet);
                                _player.BeginPitFall(
                                    FallSurfaceTint(_dungeonRoomLayout.fallSurface));
                            }
                        }
                        else
                        {
                            _pitDragTimer = 0.f;
                            _dungeonLastSafePos = _player.GetWorldPos();
                        }
                    }
                }
                UpdateEnemyPitfalls(cellW, cellH);
            }
        }

        // -- Combat update -----------------------------------------------------
        if (_currentRoomType == RoomType::Store)
        {
            Vector2 shopWorldOffset{ -_cameraPos.x, -_cameraPos.y };
            bool gamepadInteract = _gamepad.isActive && (_gamepad.attackPressed || _gamepad.dashPressed);
            if (_shop.UpdateNpc(_player, shopWorldOffset, _touchModeActive, gamepadInteract))
            {
                _levelUpReturnState = GameState::DungeonRun;
                if (!_starterAbilityGiftClaimed)
                {
                    GenerateStartingAbilityOptions();
                    _awaitingStartingAbility = true;
                    _levelUpOpenTimer = 0.3f;
                    _gameState = GameState::LevelUpChoice;
                }
                else
                {
                    _gameState = GameState::Shop;
                }
                return;
            }

            // -- Poe's altar (Echoes / meta progression unlocks) -------------
            // Poe, a hovering spirit west of Zeph. Standing near him and
            // pressing the interact key opens the permanent unlock screen.
            _poeAltarBobTimer += dt;
            Vector2 altarPos = GetPoeAltarPos();
            _nearPoeAltar = Vector2Distance(_player.GetWorldPos(), altarPos) < 155.f;
            if (_nearPoeAltar && !_shop.IsNearNpc())
            {
                bool interactPressed = IsKeyPressed(KEY_E) || gamepadInteract;
                if (_touchModeActive && IsGestureDetected(GESTURE_TAP))
                {
                    // Touch: tapping anywhere near the altar counts as interact.
                    Vector2 tap = GetVirtualTouchPos(0);
                    Vector2 altarScreen{ altarPos.x - _cameraPos.x + kVirtualWidth * 0.5f,
                                         altarPos.y - _cameraPos.y + kVirtualHeight * 0.5f };
                    if (Vector2Distance(tap, altarScreen) < 170.f)
                        interactPressed = true;
                }
                if (interactPressed)
                {
                    _metaShopCursor      = 0;
                    _metaShopOpenTimer   = 0.25f;
                    _metaShopReturnState = GameState::DungeonRun;
                    _poeGreetingIdx      = GetRandomValue(0, kPoeGreetingCount - 1);
                    _gameState           = GameState::MetaShop;
                    return;
                }
            }

            // -- Cursed Wager — east of Zeph; one wager per biome (re-arms each shop).
            _curseShrineBobTimer += dt;
            Vector2 shrinePos = GetCurseShrinePos();
            _nearCurseShrine = _wagerAccessGranted && !_curseShrineUsed &&
                               Vector2Distance(_player.GetWorldPos(), shrinePos) < 155.f;
            if (_nearCurseShrine && !_shop.IsNearNpc())
            {
                bool interactPressed = IsKeyPressed(KEY_E) || gamepadInteract;
                if (_touchModeActive && IsGestureDetected(GESTURE_TAP))
                {
                    Vector2 tap = GetVirtualTouchPos(0);
                    Vector2 shrineScreen{ shrinePos.x - _cameraPos.x + kVirtualWidth * 0.5f,
                                          shrinePos.y - _cameraPos.y + kVirtualHeight * 0.5f };
                    if (Vector2Distance(tap, shrineScreen) < 170.f)
                        interactPressed = true;
                }
                if (interactPressed)
                {
                    OpenCurseShrine();
                    return;
                }
            }
        }
        else
        {
            _nearPoeAltar = false;

            // -- Decision room (Risk Shrine, …) — walk to the shrine, press [E].
            DungeonRoomState& st = _dungeonRoomStates[_dungeonRoomIdx];
            _nearDecisionShrine = false;
            if (st.special != RoomSpecialType::None && !st.specialClaimed)
            {
                _decisionShrineBobTimer += dt;
                Vector2 sp = GetDecisionShrinePos();
                _nearDecisionShrine = Vector2Distance(_player.GetWorldPos(), sp) < 155.f;
                if (_nearDecisionShrine)
                {
                    bool gp = _gamepad.isActive && (_gamepad.attackPressed || _gamepad.dashPressed);
                    bool interactPressed = IsKeyPressed(KEY_E) || gp;
                    if (_touchModeActive && IsGestureDetected(GESTURE_TAP))
                    {
                        Vector2 tap = GetVirtualTouchPos(0);
                        Vector2 shrineScreen{ sp.x - _cameraPos.x + kVirtualWidth * 0.5f,
                                              sp.y - _cameraPos.y + kVirtualHeight * 0.5f };
                        if (Vector2Distance(tap, shrineScreen) < 170.f)
                            interactPressed = true;
                    }
                    if (interactPressed)
                    {
                        OpenDecisionRoom();
                        return;
                    }
                }
            }
        }
        _nav.TickRefresh(dt, _player.GetFeetWorldPos());
        _nav.ApplyPendingRefresh();

        float dungeonPxScaleX = cellW / 16.f;
        float dungeonPxScaleY = cellH / 16.f;
        _dungeonPropCentersScratch.clear();
        _dungeonPropCentersScratch.reserve(_dungeonRoomLayout.props.size() + _dungeonRoomLayout.animProps.size());
        auto addDungeonPropCenter = [&](const SpritePlacement& prop, const Rectangle& coll)
        {
            Rectangle rec{
                prop.col * cellW + coll.x * dungeonPxScaleX,
                prop.row * cellH + coll.y * dungeonPxScaleY,
                coll.width * dungeonPxScaleX,
                coll.height * dungeonPxScaleY
            };
            _dungeonPropCentersScratch.push_back({ rec.x + rec.width * 0.5f, rec.y + rec.height * 0.5f });
        };
        for (const SpritePlacement& prop : _dungeonRoomLayout.props)
        {
            const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,prop,_tileDefs);
            if(defs==nullptr||prop.defIdx<0||prop.defIdx>=(int)defs->props.size()) continue;
            addDungeonPropCenter(prop, defs->props[prop.defIdx].collision);
        }
        for (const SpritePlacement& prop : _dungeonRoomLayout.animProps)
        {
            const TileDefSet* defs=ResolveRoomDefinitions(_dungeonRoomLayout,prop,_tileDefs);
            if(defs==nullptr||prop.defIdx<0||prop.defIdx>=(int)defs->animProps.size()) continue;
            addDungeonPropCenter(prop, defs->animProps[prop.defIdx].collision);
        }

        EnemyRuntimeContext eCtx{};
        eCtx.player             = &_player;
        eCtx.nav                = &_nav;
        eCtx.props              = &_props;   // used by overworld rooms
        eCtx.propCenters        = &_dungeonPropCentersScratch;
        eCtx.enemies            = &_enemies;
        eCtx.cyclopsLasers      = &_cyclopsLasers;
        eCtx.lavaBalls          = &_lavaBalls;
        eCtx.enemyProjectiles   = &_enemyProjectiles;
        eCtx.triggerScreenShake = [&](float s, float d){ TriggerScreenShake(s, d); };
        eCtx.spawnSmallSlime    = [&](Vector2 pos) { SpawnSlime(pos, SlimeSize::Small); };
        eCtx.spawnBasicEnemy    = [&](Vector2 pos) { return SpawnBasicEnemy(pos); };
        eCtx.spawnBossPoisonPool = [&](Vector2 pos) { SpawnPoisonCloud(pos, 130.f); };
        eCtx.spawnBossFx        = [&](Vector2 pos, int fxId) { SpawnBossFx(pos, fxId); };
        eCtx.spawnBossCallout   = [&](Vector2 pos, const char* text) { ShowBossCallout(pos, text); };
        eCtx.spawnEliteFx       = [&](Vector2 pos, int fxId, float scale, Color tint)
            { SpawnEliteFx(pos, fxId, scale, tint); };
        eCtx.spawnEliteHazardFx = [&](Vector2 pos, int fxId, float scale, float duration, Color tint)
            { SpawnEliteHazardFx(pos, fxId, scale, duration, tint); };
        eCtx.triggerScreenFlash = [&](Color color, float duration)
            { TriggerScreenFlash(color, duration); };
        eCtx.requestHitStop     = [&](float seconds) { RequestHitStop(seconds); };
        RebuildEnemyHazardZones();
        eCtx.hazards            = &_enemyHazardZones;
        _combatDirector.UpdateEnemyRuntime(eCtx, dt);

        // Enemy-fired shots and environmental shots use independent safety
        // limits. Preserve older active shots first and discard only overflow
        // appended by this frame's enemy casts; hazards apply their own cap
        // immediately below.
        const int enemyProjectileCap = Balance::Pressure::kEnemyProjectileCap[
            std::clamp(_roomEncounterTier, 0, 2)];
        if ((int)_enemyProjectiles.size() > enemyProjectileCap)
            _enemyProjectiles.erase(_enemyProjectiles.begin() + enemyProjectileCap,
                                    _enemyProjectiles.end());

        if (_currentBiome == Biome::DreamRealm)
            UpdateDreamFlicker(dt);

        UpdateBiomeModifiers(dt);

        // Room hazards: bolts go through the shared _enemyProjectiles list so
        // collision/draw stay centralized; the in-flight count enforces the
        // environmental projectile cap across hazards AND flame wisps.
        {
            RoomHazardContext hazardCtx;
            hazardCtx.playerPos = _player.GetFeetWorldPos();
            hazardCtx.deltaTime = dt;
            int envBoltsInFlight = 0;
            for (const auto& projectile : _enemyProjectiles)
                if (projectile.IsActive() && projectile.GetKind() == EnemyProjectileKind::FireBolt)
                    envBoltsInFlight++;
            hazardCtx.envProjectilesInFlight = envBoltsInFlight;
            // Hazard damage rides the same growth curve as enemy damage
            // (+kDamagePerLevel per power level), so hazards stay relevant
            // without ever spiking ahead of the run.
            {
                int powerLevel = 1 + (_wave - 1) / Balance::Curve::kRoomsPerPowerLevel;
                float damageGrowth = 1.f + Balance::Curve::kDamagePerLevel * (float)(powerLevel - 1);
                hazardCtx.scaledBoltDamage = std::max(1, (int)roundf((float)Balance::Hazards::kTotemBoltDamage  * damageGrowth));
                hazardCtx.scaledTickDamage = std::max(1, (int)roundf((float)Balance::Hazards::kHazardTickDamage * damageGrowth));
            }
            hazardCtx.spawnFireBolt = [&](Vector2 pos, Vector2 dir, int damage)
            {
                EnemyProjectile bolt;
                bolt.Init(pos, dir, EnemyProjectileKind::FireBolt, damage);
                // Hazard bolts stay SMALL — a wisp bolt at 55% sprite+hitbox
                // size (Balance::Hazards::kHazardBoltScale is the knob).
                bolt.SetScale(Balance::Hazards::kHazardBoltScale);
                _enemyProjectiles.push_back(bolt);
                SfxBank::Get().Play(SfxId::CastFire, 0.3f);   // totem/torch fire whoosh
            };
            hazardCtx.damagePlayer = [&](int damage, Vector2 fromPos)
            {
                _player.TakeDamage(damage, fromPos);
            };
            _roomHazards.Update(hazardCtx);
            // Persist hazard health/destroyed state every frame (cheap: the
            // room holds at most a few hazards) so re-entry stays truthful.
            SaveDungeonRoomHazardState();
        }

        HandlePlayerMeleeDamage();

        // Player-enemy capsule separation - gives enemies physical presence.
        // Dash passes through enemies; on landing inside one, the player is ejected.
        //
        // Task 6 (wall-safe separation): ordinary contact now moves the ENEMY
        // away from the player instead of pushing the player into whatever is
        // behind them. The old code re-read _player.GetWorldPos() fresh on
        // every loop iteration, so each overlapping enemy this frame applied
        // its own full MTV on top of whatever the PREVIOUS enemy had already
        // pushed the player to, with no re-validation against room geometry
        // afterward - that compounding, unvalidated push was the wall-trap
        // bug. The fix: capture one STABLE player position/capsule before the
        // loop (every contact this frame is measured against the same
        // pre-resolution player, per the design's "Wall-Safe Physical
        // Separation"), move each overlapping enemy by the policy's chosen
        // delta, and cap actual player displacement to at most ONE small
        // fallback nudge per frame (see ChooseBodySeparation/CombatSeparation
        // for why summing playerDelta across many contacts can never recreate
        // the old bug even without this cap - the cap is a second, structural
        // guarantee on top of that).
        //
        // ResolveDungeonEnemyCollisions() (wall/prop correction for enemies)
        // is deliberately called AFTER this block now, not before it as in
        // the pre-Task-6 code - reordered per the plan's Step 4: "Capture one
        // stable player position, move ordinary enemies outward, then run
        // dungeon enemy collision." Running it after means any enemy this
        // block moves toward a wall/prop gets corrected the same frame,
        // instead of only correcting that frame's earlier AI movement and
        // leaving a freshly-separated enemy's position unvalidated until next
        // frame. HandlePlayerMeleeDamage() above is unaffected by the reorder
        // - it already ran before ResolveDungeonEnemyCollisions() in the old
        // code too, so melee still sees enemies at their pre-wall-correction
        // positions for this frame, exactly as before.
        if (!_player.IsDashing())
        {
            const Vector2   stablePlayerPos     = _player.GetWorldPos();
            const Capsule2D stablePlayerCapsule = _player.GetCapsule();

            // Deep-overlap threshold: the default enemy capsule radius is
            // 36px (Enemy::GetCapsule) and the player's is 50px
            // (BaseCharacter::GetCapsule), so ordinary brushing contact
            // produces MTV magnitudes of a few pixels up to roughly a third
            // of those radii. 24px (two-thirds of the smaller, enemy-side
            // radius) is chosen as the line between "normal jostling" and "a
            // genuinely deep embed" (e.g. an enemy spawned/shoved well inside
            // the player) that enemy-only correction may not be able to
            // resolve cleanly - only overlaps at least this deep are eligible
            // for the rare player-fallback nudge below.
            constexpr float kDeepOverlapThresholdPx = 24.f;

            Vector2 totalPlayerDelta{ 0.f, 0.f };
            // Structural guarantee against summed player pushes: regardless
            // of how many enemies overlap the player this frame, at most ONE
            // of them may contribute a (small, clamped) player-fallback
            // delta. ChooseBodySeparation already keeps playerDelta at
            // {0,0} in every case except the rare deep-overlap fallback, but
            // capping to one contact per frame here removes any dependency
            // on that alone and makes the "no summed pushes" property true
            // by construction at the integration site too.
            bool playerFallbackApplied = false;

            std::vector<Rectangle> separationBlockers;
            bool blockersComputed = false;
            auto ensureBlockers = [&]() -> const std::vector<Rectangle>&
            {
                if (!blockersComputed)
                {
                    separationBlockers = GetDungeonSpawnBlockers(cellW, cellH);
                    blockersComputed = true;
                }
                return separationBlockers;
            };

            for (auto& enemy : _enemies)
            {
                if (!enemy->IsActive() || !enemy->IsAlive()) continue;
                Vector2 mtv{};
                if (!CheckCapsuleCapsule(stablePlayerCapsule, enemy->GetCapsule(), mtv)) continue;
                if (_player.IsBeingForcedPushed()) continue;

                // Would moving the enemy the full separation distance away
                // from the player land it somewhere clear of walls/props?
                // Reuses the existing dungeon spawn-position validity check
                // (the same one reinforcement placement uses) against the
                // enemy's own real collision footprint - no new wall-check
                // logic, just the existing one applied to this candidate.
                const Vector2 enemyPos = enemy->GetWorldPos();
                const Vector2 candidateEnemyPos{ enemyPos.x - mtv.x, enemyPos.y - mtv.y };
                const bool enemyMoveValid = IsDungeonEnemySpawnPositionValid(
                    enemy.get(), candidateEnemyPos, cellW, cellH, ensureBlockers());
                const bool deepOverlap = Vector2Length(mtv) > kDeepOverlapThresholdPx;

                const SeparationMove move = ChooseBodySeparation(mtv, enemyMoveValid, deepOverlap);

                if (move.enemyDelta.x != 0.f || move.enemyDelta.y != 0.f)
                    enemy->Teleport({ enemyPos.x + move.enemyDelta.x, enemyPos.y + move.enemyDelta.y });

                if (!playerFallbackApplied && (move.playerDelta.x != 0.f || move.playerDelta.y != 0.f))
                {
                    totalPlayerDelta = move.playerDelta;
                    playerFallbackApplied = true;
                }
            }

            if (totalPlayerDelta.x != 0.f || totalPlayerDelta.y != 0.f)
            {
                // Immediately resolve the small fallback nudge against room
                // tiles/props before committing it - reuses IsRoomSpawnAreaValid
                // (the same free function GetDungeonSpawnBlockers/
                // IsDungeonEnemySpawnPositionValid already wrap for enemies)
                // against the player's own real collision rect. If the nudge
                // itself would land the player in geometry, it is simply
                // skipped rather than applied - the nudge exists to unstick a
                // pinch, never to push the player into a wall.
                const Vector2   candidatePlayerPos{ stablePlayerPos.x + totalPlayerDelta.x,
                                                     stablePlayerPos.y + totalPlayerDelta.y };
                const Rectangle currentPlayerBody = _player.GetCollisionRec();
                const Rectangle candidatePlayerBody{
                    currentPlayerBody.x + totalPlayerDelta.x,
                    currentPlayerBody.y + totalPlayerDelta.y,
                    currentPlayerBody.width, currentPlayerBody.height };
                if (IsRoomSpawnAreaValid(_dungeonRoomLayout, candidatePlayerBody, cellW, cellH, ensureBlockers()))
                    _player.SetWorldPos(candidatePlayerPos);
            }
        }

        ResolveDungeonEnemyCollisions();

        // Keep all enemies within the tile room bounds (one cell margin from edges).
        {
            float roomRight  = (float)kVirtualWidth  - cellW;
            float roomBottom = (float)kVirtualHeight - cellH;
            for (auto& enemy : _enemies)
            {
                if (!enemy->IsActive() || enemy->IsDying()) continue;
                Vector2 epos = enemy->GetWorldPos();
                bool clamped = false;
                if (epos.x < cellW)              { epos.x = cellW;        clamped = true; }
                if (epos.x > roomRight)          { epos.x = roomRight;    clamped = true; }
                if (epos.y < cellH)              { epos.y = cellH;        clamped = true; }
                if (epos.y > roomBottom)         { epos.y = roomBottom;   clamped = true; }
                if (clamped)
                {
                    bool specialStopHandled = false;
                    if (Molarbeast* mb = enemy->AsMolarbeast())
                    {
                        if (mb->IsDashing()) { mb->OnDashBlocked(); specialStopHandled = true; }
                    }
                    else if (Ogre* og = enemy->AsOgre())
                    {
                        if (og->IsRushing()) { og->OnRushBlocked(); specialStopHandled = true; }
                    }

                    if (!specialStopHandled)
                        enemy->Teleport(epos);
                }
            }
        }

        UpdateSpreadProjectiles(dt);
        UpdateLavaBallProjectiles(dt);
        UpdateCyclopsLasers(dt);
        UpdateEnemyProjectiles(dt);
        ApplyPendingReflect();
        UpdateWarlockMinions(dt);
        UpdateWarriorEffects(dt);
        UpdateMageSpells(dt);
        UpdatePoisonClouds(dt);
        _vfx.Update(dt);
        _damageNumbers.Update(dt);
        UpdateDungeonClearEffects(dt);
        UpdateEnemyCount(dt);

        // Collect gold coins and health pickups dropped by enemies.
        Vector2 lootCentre = _player.GetWorldPos();
        for (auto& pickup : _pickups)
        {
            if (!pickup->IsActive()) continue;
            // Loot magnetism — nearby drops vacuum toward the player.
            Vector2 pp = pickup->GetWorldPos();
            float dx = lootCentre.x - pp.x, dy = lootCentre.y - pp.y;
            if (dx * dx + dy * dy < 210.f * 210.f)
                pickup->Magnetize(lootCentre, std::min(1.f, GetFrameTime() * 9.f));
            if (CheckCollisionRecs(_player.GetCollisionRec(), pickup->GetCollisionRec()))
            {
                switch (pickup->GetType())
                {
                case PickupType::Gold: SfxBank::Get().Play(SfxId::PickupGold,  0.5f); break;
                case PickupType::Cell: SfxBank::Get().Play(SfxId::PickupCell,  0.6f); break;
                default:               SfxBank::Get().Play(SfxId::PickupMagic, 0.5f); break;   // Heal / Mana
                }
                _vfx.SpawnImpactBurst(pickup->GetWorldPos(), Color{ 255, 220, 120, 255 }, 5, 170.f);
                pickup->OnCollect(_player);
            }
        }
        _pickups.erase(
            std::remove_if(_pickups.begin(), _pickups.end(),
                [](const std::unique_ptr<Pickup>& pickup) { return !pickup->IsActive(); }),
            _pickups.end());

        // Elite-room mechanics (cage damage, bodyguard invuln, enrage, leap, hazards).
        if (_currentRoomType == RoomType::Elite && _eliteMechanic >= 0)
        {
            EliteMechanicsContext eliteCtx{};
            eliteCtx.currentRoomType        = _currentRoomType;
            eliteCtx.map                    = &_map;
            eliteCtx.mapScale               = _mapScale;
            eliteCtx.worldBoundsW           = sw;
            eliteCtx.worldBoundsH           = sh;
            eliteCtx.player                 = &_player;
            eliteCtx.enemies                = &_enemies;
            eliteCtx.lavaBalls              = &_lavaBalls;
            eliteCtx.eliteMechanic          = &_eliteMechanic;
            eliteCtx.eliteMinibossPtr       = &_eliteMinibossPtr;
            eliteCtx.eliteCageCenter        = &_eliteCageCenter;
            eliteCtx.eliteCageRadius        = &_eliteCageRadius;
            eliteCtx.eliteCageDamageTimer   = &_eliteCageDamageTimer;
            eliteCtx.eliteEnrageWarningTimer = &_eliteEnrageWarningTimer;
            eliteCtx.eliteHazardSpawnTimer  = &_eliteHazardSpawnTimer;
            eliteCtx.isSpawnPositionValid   = [&](Vector2 pos) { return IsSpawnPositionValid(pos); };
            eliteCtx.triggerScreenShake     = [&](float s, float d){ TriggerScreenShake(s, d); };
            _combatDirector.UpdateEliteMechanics(eliteCtx, dt);
        }

        // Drain pending EXP slowly (50/sec) so the HUD bar animates, then trigger
        // a LevelUpChoice screen for each level the player gains - same as the
        // wave-based ExpTally flow but without the full overlay screen.
        if (!_dungeonScrolling && _pendingExp > 0.f)
        {
            static constexpr float kExpDrainRate = 50.f;
            float drain        = std::min(kExpDrainRate * dt, _pendingExp);
            _pendingExp       -= drain;
            _expTallyAccum    += drain;

            int levelBefore = _player.GetLevel();
            int wholeExp    = (int)_expTallyAccum;
            if (wholeExp > 0)
            {
                _expTallyAccum -= (float)wholeExp;
                _player.AddExp(wholeExp);
            }
            if (_pendingExp <= 0.f)
            {
                _pendingExp    = 0.f;
                _expTallyAccum = 0.f;
            }
            _tallyLevelUpsRemaining += _player.GetLevel() - levelBefore;
        }

        // Show a LevelUpChoice card screen for every level gained once the EXP drain finishes.
        if (!_dungeonScrolling && _pendingExp <= 0.f && _tallyLevelUpsRemaining > 0)
        {
            _tallyLevelUpsRemaining--;
            SfxBank::Get().Play(SfxId::LevelUp, 0.75f);   // you leveled — card screen opens
            GenerateLevelUpOptions(LevelUpOfferContext::NormalLevel);
            _levelUpReturnState = GameState::DungeonRun;
            _levelUpOpenTimer   = 0.25f;
            _gameState          = GameState::LevelUpChoice;
        }

        // Holdout rooms are won by surviving the clock, not by exhausting a
        // fixed body count. Refill only after the field is empty so the room
        // remains pressuring without exceeding its geometry-derived active cap.
        if (_dungeonEnemiesSpawned &&
            _currentEncounterProfile == EncounterProfile::Holdout &&
            !_dungeonScrolling && !_roomObjectiveComplete)
        {
            _roomObjectiveTimer = std::max(0.f, _roomObjectiveTimer - dt);
            _dungeonRoomStates[_dungeonRoomIdx].holdoutTimeRemaining = _roomObjectiveTimer;
            if (_roomObjectiveTimer <= 0.f)
            {
                _roomObjectiveComplete = true;
                _dungeonReinforcements.clear();
                _pendingEnemySpawns.clear();
                for (auto& enemy : _enemies)
                    if (enemy->IsActive()) enemy->SetActive(false);
                _enemyProjectiles.clear();
                _cyclopsLasers.clear();
                _lavaBalls.clear();
                _poisonClouds.clear();
                _roomHazards.ClearRoom();
                _vfx.SpawnFloatingLabel(
                    { (float)kVirtualWidth * 0.5f, (float)kVirtualHeight * 0.30f },
                    "HOLDOUT COMPLETE", GOLD, 1.7f);
            }
            else if (GetActiveEnemyCount() == 0 && _dungeonReinforcements.empty() &&
                     _pendingEnemySpawns.empty())
            {
                const int refillCount = std::clamp(_dungeonOpeningCap / 2, 2, 4);
                for (int n = 0; n < refillCount; ++n)
                {
                    EncounterSpawnEntry entry{};
                    entry.kind = (n % 2 == 0) ? EnemySpawnKind::Shadow
                                              : EnemySpawnKind::Sporeling;
                    entry.role = EnemyRole::Grunt;
                    _dungeonReinforcements.push_back(entry);
                }
                _dungeonReinforcementTimer = 0.f;
            }
        }

        // -- Reinforcement waves (see Balance::Pressure / ReinforcementPacing) ----
        // Queued surplus bodies stream in when the field thins out or the wave
        // timer fires, so late-run populations pressure the player continuously
        // without opening the fight as an unreadable wall. Timer/active-count
        // expiry only makes a batch ELIGIBLE to reserve — TryReserveDungeon-
        // ReinforcementBatch (via ReinforcementBatchSize) still clamps the
        // actual reservation to the gap under the room's simultaneous body
        // target, 1-2 entries at a time, and never starts a new batch while a
        // previous batch's telegraph is still pending. Reserved entries do not
        // spawn immediately: they run the purple-circle/smoke telegraph
        // (AdvanceDungeonPendingSpawns, below) before SpawnDungeonGrunt is
        // ever called.
        if (_dungeonEnemiesSpawned && !_dungeonReinforcements.empty() && !_dungeonScrolling)
        {
            _dungeonReinforcementTimer -= dt;
            int activeEnemies = GetActiveEnemyCount();
            bool eligible = (_dungeonReinforcementTimer <= 0.f) ||
                           (activeEnemies <= Balance::Pressure::kReinforceRefillActive);
            if (eligible)
            {
                float waveCellW = (float)kVirtualWidth  / (float)RoomLayout::kCols;
                float waveCellH = (float)kVirtualHeight / (float)RoomLayout::kRows;
                const size_t reservedBefore = _pendingEnemySpawns.size();
                TryReserveDungeonReinforcementBatch(waveCellW, waveCellH);
                const int batchSize = (int)(_pendingEnemySpawns.size() - reservedBefore);
                _dungeonReinforcementTimer = Balance::Pressure::kReinforceInterval;
                if (batchSize > 0)
                    _vfx.SpawnFloatingLabel({ (float)kVirtualWidth * 0.5f, (float)kVirtualHeight * 0.30f },
                                            "REINFORCEMENTS!", Color{ 255, 170, 70, 255 }, 1.4f);
                if (_debug.IsActive())
                    TraceLog(LOG_INFO, "REINFORCEMENT batch reserved: +%d (queued left %d, pending %d)",
                             batchSize, (int)_dungeonReinforcements.size(), (int)_pendingEnemySpawns.size());
            }
        }

        // Advance any reserved reinforcement telegraphs (Circle -> Smoke ->
        // Ready). Runs independent of whether a new batch was reserved this
        // frame, and independent of the queue's own empty/non-empty state
        // (reserved entries already left the queue) — otherwise a batch's
        // telegraph could stall mid-sequence once the queue drains.
        if (!_pendingEnemySpawns.empty() && !_dungeonScrolling)
        {
            float waveCellW = (float)kVirtualWidth  / (float)RoomLayout::kCols;
            float waveCellH = (float)kVirtualHeight / (float)RoomLayout::kRows;
            AdvanceDungeonPendingSpawns(dt, waveCellW, waveCellH);
        }

        // -- Room clear detection -----------------------------------------------
        if (_dungeonEnemiesSpawned)
        {
            SaveDungeonRoomEnemyState();
            // Waves still pending — queued OR mid-telegraph — count as not cleared.
            bool allDead = _dungeonReinforcements.empty() && _pendingEnemySpawns.empty();
            for (const auto& e : _enemies)
                if (e->IsActive()) { allDead = false; break; }
            if (_currentEncounterProfile == EncounterProfile::Holdout &&
                !_roomObjectiveComplete)
                allDead = false;
            if (allDead)
            {
                if (_prologueActive)
                {
                    ProloguePhase before = _prologue.GetPhase();
                    _prologue.Update({ false, false, false, true, false });
                    ProloguePhase after = _prologue.GetPhase();
                    _pendingExp = 0.f;
                    _pickups.clear();

                    if (after == ProloguePhase::LastStand || before == ProloguePhase::LastStand)
                    {
                        // The last stand cannot be won. A full clear immediately
                        // rebuilds the firing ring without granting rewards.
                        DungeonRoomState& state = _dungeonRoomStates[_dungeonRoomIdx];
                        state.cleared = false;
                        state.enemiesInitialized = false;
                        state.survivors.clear();
                        _dungeonEnemiesSpawned = false;
                        SpawnDungeonRoomEnemies();
                        return;
                    }

                    if (after != before && after != ProloguePhase::Dash)
                    {
                        DungeonRoomState& state = _dungeonRoomStates[_dungeonRoomIdx];
                        state.cleared = true;
                        state.enemiesInitialized = true;
                        state.survivors.clear();
                        _dungeonEnemiesSpawned = false;
                        ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
                        SpawnDungeonDoorOpenEffects();
                        RebuildDungeonNav();
                    }
                    else if (after == before)
                    {
                        // Clearing without demonstrating the requested action
                        // repeats the tiny teaching encounter.
                        DungeonRoomState& state = _dungeonRoomStates[_dungeonRoomIdx];
                        state.enemiesInitialized = false;
                        state.survivors.clear();
                        _dungeonEnemiesSpawned = false;
                        SpawnDungeonRoomEnemies();
                    }
                    else
                    {
                        // Ability lesson is complete; wait for one dash before
                        // opening the east door into the last stand.
                        _dungeonEnemiesSpawned = false;
                    }
                    return;
                }

                int roomClearExp = Balance::Levelling::kStandardRoomClearExp;
                if (_currentRoomType == RoomType::Elite)
                    roomClearExp = Balance::Levelling::kEliteRoomClearExp;
                else if (_currentRoomType == RoomType::Boss)
                    roomClearExp = Balance::Levelling::kBossRoomClearExp;

                // Zeph's Wager payout: the harder room was cleared, so the
                // gamble pays bonus gold and +25% of the room's clear XP.
                if (_shopWagerRoomActive)
                {
                    _shopWagerRoomActive = false;
                    roomClearExp += roomClearExp / 4;
                    _player.AddGold(40);
                    _message = "Zeph's Wager pays out: +40 gold, bonus XP";
                }
                _pendingExp += (float)roomClearExp;

                // Class cards with "heal on room clear" (Battle Meditation /
                // Sanctuary) — sustain that respects the no-auto-heal economy.
                if (_player.GetHealOnRoomClear() > 0)
                    _player.Heal(_player.GetHealOnRoomClear());

                // Telemetry: record the cleared room for the debug overlay.
                {
                    RoomTelemetry& rec = _roomTelemetry[_roomTelemetryCount % kTelemetryHistory];
                    rec.act          = _currentAct;
                    rec.room         = _currentRoom;
                    snprintf(rec.roomType, sizeof(rec.roomType), "%s", GetDebugRoomTypeName(_currentRoomType));
                    rec.clearSeconds = (float)(GetTime() - _roomEnterTime);
                    rec.dmgTaken     = (int)std::ceil(_player.GetTelemDamageTaken());
                    rec.healed       = (int)_player.GetTelemHealed();
                    rec.healDrops    = _roomHealDrops;
                    rec.xpGained     = roomClearExp;
                    rec.goldDelta    = _player.GetGold() - _roomEnterGold;
                    rec.playerLevel  = _player.GetLevel();
                    rec.powerLevel   = 1 + (_wave - 1) / Balance::Curve::kRoomsPerPowerLevel;
                    _roomTelemetryCount++;
                }

                _dungeonRoomStates[_dungeonRoomIdx].cleared = true;
                _dungeonRoomStates[_dungeonRoomIdx].enemiesInitialized = true;
                _dungeonRoomStates[_dungeonRoomIdx].survivors.clear();
                _dungeonEnemiesSpawned = false;
                MusicCue victoryCue = ResolveRoomClearVictoryCue(_currentRoomType);
                if (victoryCue != MusicCue::None)
                    StartVictoryMusic(victoryCue);
                ResolveActiveModifiersOnRoomClear();   // Risk Shrine contract pays out
                ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
                if (_dungeonRoomIdx == _dungeonGen.GetKeyIndex() && !_magicGemCollected)
                {
                    _magicGemSpawned = true;
                    _message = "A magic gem appeared";
                }
                if (_dungeonRoomIdx == _dungeonGen.GetBossIndex())
                {
                    ApplyDungeonBossExitTiles(TileType::DoorOpen);
                    // Cinematic slow-mo now that the boss AND all its adds are down —
                    // longer than a normal kill for a proper victory beat.
                    TriggerSlowMo(_juiceBossSlowDur * 1.6f, _juiceBossSlowScale);
                }
                SpawnDungeonDoorOpenEffects();
                RebuildDungeonNav();

                // Treasure room: chest tile appears at screen centre after all enemies die.
                if (_currentRoomType == RoomType::Treasure)
                {
                    _treasureChestSpawned = true;
                    _treasureChestBroken  = false;
                    SetTreasureChestTile(TileType::ChestClosed);
                }
                // Elite room: scatter gold and offer an upgrade card.
                else if (_currentRoomType == RoomType::Elite && !_eliteRewardGranted)
                {
                    _eliteRewardGranted = true;
                    Vector2 dropAnchor = _player.GetWorldPos();
                    auto dropGold = [&](GoldDenomination denom, float ox, float oy)
                    {
                        auto g = std::make_unique<GoldPickup>();
                        g->Init({ dropAnchor.x + ox, dropAnchor.y + oy }, denom);
                        _pickups.push_back(std::move(g));
                    };
                    dropGold(GoldDenomination::Ten,    0.f,  -50.f);
                    dropGold(GoldDenomination::Five,  -55.f,  20.f);
                    dropGold(GoldDenomination::Five,   55.f,  20.f);
                    GenerateLevelUpOptions(LevelUpOfferContext::EliteReward);
                    _levelUpReturnState = GameState::DungeonRun;
                    _levelUpOpenTimer   = 0.5f;
                    _gameState          = GameState::LevelUpChoice;
                }
            }
        }

        // -- Boss room fallback: respawn 2 adds after 10s with no non-boss enemies --
        {
            int bossIdx = _dungeonGen.GetBossIndex();
            bool inBoss  = (_dungeonRoomIdx == bossIdx);
            bool cleared = inBoss && _dungeonRoomStates.count(bossIdx)
                           && _dungeonRoomStates[bossIdx].cleared;

            if (inBoss && !cleared && _dungeonEnemiesSpawned)
            {
                int nonBossCount = 0;
                for (const auto& e : _enemies)
                    if (e->IsActive() && !e->IsBoss()) nonBossCount++;

                if (nonBossCount == 0)
                {
                    _bossNoEnemyTimer += dt;
                    if (_bossNoEnemyTimer >= 10.f)
                    {
                        _bossNoEnemyTimer = 0.f;
                        float cellW = sw / (float)RoomLayout::kCols;
                        float cellH = sh / (float)RoomLayout::kRows;
                        SpawnBasicEnemy(GetDungeonSpawnPos(cellW, cellH));
                        SpawnBasicEnemy(GetDungeonSpawnPos(cellW, cellH));
                    }
                }
                else
                {
                    _bossNoEnemyTimer = 0.f;
                }
            }
            else
            {
                _bossNoEnemyTimer = 0.f;
            }
        }

        // -- Boss exit trigger --------------------------------------------------
        int bossIdx = _dungeonGen.GetBossIndex();
        if (_dungeonRoomIdx == bossIdx && _dungeonRoomStates[bossIdx].cleared)
        {
            Rectangle exitRect = GetDungeonBossExitTrigger();
            if (CheckCollisionPointRec(_player.GetWorldPos(), exitRect))
            {
                // Boss cleared - fade to black, then show the world map so player picks next biome.
                // Note: the fade handler sets FadingIn after the action fires; that stale state
                // is overwritten when UpdateWorldMap later calls EnterDungeonRoom.
                ClearDungeonEnemies();
                // Every boss now offers the return-vs-onward choice instead of
                // jumping straight to the world map.
                _dungeonFadePendingAction = [this]() { OpenBossChoice(); };
                _dungeonFadeState = DungeonFadeState::FadingOut;
                _dungeonFadeTimer = kDungeonFadeDuration;
                return;
            }
        }

        // Treasure chest overlap - player walks over the chest to open it.
        if (_treasureChestSpawned && !_treasureChestBroken)
        {
            // Body-overlap test (not a centre-point test) so brushing against
            // the chest is enough to open it.
            if (CheckCollisionRecs(_player.GetCollisionRec(), GetTreasureChestRect()))
            {
                _treasureChestBroken = true;
                OpenTreasureChest();
                return;
            }
        }

        // Room fills the screen - camera stays fixed at screen centre.
        _cameraPos = { sw * 0.5f, sh * 0.5f };
        return;
    }

    // -- Room view - static tile preview --------------------------------------
    if (_dungeonView == DungeonView::Room)
    {
        if (IsKeyPressed(KEY_ESCAPE))
        {
            _dungeonView          = DungeonView::Graph;
            _dungeonRoomIdx = -1;
        }

        // [Enter] enters the room with the player for walking around.
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
        {
            float hw = kVirtualWidth  * 0.5f;
            float hh = kVirtualHeight * 0.5f;
            _player.SetWorldPos({ hw, hh });
            _player.RefreshForRoomEntry();   // debug path — same no-heal rule as live rooms
            _cameraPos = { hw, hh };
            _dungeonView = DungeonView::Play;

            // Build nav grid (walls + props) and spawn room enemies.
            RebuildDungeonNav();
            ClearDungeonEnemies();
            SpawnDungeonRoomEnemies();
            InitBiomeModifierRoom();
            if (_dungeonRoomIdx == _dungeonGen.GetBossIndex())
                ApplyDungeonBossExitTiles(TileType::DoorLocked);
        }

        // [V] previews another approved visual palette for this room only. This
        // makes variant assignments easy to inspect before editing their weights.
        if (IsKeyPressed(KEY_V) && _dungeonVisualVariants.size() > 1 &&
            _dungeonRoomIdx >= 0 && _dungeonRoomIdx < (int)_dungeonRoomVisualVariants.size())
        {
            int nextVariant = (_dungeonRoomVisualVariants[_dungeonRoomIdx] + 1)
                            % (int)_dungeonVisualVariants.size();
            _dungeonRoomVisualVariants[_dungeonRoomIdx] = nextVariant;
            LoadDungeonVisualVariantAssets(nextVariant, _tileDefs, _tileRenderer);
            _currentDungeonVisualVariant = nextVariant;
            const DungeonRoom& room = _dungeonGen.GetRooms()[_dungeonRoomIdx];
            int densityBonus = (_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle) ? 2 : 0;
            _dungeonRoomLayout = RoomLayout::Generate(
                room.hasNorth, room.hasSouth, room.hasEast, room.hasWest, room.type,
                &_tileDefs, densityBonus);
            _dungeonRoomLayout.visualVariant = nextVariant;
            ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
            _message = "Visual variant: " + _dungeonVisualVariants[nextVariant].name;
        }

        // [B] cycles biome while previewing a room so you can see the tileset change live.
        if (IsKeyPressed(KEY_B))
        {
            int currentIdx = 0;
            for (int i = 0; i < kTilesetBiomeCount; i++)
                if (kTilesetBiomes[i] == _currentBiome) { currentIdx = i; break; }
            _currentBiome = kTilesetBiomes[(currentIdx + 1) % kTilesetBiomeCount];
            _pendingBiome = _currentBiome;
            LoadTilesetForBiome(_currentBiome);

            // Regenerate the room preview with the new biome's prop set.
            if (_dungeonRoomIdx >= 0 && _dungeonRoomIdx < (int)_dungeonGen.GetRooms().size())
            {
                const DungeonRoom& room = _dungeonGen.GetRooms()[_dungeonRoomIdx];
                int densityBonus = (_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle) ? 2 : 0;
                _dungeonRoomLayout = RoomLayout::Generate(
                    room.hasNorth, room.hasSouth, room.hasEast, room.hasWest, room.type,
                    &_tileDefs, densityBonus);
                _dungeonRoomLayout.visualVariant = _currentDungeonVisualVariant;
                ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
            }
        }
        return;
    }

    // Graph view.
    if (IsKeyPressed(KEY_ESCAPE))
    {
        _tileRenderer.Unload();
        _menu.Init();
        _gameState = GameState::Menu;
        return;
    }
    if (IsKeyPressed(KEY_R))
    {
        _dungeonGen.Generate();
        AssignDungeonVisualVariantWings();
    }

    // [B] cycles through all biomes so you can test each one in the map test mode.
    if (IsKeyPressed(KEY_B))
    {
        int currentIdx = 0;
        for (int i = 0; i < kTilesetBiomeCount; i++)
            if (kTilesetBiomes[i] == _currentBiome) { currentIdx = i; break; }
        _currentBiome = kTilesetBiomes[(currentIdx + 1) % kTilesetBiomeCount];
        _pendingBiome = _currentBiome;
        LoadTilesetForBiome(_currentBiome);
    }

    // Click a room node to open its tile-rendered preview.
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        const auto& rooms = _dungeonGen.GetRooms();
        for (int i = 0; i < (int)rooms.size(); i++)
        {
            if (CheckCollisionPointRec(GetVirtualMousePos(), GetDungeonRoomRect(i)))
            {
                const DungeonRoom& room = rooms[i];
                int visualVariant = GetDungeonVisualVariantForRoom(i);
                if (visualVariant != _currentDungeonVisualVariant)
                {
                    LoadDungeonVisualVariantAssets(visualVariant, _tileDefs, _tileRenderer);
                    _currentDungeonVisualVariant = visualVariant;
                }
                int densityBonus = (_currentBiome == Biome::Forest || _currentBiome == Biome::Jungle) ? 2 : 0;
                _dungeonRoomLayout    = RoomLayout::Generate(
                    room.hasNorth, room.hasSouth, room.hasEast, room.hasWest, room.type,
                    &_tileDefs, densityBonus);
                _dungeonRoomLayout.visualVariant = visualVariant;
                _dungeonRoomIdx = i;
                _dungeonEntryDoorSide = DungeonDoorSide::None;
                ApplyDungeonRoomDoorState(_dungeonRoomLayout, _dungeonRoomIdx, _dungeonEntryDoorSide);
                _dungeonView          = DungeonView::Room;
                break;
            }
        }
    }
}

void Engine::DrawDungeonRun()
{
    const float sw = (float)kVirtualWidth;
    const float sh = (float)kVirtualHeight;

    // Scale so the room fills the entire game window exactly.
    float scaleX = sw / (RoomLayout::kCols * 16.f);
    float scaleY = sh / (RoomLayout::kRows * 16.f);

    // -- Room type label helper ------------------------------------------------
    auto drawRoomLabel = [&]()
    {
        const auto& rooms = _dungeonGen.GetRooms();
        if (_dungeonRoomIdx < 0 || _dungeonRoomIdx >= (int)rooms.size()) return;
        int i = _dungeonRoomIdx;
        const char* label =
            i == _dungeonGen.GetStartIndex() ? "START ROOM" :
            i == _dungeonGen.GetBossIndex()  ? "BOSS ROOM"  :
            i == _dungeonGen.GetKeyIndex()   ? "KEY ROOM"   :
            [&]() -> const char* {
                switch (rooms[i].type) {
                case RoomType::Elite:    return "ELITE ROOM";
                case RoomType::Rest:     return "REST ROOM";
                case RoomType::Treasure: return "TREASURE ROOM";
                default:                 return "STANDARD ROOM";
                }
            }();
        int lw = MeasureText(label, 24);
        DrawText(label, (int)(sw * 0.5f - lw * 0.5f), 16, 24, GOLD);
        if (_debug.IsActive() && _currentDungeonVisualVariant >= 0 &&
            _currentDungeonVisualVariant < (int)_dungeonVisualVariants.size())
        {
            const std::string& visualName = _dungeonVisualVariants[_currentDungeonVisualVariant].name;
            int visualW = MeasureText(visualName.c_str(), 16);
            DrawText(visualName.c_str(), (int)(sw * 0.5f - visualW * 0.5f), 44, 16,
                     Fade(RAYWHITE, 0.65f));
        }
        if (_debug.IsActive())
        {
            const char* profile = "SKIRMISH";
            switch (_currentEncounterProfile)
            {
            case EncounterProfile::Assault: profile = "ASSAULT"; break;
            case EncounterProfile::Swarm: profile = "SWARM"; break;
            case EncounterProfile::Holdout: profile = "HOLDOUT"; break;
            default: break;
            }
            const char* capacity = TextFormat(
                "%s | %s CAP | OPEN %d / TOTAL %d | WALK %d",
                profile, RoomCapacityAnalyzer::BandName(_roomCombatCapacity.band),
                _roomCombatCapacity.openingBodyCap, _roomCombatCapacity.totalBodyCap,
                _roomCombatCapacity.connectedTiles);
            const int capacityW = MeasureText(capacity, 15);
            DrawText(capacity, (int)(sw * 0.5f - capacityW * 0.5f), 64, 15,
                     Fade(Color{ 160, 220, 255, 255 }, 0.82f));
        }
    };

    // -- Play view - tile room with live player --------------------------------
    if (_dungeonView == DungeonView::Play)
    {
        ClearBackground(Color{ 8, 6, 10, 255 });

        if (_tileRenderer.IsLoaded())
        {
            if (_dungeonScrolling)
            {
                float t    = _dungeonScrollT;
                float ease = t * t * (3.f - 2.f * t);
                Vector2 curOff{  _dungeonScrollVec.x * ease * sw,
                                 _dungeonScrollVec.y * ease * sh };
                Vector2 nextOff{ curOff.x - _dungeonScrollVec.x * sw,
                                 curOff.y - _dungeonScrollVec.y * sh };
                _tileRenderer.DrawRoom(_dungeonRoomLayout,       scaleX, scaleY, curOff);
                const TileRenderer& nextRenderer = _dungeonScrollTileRenderer.IsLoaded()
                    ? _dungeonScrollTileRenderer : _tileRenderer;
                nextRenderer.DrawRoom(_dungeonScrollNextLayout, scaleX, scaleY, nextOff);
            }
            else
            {
                // Y-sort vs the player: props whose BASE is above the player's
                // feet draw here (behind, the old order); props whose base is
                // below the feet draw after the player, so standing behind a
                // tree tucks the player under its canopy.
                float playerFeetScreenY = _player.GetFeetWorldPos().y
                                        - (_cameraPos.y - _shakeOffset.y)
                                        + sh * 0.5f;
                _tileRenderer.DrawRoom(_dungeonRoomLayout, scaleX, scaleY, _shakeOffset, false);
                _tileRenderer.DrawRoomPropsSplit(_dungeonRoomLayout, scaleX, scaleY,
                                                 _shakeOffset, playerFeetScreenY, false);
                DrawDungeonClearEffects();
                DrawDungeonMagicGemAndBarrier();
                DrawBiomeModifiers();
                _roomHazards.Draw(_shakeOffset);   // environmental room hazards

                // Enemies, projectiles, VFX - world == screen in dungeon run mode.
                // _shakeOffset shifts everything together so the screen-shake effect is visible.
                Vector2 worldOffset{ -_cameraPos.x + _shakeOffset.x, -_cameraPos.y + _shakeOffset.y };
                Vector2 shakenCamRef{ _cameraPos.x - _shakeOffset.x, _cameraPos.y - _shakeOffset.y };

                // Elite signature warnings (charge lanes, fissure outlines,
                // landing markers) draw in WORLD space under the enemies and
                // projectiles — one matrix translation supplies the camera.
                rlPushMatrix();
                rlTranslatef(sw * 0.5f - shakenCamRef.x, sh * 0.5f - shakenCamRef.y, 0.f);
                _combatDirector.DrawEliteWorld(_enemies);
                rlPopMatrix();
                for (const auto& proj : _spreadProjectiles)
                    proj.Draw(worldOffset);
                for (const auto& proj : _lavaBalls)
                    if (proj.IsActive())
                        proj.Draw(worldOffset);
                DrawPoisonClouds(worldOffset);
                DrawWarlockMinions(worldOffset);
                DrawEnemyProjectiles(worldOffset);
                DrawCyclopsLasers(worldOffset);
                _vfx.Draw(worldOffset, _player.GetWorldPos(), _player.GetCastOrigin());
                DrawPendingEnemySpawns(worldOffset);   // purple-circle/smoke reinforcement telegraphs
                for (auto& pickup : _pickups)
                    pickup->Draw(worldOffset);
                // Treasure chest tile (ChestClosed / ChestOpen) is drawn by the tile renderer.
                // Show a hint label above the chest while it is unopened.
                if (_treasureChestSpawned && !_treasureChestBroken)
                {
                    const Vector2 chest = GetTreasureChestWorldPos();
                    float cx = chest.x - _cameraPos.x + sw * 0.5f + _shakeOffset.x;
                    float cy = chest.y - _cameraPos.y + sh * 0.5f + _shakeOffset.y;
                    float pulse = 0.65f + 0.35f * sinf((float)GetTime() * 3.f);
                    const char* hint = "Walk over to open!";
                    int hintSz = 22;
                    int hintW  = MeasureText(hint, hintSz);
                    DrawText(hint, (int)(cx - hintW * 0.5f), (int)(cy - 72), hintSz,
                             Fade(GOLD, pulse));
                }

                for (auto& enemy : _enemies)
                {
                    if (!enemy->IsActive()) continue;
                    enemy->DrawEnemy(shakenCamRef);
                }

                // ── Shared status-effect overlays ──────────────────────────────
                // Drawn centrally over every enemy/boss (so no per-enemy DrawEnemy
                // needs to know about statuses): poison bubbles, bleed drips, an
                // armor-break crack ring, a chill haze, and an execute reticle.
                for (const auto& enemy : _enemies)
                {
                    if (!enemy->IsActive() || enemy->IsDying()) continue;
                    const bool anyStatus = enemy->IsPoisoned() || enemy->IsBleeding() ||
                                           enemy->IsVulnerable() || enemy->IsSlowed() || enemy->IsMarked();
                    if (!anyStatus) continue;

                    Rectangle wr = enemy->GetCollisionRec();
                    Vector2 c = Vector2Subtract(Vector2{ wr.x + wr.width * 0.5f, wr.y + wr.height * 0.5f }, shakenCamRef);
                    c.x += sw * 0.5f; c.y += sh * 0.5f;
                    float bodyR = std::max(60.f, std::max(wr.width, wr.height) * 0.55f);
                    float t = (float)GetTime();

                    // Chill haze (slow) — bold blue aura + orbiting frost motes.
                    if (enemy->IsSlowed())
                    {
                        DrawCircleV(c, bodyR, Fade(Color{ 120, 200, 255, 255 }, 0.18f));
                        DrawCircleLines((int)c.x, (int)c.y, bodyR, Fade(Color{ 150, 215, 255, 255 }, 0.9f));
                        DrawCircleLines((int)c.x, (int)c.y, bodyR + 4.f, Fade(Color{ 150, 215, 255, 255 }, 0.5f));
                        for (int i = 0; i < 5; ++i)
                        {
                            float a = t * 1.4f + i * (2.f * PI / 5.f);
                            Vector2 p{ c.x + cosf(a) * bodyR, c.y + sinf(a) * bodyR * 0.7f };
                            DrawCircleV(p, 5.f, Fade(Color{ 220, 245, 255, 255 }, 0.95f));
                        }
                    }
                    // Armor break / vulnerability — thick pulsing broken orange ring + glow.
                    if (enemy->IsVulnerable())
                    {
                        float pulse = 0.5f + 0.5f * sinf(t * 9.f);
                        DrawCircleV(c, bodyR * 0.9f, Fade(Color{ 255, 130, 40, 255 }, 0.12f + 0.10f * pulse));
                        for (int i = 0; i < 8; ++i)
                        {
                            float a0 = i * (2.f * PI / 8.f) + t * 0.7f;
                            float a1 = a0 + 0.34f;
                            Vector2 p0{ c.x + cosf(a0) * bodyR, c.y + sinf(a0) * bodyR };
                            Vector2 p1{ c.x + cosf(a1) * bodyR, c.y + sinf(a1) * bodyR };
                            DrawLineEx(p0, p1, 5.f, Fade(Color{ 255, 160, 60, 255 }, 0.7f + 0.3f * pulse));
                        }
                    }
                    // Poison — fat green bubbles streaming up + faint green haze.
                    if (enemy->IsPoisoned())
                    {
                        DrawCircleV(c, bodyR * 0.85f, Fade(Color{ 90, 210, 70, 255 }, 0.14f));
                        for (int i = 0; i < 6; ++i)
                        {
                            float ph = fmodf(t * 1.0f + i * 0.17f, 1.f);
                            float bx = c.x + sinf(t * 3.f + i * 2.1f) * bodyR * 0.6f;
                            float by = c.y + bodyR * 0.4f - ph * bodyR * 1.7f;
                            DrawCircleV({ bx, by }, (1.f - ph) * 9.f + 2.5f, Fade(Color{ 130, 235, 90, 255 }, 0.95f * (1.f - ph)));
                        }
                    }
                    // Bleed — red droplets spraying off the body.
                    if (enemy->IsBleeding())
                    {
                        for (int i = 0; i < 6; ++i)
                        {
                            float ph = fmodf(t * 1.8f + i * 0.22f, 1.f);
                            float bx = c.x + (i - 2.5f) * bodyR * 0.35f;
                            float by = c.y - bodyR * 0.1f + ph * bodyR * 1.1f;
                            DrawCircleV({ bx, by }, 5.f * (1.f - ph * 0.5f), Fade(Color{ 210, 25, 25, 255 }, 0.95f * (1.f - ph)));
                        }
                    }
                    // Marked / execute — big pulsing red reticle above the head.
                    if (enemy->IsMarked())
                    {
                        float my = c.y - bodyR - 26.f;
                        float bob = sinf(t * 6.f) * 4.f;
                        float sc = 1.f + 0.15f * sinf(t * 8.f);
                        Color mk = Color{ 255, 60, 60, 255 };
                        DrawLineEx({ c.x - 16.f * sc, my + bob }, { c.x, my + 14.f * sc + bob }, 5.f, mk);
                        DrawLineEx({ c.x + 16.f * sc, my + bob }, { c.x, my + 14.f * sc + bob }, 5.f, mk);
                        DrawCircleLines((int)c.x, (int)(my + 4.f + bob), 20.f * sc, Fade(mk, 0.7f));
                    }
                }

                // Warrior melee VFX (spins, waves, spikes) drawn over the enemies.
                DrawWarriorEffects(shakenCamRef);
                DrawMageSpells(shakenCamRef);

                // Dream Realm flicker - pulsing ring at windup position + destination marker.
                if (_currentBiome == Biome::DreamRealm)
                {
                    float pulse = sinf((float)GetTime() * 18.f) * 0.35f + 0.65f;
                    for (const auto& enemy : _enemies)
                    {
                        if (!enemy->IsActive() || !enemy->IsFlickerInWindup()) continue;

                        Vector2 srcScreen = Vector2Subtract(enemy->GetWorldPos(), shakenCamRef);
                        srcScreen.x += sw * 0.5f;
                        srcScreen.y += sh * 0.5f;

                        Vector2 dstScreen = Vector2Subtract(enemy->GetFlickerTarget(), shakenCamRef);
                        dstScreen.x += sw * 0.5f;
                        dstScreen.y += sh * 0.5f;

                        // Ring where the enemy is fading out.
                        DrawCircleLines((int)srcScreen.x, (int)srcScreen.y, 40.f,
                            Fade(Color{ 180, 100, 255, 255 }, pulse));
                        DrawCircleLines((int)srcScreen.x, (int)srcScreen.y, 26.f,
                            Fade(Color{ 200, 140, 255, 255 }, pulse * 0.6f));

                        // Smaller ring marking the destination.
                        DrawCircleLines((int)dstScreen.x, (int)dstScreen.y, 28.f,
                            Fade(Color{ 130, 60, 220, 255 }, 0.55f));

                        // Faint line connecting source to destination.
                        DrawLineEx(srcScreen, dstScreen, 1.5f,
                            Fade(Color{ 180, 100, 255, 255 }, 0.25f));
                    }
                }

                if (_currentRoomType == RoomType::Store)
                {
                    _shop.DrawNpc(worldOffset);

                    // Poe — a hovering ghostly spirit (Phantom sprite) who keeps your
                    // Echoes, with a small floating Echo above his head. He IS the
                    // glow (a spirit), so there's no painted feet-circle — just a
                    // faint pale aura around him.
                    {
                        Vector2 altarPos = GetPoeAltarPos();
                        float bobOffset  = sinf(_poeAltarBobTimer * 2.2f) * 5.f;
                        Vector2 altarScreen{ altarPos.x + worldOffset.x + kVirtualWidth * 0.5f,
                                             altarPos.y + worldOffset.y + kVirtualHeight * 0.5f };

                        // Faint pale-lavender spirit aura that breathes around Poe.
                        float auraPulse = 0.10f + 0.05f * sinf(_poeAltarBobTimer * 2.4f);
                        DrawCircleV(Vector2{ altarScreen.x, altarScreen.y + bobOffset }, 48.f,
                            Fade(Color{ 184, 170, 236, 255 }, auraPulse));

                        // Merchant sprite (animated idle: cycle the 6 idle frames).
                        if (_cellsMerchantTex.id != 0)
                        {
                            const int frames = 6;
                            float fw = _cellsMerchantTex.width / (float)frames;
                            int frame = ((int)(_poeAltarBobTimer * 6.f)) % frames;
                            float mScale = 3.2f;
                            Rectangle mSrc{ frame * fw, 0.f, fw, (float)_cellsMerchantTex.height };
                            Rectangle mDst{ altarScreen.x, altarScreen.y + bobOffset,
                                            fw * mScale, _cellsMerchantTex.height * mScale };
                            DrawTexturePro(_cellsMerchantTex, mSrc, mDst,
                                Vector2{ mDst.width * 0.5f, mDst.height * 0.5f }, 0.f, WHITE);
                        }

                        // "Poe" nameplate (mirrors Zeph's, in spectral lavender).
                        {
                            const char* np = "Poe";
                            int npFs = 28, npW = MeasureText(np, npFs);
                            float npY = altarScreen.y - 104.f + bobOffset;
                            DrawRectangle((int)(altarScreen.x - npW * 0.5f - 10.f), (int)npY,
                                          npW + 20, 34, Fade(BLACK, 0.6f));
                            DrawText(np, (int)(altarScreen.x - npW * 0.5f),
                                     (int)(npY + 17.f - npFs * 0.5f), npFs, Color{ 212, 194, 255, 255 });
                        }

                        // Small floating Echo bobbing beside Poe (marks him the keeper).
                        {
                            const Texture2D& cellTexture = CellPickup::GetMediumTexture();
                            float cellScale = 2.6f;
                            float cellBob = sinf(_poeAltarBobTimer * 2.6f) * 5.f;
                            Rectangle cSrc{ 0.f, 0.f, (float)cellTexture.width, (float)cellTexture.height };
                            Rectangle cDst{ altarScreen.x, altarScreen.y - 66.f + cellBob,
                                            cellTexture.width * cellScale, cellTexture.height * cellScale };
                            DrawTexturePro(cellTexture, cSrc, cDst,
                                Vector2{ cDst.width * 0.5f, cDst.height * 0.5f }, 0.f, WHITE);
                        }

                        if (_nearPoeAltar && !_shop.IsNearNpc())
                        {
                            const char* altarPrompt = "[E] Poe";
                            int promptFontSize = 26;
                            int promptWidth = MeasureText(altarPrompt, promptFontSize);
                            DrawText(altarPrompt,
                                (int)(altarScreen.x - promptWidth * 0.5f),
                                (int)(altarScreen.y + 96.f + bobOffset),
                                promptFontSize, Color{ 212, 194, 255, 255 });
                        }

                        // Cursed Shrine — a dark pedestal with a swirling red orb.
                        // Only manifests once "push onward" has unlocked the wager.
                        if (_wagerAccessGranted)
                        {
                            Vector2 sp = GetCurseShrinePos();
                            Vector2 ss{ sp.x + worldOffset.x + kVirtualWidth * 0.5f,
                                        sp.y + worldOffset.y + kVirtualHeight * 0.5f };
                            float bob = sinf(_curseShrineBobTimer * 2.2f) * 5.f;
                            bool used = _curseShrineUsed;
                            DrawRectangleRounded({ ss.x - 28.f, ss.y + 12.f, 56.f, 26.f }, 0.4f, 6, Color{ 40, 26, 32, 255 });
                            Color orbCol = used ? Color{ 100, 78, 88, 255 } : Color{ 232, 70, 110, 255 };
                            float glow = used ? 0.08f : (0.18f + 0.08f * sinf(_curseShrineBobTimer * 3.f));
                            DrawCircleV({ ss.x, ss.y - 6.f + bob }, 42.f, Fade(orbCol, glow));
                            DrawCircleV({ ss.x, ss.y - 6.f + bob }, 16.f, Fade(orbCol, used ? 0.4f : 0.9f));
                            if (!used)
                                for (int i = 0; i < 3; i++)
                                {
                                    float a = _curseShrineBobTimer * 3.f + i * 2.094f;
                                    DrawCircleV({ ss.x + cosf(a) * 22.f, ss.y - 6.f + bob + sinf(a) * 22.f }, 4.f, Fade(orbCol, 0.85f));
                                }
                            if (_nearCurseShrine && !_shop.IsNearNpc())
                            {
                                const char* pr = "[E] Cursed Wager";
                                int fs = 26, w = MeasureText(pr, fs);
                                DrawText(pr, (int)(ss.x - w * 0.5f), (int)(ss.y - 92.f), fs, Color{ 255, 120, 150, 255 });
                            }
                        }
                    }
                }

                // Decision-room shrine (Risk Shrine, …) — an amber rune-altar at
                // room centre; dims once its contract has been bound.
                {
                    auto drsIt = _dungeonRoomStates.find(_dungeonRoomIdx);
                    if (drsIt != _dungeonRoomStates.end() &&
                        drsIt->second.special != RoomSpecialType::None)
                    {
                        bool used = drsIt->second.specialClaimed;
                        Vector2 sp = GetDecisionShrinePos();
                        Vector2 ss{ sp.x + worldOffset.x + kVirtualWidth * 0.5f,
                                    sp.y + worldOffset.y + kVirtualHeight * 0.5f };
                        float bob = sinf(_decisionShrineBobTimer * 2.2f) * 5.f;
                        DrawRectangleRounded({ ss.x - 30.f, ss.y + 12.f, 60.f, 28.f }, 0.4f, 6, Color{ 40, 32, 22, 255 });
                        Color orbCol = used ? Color{ 96, 86, 70, 255 } : Color{ 240, 176, 80, 255 };
                        float glow = used ? 0.08f : (0.18f + 0.08f * sinf(_decisionShrineBobTimer * 3.f));
                        DrawCircleV({ ss.x, ss.y - 6.f + bob }, 44.f, Fade(orbCol, glow));
                        DrawCircleV({ ss.x, ss.y - 6.f + bob }, 16.f, Fade(orbCol, used ? 0.4f : 0.9f));
                        if (!used)
                            for (int i = 0; i < 3; i++)
                            {
                                float a = _decisionShrineBobTimer * 3.f + i * 2.094f;
                                DrawCircleV({ ss.x + cosf(a) * 24.f, ss.y - 6.f + bob + sinf(a) * 24.f }, 4.f, Fade(orbCol, 0.85f));
                            }
                        if (_nearDecisionShrine && !used)
                        {
                            const char* pr = "[E] Risk Shrine";
                            int fs = 26, w = MeasureText(pr, fs);
                            DrawText(pr, (int)(ss.x - w * 0.5f), (int)(ss.y - 92.f), fs, Color{ 255, 196, 110, 255 });
                        }
                    }
                }

                _player.DrawPlayer(shakenCamRef);
                DrawAbilityAimPreview();

                // Front half of the Y-sort: props the player is standing
                // behind draw over them (see split above).
                _tileRenderer.DrawRoomPropsSplit(_dungeonRoomLayout, scaleX, scaleY,
                                                 _shakeOffset, playerFeetScreenY, true);
                _damageNumbers.Draw(worldOffset);
            }
        }
        else
        {
            DrawText("Tilesheet not loaded.", 20, 20, 22, RED);
        }

        if (!_dungeonScrolling)
        {
            DrawHUD();

            if (_currentEncounterProfile == EncounterProfile::Holdout &&
                _dungeonEnemiesSpawned && !_roomObjectiveComplete)
            {
                const char* objective = TextFormat("SURVIVE  %.1f", _roomObjectiveTimer);
                const int objectiveFs = 30;
                const int objectiveW = MeasureText(objective, objectiveFs);
                Rectangle objectiveBox{
                    sw * 0.5f - objectiveW * 0.5f - 18.f, 72.f,
                    (float)objectiveW + 36.f, 46.f
                };
                DrawRectangleRounded(objectiveBox, 0.18f, 6, Fade(BLACK, 0.82f));
                DrawRectangleRoundedLines(objectiveBox, 0.18f, 6,
                                          Color{ 255, 180, 70, 230 });
                DrawText(objective, (int)(sw * 0.5f - objectiveW * 0.5f), 80,
                         objectiveFs, RAYWHITE);
            }

            // Polished dungeon HUD: intro banner (temporary), badge (special
            // rooms only), modifier pills (top-right, nothing when empty),
            // and the corner minimap with room-type icons.
            DrawRoomIntroBanner();
            DrawRoomBadge();
            {
                constexpr float kMiniMapCell = 30.f;
                const float mapW = DungeonGen::kGridSize * kMiniMapCell + kMiniMapCell;
                DrawDungeonMiniMap(sw - mapW - 10.f, 8.f, kMiniMapCell, false);
            }
            DrawModifierStack();

            DrawUltimateSequence();
            DrawMagicGemHudIcon();
            if (_debug.IsActive()) drawRoomLabel();   // room label is debug info now

            if (_prologueActive)
            {
                const KeyBindings& bindings = _player.GetBindings();
                const char* prompt = "";
                switch (_prologue.GetPhase())
                {
                case ProloguePhase::BasicAttack:
                    prompt = TextFormat("They're coming. %s - %s.",
                        bindings.attack == KEY_NULL ? "LMB" : GetKeyName(bindings.attack),
                        GetPrologueBasicAttackName(_player.GetClass()));
                    break;
                case ProloguePhase::Ability:
                    prompt = TextFormat("Use what you know. %s - %s.",
                        GetKeyName(bindings.ability[0]),
                        GetAbilityName(_player.GetLearnedAbility(0)));
                    break;
                case ProloguePhase::Dash:
                    prompt = TextFormat("Move! %s to dash through danger.", GetKeyName(bindings.dash));
                    break;
                case ProloguePhase::LastStand:
                    prompt = TextFormat("There are too many. Keep moving.  HP %d / 3",
                                        3 - _prologue.GetLastStandHits());
                    break;
                default: break;
                }
                int fs = 30;
                int width = MeasureText(prompt, fs);
                Rectangle box{ sw * 0.5f - width * 0.5f - 20.f, 72.f, (float)width + 40.f, 52.f };
                DrawRectangleRounded(box, 0.18f, 6, Fade(BLACK, 0.78f));
                DrawRectangleRoundedLines(box, 0.18f, 6, Fade(GOLD, 0.85f));
                DrawText(prompt, (int)(sw * 0.5f - width * 0.5f), 83, fs, RAYWHITE);
                if (_prologue.GetPhase() == ProloguePhase::LastStand)
                {
                    const int storyHealth = 3 - _prologue.GetLastStandHits();
                    const float segmentW = 46.f;
                    const float totalW = segmentW * 3.f + 12.f;
                    for (int segment = 0; segment < 3; ++segment)
                    {
                        Rectangle hpSegment{ sw * 0.5f - totalW * 0.5f + segment * (segmentW + 6.f),
                                             box.y + box.height + 8.f, segmentW, 12.f };
                        DrawRectangleRounded(hpSegment, 0.35f, 4,
                            segment < storyHealth ? Color{ 220, 50, 58, 255 } : Color{ 64, 38, 42, 220 });
                        DrawRectangleRoundedLines(hpSegment, 0.35f, 4, Fade(BLACK, 0.75f));
                    }
                }
                if (_demoCompleted)
                    DrawText("[F8] Skip Onboarding", (int)sw - 260, 22, 20, Fade(RAYWHITE, 0.75f));
            }

            // Small biome name reminder in the bottom-right corner - useful when
            // cycling biomes in the pregen map test without losing track of which one is active.
            {
                if (_debug.IsActive())   // biome reminder is dev info, not gameplay HUD
                {
                    const char* biomeTag = TextFormat("[ %s ]", GetBiomeName(_currentBiome));
                    int biomeTagW = MeasureText(biomeTag, 16);
                    DrawText(biomeTag, (int)(sw - biomeTagW - 12), (int)(sh - 26), 16,
                        Fade(Color{ 160, 200, 255, 255 }, 0.55f));
                }

                if (_isDailyRun)
                {
                    const char* dailyTag = TextFormat("DAILY RUN  #%d", _dailySeed);
                    int dw = MeasureText(dailyTag, 16);
                    DrawText(dailyTag, (int)(sw - dw - 12), (int)(sh - 46), 16,
                        Fade(Color{ 255, 210, 120, 255 }, 0.75f));
                }
            }
            // Hold TAB: full-screen dungeon map overlay for planning.
            if (IsKeyDown(KEY_TAB) && !_dungeonScrolling)
            {
                DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.72f));
                constexpr float kOverlayCell = 64.f;
                const float overlayW = DungeonGen::kGridSize * kOverlayCell + kOverlayCell;
                DrawDungeonMiniMap(sw * 0.5f - overlayW * 0.5f,
                                   sh * 0.5f - overlayW * 0.5f, kOverlayCell, true);
            }

            if (_bossBarrierMessageTimer > 0.f)
            {
                const char* msg = "you need a a magic gem to get past the barrier";
                int mw = MeasureText(msg, 24);
                DrawText(msg, (int)(sw * 0.5f - mw * 0.5f), 52, 24, Color{ 190, 130, 255, 255 });
            }

            if (_secondWindToastTimer > 0.f)
            {
                float a = std::min(1.f, _secondWindToastTimer / 2.5f);
                const char* msg = "SECOND WIND!";
                int mw = MeasureText(msg, 54);
                DrawText(msg, (int)(sw * 0.5f - mw * 0.5f + 2), (int)(sh * 0.3f + 2), 54, Fade(BLACK, a));
                DrawText(msg, (int)(sw * 0.5f - mw * 0.5f), (int)(sh * 0.3f), 54, Fade(Color{ 120, 255, 170, 255 }, a));
            }

            if (_debug.IsActive())
            {
                DrawDebugToggleTab();
                if (_debug.IsOpen())
                {
                    _debug.Draw(_currentAct, _currentRoom, GetDebugRoomTypeName(_currentRoomType));
                    DrawRoomTelemetry();   // per-room balance readout (Phase 5)
                    if (_gameState == GameState::DungeonRun)
                        DrawEnemyFacingDebug();   // facing arrows + front/rear cones
                    DrawEliteSignatureTelemetry();   // elite state/phase/casts readout
                }
            }
            if (_isHitboxEditorActive)
                DrawHitboxEditor();

            // Dialogue is deliberately after gameplay and debug HUD. The
            // dialogue-layout editor remains last so its handles stay usable.
            if (_cutscene.IsActive())
                _cutscene.Draw(_shopBorderTex, _shopZephTex, GetPromptModeForUi());
            if (_isDlgEditorActive)
                DrawDialogueBoxEditor();
        }

        // -- Dungeon fade overlay (covers HUD, enemies, everything) ------------
        if (_dungeonFadeAlpha > 0.f)
        {
            unsigned char alpha = (unsigned char)std::clamp((int)_dungeonFadeAlpha, 0, 255);
            DrawRectangle(0, 0, (int)sw, (int)sh, Color{ 0, 0, 0, alpha });
        }
        return;
    }

    // -- Room view - static tile preview --------------------------------------
    if (_dungeonView == DungeonView::Room)
    {
        ClearBackground(Color{ 8, 6, 10, 255 });

        if (_tileRenderer.IsLoaded())
            _tileRenderer.DrawRoom(_dungeonRoomLayout, scaleX, scaleY, { 0.f, 0.f });
        else
            DrawText("Tilesheet not loaded. Check the path in Engine.cpp.",
                20, 20, 22, RED);

        drawRoomLabel();
        DrawText(TextFormat("[Enter] Walk in room   [B] Cycle Biome   [ESC] Back  |  Biome: %s",
            GetBiomeName(_currentBiome)), 20, (int)(sh - 32.f), 18, Fade(WHITE, 0.55f));
        return;
    }

    // -- Graph view ------------------------------------------------------------
    ClearBackground(Color{ 14, 14, 20, 255 });

    const auto& rooms = _dungeonGen.GetRooms();
    if (rooms.empty())
    {
        DrawText("No rooms generated.", 40, 40, 28, RED);
        return;
    }

    // Fit the dungeon grid into the centre of the screen.
    const float margin  = 90.f;
    const float gridPx  = std::min((sw - margin * 2.f) / DungeonGen::kGridSize,
                                   (sh - margin * 2.f - 80.f) / DungeonGen::kGridSize);
    const float startX  = sw * 0.5f - (gridPx * DungeonGen::kGridSize) * 0.5f;
    const float startY  = margin;

    int startIdx = _dungeonGen.GetStartIndex();
    int bossIdx  = _dungeonGen.GetBossIndex();
    int keyIdx   = _dungeonGen.GetKeyIndex();

    // Connections drawn first so they sit behind room squares.
    for (const auto& room : rooms)
    {
        float cx = startX + (room.col + 0.5f) * gridPx;
        float cy = startY + (room.row + 0.5f) * gridPx;
        if (room.hasEast)
            DrawLineEx({ cx, cy }, { cx + gridPx, cy }, 3.f, Fade(WHITE, 0.25f));
        if (room.hasSouth)
            DrawLineEx({ cx, cy }, { cx, cy + gridPx }, 3.f, Fade(WHITE, 0.25f));
    }

    // Room squares.
    for (int i = 0; i < (int)rooms.size(); i++)
    {
        const DungeonRoom& room = rooms[i];
        float pad = gridPx * 0.12f;
        Rectangle rect{
            startX + room.col * gridPx + pad,
            startY + room.row * gridPx + pad,
            gridPx - pad * 2.f,
            gridPx - pad * 2.f
        };

        Color col;
        const char* label = "";
        if (i == startIdx) { col = Color{ 70,  210,  80, 255 }; label = "START";   }
        else if (i == bossIdx)  { col = Color{210,  50,   50, 255 }; label = "BOSS";    }
        else if (i == keyIdx)   { col = Color{160,  80,  220, 255 }; label = "KEY";     }
        else switch (room.type)
        {
        case RoomType::Elite:    col = Color{220, 130,  40, 255 }; label = "ELITE";    break;
        case RoomType::Rest:     col = Color{ 55, 160,  80, 255 }; label = "REST";     break;
        case RoomType::Treasure: col = Color{200, 180,  40, 255 }; label = "CHEST";    break;
        case RoomType::Store:    col = Color{ 55, 140, 200, 255 }; label = "SHOP";     break;
        default:                 col = Color{ 75,  75,  90, 255 }; break;
        }

        DrawRectangleRounded(rect, 0.22f, 6, col);
        DrawRectangleRoundedLines(rect, 0.22f, 6, Fade(WHITE, 0.35f));

        if (label[0] != '\0')
        {
            int fs = (int)(gridPx * 0.16f);
            fs = std::max(8, std::min(fs, 20));
            int lw = MeasureText(label, fs);
            DrawText(label,
                (int)(rect.x + rect.width  * 0.5f - lw * 0.5f),
                (int)(rect.y + rect.height * 0.5f - fs * 0.5f),
                fs, WHITE);
        }
    }

    // Title
    const char* title = "PREGEN MAP TEST";
    int titleW = MeasureText(title, 32);
    DrawText(title, (int)(sw * 0.5f - titleW * 0.5f), 16, 32, GOLD);

    // Active biome name shown beneath the title.
    const char* biomeLine = TextFormat("Biome: %s", GetBiomeName(_currentBiome));
    int biomeLineW = MeasureText(biomeLine, 20);
    DrawText(biomeLine, (int)(sw * 0.5f - biomeLineW * 0.5f), 54, 20, Color{ 160, 200, 255, 220 });

    const char* info = TextFormat("%d rooms  |  Click a room to preview  |  [R] Regenerate  |  [B] Cycle Biome  |  [ESC] Back",
        (int)rooms.size());
    int infoW = MeasureText(info, 18);
    DrawText(info, (int)(sw * 0.5f - infoW * 0.5f), (int)(sh - 32.f), 18, Fade(WHITE, 0.55f));

    // Colour legend
    struct LegendEntry { Color color; const char* name; };
    const LegendEntry legend[] = {
        { Color{ 70, 210,  80, 255}, "Start"    },
        { Color{210,  50,  50, 255}, "Boss"     },
        { Color{160,  80, 220, 255}, "Key Room" },
        { Color{220, 130,  40, 255}, "Elite"    },
        { Color{ 55, 160,  80, 255}, "Rest"     },
        { Color{200, 180,  40, 255}, "Treasure" },
        { Color{ 55, 140, 200, 255}, "Shop"     },
        { Color{ 75,  75,  90, 255}, "Standard" },
    };
    float lx = 18.f;
    float ly = sh - 60.f;
    for (const auto& e : legend)
    {
        DrawRectangleRounded({ lx, ly, 16.f, 16.f }, 0.3f, 4, e.color);
        DrawText(e.name, (int)(lx + 22.f), (int)(ly + 1.f), 14, Fade(WHITE, 0.65f));
        lx += MeasureText(e.name, 14) + 44.f;
    }
}

void Engine::UpdateHitboxEditor()
{
    if (IsKeyPressed(KEY_ESCAPE))
    {
        _isHitboxEditorActive = false;
        _hitboxSelectedEntity = nullptr;
        return;
    }

    // Click to select an entity
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        Vector2 camRef = Vector2Subtract(_cameraPos, _shakeOffset);
        float sw = (float)kVirtualWidth, sh = (float)kVirtualHeight;
        auto toScreen = [&](Rectangle r) {
            return Rectangle{ r.x - camRef.x + sw / 2.f, r.y - camRef.y + sh / 2.f, r.width, r.height };
        };
    Vector2 mouse = GetVirtualMousePos();
        BaseCharacter* hit = nullptr;

        // Helper: check if mouse is inside a capsule (screen space)
        auto mouseInCapsule = [&](const Capsule2D& cap) {
            Vector2 sc = { cap.center.x - camRef.x + sw*0.5f, cap.center.y - camRef.y + sh*0.5f };
            float syTop = sc.y - cap.halfHeight, syBot = sc.y + cap.halfHeight;
            Vector2 closest = { sc.x, std::max(syTop, std::min(mouse.y, syBot)) };
            return Vector2Distance(mouse, closest) < cap.radius;
        };

        if (mouseInCapsule(_player.GetCapsule()))
            hit = &_player;

        if (!hit)
        {
            for (auto& e : _enemies)
            {
                if (!e->IsActive()) continue;
                if (mouseInCapsule(e->GetCapsule()))
                {
                    hit = e.get();
                    break;
                }
            }
        }

        if (hit != _hitboxSelectedEntity)
            _hitboxEditAttack = false;
        _hitboxSelectedEntity = hit;
    }

    if (!_hitboxSelectedEntity) return;
    _hitboxSelectedEntity->EnsureCollisionShape();

    // TAB: toggle body / attack box (player and enemies)
    if (IsKeyPressed(KEY_TAB))
        _hitboxEditAttack = !_hitboxEditAttack;

    // S: export values to VS output window
    if (IsKeyPressed(KEY_S))
    {
        const char* typeName = "Unknown";
        if (_hitboxSelectedEntity == &_player)
            typeName = "Player";
        else if (Enemy* e = dynamic_cast<Enemy*>(_hitboxSelectedEntity))
        {
            if      (e->AsCyclops())    typeName = "Cyclops";
            else if (e->AsOgre())       typeName = "Ogre";
            else if (e->AsMolarbeast()) typeName = "Molarbeast";
            else                        typeName = "Grunt";
        }

        if (!_hitboxEditAttack)
        {
            Capsule2D cap = _hitboxSelectedEntity->GetCapsule();
            Vector2   off = _hitboxSelectedEntity->GetCapsuleOffset();
            TraceLog(LOG_WARNING, "=== CAPSULE EXPORT [%s] ===", typeName);
            TraceLog(LOG_WARNING, "s->_capsuleRadius     = %.2ff;", cap.radius);
            TraceLog(LOG_WARNING, "s->_capsuleHalfHeight = %.2ff;", cap.halfHeight);
            TraceLog(LOG_WARNING, "s->_capsuleOffset     = { %.2ff, %.2ff };", off.x, off.y);
            TraceLog(LOG_WARNING, "===========================");
        }
        else if (_hitboxSelectedEntity == &_player)
        {
            TraceLog(LOG_WARNING, "=== ATTACK EXPORT [Player] ===");
            TraceLog(LOG_WARNING, "_attackWidthAdjust  = %.2ff;", _player.GetAttackWidthAdjust());
            TraceLog(LOG_WARNING, "_attackHeightAdjust = %.2ff;", _player.GetAttackHeightAdjust());
            TraceLog(LOG_WARNING, "==============================");
        }
        else if (Enemy* e = dynamic_cast<Enemy*>(_hitboxSelectedEntity))
        {
            TraceLog(LOG_WARNING, "=== ATTACK EXPORT [%s] ===", typeName);
            TraceLog(LOG_WARNING, "float _attackBoxWidth   = %.2ff;", e->GetAttackBoxWidth());
            TraceLog(LOG_WARNING, "float _attackBoxHeight  = %.2ff;", e->GetAttackBoxHeight());
            TraceLog(LOG_WARNING, "float _attackBoxOffsetX = %.2ff;", e->GetAttackBoxOffsetX());
            TraceLog(LOG_WARNING, "float _attackBoxOffsetY = %.2ff;", e->GetAttackBoxOffsetY());
            TraceLog(LOG_WARNING, "==========================");
        }
    }

    // Arrow key nudge with hold-repeat
    float dt = GetFrameTime();
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    bool rDown = IsKeyDown(KEY_RIGHT), lDown = IsKeyDown(KEY_LEFT);
    bool uDown = IsKeyDown(KEY_UP),    dDown = IsKeyDown(KEY_DOWN);
    bool rPressed = IsKeyPressed(KEY_RIGHT), lPressed = IsKeyPressed(KEY_LEFT);
    bool uPressed = IsKeyPressed(KEY_UP),    dPressed = IsKeyPressed(KEY_DOWN);
    bool anyDown    = rDown    || lDown    || uDown    || dDown;
    bool anyPressed = rPressed || lPressed || uPressed || dPressed;

    float dx = 0.f, dy = 0.f;

    if (anyPressed)
    {
        if (rPressed) dx += 1.f;
        if (lPressed) dx -= 1.f;
        if (uPressed) dy -= 1.f;
        if (dPressed) dy += 1.f;
        _hitboxNudgeAccum = -kHitboxNudgeInitDelay;
    }
    else if (anyDown)
    {
        _hitboxNudgeAccum += dt;
        if (_hitboxNudgeAccum >= 0.f)
        {
            _hitboxNudgeAccum -= kHitboxNudgeRepeatRate;
            if (rDown) dx += 1.f;
            if (lDown) dx -= 1.f;
            if (uDown) dy -= 1.f;
            if (dDown) dy += 1.f;
        }
    }
    else
    {
        _hitboxNudgeAccum = -kHitboxNudgeInitDelay;
    }

    if (dx == 0.f && dy == 0.f) return;

    if (!_hitboxEditAttack)
    {
        if (!shift)
        {
            // LR = radius (fatter/skinnier), UD = halfHeight (UP=taller, DOWN=shorter)
            if (dx != 0.f)
                _hitboxSelectedEntity->SetCapsuleRadius(_hitboxSelectedEntity->GetCapsuleRadius() + dx);
            if (dy != 0.f)
                _hitboxSelectedEntity->SetCapsuleHalfHeight(_hitboxSelectedEntity->GetCapsuleHalfHeight() - dy);
        }
        else
        {
            // Shift+arrows = move capsule offset
            Vector2 off = _hitboxSelectedEntity->GetCapsuleOffset();
            off.x += dx;
            off.y += dy;
            _hitboxSelectedEntity->SetCapsuleOffset(off);
        }
    }
    else if (_hitboxSelectedEntity == &_player)
    {
        _player.SetAttackWidthAdjust(_player.GetAttackWidthAdjust() + dx);
        _player.SetAttackHeightAdjust(_player.GetAttackHeightAdjust() + dy);
    }
    else if (Enemy* e = dynamic_cast<Enemy*>(_hitboxSelectedEntity))
    {
        if (!shift)
        {
            e->SetAttackBoxOffsetX(e->GetAttackBoxOffsetX() + dx);
            e->SetAttackBoxOffsetY(e->GetAttackBoxOffsetY() + dy);
        }
        else
        {
            e->SetAttackBoxWidth(std::max(4.f,  e->GetAttackBoxWidth()  + dx));
            e->SetAttackBoxHeight(std::max(4.f, e->GetAttackBoxHeight() + dy));
        }
    }
}

void Engine::DrawHitboxEditor()
{
    Vector2 camRef = Vector2Subtract(_cameraPos, _shakeOffset);
    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;

    auto toScreen = [&](Rectangle r) {
        return Rectangle{ r.x - camRef.x + sw / 2.f, r.y - camRef.y + sh / 2.f, r.width, r.height };
    };

    auto drawEntity = [&](BaseCharacter* entity)
    {
        bool isSelected = (entity == _hitboxSelectedEntity);

        // Draw capsule body outline
        Capsule2D cap = entity->GetCapsule();
        Vector2 capScr = { cap.center.x - camRef.x + sw*0.5f, cap.center.y - camRef.y + sh*0.5f };
        Color bodyCol = (isSelected && !_hitboxEditAttack) ? GREEN : RED;
        DrawCapsule2DLines(capScr, cap.halfHeight, cap.radius, bodyCol);

        // Draw centre dot when selected
        if (isSelected && !_hitboxEditAttack)
            DrawCircle((int)capScr.x, (int)capScr.y, 5.f, BLUE);

        if (isSelected)
        {
            Rectangle atkScr = {};
            if (entity == &_player)
                atkScr = toScreen(_player.GetAttackCollisionRec());
            else if (Enemy* e = dynamic_cast<Enemy*>(entity))
                atkScr = toScreen(e->GetAttackCollisionRec());

            Color atkCol   = _hitboxEditAttack ? GREEN : YELLOW;
            float atkThick = _hitboxEditAttack ? 3.f   : 1.5f;
            DrawRectangleLinesEx(atkScr, atkThick, atkCol);

            if (_hitboxEditAttack)
            {
                const float hs = 7.f;
                DrawRectangle((int)(atkScr.x - hs/2.f),                (int)(atkScr.y - hs/2.f),                (int)hs, (int)hs, ORANGE);
                DrawRectangle((int)(atkScr.x + atkScr.width - hs/2.f), (int)(atkScr.y - hs/2.f),                (int)hs, (int)hs, ORANGE);
                DrawRectangle((int)(atkScr.x - hs/2.f),                (int)(atkScr.y + atkScr.height - hs/2.f),(int)hs, (int)hs, ORANGE);
                DrawRectangle((int)(atkScr.x + atkScr.width - hs/2.f), (int)(atkScr.y + atkScr.height - hs/2.f),(int)hs, (int)hs, ORANGE);
            }
        }
    };

    drawEntity(&_player);
    for (auto& e : _enemies)
        if (e->IsActive()) drawEntity(e.get());

    // Top status bar
    DrawRectangle(0, 0, (int)sw, 30, Fade(BLACK, 0.80f));
    const char* modeStr;
    if (!_hitboxSelectedEntity)
        modeStr = "[CAPSULE EDITOR]  Click entity to select  |  ESC: exit";
    else if (_hitboxEditAttack)
        modeStr = "[CAPSULE EDITOR]  ATTACK BOX  |  LR: width   UD: height  |  Shift+LR/UD: offset  |  TAB: capsule  |  S: export  |  ESC: exit";
    else
        modeStr = "[CAPSULE EDITOR]  CAPSULE  |  LR: radius   UD: halfHeight  |  Shift+Arrows: offset  |  TAB: attack  |  S: export  |  ESC: exit";
    DrawText(modeStr, 8, 5, 18, YELLOW);

    // Bottom values bar
    if (_hitboxSelectedEntity)
    {
        DrawRectangle(0, (int)(sh - 30.f), (int)sw, 30, Fade(BLACK, 0.80f));
        const char* info = "";
        if (!_hitboxEditAttack)
        {
            Capsule2D cap = _hitboxSelectedEntity->GetCapsule();
            Vector2   off = _hitboxSelectedEntity->GetCapsuleOffset();
            info = TextFormat("  Capsule  Radius: %.1f   HalfHeight: %.1f   Offset: (%.1f, %.1f)   [S to export]",
                              cap.radius, cap.halfHeight, off.x, off.y);
        }
        else if (_hitboxSelectedEntity == &_player)
        {
            info = TextFormat("  Attack  Width adj: %.1f   Height adj: %.1f   [S to export]",
                              _player.GetAttackWidthAdjust(), _player.GetAttackHeightAdjust());
        }
        else if (Enemy* e = dynamic_cast<Enemy*>(_hitboxSelectedEntity))
        {
            info = TextFormat("  Attack Box  Offset: (%.1f, %.1f)   Size: (%.1f, %.1f)   [S to export]",
                              e->GetAttackBoxOffsetX(), e->GetAttackBoxOffsetY(),
                              e->GetAttackBoxWidth(), e->GetAttackBoxHeight());
        }
        DrawText(info, 8, (int)(sh - 24.f), 18, WHITE);
    }
}

void Engine::DrawDebugToggleTab()
{
    if (!_debug.IsActive())
        return;

    const Rectangle debugTab = GetDebugToggleTabRect();
    DrawRectangleRounded(debugTab, 0.35f, 6, Fade(Color{ 92, 58, 26, 255 }, 0.78f));
    DrawRectangleRoundedLines(debugTab, 0.35f, 6, Fade(Color{ 255, 214, 150, 255 }, 0.66f));

    const char* tabLabel = _debug.IsOpen() ? "DBG <" : "DBG >";
    const int tabSz = 18;
    const int tabW = MeasureText(tabLabel, tabSz);
    DrawText(tabLabel,
        (int)(debugTab.x + debugTab.width * 0.5f - tabW * 0.5f),
        (int)(debugTab.y + debugTab.height * 0.5f - tabSz * 0.5f),
        tabSz, RAYWHITE);
}

// Debug-panel-only overlay: facing arrow, front defence cone, rear vulnerability
// cone, and the facing-lock countdown for every live enemy. Dungeon rooms fill
// the screen, so world position == screen position here.
void Engine::DrawEnemyFacingDebug() const
{
    // Cone edges match the gameplay checks in Balance::Facing (cos-space).
    const float frontHalfAngle = acosf(Balance::Facing::kFrontConeDot);
    const float rearHalfAngle  = acosf(Balance::Facing::kRearConeDot);
    const float coneLen  = 90.f;
    const float arrowLen = 60.f;

    auto drawCone = [](Vector2 center, float baseAngle, float halfAngle, float len, Color color)
    {
        Vector2 edgeA{ center.x + cosf(baseAngle - halfAngle) * len,
                       center.y + sinf(baseAngle - halfAngle) * len };
        Vector2 edgeB{ center.x + cosf(baseAngle + halfAngle) * len,
                       center.y + sinf(baseAngle + halfAngle) * len };
        DrawLineEx(center, edgeA, 2.f, color);
        DrawLineEx(center, edgeB, 2.f, color);
    };

    for (const auto& enemy : _enemies)
    {
        if (!enemy->IsActive() || !enemy->IsAlive())
            continue;

        Vector2 center = enemy->GetWorldPos();
        float sign = enemy->GetFacingSign();
        float facingAngle = (sign >= 0.f) ? 0.f : PI;

        // Facing arrow (yellow), front defence cone (orange), rear cone (green).
        Vector2 tip{ center.x + sign * arrowLen, center.y };
        DrawLineEx(center, tip, 3.f, YELLOW);
        DrawLineEx(tip, { tip.x - sign * 12.f, tip.y - 8.f }, 3.f, YELLOW);
        DrawLineEx(tip, { tip.x - sign * 12.f, tip.y + 8.f }, 3.f, YELLOW);
        drawCone(center, facingAngle, frontHalfAngle, coneLen, Fade(ORANGE, 0.8f));
        drawCone(center, facingAngle + PI, rearHalfAngle, coneLen, Fade(GREEN, 0.8f));

        float lockRemaining = enemy->GetFacingLockRemaining();
        if (lockRemaining > 0.f)
            DrawText(TextFormat("lock %.2f", lockRemaining),
                     (int)(center.x - 30.f), (int)(center.y - 70.f), 16, ORANGE);
    }

    // Room pressure readout (Balance::Pressure): spent/cap, pending waves,
    // live hazards. Zeroed in non-combat rooms.
    DrawText(TextFormat("PRESSURE %d / %d   waves %d   telegraphing %d   hazards %d   shots %d/%d",
                        _roomPressureSpent, _roomPressureCapDbg,
                        (int)_dungeonReinforcements.size(), (int)_pendingEnemySpawns.size(),
                        _roomHazards.ActiveCount(),
                        (int)_enemyProjectiles.size(),
                        Balance::Pressure::kEnemyProjectileCap[std::clamp(_roomEncounterTier, 0, 2)]),
             20, 150, 20, ORANGE);
}

void Engine::DrawRoomTelemetry() const
{
    // Balance telemetry readout — one line per cleared room, newest first.
    // Rendered beside the debug panel so playtest tuning reads real numbers.
    const int shown = std::min(_roomTelemetryCount, kTelemetryHistory);
    if (shown == 0) return;

    const int   lineH = 20;
    const int   fs    = 16;
    const float x     = 12.f;
    float       y     = (float)kVirtualHeight - 40.f - (shown + 1) * lineH;

    DrawRectangle((int)x - 6, (int)y - 6, 560, (shown + 1) * lineH + 12, Fade(BLACK, 0.62f));
    DrawText("ROOM TELEMETRY  (clear s | dmg | heal | drops | xp | gold | lvl | pwr)",
             (int)x, (int)y, fs, Color{ 255, 214, 150, 255 });
    y += lineH;

    for (int i = 0; i < shown; i++)
    {
        // Newest record first: walk the ring backwards from the last write.
        int idx = (_roomTelemetryCount - 1 - i) % kTelemetryHistory;
        const RoomTelemetry& rec = _roomTelemetry[idx];
        DrawText(TextFormat("A%d-R%d %-8s %5.1fs  dmg %2d  heal %2d  drops %d  xp %2d  gold %+3d  L%d  P%d",
                            rec.act, rec.room, rec.roomType, rec.clearSeconds,
                            rec.dmgTaken, rec.healed, rec.healDrops,
                            rec.xpGained, rec.goldDelta, rec.playerLevel, rec.powerLevel),
                 (int)x, (int)y, fs, (i == 0) ? RAYWHITE : Fade(RAYWHITE, 0.7f));
        y += lineH;
    }
}

// Sets player touch direction/attack/dash from _touch, then scans for new
// touches on the ability arc.  Called each gameplay frame when touch mode is on.
void Engine::UpdateTouchControls()
{
    const int screenW = kVirtualWidth;
    const int screenH = kVirtualHeight;

    // Sync touch layout from HUD config so editor changes take effect immediately
    _touch.kJoyRadius     = _hudCfg.touchJoyR;
    _touch.kBtnRadius     = _hudCfg.touchAtkR;
    _touch.kDashBtnRadius = _hudCfg.touchDashR;
    _touch.kBtnRightPad   = _hudCfg.touchAtkPadR;
    _touch.kBtnBotPad     = _hudCfg.touchAtkPadB;
    _touch.kDashBtnOffset = _hudCfg.touchDashOffset;
    _touch.kAtkLabelFs    = _hudCfg.touchAtkFs;
    _touch.kDashLabelFs   = _hudCfg.touchDashFs;
    _touch.kDashBotPad    = _hudCfg.touchDashBotPad;

    // -- Ability slot drag (only when HUD editor is open) ---------------------
    if (_hudEditorActive)
    {
        const Vector2 mousePos = GetVirtualMousePos();
        const int ts = _player.GetMaxAbilitySlots();

        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && _touchSlotDragIdx < 0)
        {
            for (int s = 0; s < ts; s++)
            {
                Rectangle r = TouchAbilityRect(s, screenW, screenH,
                    _hudCfg.touchAtkPadB, _hudCfg.touchAtkR,
                    _hudCfg.touchSlotSz, _hudCfg.touchSlotGap,
                    _hudCfg.touchSlotRightPad, _hudCfg.touchSlotYOff,
                    _touchSlotOffset[s]);
                if (CheckCollisionPointRec(mousePos, r))
                {
                    _touchSlotDragIdx         = s;
                    _touchSlotDragMouseStart  = mousePos;
                    _touchSlotDragOffsetStart = _touchSlotOffset[s];
                    break;
                }
            }
        }

        if (_touchSlotDragIdx >= 0)
        {
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
            {
                Vector2 delta = Vector2Subtract(mousePos, _touchSlotDragMouseStart);
                _touchSlotOffset[_touchSlotDragIdx] = Vector2Add(_touchSlotDragOffsetStart, delta);
            }
            else
            {
                _touchSlotDragIdx = -1;
            }
            _player.SetTouchDirection(Vector2Zero());
            return;
        }
    }

    // -- Zeph tap priority -----------------------------------------------------
    // If a new press lands near Zeph's sprite, skip _touch.Update() so the
    // joystick never activates. UpdateNpc() (called later in UpdateGamePlay)
    // will handle the tap independently.
    if (_currentRoomType == RoomType::Store && _shop.IsNearNpc())
    {
        const float sw2 = screenW * 0.5f;
        const float sh2 = screenH * 0.5f;
        const Vector2 worldOff = { -_cameraPos.x + _shakeOffset.x,
                                   -_cameraPos.y + _shakeOffset.y };
        const Vector2 npc = _shop.GetNpcPos();
        const Vector2 npcScreen = { npc.x + worldOff.x + sw2,
                                    npc.y + worldOff.y + sh2 };
        const Rectangle btnRect = _shop.GetNpcTouchBtnRect(npcScreen.x, npcScreen.y);

        bool zephHit = false;
        const int tc0 = GetTouchPointCount();
        if (tc0 == 0)
        {
            zephHit = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
                      CheckCollisionPointRec(GetVirtualMousePos(), btnRect);
        }
        else
        {
            for (int i = 0; i < tc0; i++)
            {
                int id = GetTouchPointId(i);
                if (id == _touch.GetJoyTouchId() ||
                    id == _touch.GetAtkTouchId()  ||
                    id == _touch.GetDashTouchId()) continue;
                if (CheckCollisionPointRec(GetVirtualTouchPos(i), btnRect))
                { zephHit = true; break; }
            }
        }
        if (zephHit) return;
    }

    _touch.Update(screenW, screenH);

    _player.SetTouchDirection(_touch.joystickDir);
    if (_touch.attackPressed) _player.SetTouchAttack();
    if (_touch.dashPressed)   _player.SetTouchDash();

    ScanAbilityArcTaps();

    // Pause button (top-right corner) - same rect as DrawHUD draws it
    const Rectangle pauseRec{ (float)screenW - _hudCfg.touchPauseW - _hudCfg.touchPausePad,
                               _hudCfg.touchPausePad, _hudCfg.touchPauseW, _hudCfg.touchPauseH };

    const int tc = GetTouchPointCount();
    if (tc == 0)
    {
        // Mouse simulation
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            CheckCollisionPointRec(GetVirtualMousePos(), pauseRec))
        {
            _gameState = GameState::Pause;
        }
    }
    else
    {
        // Real touch - check any new touch ID not already owned by other controls
        int joyId  = _touch.GetJoyTouchId();
        int atkId  = _touch.GetAtkTouchId();
        int dashId = _touch.GetDashTouchId();
        for (int i = 0; i < tc; i++)
        {
            int id = GetTouchPointId(i);
            if (id == joyId || id == atkId || id == dashId) continue;
            bool seen = false;
            for (int sid : _abilityTapSeenIds) if (sid == id) { seen = true; break; }
            if (!seen && CheckCollisionPointRec(GetVirtualTouchPos(i), pauseRec))
            {
                _gameState = GameState::Pause;
                return;
            }
        }
    }
}

// Detects new touches on ability arc buttons and triggers the matching cast.
// Tracks seen touch IDs to avoid re-triggering on hold.
void Engine::ScanAbilityArcTaps()
{
    if (_waveStarting || _ultimatePhase != UltimatePhase::None)
        return;

    const int tc         = GetTouchPointCount();
    const int totalSlots = _player.GetMaxAbilitySlots();
    const int screenW    = kVirtualWidth;
    const int screenH    = kVirtualHeight;

    auto hitSlot = [&](Vector2 pos) -> int
    {
        for (int s = 0; s < totalSlots; s++)
        {
            Rectangle r = TouchAbilityRect(s, screenW, screenH,
                _touch.kBtnBotPad, _touch.kBtnRadius,
                _hudCfg.touchSlotSz, _hudCfg.touchSlotGap,
                _hudCfg.touchSlotRightPad, _hudCfg.touchSlotYOff,
                _touchSlotOffset[s]);

            const float pad = _hudCfg.touchSlotSz * 0.18f;
            r.x -= pad;
            r.y -= pad;
            r.width += pad * 2.f;
            r.height += pad * 2.f;

            if (CheckCollisionPointRec(pos, r)) return s;
        }
        return -1;
    };

    // -- Mouse simulation path -------------------------------------------------
    if (tc == 0)
    {
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        {
            int slot = hitSlot(GetVirtualMousePos());
            if (slot >= 0) BeginAbilityInput(slot);
        }
        return;
    }

    // -- Real touch path -------------------------------------------------------
    // Remove IDs that have lifted
    _abilityTapSeenIds.erase(
        std::remove_if(_abilityTapSeenIds.begin(), _abilityTapSeenIds.end(),
            [tc](int id)
            {
                for (int i = 0; i < tc; i++)
                    if (GetTouchPointId(i) == id) return false;
                return true;
            }),
        _abilityTapSeenIds.end());

    int joyId  = _touch.GetJoyTouchId();
    int atkId  = _touch.GetAtkTouchId();
    int dashId = _touch.GetDashTouchId();

    for (int i = 0; i < tc; i++)
    {
        int id = GetTouchPointId(i);
        if (id == joyId || id == atkId || id == dashId) continue;

        bool seen = false;
        for (int sid : _abilityTapSeenIds) if (sid == id) { seen = true; break; }
        if (seen) continue;

        int slot = hitSlot(GetVirtualTouchPos(i));
        if (slot >= 0)
        {
            _abilityTapSeenIds.push_back(id);
            BeginAbilityInput(slot);
        }
    }
}

// Draws touch-mode ability slots as square icons - same visual language as the
// desktop bar, just repositioned and slightly larger for thumb tapping.
void Engine::DrawTouchAbilityArc()
{
    const int totalSlots = _player.GetMaxAbilitySlots();
    const int screenW    = kVirtualWidth;
    const int screenH    = kVirtualHeight;

    if (totalSlots > 0)
    {
        Rectangle firstSlot = TouchAbilityRect(0, screenW, screenH,
            _touch.kBtnBotPad, _touch.kBtnRadius,
            _hudCfg.touchSlotSz, _hudCfg.touchSlotGap,
            _hudCfg.touchSlotRightPad, _hudCfg.touchSlotYOff,
            _touchSlotOffset[0]);
        Rectangle lastSlot = TouchAbilityRect(totalSlots - 1, screenW, screenH,
            _touch.kBtnBotPad, _touch.kBtnRadius,
            _hudCfg.touchSlotSz, _hudCfg.touchSlotGap,
            _hudCfg.touchSlotRightPad, _hudCfg.touchSlotYOff,
            _touchSlotOffset[totalSlots - 1]);

        float expBarX = std::min(firstSlot.x, lastSlot.x);
        float expBarRight = std::max(firstSlot.x + firstSlot.width, lastSlot.x + lastSlot.width);
        float expBarW = expBarRight - expBarX;
        float expBarY = std::min(firstSlot.y, lastSlot.y) - _hudCfg.expBarGap - _hudCfg.expBarH;

        int curExp = _player.GetExp();
        int maxExp = _player.GetExpToNext();
        float expPct = (maxExp > 0) ? std::min((float)curExp / (float)maxExp, 1.f) : 0.f;
        static const Color kExpFill = { 160, 60, 255, 220 };

        DrawRectangleRounded({ expBarX, expBarY, expBarW, _hudCfg.expBarH }, 0.3f, 4, Fade(BLACK, 0.65f));
        DrawRectangleRounded({ expBarX, expBarY, expBarW * expPct, _hudCfg.expBarH }, 0.3f, 4, kExpFill);
        DrawRectangleRoundedLines({ expBarX, expBarY, expBarW, _hudCfg.expBarH }, 0.3f, 4, Fade(WHITE, 0.22f));

        int eFs = (int)_hudCfg.expLabelFs;
        const char* expLabel = (_player.GetLevel() >= _player.GetMaxLevel())
            ? TextFormat("LVL MAX")
            : TextFormat("LVL %d   EXP  %d / %d", _player.GetLevel(), curExp, maxExp);
        int eLW = MeasureText(expLabel, eFs);
        DrawText(expLabel,
            (int)(expBarX + expBarW * 0.5f - eLW * 0.5f),
            (int)(expBarY + _hudCfg.expBarH * 0.5f - eFs * 0.5f),
            eFs, WHITE);

        if (_upgradeDefenseTex.id != 0)
        {
            const int armour = _player.GetArmour();
            const int maxArmour = _player.GetMaxArmour();
            const float armourIconH = std::max(1.f, _hudCfg.armourSize);
            const float scale = armourIconH / (float)_upgradeDefenseTex.height;
            const float iconW = _upgradeDefenseTex.width * scale;
            const float iconGap = armourIconH * 0.35f;
            float iconX = _hudCfg.armourX;
            const float iconY = _hudCfg.armourY;

            for (int i = 0; i < maxArmour; i++)
            {
                Color tint = (i < armour) ? WHITE : Fade(WHITE, 0.38f);
                DrawTextureEx(_upgradeDefenseTex, { iconX, iconY }, 0.f, scale, tint);
                iconX += iconW + iconGap;
            }
        }
    }
    for (int slot = 0; slot < totalSlots; slot++)
    {
        AbilityType ability = _player.GetLearnedAbility(slot);
        Rectangle   rec     = TouchAbilityRect(slot, screenW, screenH,
                                  _touch.kBtnBotPad, _touch.kBtnRadius,
                                  _hudCfg.touchSlotSz, _hudCfg.touchSlotGap,
                                  _hudCfg.touchSlotRightPad, _hudCfg.touchSlotYOff,
                                  _touchSlotOffset[slot]);
        bool isEmpty = (ability == AbilityType::None);
        bool onCooldown = !isEmpty && _player.IsSlotOnCooldown(slot);
        bool canCast = !isEmpty && !onCooldown && _player.CanCastAbility(ability);
        bool isDragged = (_hudEditorActive && slot == _touchSlotDragIdx);

        Color bgColor     = isDragged ? Fade(DARKBLUE, 0.70f)
                          : isEmpty   ? Fade(BLACK, 0.30f) : Fade(BLACK, 0.55f);
        Color borderColor = isDragged ? Fade(YELLOW, 0.95f)
                          : isEmpty   ? Fade(WHITE, 0.12f)
                          : canCast   ? Fade(LIGHTGRAY, 0.35f) : Fade(RED, 0.40f);

        DrawRectangleRounded(rec, 0.18f, 6, bgColor);
        DrawRectangleRoundedLines(rec, 0.18f, 6, borderColor);

        if (isEmpty)
        {
            // Show slot number dimly, same as desktop bar
            DrawText(PromptAbilitySlot(GetPromptModeForUi(), slot),
                (int)(rec.x + 8.f), (int)(rec.y + 8.f), 18, Fade(WHITE, 0.25f));
            continue;
        }

        // Slot number in top-left corner
        DrawText(PromptAbilitySlot(GetPromptModeForUi(), slot),
            (int)(rec.x + 8.f), (int)(rec.y + 8.f), 18, Fade(WHITE, 0.6f));

        // Element icon centred in the upper portion of the slot
        const Texture2D* iconTex = GetAbilityIcon(ability);
        if (!iconTex || iconTex->id == 0) iconTex = &_abilityIconFireTex;

        Color iconTint  = canCast ? WHITE : Fade(WHITE, 0.35f);
        float maxIconSz = rec.width * 0.55f;
        float iconScale = std::min(maxIconSz / (float)iconTex->width,
                                   maxIconSz / (float)iconTex->height);
        float iw = iconTex->width  * iconScale;
        float ih = iconTex->height * iconScale;
        float cx = rec.x + rec.width  * 0.5f;
        float cy = rec.y + rec.height * 0.42f;
        DrawTextureEx(*iconTex, { cx - iw * 0.5f, cy - ih * 0.5f }, 0.f, iconScale, iconTint);

        // Cooldown sweep + seconds remaining, matching the desktop bar.
        if (onCooldown)
        {
            float fraction = _player.GetSlotCooldownFraction(slot);
            DrawRectangleRec({ rec.x, rec.y, rec.width, rec.height * fraction }, Fade(BLACK, 0.62f));
            const char* cdText = TextFormat("%.1f", _player.GetSlotCooldownRemaining(slot));
            int cdFs = 26;
            DrawText(cdText,
                (int)(cx - MeasureText(cdText, cdFs) * 0.5f),
                (int)(rec.y + rec.height * 0.42f - cdFs * 0.5f),
                cdFs, RAYWHITE);
        }

        // Ability name at the bottom, matching desktop bar
        const char* abilityName = GetAbilityName(ability);
        int nameW = MeasureText(abilityName, 16);
        DrawText(abilityName,
            (int)(rec.x + rec.width / 2.f - nameW / 2.f),
            (int)(rec.y + rec.height - 22.f),
            16, canCast ? RAYWHITE : Fade(GRAY, 0.6f));

        // Level badge
        int abilityLv = _player.GetAbilityLevel(ability);
        if (abilityLv > 1)
        {
            const char* badge = TextFormat("Lv%d", abilityLv);
            int badgeW = MeasureText(badge, 16);
            Color badgeColor = (abilityLv >= 3) ? GOLD : Fade(SKYBLUE, 0.9f);
            DrawText(badge,
                (int)(rec.x + rec.width - badgeW - 5.f),
                (int)(rec.y + rec.height - 20.f),
                16, badgeColor);
        }
    }
}

// -----------------------------------------------------------------------------
// Touch Button Mapping Screen
// -----------------------------------------------------------------------------

float Engine::GetTouchMappingRadius(int idx) const
{
    if (idx == 0) return _hudCfg.touchAtkR;
    if (idx == 1) return _hudCfg.touchDashR;
    return _hudCfg.touchSlotSz * 0.58f;
}

void Engine::EnterTouchButtonMapping()
{
    _touchMappingDragIdx = -1;
    const int sw = kVirtualWidth;
    const int sh = kVirtualHeight;

    if (_touchLayoutCustom)
    {
        for (int i = 0; i < 6; i++) _touchMappingPos[i] = _touchCustomPos[i];
    }
    else
    {
        // Derive positions from current HUDConfig (only valid before any Save has dirtied it)
        _touchMappingPos[0] = { (float)sw - _hudCfg.touchAtkPadR,
                                 (float)sh - _hudCfg.touchAtkPadB };
        const float dashBotY = (_hudCfg.touchDashBotPad >= 0.f)
                               ? _hudCfg.touchDashBotPad : _hudCfg.touchAtkPadB;
        _touchMappingPos[1] = { (float)sw - _hudCfg.touchAtkPadR - _hudCfg.touchDashOffset,
                                 (float)sh - dashBotY };
        for (int s = 0; s < 4; s++)
        {
            Rectangle r = TouchAbilityRect(s, sw, sh,
                _hudCfg.touchAtkPadB, _hudCfg.touchAtkR,
                _hudCfg.touchSlotSz, _hudCfg.touchSlotGap,
                _hudCfg.touchSlotRightPad, _hudCfg.touchSlotYOff,
                _touchSlotOffset[s]);
            _touchMappingPos[2+s] = { r.x + r.width * 0.5f, r.y + r.height * 0.5f };
        }

        // Capture the baked-in defaults exactly once - before any Save can dirty _hudCfg
        if (!_touchDefaults.captured)
        {
            _touchDefaults.atkPadR    = _hudCfg.touchAtkPadR;
            _touchDefaults.atkPadB    = _hudCfg.touchAtkPadB;
            _touchDefaults.dashOffset = _hudCfg.touchDashOffset;
            _touchDefaults.dashBotPad = _hudCfg.touchDashBotPad;
            for (int s = 0; s < 4; s++) _touchDefaults.slotOffset[s] = _touchSlotOffset[s];
            for (int i = 0; i < 6; i++) _touchDefaults.pos[i] = _touchMappingPos[i];
            _touchDefaults.captured = true;
        }
    }
}

void Engine::ApplyTouchCustomLayout()
{
    const int sw = kVirtualWidth;
    const int sh = kVirtualHeight;

    _hudCfg.touchAtkPadR    = (float)sw - _touchCustomPos[0].x;
    _hudCfg.touchAtkPadB    = (float)sh - _touchCustomPos[0].y;
    _hudCfg.touchDashBotPad = (float)sh - _touchCustomPos[1].y;
    _hudCfg.touchDashOffset  = _touchCustomPos[0].x - _touchCustomPos[1].x;

    for (int s = 0; s < 4; s++)
    {
        Rectangle rDef = TouchAbilityRect(s, sw, sh,
            _hudCfg.touchAtkPadB, _hudCfg.touchAtkR,
            _hudCfg.touchSlotSz, _hudCfg.touchSlotGap,
            _hudCfg.touchSlotRightPad, _hudCfg.touchSlotYOff,
            {0.f, 0.f});
        Vector2 defCenter = { rDef.x + rDef.width * 0.5f, rDef.y + rDef.height * 0.5f };
        _touchSlotOffset[s] = { _touchCustomPos[2+s].x - defCenter.x,
                                 _touchCustomPos[2+s].y - defCenter.y };
    }
}

int Engine::DrawTouchButtonMapping()
{
    const int sw = kVirtualWidth;
    const int sh = kVirtualHeight;

    ClearBackground(Color{ 8, 6, 10, 255 });
    float scaleX = sw / (RoomLayout::kCols * 16.f);
    float scaleY = sh / (RoomLayout::kRows * 16.f);
    if (_tileRenderer.IsLoaded())
        _tileRenderer.DrawRoom(_dungeonRoomLayout, scaleX, scaleY, { 0.f, 0.f });
    DrawHUD();
    DrawRectangle(0, 0, sw, sh, Fade(BLACK, 0.54f));

    const Vector2 inputPos  = GetVirtualMousePos();
    const bool    inputDown = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    const bool    inputPress= IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    auto hitButton = [&](Vector2 pos) -> int
    {
        for (int i = 0; i < 2; i++)
            if (Vector2Distance(pos, _touchMappingPos[i]) <= GetTouchMappingRadius(i)) return i;
        for (int i = 2; i < 6; i++)
        {
            float half = _hudCfg.touchSlotSz * 0.5f;
            Rectangle r{ _touchMappingPos[i].x - half, _touchMappingPos[i].y - half,
                         _hudCfg.touchSlotSz, _hudCfg.touchSlotSz };
            if (CheckCollisionPointRec(pos, r)) return i;
        }
        return -1;
    };

    if (inputPress && _touchMappingDragIdx < 0)
    {
        int hit = hitButton(inputPos);
        if (hit >= 0)
        {
            _touchMappingDragIdx   = hit;
            _touchMappingDragStart = inputPos;
            _touchMappingPosAtDrag = _touchMappingPos[hit];
        }
    }

    if (_touchMappingDragIdx >= 0)
    {
        if (inputDown)
        {
            Vector2 delta  = Vector2Subtract(inputPos, _touchMappingDragStart);
            Vector2 newPos = Vector2Add(_touchMappingPosAtDrag, delta);

            for (int i = 0; i < 6; i++)
            {
                if (i == _touchMappingDragIdx) continue;
                float minD = GetTouchMappingRadius(_touchMappingDragIdx) + GetTouchMappingRadius(i);
                Vector2 diff = Vector2Subtract(newPos, _touchMappingPos[i]);
                float dist   = Vector2Length(diff);
                if (dist < minD && dist > 0.001f)
                    newPos = Vector2Add(_touchMappingPos[i],
                                        Vector2Scale(Vector2Normalize(diff), minD));
            }

            float r = GetTouchMappingRadius(_touchMappingDragIdx);
            newPos.x = Clamp(newPos.x, r, (float)sw - r);
            newPos.y = Clamp(newPos.y, r, (float)sh - r);
            _touchMappingPos[_touchMappingDragIdx] = newPos;
        }
        else
        {
            _touchMappingDragIdx = -1;
        }
    }

    // -- ATK button -----------------------------------------------------------
    {
        bool drag = (_touchMappingDragIdx == 0);
        DrawCircleV(_touchMappingPos[0], _hudCfg.touchAtkR,
                    drag ? Fade(YELLOW, 0.65f) : Fade(RED, 0.58f));
        DrawCircleLinesV(_touchMappingPos[0], _hudCfg.touchAtkR,
                         drag ? YELLOW : Fade(WHITE, 0.75f));
        const char* lbl = "ATTACK";
        int fs = (int)_hudCfg.touchAtkFs;
        DrawText(lbl,
            (int)(_touchMappingPos[0].x - MeasureText(lbl, fs) * 0.5f),
            (int)(_touchMappingPos[0].y - fs * 0.5f), fs, RAYWHITE);
    }

    // -- DASH button ----------------------------------------------------------
    {
        bool drag = (_touchMappingDragIdx == 1);
        DrawCircleV(_touchMappingPos[1], _hudCfg.touchDashR,
                    drag ? Fade(YELLOW, 0.65f) : Fade(SKYBLUE, 0.58f));
        DrawCircleLinesV(_touchMappingPos[1], _hudCfg.touchDashR,
                         drag ? YELLOW : Fade(WHITE, 0.75f));
        const char* lbl = "DASH";
        int fs = (int)_hudCfg.touchDashFs;
        DrawText(lbl,
            (int)(_touchMappingPos[1].x - MeasureText(lbl, fs) * 0.5f),
            (int)(_touchMappingPos[1].y - fs * 0.5f), fs, RAYWHITE);
    }

    // -- Ability slots ---------------------------------------------------------
    static const Color kSlotColors[4] = {
        {220,120, 60,255}, {60,160,220,255}, {120,200,80,255}, {180,80,200,255}
    };
    for (int s = 0; s < 4; s++)
    {
        int   idx  = 2 + s;
        bool  drag = (_touchMappingDragIdx == idx);
        float half = _hudCfg.touchSlotSz * 0.5f;
        Rectangle r{ _touchMappingPos[idx].x - half,
                     _touchMappingPos[idx].y - half,
                     _hudCfg.touchSlotSz, _hudCfg.touchSlotSz };
        DrawRectangleRounded(r, 0.18f, 6,
            drag ? Fade(YELLOW, 0.55f) : Fade(kSlotColors[s], 0.58f));
        DrawRectangleRoundedLines(r, 0.18f, 6,
            drag ? YELLOW : Fade(WHITE, 0.55f));
        const char* numLabel = TextFormat("%d", s + 1);
        int nfs = 42;
        DrawText(numLabel,
            (int)(r.x + r.width  * 0.5f - MeasureText(numLabel, nfs) * 0.5f),
            (int)(r.y + r.height * 0.5f - nfs * 0.5f),
            nfs, RAYWHITE);
    }

    // -- Center UI buttons -----------------------------------------------------
    const float btnW   = 230.f;
    const float btnH   = 72.f;
    const float bGap   = 18.f;
    const float uiX    = (float)sw * 0.5f - btnW * 0.5f;
    const float totalH = 3.f * btnH + 2.f * bGap;
    const float uiY    = (float)sh * 0.5f - totalH * 0.5f;

    auto drawBtn = [&](const char* label, float y, Color col) -> bool
    {
        Rectangle rec{ uiX, y, btnW, btnH };
        bool hov = CheckCollisionPointRec(inputPos, rec) && _touchMappingDragIdx < 0;
        DrawRectangleRounded(rec, 0.24f, 6, hov ? Fade(col, 0.92f) : Fade(col, 0.72f));
        DrawRectangleRoundedLines(rec, 0.24f, 6, Fade(WHITE, 0.50f));
        int fs = 32;
        DrawText(label,
            (int)(rec.x + rec.width  * 0.5f - MeasureText(label, fs) * 0.5f),
            (int)(rec.y + rec.height * 0.5f - fs * 0.5f),
            fs, WHITE);
        return hov && inputPress;
    };

    int result = 0;
    if (drawBtn("Back",    uiY,                   Color{ 80,  80,  80, 255})) result = 2;
    if (drawBtn("Save",    uiY + btnH + bGap,     Color{ 55, 175,  85, 255})) result = 1;
    if (drawBtn("Default", uiY + (btnH + bGap)*2.f, Color{195, 135,  40, 255})) result = 3;

    const char* hint = "Drag buttons anywhere  |  they won't overlap";
    int hfs = 24;
    DrawText(hint,
        (int)((float)sw * 0.5f - MeasureText(hint, hfs) * 0.5f),
        22, hfs, Fade(WHITE, 0.80f));

    return result;
}

// -- Dialogue Box Designer (F11 in debug mode) ---------------------------------
// Eight drag handles: four panel corners + portrait centre + portrait scale knob.
// Handle IDs:
//   0-3  = panel corners (TL, TR, BL, BR)
//   4    = panel body (drag to move whole panel)
//   5    = portrait centre
//   6    = portrait scale knob (drag right to grow)

static const float kDlgHandleRadius = 8.f;

// Returns true if the mouse just grabbed handle 'id' at position 'pos'.
static bool DlgHandleHit(Vector2 pos, int id, int& activeHandle)
{
    if (activeHandle != -1 && activeHandle != id) return false;
    Vector2 mouse = GetVirtualMousePos();
    bool over = (CheckCollisionPointCircle(mouse, pos, kDlgHandleRadius + 4.f));
    if (over && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) { activeHandle = id; return true; }
    return activeHandle == id;
}

void Engine::UpdateDialogueBoxEditor()
{
    DialogueBox& box = _cutscene.GetLayout();

    if (IsKeyPressed(KEY_ESCAPE))
    {
        _isDlgEditorActive  = false;
        _dlgEditorHandle    = -1;
        _dlgSpeakerFsDrag   = false;
        _dlgBodyFsDrag      = false;
        _dlgInsetLeftDrag   = false;
        _dlgInsetTopDrag    = false;
        return;
    }

    // S - print all current values to the VS output console in paste-ready format.
    if (IsKeyPressed(KEY_S))
    {
        const Rectangle& p = box.panelRect;
        printf("\n===== Dialogue Box Layout (paste to Claude) =====\n");
        printf("panelRect       = { %.1ff, %.1ff, %.1ff, %.1ff };\n",
               p.x, p.y, p.width, p.height);
        printf("speakerFontSize = %d;\n", box.speakerFontSize);
        printf("bodyFontSize    = %d;\n", box.bodyFontSize);
        printf("textInsetLeft   = %.1ff;\n", box.textInsetLeft);
        printf("textInsetTop    = %.1ff;\n", box.textInsetTop);
        printf("srcCorner       = %.1ff;   dstCorner = %.1ff;\n",
               box.srcCorner, box.dstCorner);
        printf("=================================================\n\n");
        fflush(stdout);
    }

    Vector2 mouse = GetVirtualMousePos();
    Vector2 delta = GetMouseDelta();
    Rectangle& p  = box.panelRect;

    // -- Panel handles (release on mouse-up) -----------------------------------
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON))
        _dlgEditorHandle = -1;

    Vector2 cornerTL = { p.x,            p.y             };
    Vector2 cornerTR = { p.x + p.width,  p.y             };
    Vector2 cornerBL = { p.x,            p.y + p.height  };
    Vector2 cornerBR = { p.x + p.width,  p.y + p.height  };
    Vector2 centre   = { p.x + p.width * 0.5f, p.y + p.height * 0.5f };

    if (DlgHandleHit(cornerTL, 0, _dlgEditorHandle) && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
    { p.x += delta.x; p.width -= delta.x; p.y += delta.y; p.height -= delta.y; }
    if (DlgHandleHit(cornerTR, 1, _dlgEditorHandle) && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
    { p.width += delta.x; p.y += delta.y; p.height -= delta.y; }
    if (DlgHandleHit(cornerBL, 2, _dlgEditorHandle) && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
    { p.x += delta.x; p.width -= delta.x; p.height += delta.y; }
    if (DlgHandleHit(cornerBR, 3, _dlgEditorHandle) && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
    { p.width += delta.x; p.height += delta.y; }
    if (DlgHandleHit(centre, 4, _dlgEditorHandle) && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
    { p.x += delta.x; p.y += delta.y; }

    if (p.width  < 80.f) p.width  = 80.f;
    if (p.height < 40.f) p.height = 40.f;

    // -- Font-size drag controls ------------------------------------------------
    // These hit rects match the positions drawn in DrawDialogueBoxEditor's readout.
    // Drag left = smaller, drag right = larger (0.15 px per font-size unit).
    const float kFontDragSpeed = 0.15f;

    // Speaker font size - readout row 5 (after Panel X/Y/W/H)
    Rectangle speakerHit{ 0.f, 88.f, 260.f, 22.f };
    if (!_dlgSpeakerFsDrag && CheckCollisionPointRec(mouse, speakerHit)
        && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        _dlgSpeakerFsDrag    = true;
        _dlgSpeakerFsDragX   = mouse.x;
        _dlgSpeakerFsDragVal = box.speakerFontSize;
    }
    if (_dlgSpeakerFsDrag)
    {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            int newVal = _dlgSpeakerFsDragVal + (int)((mouse.x - _dlgSpeakerFsDragX) * kFontDragSpeed);
            box.speakerFontSize = std::max(8, std::min(72, newVal));
        }
        else { _dlgSpeakerFsDrag = false; }
    }

    // Body font size - readout row 6
    Rectangle bodyHit{ 0.f, 112.f, 260.f, 22.f };
    if (!_dlgBodyFsDrag && CheckCollisionPointRec(mouse, bodyHit)
        && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        _dlgBodyFsDrag    = true;
        _dlgBodyFsDragX   = mouse.x;
        _dlgBodyFsDragVal = box.bodyFontSize;
    }
    if (_dlgBodyFsDrag)
    {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            int newVal = _dlgBodyFsDragVal + (int)((mouse.x - _dlgBodyFsDragX) * kFontDragSpeed);
            box.bodyFontSize = std::max(8, std::min(64, newVal));
        }
        else { _dlgBodyFsDrag = false; }
    }

    // Text left inset - how far from left edge text starts (row 7)
    const float kInsetDragSpeed = 0.5f;
    Rectangle insetLeftHit{ 0.f, 132.f, 260.f, 22.f };
    if (!_dlgInsetLeftDrag && CheckCollisionPointRec(mouse, insetLeftHit)
        && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        _dlgInsetLeftDrag    = true;
        _dlgInsetLeftDragX   = mouse.x;
        _dlgInsetLeftDragVal = box.textInsetLeft;
    }
    if (_dlgInsetLeftDrag)
    {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            float newVal = _dlgInsetLeftDragVal + (mouse.x - _dlgInsetLeftDragX) * kInsetDragSpeed;
            box.textInsetLeft = std::max(0.f, std::min(500.f, newVal));
        }
        else { _dlgInsetLeftDrag = false; }
    }

    // Text top inset - vertical gap from panel top to first text row (row 8)
    Rectangle insetTopHit{ 0.f, 154.f, 260.f, 22.f };
    if (!_dlgInsetTopDrag && CheckCollisionPointRec(mouse, insetTopHit)
        && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        _dlgInsetTopDrag    = true;
        _dlgInsetTopDragX   = mouse.x;
        _dlgInsetTopDragVal = box.textInsetTop;
    }
    if (_dlgInsetTopDrag)
    {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            float newVal = _dlgInsetTopDragVal + (mouse.x - _dlgInsetTopDragX) * kInsetDragSpeed;
            box.textInsetTop = std::max(0.f, std::min(200.f, newVal));
        }
        else { _dlgInsetTopDrag = false; }
    }
}

void Engine::DrawDialogueBoxEditor()
{
    const DialogueBox& box = _cutscene.GetLayout();

    float sw = (float)kVirtualWidth;
    float sh = (float)kVirtualHeight;

    // -- Background: dungeon tiles for context ---------------------------------
    ClearBackground(Color{ 8, 6, 10, 255 });
    if (_tileRenderer.IsLoaded())
    {
        float scaleX = sw / (RoomLayout::kCols * 16.f);
        float scaleY = sh / (RoomLayout::kRows * 16.f);
        _tileRenderer.DrawRoom(_dungeonRoomLayout, scaleX, scaleY, { 0.f, 0.f });
    }

    // -- Live dialogue box preview (no portrait) -------------------------------
    {
        std::string previewText = "The dungeon will still be there. Take a breath.";
        box.Draw(_shopBorderTex, {}, "Zeph", previewText, true, GetPromptModeForUi());
    }

    // -- Panel drag handles ----------------------------------------------------
    const Rectangle& p = box.panelRect;

    auto drawHandle = [&](Vector2 pos, Color col, int id)
    {
        Color c = (_dlgEditorHandle == id) ? WHITE : col;
        DrawCircleV(pos, kDlgHandleRadius, c);
        DrawCircleLines((int)pos.x, (int)pos.y, kDlgHandleRadius, Fade(BLACK, 0.6f));
    };

    drawHandle({ p.x,                          p.y            }, YELLOW,  0);
    drawHandle({ p.x + p.width,                p.y            }, YELLOW,  1);
    drawHandle({ p.x,                          p.y + p.height }, YELLOW,  2);
    drawHandle({ p.x + p.width,                p.y + p.height }, YELLOW,  3);
    drawHandle({ p.x + p.width * 0.5f, p.y + p.height * 0.5f }, SKYBLUE, 4);

    // -- Value readout panel ---------------------------------------------------
    const int readoutFs = 18;
    const int rowH      = readoutFs + 4;
    DrawRectangle(0, 0, 262, 184, Fade(BLACK, 0.60f));

    int ry = 6;
    auto staticLine = [&](const char* label, float val)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %.1f", label, val);
        DrawText(buf, 8, ry, readoutFs, LIME);
        ry += rowH;
    };

    staticLine("Panel X", p.x);
    staticLine("Panel Y", p.y);
    staticLine("Panel W", p.width);
    staticLine("Panel H", p.height);

    // Speaker font size - draggable (highlighted when active)
    {
        bool active = _dlgSpeakerFsDrag;
        Rectangle hit{ 0.f, (float)ry, 260.f, (float)rowH };
        if (!active) DrawRectangleRec(hit, Fade(WHITE, CheckCollisionPointRec(GetVirtualMousePos(), hit) ? 0.08f : 0.0f));
        else         DrawRectangleRec(hit, Fade(YELLOW, 0.18f));
        char buf[64];
        snprintf(buf, sizeof(buf), "Speaker Size: %d", box.speakerFontSize);
        DrawText(buf, 8, ry, readoutFs, active ? YELLOW : LIME);
        DrawText("< drag >", 180, ry, 12, Fade(WHITE, 0.4f));
        ry += rowH;
    }

    // Body font size - draggable
    {
        bool active = _dlgBodyFsDrag;
        Rectangle hit{ 0.f, (float)ry, 260.f, (float)rowH };
        if (!active) DrawRectangleRec(hit, Fade(WHITE, CheckCollisionPointRec(GetVirtualMousePos(), hit) ? 0.08f : 0.0f));
        else         DrawRectangleRec(hit, Fade(SKYBLUE, 0.18f));
        char buf[64];
        snprintf(buf, sizeof(buf), "Body Size: %d", box.bodyFontSize);
        DrawText(buf, 8, ry, readoutFs, active ? SKYBLUE : LIME);
        DrawText("< drag >", 180, ry, 12, Fade(WHITE, 0.4f));
        ry += rowH;
    }

    // Text left inset - draggable
    {
        bool active = _dlgInsetLeftDrag;
        Rectangle hit{ 0.f, (float)ry, 260.f, (float)rowH };
        if (!active) DrawRectangleRec(hit, Fade(WHITE, CheckCollisionPointRec(GetVirtualMousePos(), hit) ? 0.08f : 0.0f));
        else         DrawRectangleRec(hit, Fade(ORANGE, 0.18f));
        char buf[64];
        snprintf(buf, sizeof(buf), "Text Left: %.0f", box.textInsetLeft);
        DrawText(buf, 8, ry, readoutFs, active ? ORANGE : LIME);
        DrawText("< drag >", 180, ry, 12, Fade(WHITE, 0.4f));
        ry += rowH;
    }

    // Text top inset - draggable
    {
        bool active = _dlgInsetTopDrag;
        Rectangle hit{ 0.f, (float)ry, 260.f, (float)rowH };
        if (!active) DrawRectangleRec(hit, Fade(WHITE, CheckCollisionPointRec(GetVirtualMousePos(), hit) ? 0.08f : 0.0f));
        else         DrawRectangleRec(hit, Fade(ORANGE, 0.18f));
        char buf[64];
        snprintf(buf, sizeof(buf), "Text Top: %.0f", box.textInsetTop);
        DrawText(buf, 8, ry, readoutFs, active ? ORANGE : LIME);
        DrawText("< drag >", 180, ry, 12, Fade(WHITE, 0.4f));
        ry += rowH;
    }

    // -- Instructions banner ---------------------------------------------------
    const char* banner = "Drag handles to resize panel  |  Drag rows to change font/inset  |  [S] Print values  |  ESC close";
    int bfs = 18;
    DrawRectangle(0, (int)sh - bfs - 10, (int)sw, bfs + 10, Fade(BLACK, 0.65f));
    DrawText(banner,
        (int)(sw * 0.5f - MeasureText(banner, bfs) * 0.5f),
        (int)sh - bfs - 5, bfs, Fade(WHITE, 0.85f));
}












