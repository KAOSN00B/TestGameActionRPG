
#pragma once

#include "raylib.h"
#include "GameTypes.h"
#include "KeyBindings.h"
#include "Character.h"
#include "Prop.h"
#include "Enemy.h"
#include "Cyclops.h"
#include "Ogre.h"
#include "Molarbeast.h"
#include "SkeletonArcher.h"
#include "FlameWisp.h"
#include "SlimeEnemy.h"
#include "AbyssSlime.h"
#include "PumpkinJack.h"
#include "Minotaur.h"
#include "Sporeling.h"
#include "Shieldbearer.h"
#include "Phantom.h"
#include "BomberImp.h"
#include "Warchief.h"
#include "LivingBlade.h"
#include "Werewolf.h"
#include "ChompBug.h"
#include "Osiris.h"
#include "TitanGuard.h"
#include "ToxicVermin.h"
#include "AncientBear.h"
#include "Infernal.h"
#include "Bonechill.h"
#include "Stormclub.h"
#include "Venomfang.h"
#include "EnemyProjectile.h"
#include "MainMenu.h"
#include "PauseAndGameOver.h"
#include "Pickup.h"
#include "HealPickup.h"
#include "GoldPickup.h"
#include "SpreadProjectile.h"
#include "CyclopsLaserProjectile.h"
#include "LavaBallProjectile.h"
#include "TouchControls.h"
#include "NavigationGrid.h"
#include "VFXManager.h"
#include "DamageNumberManager.h"
#include "EncounterPlanner.h"
#include "ReinforcementPacing.h"
#include "ShopManager.h"
#include "DebugPanel.h"
#include "WorldConfig.h"
#include "AudioManager.h"
#include "RunStateController.h"
#include "RunSession.h"
#include "ClassUnlockRules.h"
#include "PrologueController.h"
#include "VillageLayoutData.h"
#include "CombatDirector.h"
#include "RoomHazardDirector.h"
#include "OverlayRenderer.h"
#include "CutsceneManager.h"
#include "WorldMapManager.h"
#include "SettingsManager.h"
#include "MetaProgression.h"
#include "Ascension.h"
#include "RoomAffix.h"
#include "VirtualCanvas.h"
#include "TileMapper.h"
#include "NineSliceEditor.h"
#include "CharacterAnimator.h"
#include "AttackEditor.h"
#include "MapEditor.h"
#include "DungeonGen.h"
#include "TileDefs.h"
#include "RoomLayout.h"
#include "RoomLibrary.h"
#include "RoomAssetCatalog.h"
#include "TileRenderer.h"
#include "GamepadInput.h"
#include "InputPrompts.h"

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>


class Engine
{
public:
    Engine();
    ~Engine();

    void Run();
    void RunFrame();

private:
    // Draws the custom sword mouse cursor on the real window, and hides the OS
    // cursor, for all normal game screens. Editors (tile mapper, 9-slice, touch
    // layout, HUD/map/hitbox/dialogue editors) keep the normal OS cursor.
    void UpdateSwordCursor();

    enum class DungeonDoorSide;

    void Init();

    void Update(float dt);
    void Draw();
    void UpdateInputPromptMode();
    InputPromptMode GetPromptModeForUi() const;

    void UpdateGamePlay(float dt);
    void SpawnWave();
    void SpawnEnemies();
    void HandleCollisions();
    void UpdateEnemyCount(float dt);
    Enemy* SpawnBasicEnemy(Vector2 pos);
    Enemy* SpawnCyclops(Vector2 pos);
    Enemy* SpawnOgre(Vector2 pos);
    void SpawnMolarbeast(Vector2 pos);
    Enemy* SpawnSkeletonArcher(Vector2 pos);
    Enemy* SpawnFlameWisp(Vector2 pos);
    Enemy* SpawnSlime(Vector2 pos, SlimeSize size);
    Enemy* SpawnSporeling(Vector2 pos);
    Enemy* SpawnShieldbearer(Vector2 pos);
    Enemy* SpawnPhantom(Vector2 pos);
    Enemy* SpawnBomberImp(Vector2 pos);
    Enemy* SpawnWarchief(Vector2 pos);
    Enemy* SpawnLivingBlade(Vector2 pos);
    Enemy* SpawnInfernal(Vector2 pos);
    Enemy* SpawnBonechill(Vector2 pos);
    Enemy* SpawnStormclub(Vector2 pos);
    Enemy* SpawnVenomfang(Vector2 pos);
    // Elite miniboss factory: picks a curated bruiser type, marks it elite.
    Enemy* SpawnEliteMiniboss(Vector2 pos);
    void SpawnBossForBiome(Vector2 pos);      // picks the boss class for _currentBiome

    // Class select + run start
    void StartMainRun();                       // ResetRunState + enter Forest
    void StartSelectedGameMode();
    void StartPrologue();
    void UpdateClassSelect();
    void DrawClassSelect();
    int  _classSelectCursor = 0;
    PrologueEntryMode _prologueEntryMode = PrologueEntryMode::NewGame;
    Texture2D _classPortraits[(int)PlayerClass::Count]{};   // idle sheets for the select cards
    // Appearance selection (independent of class).
    int  _appearanceCursor = 2;                 // default Hero03
    Texture2D _appearancePortrait{};            // idle sheet of the selected look
    void ReloadAppearancePortrait();

    // ── Cosmetic Shop (village Mirror/Tailor) ───────────────────────────────────
    // The home for changing the player's look. Appearance was REMOVED from the
    // revive/gate ClassSelect (class-only now) and lives here instead. v1 = free
    // switching among looks; gold-gated skin unlocks are a later gold sink.
    void OpenCosmeticShop();
    void UpdateCosmeticShop();
    void DrawCosmeticShop();
    GameState _cosmeticShopReturnState = GameState::Village;  // where ESC/equip returns
    float     _cosmeticShopOpenTimer   = 0.f;                 // brief input lock after opening
    Texture2D _cellsMerchantTex{};              // NPC sprite for the cells (Poe) shop

    // ── Death -> Poe revive cutscene (one-wallet meta loop, 2026-07-09) ─────────
    // On death the fallen mystic hangs as a lone lit sprite on black while the
    // gold + echoes it carried stream away and fade; then Poe (spectral phantom,
    // purple frame) offers to bring the player back and asks what they'll become,
    // flowing straight into ClassSelect (class + look) which starts the next run.
    void BeginDeathRevive();      // from the dying tick once the death beat ends
    void UpdateDeathRevive();
    void DrawDeathRevive();
    struct DeathMote { Vector2 pos; Vector2 vel; float life; float maxLife; bool gold; };
    std::vector<DeathMote> _deathMotes;
    int     _deathRevivePhase     = 0;    // 0 = loss/fade beat, 1 = Poe dialogue
    float   _deathReviveTimer     = 0.f;  // seconds elapsed in the current phase
    float   _deathReviveTypeT     = 0.f;  // typewriter reveal progress (phase 1)
    int     _deathReviveLostGold  = 0;    // captured at death for the loss animation
    int     _deathReviveLostCells = 0;
    Vector2 _deathReviveWorldPos{};       // player world pos at death (mote origin)

    // ── Boss-clear choice (every boss): return to village vs double-or-nothing ──
    // Return -> EnterVillage keeping hauled gold (run ends; spend at the build
    // menu, restart via the gate). Push onward -> full heal + bonus gold + the
    // cursed wager unlocks (only path it appears on), then continue this run.
    void OpenBossChoice();
    void UpdateBossChoice();
    void DrawBossChoice();
    int   _bossChoiceCursor    = 0;      // 0 = return to village, 1 = push onward
    float _bossChoiceOpenTimer = 0.f;    // brief input lock after the screen opens
    bool  _wagerAccessGranted  = false;  // set by "push onward"; gates the cursed wager

    bool _secondWindAvailable = false;          // meta unlock: one free revive per run
    float _secondWindToastTimer = 0.f;          // "SECOND WIND!" banner countdown

    // ── Cursed Wager (Cursed Shrine #19, redesigned) ───────────────────────────
    // Set at Zeph's shrine: pay gold to curse the whole upcoming biome (much
    // tougher enemies) in exchange for bonus gold/XP/Echoes. Repeatable — it
    // resets each time you reach the next shop. _wagerTier 0 = no active wager.
    float _runPlayerDamageMult = 1.f;           // folded into ScalePlayerHit (unused by wager)
    float _runEnemyHealthMult  = 1.f;           // folded into ConfigureSpawnedEnemy
    float _runEnemyDamageMult  = 1.f;
    int   _wagerTier           = 0;             // 0 none, 1-3 = kWagerTiers index+1
    bool  _curseShrineUsed     = false;         // one wager per biome (re-armed at each shop)
    bool  _nearCurseShrine     = false;
    float _curseShrineBobTimer = 0.f;

    // ── Generic ChoiceCardScreen (extracted from the Cursed Wager UI) ──────────
    // One debugged "N cards, pick one or leave" picker (keyboard + gamepad +
    // mouse + affordability gating + deny flash) reused by every decision room
    // (Cursed Wager, Risk Shrine, Cursed Reward, …). See
    // ROOM_EVENTS_INTEGRATION_PLAN.txt. Build the UI once; each new room is then
    // just an option table, not a UI project.
    struct ChoiceCard
    {
        std::string title;
        std::string flavor;                     // optional italic quote under the title
        int         costGold       = 0;         // 0 = free (no cost line drawn)
        std::string downsideHeader = "RISK";    // red block header
        std::string downsideText;               // may contain \n
        std::string upsideHeader   = "REWARD";  // green block header
        std::string upsideText;                 // may contain \n
        Color       tint    = RAYWHITE;
        bool        accentBar = false;          // colored top stripe in tint
        bool        enabled = true;             // false = cap-gated / unavailable
        std::string disabledReason;             // pick-label text when !enabled
    };
    struct ChoiceCardScreen
    {
        std::string title;
        std::string subtitle;
        std::string confirmVerb = "CHOOSE";     // pick-label verb ("WAGER")
        std::string confirmHint = "Confirm";    // bottom hint verb ("Wager")
        Color       titleColor = Color{ 235, 90, 120, 255 };
        Color       bgColorA   = Color{ 30, 14, 20, 255 };
        Color       bgColorB   = Color{ 42, 20, 30, 255 };
        bool        showPurse  = true;          // draw "Your gold: N"
        std::vector<ChoiceCard> cards;
        int         cursor    = 0;
        float       openTimer = 0.f;            // brief input lock after opening
        float       denyFlash = 0.f;            // red flash on unaffordable pick
        int         result    = -1;             // -2 leave, -1 pending, >=0 chosen
    };
    void OpenChoiceCards(ChoiceCardScreen& s);    // reset cursor/timers/result
    void UpdateChoiceCards(ChoiceCardScreen& s);  // input → sets s.result
    void DrawChoiceCards(const ChoiceCardScreen& s);

    ChoiceCardScreen _wagerScreen;                // Cursed Wager (first consumer)
    void  OpenCurseShrine();
    void  UpdateCurseShrine();
    void  DrawCurseShrine();
    Vector2 GetCurseShrinePos() const;

    // ── Room events: decision rooms + unified pending-modifier system ──────────
    // A "decision room" is a Standard room tagged with a RoomSpecialType at
    // generation time. The player walks to a shrine and picks a contract (via the
    // shared ChoiceCardScreen) that arms a RunModifier applied to the NEXT combat
    // room. See ROOM_EVENTS_INTEGRATION_PLAN.txt / ROOM_INTERACTION_IDEAS.txt.
    struct RunModifier
    {
        std::string label;              // HUD pill + resolve-toast name
        float enemyHpMult  = 1.f;
        float enemyDmgMult  = 1.f;
        float goldMult      = 1.f;
        float xpMult        = 1.f;
        Color tint          = Color{ 235, 180, 90, 255 };
        bool  active        = false;    // false = armed/pending, true = applying now
        int   roomsRemaining = 1;       // combat rooms it covers once active
        ShopContractType shopContract = ShopContractType::Count;
    };
    std::vector<RunModifier> _runModifiers;
    static constexpr int kMaxAcceptedModifiers = 2;   // accepted-danger cap (A4)
    float _roomModHpMult  = 1.f;        // product of active modifiers, folded into
    float _roomModDmgMult = 1.f;        // ConfigureSpawnedEnemy this room

    ChoiceCardScreen _decisionScreen;   // Risk Shrine et al. (consumer #2)
    bool  _nearDecisionShrine    = false;
    float _decisionShrineBobTimer = 0.f;
    std::string _contractToastText;     // "GREED HONORED  +N gold  +N XP"
    float       _contractToastTimer = 0.f;

    void RollDungeonRoomSpecials();     // gen-time roll of which rooms are decisions
    void OpenDecisionRoom();
    void UpdateDecisionRoom();
    void DrawDecisionRoom();
    Vector2 GetDecisionShrinePos() const;
    int  AcceptedModifierCount() const { return (int)_runModifiers.size(); }
    void ActivatePendingModifiers();            // on entering a fresh combat room
    void ResolveActiveModifiersOnRoomClear();   // on combat-room clear
    void ClearRunModifiers();                   // on death / new run

    // Bestiary (#20)
    void  UpdateBestiary();
    void  DrawBestiary();
    GameState _bestiaryReturnState = GameState::Menu;
    float _bestiaryScroll = 0.f;

    // Daily runs (#20) — a seeded, reproducible dungeon shared by the calendar day.
    bool _isDailyRun = false;
    int  _dailySeed  = 0;
    int  ComputeDailySeed() const;   // yyyymmdd from the local clock

    // Relic choice (#22) — elite/boss kills owe a 3-card relic pick, deferred
    // until the room is clear so it never interrupts combat.
    int       _pendingRelicChoices = 0;
    RelicType _relicChoices[3] = { RelicType::Count, RelicType::Count, RelicType::Count };
    int       _relicChoiceCursor = 0;
    float     _relicChoiceOpenTimer = 0.f;
    void OpenRelicChoice();
    void UpdateRelicChoice();
    void DrawRelicChoice();

    // Per-ability HUD/card icons for non-elemental abilities (PowerUps/Ability_*.png).
    Texture2D _abilityIcons[(int)AbilityType::Count]{};
    Texture2D* GetAbilityIcon(AbilityType type);   // nullptr => use procedural/element fallback

    // Per-ability animated in-world FX strips (PowerUps/FX_*.png, 64px cells).
    Texture2D _abilityFx[(int)AbilityType::Count]{};
    int       _abilityFxFrames[(int)AbilityType::Count]{};
    void SpawnAbilityFx(AbilityType type, Vector2 playerPos, Vector2 facing, float facingSign);

    // Owned boss impact/cast FX sheets (FX_Boss*.png), indexed by BossFx id.
    std::vector<Texture2D> _bossFx;
    std::vector<int>       _bossFxFrames;
    void SpawnBossFx(Vector2 worldPos, int fxId);
    // Elite signature art (BossFx strips with caller scale/tint): one-shot
    // impact and lingering hazard decal. See CombatDirector's zone routing.
    void SpawnEliteFx(Vector2 worldPos, int fxId, float scale, Color tint);
    void SpawnEliteHazardFx(Vector2 worldPos, int fxId, float scale, float duration, Color tint);
    Enemy* FindEncounterDebugTarget();
    const Enemy* FindEncounterDebugTarget() const;
    void DrawEliteSignatureTelemetry() const;   // debug-panel-only elite/boss readout

    // Relic icons, one per archetype (PowerUps/Relic_<archetype>.png).
    Texture2D _relicIcons[7]{};
    const Texture2D* GetRelicIcon(RelicType type) const;

    // Debug-panel direct spawns (index maps to kDebugEnemyList / kDebugBossList).
    void DebugSpawnNewEnemy(int index, Vector2 pos);
    void DebugSpawnLoot();   // drops a pile of gold + echo pickups near the player
    void DebugSpawnNewBoss(int index, Vector2 pos);
    void UpdateEnemyProjectiles(float dt);    // arrows + fire bolts (player collision)
    void DrawEnemyProjectiles(Vector2 worldOffset) const;

    // Playable village-builder placement test (debug/dev). This is intentionally
    // runtime-facing, separate from MapEditor, so placement can be tested with the
    // real player controller, camera, attacks, abilities, and collision response.
    enum class VillageObjectLayer { Ground, Objects, Overhead };

    struct VillageRuntimePart
    {
        VillageObjectLayer layer = VillageObjectLayer::Objects;
        short localCol = 0, localRow = 0;
        short sheet = -1;
        short col = 0, row = 0;
    };
    struct VillageRuntimeSolid
    {
        short localCol = 0, localRow = 0;
        short w = 1, h = 1;
    };
    // A doorway authored on a village object ("door ..." line in villageobject_*.txt).
    // Doors open automatically when the player walks close, and stepping onto an
    // open door transitions into the interior named by targetInterior.
    struct VillageRuntimeDoor
    {
        std::string doorName;                 // e.g. "door_01"
        short localCol = 0, localRow = 0;     // top-left cell within the object
        short w = 1, h = 1;                   // door size in cells
        std::string targetInterior;           // e.g. "interior_default"
        bool blocksWhenClosed = true;         // door is solid until it opens
        int openAnimationIndex = -1;          // -1 = no authored open animation yet
    };
    // An NPC anchor authored on a village object ("npc ..." line). Zeph reuses the
    // real ShopManager NPC; other names are parsed but inert for now.
    struct VillageRuntimeNpc
    {
        std::string npcName;                  // e.g. "Zeph"
        short localCol = 0, localRow = 0;     // cell within the object
        std::string assignment;               // authored hint (e.g. "object:object_test"), unused at runtime
    };
    struct VillageRuntimeObjectDef
    {
        std::string name;
        std::string path;
        int cols = 1, rows = 1;              // trimmed to the drawn sprite bounds at load
        int costGold = 0;                    // gold spent to place (refunded on remove)
        bool isDecoration = false;           // decor: no collision/footprint, paints under everything
        bool required = false;               // service/quest asset, not optional decor
        bool uniqueInVillage = false;        // only one allowed in the real village
        bool removable = true;               // can be removed/refunded after placement
        std::string serviceName;             // Shop, Relic, Wardrobe, Bestiary, etc. for future dispatch
        Texture2D pngTexture{};              // VillageAssets PNG path, used by .vasset-backed entries
        float pngScale = 3.f;
        bool  pngAnimated = false;
        int   pngAnimCols = 1;
        int   pngAnimRows = 1;
        int   pngAnimFrames = 1;
        float pngAnimFps = 6.f;
        std::vector<Rectangle> pngColliders; // image-local px colliders from .vasset
        std::vector<VillageRuntimePart> parts;
        std::vector<VillageRuntimeSolid> solids;
        std::vector<VillageRuntimeDoor> doors;
        std::vector<VillageRuntimeNpc> npcs;
    };
    struct VillagePlacedObject
    {
        std::string defName;                 // persisted identity (catalog order can change)
        int defIndex = -1;                   // runtime-resolved catalog index
        int cellCol = 0, cellRow = 0;
    };
    void EnterVillagePlayground();            // Y-key sandbox tester (never saves)
    void EnterVillage();                      // main-game hub (saves, graveyard respawn, gate)
    void EnterVillageShared(bool sandboxMode);
    bool VillageHasPlacedObject(const std::string& defName) const;
    void UnloadVillagePlayground();
    void LoadVillagePlaygroundSheets();
    void LoadVillagePlaygroundCatalog();
    bool LoadVillageRuntimeObject(const std::string& path, VillageRuntimeObjectDef& outDef) const;
    bool LoadVillageRuntimeAssetObject(const std::string& path, VillageRuntimeObjectDef& outDef) const;
    void UpdateVillagePlayground(float dt);
    void DrawVillagePlayground();
    void DrawVillageRuntimeObject(const VillageRuntimeObjectDef& def, int cellCol, int cellRow, Vector2 worldOffset, Color tint, VillageObjectLayer layer) const;
    bool VillagePlaygroundCanPlace(int defIndex, int cellCol, int cellRow) const;
    Rectangle VillagePlacedObjectWorldRect(const VillagePlacedObject& placed, const VillageRuntimeSolid* solid) const;
    bool VillageObjectHasSolidAt(const VillageRuntimeObjectDef& def, int localCol, int localRow) const;
    static bool VillageObjectIsOneTimeService(const VillageRuntimeObjectDef& def);
    static bool VillageObjectIsPermanentService(const VillageRuntimeObjectDef& def);
    static const char* VillageObjectRuleLabel(const VillageRuntimeObjectDef& def);
    void ResolveVillagePlaygroundCollision(Vector2 beforePos);
    // Doors + NPCs on placed village objects
    Rectangle VillageDoorWorldRect(const VillagePlacedObject& placed, const VillageRuntimeDoor& door) const;
    bool VillageDoorIsOpen(const VillagePlacedObject& placed, const VillageRuntimeDoor& door) const;
    void RefreshVillageShopNpc();
    int FindVillageObjectDefIndex(const std::string& defName) const;
    void DrawVillageField(Vector2 worldOffset) const;   // forest floor + wall border ring
    void DrawVillagePlacementGhost(Vector2 worldOffset, Vector2 mouseWorld);
    void SaveVillageLayout() const;   // village_layout.txt — main-game village only, tester never writes
    void LoadVillageLayout();

    // Citizen NPCs — ambient wanderers; more appear as the village grows.
    struct VillageCitizen
    {
        Vector2 position{};
        Vector2 walkTarget{};
        int behaviour = 0;               // 0 = walking, 1 = idle, 2 = chatting
        float behaviourTimer = 0.f;      // time left in idle/chat
        int chatPartnerIndex = -1;
        float chatCooldown = 0.f;        // seconds before this citizen may chat again
        bool facingLeft = false;
        float walkBobPhase = 0.f;        // placeholder-sprite walk animation
        Color tint{};                    // per-citizen clothing colour (placeholder art)
    };
    bool VillageCitizenSpotIsClear(Vector2 position) const;

    Vector2 VillageRandomStandablePoint() const;

    void SyncVillageCitizenCount();      // citizen count follows placed building count (capped)
    void UpdateVillageCitizens(float deltaTime);
    void DrawVillageCitizens(Vector2 worldOffset) const;
    // Placeholder building interior ("interior_default") reached through doors
    void EnterVillageInterior(const std::string& interiorName, Vector2 villageReturnPos);
    void ExitVillageInterior();
    void UpdateVillageInterior(float dt);
    void DrawVillageInterior();
    // -- Poison clouds (Sporeling deaths + ToxicVermin pools) ----------------
    void SpawnPoisonCloud(Vector2 pos, float radius);
    void UpdatePoisonClouds(float dt);
    void DrawPoisonClouds(Vector2 worldOffset) const;

    // -- Relics ---------------------------------------------------------------
    // Scales a player hit by the owned damage relics, reading the target's
    // status. Every enemy->TakeDamage call from the player routes through this.
    int  ScalePlayerHit(const Enemy& target, int baseDamage, bool& outCrit) const;
    // Fires on-kill relic effects (lifesteal, corpse explosions). Called from
    // the enemy-death path with the dead enemy's status snapshot.
    void ApplyRelicOnKill(Vector2 pos, bool wasBurning, bool wasFrozen,
                          bool wasCharged, bool eliteOrBoss);
    void ApplyPendingReflect();                   // Paladin Aegis: reflect damage to attacker

    // ── Juice ──────────────────────────────────────────────────────────────────
    float _hitStopTimer = 0.f;   // > 0 = gameplay frozen for a few frames (impact)
    void  RequestHitStop(float seconds) { if (seconds > _hitStopTimer) _hitStopTimer = seconds; }

    // Slow-mo (crit + boss death): scales gameplay dt while active.
    float _slowMoTimer = 0.f;
    float _slowMoScale = 1.f;
    void  TriggerSlowMo(float dur, float scale) { if (dur > _slowMoTimer) { _slowMoTimer = dur; _slowMoScale = scale; } }

    // Crit focus: a soft dark spotlight (no blur — pixel-friendly) on the hit.
    float   _critFocusTimer = 0.f;
    float   _critFocusDur   = 0.3f;
    Vector2 _critFocusScreenPos{};

    // Screen flash (level-up gold, ultimate elemental).
    float _screenFlashTimer = 0.f;
    float _screenFlashDur   = 0.f;
    Color _screenFlashColor{};
    int   _lastPlayerLevel  = 1;
    void  TriggerScreenFlash(Color c, float dur) { _screenFlashColor = c; _screenFlashDur = dur; _screenFlashTimer = dur; }
    void  DrawScreenFx();   // crit spotlight + flash + hurt/death vignette (end of Draw)

    // Player hurt / death feedback (red edge vignette; death adds slow-mo).
    float _playerHurtTimer = 0.f;
    float _lastPlayerHp    = 0.f;
    float _deathVignette    = 0.f;   // 0..1 dark-red overlay strength on death

    // Smoothed UI — displayed values lerp toward the real ones so counters tick
    // and orbs slide instead of snapping.
    float _displayGold    = 0.f;
    float _displayCells   = 0.f;
    float _displayHpPct   = 1.f;
    float _displayManaPct = 1.f;

    // ── Live juice tuning (debug panel: J in debug mode) ────────────────────────
    // Runtime copies of the feel constants so they can be nudged while playing.
    float _juiceCritSlowDur   = 0.30f;
    float _juiceCritSlowScale = 0.40f;
    float _juiceBossSlowDur    = 0.80f;
    float _juiceBossSlowScale  = 0.30f;
    float _juiceKillHitStop    = 0.07f;
    // Every landed (non-kill) hit now gets a brief freeze so strikes "bite" instead
    // of passing through. Scaled by the fraction of the target's max HP removed, so
    // a chunky blow freezes longer than chip damage — and huge-HP bosses barely
    // stutter. Clamped to [min, max].
    float _juiceHitStopMin     = 0.022f;   // floor: even a tiny tick registers
    float _juiceHitStopMax     = 0.060f;   // cap on a normal (non-kill) hit
    float _juiceShakeMult      = 1.00f;
    float _juiceCritFocus      = 0.55f;
    bool  _juiceForceCrit      = false;   // debug: every player hit crits
    bool  _juicePanelOpen      = false;
    int   _juiceSel            = 0;
    float _juiceNudgeCd        = 0.f;
    void  UpdateJuicePanel();
    void  DrawJuicePanel();

    // Central hit feedback: floating number (pop on crit), impact sparks, and on
    // crit → slow-mo + focus, on kill → hit-stop (boss kill → cinematic slow-mo).
    int RegisterHitFx(Enemy& enemy, float healthBefore, bool crit,
                      bool backstab = false, bool damageOverTime = false,
                      std::uint32_t attackId = 0);
    // Floating "SHIELDED" / "BLOCKED" callout when a hit is denied (see Enemy::ConsumeHitBlock).
    void ShowBlockedHitFeedback(Enemy& enemy, Enemy::HitBlockReason reason);
    // Big floating boss-state word (ENRAGED / PHASE SHIFT) on a state transition.
    void  ShowBossCallout(Vector2 enemyPos, const char* text);
    void GrantRelic(RelicType type);              // adds relic + shows a toast
    RelicType RollRandomRelic() const;            // weighted by rarity, unowned
    void DrawOwnedRelics() const;                 // HUD strip
    void UpdateCyclopsLasers(float dt);
    void UpdateLavaBallProjectiles(float dt);
    void TriggerScreenShake(float strength, float duration);
    void DrawCyclopsLasers(Vector2 worldOffset);
    void DrawHUD(bool villageMode = false);
    void DebugStartRun();
    void DebugRestartRoomAs(RoomType type);
    void DebugRestartDungeonRoomAs(RoomType type);
    void DebugSetEliteMechanic(int mechanic);
    void DrawHowToPlay();
    void DrawAbilityBar();   // unified 1-2-3-4 slot HUD
    void DrawWaveIntro();
    void HandlePlayerMeleeDamage();
    Vector2 GetTreasureChestWorldPos() const;
    void SetTreasureChestTile(TileType type);
    Rectangle GetTreasureChestRect() const;
    void OpenTreasureChest();
    void HandlePlayerCastRequest();
    enum class AbilityAimMode { Instant, Direction, GroundTarget };
    struct AbilityAimProfile
    {
        AbilityAimMode mode = AbilityAimMode::Instant;
        float range = 0.f;
        float radius = 0.f;
        float length = 0.f;
        float width = 0.f;
        float moveScale = 1.f;
    };
    struct PendingAbilityAim
    {
        bool active = false;
        int slot = -1;
        AbilityType ability = AbilityType::None;
        Vector2 direction{ 1.f, 0.f };
        Vector2 target{};
        bool commitOnMouseRelease = false;
    };
    AbilityAimProfile GetAbilityAimProfile(AbilityType ability) const;
    void BeginAbilityInput(int slot);
    void UpdateAbilityAiming();
    void CommitAbilityAim();
    void CancelAbilityAim();
    void DrawAbilityAimPreview() const;
    Vector2 GetCurrentAimWorldPoint() const;
    PendingAbilityAim _pendingAbilityAim{};
    Vector2 _committedAimDirection{ 1.f, 0.f };
    Vector2 _committedAimTarget{};
    AbilityType _committedAimAbility = AbilityType::None;

    void SpawnSpreadBurst(AbilityType element);
    void SpawnBolt(AbilityType element);
    void SpawnUltimateBurst(AbilityType element);
    void UpdateUltimateBlasts(float dt);
    void DrawUltimateBlasts(Vector2 worldOffset);

    // Cinematic ultimate sequence
    void TriggerUltimateSequence(AbilityType element);
    void UpdateUltimateSequence(float dt);
    void DrawUltimateSequence();
    void ApplyUltimateImpact();

    // Mage spells retain the legacy enum IDs for save compatibility, but use
    // distinct runtime behaviours (walls, blink, weather and aimed projectiles).
    struct MageSpellField
    {
        AbilityType ability = AbilityType::None;
        Vector2 pos{};
        Vector2 direction{ 1.f, 0.f };
        Vector2 strikePos{};
        float timer = 0.f;
        float duration = 0.f;
        float tickAccum = 0.f;
        float tickInterval = 0.45f;
        float radius = 0.f;
        float length = 0.f;
        float width = 0.f;
        float windup = 0.f;
        float strikeFlash = 0.f;
        int tickCount = 0;
        bool impacted = false;
    };
    std::vector<MageSpellField> _mageSpellFields;
    void CastMageSpell(AbilityType ability, Vector2 direction, Vector2 target);
    void UpdateMageSpells(float dt);
    void DrawMageSpells(Vector2 cameraRef) const;
    bool DamageMageEnemy(Enemy& enemy, int damage, AbilityType element,
                         bool burn, bool chill, bool shock);
    void DamageMageArea(Vector2 centre, float radius, int damage, AbilityType element,
                        bool burn, bool chill, bool shock);

    // ── Class abilities (Warrior kit, and future non-elemental classes) ─────────
    void HandleClassAbilityCast(AbilityType ability);
    // Deals damage to every live enemy overlapping a world-space rectangle.
    // Returns how many were hit. Optional knockback/stun/bleed on each victim.
    int  DamageEnemiesInRect(Rectangle worldRect, int damage, float knockback,
                             float stunSeconds, float bleedSeconds, int bleedDmgPerTick,
                             bool ignoreShield = false);
    int  DamageEnemiesInDirectedBox(Vector2 origin, Vector2 direction, float length, float width,
                                    int damage, float knockback, float stunSeconds,
                                    float bleedSeconds, int bleedDmgPerTick,
                                    bool ignoreShield = false);
    void ApplyPlayerLifesteal(int damageDealt);
    void UpdateWarriorEffects(float dt);
    void DrawWarriorEffects(Vector2 camRef);
    // Transient class-ability VFX (spins, waves, spikes, daggers, smoke...).
    // Mostly visual — most abilities apply damage instantly on cast. A few kinds
    // (e.g. the Rogue's poison pool) tick damage over their lifetime via tickDamage.
    enum class WarriorVfxKind {
        Whirl, Slam, Wave, Spikes, Axe, Bash, Cry,          // Warrior
        Fan, Teleport, Smoke, Marks, Barrage, PoisonZone, Flurry,  // Rogue
        Trap                                                // Hunter (armed ground trap)
    };
    struct WarriorVfx
    {
        WarriorVfxKind kind;
        Vector2 pos;          // world anchor
        Vector2 dir;          // facing (for directional effects)
        float   timer   = 0.f;
        float   lifetime = 0.3f;
        float   radius  = 0.f;
        Color   tint    = WHITE;
        // Optional lingering damage (0 = pure visual). Ticks enemies inside radius.
        int     tickDamage   = 0;
        float   tickInterval = 0.4f;
        float   tickAccum    = 0.f;
        // Hunter traps: sit dormant, arm after armDelay, then snap when an enemy
        // enters triggerRadius — firing a one-shot burst (trapDamage + trapFreeze
        // seconds of hard freeze). Untriggered traps expire at their long lifetime.
        bool    isTrap        = false;
        bool    triggered     = false;
        float   armDelay      = 0.5f;   // seconds before the trap becomes live
        float   triggerRadius = 70.f;   // enemy contact distance that snaps it
        int     trapDamage    = 0;      // blast damage dealt inside radius on snap
        float   trapFreeze    = 0.f;    // >0 = freeze foes in radius on snap (secs)
        float   trapKnockback = 0.f;    // knockback strength on snap
        // Optional flying sprite (e.g. the ability's icon as a thrown projectile).
        Texture2D* sprite    = nullptr;
        bool    spin         = false;   // rotate the sprite as it travels (axes)
        // Optional animated FX strip (64px cells). Persistent zones can loop the
        // strip for their full lifetime instead of stretching one pass over it.
        Texture2D* fxStrip   = nullptr;
        int     fxFrames     = 0;
        float   fxScale      = 1.f;
        bool    fxLoop       = false;
        float   fxFrameTime  = 1.f / 20.f;
        float   vy           = 0.f;   // vertical drift over lifetime (fan spreads)
    };
    std::vector<WarriorVfx> _warriorVfx;

    // Hunter class identity: counts landed basic shots; every 3rd shot marks the
    // target (see the projectile.IsBasic() hit path).
    int _hunterShotsSinceMark = 0;
    float _lifestealAccum = 0.f;   // fractional HP banked from lifesteal hits
    // Per-room sustain caps (reset on room entry — see Balance::Sustain).
    int   _roomLifestealHealed = 0;   // HP healed via lifesteal this room
    int   _roomHealDrops       = 0;   // heal pickups spawned this room (chance + timed)

    // ── Balance telemetry (Phase 5) ───────────────────────────────────────────
    // One record per cleared combat room; shown in the debug panel overlay so
    // balance tuning reads real numbers instead of vibes.
    struct RoomTelemetry
    {
        int   act = 0, room = 0;
        char  roomType[12] = "";
        float clearSeconds = 0.f;   // room entry -> all enemies dead
        int   dmgTaken = 0;         // player HP lost during the room
        int   healed = 0;           // player HP recovered during the room
        int   healDrops = 0;        // heal pickups spawned
        int   xpGained = 0;         // room-clear XP awarded (after wager bonus)
        int   goldDelta = 0;        // wallet change across the room
        int   playerLevel = 0;      // level when the room cleared
        int   powerLevel = 0;       // enemy power level for the room
    };
    static constexpr int kTelemetryHistory = 8;
    RoomTelemetry _roomTelemetry[kTelemetryHistory]{};
    int    _roomTelemetryCount = 0;      // total records written (ring index derives)
    double _roomEnterTime = 0.0;         // GetTime() stamp on room entry
    int    _roomEnterGold = 0;           // wallet snapshot on room entry
    void   DrawRoomTelemetry() const;    // debug-panel companion overlay
    void GenerateStartingAbilityOptions();
    void DrawStartingAbilityChoice();
    void EnterDungeonRoom(int roomIdx, DungeonDoorSide entryDoorSide, Vector2 playerSpawnPos, bool resetRoomStates);
    RoomLayout BuildDungeonRoomLayout(int roomIdx, const TileDefSet& definitions,
                                      int visualVariant, int propDensityBonus = 0);
    void RefreshHandcraftedRooms();
    bool GenerateHandcraftedDungeon(Biome biome,
                                    std::vector<std::string>& roomIds,
                                    int maxAttempts = 600);
    Vector2 GetDungeonEntranceSpawnPos() const;
    void EnterDungeonShopIfNeeded(const DungeonRoom& room);
    void UpdateSpreadProjectiles(float dt);
    void SpawnEnemyDrop(Vector2 worldPos, bool isOgre, bool isBoss);
    void SpawnTimedPickup();

    // -- Meta progression (Echoes / Poe's Altar) ----------------------
    Vector2 GetPoeAltarPos() const;         // world pos of the altar in Zeph's room
    void    HandlePlayerDeathMetaPenalty();    // gold retention + lose carried cells
    void    UpdateMetaShop(float dt);          // GameState::MetaShop input handling
    void    DrawMetaShop();                    // GameState::MetaShop rendering
    void SpawnBossSupportAdds();
    void UpdateBossSupportRespawns(float dt);
    void ClearBossSupportAdds();
    const char* GetBiomeName(Biome biome) const;
    void LoadTilesetForBiome(Biome biome);
    void LoadDungeonVisualVariants(Biome biome);
    void AssignDungeonVisualVariantWings();
    int  GetDungeonVisualVariantForRoom(int roomIdx) const;
    void LoadDungeonVisualVariantAssets(int variantIdx, TileDefSet& defs, TileRenderer& renderer);
    void ApplyBiome(Biome biome);
    void PopulatePropsForBiome(Biome biome);
    void UpdateBiomeTransition(float dt);
    float GetBiomeTransitionAlpha() const;

    void DrawLevelUpChoice();
    void GenerateLevelUpOptions(LevelUpOfferContext context = LevelUpOfferContext::NormalLevel);
    void GenerateTreasureChestOptions(); // mixed upgrade + ability cards for treasure room chest
    void DrawAbilityChoice();
    void GenerateAbilityChoiceOptions();
    void ResetRunState();


    // -- EXP Tally screen -----------------------------------------------------
    void UpdateExpTally(float dt);
    void DrawExpTally();

    // -- Demo end screen -------------------------------------------------------
    void DrawDemoEnd();

    // -- Room-based run progression --------------------------------------------
    void StartNextRoom(RoomType type);    // internal setup helper (biome + wave intro)
    void GenerateActMap();                // builds the full act node graph
    void EnterMapRoom(int nodeIdx);       // called when the player clicks a map node
    void CompleteCurrentMapNode();        // marks the current node complete and unlocks next nodes
    void HandleRoomContinueAction();      // shared continue path for cleared/completed rooms
    void DrawMap();                       // Slay-the-Spire�style act map screen
    std::vector<Vector2> GetCyclopsLaserEndpoints(const CyclopsLaserProjectile& laser) const;
    bool SegmentHitsRect(Vector2 start, Vector2 end, float thickness, const Rectangle& rect) const;

    // -- Settings screen -------------------------------------------------------
    void UpdateSettings(float dt);
    void DrawSettings() const;
    void DrawSettingsKeybindings(float contentY, float panelX, float panelW, Vector2 mouse) const;
    void ApplySfxVolume();   // push current SFX scale to all owned Sound objects

    void UpdateMusicSystem();
    void EnsureAudioInitialized();
    void StartVictoryMusic(MusicCue cue);
    void ResetMusicState();

    Biome GetBiomeForAct(int act) const;

    // Touch-mode helpers
    void UpdateTouchControls();
    void DrawTouchAbilityArc();
    void ScanAbilityArcTaps();
    void EnterTouchButtonMapping();
    int  DrawTouchButtonMapping();     // returns 0=none 1=save 2=back 3=default
    void ApplyTouchCustomLayout();
    float GetTouchMappingRadius(int idx) const;
    Rectangle GetDebugToggleTabRect() const;
    bool HandleDebugToggleTabInput();
    void DrawDebugToggleTab();
    void SaveKeybindings();
    void LoadKeybindings();
    int GetActiveEnemyCount() const;
    bool IsBossFightActive() const;
    bool TryGetPooledEnemySpawn(Vector2 pos);
    bool TryGetPooledCyclopsSpawn(Vector2 pos);
    bool TryGetPooledOgreSpawn(Vector2 pos);
    bool TryGetPooledMolarbeastSpawn(Vector2 pos);
    int GetCyclopsSpawnCountForWave(int wave) const;
    int GetOgreSpawnCountForWave(int wave) const;
    int GetEnemyPowerLevelForWave(int wave) const;
    void ConfigureSpawnedEnemy(Enemy& enemy);

    Vector2 GetRandomPropPosition();
    bool IsSpawnPositionValid(Vector2 pos);
    void RespawnOutOfBoundsEnemies();
    bool TryGetFarSpawnPosition(Vector2& pos, float minPlayerDistance);

private:
    // -- Act map node � one room on the Slay-the-Spire�style map --------------
    struct MapNode
    {
        int      row       = 0;               // 0 = entry, 5 = boss
        float    normX     = 0.5f;            // 0�1 horizontal position in row
        RoomType type      = RoomType::Standard;
        bool     completed = false;           // player has cleared this room
        bool     available = false;           // player can click this node now
        std::vector<int> nextNodes;           // indices into _actMap
        Vector2  drawPos{};                   // screen-space position (computed in GenerateActMap)
    };

    // -- Virtual canvas + settings ---------------------------------------------
    RenderTexture2D _virtualCanvas{};
    Texture2D       _settingsBorderTex{};
    SettingsManager _settingsMgr;
    GameState       _stateBeforeSettings = GameState::Menu;
    // Settings screen UI state
    int   _settingsTab          = 0;   // 0=Display  1=Audio  2=Keybindings
    int   _settingsDragSlider   = -1;  // -1=none  0=master  1=music  2=sfx
    float _settingsDragStartX   = 0.f;
    float _settingsDragStartVal = 0.f;
    int   _settingsRebindSlot   = -1;  // -1=none; slot index 0-9 when waiting for key (M&K)
    int   _keybindSubTab        = 0;   // 0=M&K  1=Gamepad  (sub-tab inside KEYBINDINGS tab)
    int   _settingsGpRebindSlot = -1;  // -1=none; 0=attack 1=dash 2-5=ability[0-3] 6=pause
    GamepadBindings _gamepadBindingsEdit;
    // Settings screen gamepad cursor
    int   _settingsGpSection    = 0;   // 0=main tabs  1=sub-tabs  2=content rows  3=back button
    int   _settingsGpTabCursor  = 0;   // 0=Display  1=Audio  2=Keybindings
    int   _settingsGpSubCursor  = 0;   // 0=M&K  1=Gamepad
    int   _settingsGpContentRow = 0;   // row index within active tab content
    int   _settingsGpContentCol = 0;   // column (Display option buttons; unused elsewhere)
    float _settingsGpCooldown   = 0.f;

    // -- Meta progression (Echoes / Poe's Altar) --------------------------
    MetaProgressionManager _meta;
    bool  _nearPoeAltar        = false;  // player in range of the altar this frame
    bool  _deathPenaltyApplied    = false;  // guards double-applying gold retention
    float _cellsBankedToastTimer  = 0.f;    // "+N Echoes banked" HUD toast countdown
    int   _cellsBankedToastAmount = 0;

    // Relic acquisition toast ("New Relic: <name>")
    float _relicToastTimer = 0.f;
    std::string _relicToastName;

    // Ascension — difficulty tier captured at run start from _meta. The chosen
    // tier's cumulative modifiers are resolved once (ResetRunState) and cached
    // here; most effects fold into the run-wide multipliers, bossHpMult/heal
    // penalty are read directly at their hook sites.
    int  _ascensionTier     = 0;
    bool _ascensionRecorded = false;
    AscensionModifiers _ascensionMods;
    float _poeAltarBobTimer    = 0.f;    // altar orb floating animation
    int   _metaShopCursor         = 0;      // keyboard/gamepad cursor into the unlock grid
    float _metaShopNavCooldown    = 0.f;    // gamepad d-pad repeat cooldown
    int   _poeGreetingIdx         = 0;      // which of Poe's lines shows this visit
    float _metaShopOpenTimer      = 0.f;    // brief input lock so the opening press isn't consumed
    GameState _metaShopReturnState = GameState::DungeonRun;

    RunStateController _runState;
    GameState& _gameState;
    GameState& _howToPlayFrom;
    int& _htpTab;
    float& _htpSlideOffset;
    float _htpGpCooldown = 0.f;

    bool       _audioInitialised  = false;
    bool       _shouldExit        = false;
    GameState  _stateBeforePause  = GameState::Play;  // so Pause ESC returns to the right state
    bool _waveStarting = true;
    bool _wave1LevelUpDone = false; // ensures forced level-up after wave 1 only fires once
    bool _playerDying = false;
    bool _shouldClose = false;
    bool _awaitingStartingAbility = false;

    int _wave        = 0;
    int  _enemiesKilled      = 0;
    int  _goldDroughtCounter = 0;  // kills since last Five-or-better drop; resets at 5
    int  _bossesDefeated     = 0;  // how many Molarbeasts have been killed this run
    bool _demoCompleted      = false; // true after 2 boss kills OR secret code � unlocks debug

    float _shakeTimer = 0.f;
    float _shakeStrength = 0.f;
    float _gameTimer = 0.f;
    float _pickupSpawnTimer = 0.f;

    Vector2 _shakeOffset = { 0.f, 0.f };
    Vector2 _cameraPos   = { 0.f, 0.f };

    const int _windowWidth = 1920;
    const int _windowHeight = 1080;

    float _waveIntroTimer = 0.f;
    float _waveIntroDuration = 2.5f;

    float _gameOverTimer  = 0.f;
    float _gameOverDelay  = 2.f;
    float _fadeInTimer    = 0.f;
    float _fadeInDuration = 2.f;  // set alongside _fadeInTimer; controls fade speed
    float _bossWarningTimer = 0.f;
    float _levelUpOpenTimer = 0.f;  // blocks card clicks briefly after panel opens

    // -- Act / room progression ------------------------------------------------
    // _wave keeps its name but now means "total rooms entered this run"
    // (used for enemy scaling via GetEnemyPowerLevelForWave).
    // _currentAct / _currentRoom drive display and encounter selection.
    int      _currentAct      = 1;                  // 1-indexed; advances after each boss clear
    int      _currentRoom     = 0;                  // 1-5 normal + 6 boss within the act
    RoomType _currentRoomType = RoomType::Standard; // drives SpawnEnemies and reward logic

    // Room affixes — per-room modifier rolled on entry to a Standard combat room.
    // Read at the spawn/drop/on-kill hooks; None is a pure no-op.
    RoomAffix _currentRoomAffix    = RoomAffix::None;
    float     _roomAffixBannerTimer = 0.f;   // HUD "AFFIX" banner fade countdown

    bool     _pendingRoomChoice  = false; // after AbilityChoice (boss clear), show new-act map
    bool     _roomClearPending   = false; // combat finished � waiting for player to click Continue
    float    _roomClearTimer     = 0.f;  // non-combat rooms wait before advancing (Rest/Store)

    // EXP tally state
    float _pendingExp             = 0.f;   // EXP accumulated during combat, drained during tally
    float _expTallyAccum          = 0.f;   // fractional drain accumulator
    bool  _expTallyDone           = false; // bar fully drained � show dismiss hint
    int   _tallyStartLevel        = 1;     // player level when tally begins, to count level-ups
    int   _tallyLevelUpsRemaining = 0;     // level-up choices still to show after tally
    bool  _tallyChoiceChaining    = false; // true once player pressed Continue to start chain

    // Act map state (Slay-the-Spire�style node graph)
    std::vector<MapNode> _actMap;
    int   _currentMapNodeIdx  = -1;         // index of the node currently in / last completed
    int   _mapKeySelectedIdx  = -1;         // keyboard-highlighted node on the map screen (-1 = none)
    float _mapOpenTimer       = 0.f;        // brief block after the map opens to prevent accidental clicks
    bool  _startBiomeDungeon  = true;       // fallback only; sequence takes precedence
    static constexpr int kTotalActs = 5;
    std::vector<Biome> _biomeSequence;      // 5 randomly chosen biomes per run

    // -- World map (biome selection) ---------------------------------------
    WorldMapManager       _worldMap;
    RunSession            _runSessionData;
    int                   _worldZone = 0;          // 0=Forest, 1-4=picked zones, 5=DemonsInsides
    std::vector<Biome>    _worldCompletedBiomes;   // selected biomes played after Forest
    std::vector<int>      _worldChosenNodeIndices; // 0/1/2 tierIdx chosen at each completed tier
    int                   _worldMapPreparedZone = -1;

    // -- Map screen right-panel debug editor -------------------------------
    bool  _mapEditorActive  = false;
    int   _mapEditorSelIdx  = 0;
    float _mapJourneyX      = 0.725f;  // 0  right panel X as fraction of sw
    float _mapPad           = 21.f;    // 1  padding inside journey panel
    float _mapTitleFs       = 43.f;    // 2  "JOURNEY" title font
    float _mapRoomsFs       = 44.f;    // 3  Rooms / Gold row font
    float _mapNoRoomsFs     = 30.f;    // 4  "No rooms cleared yet" font
    float _mapBiomeTitleFs  = 43.f;    // 5  "BIOMES" label font
    float _mapBiomeLabelFs  = 0.350f;  // 6  diamond text size as fraction of half-size
    float _mapSqSz          = 96.f;    // 7  visited-room square size
    float _mapSqGap         = 33.f;    // 8  gap between visited-room squares
    float _mapIconSz        = 87.f;    // 9  map node icon size
    float _mapDiamondSz     = 56.f;    // 10 biome diamond half-size in pixels
    float _mapHeaderX       = 0.525f;  // 11 top Act header center X as fraction of sw
    float _mapHeaderY       = 10.f;    // 12 top Act header Y
    float _mapHeaderFs      = 55.f;    // 13 top Act header font size
    float _mapSubX          = 0.535f;  // 14 subtitle center X as fraction of sw
    float _mapSubY          = 61.f;    // 15 subtitle Y
    float _mapSubFs         = 30.f;    // 16 subtitle font size
    float _mapHintY         = 35.f;    // 17 bottom hint distance from screen bottom
    float _mapHintFs        = 34.f;    // 18 bottom hint font size

    // -- In-game HUD debug editor ------------------------------------------
    struct HUDConfig
    {
        // HP / MP bars (0�4)
        float barW         = 534.f;   // 0  bar width
        float barH         = 30.f;    // 1  bar height
        float barGap       = 28.f;    // 2  gap between HP and MP bars
        float barTopPad    = 16.f;    // 3  Y of HP bar from top
        float barLabelFs   = 15.f;    // 4  font inside bars
        // Gold label (5�7)
        float goldX        = 21.f;    // 5  gold X
        float goldY        = 16.f;    // 6  gold Y
        float goldFs       = 48.f;    // 7  gold font size
        // Enemies left (8�10)
        float enemiesX     = 22.f;    // 8  enemies label X
        float enemiesY     = 90.f;    // 9  enemies label Y
        float enemiesFs    = 45.f;    // 10 enemies label font size
        // Act / Room label (11�13)
        float actOffsetX   = 42.f;    // 11 offset from right edge
        float actY         = 30.f;    // 12 act label Y
        float actFs        = 43.f;    // 13 act label font size
        // Minimap (14�22)
        float miniX        = 16.f;    // 14 minimap origin X
        float miniY        = 164.f;   // 15 minimap origin Y
        float miniW        = 271.f;   // 16 minimap width (height auto-derived)
        float miniDotBoss  = 6.f;     // 17 boss dot base radius
        float miniDotElite = 2.f;     // 18 cyclops/ogre dot base radius
        float miniDotEnemy = 0.f;     // 19 regular enemy dot base radius
        float miniDotProp  = 3.f;     // 20 prop dot base radius
        float miniDotPickup= 2.f;     // 21 pickup dot base radius
        float miniDotPlayer= 4.f;     // 22 player dot base radius
        // PC ability bar (23�27)
        float slotSz       = 103.f;   // 23 ability slot size (square)
        float slotGap      = 108.f;    // 24 gap between slots
        float slotBotPad   = 19.f;    // 25 slot Y from bottom
        float slotKeyFs    = 15.f;    // 26 keybind label font size
        float slotNameFs   = 21.f;    // 27 ability name font size
        // Touch buttons (28�38)
        float touchJoyR       = 90.f;   // 28 joystick radius
        float touchAtkR       = 113.f;  // 29 ATK button radius
        float touchDashR      = 111.f;  // 30 DASH button radius
        float touchAtkPadR    = 176.f;  // 31 ATK centre from right
        float touchAtkPadB    = 214.f;  // 32 ATK centre from bottom
        float touchDashOffset = 302.f;  // 33 DASH left-offset from ATK (ATK.x - DASH.x)
        float touchDashBotPad = 214.f;  // 33b DASH centre Y from bottom (-1 = share ATK pad)
        float touchPauseW     = 130.f;  // 34 pause button width
        float touchPauseH     = 67.f;   // 35 pause button height
        float touchPausePad   = 85.f;   // 36 pause button edge padding
        float touchAtkFs      = 46.f;   // 37 ATK button text size
        float touchDashFs     = 57.f;   // 38 DASH button text size
        // Touch ability slots (39�42)
        float touchSlotSz      = 130.f;  // 39 ability slot square size
        float touchSlotGap     = 20.f;   // 40 gap between slots
        float touchSlotRightPad= 27.f;   // 41 right-edge padding for slot row
        float touchSlotYOff    = 29.f;   // 42 gap between slot bottom and ATK top
        // EXP bar (43-45)
        float expBarH    = 27.f;   // 43 height of the EXP bar
        float expBarGap  = 19.f;   // 44 gap between MP bar bottom and EXP bar top
        float expLabelFs = 16.f;   // 45 font size of the level / exp label
        // HP / MP orbs (46-50)
        float hpOrbX     = 513.f;  // 46 HP orb centre X
        float hpOrbY     = 999.f;  // 47 HP orb centre Y
        float mpOrbX     = 1408.f; // 48 MP orb centre X
        float mpOrbY     = 992.f;  // 49 MP orb centre Y
        float statOrbR   = 64.f;   // 50 shared HP/MP orb radius
        // Armour icons (51-53)
        float armourX    = 590.f;  // 51 first armour icon X
        float armourY    = 853.f;  // 52 armour icon Y
        float armourSize = 55.f;   // 53 armour icon height
    };
    HUDConfig _hudCfg;
    bool  _hudEditorActive  = false;
    int   _hudEditorSelIdx  = 0;

    // Per-slot drag offsets (set interactively when HUD editor is open)
    Vector2 _touchSlotOffset[4]{};
    int     _touchSlotDragIdx        = -1;
    Vector2 _touchSlotDragMouseStart{};
    Vector2 _touchSlotDragOffsetStart{};

    // Touch button mapping screen
    // Indices: 0=ATK  1=DASH  2..5=ability slots 0..3
    bool    _touchLayoutCustom   = false;
    Vector2 _touchCustomPos[6]{};      // saved absolute centres (session only)
    Vector2 _touchMappingPos[6]{};     // working positions during mapping
    int     _touchMappingDragIdx  = -1;
    Vector2 _touchMappingDragStart{};
    Vector2 _touchMappingPosAtDrag{};

    // Snapshot of the original baked values � captured once on first mapping screen open.
    // Default always restores to these, regardless of how many saves have been made.
    struct TouchDefaults
    {
        float   atkPadR, atkPadB, dashOffset, dashBotPad;
        Vector2 slotOffset[4];
        Vector2 pos[6];
        bool    captured = false;
    } _touchDefaults;

    // Level-up choice state
    UpgradeType _levelUpOptions[3] = { UpgradeType::AttackPower, UpgradeType::AttackRange, UpgradeType::MaxHealth };
    UpgradeType _levelUpUltimateOptions[3] = { UpgradeType::LearnFireUltimate, UpgradeType::LearnIceUltimate, UpgradeType::LearnElectricUltimate };
    GameState   _levelUpReturnState  = GameState::Play;
    LevelUpOfferContext _levelUpOfferContext = LevelUpOfferContext::NormalLevel;
    bool        _showUltimateRow     = false;
    bool        _ultimateRowPicked   = false;
    bool        _regularRowPicked    = false;
    bool        _eliteRewardGranted  = false;
    UpgradeType _startingAbilityOptions[6] = {
        UpgradeType::LearnFireSpread, UpgradeType::LearnIceSpread, UpgradeType::LearnElectricSpread,
        UpgradeType::LearnFireBolt,   UpgradeType::LearnIceBolt,   UpgradeType::LearnElectricBolt
    };
    bool        _startingAbilitySelected[6] = {};
    int         _startingAbilityPickCount = 0;
    bool        _starterAbilityGiftClaimed = false;

    // -- Elite-room state (all reset in StartNextRoom) ---------------------
    // Active modifier index (matches EliteModifier): 0=Cage, 1=GuardLinks,
    // 2=Enrage, 3=ArenaPressure; -1=none. The legacy Leap modifier is gone —
    // movement is part of each elite's own signature kit now.
    int     _eliteMechanic            = -1;
    Enemy*  _eliteMinibossPtr         = nullptr;  // non-owning ptr into _enemies
    Vector2 _eliteCageCenter          = {};
    float   _eliteCageRadius          = 0.f;
    float   _eliteCageDamageTimer     = 0.f;
    float   _eliteEnrageWarningTimer  = 0.f;
    float   _eliteHazardSpawnTimer    = 0.f;

    // -- Elite constants ---------------------------------------------------
    static constexpr float kEliteCageRadius             = 500.f;
    static constexpr float kEliteCageDamageInterval     = 0.5f;
    static constexpr float kEliteEnrageWarningDuration  = 4.0f;
    static constexpr float kHazardVolleyMinInterval     = 0.55f;
    static constexpr float kHazardVolleyMaxInterval     = 0.95f;
    static constexpr int   kHazardVolleyMinCount        = 3;
    static constexpr int   kHazardVolleyMaxCount        = 6;

    // -- Biome-modifier room state -----------------------------------------
    // Wastelands: warning circles that detonate on the player after a delay
    struct WastelandHazard
    {
        Vector2 pos;
        float   warningTimer;   // counts down; on zero, explodes and sets exploded = true
        float   activeTimer;    // counts down while the sprite is playing
        float   dmgCooldown;    // per-hazard cooldown so the player isn't hit every frame
        bool    exploded;
    };
    std::vector<WastelandHazard> _wastelandHazards;
    float _wastelandHazardTimer = 0.f;

    // Lost City: two long beams that rotate around fixed pivot points
    struct LostCityBeam
    {
        Vector2 center;
        float   angle;          // current angle in radians
        float   rotSpeed;       // rad/s (negative = counter-clockwise)
        float   length;
        float   damageCooldown; // per-beam cooldown so the player isn't hit every frame
    };
    std::vector<LostCityBeam> _lostCityBeams;

    // Sanctuary: debuff zones � player inside one is dash-locked and slowed
    struct SanctuaryZone { Vector2 pos; float radius; };
    std::vector<SanctuaryZone> _sanctuaryZones;

    // Demon Insides: pulsing red overlay timer
    float _demonPulseTimer = 0.f;

    // Biome-modifier constants
    static constexpr float kWastelandHazardInterval  = 2.8f;
    static constexpr float kWastelandWarningDuration = 1.8f;
    static constexpr float kWastelandExplosionRadius = 75.f;
    static constexpr int   kWastelandDamage          = 1;
    static constexpr float kWastelandActiveDuration  = 0.80f;
    static constexpr float kLostCityBeamLength       = 340.f;
    static constexpr float kLostCityBeamWidth        = 10.f;
    static constexpr float kLostCityBeamRotSpeed     = 0.9f;
    static constexpr float kLostCityBeamDmgCooldown  = 0.35f;
    static constexpr float kSanctuaryZoneRadius      = 130.f;
    static constexpr float kSanctuarySlowFactor      = 0.45f;

    // Ability choice state (shown after every 5th-wave boss clear)
    UpgradeType _abilityChoiceOptions[3] = { UpgradeType::LearnFireSpread, UpgradeType::LearnFireSpread, UpgradeType::LearnFireSpread };
    int         _abilityChoiceOptionCount = 0;
    bool        _abilityChoiceSwapPending = false;
    UpgradeType _abilityChoiceSwapTarget  = UpgradeType::AttackPower;
    int         _lastAbilityChoiceWave    = -1;
    float       _abilityChoiceOpenTimer   = 0.f;
    int         _abilityChoiceGpCursor   = 0;
    float       _abilityChoiceGpCooldown = 0.f;
    int         _levelUpGpCursor         = 0;
    int         _levelUpGpRow            = 1;   // 0 = ultimate row, 1 = regular row
    float       _levelUpGpCooldown       = 0.f;

    // Upgrade icon textures (loaded once, never reloaded)
    Texture2D _upgradeAttackPowerTex{};
    Texture2D _upgradeAttackRangeTex{};
    Texture2D _upgradeHealthTex{};
    Texture2D _upgradeMagicTex{};
    Texture2D _upgradeDefenseTex{};
    Texture2D _upgradeMoveSpeedTex{};

    Sound _pickupSound{};
    Sound _fireballCastSound{};
    Sound _explosionSound{};
    Sound _roomClearExplosionSound{};
    Sound _lavaBallImpactSound{};
    Sound _buttonPressSound{};

    Texture2D _map{};
    Texture2D _treeTex{};
    Texture2D _smallTreeTex{};
    Texture2D _rockTex{};
    Texture2D _bigRockTex{};
    Vector2 _mapPos{};
    // Map scale is now computed by _worldConfig � do not set this by hand.
    // Read/write it through _worldConfig.Recalculate() and GetScale().
    float      _mapScale    = 3.f;
    WorldConfig _worldConfig;          // owns all map-scale and camera logic
    int _maxActiveEnemies = 16;

    Texture2D _pillarTex{};
    Texture2D _torchTex{};       // Torch.png � 256x29, 8 frames of 32x29
    Texture2D _pillarTorchTex{}; // PillarTorch.png � 290x52, 8 frames of 32x52 (content offset x=17)
    Texture2D _fireballCastTex{};
    Texture2D _fireballHitTex{};
    Texture2D _genericHitTex{};   // Hit03.png � melee hit splat + electric impact sprite
    Texture2D _iceHitTex{};       // Ice_Shard_Hit.png � ice ability impact
    Texture2D _lightningCastTex{};
    Texture2D _thunderHitTex{};      // electric impact burst (Thunder_Blast.png)
    Texture2D _healEffectTex{};
    Texture2D _roomClearExplosionTex{};

    // Ability card icons for the level-up / starting ability panels
    Texture2D _abilityIconFireTex{};
    Texture2D _abilityIconIceTex{};
    Texture2D _abilityIconElectricTex{};

    Texture2D _shopBorderTex{};
    Texture2D _shopZephTex{};
    Texture2D _htpBorderTex{};   // HowToPlayBorder.png - used for the How To Play content panel
    Texture2D _magicGemTex{};
    Texture2D _bossBarrierTex{};
    Texture2D _swordCursorTex{}; // CursorSword.png - custom sword mouse cursor (game only, not editors)

    // Map node icons (TileSet/MapIcons/)
    Texture2D _mapIconNormal{};
    Texture2D _mapIconElite{};
    Texture2D _mapIconShop{};
    Texture2D _mapIconTreasure{};
    Texture2D _mapIconBoss{};
    Texture2D _mapIconRest{};

    Character _player;
    MainMenu _menu;
    PauseAndGameOver _pauseUI;
    KeyBindings _keybindingsEdit;

    std::vector<Prop> _props;
    std::vector<std::unique_ptr<Pickup>> _pickups;
    enum class UltimatePhase { None, WindUp, Cinematic, Impact, Release };

    struct UltimateBlast
    {
        Vector2     worldPos{};
        AbilityType element  = AbilityType::FireUltimate;
        float       timer    = 0.f;
        float       lifetime = 0.f;
        float       rotation = 0.f;   // initial rotation angle (degrees)
    };

    static constexpr float _ultWindUpDuration    = 0.75f;
    static constexpr float _ultCinematicDuration = 1.5f;
    static constexpr float _ultImpactDuration    = 0.35f;
    static constexpr float _ultReleaseDuration   = 0.5f;

    UltimatePhase _ultimatePhase       = UltimatePhase::None;
    float         _ultimatePhaseTimer  = 0.f;
    float         _ultimateCircleAngle = 0.f;
    AbilityType   _ultimateElement     = AbilityType::FireUltimate;

    std::vector<UltimateBlast>       _ultimateBlasts;
    std::vector<SpreadProjectile>    _spreadProjectiles;
    VFXManager     _vfx;
    DamageNumberManager _damageNumbers;
    std::uint64_t _nextCombatTargetId = 1;
    NavigationGrid _nav;
    ShopManager    _shop;
    std::vector<std::unique_ptr<Enemy>>   _enemies;
    std::vector<CyclopsLaserProjectile>   _cyclopsLasers;
    std::vector<LavaBallProjectile>       _lavaBalls;
    std::vector<EnemyProjectile>          _enemyProjectiles;   // arrows + fire bolts

    // Lingering poison pools (Sporeling deaths, Toxic Vermin spit).
    struct PoisonCloud
    {
        Vector2 pos{};
        float   timer  = 0.f;
        float   radius = 120.f;
    };
    std::vector<PoisonCloud> _poisonClouds;
    float _poisonDamageCooldown = 0.f;

    // Warlock's imp minion — a persistent pet that hunts enemies. Its damage is
    // atk-independent (routes through ScalePlayerHit so it scales with relics),
    // which is how the Warlock's damage identity is fixed.
    struct WarlockMinion
    {
        Vector2 pos{};
        float   attackCooldown = 0.f;
        float   bob = 0.f;
        int     frame = 0;
        float   frameTimer = 0.f;
        float   facing = 1.f;
    };
    std::vector<WarlockMinion> _warlockMinions;
    Texture2D _minionTex{};
    void SpawnWarlockMinion();
    void UpdateWarlockMinions(float dt);
    void DrawWarlockMinions(Vector2 worldOffset) const;
    BossSupportState _bossCyclopsSupport;
    BossSupportState _bossOgreSupport;

    std::string _message = "Objective: Survive";

    Biome _currentBiome = Biome::Caverns;
    Biome _pendingBiome = Biome::Caverns;
    bool  _biomeTransitionActive = false;
    bool  _biomeTransitionSwapped = false;
    float _biomeTransitionTimer = 0.f;
    bool     _demoEndTouchHeld = false;
    bool     _isMainGameRun   = false; // true when started via "Start Game" (not Dungeon Run)
    PrologueController _prologue;
    bool _prologueActive = false;
    bool _firstDeathRevive = false;
    int  _deathReviveDialogueLine = 0;
    bool _firstVillageVisit = false;
    bool _pendingNewRunFromVillage = false;
    float _prologueLastHealth = 0.f;
    bool _villageIntroDialogueActive = false;
    int  _villageIntroDialogueLine = 0;
    AudioManager _audio;
    CombatDirector _combatDirector;
    OverlayRenderer _overlayRenderer;

    // -- Touch mode -----------------------------------------------------------
    bool          _touchModeActive = false;
    TouchControls _touch;
    GamepadInput  _gamepad;
    InputPromptMode _inputPromptMode = InputPromptMode::KeyboardMouse;
    // Touch IDs that have already triggered an ability cast this press.
    // Cleared each frame when the touch lifts; prevents repeat casts on hold.
    std::vector<int> _abilityTapSeenIds;

    DebugPanel      _debug;
    TileMapper      _tileMapper;
    NineSliceEditor _nineSliceEditor;
    CharacterAnimator _charAnimator;
    AttackEditor      _attackEditor;
    MapEditor         _mapEditor;      // village PNG asset metadata adjuster (V key)
    std::vector<Texture2D> _villagePlaygroundSheets;
    std::vector<std::string> _villagePlaygroundSheetNames;
    std::vector<VillageRuntimeObjectDef> _villageObjectCatalog;
    std::vector<VillagePlacedObject> _villagePlacedObjects;
    int _villageActiveObjectIndex = -1;
    float _villageCatalogScroll = 0.f;
    std::string _villagePlaygroundMessage;
    float _villagePlaygroundMessageTimer = 0.f;
    bool _villageShopNpcActive = false;        // a placed object provides the Zeph shop NPC
    bool _villageInsideInterior = false;       // player is inside a building interior
    std::string _villageInteriorName;          // which interior (e.g. "interior_default")
    Vector2 _villageInteriorReturnPos{};       // village world pos restored on exit
    bool _villageBuildMode = false;            // B toggles: build (catalog+ghost) vs walk (doors/NPCs)
    bool _villageSandboxMode = true;           // Y-key tester: no saving, no tutorial gating, free placement
    GameState _classSelectReturnState = GameState::Menu;   // where ESC from ClassSelect goes (Menu or Village)
    std::vector<VillageCitizen> _villageCitizens;
    TileDefSet _villageTileDefs;               // Forest tile assignments for the village field
    Texture2D _villageFieldSheet{};            // Forest.png (biome sheet for the field tiles)
    Texture2D _villageFieldGroundSheet{};      // Ground TIles.png (fromGround tile types)
    Texture2D _villageGraveyardTex{};          // VillageAssets/VillageGraveyard.png
    Texture2D _villageZephShopTex{};           // VillageAssets/ZephsShop.png
    Vector2 _villageGraveyardPoeLocal{ 30.90f, 123.72f };
    Vector2 _villageGraveyardRespawnLocal{ 104.76f, 117.84f };
    std::vector<Rectangle> _villageGraveyardColliders;
    Vector2 _villageZephShopZephLocal{ 50.22f, 124.20f };
    std::vector<Rectangle> _villageZephShopColliders;
    DungeonGen   _dungeonGen;
    TileDefSet   _tileDefs;
    TileRenderer _tileRenderer;
    RoomLibrary  _roomLibrary;
    RoomAssetCatalog _roomAssetCatalog;
    bool         _useHandcraftedDungeonRooms = false;
    bool         _editorPlaytestActive = false; // playtesting a room from the map editor
    RoomBlueprint _editorPlaytestBlueprint{};   // exact in-memory room; never procgen fallback
    int          _editorPlaytestStartRoomIdx = -1;
    std::vector<std::string> _editorPlaytestRoomIds; // stable library assignment per graph node
    std::vector<std::string> _handcraftedDungeonRoomIds; // stable authored room per main-run node
    bool         _editorPlaytestEnemiesOn = true; // live enemy/door toggle in playtest
    void SetPlaytestEnemies(bool enemiesOn);      // respawn/clear enemies + doors live
    std::string  _lastHandcraftedRoomId;
    std::string  _forcedHandcraftedRoomId;
    TileDefSet   _dungeonScrollTileDefs;
    TileRenderer _dungeonScrollTileRenderer;
    struct DungeonVisualVariant
    {
        std::string name;
        std::string sheetStem;
        std::string mapperStem;
        int weight = 1;
        int minWingRooms = 2;
        int maxWingRooms = 4;
    };
    std::vector<DungeonVisualVariant> _dungeonVisualVariants;
    std::vector<int> _dungeonRoomVisualVariants;
    int _currentDungeonVisualVariant = -1;

    // Per-room persistent state tracked during a dungeon run session.
    struct DungeonEnemySnapshot
    {
        std::string type;
        Vector2     pos{};
    };
    // Environmental hazards persist per room: destroyed ones stay destroyed,
    // live ones are rebuilt on re-entry (lava even in cleared rooms — terrain
    // doesn't despawn; turret hazards only while the room still has a fight).
    struct DungeonHazardSnapshot
    {
        int     type = 0;            // RoomHazardType as int
        Vector2 pos{};
        Vector2 fireDir{ 1.f, 0.f };
        float   health = 3.f;
        bool    destroyed = false;
    };
    struct DungeonRoomState
    {
        bool cleared = false;
        bool enemiesInitialized = false;
        bool visited = false;             // player has entered — map remembers the type
        int  eliteMechanic = -1;
        std::vector<DungeonEnemySnapshot> survivors;
        std::vector<DungeonHazardSnapshot> hazards;
        bool hazardsInitialized = false;  // first-visit roll already happened
        float holdoutTimeRemaining = -1.f;

        // Room-events / decision-room state (rolled at generation time, seeded by
        // the dungeon seed, so it never rerolls on re-entry and dailies match).
        RoomSpecialType special      = RoomSpecialType::None;
        bool specialClaimed          = false;   // player chose an option or left
        int  specialChoice           = -1;      // chosen card index, -1 = none/left
        int  specialOptions[3]       = { -1, -1, -1 }; // option-table indices offered
    };

    // ── Dungeon HUD polish (intro banner / badge / modifier stack / map) ───────
    // Normal gameplay HUD only shows exceptions: standard rooms draw no label;
    // special rooms announce themselves with a fading intro banner, then shrink
    // to a compact badge. Active modifiers collect into one top-right pill stack.
    float       _roomIntroTimer = 0.f;        // >0 = intro banner on screen
    std::string _roomIntroTitle;              // big line ("ELITE ENCOUNTER")
    std::string _roomIntroSub;                // condition line ("Room Hazards")
    static constexpr float kRoomIntroDuration = 2.6f;
    void QueueRoomIntroBanner();              // builds title/sub for the room just entered
    void DrawRoomIntroBanner();               // temporary top-center announcement
    void DrawRoomBadge() const;               // compact persistent badge (special rooms only)
    void DrawModifierStack() const;           // top-right pills: affix / wager / future curses
    // Minimap: visited rooms remember their type icon, unvisited show "?"
    // (boss + shop always shown). Cartographer's Echo reveals everything.
    void DrawDungeonMiniMap(float originX, float originY, float cellPx, bool overlay) const;

    // Dungeon run sub-state
    enum class DungeonView { Graph, Room, Play };
    DungeonView _dungeonView          = DungeonView::Graph;
    int        _dungeonRoomIdx = -1;
    RoomLayout _dungeonRoomLayout{};
    RoomCombatCapacity _roomCombatCapacity{};
    EncounterProfile _currentEncounterProfile = EncounterProfile::Skirmish;
    float _roomObjectiveTimer = 0.f;
    bool _roomObjectiveComplete = false;
    Vector2   _dungeonRoomEntrySpawnPos{};
    float     _dungeonFallRecoveryCooldown = 0.f;
    Vector2   _dungeonLastSafePos{};        // last player pos off any pit (edge respawn)
    float     _pitDragTimer = 0.f;          // time feet have hovered a pit (escape window)
    // Direction that drags the player deeper into the nearby pit (0 if none).
    Vector2 PitPullDirection(Vector2 feet, float cellW, float cellH) const;
    void SpawnFallSurfaceFx(Vector2 worldPos);
    // Basic enemies over a pit die instantly (bosses/elites immune).
    void UpdateEnemyPitfalls(float cellW, float cellH);

    enum class DungeonDoorSide { None = -1, North, South, West, East };
    DungeonDoorSide _dungeonEntryDoorSide = DungeonDoorSide::None;

    struct DungeonClearEffect
    {
        Vector2 worldPos{};
        float   timer = 0.f;
        Rectangle glowRect{};
        bool hasGlow = false;
    };
    std::vector<DungeonClearEffect> _dungeonClearEffects;
    std::vector<Vector2> _dungeonPropCentersScratch;
    // Player-made damage zones (ticking _warriorVfx entries) rebuilt each frame
    // and shared with enemies so they steer around Consecrate / poison pools.
    std::vector<HazardZone> _enemyHazardZones;
    void RebuildEnemyHazardZones();
    // Debug overlay: enemy facing arrows + front/rear cones + facing-lock
    // timers. Only drawn while the debug panel is open (never in normal play).
    void DrawEnemyFacingDebug() const;

    // ── Room pressure budget + reinforcement waves (see Balance::Pressure) ────
    // Standard rooms roll their full composition up front, clamp it to the
    // tier's pressure cap, spawn only the opening slice, and queue the surplus
    // as reinforcement waves so big populations stay readable.
    RoomHazardDirector _roomHazards;                 // environmental hazards, current room
    std::deque<EncounterSpawnEntry> _dungeonReinforcements;
    float _dungeonReinforcementTimer = 0.f;
    int   _dungeonOpeningCap  = 7;                   // opening active cap for this room
    int   _roomEncounterTier  = 0;
    int   _roomPressureSpent  = 0;                   // debug readout
    int   _roomPressureCapDbg = 0;                   // debug readout
    // Reserved-but-not-yet-instantiated reinforcement slots (purple-circle /
    // smoke telegraph state machine — see ReinforcementPacing.h). Stays small
    // in practice (at most 1-2 entries per batch, one batch pending at a
    // time), so an unbounded-looking std::vector never actually grows beyond
    // a handful of entries.
    std::vector<PendingEnemySpawn> _pendingEnemySpawns;
    // Spawns one mixed-table grunt by typeId (shared by the room opening and
    // reinforcement waves), including the role-based placement nudge.
    Enemy* SpawnDungeonGrunt(const EncounterSpawnEntry& entry, Vector2 pos, float cellW, float cellH);
    // Reinforcement pacing (Task 4): reserve 1-2 new telegraphed spawns from
    // the front of _dungeonReinforcements when the room is below its
    // simultaneous body target and no batch is already pending; advance every
    // pending spawn's Circle->Smoke->Ready timer and instantiate/requeue at
    // Ready. See ReinforcementPacing.h for the underlying pure policy.
    void TryReserveDungeonReinforcementBatch(float cellW, float cellH);
    void AdvanceDungeonPendingSpawns(float dt, float cellW, float cellH);
    bool IsDungeonReinforcementPosStillValid(Vector2 pos, float cellW, float cellH) const;
    // Draws the procedural purple-circle warning (Circle phase: pulsing filled
    // circle + expanding outline rings) and its contract-and-brighten Smoke
    // phase for every pending reinforcement, in world space, converted to
    // screen space via worldOffset the same way the sibling _vfx.Draw/pickup
    // draws in that block do. Called from the dungeon draw pass, below enemy
    // sprites and above floor tiles (see design's "Spawn Telegraph and VFX").
    void DrawPendingEnemySpawns(Vector2 worldOffset) const;
    // Hazard persistence: snapshot the live hazards into the current room's
    // state and rebuild them on re-entry (lavaOnly = cleared-room terrain).
    void SaveDungeonRoomHazardState();
    void RestoreDungeonRoomHazardState(int roomIdx, bool lavaOnly);

    std::unordered_map<int, DungeonRoomState> _dungeonRoomStates;
    bool  _dungeonEnemiesSpawned  = false;
    float _bossNoEnemyTimer       = 0.f;   // seconds since boss room had 0 active enemies
    // Zeph's Wager: set when the wagered combat room spawns; pays out bonus
    // gold + clear XP in the room-clear block, then resets.
    bool  _shopWagerRoomActive    = false;

    bool _hasMagicGem = false;
    bool _magicGemSpawned = false;
    bool _magicGemCollected = false;
    bool _bossBarrierUnlocked = false;
    float _magicGemAnimTimer = 0.f;
    int   _magicGemFrame = 0;
    float _bossBarrierAnimTimer = 0.f;
    int   _bossBarrierFrame = 0;
    float _bossBarrierMessageTimer = 0.f;
    // Zelda-style room scroll transition
    bool       _dungeonScrolling      = false;
    float      _dungeonScrollT        = 0.f;
    Vector2    _dungeonScrollVec      = {};         // direction current room slides (unit, applied to sw/sh)
    RoomLayout _dungeonScrollNextLayout{};
    int        _dungeonScrollNextIdx  = -1;
    Vector2    _dungeonScrollSpawnPos = {};
    DungeonDoorSide _dungeonScrollNextEntryDoorSide = DungeonDoorSide::None;
    static constexpr float kDungeonScrollDur = 0.40f;

    // Full-screen fade used when entering the dungeon from the Store and after boss clears.
    enum class DungeonFadeState { None, FadingOut, FadingIn };
    DungeonFadeState      _dungeonFadeState  = DungeonFadeState::None;
    float                 _dungeonFadeAlpha  = 0.f;   // 0�255
    float                 _dungeonFadeTimer  = 0.f;
    std::function<void()> _dungeonFadePendingAction;
    static constexpr float kDungeonFadeDuration = 0.40f;

    // Treasure room chest � spawns at screen centre after all enemies die.
    bool _treasureChestSpawned = false;
    bool _treasureChestBroken  = false;

    // Folder scanned by the TileMapper debug tool for PNG tilesets.
    static constexpr const char* kTilesheetFolder = "MapTilesets";

    // Folder scanned by the 9-Slice Editor � top-level PNGs in UI/ only, no subfolders.
    static constexpr const char* kUIFolder = "UI";

    // -- Hitbox debug editor (F12 while debug active) -------------------------
    bool           _isHitboxEditorActive = false;
    BaseCharacter* _hitboxSelectedEntity = nullptr;
    bool           _hitboxEditAttack     = false;
    float          _hitboxNudgeAccum     = 0.f;
    static constexpr float kHitboxNudgeInitDelay  = 0.30f;
    static constexpr float kHitboxNudgeRepeatRate = 0.05f;

    void UpdateHitboxEditor();
    void DrawHitboxEditor();

    // -- Dialogue box designer (F11 while debug active) -----------------------
    // Lets you drag and resize the dialogue panel and text sizes live
    // so you can tweak layout without recompiling.
    bool  _isDlgEditorActive   = false;
    int   _dlgEditorHandle     = -1;   // panel handle being dragged (0-4), -1 = none

    // Font-size drag state (drag the label left/right to shrink/grow)
    bool  _dlgSpeakerFsDrag    = false;
    float _dlgSpeakerFsDragX   = 0.f;
    int   _dlgSpeakerFsDragVal = 0;
    bool  _dlgBodyFsDrag       = false;
    float _dlgBodyFsDragX      = 0.f;
    int   _dlgBodyFsDragVal    = 0;

    // Text-inset drag state (horizontal and vertical padding inside the panel)
    bool  _dlgInsetLeftDrag    = false;
    float _dlgInsetLeftDragX   = 0.f;
    float _dlgInsetLeftDragVal = 0.f;
    bool  _dlgInsetTopDrag     = false;
    float _dlgInsetTopDragX    = 0.f;
    float _dlgInsetTopDragVal  = 0.f;

    void UpdateDialogueBoxEditor();
    void DrawDialogueBoxEditor();

    // -- Cutscene system -------------------------------------------------------
    CutsceneManager _cutscene;
    bool            _cutsceneIntroPlayed = false; // stays true after first-ever intro

    // Lock / unlock the Store room's north exit door tile directly.
    void SetStoreDoorTiles(TileType doorType);

    // World map (biome selection after each boss clear)
    void UpdateWorldMap(float dt);
    void DrawWorldMap();
    void OpenWorldMap();   // generates the map and switches state

    // Dungeon run
    void UpdateDungeonRun(float dt);
    void RollRoomAffix(int roomIdx, RoomType type);  // pick this room's affix (or None)
    void DrawRoomAffixBanner();                       // HUD banner when an affix is active
    void UpdateDreamFlicker(float dt);
    void InitBiomeModifierRoom();
    void UpdateBiomeModifiers(float dt);
    void DrawBiomeModifiers();
    void DrawDungeonRun();
    Rectangle GetDungeonRoomRect(int roomIdx) const;
    DungeonDoorSide GetBossBarrierSide() const;
    Rectangle GetBossBarrierRect(DungeonDoorSide side) const;
    Vector2 GetMagicGemWorldPos() const;
    void UpdateDungeonMagicGemAndBarrier(float dt);
    void DrawDungeonMagicGemAndBarrier() const;
    void DrawMagicGemHudIcon() const;

    // Dungeon run combat helpers
    Vector2   GetDungeonSpawnPos(float cellW, float cellH) const;
    std::vector<Rectangle> GetDungeonSpawnBlockers(float cellW, float cellH) const;
    Rectangle GetDungeonEnemySpawnBody(const Enemy* enemy, Vector2 worldPos,
                                       float cellW, float cellH) const;
    bool IsDungeonEnemySpawnPositionValid(const Enemy* enemy, Vector2 worldPos,
                                          float cellW, float cellH,
                                          const std::vector<Rectangle>& blockers) const;
    bool TryFindDungeonEnemySpawnPosition(const Enemy* enemy, Vector2 desiredPos,
                                          float cellW, float cellH,
                                          float minPlayerDistance,
                                          Vector2& result) const;
    // Role-aware placement: picks a valid spawn biased for the enemy's role
    // (ranged in back, tanks mid, assassins off-angle). See EnemyRole.
    Vector2   GetDungeonSpawnPosForRole(EnemyRole role, float cellW, float cellH) const;
    void      SpawnDungeonRoomEnemies();
    void      ClearDungeonEnemies();
    void      SaveDungeonRoomEnemyState();
    bool      RestoreDungeonRoomEnemyState(int roomIdx);
    Enemy*    SpawnDungeonSnapshotEnemy(const DungeonEnemySnapshot& snapshot);
    std::string GetDungeonSnapshotType(Enemy& enemy) const;
    void      ResetEliteRoomRuntime();
    // ONE elite-room setup path: debug restarts, fresh dungeon rooms, room
    // re-entry and snapshot restore all initialize the modifier here so no
    // path can drift. Call AFTER every enemy for the room has been spawned
    // (Guard Links counts the living guards).
    void      InitializeEliteRoomRuntime(Enemy* elite, int roomIdx,
                                         float worldWidth, float worldHeight);
    // Rolls (or restores) a modifier COMPATIBLE with this elite archetype.
    int       GetCompatibleEliteMechanicForRoom(int roomIdx, EliteArchetype archetype);

    // Door state helpers for tile-dungeon rooms.
    void ApplyDungeonRoomDoorState(RoomLayout& layout, int roomIdx, DungeonDoorSide entryDoorSide) const;
    bool IsDungeonDoorOpen(DungeonDoorSide side) const;
    DungeonDoorSide OppositeDungeonDoorSide(int dr, int dc) const;
    void SpawnDungeonDoorOpenEffects();
    void UpdateDungeonClearEffects(float dt);
    void DrawDungeonClearEffects() const;

    // Boss room exit door - sets tiles in _dungeonRoomLayout and returns the trigger rect.
    void      ApplyDungeonBossExitTiles(TileType doorType);
    Rectangle GetDungeonBossExitTrigger() const;

    // Rebuilds the nav grid for the current dungeon room layout, including wall tiles.
    void RebuildDungeonNav();

    // Resolves all active enemies out of tile walls and props. Ends forced pushes on impact.
    void ResolveDungeonEnemyCollisions();
};
