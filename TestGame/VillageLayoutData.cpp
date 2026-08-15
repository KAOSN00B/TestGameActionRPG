#include "VillageLayoutData.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall MoveFileExW(
    const wchar_t* existingName, const wchar_t* newName, unsigned long flags);
namespace
{
    constexpr unsigned long kMoveFileReplaceExisting = 0x1;
    constexpr unsigned long kMoveFileWriteThrough = 0x8;
}
#endif

namespace
{
    bool ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
#ifdef _WIN32
        return MoveFileExW(source.c_str(), destination.c_str(),
                           kMoveFileReplaceExisting | kMoveFileWriteThrough) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(source, destination, ec);
        return !ec;
#endif
    }
}

VillageLayout VillageLayoutLoader::Fallback()
{
    VillageLayout layout;
    layout.objects.push_back({ "VillageGraveyard", { 310.f, 190.f }, 2.f, true });
    layout.objects.push_back({ "ZephsShop", { 1190.f, 210.f }, 2.f, true });
    // Fixed north-wall gate — worldOrigin matches Engine.cpp's VillageGateWorldRect()
    // (col 18, row 0 at the 48px village grid) so the visible object lines up
    // exactly with the invisible trigger rect that still owns the actual logic.
    layout.objects.push_back({ "VillageGate", { 864.f, 0.f }, 1.f, true });
    return layout;
}

VillageLayout VillageLayoutLoader::Load(const std::string& path)
{
    std::ifstream file(path);
    if (!file)
        return Fallback();

    VillageLayout layout;
    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream stream(line);
        std::string command;
        stream >> command;
        if (command.empty() || command[0] == '#')
            continue;

        if (command == "size")
        {
            int width = 0;
            int height = 0;
            if (stream >> width >> height && width > 0 && height > 0)
            {
                layout.width = width;
                layout.height = height;
            }
        }
        else if (command == "spawn")
        {
            std::string asset;
            std::string marker;
            if (stream >> asset >> marker)
            {
                layout.spawnAssetName = asset;
                layout.spawnMarkerName = marker;
            }
        }
        else if (command == "exit")
        {
            Rectangle exit{};
            if (stream >> exit.x >> exit.y >> exit.width >> exit.height && exit.width > 0.f && exit.height > 0.f)
                layout.exitRect = exit;
        }
        else if (command == "object")
        {
            VillageLayoutObject object;
            std::string rule;
            if (stream >> object.assetName >> object.worldOrigin.x >> object.worldOrigin.y >> object.scale >> rule)
            {
                object.scale = std::clamp(object.scale, 0.25f, 8.f);
                object.permanent = (rule == "permanent");
                layout.objects.push_back(object);
            }
        }
    }

    return layout.objects.empty() ? Fallback() : layout;
}

bool VillageLayoutLoader::Save(const std::string& path, const VillageLayout& layout)
{
    std::filesystem::path filePath(path);
    std::error_code dirEc;
    if (!filePath.parent_path().empty())
        std::filesystem::create_directories(filePath.parent_path(), dirEc);

    const std::filesystem::path temporary = filePath.string() + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out << "size " << layout.width << ' ' << layout.height << '\n';
    out << "spawn " << layout.spawnAssetName << ' ' << layout.spawnMarkerName << '\n';
    out << "exit " << layout.exitRect.x << ' ' << layout.exitRect.y << ' '
        << layout.exitRect.width << ' ' << layout.exitRect.height << '\n';
    for (const VillageLayoutObject& object : layout.objects)
    {
        out << "object " << object.assetName << ' ' << object.worldOrigin.x << ' ' << object.worldOrigin.y
            << ' ' << object.scale << ' ' << (object.permanent ? "permanent" : "removable") << '\n';
    }
    out.close();

    if (!ReplaceFile(temporary, filePath))
    {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        return false;
    }
    return true;
}

Vector2 VillageLayoutLoader::LocalToWorld(const VillageLayoutObject& object, Vector2 localPoint)
{
    return {
        object.worldOrigin.x + localPoint.x * object.scale,
        object.worldOrigin.y + localPoint.y * object.scale,
    };
}

Rectangle VillageLayoutLoader::LocalToWorld(const VillageLayoutObject& object, Rectangle localRect)
{
    return {
        object.worldOrigin.x + localRect.x * object.scale,
        object.worldOrigin.y + localRect.y * object.scale,
        localRect.width * object.scale,
        localRect.height * object.scale,
    };
}
