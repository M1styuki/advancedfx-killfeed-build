#pragma once
#include <Windows.h>
class CMaterial2;
void MirvWeather_Initialize(HMODULE clientDll);
void MirvWeather_ResolveSchemaOffsets();
void MirvWeather_Frame();
void MirvWeather_Reset();
bool MirvWeather_HasGroundOverride();
CMaterial2 * MirvWeather_Material(CMaterial2 * original);
