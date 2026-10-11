#pragma once

#include <Windows.h>
#include <cstdint>

class CEntityInstance;
namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

// Resolve before the shared local-pawn getter is detoured.
void MirvPovMvpMusic_ResolveAddresses(HMODULE clientDll, const void * localPawnGetter);
uint32_t MirvPovMvpMusic_PushEvent(SOURCESDK::CS2::IGameEvent * event);
void MirvPovMvpMusic_PopEvent(uint32_t previous);
CEntityInstance * MirvPovMvpMusic_GetLocalPawn(void * caller);

// Stack-owned, value-only diagnostics. Never retains the native event object.
struct MirvPovMvpMusicTrace {
    MirvPovMvpMusicTrace * previous = nullptr;
    bool mvp = false;
    int tick = -1;
    int kit = -1;
    int noMusic = -1;
    uint32_t pawn = 0xFFFFFFFFu;
    const char * gate = "not-entered";
    unsigned getterCalls = 0;
    uintptr_t firstCallerRva = 0;
    unsigned playerQueries = 0;
    unsigned muteQueries = 0;
    unsigned playerOverrides = 0;
    unsigned muteOverrides = 0;
    int inventory = -1;
    int controller = -1;
    int demoSuppress = -1;
    unsigned eligibilityCalls = 0;
    int eligible = -1;
    unsigned musicCalls = 0;
    int nativeKit = -1;
    unsigned soundCalls = 0;
    char soundName[128] = {};
};
void MirvPovMvpMusic_BeginTrace(SOURCESDK::CS2::IGameEvent * event, MirvPovMvpMusicTrace & trace);
void MirvPovMvpMusic_EndTrace(MirvPovMvpMusicTrace & trace);
void MirvPovMvpMusic_PrintStatus();
