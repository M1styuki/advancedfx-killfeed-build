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
    MirvWeatherParticleStage_Record
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

// Number of retained .vsndevts definition bindings and whether the last attempt
// failed. Retained definitions are separate from resolved native event ids.
int MirvWeather_AudioDefinitionCount();
int MirvWeather_AudioDefinitionTargets();
bool MirvWeather_AudioPrecacheFailed();
