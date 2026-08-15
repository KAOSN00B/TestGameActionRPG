#pragma warning(disable: 4996)  // fopen/fprintf are safe here; path is an internal constant

#include "MetaProgression.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* kMetaProgressPath = "metaprogress.cfg";

// ─────────────────────────────────────────────────────────────────────────────
// Unlock table — order must match the MetaUnlockType enum exactly.
// Save keys are derived from the enum index, so NEVER reorder existing entries;
// only append before Count.
// ─────────────────────────────────────────────────────────────────────────────
static const MetaUnlockInfo kUnlockTable[(int)MetaUnlockType::Count] = {
    // name                    description                                    cost  prerequisite
    { "Fireball",            "Adds Fireball to the\nability pool",             25, MetaUnlockType::Count },
    { "Ice Lance",           "Adds Ice Lance to the\nability pool",             25, MetaUnlockType::Count },
    { "Chain Lightning",     "Adds Chain Lightning to\nthe ability pool",        25, MetaUnlockType::Count },
    { "Meteor",              "Adds Meteor to the\nability pool",              50, MetaUnlockType::Count },
    { "Blizzard",            "Adds Blizzard to the\nability pool",              50, MetaUnlockType::Count },
    { "Thunderstorm",        "Adds Thunderstorm to the\nability pool",             50, MetaUnlockType::Count },
    { "Zeph's Stipend I",    "Start every run with\n+30 gold",                 20, MetaUnlockType::Count },
    { "Zeph's Stipend II",   "Start every run with\n+60 gold",                 40, MetaUnlockType::StartingGold1 },
    { "Zeph's Stipend III",  "Start every run with\n+120 gold",                80, MetaUnlockType::StartingGold2 },
    { "Deep Pockets I",      "Keep 20% of your gold\nwhen you die",            35, MetaUnlockType::Count },
    { "Deep Pockets II",     "Keep 40% of your gold\nwhen you die",            70, MetaUnlockType::GoldRetention1 },
    { "Deep Pockets III",    "Keep 60% of your gold\nwhen you die",           140, MetaUnlockType::GoldRetention2 },
    { "Mana Flow I",         "+50% mana regeneration\nevery run",              30, MetaUnlockType::Count },
    { "Mana Flow II",        "+100% mana regeneration\nevery run",             60, MetaUnlockType::ManaFlow1 },
    { "Vitality I",          "+2 max HP at the start\nof every run",           40, MetaUnlockType::Count },
    { "Vitality II",         "+4 max HP at the start\nof every run",           80, MetaUnlockType::Vitality1 },
    { "Arcane Attunement",   "Unlock a 5th ability\nslot every run",          100, MetaUnlockType::Count },
    { "Ability Mastery",     "Unlock a 6th ability\nslot every run",          200, MetaUnlockType::FifthAbilitySlot },
    { "Bulwark",             "+1 armour at the start\nof every run",           60, MetaUnlockType::Count },
    { "Heirloom",            "Start each run with a\nrandom relic",           120, MetaUnlockType::Count },
    { "Echo Surge",          "+50% Echoes from\nevery kill",             90, MetaUnlockType::Count },
    { "Second Wind",         "Revive once per run\nat 40% health",           160, MetaUnlockType::Count },
    { "Cartographer's Echo", "The dungeon map reveals\nevery room's type",    75, MetaUnlockType::Count },
};

const MetaUnlockInfo& GetMetaUnlockInfo(MetaUnlockType type)
{
    return kUnlockTable[(int)type];
}

// ── Load ──────────────────────────────────────────────────────────────────────
void MetaProgressionManager::Load()
{
#ifdef PLATFORM_WEB
    return;  // no persistent file system on web; meta progress resets each session
#else
    FILE* f = fopen(kMetaProgressPath, "r");
    if (!f) return;

    char key[64], val[64];
    while (fscanf(f, " %63[^=]=%63s", key, val) == 2)
    {
        if      (strcmp(key, "banked_cells") == 0)   _bankedCells   = atoi(val);
        else if (strcmp(key, "lifetime_cells") == 0) _lifetimeCells = atoi(val);
        else if (strcmp(key, "gold_carryover") == 0) _goldCarryover = atoi(val);
        else if (strcmp(key, "ascension_sel") == 0)  _selectedAscension = atoi(val);
        else if (strcmp(key, "ascension_max") == 0)  _maxAscensionUnlocked = atoi(val);
        else if (strcmp(key, "onboarding_complete") == 0) _onboardingComplete = (atoi(val) != 0);
        else if (strcmp(key, "rogue_unlocked") == 0)      _rogueUnlocked = (atoi(val) != 0);
        else if (strcmp(key, "warlock_unlocked") == 0)    _warlockUnlocked = (atoi(val) != 0);
        else if (strcmp(key, "game_completed") == 0)      _gameCompleted = (atoi(val) != 0);
        else if (strncmp(key, "unlock_", 7) == 0)
        {
            int index = atoi(key + 7);
            if (index >= 0 && index < (int)MetaUnlockType::Count)
                _unlocked[index] = (atoi(val) != 0);
        }
    }
    fclose(f);

    if (_bankedCells < 0)   _bankedCells = 0;
    if (_lifetimeCells < 0) _lifetimeCells = 0;
    if (_goldCarryover < 0) _goldCarryover = 0;
    if (_maxAscensionUnlocked < 0) _maxAscensionUnlocked = 0;
    if (_maxAscensionUnlocked > kMaxAscension) _maxAscensionUnlocked = kMaxAscension;
    if (_selectedAscension < 0) _selectedAscension = 0;
    if (_selectedAscension > _maxAscensionUnlocked) _selectedAscension = _maxAscensionUnlocked;
#endif
}

// ── Save ──────────────────────────────────────────────────────────────────────
void MetaProgressionManager::Save() const
{
#ifdef PLATFORM_WEB
    return;
#else
    FILE* f = fopen(kMetaProgressPath, "w");
    if (!f) return;

    fprintf(f, "banked_cells=%d\n",   _bankedCells);
    fprintf(f, "lifetime_cells=%d\n", _lifetimeCells);
    fprintf(f, "gold_carryover=%d\n", _goldCarryover);
    fprintf(f, "ascension_sel=%d\n",  _selectedAscension);
    fprintf(f, "ascension_max=%d\n",  _maxAscensionUnlocked);
    fprintf(f, "onboarding_complete=%d\n", _onboardingComplete ? 1 : 0);
    fprintf(f, "rogue_unlocked=%d\n", _rogueUnlocked ? 1 : 0);
    fprintf(f, "warlock_unlocked=%d\n", _warlockUnlocked ? 1 : 0);
    fprintf(f, "game_completed=%d\n", _gameCompleted ? 1 : 0);
    for (int i = 0; i < (int)MetaUnlockType::Count; i++)
        if (_unlocked[i])
            fprintf(f, "unlock_%d=1\n", i);
    fclose(f);
#endif
}

// ── Ascension ───────────────────────────────────────────────────────────────
void MetaProgressionManager::SetSelectedAscension(int tier)
{
    if (tier < 0) tier = 0;
    if (tier > _maxAscensionUnlocked) tier = _maxAscensionUnlocked;
    _selectedAscension = tier;
    Save();
}

void MetaProgressionManager::RecordAscensionCleared(int tier)
{
    // Winning at a tier unlocks the next one.
    int newMax = tier + 1;
    if (newMax > kMaxAscension) newMax = kMaxAscension;
    if (newMax > _maxAscensionUnlocked)
    {
        _maxAscensionUnlocked = newMax;
        Save();
    }
}

void MetaProgressionManager::SetOnboardingComplete(bool complete)
{
    if (_onboardingComplete == complete) return;
    _onboardingComplete = complete;
    Save();
}

void MetaProgressionManager::SetRogueUnlocked(bool unlocked)
{
    if (_rogueUnlocked == unlocked) return;
    _rogueUnlocked = unlocked;
    Save();
}

void MetaProgressionManager::SetWarlockUnlocked(bool unlocked)
{
    if (_warlockUnlocked == unlocked) return;
    _warlockUnlocked = unlocked;
    Save();
}

void MetaProgressionManager::RecordGameCompleted()
{
    if (_gameCompleted) return;
    _gameCompleted = true;
    Save();
}

// ── Banking ───────────────────────────────────────────────────────────────────
void MetaProgressionManager::BankCells(int amount)
{
    if (amount <= 0) return;
    _bankedCells   += amount;
    _lifetimeCells += amount;
    Save();
}

// ── Unlock purchases ──────────────────────────────────────────────────────────
bool MetaProgressionManager::IsUnlocked(MetaUnlockType type) const
{
    if (type == MetaUnlockType::Count) return true;   // "no prerequisite" sentinel
    return _unlocked[(int)type];
}

bool MetaProgressionManager::CanPurchase(MetaUnlockType type) const
{
    if (type == MetaUnlockType::Count) return false;
    if (_unlocked[(int)type]) return false;

    const MetaUnlockInfo& info = kUnlockTable[(int)type];
    if (!IsUnlocked(info.prerequisite)) return false;
    return _bankedCells >= info.cost;
}

bool MetaProgressionManager::Purchase(MetaUnlockType type)
{
    if (!CanPurchase(type)) return false;

    _bankedCells -= kUnlockTable[(int)type].cost;
    _unlocked[(int)type] = true;
    Save();
    return true;
}

// ── Ability pool gating ───────────────────────────────────────────────────────
bool MetaProgressionManager::IsAbilityUnlocked(AbilityType type) const
{
    switch (type)
    {
    case AbilityType::FireBolt:         return IsUnlocked(MetaUnlockType::UnlockFireBolt);
    case AbilityType::IceBolt:          return IsUnlocked(MetaUnlockType::UnlockIceBolt);
    case AbilityType::ElectricBolt:     return IsUnlocked(MetaUnlockType::UnlockElectricBolt);
    case AbilityType::FireUltimate:     return IsUnlocked(MetaUnlockType::UnlockFireUltimate);
    case AbilityType::IceUltimate:      return IsUnlocked(MetaUnlockType::UnlockIceUltimate);
    case AbilityType::ElectricUltimate: return IsUnlocked(MetaUnlockType::UnlockElectricUltimate);
    default:                            return true;   // spreads always available
    }
}

// ── Permanent bonus queries ───────────────────────────────────────────────────
int MetaProgressionManager::GetStartingGoldBonus() const
{
    if (IsUnlocked(MetaUnlockType::StartingGold3)) return 120;
    if (IsUnlocked(MetaUnlockType::StartingGold2)) return 60;
    if (IsUnlocked(MetaUnlockType::StartingGold1)) return 30;
    return 0;
}

float MetaProgressionManager::GetGoldRetentionPercent() const
{
    if (IsUnlocked(MetaUnlockType::GoldRetention3)) return 0.6f;
    if (IsUnlocked(MetaUnlockType::GoldRetention2)) return 0.4f;
    if (IsUnlocked(MetaUnlockType::GoldRetention1)) return 0.2f;
    return 0.f;
}

float MetaProgressionManager::GetManaRegenMultiplier() const
{
    if (IsUnlocked(MetaUnlockType::ManaFlow2)) return 2.0f;
    if (IsUnlocked(MetaUnlockType::ManaFlow1)) return 1.5f;
    return 1.0f;
}

int MetaProgressionManager::GetVitalityBonus() const
{
    if (IsUnlocked(MetaUnlockType::Vitality2)) return 4;
    if (IsUnlocked(MetaUnlockType::Vitality1)) return 2;
    return 0;
}

// ── Gold carryover across a death ─────────────────────────────────────────────
void MetaProgressionManager::SetGoldCarryover(int gold)
{
    if (gold < 0) gold = 0;
    _goldCarryover = gold;
    Save();
}

int MetaProgressionManager::TakeGoldCarryover()
{
    int gold = _goldCarryover;
    if (gold != 0)
    {
        _goldCarryover = 0;
        Save();
    }
    return gold;
}
