#pragma once
#include <Windows.h>
#include <cstdint>
struct ID3D11DeviceContext;
namespace advancedfx { class ICommandArgs; }
struct MirvStormGrade {
    float exposure, saturation, highlight, cool, shadows, flash;
    bool active;
};
void MirvWeatherStorm_Initialize(HMODULE client);
void MirvWeatherStorm_ResolveSchema();
void MirvWeatherStorm_Frame(bool active);
void MirvWeatherStorm_Reset();
void MirvWeatherStorm_VisibleKill(int attacker, int victim);
bool MirvWeatherStorm_Command(advancedfx::ICommandArgs * args);
void MirvWeatherStorm_Status();
bool MirvWeatherStorm_HideSun(const char * material);
bool MirvWeatherStorm_SunActive();
MirvStormGrade MirvWeatherStorm_Grade();
void MirvWeatherStorm_Render(ID3D11DeviceContext * context);
void MirvWeatherStorm_RenderStatus();
