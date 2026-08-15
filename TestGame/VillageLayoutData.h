#pragma once

#include "raylib.h"

#include <string>
#include <vector>

struct VillageLayoutObject
{
    std::string assetName;
    Vector2 worldOrigin{};
    float scale = 1.f;
    bool permanent = false;
};

struct VillageLayout
{
    int width = 1920;
    int height = 1080;
    std::string spawnAssetName = "VillageGraveyard";
    std::string spawnMarkerName = "Respawn";
    Rectangle exitRect{ 870.f, 40.f, 180.f, 90.f };
    std::vector<VillageLayoutObject> objects;
};

class VillageLayoutLoader
{
public:
    static VillageLayout Load(const std::string& path);
    static VillageLayout Fallback();
    static Vector2 LocalToWorld(const VillageLayoutObject& object, Vector2 localPoint);
    static Rectangle LocalToWorld(const VillageLayoutObject& object, Rectangle localRect);

    // Writes the same grammar Load() parses. Atomic: writes to "<path>.tmp" then
    // replaces the destination, so a save on every place/remove click can never
    // leave a half-written file behind. Returns false if the temp file could
    // not be written or the replace failed.
    static bool Save(const std::string& path, const VillageLayout& layout);
};
