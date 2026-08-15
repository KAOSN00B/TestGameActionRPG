#define _CRT_SECURE_NO_WARNINGS
#include "VillageAssetData.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall MoveFileExW(
    const wchar_t* existingName, const wchar_t* newName, unsigned long flags);
constexpr unsigned long kMoveFileReplaceExisting = 0x1;
constexpr unsigned long kMoveFileWriteThrough = 0x8;
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Parsing helpers (file-local)
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    // Trims leading/trailing ASCII whitespace (spaces, tabs, CR, LF) in place.
    std::string Trim(const std::string& text)
    {
        size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return "";
        size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    // Strips a single pair of surrounding double quotes, if present.
    std::string Unquote(const std::string& text)
    {
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            return text.substr(1, text.size() - 2);
        return text;
    }

    // Splits a line into whitespace-separated tokens, but keeps a double-quoted
    // run as a single token (so prompt="Browse wares" stays intact). The quotes
    // are preserved on the token and removed later by Unquote where needed.
    std::vector<std::string> SplitTokens(const std::string& line)
    {
        std::vector<std::string> tokens;
        std::string current;
        bool insideQuotes = false;
        for (char character : line)
        {
            if (character == '"')
            {
                insideQuotes = !insideQuotes;
                current += character;
            }
            else if ((character == ' ' || character == '\t') && !insideQuotes)
            {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
            }
            else
            {
                current += character;
            }
        }
        if (!current.empty()) tokens.push_back(current);
        return tokens;
    }

    // Given tokens after the fixed positional ones, pulls the value for a
    // "key=value" pair. Returns true and fills outValue if the key is present.
    bool FindKeyValue(const std::vector<std::string>& tokens, size_t firstKeyValueIndex,
                      const char* key, std::string& outValue)
    {
        std::string prefix = std::string(key) + "=";
        for (size_t i = firstKeyValueIndex; i < tokens.size(); ++i)
        {
            if (tokens[i].rfind(prefix, 0) == 0)
            {
                outValue = Unquote(tokens[i].substr(prefix.size()));
                return true;
            }
        }
        return false;
    }

    // Everything on the line after the first token, trimmed and unquoted. Used
    // for free-text scalar values that may contain spaces (e.g. display_name).
    std::string RemainderAfterKeyword(const std::string& line)
    {
        size_t firstSpace = line.find_first_of(" \t");
        if (firstSpace == std::string::npos) return "";
        return Unquote(Trim(line.substr(firstSpace + 1)));
    }

    bool EqualsIgnoreCase(const std::string& a, const char* b)
    {
    #ifdef _WIN32
        return _stricmp(a.c_str(), b) == 0;
    #else
        return strcasecmp(a.c_str(), b) == 0;
    #endif
    }

    VillageBuildCategory ParseCategory(const std::string& text)
    {
        if (EqualsIgnoreCase(text, "Decor"))   return VillageBuildCategory::Decor;
        if (EqualsIgnoreCase(text, "Path"))    return VillageBuildCategory::Path;
        if (EqualsIgnoreCase(text, "Utility")) return VillageBuildCategory::Utility;
        if (EqualsIgnoreCase(text, "Trophy"))  return VillageBuildCategory::Trophy;
        return VillageBuildCategory::Building;   // default / "Building"
    }

    VillageService ParseService(const std::string& text)
    {
        if (EqualsIgnoreCase(text, "Shop"))        return VillageService::Shop;
        if (EqualsIgnoreCase(text, "Graveyard"))   return VillageService::Graveyard;
        if (EqualsIgnoreCase(text, "Training"))    return VillageService::Training;
        if (EqualsIgnoreCase(text, "ClassChange")) return VillageService::ClassChange;
        if (EqualsIgnoreCase(text, "Wardrobe"))    return VillageService::Wardrobe;
        if (EqualsIgnoreCase(text, "Cartographer"))return VillageService::Cartographer;
        if (EqualsIgnoreCase(text, "TrophyHall"))  return VillageService::TrophyHall;
        if (EqualsIgnoreCase(text, "DungeonGate")) return VillageService::DungeonGate;
        if (EqualsIgnoreCase(text, "Relic"))       return VillageService::Relic;
        return VillageService::None;
    }

    VillageInteractionType ParseInteractionType(const std::string& text)
    {
        if (EqualsIgnoreCase(text, "Shop"))        return VillageInteractionType::Shop;
        if (EqualsIgnoreCase(text, "Talk"))        return VillageInteractionType::Talk;
        if (EqualsIgnoreCase(text, "Enter"))       return VillageInteractionType::Enter;
        if (EqualsIgnoreCase(text, "Train"))       return VillageInteractionType::Train;
        if (EqualsIgnoreCase(text, "ChangeClass")) return VillageInteractionType::ChangeClass;
        if (EqualsIgnoreCase(text, "Wardrobe"))    return VillageInteractionType::Wardrobe;
        if (EqualsIgnoreCase(text, "Inspect"))     return VillageInteractionType::Inspect;
        if (EqualsIgnoreCase(text, "StartRun"))    return VillageInteractionType::StartRun;
        return VillageInteractionType::None;
    }

    // A marker's type is taken from an explicit type= when given, otherwise
    // inferred from well-known names so legacy "marker Zeph/Poe/Respawn" lines
    // get a meaningful type for free.
    VillageMarkerType MarkerTypeFromName(const std::string& name)
    {
        if (EqualsIgnoreCase(name, "Zeph"))            return VillageMarkerType::Zeph;
        if (EqualsIgnoreCase(name, "Poe"))             return VillageMarkerType::Poe;
        if (EqualsIgnoreCase(name, "Respawn"))         return VillageMarkerType::Respawn;
        if (EqualsIgnoreCase(name, "Door"))            return VillageMarkerType::Door;
        if (EqualsIgnoreCase(name, "Interact"))        return VillageMarkerType::Interact;
        if (EqualsIgnoreCase(name, "NPCSpawn"))        return VillageMarkerType::NPCSpawn;
        if (EqualsIgnoreCase(name, "AmbientNpcSpawn")) return VillageMarkerType::AmbientNpcSpawn;
        if (EqualsIgnoreCase(name, "PlayerSpawn"))     return VillageMarkerType::PlayerSpawn;
        return VillageMarkerType::Generic;
    }

    VillageMarkerType ParseMarkerType(const std::string& text)
    {
        // type= uses the same vocabulary as the inferred names.
        return MarkerTypeFromName(text);
    }

    // Atomically replaces `destination` with `source`. Used so a save that
    // crashes (or races with the running game reading the same file) can never
    // leave a half-written ".vasset" on disk.
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

    // Quote a value only if it contains whitespace; keeps simple names/paths
    // readable while still round-tripping through the quote-aware tokenizer.
    std::string QuoteIfNeeded(const std::string& text)
    {
        if (text.find_first_of(" \t") == std::string::npos) return text;
        return "\"" + text + "\"";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// VillageAssetData member functions
// ─────────────────────────────────────────────────────────────────────────────
const VAssetMarker* VillageAssetData::FindMarker(const char* markerName) const
{
    for (const VAssetMarker& marker : markers)
        if (EqualsIgnoreCase(marker.name, markerName)) return &marker;
    return nullptr;
}

const VAssetMarker* VillageAssetData::FindMarkerByType(VillageMarkerType markerType) const
{
    for (const VAssetMarker& marker : markers)
        if (marker.type == markerType) return &marker;
    return nullptr;
}

VaRect VillageAssetData::ImageWorldRect(VaVec2 worldOrigin, float scale) const
{
    return VaRect{ worldOrigin.x, worldOrigin.y, imageSize.x * scale, imageSize.y * scale };
}

VaRect VillageAssetData::ColliderWorldRect(int index, VaVec2 worldOrigin, float scale) const
{
    if (index < 0 || index >= (int)colliders.size()) return VaRect{};
    const VaRect& local = colliders[index];
    return VaRect{ worldOrigin.x + local.x * scale, worldOrigin.y + local.y * scale,
                   local.w * scale, local.h * scale };
}

VaVec2 VillageAssetData::MarkerWorldPos(const VAssetMarker& marker, VaVec2 worldOrigin, float scale) const
{
    return VaVec2{ worldOrigin.x + marker.localPos.x * scale,
                   worldOrigin.y + marker.localPos.y * scale };
}

VaRect VillageAssetData::InteractionWorldRect(int index, VaVec2 worldOrigin, float scale) const
{
    if (index < 0 || index >= (int)interactions.size()) return VaRect{};
    const VaRect& local = interactions[index].localRect;
    return VaRect{ worldOrigin.x + local.x * scale, worldOrigin.y + local.y * scale,
                   local.w * scale, local.h * scale };
}

// ─────────────────────────────────────────────────────────────────────────────
// Loader
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    // Derives an asset id from a file path: the filename without directory or
    // extension (e.g. ".../ZephsShop.vasset" -> "ZephsShop").
    std::string FileStem(const std::string& path)
    {
        size_t slash = path.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        return name;
    }
}

bool VillageAssetLoader::Load(const std::string& vassetPath, VillageAssetData& outData)
{
    FILE* file = fopen(vassetPath.c_str(), "r");
    if (!file) return false;

    outData = VillageAssetData{};
    outData.sourcePath = vassetPath;
    outData.id = FileStem(vassetPath);          // default id; overridden by an id line
    outData.displayName = outData.id;           // default; overridden by display_name

    bool sawHeader = false;
    char rawLine[1024];
    while (fgets(rawLine, sizeof(rawLine), file))
    {
        std::string line = Trim(rawLine);
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> tokens = SplitTokens(line);
        if (tokens.empty()) continue;
        const std::string& keyword = tokens[0];

        if (EqualsIgnoreCase(keyword, "village_asset")) { sawHeader = true; continue; }

        if (EqualsIgnoreCase(keyword, "image"))
        {
            outData.imageFile = RemainderAfterKeyword(line);
        }
        else if (EqualsIgnoreCase(keyword, "size") && tokens.size() >= 3)
        {
            outData.imageSize = VaVec2{ (float)atof(tokens[1].c_str()), (float)atof(tokens[2].c_str()) };
        }
        else if (EqualsIgnoreCase(keyword, "id"))
        {
            std::string value = RemainderAfterKeyword(line);
            if (!value.empty()) { outData.id = value; if (outData.displayName == FileStem(vassetPath)) outData.displayName = value; }
        }
        else if (EqualsIgnoreCase(keyword, "display_name"))
        {
            outData.displayName = RemainderAfterKeyword(line);
        }
        else if (EqualsIgnoreCase(keyword, "category"))
        {
            outData.category = ParseCategory(RemainderAfterKeyword(line));
        }
        else if (EqualsIgnoreCase(keyword, "service"))
        {
            outData.service = ParseService(RemainderAfterKeyword(line));
        }
        else if (EqualsIgnoreCase(keyword, "cost") && tokens.size() >= 2)
        {
            outData.cost = atoi(tokens[1].c_str());
        }
        else if (EqualsIgnoreCase(keyword, "required") && tokens.size() >= 2)
        {
            outData.required = atoi(tokens[1].c_str()) != 0;
        }
        else if (EqualsIgnoreCase(keyword, "unique") && tokens.size() >= 2)
        {
            outData.unique = atoi(tokens[1].c_str()) != 0;
        }
        else if (EqualsIgnoreCase(keyword, "removable") && tokens.size() >= 2)
        {
            outData.removable = atoi(tokens[1].c_str()) != 0;
        }
        else if (EqualsIgnoreCase(keyword, "collider") && tokens.size() >= 5)
        {
            outData.colliders.push_back(VaRect{
                (float)atof(tokens[1].c_str()), (float)atof(tokens[2].c_str()),
                (float)atof(tokens[3].c_str()), (float)atof(tokens[4].c_str()) });
        }
        else if (EqualsIgnoreCase(keyword, "marker") && tokens.size() >= 4)
        {
            VAssetMarker marker;
            marker.name = tokens[1];
            marker.localPos = VaVec2{ (float)atof(tokens[2].c_str()), (float)atof(tokens[3].c_str()) };
            std::string typeValue, idValue;
            marker.type = FindKeyValue(tokens, 4, "type", typeValue)
                          ? ParseMarkerType(typeValue) : MarkerTypeFromName(marker.name);
            if (FindKeyValue(tokens, 4, "id", idValue)) marker.npcId = idValue;
            outData.markers.push_back(marker);
        }
        else if (EqualsIgnoreCase(keyword, "interact") && tokens.size() >= 5)
        {
            VAssetInteraction interaction;
            interaction.localRect = VaRect{
                (float)atof(tokens[1].c_str()), (float)atof(tokens[2].c_str()),
                (float)atof(tokens[3].c_str()), (float)atof(tokens[4].c_str()) };
            std::string typeValue, promptValue, targetValue;
            if (FindKeyValue(tokens, 5, "type", typeValue))     interaction.type = ParseInteractionType(typeValue);
            if (FindKeyValue(tokens, 5, "prompt", promptValue)) interaction.prompt = promptValue;
            if (FindKeyValue(tokens, 5, "target", targetValue)) interaction.target = targetValue;
            outData.interactions.push_back(interaction);
        }
        else if (EqualsIgnoreCase(keyword, "door") && tokens.size() >= 5)
        {
            VAssetDoor door;
            door.localRect = VaRect{
                (float)atof(tokens[1].c_str()), (float)atof(tokens[2].c_str()),
                (float)atof(tokens[3].c_str()), (float)atof(tokens[4].c_str()) };
            std::string mapValue, spawnValue, blocksValue, promptValue;
            if (FindKeyValue(tokens, 5, "target", mapValue))       door.targetMap = mapValue;
            if (FindKeyValue(tokens, 5, "spawn", spawnValue))      door.targetSpawn = spawnValue;
            if (FindKeyValue(tokens, 5, "blocks_closed", blocksValue)) door.blocksWhenClosed = atoi(blocksValue.c_str()) != 0;
            if (FindKeyValue(tokens, 5, "prompt", promptValue))   door.prompt = promptValue;
            outData.doors.push_back(door);
        }
        else if (EqualsIgnoreCase(keyword, "ambient") && tokens.size() >= 3)
        {
            VAssetAmbientSpawn spawn;
            spawn.localPos = VaVec2{ (float)atof(tokens[1].c_str()), (float)atof(tokens[2].c_str()) };
            std::string groupValue, maxValue, unlockValue;
            if (FindKeyValue(tokens, 3, "group", groupValue))   spawn.group = groupValue;
            if (FindKeyValue(tokens, 3, "max", maxValue))       spawn.maxCount = atoi(maxValue.c_str());
            if (FindKeyValue(tokens, 3, "unlock_key", unlockValue)) spawn.unlockKey = unlockValue;
            outData.ambientSpawns.push_back(spawn);
        }
        else if (EqualsIgnoreCase(keyword, "animation") && tokens.size() >= 5)
        {
            outData.animation.enabled = true;
            outData.animation.columns = std::max(1, atoi(tokens[1].c_str()));
            outData.animation.rows = std::max(1, atoi(tokens[2].c_str()));
            outData.animation.frameCount = std::max(1, atoi(tokens[3].c_str()));
            outData.animation.fps = std::max(0.1f, (float)atof(tokens[4].c_str()));
        }
        // Any other keyword is silently ignored (forward-compatible).
    }
    fclose(file);
    return sawHeader;
}

bool VillageAssetLoader::Save(const std::string& vassetPath, const VillageAssetData& data)
{
    std::filesystem::path path(vassetPath);
    std::error_code dirEc;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), dirEc);

    const std::filesystem::path temporary = path.string() + ".tmp";
    FILE* file = fopen(temporary.string().c_str(), "w");
    if (!file) return false;

    fprintf(file, "village_asset 1\n");
    fprintf(file, "image %s\n", QuoteIfNeeded(data.imageFile).c_str());
    fprintf(file, "size %.0f %.0f\n", data.imageSize.x, data.imageSize.y);
    if (data.animation.enabled)
    {
        fprintf(file, "animation %d %d %d %.2f\n", data.animation.columns, data.animation.rows,
                data.animation.frameCount, data.animation.fps);
    }
    fprintf(file, "id %s\n", QuoteIfNeeded(data.id).c_str());
    if (data.displayName != data.id)
        fprintf(file, "display_name %s\n", QuoteIfNeeded(data.displayName).c_str());
    fprintf(file, "category %s\n", ToString(data.category));
    fprintf(file, "service %s\n", ToString(data.service));
    fprintf(file, "cost %d\n", data.cost);
    fprintf(file, "required %d\n", data.required ? 1 : 0);
    fprintf(file, "unique %d\n", data.unique ? 1 : 0);
    fprintf(file, "removable %d\n", data.removable ? 1 : 0);

    for (const VaRect& collider : data.colliders)
        fprintf(file, "collider %.2f %.2f %.2f %.2f\n", collider.x, collider.y, collider.w, collider.h);

    for (const VAssetMarker& marker : data.markers)
    {
        fprintf(file, "marker %s %.2f %.2f", QuoteIfNeeded(marker.name).c_str(), marker.localPos.x, marker.localPos.y);
        if (marker.type != MarkerTypeFromName(marker.name)) fprintf(file, " type=%s", ToString(marker.type));
        if (!marker.npcId.empty()) fprintf(file, " id=\"%s\"", marker.npcId.c_str());
        fprintf(file, "\n");
    }

    for (const VAssetInteraction& interaction : data.interactions)
    {
        fprintf(file, "interact %.2f %.2f %.2f %.2f", interaction.localRect.x, interaction.localRect.y,
                interaction.localRect.w, interaction.localRect.h);
        if (interaction.type != VillageInteractionType::None) fprintf(file, " type=%s", ToString(interaction.type));
        if (!interaction.prompt.empty()) fprintf(file, " prompt=\"%s\"", interaction.prompt.c_str());
        if (!interaction.target.empty()) fprintf(file, " target=\"%s\"", interaction.target.c_str());
        fprintf(file, "\n");
    }

    for (const VAssetDoor& door : data.doors)
    {
        fprintf(file, "door %.2f %.2f %.2f %.2f", door.localRect.x, door.localRect.y,
                door.localRect.w, door.localRect.h);
        if (!door.targetMap.empty()) fprintf(file, " target=\"%s\"", door.targetMap.c_str());
        if (!door.targetSpawn.empty()) fprintf(file, " spawn=\"%s\"", door.targetSpawn.c_str());
        if (!door.blocksWhenClosed) fprintf(file, " blocks_closed=0");
        if (!door.prompt.empty()) fprintf(file, " prompt=\"%s\"", door.prompt.c_str());
        fprintf(file, "\n");
    }

    for (const VAssetAmbientSpawn& spawn : data.ambientSpawns)
    {
        fprintf(file, "ambient %.2f %.2f", spawn.localPos.x, spawn.localPos.y);
        if (!spawn.group.empty()) fprintf(file, " group=\"%s\"", spawn.group.c_str());
        if (spawn.maxCount != 1) fprintf(file, " max=%d", spawn.maxCount);
        if (!spawn.unlockKey.empty()) fprintf(file, " unlock_key=\"%s\"", spawn.unlockKey.c_str());
        fprintf(file, "\n");
    }

    fclose(file);

    if (!ReplaceFile(temporary, path))
    {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        return false;
    }
    return true;
}

std::vector<VillageAssetData> VillageAssetLoader::LoadCatalog(const std::string& folder)
{
    std::vector<VillageAssetData> catalog;
    std::error_code errorCode;
    std::filesystem::directory_iterator iterator(folder, errorCode);
    if (errorCode) return catalog;

    for (const std::filesystem::directory_entry& entry : iterator)
    {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".vasset") continue;
        VillageAssetData data;
        if (Load(entry.path().string(), data)) catalog.push_back(std::move(data));
    }
    return catalog;
}

// ─────────────────────────────────────────────────────────────────────────────
// Enum -> string (readable output + debugging)
// ─────────────────────────────────────────────────────────────────────────────
const char* VillageAssetLoader::ToString(VillageBuildCategory category)
{
    switch (category)
    {
    case VillageBuildCategory::Building: return "Building";
    case VillageBuildCategory::Decor:    return "Decor";
    case VillageBuildCategory::Path:     return "Path";
    case VillageBuildCategory::Utility:  return "Utility";
    case VillageBuildCategory::Trophy:   return "Trophy";
    }
    return "Building";
}

const char* VillageAssetLoader::ToString(VillageService service)
{
    switch (service)
    {
    case VillageService::None:         return "None";
    case VillageService::Shop:         return "Shop";
    case VillageService::Graveyard:    return "Graveyard";
    case VillageService::Training:     return "Training";
    case VillageService::ClassChange:  return "ClassChange";
    case VillageService::Wardrobe:     return "Wardrobe";
    case VillageService::Cartographer: return "Cartographer";
    case VillageService::TrophyHall:   return "TrophyHall";
    case VillageService::DungeonGate:  return "DungeonGate";
    case VillageService::Relic:        return "Relic";
    }
    return "None";
}

const char* VillageAssetLoader::ToString(VillageInteractionType interactionType)
{
    switch (interactionType)
    {
    case VillageInteractionType::None:        return "None";
    case VillageInteractionType::Shop:        return "Shop";
    case VillageInteractionType::Talk:        return "Talk";
    case VillageInteractionType::Enter:       return "Enter";
    case VillageInteractionType::Train:       return "Train";
    case VillageInteractionType::ChangeClass: return "ChangeClass";
    case VillageInteractionType::Wardrobe:    return "Wardrobe";
    case VillageInteractionType::Inspect:     return "Inspect";
    case VillageInteractionType::StartRun:    return "StartRun";
    }
    return "None";
}

const char* VillageAssetLoader::ToString(VillageMarkerType markerType)
{
    switch (markerType)
    {
    case VillageMarkerType::Generic:         return "Generic";
    case VillageMarkerType::Zeph:            return "Zeph";
    case VillageMarkerType::Poe:             return "Poe";
    case VillageMarkerType::Respawn:         return "Respawn";
    case VillageMarkerType::Door:            return "Door";
    case VillageMarkerType::Interact:        return "Interact";
    case VillageMarkerType::NPCSpawn:        return "NPCSpawn";
    case VillageMarkerType::AmbientNpcSpawn: return "AmbientNpcSpawn";
    case VillageMarkerType::PlayerSpawn:     return "PlayerSpawn";
    }
    return "Generic";
}
