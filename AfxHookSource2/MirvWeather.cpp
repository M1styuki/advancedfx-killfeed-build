// Native demo weather controller. External resource data retains its own license.
#define NOMINMAX
#include "stdafx.h"
#include "MirvWeather.h"
#include "MirvWeatherStorm.h"
#include "MirvTime.h"
#include "Globals.h"
#include "SceneSystem.h"
#include "SchemaSystem.h"
#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "hlaeFolder.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <atomic>
#include <array>
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
using RainEnvironmentFn = void (__fastcall *)(void *, void *, float, float, float, float, float);
struct EnvironmentState {
    SOURCESDK::CS2::CBaseHandle handle;
    uint32_t worldGroup;
    void * scene;
    void * world;
    std::array<float, 5> original;
};
std::vector<EnvironmentState> environmentStates;
std::array<ptrdiff_t, 5> environmentOffsets {{ -1, -1, -1, -1, -1 }};
std::array<float, 5> weatherEnvironment {{ 1, 1, 0, 1, 0 }};
bool environmentSignatureReady = false, environmentSchemaReady = false, environmentWarning = false;
struct MapProfile { const char * map; const char * file; size_t rainCount; size_t materialCount; };
const MapProfile mapProfiles[] = {
    { "de_dust2", "dust2.txt", 385, 18 },
    { "de_mirage", "mirage.txt", 832, 12 },
    { "de_cache", "cache.txt", 969, 3 },
    { "de_inferno", "inferno.txt", 604, 2 },
    { "de_ancient", "ancient.txt", 166, 0 },
    { "de_nuke", "nuke.txt", 221, 0 },
    { "de_anubis", "anubis.txt", 1004, 0 }
};
const MapProfile * loadedProfile = nullptr;
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
bool postLifetimeLayoutReady = false;
std::vector<Vec3> positions;
struct MaterialEntry { std::string original, wet; };
std::vector<MaterialEntry> materialEntries;
struct Particle { int index; void * record; };
std::vector<Particle> rainParticles;
std::vector<void **> audioBindings;
bool audioPrecacheFailed = false;
// Diagnostic submission counters for the owned native rain particle path. They
// record how far a spawn attempt got, not whether anything became visible.
struct ParticleCounters {
    std::atomic<unsigned> tries {0};
    std::atomic<unsigned> definitionFails {0};
    std::atomic<unsigned> createFails {0};
    std::atomic<unsigned> created {0};
    std::atomic<unsigned> live {0};
    std::atomic<int> stage {MirvWeatherParticleStage_None};
};
ParticleCounters rainCounters;
void * rainManager = nullptr;
void ** rainResource = nullptr;
struct PostBackup { SOURCESDK::CS2::CBaseHandle handle; void ** original; };
std::vector<PostBackup> postBackups;
void ** postResource = nullptr;
int previousTick = -1;
const char * rainName = "particles/mirv_weather/dust2_rain.vpcf";

// C_PostProcessingVolume's destructor releases its m_hPostSettings binding at
// +0x20. A raw pointer swap must therefore transfer the field's owned reference.
void RetainBinding(void ** binding) {
    if(binding) InterlockedIncrement(reinterpret_cast<LONG *>(reinterpret_cast<unsigned char *>(binding) + 0x20));
}

void ReleaseBinding(void ** binding) {
    if(binding) InterlockedDecrement(reinterpret_cast<LONG *>(reinterpret_cast<unsigned char *>(binding) + 0x20));
}

void ReplacePostBinding(void *** field, void ** replacement) {
    if(*field == replacement) return;
    RetainBinding(replacement);
    void ** previous = *field;
    *field = replacement;
    ReleaseBinding(previous);
}

std::string Normalize(std::string s) {
    for(char & c : s) { if(c == '\\') c = '/'; else if(c >= 'A' && c <= 'Z') c += 'a' - 'A'; }
    if(s.size() > 2 && s.compare(s.size()-2, 2, "_c") == 0) s.resize(s.size()-2);
    return s;
}

const MapProfile * GetDemoProfile() {
    if(!g_pEngineToClient) return nullptr;
    auto * demo = g_pEngineToClient->GetDemoFile();
    if(!demo || !demo->IsPlayingDemo()) return nullptr;
    const char * level = g_pEngineToClient->GetLevelNameShort();
    if(level) {
        const std::string name = Normalize(level);
        for(const auto & profile : mapProfiles) if(name == profile.map) return &profile;
    }
    return nullptr;
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
    rainCounters.live.store(0);
    rainManager = nullptr;
    rainAttempted = false;
    ReleaseBinding(rainResource);
    rainResource = nullptr;
}

void RestorePost() {
    if(postSettingsOffset > 0 && g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
        for(const auto & b : postBackups) {
            auto * entity = GetEntityFromIndex(b.handle.GetEntryIndex());
            if(!entity || entity->GetHandle() != b.handle) continue;
            auto *** field = reinterpret_cast<void ***>(reinterpret_cast<unsigned char *>(entity) + postSettingsOffset);
            if(*field == postResource) ReplacePostBinding(field, b.original);
        }
    }
    for(const auto & b : postBackups) ReleaseBinding(b.original);
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

bool ReadEnvironment(CEntityInstance * entity, std::array<float, 5> & values) {
    if(!entity || !environmentSchemaReady) return false;
    for(size_t i = 0; i < values.size(); ++i) {
        values[i] = *reinterpret_cast<float *>(reinterpret_cast<unsigned char *>(entity) + environmentOffsets[i]);
        if(!std::isfinite(values[i])) return false;
    }
    return true;
}

void PublishEnvironment(void * scene, void * world, const std::array<float, 5> & values) {
    // Same five-float publication used by native CMapInfo initialization.
    auto ** table = *reinterpret_cast<void ***>(scene);
    auto setter = reinterpret_cast<RainEnvironmentFn>(table[0x1c8 / sizeof(void *)]);
    setter(scene, world, values[0], values[1], values[2], values[3], values[4]);
}

void RestoreEnvironment() {
    if(g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
        for(auto & state : environmentStates) {
            auto * entity = GetEntityFromIndex(state.handle.GetEntryIndex());
            if(!entity || entity->GetHandle() != state.handle) continue;
            uint32_t group = 0;
            bool enabled = false;
            void * scene = nullptr, * world = nullptr;
            if(!ReadContact(entity, group, enabled) || group != state.worldGroup
                || !ResolveContact(group, scene, world) || scene != state.scene || world != state.world) continue;
            // We publish to the scene, not to entity fields. Native values remain
            // the current source of truth even if the map changed them while on.
            if(ReadEnvironment(entity, state.original)) PublishEnvironment(scene, world, state.original);
        }
    }
    environmentStates.clear();
}

void ApplyEnvironment() {
    if(!environmentSignatureReady || !environmentSchemaReady || !contactSchemaReady
        || !worldFn || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) {
        if(!environmentWarning) {
            environmentWarning = true;
            advancedfx::Warning("[mirv_weather] Native environment interface/schema unavailable; environment left unchanged.\n");
        }
        return;
    }
    for(const auto & handle : mapInfoHandles) {
        auto * entity = GetEntityFromIndex(handle.GetEntryIndex());
        if(!entity || entity->GetHandle() != handle) continue;
        uint32_t group = 0;
        bool enabled = false;
        void * scene = nullptr, * world = nullptr;
        std::array<float, 5> original {};
        if(!ReadContact(entity, group, enabled) || !ReadEnvironment(entity, original)
            || !ResolveContact(group, scene, world)) continue;
        auto it = environmentStates.begin();
        while(it != environmentStates.end() && it->handle != handle) ++it;
        if(it == environmentStates.end()) {
            if(environmentStates.size() >= 32) continue;
            environmentStates.push_back({handle, group, scene, world, original});
        } else *it = {handle, group, scene, world, original};
        PublishEnvironment(scene, world, weatherEnvironment);
    }
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
    if(!loadedProfile) return false;
    loadAttempted = true;
    const auto root = std::filesystem::path(GetHlaeFolderW()) / L"resources/AfxHookSource2/cs2/mirv_weather";
    std::ifstream f(root / loadedProfile->file);
    std::string line;
    std::string magic, map, extra;
    size_t rainCount = 0, materialCount = 0;
    if(std::getline(f, line)) {
        std::istringstream header(line);
        header >> magic >> map >> rainCount >> materialCount;
        header >> extra;
    }
    if(magic != "MIRV_MAP_WEATHER_2" || map != loadedProfile->map || !extra.empty()
        || rainCount != loadedProfile->rainCount || materialCount != loadedProfile->materialCount) {
        advancedfx::Warning("[mirv_weather] %s resource profile missing or unsupported. Install the seven-map resources, then use mirv_weather reload.\n", loadedProfile->map);
        return false;
    }
    bool hasEnvironment = false, parsed = true;
    while(std::getline(f, line)) {
        if(line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string type;
        row >> type;
        if(type == "environment") {
            if(hasEnvironment) { parsed = false; break; }
            bool valid = true;
            for(float & value : weatherEnvironment)
                if(!(row >> value) || !std::isfinite(value) || value < 0 || value > 1) valid = false;
            if(!valid) { parsed = false; break; }
            hasEnvironment = true;
        } else if(type == "rain") {
            Vec3 p {};
            if(!(row >> p.x >> p.y >> p.z) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
                || fabsf(p.x) > 32768 || fabsf(p.y) > 32768 || fabsf(p.z) > 32768 || positions.size() >= loadedProfile->rainCount) { parsed = false; break; }
            positions.push_back(p);
        } else if(type == "material") {
            MaterialEntry e;
            if(!(row >> e.original >> e.wet) || e.original.find("materials/") != 0
                || e.wet.find("materials/mirv_weather/") != 0 || e.wet.find("..") != std::string::npos
                || e.original.find("..") != std::string::npos || e.original.size() > 256 || e.wet.size() > 256
                || e.original.size() < 6 || e.original.compare(e.original.size()-5, 5, ".vmat") != 0
                || e.wet.size() < 6 || e.wet.compare(e.wet.size()-5, 5, ".vmat") != 0
                || materialEntries.size() >= loadedProfile->materialCount) { parsed = false; break; }
            e.original = Normalize(e.original);
            materialEntries.push_back(e);
        } else { parsed = false; break; }
        if(row >> extra) { parsed = false; break; }
    }
    if(!parsed || !f.eof() || !hasEnvironment || positions.size() != rainCount || materialEntries.size() != materialCount) {
        positions.clear(); materialEntries.clear();
        advancedfx::Warning("[mirv_weather] Invalid %s resource profile; effects left off.\n", loadedProfile->map);
        return false;
    }
    configLoaded = true;
    return true;
}

void LoadMaterials() {
    if(materialEntries.empty()) return; // native ground / rain-only map profile
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
    // Each guard records the early-return reason so a failed rain start can be
    // told apart from a missing interface, layout or definition.
    if(!managerFn || !createFn || !updateFn || !destroyFn) { rainCounters.stage.store(MirvWeatherParticleStage_NativeInterface); return; }
    if(!g_pCResourceSystem) { rainCounters.stage.store(MirvWeatherParticleStage_ResourceSystem); return; }
    if(!resourcePinLayoutReady) { rainCounters.stage.store(MirvWeatherParticleStage_Layout); return; }
    if(rainAttempted) return;
    rainAttempted = true;
    rainCounters.tries.fetch_add(1);
    // The generic resource precache wrapper accepts particle definitions too.
    auto ** resource = g_pCResourceSystem->PreCache(rainName);
    if(!resource || !*resource) {
        rainCounters.definitionFails.fetch_add(1);
        rainCounters.stage.store(MirvWeatherParticleStage_Definition);
        advancedfx::Warning("[mirv_weather] Rain particle resource could not be loaded.\n");
        return;
    }
    rainManager = managerFn();
    if(!rainManager) { rainAttempted = false; rainCounters.stage.store(MirvWeatherParticleStage_Manager); return; }
    rainResource = reinterpret_cast<void **>(resource);
    RetainBinding(rainResource);
    for(const auto & position : positions) {
        int index = -1;
        // World-origin attachment, as used by the native spectator trail.
        createFn(rainManager, &index, rainName, 8, 0, 0, 0, 0);
        if(index < 0) { rainCounters.createFails.fetch_add(1); rainCounters.stage.store(MirvWeatherParticleStage_Create); continue; }
        auto * record = FindRecord(rainManager, index);
        if(!record) { rainCounters.createFails.fetch_add(1); rainCounters.stage.store(MirvWeatherParticleStage_Record); continue; }
        updateFn(rainManager, index, 0, &position, 0.0f);
        rainParticles.push_back({index, record});
        rainCounters.created.fetch_add(1);
    }
    rainCounters.live.store(static_cast<unsigned>(rainParticles.size()));
    if(rainParticles.size() == positions.size()) rainCounters.stage.store(MirvWeatherParticleStage_None);
    advancedfx::Message("[mirv_weather] Rain hosts created: %zu/%zu.\n", rainParticles.size(), positions.size());
}

void ApplyPost() {
    if((postSettingsOffset != 0x1190 || !postLifetimeLayoutReady || !resourcePinLayoutReady) && !postSchemaWarning) {
        postSchemaWarning = true;
        advancedfx::Warning("[mirv_weather] Postprocessing ownership layout unavailable; postprocessing left unchanged.\n");
    }
    if(postSettingsOffset != 0x1190 || !postLifetimeLayoutReady || !resourcePinLayoutReady || !g_pCResourceSystem || !g_pEntityList || !*g_pEntityList
        || !postVolumeVtable || !g_GetEntityFromIndex) return;
    if(!postReady) {
        if(postLoadAttempted) return;
        postLoadAttempted = true;
        postResource = reinterpret_cast<void **>(g_pCResourceSystem->PreCache(
            "lighting/postprocessing/de_train_prefab/de_train_postprocess.vpost"));
        postReady = postResource && *postResource;
        if(!postReady) { postResource = nullptr; advancedfx::Warning("[mirv_weather] Train postprocessing resource unavailable.\n"); return; }
        RetainBinding(postResource); // controller ownership, separate from each entity field
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
        RetainBinding(*field); // keep the original alive until restoration, including seeks
        if(it == postBackups.end()) postBackups.push_back({handle, *field});
        else {
            ReleaseBinding(it->original);
            it->original = *field; // honor native changes made while enabled
        }
        ReplacePostBinding(field, postResource);
    }
}

void PrintStatus() {
    const auto * profile = GetDemoProfile();
    advancedfx::Message("[mirv_weather] requested=%d map=%s supportedDemo=%d pack=%d; rain=%d hosts=%zu/%zu native=%d; ground=%d materials=%zu/%zu; postprocess=%d volumes=%zu schema=%d.\n",
        requested, profile ? profile->map : "unsupported", profile != nullptr, configLoaded,
        wantRain, rainParticles.size(), profile ? profile->rainCount : 0, destroyFn && managerFn && createFn && updateFn,
        groundActive.load(), wetMaterials.size(), profile ? profile->materialCount : 0, wantPost, postBackups.size(), postSettingsOffset > 0);
    advancedfx::Message("[mirv_weather] contact requested=%d active=%d worlds=%zu interface=%d schema=%d.\n",
        wantContact && MirvPovDebug_IsFeatureEnabled("weather_contact"), !contactStates.empty(), contactStates.size(),
        worldFn != nullptr, contactSchemaReady);
    advancedfx::Message("[mirv_weather] map objects: mapInfo=%zu postVolumes=%zu RTTI=%d/%d; ground requested=%d debug=%d ready=%d.\n",
        mapInfoHandles.size(), postVolumeHandles.size(), mapInfoVtable != nullptr, postVolumeVtable != nullptr,
        wantGround, MirvPovDebug_IsFeatureEnabled("weather_ground"), groundReady);
    advancedfx::Message("[mirv_weather] environment requested=%d worlds=%zu interface=%d schema=%d; ground profile=%s.\n",
        MirvPovDebug_IsFeatureEnabled("weather_environment"), environmentStates.size(), environmentSignatureReady,
        environmentSchemaReady, profile && profile->materialCount ? "wet-materials" : "native-ground");
    // Particle submission counters. They show how far the owned rain path got;
    // a created/live count is not proof that the effect is visible.
    const MirvWeatherParticleStatus particles=MirvWeather_ParticleStatus();
    advancedfx::Message("[mirv_weather] rain spawn: tries=%u def=%u fail=%u created=%u live=%u stage=%s; native=%d layout=%d resources=%d manager=%d.\n",
        particles.rain.tries,particles.rain.definitionFails,particles.rain.createFails,particles.rain.created,particles.rain.live,
        MirvWeather_ParticleStageName(particles.rain.stage),
        destroyFn && managerFn && createFn && updateFn,resourcePinLayoutReady,g_pCResourceSystem!=nullptr,managerFn!=nullptr);
    MirvWeatherStorm_Status();
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
    MirvWeatherStorm_Initialize(clientDll);
    if(HMODULE resources = GetModuleHandleA("resourcesystem.dll"))
        resourcePinLayoutReady = 0 != UniqueAddress(resources, "8B 43 20 85 C0 0F 84 ?? ?? ?? ?? 48 0F BE 43 1C 48 83 C5 D0 3C FF");
    postLifetimeLayoutReady = 0 != UniqueAddress(clientDll, "48 8B 91 90 11 00 00 48 85 D2 74 ?? 48 83 3D ?? ?? ?? ?? 00 74 ?? F0 FF 4A 20");
    environmentSignatureReady = 0 != UniqueAddress(clientDll, "F3 0F 10 B6 2C 06 00 00 F3 0F 10 BE 28 06 00 00 48 8B 08 8B 52 38 F3 44 0F 10 86 24 06 00 00 F3 44 0F 10 8E 20 06 00 00");
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
    MirvWeatherStorm_ResolveSchema();
    // HookSchemaSystem clears the temporary schema table after initialization.
    // Resolve optional weather fields while that table is still populated.
    getOffset(&postSettingsOffset, "client.dll", "C_PostProcessingVolume", "m_hPostSettings");
    getOffset(&identityOffset, "client.dll", "CEntityInstance", "m_pEntity");
    getOffset(&worldGroupOffset, "client.dll", "CEntityIdentity", "m_worldGroupId");
    getOffset(&rainContactOffset, "client.dll", "CMapInfo", "m_bRainTraceToSkyEnabled");
    // The verified native signature embeds this layout; do not guess if it changes.
    contactSchemaReady = identityOffset == 0x10 && worldGroupOffset == 0x38 && rainContactOffset == 0x619;
    const char * names[] = { "m_flEnvRainStrength", "m_flEnvPuddleRippleStrength", "m_flEnvPuddleRippleDirection", "m_flEnvWetnessCoverage", "m_flEnvWetnessDryingAmount" };
    environmentSchemaReady = true;
    for(size_t i = 0; i < environmentOffsets.size(); ++i) {
        getOffset(&environmentOffsets[i], "client.dll", "CMapInfo", names[i]);
        if(environmentOffsets[i] != 0x61c + static_cast<ptrdiff_t>(i * 4)) environmentSchemaReady = false;
    }
}

void MirvWeather_Reset() {
    MirvWeatherStorm_Reset();
    groundActive.store(false);
    StopRain();
    RestorePost();
    RestoreEnvironment();
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
    ReleaseBinding(postResource);
    postResource = nullptr;
    previousTick = -1;
    loadedProfile = nullptr;
    configLoaded = loadAttempted = false;
    // A resource lookup that failed transiently (for example while the map's
    // resource system was still warming up) must be retryable; without this a
    // single failure would disable storm audio until process restart.
    audioPrecacheFailed = false;
    positions.clear();
    materialEntries.clear();
}

void MirvWeather_Frame(bool fromRenderFrame) {
    const MapProfile * profile = requested ? GetDemoProfile() : nullptr;
    if(!profile) { if(loadedProfile) MirvWeather_Reset(); return; }
    // The master preference survives map changes, all map-owned resources do not.
    if(profile != loadedProfile) {
        MirvWeather_Reset();
        loadedProfile = profile;
    }
    if(!ReadConfig()) return;
    const int tick = g_pEngineToClient->GetDemoFile()->GetDemoTick();
    // Avoid retaining particle state across a rewind or large demo jump.
    if(previousTick >= 0 && (tick < previousTick || tick - previousTick > 128)) { MirvWeatherStorm_Reset(); StopRain(); RestorePost(); RestoreEnvironment(); RestoreContact(); lastEntityScan = 0; }
    previousTick = tick;
    DiscoverWeatherEntities();
    if(!rainParticles.empty() && (managerFn() != rainManager
        || FindRecord(rainManager, rainParticles.front().index) != rainParticles.front().record
        || FindRecord(rainManager, rainParticles.back().index) != rainParticles.back().record)) StopRain();
    if(wantRain && MirvPovDebug_IsFeatureEnabled("weather_rain")) StartRain(); else StopRain();
    if(wantGround && MirvPovDebug_IsFeatureEnabled("weather_ground")) LoadMaterials();
    groundActive.store(wantGround && groundReady && MirvPovDebug_IsFeatureEnabled("weather_ground"));
    if(wantPost && MirvPovDebug_IsFeatureEnabled("weather_postprocess")) ApplyPost(); else RestorePost();
    if(MirvPovDebug_IsFeatureEnabled("weather_environment")) ApplyEnvironment(); else RestoreEnvironment();
    if(wantContact && MirvPovDebug_IsFeatureEnabled("weather_contact")) ApplyContact(); else RestoreContact();
    MirvWeatherStorm_Frame(true, fromRenderFrame);
}

bool MirvWeather_RetainAudioData() {
    // A failed definition lookup is not retried every frame: the weather
    // resource pack is static, and per-frame retries would both spam and
    // repeatedly call the native precache path for the same missing resource.
    // It is cleared by MirvWeather_Reset instead, so a reload, map change or
    // master off/on can recover from a transient resource failure.
    if(audioPrecacheFailed) return false;
    if(!resourcePinLayoutReady || !g_pCResourceSystem) return false;
    if(!audioBindings.empty()) return true;
    for(const char * name:{"soundevents/ambience/game_sounds_train.vsndevts","soundevents/ambience/game_sounds_amb_common.vsndevts"}) {
        auto ** binding=reinterpret_cast<void **>(g_pCResourceSystem->PreCache(name));
        if(!binding || !*binding) {
            MirvWeather_ReleaseAudioData();
            audioPrecacheFailed=true;
            advancedfx::Warning("[mirv_weather] Native rain/thunder sound definitions unavailable; storm audio left off.\n");
            return false;
        }
        RetainBinding(binding);audioBindings.push_back(binding);
    }
    return true;
}
void MirvWeather_ReleaseAudioData() {
    for(auto ** binding:audioBindings)ReleaseBinding(binding);
    audioBindings.clear();
}

const char * MirvWeather_ParticleStageName(int stage) {
    switch(stage) {
    case MirvWeatherParticleStage_None: return "none";
    case MirvWeatherParticleStage_Layout: return "layout";
    case MirvWeatherParticleStage_NativeInterface: return "native-interface";
    case MirvWeatherParticleStage_ResourceSystem: return "resource-system";
    case MirvWeatherParticleStage_Manager: return "manager";
    case MirvWeatherParticleStage_Definition: return "definition";
    case MirvWeatherParticleStage_Create: return "create";
    case MirvWeatherParticleStage_Record: return "record";
    default: return "unknown";
    }
}

static MirvWeatherParticleResourceStatus ParticleResourceStatus(const ParticleCounters & counters) {
    MirvWeatherParticleResourceStatus status {};
    status.tries=counters.tries.load();
    status.definitionFails=counters.definitionFails.load();
    status.createFails=counters.createFails.load();
    status.created=counters.created.load();
    status.live=counters.live.load();
    status.stage=counters.stage.load();
    return status;
}

MirvWeatherParticleStatus MirvWeather_ParticleStatus() {
    MirvWeatherParticleStatus status {};
    status.rain=ParticleResourceStatus(rainCounters);
    return status;
}

int MirvWeather_AudioDefinitionCount() { return static_cast<int>(audioBindings.size()); }
int MirvWeather_AudioDefinitionTargets() { return 2; }
bool MirvWeather_AudioPrecacheFailed() { return audioPrecacheFailed; }

bool MirvWeather_HasGroundOverride() { return groundActive.load(); }

CMaterial2 * MirvWeather_Material(CMaterial2 * original) {
    if(!original || !groundActive.load()) return original;
    std::shared_lock<std::shared_mutex> lock(materialMutex);
    const char * name = original->GetName();
    if(!name) return original;
    auto it = wetMaterials.find(Normalize(name));
    return it == wetMaterials.end() ? original : it->second;
}

CON_COMMAND(mirv_weather, "Seven-map demo rain, native rain contact/environment, wet ground and postprocessing") {
    if(MirvWeatherStorm_Command(args)) {
        // Storm options are independent of the base rain/ground profile.
    } else if(args->ArgC() == 2 && (!strcmp(args->ArgV(1), "0") || !strcmp(args->ArgV(1), "1"))) {
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
        advancedfx::Message("mirv_weather 0|1; mirv_weather rain|ground|postprocess|contact 0|1; mirv_weather status|reload.\nStorm options: mirv_weather storm 0|1; mirv_weather lightning demo|kills|both|off|test (compatibility trigger for thunder scheduling; no lightning visuals); mirv_weather interval 5..300; mirv_weather sun 0..1; mirv_weather exposure -3..1; mirv_weather rainsound 0..1; mirv_weather thunder 0..1.\nSeven-map offline demos: de_dust2, de_mirage, de_cache, de_inferno, de_ancient, de_nuke, de_anubis. Install the matching resources in HLAE. Native environment: mirv_pov_debug_feature weather_environment 0|1. Storm effects: mirv_pov_debug_feature weather_sun|weather_exposure|weather_grade|weather_rainsound|weather_thunder 0|1. Sky remains controlled by mirv_sky material.\n");
    }
    MirvWeather_Frame(false);
    PrintStatus();
}
