/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Modified in 2026. See ../NOTICE.md for provenance and license details.
 */

#pragma once

#include <Windows.h>

void MirvPovSoundCircle_Initialize(HMODULE clientDll);
// Resolve and validate the owned-weather sound interface (direct start helper,
// its sound-event interface global, the unique stop wrapper and the owned
// system singleton) WITHOUT installing the POV radar/local-pawn/network sound
// hooks. Weather uses only this entry point, so a failed POV initialization can
// never erase weather audio state. It fails closed when the native code no
// longer matches and never clears an interface it already verified.
void MirvPovSoundCircle_InitializeOwnedAudio(HMODULE clientDll);
void MirvPovSoundCircle_ResetDemoState();
bool MirvPovSoundCircle_IsHooked();
bool MirvPovSoundCircle_IsDirectEmitterReady();

// Emit a native CS2 sound-event with an explicit source entity.  The caller
// must pass the player pawn/entity entry index so the engine can apply normal
// positional attenuation and stereo spatialization.  This never falls back
// to a listener-local console "play" command.
bool MirvPovSoundCircle_EmitSoundAtEntity(const char * soundName, int sourceEntityIndex);

// Emit the same native CS2 sound-event path with entidx=-1, matching the
// original SendAudio/RawAudio user-message behavior (global, non-spatialized).
bool MirvPovSoundCircle_EmitSoundGlobal(const char * soundName);

// Owned weather sounds use only the verified direct path and return their GUID.
// Stop affects that GUID only, never the game's unrelated audio.
struct MirvOwnedSound { unsigned char guid[20] {}; void * system = nullptr; };

// Stage of the most recent owned-sound failure. Diagnostic only: it never
// changes what is started, and it stays available in normal Release builds so
// a status command can distinguish a missing native interface from a missing
// sound definition or a rejected event name.
enum MirvOwnedSoundStage {
    MirvOwnedSoundStage_None = 0,
    MirvOwnedSoundStage_Init,
    MirvOwnedSoundStage_Start,
    MirvOwnedSoundStage_Stop,
    MirvOwnedSoundStage_System,
    MirvOwnedSoundStage_EventInterface,
    MirvOwnedSoundStage_Name,
    MirvOwnedSoundStage_Guid
};
const char * MirvPovSoundCircle_OwnedSoundStageName(int stage);

struct MirvOwnedSoundReadiness {
    bool hooked;
    bool startHelper;
    bool stopHelper;
    bool ownedSystem;
    bool eventInterface;
    int lastFailureStage;
    unsigned attempts;
    unsigned started;
};
// `hooked` here means "the owned interface was resolved", not "the POV hooks
// were installed": weather audio readiness is deliberately independent of the
// POV sound-circle.
MirvOwnedSoundReadiness MirvPovSoundCircle_OwnedSoundReadiness();
int MirvPovSoundCircle_OwnedSoundLastFailure();

// A start can only use the owned path once the owned start helper, event
// interface, stop helper and owned-sound system slot are all present. The POV
// radar/local-pawn/network hooks are not required.
bool MirvPovSoundCircle_IsOwnedSoundReady();

// Resolve and validate an owned-sound event name without starting playback.
// Returns MirvOwnedSoundStage_None when the name is a valid registered event.
int MirvPovSoundCircle_ProbeOwnedSoundEvent(const char * name);

bool MirvPovSoundCircle_StartOwnedSound(const char * name, float volume, MirvOwnedSound & sound);
void MirvPovSoundCircle_StopOwnedSound(MirvOwnedSound & sound);
