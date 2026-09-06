#pragma once

#include <Windows.h>

void MirvPovTeamID_ApplyPatches(HMODULE clientDll);
void MirvPovTeamID_RemovePatches();

bool MirvPovTeamID_GetDebug();
void MirvPovTeamID_SetDebug(bool value);
