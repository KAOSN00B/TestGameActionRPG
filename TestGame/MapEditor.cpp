#pragma warning(disable: 4996)
#include "MapEditor.h"
#include "AssetPaths.h"
#include "VirtualCanvas.h"
#include "VillageAssetData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr float kPanelW = 390.f;
    constexpr float kStatusH = 44.f;
    constexpr float kRowH = 38.f;
    constexpr float kHandleSize = 12.f;

    std::string StemFromFileName(const char* fileName)
    {
        std::string stem = fileName ? fileName : "asset";
        size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        return stem;
    }

    std::string JoinPath(const std::string& folder, const std::string& file)
    {
        if (folder.empty()) return file;
        char last = folder[folder.size() - 1];
        if (last == '/' || last == '\\') return folder + file;
        return folder + "/" + file;
    }

    float ClampFloat(float v, float lo, float hi)
    {
        return std::max(lo, std::min(v, hi));
    }

    // Canonical .vasset strings — MUST match VillageAssetData's ParseCategory /
    // ParseService so the editor's output round-trips through the loader.
    const char* kCategoryNames[] = { "Building", "Decor", "Path", "Utility", "Trophy" };
    const int   kCategoryCount   = 5;
    // None first, then the services village buildings actually use.
    const char* kServiceNames[]  = { "None", "Shop", "Relic", "Wardrobe",
                                     "Training", "ClassChange", "Cartographer",
                                     "TrophyHall", "Graveyard", "DungeonGate" };
    const int   kServiceCount    = 10;

    bool EqualsCI(const char* a, const char* b)
    {
    #if defined(_WIN32)
        return _stricmp(a, b) == 0;
    #else
        return strcasecmp(a, b) == 0;
    #endif
    }

    // kCategoryNames/kServiceNames are UI-ordered, not enum-ordered, so map by
    // string (via VillageAssetLoader::ToString) rather than casting the index.
    VillageBuildCategory CategoryFromIndex(int idx)
    {
        for (int e = 0; e <= (int)VillageBuildCategory::Trophy; ++e)
            if (EqualsCI(VillageAssetLoader::ToString((VillageBuildCategory)e), kCategoryNames[idx]))
                return (VillageBuildCategory)e;
        return VillageBuildCategory::Building;
    }

    VillageService ServiceFromIndex(int idx)
    {
        for (int e = 0; e <= (int)VillageService::Relic; ++e)
            if (EqualsCI(VillageAssetLoader::ToString((VillageService)e), kServiceNames[idx]))
                return (VillageService)e;
        return VillageService::None;
    }
}

void MapEditor::Init()
{
    _wantsToExit = false;
    _helpOpen = false;
    _selectedCollider = -1;
    _selectedMarker = -1;
    _dragMode = DragMode::None;
    _dragCollider = -1;
    _dragMarker = -1;
    _assetListScroll = 0.f;
    _statusTimer = 0.f;
    _nameEntryPurpose = NameEntryPurpose::None;
    _nameEntryBuffer.clear();
    _placingMarker = false;
    LoadAssets();
    SelectAsset(_assets.empty() ? -1 : 0);
    _status = _assets.empty()
        ? "No PNGs found in VillageAssets — click New Asset to start one"
        : "Village Asset Adjuster: add colliders + markers, S save";
    _statusTimer = 5.f;
}

void MapEditor::Unload()
{
    for (Asset& asset : _assets)
    {
        if (asset.texture.id != 0) UnloadTexture(asset.texture);
        asset.texture = Texture2D{};
    }
    _assets.clear();
    _activeAsset = -1;
}

void MapEditor::LoadAssets()
{
    Unload();
    _assetFolder = AssetFolderPath("VillageAssets");
    if (!DirectoryExists(_assetFolder.c_str()))
        _assetFolder = "VillageAssets";

    FilePathList files = LoadDirectoryFiles(_assetFolder.c_str());
    for (unsigned int i = 0; i < files.count; ++i)
    {
        const char* fileName = GetFileName(files.paths[i]);
        if (!fileName || !IsFileExtension(fileName, ".png")) continue;

        Asset asset;
        asset.name = StemFromFileName(fileName);
        asset.pngFile = fileName;
        asset.pngPath = JoinPath(_assetFolder, asset.pngFile);
        asset.metaPath = JoinPath(_assetFolder, asset.name + ".vasset");
        asset.texture = LoadTexture(asset.pngPath.c_str());
        SetTextureFilter(asset.texture, TEXTURE_FILTER_POINT);
        LoadMetadata(asset);
        _assets.push_back(asset);
    }
    UnloadDirectoryFiles(files);

    std::sort(_assets.begin(), _assets.end(), [](const Asset& a, const Asset& b) {
        return a.name < b.name;
    });
}

void MapEditor::SelectAsset(int index)
{
    if (_assets.empty())
    {
        _activeAsset = -1;
        _selectedCollider = -1;
        _selectedMarker = -1;
        return;
    }

    _activeAsset = std::max(0, std::min(index, (int)_assets.size() - 1));
    _selectedCollider = -1;
    _selectedMarker = -1;
    _dragMarker = -1;
    _dragMode = DragMode::None;
    Rectangle canvas{ kPanelW, 0.f, (float)kVirtualWidth - kPanelW, (float)kVirtualHeight - kStatusH };
    FitActiveAssetToCanvas(canvas);
}

MapEditor::Asset* MapEditor::ActiveAsset()
{
    if (_activeAsset < 0 || _activeAsset >= (int)_assets.size()) return nullptr;
    return &_assets[_activeAsset];
}

const MapEditor::Asset* MapEditor::ActiveAsset() const
{
    if (_activeAsset < 0 || _activeAsset >= (int)_assets.size()) return nullptr;
    return &_assets[_activeAsset];
}

Color MapEditor::MarkerColor(const std::string& name) const
{
    // Well-known names keep their original colors; any custom marker name
    // (a new NPC spawn, a door anchor, etc.) gets a consistent default.
    if (EqualsCI(name.c_str(), "Zeph"))    return Color{ 90, 220, 255, 255 };
    if (EqualsCI(name.c_str(), "Poe"))     return Color{ 205, 150, 255, 255 };
    if (EqualsCI(name.c_str(), "Respawn")) return Color{ 120, 255, 145, 255 };
    return GOLD;
}

int MapEditor::AssetFrameWidth(const Asset& asset) const
{
    int cols = std::max(1, asset.animCols);
    return std::max(1, asset.animEnabled ? asset.texture.width / cols : asset.texture.width);
}

int MapEditor::AssetFrameHeight(const Asset& asset) const
{
    int rows = std::max(1, asset.animRows);
    return std::max(1, asset.animEnabled ? asset.texture.height / rows : asset.texture.height);
}

Rectangle MapEditor::AssetFrameSourceRect(const Asset& asset) const
{
    int frameW = AssetFrameWidth(asset);
    int frameH = AssetFrameHeight(asset);
    if (!asset.animEnabled) return Rectangle{ 0.f, 0.f, (float)frameW, (float)frameH };

    int cols = std::max(1, asset.animCols);
    int total = std::max(1, std::min(asset.animFrames, std::max(1, asset.animCols * asset.animRows)));
    int frame = ((int)(GetTime() * std::max(0.1f, asset.animFps))) % total;
    return Rectangle{ (float)((frame % cols) * frameW), (float)((frame / cols) * frameH), (float)frameW, (float)frameH };
}

std::string MapEditor::MarkerOffsetText(const Asset& asset, Vector2 markerPos) const
{
    std::string text;
    auto append = [&text](const char* part)
    {
        if (!text.empty()) text += ", ";
        text += part;
    };

    if (markerPos.x < 0.f) append(TextFormat("%.0f px left", -markerPos.x));
    else if (markerPos.x > AssetFrameWidth(asset)) append(TextFormat("%.0f px right", markerPos.x - AssetFrameWidth(asset)));

    if (markerPos.y < 0.f) append(TextFormat("%.0f px above", -markerPos.y));
    else if (markerPos.y > AssetFrameHeight(asset)) append(TextFormat("%.0f px below", markerPos.y - AssetFrameHeight(asset)));

    return text.empty() ? std::string("inside PNG") : std::string("outside PNG: ") + text;
}

void MapEditor::LoadMetadata(Asset& asset)
{
    asset.colliders.clear();
    asset.markers.clear();
    // Buying metadata back to defaults before parsing.
    asset.cost = 0; asset.categoryIdx = 0; asset.serviceIdx = 0;
    asset.required = false; asset.unique = false; asset.removable = true;
    asset.animEnabled = false; asset.animCols = 1; asset.animRows = 1; asset.animFrames = 1; asset.animFps = 6.f;

    VillageAssetData data;
    if (!VillageAssetLoader::Load(asset.metaPath, data)) return;

    for (const VaRect& collider : data.colliders)
        asset.colliders.push_back(ColliderBox{ Rectangle{ collider.x, collider.y, collider.w, collider.h } });
    for (const VAssetMarker& marker : data.markers)
        asset.markers.push_back(Marker{ marker.name, marker.npcId, Vector2{ marker.localPos.x, marker.localPos.y } });

    asset.cost = std::max(0, data.cost);
    asset.required = data.required;
    asset.unique = data.unique;
    asset.removable = data.removable;
    for (int k = 0; k < kCategoryCount; ++k)
        if (EqualsCI(VillageAssetLoader::ToString(data.category), kCategoryNames[k])) { asset.categoryIdx = k; break; }
    for (int k = 0; k < kServiceCount; ++k)
        if (EqualsCI(VillageAssetLoader::ToString(data.service), kServiceNames[k])) { asset.serviceIdx = k; break; }
    if (data.animation.enabled)
    {
        asset.animEnabled = true;
        asset.animCols = std::max(1, data.animation.columns);
        asset.animRows = std::max(1, data.animation.rows);
        asset.animFrames = std::max(1, data.animation.frameCount);
        asset.animFps = std::max(0.1f, data.animation.fps);
    }

    asset.dirty = false;
}

void MapEditor::SaveActiveMetadata()
{
    Asset* asset = ActiveAsset();
    if (!asset) return;

    VillageAssetData data;
    data.id = asset->name;
    data.displayName = asset->name;
    data.imageFile = asset->pngFile;
    data.imageSize = VaVec2{ (float)AssetFrameWidth(*asset), (float)AssetFrameHeight(*asset) };
    data.category = CategoryFromIndex(asset->categoryIdx);
    data.service = ServiceFromIndex(asset->serviceIdx);
    data.cost = asset->cost;
    data.required = asset->required;
    data.unique = asset->unique;
    data.removable = asset->removable;
    if (asset->animEnabled)
    {
        data.animation.enabled = true;
        data.animation.columns = std::max(1, asset->animCols);
        data.animation.rows = std::max(1, asset->animRows);
        data.animation.frameCount = std::max(1, asset->animFrames);
        data.animation.fps = std::max(0.1f, asset->animFps);
    }
    for (const ColliderBox& box : asset->colliders)
        data.colliders.push_back(VaRect{ box.rect.x, box.rect.y, box.rect.width, box.rect.height });
    for (const Marker& marker : asset->markers)
    {
        VAssetMarker vaMarker;
        vaMarker.name = marker.name;
        vaMarker.npcId = marker.npcId;
        vaMarker.localPos = VaVec2{ marker.pos.x, marker.pos.y };
        data.markers.push_back(vaMarker);
    }

    if (!VillageAssetLoader::Save(asset->metaPath, data))
    {
        _status = "Could not save " + asset->metaPath;
        _statusTimer = 4.f;
        return;
    }

    asset->dirty = false;
    _status = "Saved " + asset->name + ".vasset";
    _statusTimer = 3.f;
}

void MapEditor::FitActiveAssetToCanvas(Rectangle canvas)
{
    const Asset* asset = ActiveAsset();
    if (!asset || asset->texture.id == 0) return;
    float fitX = (canvas.width - 80.f) / std::max(1.f, (float)AssetFrameWidth(*asset));
    float fitY = (canvas.height - 80.f) / std::max(1.f, (float)AssetFrameHeight(*asset));
    _zoom = ClampFloat(std::min(fitX, fitY), 0.5f, 8.f);
    _viewOffset = Vector2{};
}

Vector2 MapEditor::ImageToScreen(Vector2 imagePos) const
{
    Rectangle canvas{ kPanelW, 0.f, (float)kVirtualWidth - kPanelW, (float)kVirtualHeight - kStatusH };
    const Asset* asset = ActiveAsset();
    if (!asset) return Vector2{};
    Vector2 origin{ canvas.x + canvas.width * 0.5f - AssetFrameWidth(*asset) * _zoom * 0.5f + _viewOffset.x,
                    canvas.y + canvas.height * 0.5f - AssetFrameHeight(*asset) * _zoom * 0.5f + _viewOffset.y };
    return Vector2{ origin.x + imagePos.x * _zoom, origin.y + imagePos.y * _zoom };
}

Vector2 MapEditor::ScreenToImage(Vector2 screenPos) const
{
    Rectangle canvas{ kPanelW, 0.f, (float)kVirtualWidth - kPanelW, (float)kVirtualHeight - kStatusH };
    const Asset* asset = ActiveAsset();
    if (!asset || _zoom <= 0.f) return Vector2{};
    Vector2 origin{ canvas.x + canvas.width * 0.5f - AssetFrameWidth(*asset) * _zoom * 0.5f + _viewOffset.x,
                    canvas.y + canvas.height * 0.5f - AssetFrameHeight(*asset) * _zoom * 0.5f + _viewOffset.y };
    return Vector2{ (screenPos.x - origin.x) / _zoom, (screenPos.y - origin.y) / _zoom };
}

Rectangle MapEditor::ImageRectToScreen(Rectangle imageRect) const
{
    Vector2 p = ImageToScreen(Vector2{ imageRect.x, imageRect.y });
    return Rectangle{ p.x, p.y, imageRect.width * _zoom, imageRect.height * _zoom };
}

Rectangle MapEditor::NormalizeImageRect(Rectangle rect) const
{
    if (rect.width < 0.f) { rect.x += rect.width; rect.width = -rect.width; }
    if (rect.height < 0.f) { rect.y += rect.height; rect.height = -rect.height; }

    // Colliders are authored in image-local coordinates but may intentionally
    // extend outside the visible PNG for doors, interaction pads, fences, and
    // sprite overhangs. Keep only a sane minimum size; do not clamp to the image.
    rect.width = std::max(1.f, rect.width);
    rect.height = std::max(1.f, rect.height);
    return rect;
}

Rectangle MapEditor::ActiveImageBoundsScreen() const
{
    const Asset* asset = ActiveAsset();
    if (!asset) return Rectangle{};
    Vector2 topLeft = ImageToScreen(Vector2{ 0.f, 0.f });
    return Rectangle{ topLeft.x, topLeft.y, AssetFrameWidth(*asset) * _zoom, AssetFrameHeight(*asset) * _zoom };
}

int MapEditor::ColliderAt(Vector2 imagePos) const
{
    const Asset* asset = ActiveAsset();
    if (!asset) return -1;
    for (int i = (int)asset->colliders.size() - 1; i >= 0; --i)
    {
        if (CheckCollisionPointRec(imagePos, asset->colliders[i].rect)) return i;
    }
    return -1;
}

int MapEditor::MarkerAt(Vector2 imagePos) const
{
    const Asset* asset = ActiveAsset();
    if (!asset) return -1;

    float pickRadius = 14.f / std::max(0.25f, _zoom);
    float pickRadiusSq = pickRadius * pickRadius;
    for (int i = 0; i < (int)asset->markers.size(); ++i)
    {
        const Marker& marker = asset->markers[i];
        float dx = marker.pos.x - imagePos.x;
        float dy = marker.pos.y - imagePos.y;
        if (dx * dx + dy * dy <= pickRadiusSq) return i;
    }
    return -1;
}

MapEditor::ColliderHandle MapEditor::ColliderHandleAt(const ColliderBox& box, Vector2 screenPos) const
{
    Rectangle screen = ImageRectToScreen(box.rect);
    Rectangle handles[] = {
        Rectangle{ screen.x - kHandleSize * 0.5f, screen.y - kHandleSize * 0.5f, kHandleSize, kHandleSize },
        Rectangle{ screen.x + screen.width - kHandleSize * 0.5f, screen.y - kHandleSize * 0.5f, kHandleSize, kHandleSize },
        Rectangle{ screen.x - kHandleSize * 0.5f, screen.y + screen.height - kHandleSize * 0.5f, kHandleSize, kHandleSize },
        Rectangle{ screen.x + screen.width - kHandleSize * 0.5f, screen.y + screen.height - kHandleSize * 0.5f, kHandleSize, kHandleSize },
    };
    ColliderHandle kinds[] = {
        ColliderHandle::TopLeft,
        ColliderHandle::TopRight,
        ColliderHandle::BottomLeft,
        ColliderHandle::BottomRight,
    };
    for (int i = 0; i < 4; ++i)
        if (CheckCollisionPointRec(screenPos, handles[i])) return kinds[i];
    return ColliderHandle::None;
}

void MapEditor::AddCollider(Rectangle imageRect)
{
    Asset* asset = ActiveAsset();
    if (!asset) return;
    imageRect = NormalizeImageRect(imageRect);
    asset->colliders.push_back(ColliderBox{ imageRect });
    _selectedCollider = (int)asset->colliders.size() - 1;
    asset->dirty = true;
    _status = "Added collider box";
    _statusTimer = 2.f;
}

void MapEditor::AddMarker(const std::string& name, Vector2 imagePos)
{
    Asset* asset = ActiveAsset();
    if (!asset || name.empty()) return;

    Marker marker;
    marker.name = name;
    marker.pos = imagePos;
    asset->markers.push_back(marker);
    _selectedMarker = (int)asset->markers.size() - 1;
    _selectedCollider = -1;
    asset->dirty = true;
    _status = "Added " + name + " marker (" + MarkerOffsetText(*asset, imagePos) + ")";
    _statusTimer = 3.f;
}

void MapEditor::BeginPlaceMarker()
{
    if (!ActiveAsset()) return;
    _placingMarker = true;
    _status = "Click on the canvas to place the new marker";
    _statusTimer = 3.f;
}

void MapEditor::BeginCreateAsset()
{
    _nameEntryPurpose = NameEntryPurpose::NewAsset;
    _nameEntryBuffer.clear();
}

void MapEditor::FinishNameEntry(bool confirmed)
{
    if (confirmed && !_nameEntryBuffer.empty())
    {
        if (_nameEntryPurpose == NameEntryPurpose::NewAsset)
            CreateAssetFromName(_nameEntryBuffer);
        else if (_nameEntryPurpose == NameEntryPurpose::NewMarker)
            AddMarker(_nameEntryBuffer, _pendingMarkerPos);
    }
    _nameEntryPurpose = NameEntryPurpose::None;
    _nameEntryBuffer.clear();
}

void MapEditor::CreateAssetFromName(const std::string& name)
{
    std::string pngPath = JoinPath(_assetFolder, name + ".png");
    if (FileExists(pngPath.c_str()))
    {
        Asset asset;
        asset.name = name;
        asset.pngFile = name + ".png";
        asset.pngPath = pngPath;
        asset.metaPath = JoinPath(_assetFolder, name + ".vasset");
        asset.texture = LoadTexture(asset.pngPath.c_str());
        SetTextureFilter(asset.texture, TEXTURE_FILTER_POINT);
        LoadMetadata(asset);   // no .vasset yet, so this just resets to defaults
        asset.dirty = true;
        _assets.push_back(asset);
        std::sort(_assets.begin(), _assets.end(), [](const Asset& a, const Asset& b) { return a.name < b.name; });
        for (int i = 0; i < (int)_assets.size(); ++i)
            if (_assets[i].name == name) { SelectAsset(i); break; }
        _status = "Created " + name + " from its PNG. Press S to save its .vasset.";
        _statusTimer = 4.f;
    }
    else
    {
        _status = "Drop " + name + ".png into VillageAssets, then click New Asset again.";
        _statusTimer = 5.f;
    }
}

void MapEditor::Update()
{
    if (_statusTimer > 0.f) _statusTimer -= GetFrameTime();

    if (_helpOpen)
    {
        if (IsKeyPressed(KEY_H) || IsKeyPressed(KEY_ESCAPE)) _helpOpen = false;
        return;
    }

    if (_nameEntryPurpose != NameEntryPurpose::None)
    {
        UpdateNameEntry();
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (_placingMarker) { _placingMarker = false; _status = "Cancelled"; _statusTimer = 1.5f; return; }
        _wantsToExit = true;
        return;
    }
    if (IsKeyPressed(KEY_H)) { _helpOpen = true; return; }
    if (IsKeyPressed(KEY_S)) SaveActiveMetadata();
    if (IsKeyPressed(KEY_F))
    {
        Rectangle canvas{ kPanelW, 0.f, (float)kVirtualWidth - kPanelW, (float)kVirtualHeight - kStatusH };
        FitActiveAssetToCanvas(canvas);
    }
    if (IsKeyPressed(KEY_DELETE))
    {
        Asset* asset = ActiveAsset();
        if (asset && _selectedCollider >= 0 && _selectedCollider < (int)asset->colliders.size())
        {
            asset->colliders.erase(asset->colliders.begin() + _selectedCollider);
            _selectedCollider = -1;
            asset->dirty = true;
            _status = "Deleted collider";
            _statusTimer = 2.f;
        }
        else if (asset && _selectedMarker >= 0 && _selectedMarker < (int)asset->markers.size())
        {
            asset->markers.erase(asset->markers.begin() + _selectedMarker);
            _selectedMarker = -1;
            asset->dirty = true;
            _status = "Deleted marker";
            _statusTimer = 2.f;
        }
    }

    Rectangle panel{ 0.f, 0.f, kPanelW, (float)kVirtualHeight - kStatusH };
    Rectangle canvas{ kPanelW, 0.f, (float)kVirtualWidth - kPanelW, (float)kVirtualHeight - kStatusH };
    UpdatePanel(panel);
    UpdateCanvas(canvas);
    UpdateSelectedColliderKeys();
    UpdateSelectedMarkerKeys();
    UpdateAssetMetadataKeys();
}

void MapEditor::UpdateNameEntry()
{
    int key = GetCharPressed();
    while (key > 0)
    {
        if (key >= 32 && key <= 126 && _nameEntryBuffer.size() < 48) _nameEntryBuffer.push_back((char)key);
        key = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) && !_nameEntryBuffer.empty()) _nameEntryBuffer.pop_back();
    if (IsKeyPressed(KEY_ENTER)) FinishNameEntry(true);
    if (IsKeyPressed(KEY_ESCAPE)) FinishNameEntry(false);
}

void MapEditor::UpdatePanel(Rectangle panel)
{
    Vector2 mouse = GetVirtualMousePos();
    if (!CheckCollisionPointRec(mouse, panel)) return;

    Rectangle list{ 10.f, 90.f, panel.width - 20.f, 230.f };
    float wheel = GetMouseWheelMove();
    if (wheel != 0.f && CheckCollisionPointRec(mouse, list))
    {
        float maxScroll = std::max(0.f, (float)_assets.size() * kRowH - list.height);
        _assetListScroll = ClampFloat(_assetListScroll - wheel * kRowH * 2.f, 0.f, maxScroll);
    }

    if (!IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) return;

    auto buttonHit = [&](float x, float y, float w, const char* label) -> bool
    {
        (void)label;
        return CheckCollisionPointRec(mouse, Rectangle{ x, y, w, 30.f });
    };

    Asset* asset = ActiveAsset();
    const float bx = 16.f;
    const float by = 330.f;
    if (buttonHit(bx, by, 86.f, "Save"))
    {
        SaveActiveMetadata();
        return;
    }
    if (buttonHit(bx + 94.f, by, 70.f, "Fit"))
    {
        Rectangle canvas{ kPanelW, 0.f, (float)kVirtualWidth - kPanelW, (float)kVirtualHeight - kStatusH };
        FitActiveAssetToCanvas(canvas);
        _status = "Fit asset to view";
        _statusTimer = 2.f;
        return;
    }
    if (buttonHit(bx + 172.f, by, 76.f, "Help"))
    {
        _helpOpen = true;
        return;
    }
    if (buttonHit(bx, by + 38.f, 98.f, "Add Box"))
    {
        if (asset)
            AddCollider(Rectangle{ AssetFrameWidth(*asset) * 0.5f - 32.f, AssetFrameHeight(*asset) * 0.5f - 32.f, 64.f, 64.f });
        return;
    }
    if (buttonHit(bx + 106.f, by + 38.f, 82.f, "Full"))
    {
        if (asset)
        {
            asset->colliders.clear();
            asset->colliders.push_back(ColliderBox{ Rectangle{ 0.f, 0.f, (float)AssetFrameWidth(*asset), (float)AssetFrameHeight(*asset) } });
            _selectedCollider = 0;
            _selectedMarker = -1;
            asset->dirty = true;
            _status = "Collider = whole sprite";
            _statusTimer = 2.f;
        }
        return;
    }
    if (buttonHit(bx + 196.f, by + 38.f, 96.f, "No Coll"))
    {
        if (asset)
        {
            asset->colliders.clear();
            _selectedCollider = -1;
            asset->dirty = true;
            _status = "Cleared all colliders";
            _statusTimer = 2.f;
        }
        return;
    }
    if (buttonHit(bx, by + 76.f, 130.f, "New Asset"))
    {
        BeginCreateAsset();
        return;
    }
    if (buttonHit(bx + 138.f, by + 76.f, 154.f, "New Marker"))
    {
        BeginPlaceMarker();
        return;
    }

    if (CheckCollisionPointRec(mouse, list))
    {
        int row = (int)((mouse.y - list.y + _assetListScroll) / kRowH);
        if (row >= 0 && row < (int)_assets.size()) SelectAsset(row);
    }
}

void MapEditor::UpdateCanvas(Rectangle canvas)
{
    Asset* asset = ActiveAsset();
    if (!asset) return;

    Vector2 mouse = GetVirtualMousePos();
    bool inCanvas = CheckCollisionPointRec(mouse, canvas);
    float wheel = inCanvas ? GetMouseWheelMove() : 0.f;
    if (wheel != 0.f)
    {
        Vector2 imageBeforeZoom = ScreenToImage(mouse);
        _zoom = ClampFloat(_zoom + wheel * 0.18f * _zoom, 0.25f, 12.f);
        Vector2 sameImageScreen = ImageToScreen(imageBeforeZoom);
        _viewOffset.x += mouse.x - sameImageScreen.x;
        _viewOffset.y += mouse.y - sameImageScreen.y;
    }

    if (IsMouseButtonPressed(MOUSE_MIDDLE_BUTTON) && inCanvas)
    {
        _panning = true;
        _panStartMouse = mouse;
        _panStartOffset = _viewOffset;
    }
    if (_panning)
    {
        if (IsMouseButtonDown(MOUSE_MIDDLE_BUTTON))
            _viewOffset = Vector2{ _panStartOffset.x + mouse.x - _panStartMouse.x,
                                   _panStartOffset.y + mouse.y - _panStartMouse.y };
        else
            _panning = false;
    }

    Vector2 imageMouse = ScreenToImage(mouse);
    if (inCanvas && IsKeyPressed(KEY_C))
        AddCollider(Rectangle{ imageMouse.x - 32.f, imageMouse.y - 32.f, 64.f, 64.f });

    if (inCanvas && _placingMarker && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        _placingMarker = false;
        _pendingMarkerPos = imageMouse;
        _nameEntryPurpose = NameEntryPurpose::NewMarker;
        _nameEntryBuffer.clear();
        return;
    }

    if (inCanvas && IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
    {
        _dragMode = DragMode::DrawCollider;
        _dragStartMouseImage = imageMouse;
        _drawPreview = Rectangle{ imageMouse.x, imageMouse.y, 0.f, 0.f };
    }
    if (_dragMode == DragMode::DrawCollider)
    {
        _drawPreview = Rectangle{ _dragStartMouseImage.x, _dragStartMouseImage.y,
                                  imageMouse.x - _dragStartMouseImage.x,
                                  imageMouse.y - _dragStartMouseImage.y };
        if (!IsMouseButtonDown(MOUSE_RIGHT_BUTTON))
        {
            Rectangle rect = NormalizeImageRect(_drawPreview);
            if (rect.width >= 3.f && rect.height >= 3.f) AddCollider(rect);
            _dragMode = DragMode::None;
        }
        return;
    }

    if (inCanvas && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    {
        int markerHit = MarkerAt(imageMouse);
        if (markerHit >= 0)
        {
            _selectedMarker = markerHit;
            _selectedCollider = -1;
            _dragMarker = markerHit;
            _dragMode = DragMode::MoveMarker;
            _status = "Selected " + asset->markers[markerHit].name + " marker";
            _statusTimer = 2.f;
            return;
        }

        _selectedCollider = -1;
        _selectedMarker = -1;
        for (int i = (int)asset->colliders.size() - 1; i >= 0; --i)
        {
            ColliderHandle handle = ColliderHandleAt(asset->colliders[i], mouse);
            if (handle != ColliderHandle::None)
            {
                _selectedCollider = i;
                _dragCollider = i;
                _dragHandle = handle;
                _dragMode = DragMode::ResizeCollider;
                _dragStartMouseImage = imageMouse;
                _dragStartRect = asset->colliders[i].rect;
                return;
            }
        }

        int hit = ColliderAt(imageMouse);
        if (hit >= 0)
        {
            _selectedCollider = hit;
            _dragCollider = hit;
            _dragMode = DragMode::MoveCollider;
            _dragStartMouseImage = imageMouse;
            _dragStartRect = asset->colliders[hit].rect;
        }
    }

    if (_dragMode == DragMode::MoveMarker)
    {
        if (_dragMarker < 0 || _dragMarker >= (int)asset->markers.size())
        {
            _dragMode = DragMode::None;
            _dragMarker = -1;
            return;
        }

        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            asset->markers[_dragMarker].pos = imageMouse;
            asset->dirty = true;
        }
        else
        {
            const Marker& moved = asset->markers[_dragMarker];
            _status = "Moved " + moved.name + " marker (" + MarkerOffsetText(*asset, moved.pos) + ")";
            _statusTimer = 3.f;
            _dragMode = DragMode::None;
            _dragMarker = -1;
        }
        return;
    }

    if (_dragMode == DragMode::MoveCollider || _dragMode == DragMode::ResizeCollider)
    {
        if (_dragCollider < 0 || _dragCollider >= (int)asset->colliders.size())
        {
            _dragMode = DragMode::None;
            _dragHandle = ColliderHandle::None;
            return;
        }

        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            Vector2 delta{ imageMouse.x - _dragStartMouseImage.x, imageMouse.y - _dragStartMouseImage.y };
            Rectangle rect = _dragStartRect;
            if (_dragMode == DragMode::MoveCollider)
            {
                rect.x += delta.x;
                rect.y += delta.y;
            }
            else
            {
                switch (_dragHandle)
                {
                case ColliderHandle::TopLeft:
                    rect.x += delta.x;
                    rect.y += delta.y;
                    rect.width -= delta.x;
                    rect.height -= delta.y;
                    break;
                case ColliderHandle::TopRight:
                    rect.y += delta.y;
                    rect.width += delta.x;
                    rect.height -= delta.y;
                    break;
                case ColliderHandle::BottomLeft:
                    rect.x += delta.x;
                    rect.width -= delta.x;
                    rect.height += delta.y;
                    break;
                case ColliderHandle::BottomRight:
                default:
                    rect.width += delta.x;
                    rect.height += delta.y;
                    break;
                }
            }
            asset->colliders[_dragCollider].rect = NormalizeImageRect(rect);
            asset->dirty = true;
        }
        else
        {
            _dragMode = DragMode::None;
            _dragCollider = -1;
            _dragHandle = ColliderHandle::None;
        }
    }
}

void MapEditor::UpdateSelectedColliderKeys()
{
    Asset* asset = ActiveAsset();
    if (!asset || _selectedCollider < 0 || _selectedCollider >= (int)asset->colliders.size()) return;

    float moveStep = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 8.f : 1.f;
    Rectangle rect = asset->colliders[_selectedCollider].rect;
    bool changed = false;
    bool resizing = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (IsKeyDown(KEY_LEFT))  { resizing ? rect.width -= moveStep : rect.x -= moveStep; changed = true; }
    if (IsKeyDown(KEY_RIGHT)) { resizing ? rect.width += moveStep : rect.x += moveStep; changed = true; }
    if (IsKeyDown(KEY_UP))    { resizing ? rect.height -= moveStep : rect.y -= moveStep; changed = true; }
    if (IsKeyDown(KEY_DOWN))  { resizing ? rect.height += moveStep : rect.y += moveStep; changed = true; }

    if (changed)
    {
        asset->colliders[_selectedCollider].rect = NormalizeImageRect(rect);
        asset->dirty = true;
    }
}

void MapEditor::UpdateAssetMetadataKeys()
{
    Asset* asset = ActiveAsset();
    if (!asset) return;

    // ── Collider quick-modes (B) ────────────────────────────────────────────────
    // W = whole-sprite collider (mushrooms & simple solid props); N = clear all
    // colliders (flowers / walk-through decor). Trees & buildings still use the
    // manual right-drag boxes for one-or-many precise colliders.
    if (IsKeyPressed(KEY_W))
    {
        asset->colliders.clear();
        asset->colliders.push_back(ColliderBox{ Rectangle{ 0.f, 0.f,
            (float)AssetFrameWidth(*asset), (float)AssetFrameHeight(*asset) } });
        _selectedCollider = 0;
        asset->dirty = true;
        _status = "Collider = whole sprite"; _statusTimer = 2.f;
    }
    if (IsKeyPressed(KEY_N))
    {
        asset->colliders.clear();
        _selectedCollider = -1;
        asset->dirty = true;
        _status = "Cleared all colliders (walk-through)"; _statusTimer = 2.f;
    }

    // ── Buying/service metadata (A) ─────────────────────────────────────────────
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool changed = false;
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) { asset->cost += shift ? 100 : 10; changed = true; }
    if (IsKeyPressed(KEY_LEFT_BRACKET))  { asset->cost -= shift ? 100 : 10; if (asset->cost < 0) asset->cost = 0; changed = true; }
    if (IsKeyPressed(KEY_PERIOD))    { asset->categoryIdx = (asset->categoryIdx + 1) % kCategoryCount; changed = true; }
    if (IsKeyPressed(KEY_COMMA))     { asset->categoryIdx = (asset->categoryIdx + kCategoryCount - 1) % kCategoryCount; changed = true; }
    if (IsKeyPressed(KEY_APOSTROPHE)){ asset->serviceIdx  = (asset->serviceIdx + 1) % kServiceCount; changed = true; }
    if (IsKeyPressed(KEY_SEMICOLON)) { asset->serviceIdx  = (asset->serviceIdx + kServiceCount - 1) % kServiceCount; changed = true; }
    if (IsKeyPressed(KEY_T)) { asset->required  = !asset->required;  changed = true; }
    if (IsKeyPressed(KEY_U)) { asset->unique    = !asset->unique;    changed = true; }
    if (IsKeyPressed(KEY_M)) { asset->removable = !asset->removable; changed = true; }

    // Animation metadata for water/torch/etc. sheets.
    if (IsKeyPressed(KEY_A)) { asset->animEnabled = !asset->animEnabled; changed = true; }
    if (IsKeyPressed(KEY_I)) { asset->animCols = std::max(1, asset->animCols + 1); changed = true; }
    if (IsKeyPressed(KEY_K)) { asset->animCols = std::max(1, asset->animCols - 1); changed = true; }
    if (IsKeyPressed(KEY_O)) { asset->animRows = std::max(1, asset->animRows + 1); changed = true; }
    if (IsKeyPressed(KEY_L)) { asset->animRows = std::max(1, asset->animRows - 1); changed = true; }
    if (IsKeyPressed(KEY_EQUAL)) { asset->animFrames = std::min(std::max(1, asset->animCols * asset->animRows), asset->animFrames + 1); changed = true; }
    if (IsKeyPressed(KEY_MINUS)) { asset->animFrames = std::max(1, asset->animFrames - 1); changed = true; }
    if (IsKeyPressed(KEY_PAGE_UP)) { asset->animFps += 1.f; changed = true; }
    if (IsKeyPressed(KEY_PAGE_DOWN)) { asset->animFps = std::max(0.1f, asset->animFps - 1.f); changed = true; }
    asset->animFrames = std::min(asset->animFrames, std::max(1, asset->animCols * asset->animRows));
    if (changed) asset->dirty = true;
}

void MapEditor::UpdateSelectedMarkerKeys()
{
    Asset* asset = ActiveAsset();
    if (!asset || _selectedCollider >= 0 || _selectedMarker < 0 || _selectedMarker >= (int)asset->markers.size()) return;
    Marker& marker = asset->markers[_selectedMarker];

    float moveStep = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 8.f : 1.f;
    bool changed = false;

    if (IsKeyDown(KEY_LEFT))  { marker.pos.x -= moveStep; changed = true; }
    if (IsKeyDown(KEY_RIGHT)) { marker.pos.x += moveStep; changed = true; }
    if (IsKeyDown(KEY_UP))    { marker.pos.y -= moveStep; changed = true; }
    if (IsKeyDown(KEY_DOWN))  { marker.pos.y += moveStep; changed = true; }

    if (changed)
    {
        asset->dirty = true;
        _status = "Moved " + marker.name + " marker (" + MarkerOffsetText(*asset, marker.pos) + ")";
        _statusTimer = 1.5f;
    }
}

void MapEditor::Draw() const
{
    ClearBackground(Color{ 22, 23, 28, 255 });
    Rectangle panel{ 0.f, 0.f, kPanelW, (float)kVirtualHeight - kStatusH };
    Rectangle canvas{ kPanelW, 0.f, (float)kVirtualWidth - kPanelW, (float)kVirtualHeight - kStatusH };
    DrawCanvas(canvas);
    DrawPanel(panel);
    DrawStatusBar();
    if (_helpOpen) DrawHelp();
    if (_nameEntryPurpose != NameEntryPurpose::None) DrawNameEntry();
}

void MapEditor::DrawNameEntry() const
{
    DrawRectangle(0, 0, kVirtualWidth, kVirtualHeight, Fade(BLACK, 0.6f));
    const char* title = _nameEntryPurpose == NameEntryPurpose::NewAsset ? "Name the new asset" : "Name the new marker";
    Rectangle box{ kVirtualWidth * 0.5f - 260.f, kVirtualHeight * 0.5f - 60.f, 520.f, 120.f };
    DrawRectangleRounded(box, 0.08f, 8, Color{ 34, 36, 44, 255 });
    DrawRectangleRoundedLines(box, 0.08f, 8, GOLD);
    int titleTw = MeasureText(title, 24);
    DrawText(title, (int)(box.x + box.width * 0.5f - titleTw * 0.5f), (int)box.y + 16, 24, GOLD);

    Rectangle field{ box.x + 20.f, box.y + 54.f, box.width - 40.f, 34.f };
    DrawRectangleRec(field, Color{ 20, 21, 26, 255 });
    DrawRectangleLinesEx(field, 1.f, Fade(RAYWHITE, 0.4f));
    std::string shown = _nameEntryBuffer + ((int)(GetTime() * 2.0) % 2 == 0 ? "_" : "");
    DrawText(shown.c_str(), (int)field.x + 10, (int)field.y + 7, 20, RAYWHITE);

    const char* hint = _nameEntryPurpose == NameEntryPurpose::NewAsset
        ? "Enter confirms (PNG must already be in VillageAssets) | Esc cancels"
        : "Enter confirms | Esc cancels";
    int hintTw = MeasureText(hint, 15);
    DrawText(hint, (int)(box.x + box.width * 0.5f - hintTw * 0.5f), (int)(box.y + box.height - 22.f), 15, Fade(RAYWHITE, 0.6f));
}

void MapEditor::DrawPanel(Rectangle panel) const
{
    DrawRectangleRec(panel, Color{ 30, 31, 38, 255 });
    DrawLineEx(Vector2{ panel.x + panel.width, panel.y }, Vector2{ panel.x + panel.width, panel.y + panel.height }, 2.f, Color{ 70, 72, 86, 255 });
    DrawText("VILLAGE ASSET ADJUSTER", 16, 14, 24, GOLD);
    DrawText("PNGs in VillageAssets | .vasset metadata", 16, 44, 16, Fade(RAYWHITE, 0.72f));
    DrawText("New Marker to name+place one | drag/arrows offset", 16, 64, 15, Fade(RAYWHITE, 0.72f));

    Rectangle list{ 10.f, 90.f, panel.width - 20.f, 230.f };
    DrawRectangleRec(list, Color{ 23, 24, 30, 255 });
    DrawRectangleLinesEx(list, 1.f, Color{ 75, 78, 90, 255 });
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    for (int i = 0; i < (int)_assets.size(); ++i)
    {
        float y = list.y + i * kRowH - _assetListScroll;
        if (y + kRowH < list.y || y > list.y + list.height) continue;
        Rectangle row{ list.x + 5.f, y + 4.f, list.width - 10.f, kRowH - 6.f };
        bool active = i == _activeAsset;
        DrawRectangleRec(row, active ? Color{ 76, 82, 58, 255 } : Color{ 42, 44, 52, 255 });
        DrawRectangleLinesEx(row, 1.f, active ? GOLD : Color{ 65, 68, 78, 255 });
        DrawText(_assets[i].name.c_str(), (int)row.x + 8, (int)row.y + 7, 18, active ? GOLD : RAYWHITE);
        if (_assets[i].dirty) DrawText("*", (int)(row.x + row.width - 18), (int)row.y + 5, 22, GOLD);
    }
    if (_assets.empty()) DrawText("No PNG assets found", (int)list.x + 12, (int)list.y + 18, 20, Fade(RAYWHITE, 0.75f));
    EndScissorMode();

    auto drawButton = [](Rectangle r, const char* label, Color fill)
    {
        DrawRectangleRounded(r, 0.18f, 6, fill);
        DrawRectangleRoundedLines(r, 0.18f, 6, Fade(RAYWHITE, 0.45f));
        int fs = 17;
        int tw = MeasureText(label, fs);
        DrawText(label, (int)(r.x + r.width * 0.5f - tw * 0.5f), (int)(r.y + r.height * 0.5f - fs * 0.5f), fs, RAYWHITE);
    };
    drawButton(Rectangle{ 16.f, 330.f, 86.f, 30.f }, "Save", Color{ 70, 100, 54, 255 });
    drawButton(Rectangle{ 110.f, 330.f, 70.f, 30.f }, "Fit", Color{ 54, 72, 100, 255 });
    drawButton(Rectangle{ 188.f, 330.f, 76.f, 30.f }, "Help", Color{ 78, 64, 102, 255 });
    drawButton(Rectangle{ 16.f, 368.f, 98.f, 30.f }, "Add Box", Color{ 95, 70, 52, 255 });
    drawButton(Rectangle{ 122.f, 368.f, 82.f, 30.f }, "Full", Color{ 98, 58, 48, 255 });
    drawButton(Rectangle{ 212.f, 368.f, 96.f, 30.f }, "No Coll", Color{ 88, 54, 54, 255 });
    drawButton(Rectangle{ 16.f, 406.f, 130.f, 30.f }, "New Asset", Color{ 60, 90, 96, 255 });
    drawButton(Rectangle{ 154.f, 406.f, 154.f, 30.f }, _placingMarker ? "Click canvas..." : "New Marker", _placingMarker ? GOLD : Color{ 96, 78, 60, 255 });

    const Asset* asset = ActiveAsset();
    if (!asset) return;

    int y = 452;
    DrawText(TextFormat("Asset: %s%s", asset->name.c_str(), asset->dirty ? " *" : ""), 16, y, 22, RAYWHITE); y += 30;
    DrawText(TextFormat("Image: %dx%d  frame: %dx%d", asset->texture.width, asset->texture.height, AssetFrameWidth(*asset), AssetFrameHeight(*asset)), 16, y, 18, Fade(RAYWHITE, 0.78f)); y += 26;
    DrawText(TextFormat("Colliders: %d   (W whole / N none)", (int)asset->colliders.size()), 16, y, 18, Fade(RAYWHITE, 0.78f)); y += 24;
    DrawText(TextFormat("Anim: %s  %dx%d frames:%d fps:%.1f", asset->animEnabled ? "ON" : "off", asset->animCols, asset->animRows, asset->animFrames, asset->animFps), 16, y, 17, Color{ 130, 220, 255, 255 }); y += 22;
    DrawText("A toggle | I/K cols | O/L rows | -/= frames | Pg fps", 16, y, 14, Fade(RAYWHITE, 0.62f)); y += 26;

    // Buying/service metadata (edit keys shown inline).
    DrawText(TextFormat("Category: %s   [ , . ]", kCategoryNames[asset->categoryIdx]), 16, y, 18, Color{ 170, 210, 255, 255 }); y += 24;
    DrawText(TextFormat("Service:  %s   [ ; ' ]", kServiceNames[asset->serviceIdx]),   16, y, 18, Color{ 202, 182, 255, 255 }); y += 24;
    DrawText(TextFormat("Cost:     %d gold   [ [ ] , shift x10 ]", asset->cost),        16, y, 18, Color{ 255, 224, 130, 255 }); y += 24;
    DrawText(TextFormat("required %s (T)  unique %s (U)  removable %s (M)", asset->required ? "ON" : "off", asset->unique ? "ON" : "off", asset->removable ? "ON" : "off"),
             16, y, 16, Fade(RAYWHITE, 0.8f)); y += 28;

    DrawText(TextFormat("Markers: %d (New Marker button to add)", (int)asset->markers.size()), 16, y, 18, Color{ 220, 200, 130, 255 }); y += 24;
    for (int i = 0; i < (int)asset->markers.size(); ++i)
    {
        const Marker& marker = asset->markers[i];
        Color c = MarkerColor(marker.name);
        bool selected = i == _selectedMarker;
        DrawText(TextFormat("%s: %.0f, %.0f", marker.name.c_str(), marker.pos.x, marker.pos.y), 16, y, 18, selected ? GOLD : c);
        y += 22;
        std::string offset = MarkerOffsetText(*asset, marker.pos);
        DrawText(offset.c_str(), 30, y, 15, offset == "inside PNG" ? Fade(RAYWHITE, 0.55f) : Fade(c, 0.78f));
        y += 20;
    }
    if (asset->markers.empty()) { DrawText("none yet", 30, y, 15, Fade(RAYWHITE, 0.5f)); y += 20; }

    y += 10;
    if (_selectedMarker >= 0 && _selectedMarker < (int)asset->markers.size())
    {
        const Marker& marker = asset->markers[_selectedMarker];
        DrawText(TextFormat("Selected marker: %s", marker.name.c_str()), 16, y, 18, GOLD); y += 24;
        DrawText(TextFormat("x %.0f y %.0f", marker.pos.x, marker.pos.y), 16, y, 17, RAYWHITE); y += 22;
        DrawText("Drag marker or use arrows; Shift moves faster; Delete removes", 16, y, 15, Fade(RAYWHITE, 0.68f)); y += 24;
    }

    DrawText("Selected collider:", 16, y, 18, GOLD); y += 24;
    if (_selectedCollider >= 0 && _selectedCollider < (int)asset->colliders.size())
    {
        Rectangle r = asset->colliders[_selectedCollider].rect;
        DrawText(TextFormat("x %.0f y %.0f w %.0f h %.0f", r.x, r.y, r.width, r.height), 16, y, 17, RAYWHITE); y += 22;
        DrawText("Drag box to move, drag corner to resize", 16, y, 15, Fade(RAYWHITE, 0.68f)); y += 20;
        DrawText("Colliders can extend outside the PNG", 16, y, 15, Fade(RAYWHITE, 0.68f)); y += 20;
        DrawText("Arrows move, Ctrl+Arrows resize, Shift faster", 16, y, 15, Fade(RAYWHITE, 0.68f));
    }
    else
    {
        DrawText("none", 16, y, 17, Fade(RAYWHITE, 0.62f));
    }
}

void MapEditor::DrawCanvas(Rectangle canvas) const
{
    DrawRectangleRec(canvas, Color{ 18, 19, 24, 255 });
    BeginScissorMode((int)canvas.x, (int)canvas.y, (int)canvas.width, (int)canvas.height);

    const Asset* asset = ActiveAsset();
    if (!asset || asset->texture.id == 0)
    {
        DrawText("Select a PNG asset from VillageAssets", (int)canvas.x + 40, (int)canvas.y + 40, 24, RAYWHITE);
        EndScissorMode();
        return;
    }

    Rectangle img = ActiveImageBoundsScreen();
    const int checker = 32;
    for (int y = (int)canvas.y; y < canvas.y + canvas.height; y += checker)
        for (int x = (int)canvas.x; x < canvas.x + canvas.width; x += checker)
        {
            bool dark = ((x / checker) + (y / checker)) % 2 == 0;
            DrawRectangle(x, y, checker, checker, dark ? Color{ 24, 25, 30, 255 } : Color{ 28, 29, 35, 255 });
        }

    DrawTexturePro(asset->texture,
                   AssetFrameSourceRect(*asset),
                   img, Vector2{ 0.f, 0.f }, 0.f, WHITE);
    DrawRectangleLinesEx(img, 2.f, Color{ 110, 114, 130, 255 });

    for (int i = 0; i < (int)asset->colliders.size(); ++i)
    {
        Rectangle sr = ImageRectToScreen(asset->colliders[i].rect);
        bool selected = i == _selectedCollider;
        Color fill = selected ? Fade(Color{ 255, 75, 55, 255 }, 0.34f) : Fade(Color{ 240, 50, 45, 255 }, 0.22f);
        Color line = selected ? GOLD : Color{ 245, 80, 70, 255 };
        DrawRectangleRec(sr, fill);
        DrawRectangleLinesEx(sr, selected ? 3.f : 2.f, line);
        DrawText(TextFormat("%d", i + 1), (int)sr.x + 4, (int)sr.y + 4, 16, line);
        if (selected)
        {
            Rectangle handles[] = {
                Rectangle{ sr.x - kHandleSize * 0.5f, sr.y - kHandleSize * 0.5f, kHandleSize, kHandleSize },
                Rectangle{ sr.x + sr.width - kHandleSize * 0.5f, sr.y - kHandleSize * 0.5f, kHandleSize, kHandleSize },
                Rectangle{ sr.x - kHandleSize * 0.5f, sr.y + sr.height - kHandleSize * 0.5f, kHandleSize, kHandleSize },
                Rectangle{ sr.x + sr.width - kHandleSize * 0.5f, sr.y + sr.height - kHandleSize * 0.5f, kHandleSize, kHandleSize },
            };
            for (const Rectangle& handle : handles)
                DrawRectangleRec(handle, GOLD);
        }
    }

    for (int i = 0; i < (int)asset->markers.size(); ++i)
    {
        const Marker& marker = asset->markers[i];
        Vector2 p = ImageToScreen(marker.pos);
        Color c = MarkerColor(marker.name);
        Vector2 nearestOnImage{
            ClampFloat(marker.pos.x, 0.f, (float)AssetFrameWidth(*asset)),
            ClampFloat(marker.pos.y, 0.f, (float)AssetFrameHeight(*asset))
        };
        if (nearestOnImage.x != marker.pos.x || nearestOnImage.y != marker.pos.y)
        {
            Vector2 anchor = ImageToScreen(nearestOnImage);
            DrawLineEx(anchor, p, 2.f, Fade(c, 0.55f));
            DrawCircleV(anchor, 5.f, Fade(c, 0.65f));
        }
        bool selected = i == _selectedMarker;
        DrawCircleV(p, selected ? 15.f : 12.f, Fade(c, selected ? 0.45f : 0.32f));
        DrawCircleLines((int)p.x, (int)p.y, selected ? 15.f : 12.f, selected ? GOLD : c);
        DrawLine((int)p.x - 16, (int)p.y, (int)p.x + 16, (int)p.y, c);
        DrawLine((int)p.x, (int)p.y - 16, (int)p.x, (int)p.y + 16, c);
        DrawText(marker.name.c_str(), (int)p.x + 14, (int)p.y - 10, 18, c);
    }

    if (_dragMode == DragMode::DrawCollider)
    {
        Rectangle sr = ImageRectToScreen(NormalizeImageRect(_drawPreview));
        DrawRectangleRec(sr, Fade(Color{ 255, 80, 60, 255 }, 0.18f));
        DrawRectangleLinesEx(sr, 2.f, Color{ 255, 110, 90, 255 });
    }

    if (_placingMarker)
    {
        Vector2 mouse = GetVirtualMousePos();
        if (CheckCollisionPointRec(mouse, canvas))
        {
            DrawCircleLines((int)mouse.x, (int)mouse.y, 10.f, GOLD);
            DrawText("Click to place marker", (int)mouse.x + 14, (int)mouse.y - 8, 16, GOLD);
        }
    }

    EndScissorMode();

    DrawText("Mouse wheel zoom | middle-drag pan | C add box | RMB drag new box | S save | H help",
             (int)canvas.x + 14, (int)canvas.y + 12, 18, Fade(RAYWHITE, 0.78f));
}

void MapEditor::DrawStatusBar() const
{
    Rectangle bar{ 0.f, (float)kVirtualHeight - kStatusH, (float)kVirtualWidth, kStatusH };
    DrawRectangleRec(bar, Color{ 36, 37, 45, 255 });
    const Asset* asset = ActiveAsset();
    const char* name = asset ? asset->name.c_str() : "no asset";
    const char* info = TextFormat("ASSET %s%s | colliders %d | drag/arrows move selected marker outside PNG | C/RMB collider | S save | ESC exit",
                                  name, (asset && asset->dirty) ? "*" : "", asset ? (int)asset->colliders.size() : 0);
    DrawText(info, 14, (int)bar.y + 10, 20, RAYWHITE);
    if (_statusTimer > 0.f && !_status.empty())
    {
        int w = MeasureText(_status.c_str(), 20);
        DrawText(_status.c_str(), kVirtualWidth - w - 16, (int)bar.y + 10, 20, GOLD);
    }
}

void MapEditor::DrawHelp() const
{
    DrawRectangle(0, 0, kVirtualWidth, kVirtualHeight, Fade(BLACK, 0.80f));
    const char* lines[] = {
        "VILLAGE ASSET ADJUSTER",
        "",
        "This edits metadata for finished PNGs in VillageAssets.",
        "The PNG is the visual. The .vasset file stores colliders, markers, and optional animation.",
        "",
        "Left panel: click ZephsShop, VillageGraveyard, or any future PNG asset.",
        "Canvas: mouse wheel zooms, middle mouse pans.",
        "",
        "Colliders: C adds a default box at mouse. Right-drag draws a new box.",
        "           W makes a full-sprite collider. N clears colliders for walk-through decor.",
        "           Collider boxes can extend outside the PNG; they save as local offsets.",
        "           Left-drag a box to move it. Drag its bottom-right handle to resize it.",
        "           Delete removes selected box. Arrows move selected; Ctrl+Arrows resize; Shift is faster.",
        "",
        "Buying:    [ ] changes cost. ,/. changes category. ;/' changes service. T/U/M toggles flags.",
        "Animation: A toggles sheet animation. I/K cols, O/L rows, -/= frames, PgUp/PgDn fps.",
        "Markers:   'New Marker' button, then click the canvas and type any name (Zeph, Poe,",
        "           Respawn, or your own) and press Enter. Zeph/Poe/Respawn keep their special colors;",
        "           any other name becomes a generic marker a service can look up later.",
        "           Markers may sit outside the PNG; the panel shows their pixel offset from it.",
        "           Click/drag a marker, or use Arrow keys after selecting it. Delete removes it.",
        "New Asset: click 'New Asset', type a name. If <name>.png is already in VillageAssets it",
        "           loads immediately; otherwise drop the PNG in and click New Asset again.",
        "Saving:    S writes VillageAssets/<asset>.vasset. ESC returns to menu.",
        "",
        "A saved asset's Service field (Shop, Training, DungeonGate, ...) is what makes it do",
        "something in-game the moment it's placed in the village build menu — no C++ needed.",
    };
    int y = 150;
    for (const char* line : lines)
    {
        int fs = (y == 150) ? 38 : 24;
        DrawText(line, kVirtualWidth / 2 - MeasureText(line, fs) / 2, y, fs, (y == 150) ? GOLD : RAYWHITE);
        y += (y == 150) ? 58 : 33;
    }
}
