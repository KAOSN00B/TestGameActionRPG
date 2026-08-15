#include "VillageLayoutData.h"

#include <cassert>
#include <cmath>

namespace
{
    bool Near(float lhs, float rhs)
    {
        return std::fabs(lhs - rhs) < 0.01f;
    }
}

int main()
{
    VillageLayout layout = VillageLayoutLoader::Load("VillageAssets/VillageLayout.vlayout");
    assert(layout.width == 1920);
    assert(layout.height == 1080);
    assert(layout.objects.size() == 3);
    assert(layout.objects[0].assetName == "VillageGraveyard");
    assert(layout.objects[1].assetName == "ZephsShop");
    assert(layout.objects[2].assetName == "VillageGate");
    assert(layout.objects[0].permanent);
    assert(layout.objects[1].permanent);
    assert(layout.objects[2].permanent);
    assert(layout.spawnAssetName == "VillageGraveyard");
    assert(layout.spawnMarkerName == "Respawn");

    Vector2 markerWorld = VillageLayoutLoader::LocalToWorld(
        layout.objects[0], Vector2{ 104.76f, 117.84f });
    assert(Near(markerWorld.x, 519.52f));
    assert(Near(markerWorld.y, 425.68f));

    VillageLayout fallback = VillageLayoutLoader::Load("VillageAssets/does-not-exist.vlayout");
    assert(fallback.objects.size() == 3);
    assert(fallback.objects[0].assetName == "VillageGraveyard");
    assert(fallback.objects[1].assetName == "ZephsShop");
    assert(fallback.objects[2].assetName == "VillageGate");
    return 0;
}
