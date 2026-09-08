#pragma once

#include <Windows.h>

void MirvPovScoreboard_Initialize(HMODULE clientDll);
void MirvPovScoreboard_Reset(bool resetSettings = true);
void MirvPovScoreboard_Update();
bool MirvPovScoreboard_IsEnabled();
void MirvPovScoreboard_SetEnabled(bool enabled);
