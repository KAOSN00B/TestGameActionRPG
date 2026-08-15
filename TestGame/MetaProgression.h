#pragma once

#include "AbilityType.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// META PROGRESSION — Mystic Onslaught (Dead Cells style)
//
// Enemies drop Mystic Cells (a separate currency from gold). The player carries
// them during the run; they are BANKED automatically when reaching Zeph's store
// room at the start of each zone. Dying loses all carried (unbanked) cells.
//
// Banked cells are spent at the Poe's Altar (in Zeph's store room) on
// PERMANENT unlocks that persist across runs and deaths:
//   - New abilities added to the run's ability pool (Bolts, Ultimates)
//   - Starting gold, gold retention on death, mana flow, vitality,
//     and a fifth ability slot
//
// Everything persists in metaprogress.cfg next to the executable.
// ─────────────────────────────────────────────────────────────────────────────

enum class MetaUnlockType
{
    // ── Ability unlocks — add the ability to the run's offer pools ────────────
    UnlockFireBolt,
    UnlockIceBolt,
    UnlockElectricBolt,
    UnlockFireUltimate,
    UnlockIceUltimate,
    UnlockElectricUltimate,
    // ── Permanent run bonuses (tiered ones require the previous tier) ─────────
    StartingGold1,      // +30 starting gold
    StartingGold2,      // +30 more (total +60)
    StartingGold3,      // +60 more (total +120)
    GoldRetention1,     // keep 20% of gold when you die
    GoldRetention2,     // keep 40%
    GoldRetention3,     // keep 60%
    ManaFlow1,          // +50% mana regeneration
    ManaFlow2,          // +100% mana regeneration
    Vitality1,          // +2 max HP at run start
    Vitality2,          // +2 more max HP (total +4)
    FifthAbilitySlot,   // unlock a 5th ability slot
    // ── Deeper / transformative unlocks ──────────────────────────────────────
    SixthAbilitySlot,   // unlock a 6th ability slot (needs the 5th)
    Bulwark,            // +1 armour at run start
    Heirloom,           // start each run with a random relic
    CellSurge,          // +50% Mystic Cells from kills
    SecondWind,         // revive once per run at 40% HP
    Cartographer,       // dungeon map reveals every room's type without visiting
    Count               // keep last
};

struct MetaUnlockInfo
{
    const char*    name;
    const char*    description;
    int            cost;          // banked cells required
    MetaUnlockType prerequisite;  // must own this first (Count = none)
};

// Static info for every unlock, indexed by (int)MetaUnlockType.
const MetaUnlockInfo& GetMetaUnlockInfo(MetaUnlockType type);

class MetaProgressionManager
{
public:
    void Load();
    void Save() const;

    // ── Banked cells ──────────────────────────────────────────────────────────
    int  GetBankedCells() const { return _bankedCells; }
    void BankCells(int amount);

    // ── Unlock purchases (Poe's Altar) ───────────────────────────────────────
    bool IsUnlocked(MetaUnlockType type) const;
    bool CanPurchase(MetaUnlockType type) const;   // affordable + prerequisite owned + not owned
    bool Purchase(MetaUnlockType type);            // deducts cells and saves on success

    // ── Ability pool gating ───────────────────────────────────────────────────
    // Spreads are always available; Bolts and Ultimates need their unlock.
    bool IsAbilityUnlocked(AbilityType type) const;

    // ── Permanent bonus queries (applied by Engine at run start) ─────────────
    int   GetStartingGoldBonus() const;
    float GetGoldRetentionPercent() const;   // 0.0 - 0.6
    float GetManaRegenMultiplier() const;    // 1.0 / 1.5 / 2.0
    int   GetVitalityBonus() const;          // 0 / 2 / 4 max HP
    bool  HasFifthAbilitySlot() const { return IsUnlocked(MetaUnlockType::FifthAbilitySlot); }
    bool  HasSixthAbilitySlot() const { return IsUnlocked(MetaUnlockType::SixthAbilitySlot); }
    int   GetStartingArmourBonus() const { return IsUnlocked(MetaUnlockType::Bulwark) ? 1 : 0; }
    bool  HasStartingRelic() const { return IsUnlocked(MetaUnlockType::Heirloom); }
    float GetCellGainMultiplier() const { return IsUnlocked(MetaUnlockType::CellSurge) ? 1.5f : 1.0f; }
    bool  HasSecondWind() const { return IsUnlocked(MetaUnlockType::SecondWind); }
    bool  HasCartographer() const { return IsUnlocked(MetaUnlockType::Cartographer); }

    // ── Gold retention across a death ─────────────────────────────────────────
    // Engine sets this when the player dies; the next run start consumes it.
    void SetGoldCarryover(int gold);
    int  TakeGoldCarryover();

    // Total cells banked over the profile's lifetime (stat for the altar screen).
    int GetLifetimeCells() const { return _lifetimeCells; }

    // ── Ascension (difficulty tiers unlocked by winning) ─────────────────────
    static constexpr int kMaxAscension = 6;
    int  GetSelectedAscension() const { return _selectedAscension; }
    int  GetMaxAscensionUnlocked() const { return _maxAscensionUnlocked; }
    void SetSelectedAscension(int tier);          // clamps to [0, maxUnlocked]; saves
    void RecordAscensionCleared(int tier);        // unlocks tier+1 on a win; saves

    // Story onboarding and class roster progression. Missing keys in older
    // profile files load as false, preserving backwards compatibility.
    bool HasCompletedOnboarding() const { return _onboardingComplete; }
    void SetOnboardingComplete(bool complete = true);
    bool IsRogueUnlocked() const { return _rogueUnlocked; }
    bool IsWarlockUnlocked() const { return _warlockUnlocked; }
    bool HasCompletedGame() const { return _gameCompleted; }
    void SetRogueUnlocked(bool unlocked = true);
    void SetWarlockUnlocked(bool unlocked = true);
    void RecordGameCompleted();

private:
    int  _bankedCells   = 0;
    int  _lifetimeCells = 0;
    int  _goldCarryover = 0;
    bool _unlocked[(int)MetaUnlockType::Count] = {};
    int  _selectedAscension    = 0;   // difficulty chosen for the next run
    int  _maxAscensionUnlocked = 0;   // highest tier the player may select
    bool _onboardingComplete = false;
    bool _rogueUnlocked = false;
    bool _warlockUnlocked = false;
    bool _gameCompleted = false;
};
