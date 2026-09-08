#pragma once

#include <Windows.h>

void MirvPov_HookVoiceHud(HMODULE clientDll);
bool MirvPovVoice_IsEnabled();
void MirvPovVoice_SetEnabled(bool enabled);
void MirvPov_ResetVoiceHud();
void MirvPovVoice_ResetDemoState();
void MirvPov_UpdateVoiceTeam();
void MirvPov_UpdateVoiceHud();
void MirvPov_ClearSyntheticSpeaking();
void MirvPovVoice_OnRenderPass();
void MirvPovVoice_AfterRenderPass();
