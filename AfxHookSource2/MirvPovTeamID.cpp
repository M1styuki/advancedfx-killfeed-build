#include "stdafx.h"

#include "MirvPovTeamID.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "MirvPovHookUtils.h"

#include "../shared/AfxConsole.h"
#include "../shared/AfxDetours.h"
#include "../shared/binutils.h"
#include "../deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <Windows.h>
#include <stdint.h>

namespace {

using GetTeamIdContextPlayerFn = CEntityInstance * (__fastcall *)();
using GetTeamIdRelationshipFn = bool (__fastcall *)(CEntityInstance *, CEntityInstance *);

constexpr size_t kTeamIdRelationshipCallCount = 4;
uint8_t * g_TeamIdRelationshipCallSites[kTeamIdRelationshipCallCount] = {};
uint8_t g_TeamIdRelationshipOriginalCalls[kTeamIdRelationshipCallCount][5] = {};
uint8_t * g_TeamIdRelationshipThunk = nullptr;
GetTeamIdRelationshipFn g_GetNativeTeamIdRelationship = nullptr;
GetTeamIdContextPlayerFn g_GetNativeTeamIdDisplayPlayer = nullptr;
bool g_TeamIdPatched = false;
bool g_TeamIdDebug = false;
ULONGLONG g_TeamIdDebugLastLogTime = 0;

bool ShouldLogTeamIdDebug()
{
    if(!g_TeamIdDebug) return false;

    const ULONGLONG now = GetTickCount64();
    if(0 != g_TeamIdDebugLastLogTime && now - g_TeamIdDebugLastLogTime < 1000) return false;
    g_TeamIdDebugLastLogTime = now;
    return true;
}

void LogTeamIdEntity(const char * label, CEntityInstance * entity)
{
    if(nullptr == entity) {
        advancedfx::Message("[mirv_pov_teamid_debug] %s=null\n", label);
        return;
    }

    __try {
        const bool isPawn = entity->IsPlayerPawn();
        const bool isController = entity->IsPlayerController();
        const auto handle = entity->GetHandle();
        const auto controllerHandle = isPawn
            ? entity->GetPlayerControllerHandle()
            : SOURCESDK::CS2::CBaseHandle();
        const auto observerTarget = isPawn
            ? entity->GetObserverTarget()
            : SOURCESDK::CS2::CBaseHandle();
        const char * className = entity->GetClientClassName();

        advancedfx::Message(
            "[mirv_pov_teamid_debug] %s=%p handle=%08X class=%s team=%d pawn=%d controller=%d observerMode=%u observerTarget=%08X pawnController=%08X\n",
            label,
            entity,
            handle.ToInt(),
            className ? className : "<null>",
            entity->GetTeam(),
            isPawn ? 1 : 0,
            isController ? 1 : 0,
            isPawn ? static_cast<unsigned int>(entity->GetObserverMode()) : 0,
            observerTarget.ToInt(),
            controllerHandle.ToInt());
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        advancedfx::Warning(
            "[mirv_pov_teamid_debug] %s=%p raised an exception while reading entity state.\n",
            label,
            entity);
    }
}

void LogTeamIdCvar(const char * name)
{
    if(nullptr == SOURCESDK::CS2::g_pCVar) {
        advancedfx::Message("[mirv_pov_teamid_debug] cvar %s unavailable (ICvar=null).\n", name);
        return;
    }

    auto handle = SOURCESDK::CS2::g_pCVar->FindConVar(name, false);
    SOURCESDK::CS2::Cvar_s * cvar = handle.IsValid()
        ? SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get())
        : nullptr;
    if(nullptr == cvar) {
        advancedfx::Message("[mirv_pov_teamid_debug] cvar %s unavailable.\n", name);
        return;
    }

    advancedfx::Message(
        "[mirv_pov_teamid_debug] cvar %s int=%d float=%.3f bool=%d\n",
        name,
        cvar->m_Value.m_i32Value,
        cvar->m_Value.m_flValue,
        cvar->m_Value.m_bValue ? 1 : 0);
}

void LogTeamIdRelationship(
    const char * reason,
    bool xray,
    CEntityInstance * nativeContext,
    CEntityInstance * candidate,
    CEntityInstance * povController,
    bool nativeResult,
    bool result)
{
    if(!ShouldLogTeamIdDebug()) return;

    uint8_t observerMode = 0;
    uint32_t observerTarget = 0xFFFFFFFFu;
    const bool haveObserverState = MirvPov_GetObserverState(observerMode, observerTarget);

    advancedfx::Message(
        "[mirv_pov_teamid_debug] reason=%s xray=%d autoSync=%d configuredController=%d observerState=%d observerMode=%u observerTarget=%08X nativeEnemy=%d resultEnemy=%d\n",
        reason,
        xray ? 1 : 0,
        GetFakePovRadarAutoSync() ? 1 : 0,
        GetFakePovRadarControllerIndex(),
        haveObserverState ? 1 : 0,
        static_cast<unsigned int>(observerMode),
        observerTarget,
        nativeResult ? 1 : 0,
        result ? 1 : 0);
    CEntityInstance * displayPlayer = g_GetNativeTeamIdDisplayPlayer
        ? g_GetNativeTeamIdDisplayPlayer()
        : nullptr;
    LogTeamIdEntity("nativeDisplay", displayPlayer);
    LogTeamIdEntity("nativeContext", nativeContext);
    LogTeamIdEntity("candidate", candidate);
    LogTeamIdEntity("povController", povController);
    LogTeamIdEntity("realLocal", GetRealLocalPlayerPawn());
    LogTeamIdEntity("observed", GetObservedPlayerPawn());
    LogTeamIdCvar("cl_teamid_overhead_mode");
    LogTeamIdCvar("cl_teamid_overhead_maxdist_spec");
    LogTeamIdCvar("cl_drawhud_force_teamid_overhead");
    LogTeamIdCvar("sv_teamid_overhead");
    LogTeamIdCvar("sv_teamid_overhead_always_prohibit");
}

bool IsSpectatorXrayEnabled()
{
    if(nullptr == SOURCESDK::CS2::g_pCVar) return true;

    static SOURCESDK::CS2::ConVarHandle handle;
    if(!handle.IsValid()) handle = SOURCESDK::CS2::g_pCVar->FindConVar("spec_show_xray", false);
    if(!handle.IsValid()) return true;

    SOURCESDK::CS2::Cvar_s * cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
    return nullptr == cvar || 0 != cvar->m_Value.m_i32Value;
}

bool __fastcall GetTeamIdRelationship(CEntityInstance * nativeContext, CEntityInstance * candidate)
{
    const bool nativeResult = g_GetNativeTeamIdRelationship
        ? g_GetNativeTeamIdRelationship(nativeContext, candidate)
        : false;

    if(!MIRV_POV_FEATURE_ACTIVE("teamid")) return nativeResult;

    __try {
        const bool xray = IsSpectatorXrayEnabled();
        if(xray) {
            LogTeamIdRelationship(
                "xray-native", true, nativeContext, candidate, nullptr, nativeResult, nativeResult);
            return nativeResult;
        }

        CEntityInstance * povController = GetCurrentPovPlayerController();
        if(nullptr == povController || !povController->IsPlayerController()
            || nullptr == candidate || !candidate->IsPlayerPawn()) {
            LogTeamIdRelationship(
                "fallback-native", false, nativeContext, candidate, povController, nativeResult, nativeResult);
            return nativeResult;
        }

        const int povTeam = povController->GetTeam();
        const int candidateTeam = candidate->GetTeam();
        if((2 != povTeam && 3 != povTeam) || (2 != candidateTeam && 3 != candidateTeam)) {
            LogTeamIdRelationship(
                "invalid-team-native", false, nativeContext, candidate, povController, nativeResult, nativeResult);
            return nativeResult;
        }

        const bool result = povTeam != candidateTeam;
        LogTeamIdRelationship(
            "pov-team", false, nativeContext, candidate, povController, nativeResult, result);
        return result;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(ShouldLogTeamIdDebug()) {
            advancedfx::Warning(
                "[mirv_pov_teamid_debug] exception in TeamID relationship wrapper; returning native=%d.\n",
                nativeResult ? 1 : 0);
        }
        return nativeResult;
    }
}

} // namespace

bool MirvPovTeamID_GetDebug()
{
    return g_TeamIdDebug;
}

void MirvPovTeamID_SetDebug(bool value)
{
    g_TeamIdDebug = value;
    g_TeamIdDebugLastLogTime = 0;
    advancedfx::Message("mirv_pov_teamid_debug %d\n", value ? 1 : 0);
}

void MirvPovTeamID_ApplyPatches(HMODULE clientDll)
{
    if(g_TeamIdPatched) return;
    if(nullptr == clientDll) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_teamid] client.dll is not loaded.\n");
        return;
    }

    Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(!sections.Eof()) textRange = sections.GetMemRange();

    // TeamID obtains its native context player immediately after its display-state player.
    auto sequence = Afx::BinUtils::FindPatternString(
        textRange,
        "E8 ?? ?? ?? ?? 4C 8B F0 48 89 45 ?? E8 ?? ?? ?? ?? 48 8B F0 48 89 45 ?? E8");
    if(sequence.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_teamid] TeamID context call-site was not found.\n");
        return;
    }

    uint8_t * displayCallSite = reinterpret_cast<uint8_t *>(sequence.Start);
    if(0xE8 != displayCallSite[0]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_teamid] TeamID display call-site has an unexpected opcode.\n");
        return;
    }

    int32_t displayNativeRelative = 0;
    memcpy(&displayNativeRelative, displayCallSite + 1, sizeof(displayNativeRelative));
    g_GetNativeTeamIdDisplayPlayer = reinterpret_cast<GetTeamIdContextPlayerFn>(
        displayCallSite + 5 + displayNativeRelative);

    auto relationshipSequence = Afx::BinUtils::FindPatternString(
        textRange,
        "45 84 ED 75 ?? 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 84 C0 0F 85");
    if(relationshipSequence.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_teamid] TeamID relationship call-site was not found.\n");
        g_GetNativeTeamIdDisplayPlayer = nullptr;
        return;
    }

    uint8_t * firstRelationshipCall = reinterpret_cast<uint8_t *>(relationshipSequence.Start) + 11;
    if(0xE8 != firstRelationshipCall[0]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_teamid] TeamID relationship call-site has an unexpected opcode.\n");
        g_GetNativeTeamIdDisplayPlayer = nullptr;
        return;
    }

    int32_t nativeRelative = 0;
    memcpy(&nativeRelative, firstRelationshipCall + 1, sizeof(nativeRelative));
    uint8_t * nativeRelationshipAddress = firstRelationshipCall + 5 + nativeRelative;
    g_GetNativeTeamIdRelationship = reinterpret_cast<GetTeamIdRelationshipFn>(nativeRelationshipAddress);

    uint8_t * relationshipCallSites[kTeamIdRelationshipCallCount] = {};
    size_t relationshipCallCount = 0;
    uint8_t * scan = reinterpret_cast<uint8_t *>(sequence.Start);
    uint8_t * scanEnd = scan + 0x1000;
    if(reinterpret_cast<size_t>(scanEnd) > textRange.End) {
        scanEnd = reinterpret_cast<uint8_t *>(textRange.End);
    }
    for(uint8_t * cursor = scan; cursor + 5 <= scanEnd; ++cursor) {
        if(0xE8 != cursor[0]) continue;
        int32_t relative = 0;
        memcpy(&relative, cursor + 1, sizeof(relative));
        if(cursor + 5 + relative != nativeRelationshipAddress) continue;
        if(relationshipCallCount >= kTeamIdRelationshipCallCount) {
            relationshipCallCount = kTeamIdRelationshipCallCount + 1;
            break;
        }
        relationshipCallSites[relationshipCallCount++] = cursor;
    }
    if(kTeamIdRelationshipCallCount != relationshipCallCount) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_teamid] Expected %u TeamID relationship calls, found %u.\n",
            static_cast<unsigned int>(kTeamIdRelationshipCallCount),
            static_cast<unsigned int>(relationshipCallCount));
        g_GetNativeTeamIdDisplayPlayer = nullptr;
        g_GetNativeTeamIdRelationship = nullptr;
        return;
    }

    uint8_t * thunk = MirvPovHookUtils::AllocateNear(firstRelationshipCall, 16);
    if(nullptr == thunk) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_teamid] Could not allocate the TeamID relationship thunk.\n");
        g_GetNativeTeamIdDisplayPlayer = nullptr;
        g_GetNativeTeamIdRelationship = nullptr;
        return;
    }

    // mov rax, GetTeamIdRelationship; jmp rax
    thunk[0] = 0x48;
    thunk[1] = 0xB8;
    uint64_t wrapperAddress = reinterpret_cast<uint64_t>(&GetTeamIdRelationship);
    memcpy(thunk + 2, &wrapperAddress, sizeof(wrapperAddress));
    thunk[10] = 0xFF;
    thunk[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), thunk, 12);

    int32_t thunkRelatives[kTeamIdRelationshipCallCount] = {};
    for(size_t i = 0; i < kTeamIdRelationshipCallCount; ++i) {
        if(!MirvPovHookUtils::CalcRel32(relationshipCallSites[i] + 5, thunk, thunkRelatives[i])) {
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_teamid] TeamID relationship thunk is out of range.\n");
            VirtualFree(thunk, 0, MEM_RELEASE);
            g_GetNativeTeamIdDisplayPlayer = nullptr;
            g_GetNativeTeamIdRelationship = nullptr;
            return;
        }
    }

    for(size_t i = 0; i < kTeamIdRelationshipCallCount; ++i) {
        uint8_t replacement[5] = {0xE8, 0, 0, 0, 0};
        memcpy(replacement + 1, &thunkRelatives[i], sizeof(thunkRelatives[i]));
        memcpy(g_TeamIdRelationshipOriginalCalls[i], relationshipCallSites[i], sizeof(replacement));

        MdtMemBlockInfos memory;
        MdtMemAccessBegin(relationshipCallSites[i], sizeof(replacement), &memory);
        memcpy(relationshipCallSites[i], replacement, sizeof(replacement));
        FlushInstructionCache(GetCurrentProcess(), relationshipCallSites[i], sizeof(replacement));
        MdtMemAccessEnd(&memory);
        g_TeamIdRelationshipCallSites[i] = relationshipCallSites[i];
    }

    g_TeamIdRelationshipThunk = thunk;
    g_TeamIdPatched = true;
}

void MirvPovTeamID_RemovePatches()
{
    if(!g_TeamIdPatched) return;

    bool restored = true;
    for(size_t i = 0; i < kTeamIdRelationshipCallCount; ++i) {
        if(nullptr == g_TeamIdRelationshipCallSites[i]) {
            restored = false;
            continue;
        }

        MdtMemBlockInfos memory;
        MdtMemAccessBegin(
            g_TeamIdRelationshipCallSites[i],
            sizeof(g_TeamIdRelationshipOriginalCalls[i]),
            &memory);
        memcpy(
            g_TeamIdRelationshipCallSites[i],
            g_TeamIdRelationshipOriginalCalls[i],
            sizeof(g_TeamIdRelationshipOriginalCalls[i]));
        restored = restored && 0 == memcmp(
            g_TeamIdRelationshipCallSites[i],
            g_TeamIdRelationshipOriginalCalls[i],
            sizeof(g_TeamIdRelationshipOriginalCalls[i]));
        MdtMemAccessEnd(&memory);
    }
    restored = restored && 0 != FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    if(!restored) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_teamid] Could not confirm relationship-call restoration; keeping the thunk allocated.\n");
        return;
    }

    if(g_TeamIdRelationshipThunk) VirtualFree(g_TeamIdRelationshipThunk, 0, MEM_RELEASE);
    for(size_t i = 0; i < kTeamIdRelationshipCallCount; ++i) {
        g_TeamIdRelationshipCallSites[i] = nullptr;
    }
    g_TeamIdRelationshipThunk = nullptr;
    g_GetNativeTeamIdDisplayPlayer = nullptr;
    g_GetNativeTeamIdRelationship = nullptr;
    g_TeamIdPatched = false;
}
