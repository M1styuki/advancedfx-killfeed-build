/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Modified in 2026. See ../NOTICE.md for provenance and license details.
 */

#include "stdafx.h"

#include "MirvPovSoundCircle.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "Globals.h"
#include "MirvPovRadio.h"
#include "SchemaSystem.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#include <Windows.h>
#include <atomic>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

namespace {

using GetLocalPawn_t = CEntityInstance * (__fastcall *)();
using DoStartSoundEvent_t = void (__fastcall *)(void *, void *);
// IDA: sub_1803D91F0 is the native helper used by CS2's direct sound-event
// producers.  It accepts the resolved event id and source entity directly,
// and normalizes pitch when the caller passes -1.  This avoids constructing a
// temporary network-message CBufferString for synthetic radio playback.
using StartSoundEvent_t = void * (__fastcall *)(
    void * soundOpGameSystem,
    void * result,
    void * context,
    uint32_t eventId,
    uint32_t sourceEntityIndex,
    int16_t pitch,
    double volume);
using QueueRadarSound_t = void (__fastcall *)(CEntityInstance *, int, float, bool);
using HashSoundField_t = uint32_t (__fastcall *)(const char *, uint32_t);
using ResolveSoundEventId_t = uint32_t (__fastcall *)(void *, const char *, bool);
using IsSoundEventValid_t = bool (__fastcall *)(void *, uint32_t);
using GetSoundEventName_t = const char * (__fastcall *)(void *, uint32_t);
using GetSoundFieldCount_t = int (__fastcall *)(void *, uint32_t, uint32_t, bool *, bool);
using GetSoundFieldValue_t = bool (__fastcall *)(void *, uint32_t, uint32_t, const void **, int, bool);

// Unique native scan patterns. Each is kept as a single literal so the POV
// sound-circle and the independent owned-weather resolution share exactly the
// same verified scans.
constexpr const char * kStartSoundEventPattern =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 8B FA 48 8D 15";
constexpr const char * kStopSoundWrapperPattern =
    "48 8D 9F A0 06 00 00 39 33 74 ?? 48 8B 0D ?? ?? ?? ?? 45 33 C0 48 8B D3 E8 ?? ?? ?? ??";

GetLocalPawn_t g_OrgGetLocalPawn = nullptr;
GetLocalPawn_t g_OrgGetSoundViewPawn = nullptr;
DoStartSoundEvent_t g_OrgDoStartSoundEvent = nullptr;
StartSoundEvent_t g_StartSoundEvent = nullptr;
using StopOwnedSound_t = void (__fastcall *)(void *, void *, int);
StopOwnedSound_t g_StopOwnedSound = nullptr;
void ** g_OwnedSoundSystemSlot = nullptr;
// Dedicated owned-weather audio interface. It is resolved independently of the
// POV sound-circle so a failed POV detour install can never clear weather audio
// state, and so weather never has to install radar/local-pawn/network hooks.
StartSoundEvent_t g_OwnedStartSoundEvent = nullptr;
void ** g_OwnedEventInterfaceSlot = nullptr;
bool g_OwnedAudioReady = false;
QueueRadarSound_t g_QueueRadarSound = nullptr;
void ** g_SoundEventInterfaceSlot = nullptr;
void * g_SoundGateReturnAddresses[3] = {};
uint32_t g_DistanceCurveKey = 0;
bool g_Hooked = false;
std::atomic_bool g_NativeProducerSeen = false;
std::atomic<int> g_OwnedSoundStage {MirvOwnedSoundStage_None};
std::atomic<unsigned> g_OwnedSoundAttempts {0};
std::atomic<unsigned> g_OwnedSoundStarted {0};
SRWLOCK g_SyntheticSoundLock = SRWLOCK_INIT;
void * g_LastSoundOpGameSystem = nullptr;
unsigned char g_LastSoundMessageTemplate[0x68] = {};
bool g_LastSoundMessageTemplateValid = false;

constexpr size_t kSoundEventMessageSize = 0x68;

// The field at message+0x48 is a pointer to a CUtlString-like object.  For
// short event names the object stores the bytes inline and uses capacity <=
// 0x0f; the native handler then reads the object address itself as the string
// pointer.  Keeping this exact shape lets us reuse DoStartSoundEvent without
// invoking any listener-local console command.
struct SyntheticSoundString {
    char inlineData[16];
    int32_t length;
    int32_t reserved;
    uint64_t capacity;
};
static_assert(offsetof(SyntheticSoundString, length) == 0x10, "sound string length offset");
static_assert(offsetof(SyntheticSoundString, capacity) == 0x18, "sound string capacity offset");

void * GetSoundEventInterface();
bool IsExecutableAddress(const void * address);

size_t FindUniqueOwnedAudioAddress(HMODULE module, const char * pattern) {
    Afx::BinUtils::ImageSectionsReader sections(module);
    auto range=sections.GetMemRange();
    auto found=FindPatternString(range,pattern);
    if(found.IsEmpty()) return 0;
    if(!FindPatternString(Afx::BinUtils::MemRange(found.Start+1,range.End),pattern).IsEmpty()) return 0;
    return found.Start;
}

bool ReadSoundEventInterface(
    void ** slot,
    void *& outInterface,
    ResolveSoundEventId_t & outResolve,
    IsSoundEventValid_t & outIsValid,
    GetSoundEventName_t & outGetName)
{
    outInterface = nullptr;
    outResolve = nullptr;
    outIsValid = nullptr;
    outGetName = nullptr;
    __try {
        outInterface = nullptr != slot ? *slot : nullptr;
        if(nullptr == outInterface) return false;
        outInterface = reinterpret_cast<unsigned char *>(outInterface) + 8;
        void ** vtable = *reinterpret_cast<void ***>(outInterface);
        if(nullptr == vtable) return false;
        outResolve = reinterpret_cast<ResolveSoundEventId_t>(vtable[0]);
        // IDA: qword_1825C9508 + 8 vtable slot 0 resolves a name to an
        // event id, slot 1 validates that id, and slot 2 returns its canonical
        // name.  A non-zero resolver result alone is only a hash/cache key;
        // unknown names also produce a non-zero value on this build.
        outIsValid = reinterpret_cast<IsSoundEventValid_t>(vtable[1]);
        outGetName = reinterpret_cast<GetSoundEventName_t>(vtable[2]);
        return IsExecutableAddress(reinterpret_cast<void *>(outResolve))
            && IsExecutableAddress(reinterpret_cast<void *>(outIsValid))
            && IsExecutableAddress(reinterpret_cast<void *>(outGetName));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        outInterface = nullptr;
        outResolve = nullptr;
        outIsValid = nullptr;
        outGetName = nullptr;
        return false;
    }
}

bool IsExecutableAddress(const void * address)
{
    if(nullptr == address) return false;
    MEMORY_BASIC_INFORMATION information = {};
    if(0 == VirtualQuery(address, &information, sizeof(information))) return false;
    if(MEM_COMMIT != information.State || 0 != (information.Protect & PAGE_GUARD)) return false;
    const DWORD protection = information.Protect & 0xff;
    return PAGE_EXECUTE == protection
        || PAGE_EXECUTE_READ == protection
        || PAGE_EXECUTE_READWRITE == protection
        || PAGE_EXECUTE_WRITECOPY == protection;
}

// Range guard for a resolved address: it must be committed and inside the
// client module image, and executable when the target is called as code.
bool IsModuleAddress(HMODULE module, const void * address, bool executable)
{
    if(nullptr == module || nullptr == address) return false;
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto image = reinterpret_cast<const unsigned char *>(module);
    __try {
        const auto * dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
        if(IMAGE_DOS_SIGNATURE != dos->e_magic) return false;
        const auto * nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(image + dos->e_lfanew);
        if(IMAGE_NT_SIGNATURE != nt->Signature) return false;
        const auto size = static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
        const auto target = reinterpret_cast<uintptr_t>(address);
        if(target < base || target >= base + size) return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if(executable) return IsExecutableAddress(address);
    MEMORY_BASIC_INFORMATION information = {};
    if(0 == VirtualQuery(address, &information, sizeof(information))) return false;
    if(MEM_COMMIT != information.State || 0 != (information.Protect & PAGE_GUARD)) return false;
    const DWORD protection = information.Protect & 0xff;
    return PAGE_NOACCESS != protection && PAGE_EXECUTE != protection;
}

uint32_t FinalizeSoundFieldHash(uint32_t hash)
{
    uint32_t result = hash * 0x5bd1e995;
    result ^= 0x66608f41;
    result = (result ^ (result >> 13)) * 0x5bd1e995;
    return result ^ (result >> 15);
}

CEntityInstance * ResolveSoundSourcePawn(int entityIndex)
{
    if(entityIndex < 0 || 0x7ffe < entityIndex) return nullptr;
    CEntityInstance * entity = GetEntityFromIndex(entityIndex);
    if(nullptr == entity) return nullptr;
    if(entity->IsPlayerPawn()) return entity;

    if(0 == g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode
        || 0 == g_clientDllOffsets.CGameSceneNode.m_pParent
        || 0 == g_clientDllOffsets.CGameSceneNode.m_pOwner) return nullptr;
    unsigned char * sceneNode = *reinterpret_cast<unsigned char **>(
        reinterpret_cast<unsigned char *>(entity)
        + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
    if(nullptr == sceneNode) return nullptr;
    unsigned char * parentNode = *reinterpret_cast<unsigned char **>(
        sceneNode + g_clientDllOffsets.CGameSceneNode.m_pParent);
    if(nullptr == parentNode) return nullptr;
    CEntityInstance * parent = *reinterpret_cast<CEntityInstance **>(
        parentNode + g_clientDllOffsets.CGameSceneNode.m_pOwner);
    return nullptr != parent && parent->IsPlayerPawn() ? parent : nullptr;
}

bool IsCurrentPovPlayer(CEntityInstance * pawn)
{
    if(nullptr == pawn) return false;
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr != povPawn && pawn == povPawn) return true;

    CEntityInstance * povController = GetCurrentPovPlayerController();
    if(nullptr == povController) return false;
    auto pawnController = pawn->GetPlayerControllerHandle();
    auto povControllerHandle = povController->GetHandle();
    return pawnController.IsValid()
        && povControllerHandle.IsValid()
        && pawnController.ToInt() == povControllerHandle.ToInt();
}

void * GetSoundEventInterface()
{
    void * interfaceBase = nullptr != g_SoundEventInterfaceSlot
        ? *g_SoundEventInterfaceSlot
        : nullptr;
    return nullptr != interfaceBase
        ? reinterpret_cast<unsigned char *>(interfaceBase) + 8
        : nullptr;
}

int GetNativeSoundRadius(void * soundEventInterface, uint32_t eventId)
{
    if(nullptr == soundEventInterface) return 0;
    void ** vtable = *reinterpret_cast<void ***>(soundEventInterface);
    auto getCount = reinterpret_cast<GetSoundFieldCount_t>(vtable[0x180 / sizeof(void *)]);
    auto getValue = reinterpret_cast<GetSoundFieldValue_t>(vtable[0x1a0 / sizeof(void *)]);
    if(nullptr == getCount || nullptr == getValue) return 0;

    const void * value = nullptr;
    if(getValue(soundEventInterface, eventId, 0x00442583, &value, 0, false)
        && nullptr != value
        && *reinterpret_cast<const float *>(value) <= 0.5f) return 0;

    float radius = 0.0f;
    value = nullptr;
    if(getValue(soundEventInterface, eventId, 0x7d58c040, &value, 0, false)
        && nullptr != value) {
        radius = *reinterpret_cast<const float *>(value);
    }
    if(radius <= 0.0f) {
        bool isArray = false;
        int count = getCount(
            soundEventInterface,
            eventId,
            g_DistanceCurveKey,
            &isArray,
            false);
        value = nullptr;
        if(isArray && 1 < count
            && getValue(
                soundEventInterface,
                eventId,
                g_DistanceCurveKey,
                &value,
                count - 1,
                false)
            && nullptr != value) {
            radius = *reinterpret_cast<const float *>(value);
        }
    }
    return 1.0f <= radius ? static_cast<int>(radius) : 0;
}

CEntityInstance * OverrideSoundPawn(CEntityInstance * nativePawn, void * returnAddress)
{
    if(!MIRV_POV_FEATURE_ACTIVE("soundcircle")) return nativePawn;

    int gate = returnAddress == g_SoundGateReturnAddresses[0]
        ? 0
        : (returnAddress == g_SoundGateReturnAddresses[1]
            ? 1
            : (returnAddress == g_SoundGateReturnAddresses[2] ? 2 : -1));
    if(gate < 0) return nativePawn;

    if(0 == gate) g_NativeProducerSeen = true;
    __try {
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn) return nativePawn;
        return povPawn;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nativePawn;
    }
}

CEntityInstance * __fastcall New_GetLocalPawn()
{
    void * previous = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * caller = MirvPov_GetHookReturnAddress();
    CEntityInstance * pawn = g_OrgGetLocalPawn();
    MirvPov_PopHookReturnAddress(previous);
    return OverrideSoundPawn(pawn, caller);
}

CEntityInstance * __fastcall New_GetSoundViewPawn()
{
    void * previous = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * caller = MirvPov_GetHookReturnAddress();
    CEntityInstance * pawn = g_OrgGetSoundViewPawn();
    MirvPov_PopHookReturnAddress(previous);
    return OverrideSoundPawn(pawn, caller);
}

void __fastcall New_DoStartSoundEvent(void * soundOpGameSystem, void * netMessage)
{
    uint32_t eventHash = 0;
    CEntityInstance * sourcePawn = nullptr;
    bool isPov = false;
    __try {
        if(nullptr != netMessage) {
            unsigned char * message = reinterpret_cast<unsigned char *>(netMessage);
            eventHash = *reinterpret_cast<uint32_t *>(message + 0x54);
            int sourceEntityIndex = *reinterpret_cast<int *>(message + 0x60);
            sourcePawn = ResolveSoundSourcePawn(sourceEntityIndex);
            isPov = IsCurrentPovPlayer(sourcePawn);

            // Retain a native message template and the sound-op system so a
            // later event fallback can re-enter the real CS2 sound-event path
            // with a different event id/source entity.  The +0x48 string
            // pointer is overwritten by EmitSoundAtEntity and is never kept
            // from this temporary network message.
            AcquireSRWLockExclusive(&g_SyntheticSoundLock);
            memcpy(g_LastSoundMessageTemplate, message, kSoundEventMessageSize);
            g_LastSoundOpGameSystem = soundOpGameSystem;
            g_LastSoundMessageTemplateValid = true;
            ReleaseSRWLockExclusive(&g_SyntheticSoundLock);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    g_OrgDoStartSoundEvent(soundOpGameSystem, netMessage);

    if(!MIRV_POV_FEATURE_ACTIVE("soundcircle") || 0 == eventHash) return;

    __try {
        void * soundEventInterface = GetSoundEventInterface();
        if(nullptr == soundEventInterface) return;
        void ** vtable = *reinterpret_cast<void ***>(soundEventInterface);
        auto getName = reinterpret_cast<GetSoundEventName_t>(vtable[2]);
        auto resolveEventId = reinterpret_cast<ResolveSoundEventId_t>(vtable[0]);
        if(nullptr == getName || nullptr == resolveEventId) return;

        const char * name = getName(soundEventInterface, eventHash);
        if(nullptr == name) return;

        // Radio/throw/bomb sound events can be emitted with an entity index
        // that is not a player pawn (or -1 in demos).  Let the radio layer
        // consume the name and use its POV/controller fallback; radar sound
        // synthesis below still requires a resolved pawn.
        MirvPovRadio_HandleSoundEvent(sourcePawn, name);

        if(!isPov || nullptr == sourcePawn || g_NativeProducerSeen.load() || nullptr == strstr(name, ".Step")) return;

        uint32_t eventId = resolveEventId(soundEventInterface, name, true);
        if(0 == eventId) return;
        int radius = GetNativeSoundRadius(soundEventInterface, eventId);
        if(radius < 1) return;

        g_QueueRadarSound(sourcePawn, radius, 0.5f, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

void MirvPovSoundCircle_ResetDemoState()
{
    // Hooks belong to client.dll, but producer observations and sound-op
    // objects belong to the demo that supplied them.
    g_NativeProducerSeen.store(false);
    AcquireSRWLockExclusive(&g_SyntheticSoundLock);
    g_LastSoundOpGameSystem = nullptr;
    memset(g_LastSoundMessageTemplate, 0, sizeof(g_LastSoundMessageTemplate));
    g_LastSoundMessageTemplateValid = false;
    ReleaseSRWLockExclusive(&g_SyntheticSoundLock);
}

void MirvPovSoundCircle_Initialize(HMODULE clientDll)
{
    if(g_Hooked || nullptr == clientDll) return;

    size_t playerSoundProducer = getAddress(
        clientDll,
        "8B D8 48 85 FF 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 3B F8 0F 85 ?? ?? ?? ?? 0F 28 DE 40 88 74 24 20 44 8B C3");
    size_t queueRadarSound = getAddress(
        clientDll,
        "48 85 C9 74 77 48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 0F 29 74 24 30 41 0F B6 F9 0F 28 F2 8B F2 48 8B D9 E8 ?? ?? ?? ?? 48 3B D8");
    size_t positionUpdaterBody = getAddress(
        clientDll,
        "48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? 00 00 48 89 6C 24 ?? 48 8D 54 24 ?? 48 89 74 24 ?? 48 8B C8");
    if(0 == playerSoundProducer || 0 == queueRadarSound || 0 == positionUpdaterBody) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native radar sound path was not found.\n");
        return;
    }
    uint8_t * callSites[3] = {
        reinterpret_cast<uint8_t *>(playerSoundProducer) + 0x0b,
        reinterpret_cast<uint8_t *>(queueRadarSound) + 0x25,
        reinterpret_cast<uint8_t *>(positionUpdaterBody) + 3
    };
    if(0xe8 != callSites[0][0] || 0xe8 != callSites[1][0] || 0xe8 != callSites[2][0]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native local-Pawn calls changed.\n");
        return;
    }

    uint8_t * localPawnTargets[3];
    for(int i = 0; i < 3; ++i) {
        int32_t relative = *reinterpret_cast<int32_t *>(callSites[i] + 1);
        localPawnTargets[i] = callSites[i] + 5 + relative;
    }
    // The event producer and queue now use the observer-aware view Pawn;
    // the position updater still uses the real local Pawn. Keep both gates
    // scoped to their verified return addresses.
    if(localPawnTargets[0] != localPawnTargets[1]
        || localPawnTargets[0] == localPawnTargets[2]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native local-Pawn target validation failed.\n");
        return;
    }
    if(!IsExecutableAddress(localPawnTargets[0]) || !IsExecutableAddress(localPawnTargets[2])) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native local-Pawn target is not executable.\n");
        return;
    }
    size_t doStartSoundEvent = getAddress(
        clientDll,
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 90 00 00 00 8B 42 40");
    if(0 == doStartSoundEvent) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native SOS start-sound handler was not found.\n");
        return;
    }

    // Direct StartSoundEvent helper, distinct from the network-message handler.
    // The owned-weather stop helper and its system singleton are resolved by
    // MirvPovSoundCircle_InitializeOwnedAudio so this POV path cannot clear
    // weather audio state.
    size_t startSoundEvent = getAddress(clientDll, kStartSoundEventPattern);
    uint8_t * createSoundEventCall = reinterpret_cast<uint8_t *>(doStartSoundEvent) + 0xec;
    if(0xe8 != createSoundEventCall[0]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native SOS create-sound call validation failed.\n");
        return;
    }
    int32_t createSoundEventRelative = *reinterpret_cast<int32_t *>(createSoundEventCall + 1);
    uint8_t * createSoundEvent = createSoundEventCall + 5 + createSoundEventRelative;
    uint8_t * soundEventGlobalLoad = createSoundEvent + 0xb8;
    const uint8_t expectedGlobalLoadTail[] = {0x41, 0x8b, 0xd4, 0x48, 0x83, 0xc1, 0x08};
    if(0x48 != soundEventGlobalLoad[0]
        || 0x8b != soundEventGlobalLoad[1]
        || 0x0d != soundEventGlobalLoad[2]
        || 0 != memcmp(
            soundEventGlobalLoad + 7,
            expectedGlobalLoadTail,
            sizeof(expectedGlobalLoadTail))) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native sound-event interface validation failed.\n");
        return;
    }
    int32_t soundEventGlobalRelative = *reinterpret_cast<int32_t *>(soundEventGlobalLoad + 3);
    void ** soundEventInterfaceSlot = reinterpret_cast<void **>(
        soundEventGlobalLoad + 7 + soundEventGlobalRelative);

    size_t hashSoundField = getAddress(
        clientDll,
        "48 89 5C 24 08 44 0F B6 09 44 8B DA 4C 8B C1 41 8D 41 BF 3C 19 77 04 41 80 C1 20");
    if(0 == hashSoundField) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native sound-field hash helper was not found.\n");
        return;
    }
    auto hashField = reinterpret_cast<HashSoundField_t>(hashSoundField);
    static const char distanceCurvePath[] = "public.distance_volume_mapping_curve";
    uint32_t curveHash = hashField(distanceCurvePath + 16, 0x26835b00);
    curveHash = hashField(distanceCurvePath + 24, curveHash);
    uint32_t distanceCurveKey = FinalizeSoundFieldHash(curveHash);
    if(0xd7da5bc8 != distanceCurveKey) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native distance-curve key validation failed.\n");
        return;
    }

    g_OrgGetSoundViewPawn = reinterpret_cast<GetLocalPawn_t>(localPawnTargets[0]);
    g_OrgGetLocalPawn = reinterpret_cast<GetLocalPawn_t>(localPawnTargets[2]);
    g_OrgDoStartSoundEvent = reinterpret_cast<DoStartSoundEvent_t>(doStartSoundEvent);
    g_StartSoundEvent = reinterpret_cast<StartSoundEvent_t>(startSoundEvent);
    g_QueueRadarSound = reinterpret_cast<QueueRadarSound_t>(queueRadarSound);
    g_SoundEventInterfaceSlot = soundEventInterfaceSlot;
    g_DistanceCurveKey = distanceCurveKey;
    for(int i = 0; i < 3; ++i) g_SoundGateReturnAddresses[i] = callSites[i] + 5;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgGetLocalPawn, New_GetLocalPawn);
    DetourAttach(&(PVOID &)g_OrgGetSoundViewPawn, New_GetSoundViewPawn);
    DetourAttach(&(PVOID &)g_OrgDoStartSoundEvent, New_DoStartSoundEvent);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgGetLocalPawn = nullptr;
        g_OrgGetSoundViewPawn = nullptr;
        g_OrgDoStartSoundEvent = nullptr;
        g_StartSoundEvent = nullptr;
        g_QueueRadarSound = nullptr;
        g_SoundEventInterfaceSlot = nullptr;
        g_DistanceCurveKey = 0;
        g_SoundGateReturnAddresses[0] = nullptr;
        g_SoundGateReturnAddresses[1] = nullptr;
        g_SoundGateReturnAddresses[2] = nullptr;
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native sound-circle detour failed.\n");
        return;
    }

    g_Hooked = true;
}

void MirvPovSoundCircle_InitializeOwnedAudio(HMODULE clientDll)
{
    // Weather-owned audio is resolved here, without installing any POV hook.
    // Failure never clears a previously verified owned interface and never
    // touches POV state, so a missing radar/local-pawn hook cannot mute weather.
    if(g_OwnedAudioReady || nullptr == clientDll) return;

    auto fail = [](MirvOwnedSoundStage stage) { g_OwnedSoundStage.store(stage); };

    // Direct StartSoundEvent helper: the unique native producer pattern already
    // used by the POV path, resolved into the dedicated owned pointer.
    size_t startSoundEvent = FindUniqueOwnedAudioAddress(clientDll, kStartSoundEventPattern);
    if(0 == startSoundEvent || !IsModuleAddress(clientDll, reinterpret_cast<void *>(startSoundEvent), true)) {
        fail(MirvOwnedSoundStage_Init);
        return;
    }
    auto * start = reinterpret_cast<unsigned char *>(startSoundEvent);

    // At +0x42 the helper loads the sound-event interface global; +0x45 is the
    // RIP-relative displacement and +0x49 begins the tail that calls the
    // event-id resolver through the interface vtable:
    //   mov r8b,1 ; add rcx,8 ; mov rax,[rcx] ; call [rax]
    // Validate the whole tail before deriving the slot, and fail closed if the
    // native code changes.
    const unsigned char expectedGlobalLoad[3] = {0x48, 0x8B, 0x0D};
    const unsigned char expectedTail[12] = {0x41, 0xB0, 0x01, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0x10};
    if(!IsModuleAddress(clientDll, start + 0x42, true)
        || !IsModuleAddress(clientDll, start + 0x49 + sizeof(expectedTail) - 1, true)
        || 0 != memcmp(start + 0x42, expectedGlobalLoad, sizeof(expectedGlobalLoad))
        || 0 != memcmp(start + 0x49, expectedTail, sizeof(expectedTail))) {
        fail(MirvOwnedSoundStage_Start);
        return;
    }
    void ** ownedEventInterfaceSlot = reinterpret_cast<void **>(start + 0x49 + *reinterpret_cast<int32_t *>(start + 0x45));
    if(!IsModuleAddress(clientDll, ownedEventInterfaceSlot, false)) {
        fail(MirvOwnedSoundStage_EventInterface);
        return;
    }

    // Unique stop wrapper; the validated callee prefix resolves the direct stop
    // helper and the system singleton slot the wrapped stop call uses.
    size_t stopSite = FindUniqueOwnedAudioAddress(clientDll, kStopSoundWrapperPattern);
    StopOwnedSound_t stop = nullptr;
    void ** ownedSystemSlot = nullptr;
    if(stopSite) {
        auto * stopFn = reinterpret_cast<unsigned char *>(stopSite + 29 + *reinterpret_cast<int32_t *>(stopSite + 25));
        const unsigned char expected[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x83,0x3a,0x00,0x48,0x8b,0xda,0x74};
        if(IsModuleAddress(clientDll, stopFn, true)
            && IsModuleAddress(clientDll, stopFn+sizeof(expected)-1, true)
            && 0 == memcmp(stopFn, expected, sizeof(expected))) {
            void ** systemSlot = reinterpret_cast<void **>(stopSite + 18 + *reinterpret_cast<int32_t *>(stopSite + 14));
            if(IsModuleAddress(clientDll, systemSlot, false)) {
                stop = reinterpret_cast<StopOwnedSound_t>(stopFn);
                ownedSystemSlot = systemSlot;
            }
        }
    }
    if(nullptr == stop) { fail(MirvOwnedSoundStage_Stop); return; }
    if(nullptr == ownedSystemSlot) { fail(MirvOwnedSoundStage_System); return; }

    // The interface must expose the resolve/validate/canonical-name slots before
    // weather may use the owned path at all.
    void * iface = nullptr;
    ResolveSoundEventId_t resolve = nullptr;
    IsSoundEventValid_t valid = nullptr;
    GetSoundEventName_t canonical = nullptr;
    if(!ReadSoundEventInterface(ownedEventInterfaceSlot, iface, resolve, valid, canonical)) {
        fail(MirvOwnedSoundStage_EventInterface);
        return;
    }

    g_OwnedStartSoundEvent = reinterpret_cast<StartSoundEvent_t>(startSoundEvent);
    g_OwnedEventInterfaceSlot = ownedEventInterfaceSlot;
    g_StopOwnedSound = stop;
    g_OwnedSoundSystemSlot = ownedSystemSlot;
    g_OwnedAudioReady = true;
    g_OwnedSoundStage.store(MirvOwnedSoundStage_None);
}

bool MirvPovSoundCircle_IsHooked()
{
    return g_Hooked;
}

bool MirvPovSoundCircle_IsDirectEmitterReady()
{
    return g_Hooked && nullptr != g_StartSoundEvent;
}

const char * MirvPovSoundCircle_OwnedSoundStageName(int stage)
{
    switch(stage) {
    case MirvOwnedSoundStage_None: return "none";
    case MirvOwnedSoundStage_Init: return "init";
    case MirvOwnedSoundStage_Start: return "start";
    case MirvOwnedSoundStage_Stop: return "stop";
    case MirvOwnedSoundStage_System: return "system";
    case MirvOwnedSoundStage_EventInterface: return "event-interface";
    case MirvOwnedSoundStage_Name: return "name";
    case MirvOwnedSoundStage_Guid: return "guid";
    default: return "unknown";
    }
}

bool MirvPovSoundCircle_IsOwnedSoundReady()
{
    // Readiness is about the verified owned native interface, not about a
    // definition or event name having been resolved for one particular sound,
    // and not about the POV sound-circle hooks.
    return g_OwnedAudioReady
        && nullptr != g_OwnedStartSoundEvent
        && nullptr != g_StopOwnedSound
        && nullptr != g_OwnedSoundSystemSlot;
}

int MirvPovSoundCircle_OwnedSoundLastFailure()
{
    return g_OwnedSoundStage.load();
}

MirvOwnedSoundReadiness MirvPovSoundCircle_OwnedSoundReadiness()
{
    MirvOwnedSoundReadiness readiness {};
    readiness.hooked = g_OwnedAudioReady;
    readiness.startHelper = nullptr != g_OwnedStartSoundEvent;
    readiness.stopHelper = nullptr != g_StopOwnedSound;
    readiness.ownedSystem = nullptr != g_OwnedSoundSystemSlot;
    if(g_OwnedAudioReady) {
        void * soundEventInterface = nullptr;
        ResolveSoundEventId_t resolveEventId = nullptr;
        IsSoundEventValid_t isSoundEventValid = nullptr;
        GetSoundEventName_t getName = nullptr;
        readiness.eventInterface = ReadSoundEventInterface(
            g_OwnedEventInterfaceSlot, soundEventInterface, resolveEventId, isSoundEventValid, getName);
    }
    readiness.lastFailureStage = g_OwnedSoundStage.load();
    readiness.attempts = g_OwnedSoundAttempts.load();
    readiness.started = g_OwnedSoundStarted.load();
    return readiness;
}

int MirvPovSoundCircle_ProbeOwnedSoundEvent(const char * name)
{
    // Diagnostic resolution only: no sound is started and no state is changed.
    // It separates "the definition is retained" from "this event id resolves
    // and validates in the native sound-event interface". The registered-event
    // stages are independent of whether one particular GUID is currently active.
    if(!g_OwnedAudioReady) return MirvOwnedSoundStage_Init;
    if(nullptr == name || '\0' == name[0]) return MirvOwnedSoundStage_Name;
    void * soundEventInterface = nullptr;
    ResolveSoundEventId_t resolveEventId = nullptr;
    IsSoundEventValid_t isSoundEventValid = nullptr;
    GetSoundEventName_t getName = nullptr;
    if(!ReadSoundEventInterface(g_OwnedEventInterfaceSlot, soundEventInterface, resolveEventId, isSoundEventValid, getName))
        return MirvOwnedSoundStage_EventInterface;
    __try {
        const uint32_t eventId = resolveEventId(soundEventInterface, name, true);
        if(0 == eventId) return MirvOwnedSoundStage_Name;
        if(!isSoundEventValid(soundEventInterface, eventId)) return MirvOwnedSoundStage_Name;
        const char * resolvedName = getName(soundEventInterface, eventId);
        if(nullptr == resolvedName || '\0' == resolvedName[0]) return MirvOwnedSoundStage_Name;
        return MirvOwnedSoundStage_None;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return MirvOwnedSoundStage_EventInterface;
    }
}

bool MirvPovSoundCircle_EmitSoundAtEntity(const char * soundName, int sourceEntityIndex)
{
    if(!g_Hooked || nullptr == g_OrgDoStartSoundEvent
        || nullptr == soundName || '\0' == soundName[0]
        || sourceEntityIndex < -1) return false;
    // Re-resolve the pawn at emission time.  A demo seek or entity slot reuse
    // must never make a delayed fallback play from an unrelated entity.
    if(sourceEntityIndex >= 0 && nullptr == ResolveSoundSourcePawn(sourceEntityIndex)) return false;

    void * soundEventInterface = nullptr;
    ResolveSoundEventId_t resolveEventId = nullptr;
    IsSoundEventValid_t isSoundEventValid = nullptr;
    GetSoundEventName_t getName = nullptr;
    if(!ReadSoundEventInterface(g_SoundEventInterfaceSlot, soundEventInterface, resolveEventId, isSoundEventValid, getName)) return false;

    uint32_t eventId = 0;
    __try {
        eventId = resolveEventId(soundEventInterface, soundName, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        eventId = 0;
    }
    if(0 == eventId) return false;
    __try {
        if(!isSoundEventValid(soundEventInterface, eventId)) return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const char * resolvedName = nullptr;
    __try {
        resolvedName = getName(soundEventInterface, eventId);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        resolvedName = nullptr;
    }
    if(nullptr == resolvedName || '\0' == resolvedName[0]) return false;

    void * soundOpGameSystem = nullptr;
    alignas(16) unsigned char message[kSoundEventMessageSize] = {};
    bool templateValid = false;
    AcquireSRWLockShared(&g_SyntheticSoundLock);
    soundOpGameSystem = g_LastSoundOpGameSystem;
    templateValid = g_LastSoundMessageTemplateValid;
    if(templateValid) memcpy(message, g_LastSoundMessageTemplate, sizeof(message));
    ReleaseSRWLockShared(&g_SyntheticSoundLock);
    if(nullptr == soundOpGameSystem) return false;

    // Prefer the same direct helper used by native CS2 sound producers.  It
    // performs the complete sound-op enqueue and uses the supplied pawn entry
    // for positional attenuation.  Passing pitch=-1 is important: the helper
    // then asks the sound system for its current/default pitch.  A synthetic
    // network message with pitch=0 can be accepted by the parser but produce
    // no audible output.
    if(nullptr != g_StartSoundEvent) {
        alignas(16) unsigned char result[0x40] = {};
        __try {
            g_StartSoundEvent(
                soundOpGameSystem,
                result,
                nullptr,
                eventId,
                static_cast<uint32_t>(sourceEntityIndex < 0 ? -1 : sourceEntityIndex),
                static_cast<int16_t>(-1),
                0.0);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Fall through to the message-compatible path for older builds or
            // a client update where the direct helper signature changed.
        }
    }

    SyntheticSoundString soundString = {};
    // CBufferString stores up to 15 bytes inline.  Agent-qualified CS2 voice
    // events (for example professional.radiobotgo01) are longer, so use the
    // heap representation instead of silently truncating the event name.
    char longSoundName[256] = {};
    size_t length = strnlen_s(soundName, sizeof(longSoundName) - 1);
    if(0 == length || sizeof(longSoundName) - 1 <= length) return false;
    if(sizeof(soundString.inlineData) - 1 < length) {
        memcpy(longSoundName, soundName, length);
        longSoundName[length] = '\0';
        *reinterpret_cast<char **>(soundString.inlineData) = longSoundName;
        soundString.capacity = static_cast<uint64_t>(length + 1);
    } else {
        memcpy(soundString.inlineData, soundName, length);
        soundString.inlineData[length] = '\0';
        soundString.capacity = sizeof(soundString.inlineData) - 1;
    }
    soundString.length = static_cast<int32_t>(length);
    soundString.reserved = 0;

    __try {
        // The native handler only needs these fields for a regular sound
        // event. Clear flags/volume so stale network metadata cannot alter the
        // fallback's attenuation, then provide the player pawn as the source.
        *reinterpret_cast<uint32_t *>(message + 0x40) = 0;
        *reinterpret_cast<void **>(message + 0x48) = &soundString;
        *reinterpret_cast<int *>(message + 0x50) = 0;
        *reinterpret_cast<uint32_t *>(message + 0x54) = eventId;
        // -1 asks the native handler to use the sound system's default pitch.
        *reinterpret_cast<int16_t *>(message + 0x58) = -1;
        *reinterpret_cast<float *>(message + 0x5c) = 0.0f;
        *reinterpret_cast<int *>(message + 0x60) = sourceEntityIndex;

        // g_OrgDoStartSoundEvent is the Detours trampoline, so this call
        // bypasses New_DoStartSoundEvent and cannot recursively enter the
        // radio observer.  The string object remains alive for the duration
        // of the native call.
        g_OrgDoStartSoundEvent(soundOpGameSystem, message);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    (void)templateValid;
    (void)resolvedName;
    return true;
}

bool MirvPovSoundCircle_EmitSoundGlobal(const char * soundName)
{
    // The native SendAudio/RawAudio path uses entidx=-1.  Keeping this as a
    // separate entry point makes the original-global and optional spatialized
    // fallback behaviors explicit at the radio layer.
    return MirvPovSoundCircle_EmitSoundAtEntity(soundName, -1);
}

bool MirvPovSoundCircle_StartOwnedSound(const char * name, float volume, MirvOwnedSound & sound)
{
    // Every failure path records the stage so a normal Release status command
    // can tell a missing owned interface from a missing definition, an
    // unregistered event name, or a start helper that returned no GUID.
    g_OwnedSoundAttempts.fetch_add(1);
    if(!g_OwnedAudioReady) { g_OwnedSoundStage.store(MirvOwnedSoundStage_Init); return false; }
    if(!name || '\0' == name[0] || volume <= 0.0f || volume > 1.0f) {
        g_OwnedSoundStage.store(MirvOwnedSoundStage_Name); return false;
    }
    if(!g_StopOwnedSound) { g_OwnedSoundStage.store(MirvOwnedSoundStage_Stop); return false; }
    if(!g_OwnedSoundSystemSlot) { g_OwnedSoundStage.store(MirvOwnedSoundStage_System); return false; }
    if(!g_OwnedStartSoundEvent) { g_OwnedSoundStage.store(MirvOwnedSoundStage_Start); return false; }
    MirvPovSoundCircle_StopOwnedSound(sound);
    void * iface = nullptr;
    ResolveSoundEventId_t resolve = nullptr;
    IsSoundEventValid_t valid = nullptr;
    GetSoundEventName_t canonical = nullptr;
    if(!ReadSoundEventInterface(g_OwnedEventInterfaceSlot, iface, resolve, valid, canonical)) {
        g_OwnedSoundStage.store(MirvOwnedSoundStage_EventInterface); return false;
    }
    __try {
        void * system = *g_OwnedSoundSystemSlot;
        if(!system) { g_OwnedSoundStage.store(MirvOwnedSoundStage_System); return false; }
        // The valid-name check stays: resolver hashes and unknown names must not
        // be started, and the source entity stays -1 (global, non-spatialized).
        uint32_t id = resolve(iface, name, true);
        if(!id || !valid(iface, id)) { g_OwnedSoundStage.store(MirvOwnedSoundStage_Name); return false; }
        alignas(16) unsigned char result[0x40] {};
        g_OwnedStartSoundEvent(system, result, nullptr, id, static_cast<uint32_t>(-1), -1, static_cast<double>(volume));
        if(!*reinterpret_cast<uint32_t *>(result)) {
            g_OwnedSoundStage.store(MirvOwnedSoundStage_Guid); return false;
        }
        memcpy(sound.guid, result, sizeof(sound.guid));
        sound.system = system;
        g_OwnedSoundStage.store(MirvOwnedSoundStage_None);
        g_OwnedSoundStarted.fetch_add(1);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_OwnedSoundStage.store(MirvOwnedSoundStage_Start); return false;
    }
}

void MirvPovSoundCircle_StopOwnedSound(MirvOwnedSound & sound)
{
    const bool hasInstance = nullptr != sound.system
        && 0 != *reinterpret_cast<uint32_t *>(sound.guid);
    __try {
        if(g_StopOwnedSound && g_OwnedSoundSystemSlot && sound.system
            && *g_OwnedSoundSystemSlot == sound.system && *reinterpret_cast<uint32_t *>(sound.guid))
            g_StopOwnedSound(sound.system, sound.guid, 0);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    // An instance we can no longer stop is a real failure; keep it observable
    // instead of silently dropping the owned playback.
    if(hasInstance && !g_StopOwnedSound) g_OwnedSoundStage.store(MirvOwnedSoundStage_Stop);
    memset(sound.guid, 0, sizeof(sound.guid));
    sound.system = nullptr;
}
