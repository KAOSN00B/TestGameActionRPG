#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// VillageAssetData — the runtime meaning of a village piece.
//
// A village asset is an authored PNG (VillageAssets/<Name>.png) plus a readable
// ".vasset" sidecar that tells the game what the piece IS and DOES: its category,
// the service it provides, what it costs, whether it is required/unique, where it
// blocks the player (colliders), where NPCs stand (markers), and where the player
// can press interact (interaction zones). Doors and ambient-NPC spawns are also
// parsed so future systems can read them, but nothing consumes those yet.
//
// This module is intentionally ENGINE-AGNOSTIC: it does not depend on raylib, so
// it can be unit-tested on its own. It uses the tiny VaVec2 / VaRect value types
// below. When this header is included AFTER raylib.h, inline converters to
// Vector2 / Rectangle become available (see the RAYLIB_H block at the bottom).
//
// The ".vasset" format is line-based and versioned ("village_asset 1"). Every
// field beyond the original size/collider/marker set is OPTIONAL and additive,
// and unknown lines are skipped — so old files keep loading and future fields do
// not break older builds.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

// Minimal geometry types so this module needs no engine headers.
struct VaVec2 { float x = 0.f; float y = 0.f; };
struct VaRect { float x = 0.f; float y = 0.f; float w = 0.f; float h = 0.f; };

// How the build catalog groups a piece. ("Essential" from the design brief is
// expressed by the separate `required` flag, not as a category.)
enum class VillageBuildCategory { Building, Decor, Path, Utility, Trophy };

// The gameplay service a placed asset unlocks. The full set is defined up front
// so future service dispatch is a clean switch; only Shop and Graveyard are
// wired into gameplay today.
enum class VillageService
{
    None, Shop, Graveyard, Training, ClassChange,
    Wardrobe, Cartographer, TrophyHall, DungeonGate, Relic
};

// What a press-interact zone does when the player triggers it.
enum class VillageInteractionType
{
    None, Shop, Talk, Enter, Train, ChangeClass,
    Wardrobe, Inspect, StartRun
};

// The meaning of a marker point. Generic markers carry their meaning in `name`;
// the typed values let assets reuse a generic anchor with an explicit id.
enum class VillageMarkerType
{
    Generic, Zeph, Poe, Respawn, Door, Interact,
    NPCSpawn, AmbientNpcSpawn, PlayerSpawn
};

// A named point in image-local pixels. Markers MAY sit outside the PNG bounds
// (an NPC standing in front of / beside a building) — the saved position is an
// offset from the asset's top-left origin.
struct VAssetMarker
{
    std::string       name;                                  // authored name, e.g. "Zeph"
    VillageMarkerType type = VillageMarkerType::Generic;     // inferred from name or explicit type=
    std::string       npcId;                                 // optional id= (which NPC stands here)
    VaVec2            localPos;                               // image-local pixels
};

// A press-interact zone in image-local pixels.
struct VAssetInteraction
{
    VaRect                 localRect;
    VillageInteractionType type = VillageInteractionType::None;
    std::string            prompt;   // shown to the player, e.g. "Browse wares"
    std::string            target;   // optional, e.g. "shop_zeph"
};

// A door / entrance zone. Parsed but not yet consumed by gameplay.
struct VAssetDoor
{
    VaRect      localRect;
    std::string targetMap;              // e.g. "interior_default"
    std::string targetSpawn;            // spawn marker name in the target
    bool        blocksWhenClosed = true;
    std::string prompt;                 // e.g. "Enter"
};

// A possible ambient-NPC origin. Parsed but not yet consumed by gameplay.
struct VAssetAmbientSpawn
{
    VaVec2      localPos;
    std::string group;              // Villager, Visitor, Spirit, Merchant, Scholar
    int         maxCount = 1;
    std::string unlockKey;          // optional gate, e.g. "build_zeph_shop"
};

// Optional spritesheet animation for assets like water, torches, smoke, etc.
// The image is split into columns x rows frames; colliders/markers are authored
// against one frame, not the entire sheet.
struct VAssetAnimation
{
    bool  enabled = false;
    int   columns = 1;
    int   rows = 1;
    int   frameCount = 1;
    float fps = 6.f;
};

// The fully parsed asset. `imageSize` is the PNG size as authored; all colliders,
// markers and zones are image-local pixels relative to the asset's top-left.
struct VillageAssetData
{
    std::string id;                 // stable id (default = file stem)
    std::string displayName;        // UI name  (default = id)
    std::string imageFile;          // e.g. "ZephsShop.png"
    std::string sourcePath;         // full path the data was loaded from
    VaVec2      imageSize;

    VillageBuildCategory category = VillageBuildCategory::Building;
    VillageService       service  = VillageService::None;
    int  cost = 0;
    bool required  = false;         // cannot be skipped in the real village
    bool unique    = false;         // only one allowed in the real village
    bool removable = true;          // may be removed after placement

    std::vector<VaRect>             colliders;
    std::vector<VAssetMarker>       markers;
    std::vector<VAssetInteraction>  interactions;
    std::vector<VAssetDoor>         doors;          // parsed, not yet consumed
    std::vector<VAssetAmbientSpawn> ambientSpawns;  // parsed, not yet consumed
    VAssetAnimation                 animation;      // optional animated sheet info

    // ── Lookups ──────────────────────────────────────────────────────────────
    const VAssetMarker* FindMarker(const char* markerName) const;
    const VAssetMarker* FindMarkerByType(VillageMarkerType markerType) const;
    bool HasService() const { return service != VillageService::None; }

    // ── World-space helpers ────────────────────────────────────────────────────
    // Given the placed asset's top-left world origin and the village draw scale
    // (markers/colliders are authored in PNG pixels; the village draws assets
    // scaled up), these return world-space geometry ready for collision /
    // interaction / NPC placement.
    VaRect ImageWorldRect(VaVec2 worldOrigin, float scale = 1.f) const;
    VaRect ColliderWorldRect(int index, VaVec2 worldOrigin, float scale = 1.f) const;
    VaVec2 MarkerWorldPos(const VAssetMarker& marker, VaVec2 worldOrigin, float scale = 1.f) const;
    VaRect InteractionWorldRect(int index, VaVec2 worldOrigin, float scale = 1.f) const;
};

namespace VillageAssetLoader
{
    // Parses one ".vasset" file. Returns false if the file cannot be opened or
    // is not a village_asset file. Unknown lines are skipped.
    bool Load(const std::string& vassetPath, VillageAssetData& outData);

    // Scans a folder for "*.vasset" files and loads each. Requires C++17
    // <filesystem>. Engine code that already enumerates the folder (e.g. via
    // raylib's LoadDirectoryFiles) can instead call Load() per file and skip
    // this entirely.
    std::vector<VillageAssetData> LoadCatalog(const std::string& folder);

    // Writes one ".vasset" file in the format Load() parses. Atomic: writes to
    // "<vassetPath>.tmp" then replaces the destination, so a crash mid-write (or
    // the running game reading the file concurrently) can never see a partial
    // file. Returns false if the temp file could not be written or the replace
    // failed.
    bool Save(const std::string& vassetPath, const VillageAssetData& data);

    // String <-> enum helpers (also used by the self-test's readable output).
    const char* ToString(VillageBuildCategory category);
    const char* ToString(VillageService service);
    const char* ToString(VillageInteractionType interactionType);
    const char* ToString(VillageMarkerType markerType);
}

// ── Optional raylib interop ──────────────────────────────────────────────────
// Include this header after raylib.h to get zero-cost converters. Kept behind a
// guard so the core module never depends on raylib.
#ifdef RAYLIB_H
inline Vector2   VaToRaylib(VaVec2 v) { return Vector2{ v.x, v.y }; }
inline Rectangle VaToRaylib(VaRect r) { return Rectangle{ r.x, r.y, r.w, r.h }; }
#endif
