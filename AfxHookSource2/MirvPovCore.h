#pragma once

#include <Windows.h>
#include <cstdint>

class CEntityInstance;

namespace SOURCESDK {
namespace CS2 {
class IGameEvent;
}
}

bool MirvPov_IsEnabled();

#if AFX_MIRV_POV_DIAGNOSTICS
bool MirvPovDebug_CanConfigure();
bool MirvPovDebug_HasFeature(const char * name);
bool MirvPovDebug_IsFeatureEnabled(const char * name);
bool MirvPovDebug_SetFeatureEnabled(const char * name, bool enabled);
void MirvPovDebug_ApplyFeatureConfiguration();
void MirvPovDebug_PrintFeatureStates();
#define MIRV_POV_FEATURE_ACTIVE(name) (MirvPov_IsEnabled() && MirvPovDebug_IsFeatureEnabled(name))
#else
#define MIRV_POV_FEATURE_ACTIVE(name) MirvPov_IsEnabled()
#endif
bool MirvPov_IsDeathFeedbackEnabled();
void MirvPov_SetDeathFeedbackEnabled(bool enabled);
void MirvPov_Enable(HMODULE clientDll);
void MirvPov_Disable();

CEntityInstance * GetCurrentPovPlayerController();
CEntityInstance * GetCurrentPovPlayerPawn();
CEntityInstance * GetObservedPlayerController();
CEntityInstance * GetObservedPlayerPawn();
bool MirvPov_GetObserverState(uint8_t & observerMode, uint32_t & observerTarget);
CEntityInstance * GetRealLocalPlayerPawn();
CEntityInstance * GetEffectiveSplitScreenPlayer(int slot);

CEntityInstance * GetFakePovRadarController();
int GetFakePovRadarControllerIndex();
void SetFakePovRadarControllerIndex(int index);
void SetFakePovRadarAutoSync(bool enabled);
bool GetFakePovRadarAutoSync();

void * MirvPov_PushHookReturnAddress(void * returnAddress);
void * MirvPov_GetHookReturnAddress();
void MirvPov_PopHookReturnAddress(void * previous);

void MirvPov_UpdateSeekDetection();
void MirvPov_OnFrameStageBefore(int frameStage);
void MirvPov_OnFrameStageAfter(int frameStage);
void MirvPov_OnGameEvent(SOURCESDK::CS2::IGameEvent * event);
void MirvPov_OnPanoramaDllLoaded(HMODULE panoramaDll);
void MirvPov_OnPanoramaLayoutFileLoaded(const char * filePath);
void MirvPov_OnLevelInitPreEntity();
