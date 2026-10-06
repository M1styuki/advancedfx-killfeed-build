#pragma once
#include <Windows.h>
class CMaterial2;
void MirvWeather_Initialize(HMODULE clientDll);
void MirvWeather_ResolveSchemaOffsets();
void MirvWeather_Frame();
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
