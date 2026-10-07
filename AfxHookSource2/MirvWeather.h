#pragma once
#include <Windows.h>
class CMaterial2;
void MirvWeather_Initialize(HMODULE clientDll);
void MirvWeather_ResolveSchemaOffsets();
// fromRenderFrame is true only for the normal FRAME_RENDER_PASS call in
// main.cpp. Command-side calls pass false so a diagnostic request queued by the
// command itself can never be consumed by that same command.
void MirvWeather_Frame(bool fromRenderFrame = true);
void MirvWeather_Reset();
bool MirvWeather_HasGroundOverride();
CMaterial2 * MirvWeather_Material(CMaterial2 * original);

// Weather core owns the native particle and sound-definition bindings used by
// the storm layer. These entry points are engine-thread only: spawn keeps the
// definitions alive while the transient exists, clear destroys every owned
// transient synchronously and releases its binding, so reset/seek/pause can
// never leave an owned particle or resource reference behind.
bool MirvWeather_SpawnLightning(const float start[3], const float end[3]);
void MirvWeather_ClearLightning();
bool MirvWeather_RetainAudioData();
void MirvWeather_ReleaseAudioData();

// Short stage names for the native particle paths. The stage identifies the
// first guard that stopped an attempt (missing schema, manager, definition,
// create or record) so a status line can distinguish the failure without
// printing addresses.
enum MirvWeatherParticleStage {
    MirvWeatherParticleStage_None = 0,
    MirvWeatherParticleStage_Requested,
    MirvWeatherParticleStage_Profile,
    MirvWeatherParticleStage_Config,
    MirvWeatherParticleStage_Layout,
    MirvWeatherParticleStage_NativeInterface,
    MirvWeatherParticleStage_ResourceSystem,
    MirvWeatherParticleStage_Manager,
    MirvWeatherParticleStage_Time,
    MirvWeatherParticleStage_Definition,
    MirvWeatherParticleStage_Create,
    MirvWeatherParticleStage_Record,
    // Control-probe scheduling states. They are appended so the existing stage
    // values keep their meaning.
    MirvWeatherParticleStage_Queued,
    MirvWeatherParticleStage_Paused,
    MirvWeatherParticleStage_Dropped
};
const char * MirvWeather_ParticleStageName(int stage);

struct MirvWeatherParticleResourceStatus {
    unsigned tries, definitionFails, createFails, created, live;
    int stage;
};
struct MirvWeatherParticleStatus {
    // The authored bolt is the visible weather lightning. The native cloud is
    // an optional secondary effect (a dark flash cloud, not a rope bolt) that
    // is reported separately so a failed authored definition cannot be hidden
    // by a successful native cloud spawn.
    MirvWeatherParticleResourceStatus lightningBolt;
    MirvWeatherParticleResourceStatus lightningCloud;
    unsigned lightningTries;
    int lightningStage;
    MirvWeatherParticleResourceStatus rain;
};
// Render/game-thread safe snapshot of the owned particle bindings. These are
// submission counters only: they do not prove that an effect is visible.
MirvWeatherParticleStatus MirvWeather_ParticleStatus();

// Manual visible-rope control probe (`mirv_weather lightning probe`). It is a
// deliberately isolated diagnostic: it spawns only the installed native
// particles/impact_fx/impact_wallbang_light_silent.vpcf control effect and is
// counted separately from the authored bolt, the optional cloud and the
// demo/kill/manual lightning schedule. It never creates a flash, kill or
// thunder event and never changes the production placement or feature gates.
//
// Queueing is transient and manual. The queued sequence runs three pulses
// spaced 0.75 demo seconds apart, starting 1.5 demo seconds after queueing, so
// closing the console cannot hide the effect. A pause, seek, map change, storm
// off or weather master off drops both the pending sequence and any owned probe
// particle.
struct MirvWeatherProbeStatus {
    unsigned queued, dropped, fired, definitionFails, createFails, created, live;
    int stage;
    int remaining;
    bool pending;
};
MirvWeatherProbeStatus MirvWeather_ProbeStatus();
void MirvWeather_QueueLightningProbe();
void MirvWeather_ClearLightningProbe();
void MirvWeather_LightningProbeFrame(bool fromRenderFrame, bool paused, double time);

// Number of retained .vsndevts definition bindings and whether the last attempt
// failed. Retained definitions are separate from resolved native event ids.
int MirvWeather_AudioDefinitionCount();
int MirvWeather_AudioDefinitionTargets();
bool MirvWeather_AudioPrecacheFailed();
