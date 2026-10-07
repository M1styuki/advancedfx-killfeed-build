#define NOMINMAX
#include "stdafx.h"
#include "MirvWeatherStorm.h"
#include "MirvWeather.h"
#include "MirvPovCore.h"
#include "MirvPovSoundCircle.h"
#include "MirvTime.h"
#include "SceneSystem.h"
#include "SchemaSystem.h"
#include "ClientEntitySystem.h"
#include "DeathMsg.h"
#include "RenderSystemDX11Hooks.h"
#include "WrpConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
extern bool getOffset(ptrdiff_t *, std::string, std::string, std::string);
extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;
namespace {
bool enabled = true, wasActive = false, audioPrecached = false;
bool soundWarning = false, manualWarning = false, sunWarning = false;
int trigger = 3; // demo=1, kept killfeed=2, both=3
float period = 30, sunScale = .35f, exposure = -.35f, rainVolume = .45f, thunderVolume = .55f;
double previousTime = -1, flashStart = -100, thunderAt = -1, lastRainVolume = -1;
long long ambientCell = -1;
uint64_t demoFlashes = 0, killFlashes = 0, manualFlashes = 0;
// Audio initialization is only latched once the owned/direct native interface
// is actually ready; a failed init is retried at a bounded, backing-off cadence
// instead of disabling storm audio until the process restarts.
bool soundReady = false;
unsigned soundInitAttempts = 0;
ULONGLONG soundInitRetryAt = 0;
unsigned rainAudioTries = 0, rainAudioOk = 0, thunderAudioTries = 0, thunderAudioOk = 0;
int rainAudioStage = MirvOwnedSoundStage_None, thunderAudioStage = MirvOwnedSoundStage_None;
// A manual diagnostic request is consumed on a later, active, unpaused weather
// frame; it is independent of the schedule and of the kill filter list.
bool manualPending = false;
unsigned long long manualQueuedFrame = 0;
unsigned long long frameCount = 0;
std::atomic<bool> active {false}, hideSun {false};
// Demo pause state of the last weather frame; published for the status command
// so a muted rain/thunder instance can be read as "expected while paused".
std::atomic<bool> demoPaused {false};
std::mutex stateMutex;
MirvStormGrade grade {0,1,0,0,0,0,false};
struct Kill { int tick, attacker, victim; };
std::deque<Kill> pending, recent;
MirvOwnedSound rainSound, thunderSound;
HMODULE clientModule = nullptr;
void ** lightVtable = nullptr, ** postVtable = nullptr, ** lightManagerSlot = nullptr;
using UpdateLightFn = void (__fastcall *)(void *, void *);
UpdateLightFn updateLight = nullptr;
ptrdiff_t lightComponentOffset = -1, brightnessOffset = -1, exposureOffset = -1, exposureControlOffset = -1;
ptrdiff_t flashOverlayOffset = -1, flashScreenshotOffset = -1;
ptrdiff_t flashEndOffset = -1, flashDurationOffset = -1;
bool sunSchemaReady = false, exposureSchemaReady = false;
struct FloatBackup { SOURCESDK::CS2::CBaseHandle handle; void * object; float original, applied; };
std::vector<FloatBackup> lights, exposures;
ULONGLONG lastScan = 0;
std::vector<SOURCESDK::CS2::CBaseHandle> lightHandles, postHandles;
bool Feature(const char * name) { return MirvPovDebug_IsFeatureEnabled(name); }
void PublishGrade(const MirvStormGrade & value) { std::lock_guard<std::mutex> lock(stateMutex); grade = value; }
bool InFlash() {
    auto * pawn = GetCurrentPovPlayerPawn();
    if(!pawn) pawn = GetObservedPlayerPawn();
    if(!pawn || !pawn->IsPlayerPawn() || flashOverlayOffset <= 0 || flashScreenshotOffset <= 0
        || flashEndOffset<=0 || flashDurationOffset<=0) return false;
    auto * bytes = reinterpret_cast<unsigned char *>(pawn);
    const float a = *reinterpret_cast<float *>(bytes + flashOverlayOffset);
    const float b = *reinterpret_cast<float *>(bytes + flashScreenshotOffset);
    const float end=*reinterpret_cast<float *>(bytes+flashEndOffset);
    const float duration=*reinterpret_cast<float *>(bytes+flashDurationOffset);
    const float now=g_MirvTime.curtime_get();
    return std::isfinite(end) && std::isfinite(duration) && end>now && duration>0 && end-now<=duration+1
        && ((std::isfinite(a) && a>.01f) || (std::isfinite(b) && b>.01f));
}
void RefreshLight(void * component) {
    if(updateLight && lightManagerSlot && *lightManagerSlot) updateLight(*lightManagerSlot, component);
}
void Restore(std::vector<FloatBackup> & backups, bool light) {
    if(g_pEntityList && *g_pEntityList && g_GetEntityFromIndex) {
        for(const auto & b : backups) {
            auto * entity = GetEntityFromIndex(b.handle.GetEntryIndex());
            if(!entity || entity->GetHandle() != b.handle) continue;
            void * object = light ? *reinterpret_cast<void **>(reinterpret_cast<unsigned char *>(entity) + lightComponentOffset) : entity;
            if(object != b.object) continue;
            auto * value = reinterpret_cast<float *>(reinterpret_cast<unsigned char *>(object) + (light ? brightnessOffset : exposureOffset));
            if(*value == b.applied) { *value = b.original; if(light) RefreshLight(object); }
        }
    }
    backups.clear();
}
void Scan() {
    if(!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return;
    const ULONGLONG now = GetTickCount64();
    if(lastScan && now-lastScan < 1000) return;
    lastScan = now; lightHandles.clear(); postHandles.clear();
    for(int i=0;i<0x7fff;++i) {
        auto * e=GetEntityFromIndex(i); if(!e) continue;
        auto ** vt=*reinterpret_cast<void ***>(e);
        if(vt==lightVtable || vt==postVtable) {
            auto h=e->GetHandle(); if(!h.IsValid()) continue;
            (vt==lightVtable?lightHandles:postHandles).push_back(h);
        }
    }
}
void ApplyFloat(const SOURCESDK::CS2::CBaseHandle & handle, void * object, bool light, float amount) {
    auto & backups=light?lights:exposures;
    auto * value=reinterpret_cast<float *>(reinterpret_cast<unsigned char *>(object)+(light?brightnessOffset:exposureOffset));
    if(!std::isfinite(*value) || std::fabs(*value)>1024) return;
    auto it=backups.begin(); while(it!=backups.end() && (it->handle!=handle || it->object!=object)) ++it;
    if(it==backups.end()) { backups.push_back({handle,object,*value,*value}); it=backups.end()-1; }
    else if(*value!=it->applied) it->original=*value;
    const float desired=light?it->original*amount:it->original+amount;
    if(*value!=desired) { *value=desired; if(light) RefreshLight(object); }
    it->applied=desired;
}
// The trigger source is kept explicit so the manual diagnostic test can never
// be counted as a demo timer event or as a retained kill.
enum FireKind { FireKind_Demo = 0, FireKind_Kill = 1, FireKind_Manual = 2 };
void Fire(double time, FireKind kind, unsigned count = 1) {
    flashStart=time; thunderAt=time+(kind==FireKind_Kill?.22:1.0);
    if(kind==FireKind_Kill) killFlashes+=count; else if(kind==FireKind_Manual) manualFlashes+=count; else demoFlashes+=count;
    // The authored bolt is a CP0->CP1 rope strike, not a snapshot generator.
    // A level first-person sky view must intersect part of it, so the endpoint
    // sits only ~320 units above the camera at 1500..1800 units forward and the
    // start ~1050 units above that. The former 1000/2700 placement put almost
    // the whole bolt outside a horizontal view. Native depth testing keeps the
    // strike behind buildings.
    const double yaw=g_CurrentGameCamera.angles[1]*3.141592653589793/180;
    const float dirX=static_cast<float>(std::cos(yaw)), dirY=static_cast<float>(std::sin(yaw));
    const float originX=static_cast<float>(g_CurrentGameCamera.origin[0]);
    const float originY=static_cast<float>(g_CurrentGameCamera.origin[1]);
    const float originZ=static_cast<float>(g_CurrentGameCamera.origin[2]);
    float end[3]={originX+dirX*1600.0f,originY+dirY*1600.0f,originZ+320.0f};
    // Start slightly behind and to the side so the strike has a natural slant.
    float start[3]={end[0]-dirX*180.0f+dirY*90.0f,end[1]-dirY*180.0f-dirX*90.0f,end[2]+1050.0f};
    const bool spawned=Feature("weather_lightning") && MirvWeather_SpawnLightning(start,end);
    if(kind==FireKind_Manual)
        advancedfx::Message("[mirv_weather] manual lightning test fired: particle=%d.\n",spawned?1:0);
}
} // namespace

void MirvWeatherStorm_Initialize(HMODULE client) {
    clientModule=client;
    lightVtable=reinterpret_cast<void **>(Afx::BinUtils::FindClassVtable(client,".?AVC_LightEnvironmentEntity@@",0,0));
    postVtable=reinterpret_cast<void **>(Afx::BinUtils::FindClassVtable(client,".?AVC_PostProcessingVolume@@",0,0));
    if(lightVtable) {
        auto * fn=reinterpret_cast<unsigned char *>(lightVtable[13]);
        const unsigned char prefix[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x99,0x98,0x10,0x00,0x00};
        if(!memcmp(fn,prefix,sizeof(prefix)) && fn[0x26]==0x48 && fn[0x27]==0x8b && fn[0x28]==0x0d && fn[0x30]==0xe8) {
            lightManagerSlot=reinterpret_cast<void **>(fn+0x2d+*reinterpret_cast<int32_t *>(fn+0x29));
            updateLight=reinterpret_cast<UpdateLightFn>(fn+0x35+*reinterpret_cast<int32_t *>(fn+0x31));
        }
    }
}
void MirvWeatherStorm_ResolveSchema() {
    getOffset(&lightComponentOffset,"client.dll","C_LightEntity","m_CLightComponent");
    getOffset(&brightnessOffset,"client.dll","CLightComponent","m_flBrightnessScale");
    getOffset(&exposureOffset,"client.dll","C_PostProcessingVolume","m_flExposureCompensation");
    getOffset(&exposureControlOffset,"client.dll","C_PostProcessingVolume","m_bExposureControl");
    getOffset(&flashOverlayOffset,"client.dll","C_CSPlayerPawnBase","m_flFlashOverlayAlpha");
    getOffset(&flashScreenshotOffset,"client.dll","C_CSPlayerPawnBase","m_flFlashScreenshotAlpha");
    getOffset(&flashEndOffset,"client.dll","C_CSPlayerPawnBase","m_flFlashBangTime");
    getOffset(&flashDurationOffset,"client.dll","C_CSPlayerPawnBase","m_flFlashDuration");
    sunSchemaReady=lightComponentOffset==0x1098 && brightnessOffset==0x84;
    exposureSchemaReady=exposureOffset==0x11ac && exposureControlOffset==0x11bd;
}
void MirvWeatherStorm_Reset() {
    active.store(false); hideSun.store(false); PublishGrade({0,1,0,0,0,0,false});
    demoPaused.store(false);
    Restore(lights,true); Restore(exposures,false);
    // Owned transient particles cannot outlive a storm reset: destroy them
    // synchronously and release their retained definition bindings here rather
    // than waiting for demo-time expiry (which is frozen while paused).
    MirvWeather_ClearLightning();
    MirvPovSoundCircle_StopOwnedSound(rainSound); MirvPovSoundCircle_StopOwnedSound(thunderSound);
    MirvWeather_ReleaseAudioData();audioPrecached=false;
    {std::lock_guard<std::mutex> lock(stateMutex); pending.clear(); recent.clear();}
    lightHandles.clear(); postHandles.clear(); lastScan=0;
    previousTime=-1; flashStart=-100; thunderAt=-1; ambientCell=-1; wasActive=false;
    lastRainVolume=-1;
    // A queued manual diagnostic test must not survive a seek, map change or
    // storm reset; it is a one-shot request for the next normal frame.
    manualPending=false;
}
void MirvWeatherStorm_VisibleKill(int attacker,int victim) {
    if(!active.load() || !(trigger&2) || attacker<0 || victim<0 || attacker==victim || !g_pEngineToClient) return;
    auto * demo=g_pEngineToClient->GetDemoFile(); if(!demo) return;
    Kill k {demo->GetDemoTick(),attacker,victim};
    std::lock_guard<std::mutex> lock(stateMutex);
    for(const auto & e:recent) if(e.tick==k.tick && e.attacker==attacker && e.victim==victim) return;
    // The pending queue is a hard per-frame bound. An event dropped because the
    // bound is reached is intentionally NOT marked as handled: it must not be
    // silently deduplicated before it has actually been enqueued.
    if(pending.size()>=16) return;
    recent.push_back(k); if(recent.size()>64) recent.pop_front();
    pending.push_back(k);
}
void MirvWeatherStorm_Frame(bool masterActive, bool fromRenderFrame) {
    if(!masterActive || !enabled) {
        if(wasActive) MirvWeatherStorm_Reset();
        // A queued manual diagnostic test must not survive storm off, even when
        // the storm was never active and Reset() therefore did not run.
        manualPending=false;
        return;
    }
    double time=0; if(!g_MirvTime.GetCurrentDemoTime(time) || !std::isfinite(time)) return;
    if(previousTime>=0 && (time<previousTime || time-previousTime>2)) MirvWeatherStorm_Reset();
    wasActive=true; active.store(true); Scan();
    // The frame stamp and the manual-test consumption belong to the normal
    // render frame only. A command-side frame must not advance the stamp, or the
    // manual test queued by that very command would already look "later".
    if(fromRenderFrame) ++frameCount;
    const bool paused=g_pEngineToClient->GetDemoFile()->IsDemoPaused();
    demoPaused.store(paused);
    // A POV death fade owns the whole transition, not only exact black. Query
    // the render-thread state so grading and native exposure stay out of it.
    const bool faded=RenderSystemDX11_DeathFade_IsActive();
    if(paused) {
        // Demo time is frozen while paused, so time-based transient expiry can
        // never run. Destroy owned lightning synchronously and never resume a
        // stale flash or a delayed thunder after unpause.
        MirvWeather_ClearLightning();
        MirvPovSoundCircle_StopOwnedSound(thunderSound);
        thunderAt=-1; flashStart=-100;
        { std::lock_guard<std::mutex> lock(stateMutex); pending.clear(); }
    }
    const long long cell=static_cast<long long>(std::floor(time/period));
    const bool feedback=Feature("weather_lightning") || Feature("weather_lightning_light") || Feature("weather_thunder");
    if(!paused && ambientCell>=0 && cell!=ambientCell && (trigger&1) && feedback) Fire(time,FireKind_Demo);
    ambientCell=cell; previousTime=time;
    std::deque<Kill> kills;
    { std::lock_guard<std::mutex> lock(stateMutex); kills.swap(pending); }
    if(!paused && (trigger&2) && feedback) {
        const int tick=g_pEngineToClient->GetDemoFile()->GetDemoTick();
        unsigned burst=0;
        for(const auto & k:kills) if(k.tick==tick || std::abs(k.tick-tick)<=128) ++burst;
        // One kept kill, or a bounded multi-kill burst inside the accepted tick
        // window, becomes a single flash with a single delayed thunder; every
        // accepted kill still increments the counter.
        if(burst) Fire(time,FireKind_Kill,burst);
    }
    // The manual diagnostic test is intentionally independent of the demo timer
    // and of the kill filter list, but it still needs a supported demo, the
    // storm layer enabled and an unpaused frame; it never fabricates a kill
    // event. It fires only on a later normal render frame than the one that
    // queued it: command-side frames neither advance frameCount nor consume it.
    if(fromRenderFrame && manualPending && !paused && frameCount>manualQueuedFrame) {
        manualPending=false;
        if(!feedback) {
            if(!manualWarning) {manualWarning=true;advancedfx::Warning("[mirv_weather] Manual lightning test ignored: all lightning feedback features are off.\n");}
        } else Fire(time,FireKind_Manual);
    }
    const double elapsed=time-flashStart;
    float flash=0;
    if(!paused && !faded && Feature("weather_lightning_light") && elapsed>=0 && elapsed<.45)
        flash=static_cast<float>((elapsed<.08?1:(elapsed<.14?.15:(elapsed<.22?.7:0)))*std::exp(-elapsed*4));
    const bool flashed=InFlash() || faded;
    if(sunSchemaReady && updateLight && lightManagerSlot && *lightManagerSlot
        && (Feature("weather_sun") || flash>0)) {
        for(const auto & h:lightHandles) {
            auto * e=GetEntityFromIndex(h.GetEntryIndex()); if(!e || e->GetHandle()!=h) continue;
            auto * component=*reinterpret_cast<void **>(reinterpret_cast<unsigned char *>(e)+lightComponentOffset);
            if(component) ApplyFloat(h,component,true,(Feature("weather_sun")?sunScale:1)+flash*2.0f);
        }
    } else { Restore(lights,true); if(Feature("weather_sun") && (!sunSchemaReady || !updateLight || !lightManagerSlot) && !sunWarning) {sunWarning=true;advancedfx::Warning("[mirv_weather] Sun interface/schema unavailable; sun unchanged.\n");} }
    hideSun.store(Feature("weather_sun"));
    if(exposureSchemaReady && Feature("weather_exposure") && !flashed) {
        for(const auto & h:postHandles) {
            auto * e=GetEntityFromIndex(h.GetEntryIndex()); if(!e || e->GetHandle()!=h) continue;
            if(*reinterpret_cast<bool *>(reinterpret_cast<unsigned char *>(e)+exposureControlOffset)) ApplyFloat(h,e,false,exposure*.5f);
        }
    } else Restore(exposures,false);
    const bool grading=Feature("weather_grade");
    const bool adjustExposure=Feature("weather_exposure");
    PublishGrade({adjustExposure?(exposures.empty()?exposure:exposure*.5f):0,
        grading?.9f:1,grading?.13f:0,grading?.025f:0,grading?.025f:0,flash*.2f,
        !flashed&&(grading||adjustExposure||flash>0)});
    // Weather resolves and owns its own direct sound-event interface. It does
    // NOT install or require the POV radar/local-pawn/network sound hooks, so a
    // failed POV sound-circle initialization cannot erase storm audio state.
    const bool wantAudio=Feature("weather_rainsound") || Feature("weather_thunder");
    if(!soundReady && wantAudio) {
        // A failed resolve is not latched for the process: retry at a bounded,
        // backing-off cadence until the owned start helper, event interface,
        // stop helper and owned system slot are all verified present.
        const ULONGLONG now=GetTickCount64();
        if(0==soundInitRetryAt || now>=soundInitRetryAt) {
            ++soundInitAttempts;
            const unsigned attempt=soundInitAttempts;
            const ULONGLONG delay=attempt<=1?2000:(attempt==2?4000:(attempt==3?8000:15000));
            soundInitRetryAt=now+delay;
            MirvPovSoundCircle_InitializeOwnedAudio(clientModule);
            if(MirvPovSoundCircle_IsOwnedSoundReady()) {
                soundReady=true;
                if(attempt>1) advancedfx::Message("[mirv_weather] Native storm audio interface ready after %u attempts.\n",attempt);
            } else if(1==attempt) {
                advancedfx::Warning("[mirv_weather] Native storm audio interface not ready; retrying at a bounded cadence.\n");
            }
        }
    }
    // Retain the sound definitions only while at least one storm sound feature
    // needs them; a failed lookup is reported and not retried every frame.
    if(wantAudio && !audioPrecached) audioPrecached=MirvWeather_RetainAudioData();
    if(audioPrecached && !paused && Feature("weather_rainsound") && rainVolume>0) {
        if(!rainSound.system || lastRainVolume!=rainVolume) {
            ++rainAudioTries;
            if(MirvPovSoundCircle_StartOwnedSound("train.Outside_TSpawn.Rain",rainVolume,rainSound)) {lastRainVolume=rainVolume;++rainAudioOk;}
            else {
                rainAudioStage=MirvPovSoundCircle_OwnedSoundLastFailure();
                if(!soundWarning) {soundWarning=true;advancedfx::Warning("[mirv_weather] Native rain audio unavailable; no unowned audio fallback.\n");}
            }
        }
    } else {MirvPovSoundCircle_StopOwnedSound(rainSound); lastRainVolume=-1;}
    if(!Feature("weather_thunder") || thunderVolume<=0) {MirvPovSoundCircle_StopOwnedSound(thunderSound);thunderAt=-1;}
    if(!paused && thunderAt>=0 && time>=thunderAt) {
        if(audioPrecached && Feature("weather_thunder") && thunderVolume>0) {
            ++thunderAudioTries;
            if(MirvPovSoundCircle_StartOwnedSound("Weather.thunder_close_1",thunderVolume,thunderSound)) ++thunderAudioOk;
            else thunderAudioStage=MirvPovSoundCircle_OwnedSoundLastFailure();
        }
        thunderAt=-1;
    }
    // Release definitions only after both owned playback instances have stopped.
    if(!wantAudio && audioPrecached) {
        MirvWeather_ReleaseAudioData();
        audioPrecached=false;
    }
}
MirvStormGrade MirvWeatherStorm_Grade() {std::lock_guard<std::mutex> lock(stateMutex);return grade;}
bool MirvWeatherStorm_HideSun(const char * name) {
    return hideSun.load() && name && (strstr(name,"/sun_disc_glow_") || strstr(name,"/sun_glow_"));
}
bool MirvWeatherStorm_SunActive() { return hideSun.load(); }
bool MirvWeatherStorm_Command(advancedfx::ICommandArgs * args) {
    if(args->ArgC()!=3) return false;
    const char * key=args->ArgV(1), * value=args->ArgV(2);
    if(!_stricmp(key,"storm") && (!strcmp(value,"0") || !strcmp(value,"1"))) {
        enabled=value[0]=='1';
        // Documented queue cleanup: turning the storm off discards a queued
        // manual diagnostic test immediately, even if it was never active.
        if(!enabled) manualPending=false;
        return true;
    }
    if(!_stricmp(key,"lightning")) {
        if(!_stricmp(value,"demo")) trigger=1;else if(!_stricmp(value,"kills")) trigger=2;
        else if(!_stricmp(value,"both")) trigger=3;else if(!_stricmp(value,"off")) trigger=0;
        else if(!_stricmp(value,"test")) {manualPending=true;manualQueuedFrame=frameCount;}
        else return false;
        return true;
    }
    char * end=nullptr; float number=strtof(value,&end); if(end==value || *end || !std::isfinite(number)) return false;
    if(!_stricmp(key,"sun") && number>=0 && number<=1) sunScale=number;
    else if(!_stricmp(key,"exposure") && number>=-3 && number<=1) exposure=number;
    else if(!_stricmp(key,"rainsound") && number>=0 && number<=1) rainVolume=number;
    else if(!_stricmp(key,"thunder") && number>=0 && number<=1) thunderVolume=number;
    else if(!_stricmp(key,"interval") && number>=5 && number<=300) {period=number;ambientCell=-1;}
    else return false;
    return true;
}
static const char * EventStateName(int stage) {
    return MirvOwnedSoundStage_None==stage ? "ok" : MirvPovSoundCircle_OwnedSoundStageName(stage);
}
void MirvWeatherStorm_Status() {
    advancedfx::Message("[mirv_weather] storm=%d active=%d paused=%d sky=preserved; sun=%zu scale=%.2f exposure=%zu EV=%.2f grade=%d; lightning mode=%d demo=%llu kills=%llu manual=%llu; rainAudio=%d thunderAudio=%d.\n",
        enabled,active.load(),demoPaused.load(),lights.size(),sunScale,exposures.size(),exposure,MirvWeatherStorm_Grade().active,
        trigger,static_cast<unsigned long long>(demoFlashes),static_cast<unsigned long long>(killFlashes),
        static_cast<unsigned long long>(manualFlashes),rainSound.system!=nullptr,thunderSound.system!=nullptr);
    // Definitions retained (precache bindings) and registered event ids
    // (resolution + validation in the native sound-event interface) are
    // deliberately reported separately: one can exist without the other. The
    // registered-event stages stay independent of the GUID/system-active state.
    const MirvOwnedSoundReadiness ready=MirvPovSoundCircle_OwnedSoundReadiness();
    const int rainEvent=MirvPovSoundCircle_ProbeOwnedSoundEvent("train.Outside_TSpawn.Rain");
    const int thunderEvent=MirvPovSoundCircle_ProbeOwnedSoundEvent("Weather.thunder_close_1");
    advancedfx::Message("[mirv_weather] storm audio: init=%d tries=%u owned=%d start=%d stop=%d system=%d iface=%d last=%s attempts=%u started=%u; defs=%d/%d failed=%d; rain tries=%u ok=%u stage=%s; thunder tries=%u ok=%u stage=%s; events rain=%s thunder=%s (rainAudio/thunderAudio=0 is expected while paused).\n",
        soundReady,soundInitAttempts,ready.hooked,ready.startHelper,ready.stopHelper,ready.ownedSystem,ready.eventInterface,
        MirvPovSoundCircle_OwnedSoundStageName(ready.lastFailureStage),ready.attempts,ready.started,
        MirvWeather_AudioDefinitionCount(),MirvWeather_AudioDefinitionTargets(),MirvWeather_AudioPrecacheFailed()?1:0,
        rainAudioTries,rainAudioOk,MirvPovSoundCircle_OwnedSoundStageName(rainAudioStage),
        thunderAudioTries,thunderAudioOk,MirvPovSoundCircle_OwnedSoundStageName(thunderAudioStage),
        EventStateName(rainEvent),EventStateName(thunderEvent));
    MirvWeatherStorm_RenderStatus();
}
