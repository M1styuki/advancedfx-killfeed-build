// Native demo weather controller. External resource data retains its own license.
#define NOMINMAX
#include "stdafx.h"
#include "MirvWeather.h"
#include "Globals.h"
#include "SceneSystem.h"
#include "SchemaSystem.h"
#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "hlaeFolder.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;
extern bool getOffset(ptrdiff_t *, std::string, std::string, std::string);

namespace {
struct Vec3 { float x, y, z; };
using ManagerFn = void * (__fastcall *)();
using CreateFn = int * (__fastcall *)(void *, int *, const char *, int, int64_t, int64_t, int64_t, int64_t);
using UpdateFn = void (__fastcall *)(void *, int, int, const Vec3 *, float);
using DestroyFn = void (__fastcall *)(void *, int, bool, bool);
ManagerFn managerFn = nullptr;
CreateFn createFn = nullptr;
UpdateFn updateFn = nullptr;
DestroyFn destroyFn = nullptr;
ptrdiff_t postSettingsOffset = -1;
using WorldFn = void * (__fastcall *)(void *, uint32_t);
using RainContactFn = void (__fastcall *)(void *, void *, bool);
WorldFn worldFn = nullptr;
void ** contactSceneAddress = nullptr, ** contactWorldAddress = nullptr;
ptrdiff_t identityOffset = -1, worldGroupOffset = -1, rainContactOffset = -1;
bool contactSchemaReady = false, contactWarning = false, postSchemaWarning = false;
struct ContactState {
    SOURCESDK::CS2::CBaseHandle handle;
    uint32_t worldGroup;
    void * scene;
    void * world;
    bool original;
};
std::vector<ContactState> contactStates;
void ** mapInfoVtable = nullptr, ** postVolumeVtable = nullptr;
std::vector<SOURCESDK::CS2::CBaseHandle> mapInfoHandles, postVolumeHandles;
ULONGLONG lastEntityScan = 0;
bool requested = false, wantRain = true, wantGround = true, wantPost = true, wantContact = true;
bool configLoaded = false, loadAttempted = false, rainAttempted = false;
bool groundReady = false, postReady = false;
bool groundLoadAttempted = false, postLoadAttempted = false;
std::atomic<bool> groundActive { false };
std::shared_mutex materialMutex;
std::unordered_map<std::string, CMaterial2 *> wetMaterials;
std::vector<CMaterial2 **> retainedWetBindings;
bool resourcePinLayoutReady = false;
std::vector<Vec3> positions;
struct MaterialEntry { std::string original, wet; };
std::vector<MaterialEntry> materialEntries;
struct Particle { int index; void * record; };
std::vector<Particle> rainParticles;
void * rainManager = nullptr;
struct PostBackup { SOURCESDK::CS2::CBaseHandle handle; void ** original; };
std::vector<PostBackup> postBackups;
void ** postResource = nullptr;
int previousTick = -1;
const char * rainName = "particles/mirv_weather/dust2_rain.vpcf";

std::string Normalize(std::string s) {
    for(char & c : s) { if(c == '\\') c = '/'; else if(c >= 'A' && c <= 'Z') c += 'a' - 'A'; }
    if(s.size() > 2 && s.compare(s.size()-2, 2, "_c") == 0) s.resize(s.size()-2);
    return s;
}

bool IsDust2Demo() {
    if(!g_pEngineToClient) return false;
    auto * demo = g_pEngineToClient->GetDemoFile();
    if(!demo || !demo->IsPlayingDemo()) return false;
    const char * level = g_pEngineToClient->GetLevelNameShort();
    return level && Normalize(level) == "de_dust2";
}

// The matched destroy routine verifies these manager/record fields itself.
// Remember the record as well as its index so a seek cannot destroy a different
// particle when the manager has already discarded our original effect.
void * FindRecord(void * manager, int index) {
    if(!manager || index < 0) return nullptr;
    auto * bytes = static_cast<unsigned char *>(manager);
    int count = *reinterpret_cast<int *>(bytes + 0x98);
    auto ** records = *reinterpret_cast<void ***>(bytes + 0xA0);
    if(count < 0 || count > 32768 || !records) return nullptr;
    for(int i = 0; i < count; ++i) {
        auto * record = static_cast<unsigned char *>(records[i]);
        if(record && *reinterpret_cast<int *>(record + 0x38) == index) return record;
    }
    return nullptr;
}

void StopRain() {
    void * current = managerFn ? managerFn() : nullptr;
    if(destroyFn && current && current == rainManager) {
        for(const auto & p : rainParticles)
            if(p.record && FindRecord(current, p.index) == p.record) destroyFn(current, p.index, true, true);
    }
    rainParticles.clear();
    rainManager = nullptr;
    rainAttempted = false;
}

void RestorePost() {
    if(postSettingsOffset > 0 && g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
        for(const auto & b : postBackups) {
            auto * entity = GetEntityFromIndex(b.handle.GetEntryIndex());
            if(!entity || entity->GetHandle() != b.handle) continue;
            auto *** field = reinterpret_cast<void ***>(reinterpret_cast<unsigned char *>(entity) + postSettingsOffset);
            if(*field == postResource) *field = b.original;
        }
    }
    postBackups.clear();
}

bool ReadContact(CEntityInstance * entity, uint32_t & group, bool & enabled) {
    if(!entity || !contactSchemaReady) return false;
    auto * bytes = reinterpret_cast<unsigned char *>(entity);
    auto * identity = *reinterpret_cast<unsigned char **>(bytes + identityOffset);
    if(!identity) return false;
    group = *reinterpret_cast<uint32_t *>(identity + worldGroupOffset);
    enabled = *reinterpret_cast<bool *>(bytes + rainContactOffset);
    return true;
}

bool ResolveContact(uint32_t group, void *& scene, void *& world) {
    if(!worldFn || !contactSceneAddress || !contactWorldAddress) return false;
    scene = *contactSceneAddress;
    if(!scene || !*contactWorldAddress) return false;
    world = worldFn(*contactWorldAddress, group);
    return world != nullptr;
}

void PublishContact(void * scene, void * world, bool enabled) {
    // CMapInfo uses this same scene-world setter when its authored map data is
    // loaded. Do not call its entire initializer or add another rainfall source.
    auto ** vtable = *reinterpret_cast<void ***>(scene);
    auto setter = reinterpret_cast<RainContactFn>(vtable[0x1b8 / sizeof(void *)]);
    setter(scene, world, enabled);
}

void RestoreContact() {
    if(!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
        // Level teardown owns the destroyed scene worlds.
        contactStates.clear();
        return;
    }
    for(auto & state : contactStates) {
        void * scene = nullptr, * world = nullptr;
        if(!ResolveContact(state.worldGroup, scene, world) || scene != state.scene || world != state.world) continue;
        auto * entity = GetEntityFromIndex(state.handle.GetEntryIndex());
        if(entity && entity->GetHandle() == state.handle) {
            uint32_t group = 0;
            bool enabled = false;
            if(ReadContact(entity, group, enabled) && group == state.worldGroup) state.original = enabled;
        }
        PublishContact(scene, world, state.original);
    }
    contactStates.clear();
}

void DiscoverWeatherEntities() {
    if(!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;
    const ULONGLONG now = GetTickCount64();
    if(lastEntityScan && now - lastEntityScan < 1000) return;
    lastEntityScan = now;
    mapInfoHandles.clear();
    postVolumeHandles.clear();
    // The native getter accepts 0..0x7ffe, including client-only map objects.
    // The legacy script helper's hardcoded 2048 is not an entity namespace bound.
    for(int i = 0; i < 0x7fff; ++i) {
        auto * entity = GetEntityFromIndex(i);
        if(!entity) continue;
        auto ** vtable = *reinterpret_cast<void ***>(entity);
        if(vtable != mapInfoVtable && vtable != postVolumeVtable) continue;
        auto handle = entity->GetHandle();
        if(!handle.IsValid()) continue;
        if(vtable == mapInfoVtable) mapInfoHandles.push_back(handle);
        if(vtable == postVolumeVtable) postVolumeHandles.push_back(handle);
    }
}

void ApplyContact() {
    if(!contactSchemaReady || !worldFn || !g_pEntityList || !*g_pEntityList
        || !mapInfoVtable || !g_GetEntityFromIndex) {
        if(!contactWarning) {
            contactWarning = true;
            advancedfx::Warning("[mirv_weather] Native rain contact interface/schema unavailable; contact left unchanged.\n");
        }
        return;
    }
    for(const auto & cachedHandle : mapInfoHandles) {
        auto * entity = GetEntityFromIndex(cachedHandle.GetEntryIndex());
        if(!entity || entity->GetHandle() != cachedHandle) continue;
        auto handle = entity->GetHandle();
        uint32_t group = 0;
        bool original = false;
        void * scene = nullptr, * world = nullptr;
        if(!handle.IsValid() || !ReadContact(entity, group, original) || !ResolveContact(group, scene, world)) continue;
        auto it = contactStates.begin();
        while(it != contactStates.end() && it->handle != handle) ++it;
        if(it == contactStates.end()) {
            if(contactStates.size() >= 32) continue;
            contactStates.push_back({handle, group, scene, world, original});
        } else *it = {handle, group, scene, world, original};
        // The reference Dust2 profile changes only raintracetoskyenabled.
        // Preserve its existing rain strength, wetness and drying parameters.
        PublishContact(scene, world, true);
    }
}

bool ReadConfig() {
    if(loadAttempted) return configLoaded;
    loadAttempted = true;
    const auto root = std::filesystem::path(GetHlaeFolderW()) / L"resources/AfxHookSource2/cs2/mirv_weather";
    std::ifstream f(root / L"dust2.txt");
    std::string line;
    if(!std::getline(f, line) || line != "MIRV_DUST2_WEATHER_1") {
        advancedfx::Warning("[mirv_weather] Dust2 resource pack missing or unsupported. Install its resources folder in HLAE, then use mirv_weather reload.\n");
        return false;
    }
    while(std::getline(f, line)) {
        if(line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string type;
        row >> type;
        if(type == "rain") {
            Vec3 p {};
            if(!(row >> p.x >> p.y >> p.z) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
                || fabsf(p.x) > 32768 || fabsf(p.y) > 32768 || fabsf(p.z) > 32768 || positions.size() >= 512) break;
            positions.push_back(p);
        } else if(type == "material") {
            MaterialEntry e;
            if(!(row >> e.original >> e.wet) || e.original.find("materials/de_dust/") != 0
                || e.wet.find("materials/mirv_weather/") != 0 || e.wet.find("..") != std::string::npos
                || materialEntries.size() >= 32) break;
            e.original = Normalize(e.original);
            materialEntries.push_back(e);
        } else break;
    }
    if(!f.eof() || positions.size() != 385 || materialEntries.size() != 18) {
        positions.clear(); materialEntries.clear();
        advancedfx::Warning("[mirv_weather] Invalid Dust2 resource manifest; effects left off.\n");
        return false;
    }
    configLoaded = true;
    return true;
}

void LoadMaterials() {
    if(groundReady || groundLoadAttempted || !g_pCResourceSystem) return;
    groundLoadAttempted = true;
    if(!resourcePinLayoutReady) {
        advancedfx::Warning("[mirv_weather] Resource reference layout unavailable; ground left unchanged.\n");
        return;
    }
    std::unordered_map<std::string, CMaterial2 *> loaded;
    std::vector<CMaterial2 **> retained;
    for(const auto & e : materialEntries) {
        g_pCResourceSystem->PreCache(e.wet.c_str());
        auto ** material = FindRenderMaterial(e.wet.c_str());
        if(material) {
            InterlockedIncrement(reinterpret_cast<LONG *>(reinterpret_cast<unsigned char *>(material) + 0x20));
            retained.push_back(material);
        }
        if(!material || !*material || !MaterialRenderCallbacksReady(*material)) {
            advancedfx::Warning("[mirv_weather] Wet material has no callable render interface: %s; ground left unchanged.\n", e.wet.c_str());
            for(auto ** binding : retained) InterlockedDecrement(reinterpret_cast<LONG *>(reinterpret_cast<unsigned char *>(binding) + 0x20));
            return;
        }
        const char * name = material && *material ? (*material)->GetName() : nullptr;
        const std::string resolved = name ? Normalize(name) : std::string();
        // Renaming the loose compiled file does not rename its m_materialName.
        // Accept only the requested name or this manifest entry's authored name;
        // keep rejecting default/error materials and unrelated resource aliases.
        if(!name || (resolved != Normalize(e.wet) && resolved != e.original)) {
            advancedfx::Warning("[mirv_weather] Wet material could not be loaded: %s (resolved=%s).\n", e.wet.c_str(), name ? name : "<null>");
            for(auto ** binding : retained) InterlockedDecrement(reinterpret_cast<LONG *>(reinterpret_cast<unsigned char *>(binding) + 0x20));
            return;
        }
        // Current ResourceBinding reference count is +0x20 (verified against
        // the native zero-reference deletion check); hold our loaded resources.
        loaded[e.original] = *material;
    }
    std::unique_lock<std::shared_mutex> lock(materialMutex);
    wetMaterials.swap(loaded);
    retainedWetBindings.swap(retained);
    groundReady = true;
}

void StartRain() {
    if(rainAttempted || !managerFn || !createFn || !updateFn || !destroyFn || !g_pCResourceSystem) return;
    rainAttempted = true;
    // The generic resource precache wrapper accepts particle definitions too.
    auto ** resource = g_pCResourceSystem->PreCache(rainName);
    if(!resource || !*resource) {
        advancedfx::Warning("[mirv_weather] Rain particle resource could not be loaded.\n");
        return;
    }
    rainManager = managerFn();
    if(!rainManager) { rainAttempted = false; return; }
    for(const auto & position : positions) {
        int index = -1;
        // World-origin attachment, as used by the native spectator trail.
        createFn(rainManager, &index, rainName, 8, 0, 0, 0, 0);
        if(index < 0) continue;
        auto * record = FindRecord(rainManager, index);
        if(!record) continue;
        updateFn(rainManager, index, 0, &position, 0.0f);
        rainParticles.push_back({index, record});
    }
    advancedfx::Message("[mirv_weather] Rain hosts created: %zu/%zu.\n", rainParticles.size(), positions.size());
}

void ApplyPost() {
    if(postSettingsOffset <= 0 && !postSchemaWarning) {
        postSchemaWarning = true;
        advancedfx::Warning("[mirv_weather] Postprocessing schema unavailable; postprocessing left unchanged.\n");
    }
    if(postSettingsOffset <= 0 || !g_pCResourceSystem || !g_pEntityList || !*g_pEntityList
        || !postVolumeVtable || !g_GetEntityFromIndex) return;
    if(!postReady) {
        if(postLoadAttempted) return;
        postLoadAttempted = true;
        postResource = reinterpret_cast<void **>(g_pCResourceSystem->PreCache(
            "lighting/postprocessing/de_train_prefab/de_train_postprocess.vpost"));
        postReady = postResource && *postResource;
        if(!postReady) { advancedfx::Warning("[mirv_weather] Train postprocessing resource unavailable.\n"); return; }
    }
    for(const auto & cachedHandle : postVolumeHandles) {
        auto * entity = GetEntityFromIndex(cachedHandle.GetEntryIndex());
        if(!entity || entity->GetHandle() != cachedHandle) continue;
        auto handle = entity->GetHandle();
        if(!handle.IsValid()) continue;
        auto *** field = reinterpret_cast<void ***>(reinterpret_cast<unsigned char *>(entity) + postSettingsOffset);
        if(*field == postResource) continue;
        auto it = postBackups.begin();
        while(it != postBackups.end() && it->handle != handle) ++it;
        if(it == postBackups.end()) postBackups.push_back({handle, *field});
        else it->original = *field; // honor native changes made while enabled
        *field = postResource;
    }
}

void PrintStatus() {
    advancedfx::Message("[mirv_weather] requested=%d Dust2Demo=%d pack=%d; rain=%d hosts=%zu native=%d; ground=%d materials=%zu; postprocess=%d volumes=%zu schema=%d.\n",
        requested, IsDust2Demo(), configLoaded, wantRain, rainParticles.size(), destroyFn && managerFn && createFn && updateFn,
        groundActive.load(), wetMaterials.size(), wantPost, postBackups.size(), postSettingsOffset > 0);
    advancedfx::Message("[mirv_weather] contact requested=%d active=%d worlds=%zu interface=%d schema=%d.\n",
        wantContact && MirvPovDebug_IsFeatureEnabled("weather_contact"), !contactStates.empty(), contactStates.size(),
        worldFn != nullptr, contactSchemaReady);
    advancedfx::Message("[mirv_weather] map objects: mapInfo=%zu postVolumes=%zu RTTI=%d/%d; ground requested=%d debug=%d ready=%d.\n",
        mapInfoHandles.size(), postVolumeHandles.size(), mapInfoVtable != nullptr, postVolumeVtable != nullptr,
        wantGround, MirvPovDebug_IsFeatureEnabled("weather_ground"), groundReady);
}

size_t UniqueAddress(HMODULE dll, const char * pattern) {
    Afx::BinUtils::ImageSectionsReader sections(dll);
    auto range = sections.GetMemRange();
    auto found = FindPatternString(range, pattern);
    if(found.IsEmpty()) return 0;
    Afx::BinUtils::MemRange remainder(found.Start + 1, range.End);
    if(!FindPatternString(remainder, pattern).IsEmpty()) return 0;
    return found.Start;
}
} // namespace

void MirvWeather_Initialize(HMODULE clientDll) {
    if(HMODULE resources = GetModuleHandleA("resourcesystem.dll"))
        resourcePinLayoutReady = 0 != UniqueAddress(resources, "8B 43 20 85 C0 0F 84 ?? ?? ?? ?? 48 0F BE 43 1C 48 83 C5 D0 3C FF");
    // These non-networked map classes inherit a base network-class descriptor.
    // Match their actual dynamic type instead of GetClientClassName().
    mapInfoVtable = reinterpret_cast<void **>(Afx::BinUtils::FindClassVtable(clientDll, ".?AVCMapInfo@@", 0, 0));
    postVolumeVtable = reinterpret_cast<void **>(Afx::BinUtils::FindClassVtable(clientDll, ".?AVC_PostProcessingVolume@@", 0, 0));
    managerFn = reinterpret_cast<ManagerFn>(UniqueAddress(clientDll, "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 89 5C 24 10 57"));
    createFn = reinterpret_cast<CreateFn>(UniqueAddress(clientDll, "4C 8B DC 53 48 81 EC ?? ?? ?? ?? F2 0F 10 05"));
    updateFn = reinterpret_cast<UpdateFn>(UniqueAddress(clientDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? F3 0F 10 1D ?? ?? ?? ?? 41 8B F8 8B DA 4C 8D 05"));
    destroyFn = reinterpret_cast<DestroyFn>(UniqueAddress(clientDll, "83 FA FF 0F 84 ?? ?? ?? ?? 41 54 41 56 41 57 48 83 EC 40 48 89 5C 24 60 45 0F B6 E1 33 DB 48 89 74 24 38 48 8D B1 98 00 00 00 45 0F B6 F0"));
    size_t callSite = UniqueAddress(clientDll, "48 8B 05 ?? ?? ?? ?? 48 8B 56 10 0F B6 9E 19 06 00 00 48 8B 08 8B 52 38 48 8B B9 B8 01 00 00 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 44 0F B6 C3 48 8B D0 FF D7");
    if(callSite) {
        contactSceneAddress = reinterpret_cast<void **>(callSite + 7 + *reinterpret_cast<int32_t *>(callSite + 3));
        contactWorldAddress = reinterpret_cast<void **>(callSite + 38 + *reinterpret_cast<int32_t *>(callSite + 34));
        worldFn = reinterpret_cast<WorldFn>(callSite + 43 + *reinterpret_cast<int32_t *>(callSite + 39));
    }
}

void MirvWeather_ResolveSchemaOffsets() {
    // HookSchemaSystem clears the temporary schema table after initialization.
    // Resolve optional weather fields while that table is still populated.
    getOffset(&postSettingsOffset, "client.dll", "C_PostProcessingVolume", "m_hPostSettings");
    getOffset(&identityOffset, "client.dll", "CEntityInstance", "m_pEntity");
    getOffset(&worldGroupOffset, "client.dll", "CEntityIdentity", "m_worldGroupId");
    getOffset(&rainContactOffset, "client.dll", "CMapInfo", "m_bRainTraceToSkyEnabled");
    // The verified native signature embeds this layout; do not guess if it changes.
    contactSchemaReady = identityOffset == 0x10 && worldGroupOffset == 0x38 && rainContactOffset == 0x619;
}

void MirvWeather_Reset() {
    groundActive.store(false);
    StopRain();
    RestorePost();
    RestoreContact();
    mapInfoHandles.clear();
    postVolumeHandles.clear();
    lastEntityScan = 0;
    std::unique_lock<std::shared_mutex> lock(materialMutex);
    wetMaterials.clear();
    for(auto ** binding : retainedWetBindings)
        InterlockedDecrement(reinterpret_cast<LONG *>(reinterpret_cast<unsigned char *>(binding) + 0x20));
    retainedWetBindings.clear();
    groundReady = false;
    groundLoadAttempted = postLoadAttempted = false;
    postReady = false;
    postResource = nullptr;
    previousTick = -1;
}

void MirvWeather_Frame() {
    const bool active = requested && IsDust2Demo();
    if(!active) { if(rainAttempted || !postBackups.empty() || !contactStates.empty() || groundActive.load()) MirvWeather_Reset(); return; }
    if(!ReadConfig()) return;
    const int tick = g_pEngineToClient->GetDemoFile()->GetDemoTick();
    // Avoid retaining particle state across a rewind or large demo jump.
    if(previousTick >= 0 && (tick < previousTick || tick - previousTick > 128)) { StopRain(); RestorePost(); RestoreContact(); lastEntityScan = 0; }
    previousTick = tick;
    DiscoverWeatherEntities();
    if(!rainParticles.empty() && (managerFn() != rainManager
        || FindRecord(rainManager, rainParticles.front().index) != rainParticles.front().record
        || FindRecord(rainManager, rainParticles.back().index) != rainParticles.back().record)) StopRain();
    if(wantRain && MirvPovDebug_IsFeatureEnabled("weather_rain")) StartRain(); else StopRain();
    if(wantGround && MirvPovDebug_IsFeatureEnabled("weather_ground")) LoadMaterials();
    groundActive.store(wantGround && groundReady && MirvPovDebug_IsFeatureEnabled("weather_ground"));
    if(wantPost && MirvPovDebug_IsFeatureEnabled("weather_postprocess")) ApplyPost(); else RestorePost();
    if(wantContact && MirvPovDebug_IsFeatureEnabled("weather_contact")) ApplyContact(); else RestoreContact();
}

bool MirvWeather_HasGroundOverride() { return groundActive.load(); }

CMaterial2 * MirvWeather_Material(CMaterial2 * original) {
    if(!original || !groundActive.load()) return original;
    std::shared_lock<std::shared_mutex> lock(materialMutex);
    const char * name = original->GetName();
    if(!name) return original;
    auto it = wetMaterials.find(Normalize(name));
    return it == wetMaterials.end() ? original : it->second;
}

CON_COMMAND(mirv_weather, "Dust2 demo rain, native rain contact, wet ground and postprocessing") {
    if(args->ArgC() == 2 && (!strcmp(args->ArgV(1), "0") || !strcmp(args->ArgV(1), "1"))) {
        requested = args->ArgV(1)[0] == '1';
        if(!requested) MirvWeather_Reset();
    } else if(args->ArgC() == 3 && (!strcmp(args->ArgV(2), "0") || !strcmp(args->ArgV(2), "1"))) {
        bool * target = nullptr;
        if(!_stricmp(args->ArgV(1), "rain")) target = &wantRain;
        if(!_stricmp(args->ArgV(1), "ground")) target = &wantGround;
        if(!_stricmp(args->ArgV(1), "postprocess")) target = &wantPost;
        if(!_stricmp(args->ArgV(1), "contact")) target = &wantContact;
        if(target) *target = args->ArgV(2)[0] == '1';
    } else if(args->ArgC() == 2 && !_stricmp(args->ArgV(1), "reload")) {
        MirvWeather_Reset();
        configLoaded = loadAttempted = false;
        positions.clear(); materialEntries.clear();
    } else if(args->ArgC() != 2 || _stricmp(args->ArgV(1), "status")) {
        advancedfx::Message("mirv_weather 0|1; mirv_weather rain|ground|postprocess|contact 0|1; mirv_weather status|reload.\nWeather requires the Dust2 resource pack in HLAE resources. Sky remains controlled by mirv_sky material.\n");
    }
    MirvWeather_Frame();
    PrintStatus();
}
