# Dust2 native rain contact restoration
## CS2 1.41.8.8: restore the reference Dust2 dynamic-object rain contact

Client build/source revision: 1.41.8.8 / 11076591. Analyzed client SHA256: D7DB25D48F1D10C5E0B0296E20ED803426EB9509DA41760DAEDA39DD35BA89B9. Preferred image base: 0x180000000. Addresses below are verified only for that image.

Native behavior: CMapInfo initialization at RVA 0xD2F800 publishes raintracetoskyenabled to its scene world. The unique contiguous call-site signature starts at RVA 0xD2F91A; it reads CEntityInstance.m_pEntity (+0x10), CEntityIdentity.m_worldGroupId (+0x38), and CMapInfo.m_bRainTraceToSkyEnabled (+0x619), resolves the scene world via the unhooked helper at RVA 0x3B9A40, and calls scene interface vtable +0x1B8 at RVA 0xD2F953. Scene/world interface globals resolve to RVA 0x2797E30 / 0x23B8358. The adjacent native +0x1C8 call publishes rain/ripple/direction/wetness/drying floats; this patch does not override those floats.

Reference profile: CS2-insight-agent pov/weather_effects/rain/manifest.json, de_dust2.dynamic_object_rain_contact, changes raintracetoskyenabled from false to true, while env_rain_strength stays 1.0. Airborne rain remains its 385 client-only hosts and wet ground remains authored materials. The original runtime port omitted this contact publication.

Implementation: MirvWeather.cpp resolves the existing helper and scene setter with a unique complete call-site signature plus matching runtime schema layout. It repeats only the boolean publication in Dust2 offline demos; it does not hook this function, invoke CMapInfo's initializer, add another rain system, edit skin materials, or mutate map entities. Current native values remain the restoration source; retained entity handles include their serial, and scene/world identity must match before restoration. Restore on master/contact disable, seek/rewind, level transition and shutdown. Signature/schema mismatch leaves native contact unchanged with a warning.

Optional weather schema is now cached in MirvWeather_ResolveSchemaOffsets before HookSchemaSystem clears its transient table. In particular, postprocessing no longer attempts a lookup against that empty table on the first weather frame; level resets retain cached offsets.

Release controls: mirv_weather contact 0|1 and mirv_pov_debug_feature weather_contact 0|1. Contact defaults on under the weather master (master defaults off), is independent of mirv_pov and the other weather debug effects, and updates on the next render pass. Turning contact off leaves airborne rain, ground and postprocessing controls intact. Pending in-game acceptance: off/on, re-enable, interaction with POV off/on, indoor/outdoor weapons/knives/gloves, seeking, cold start, recording and restoration. Static call-site uniqueness, current image identity and schema lifetime flow were checked; Windows Release x64 build is required before delivery. No game visual claim is implied by compilation.


## CS2 1.41.8.9: discover client-only weather objects and accept authored material names

Build/source revision: 1.41.8.9 / 11087167. Current client SHA256: 7081D87FD49961A5D82E45ABD0F0B8F8F05DEEB3CB849837C1759716221C6977. Preferred base 0x180000000. Native getter RVA 0x9F6B60 bounds indices at 0x7FFE and addresses 64 chunks of 512 identities, stride 0x70. It is an unhooked helper already bound by HLAE. The script GetHighestEntityIndex returns a fixed 2048; that is not the complete namespace.

Read-only live demo evidence: one info_map_parameters at index 16878, and six post_processing_volume objects at indices 490, 540, 16583, 16766, 16901 and 16902. Dynamic RTTI vtables matched the current image: CMapInfo RVA 0x1CA87F8; C_PostProcessingVolume RVA 0x1AE9670. Their network-class getters return C_BaseEntity and C_BaseModelEntity respectively, so comparisons with derived names never matched. No game functions were called and no process memory was written during this investigation.

Current unique native rain-contact call-site RVA 0xD2F90A (setter call 0xD2F943), scene/world interface globals RVA 0x2799E70 / 0x23BA0E8, world helper RVA 0x3B9A40, scene vtable +0x1B8. The layout remains entity identity +0x10, worldGroupId +0x38, trace-to-sky +0x619. These addresses replace earlier-image analysis addresses, not the runtime signature discovery.

All 18 delivered VMAT DATA blocks retain m_materialName equal to the original manifest material path although their loose resource filenames are materials/mirv_weather/dust2/wet_NN.vmat. The strict requested-filename-only check rejected those authored aliases. The corrected loader accepts only the requested name or that exact entry's authored original name; default/error and unrelated aliases still fail. Failure diagnostics now print the resolved name.

Implementation: weather-only discovery scans the complete valid namespace once per second and caches serial-bearing handles; seek/level reset invalidates the cache. Exact dynamic RTTI identifies map info and post volumes; the global script helper and unrelated POV entity behavior are unchanged. Existing weather controls remain independent, immediate in Release, master off by default, scoped to Dust2 demos. No extra airborne rain source or new resource layer is introduced. Status adds discovered map/post counts, RTTI resolution and ground requested/debug/ready flags.

Validation: current disk hash, native signatures, RTTI and read-only live object identities verified; all authored material names decoded and checked; source whitespace and discovery/name-validation invariants checked. Windows Release x64 build required before delivery. User's previous runtime report confirmed only airborne rain; corrected visuals, off/on, seeking and recording acceptance remain pending. Restore preserves original map values and uses entity serial plus world identity.


## CS2 1.41.8.9: complete native scene-record tail in wet-ground draws

User visual evidence: wet ground/puddles appeared before a crash within about half a second of enabling weather. Disabling only ground stopped the crash while the other weather paths remained enabled. Two local crash records reported execute access violations at address zero, returning to scenesystem RVA 0x7AC4C and 0x42604 after material virtual slot +0x28; the first record referenced a stack scene record. Raw dumps, register/heap contents and user paths are not published.

Analyzed scenesystem.dll SHA256: CCA4AD4F2A4FAF4768B2BA5E6C50D2A9DDF72E8E94017D17228A10F9F94C750F. Image base 0x180000000. Native DrawSceneData RVA 0xA30B0 retains the submitted record as the current batch start at drawingData +0x18 (RVA 0xA313F). Flush RVA 0xA34C0 synchronously dispatches its current batch and clears the batch descriptor/start/count at RVAs 0xA3731, 0xA373C and 0xA3740. Before dispatch, the conditional capture path calls RVA 0xA3C50; that helper advances/resizes/copies records by 0x70 bytes (including imul count by 0x70 at 0xA3D61). These are existing native functions/helper calls, not new detours.

The override's local CBaseSceneData definition was only 0x68 bytes, so a copied override omitted the final eight bytes and native capture read past its end. Extend the opaque tail from 0x14 to 0x1C and assert sizeof=0x70. Existing sceneObject/material/color offsets stay unchanged. Continue to flush before and after each private override; preserve the full native record and leave the map-owned record unmodified. This corrects a demonstrated record-size violation; it is not proof that every possible ground crash is resolved.

Controls unchanged: weather master and independent weather_ground / mirv_weather ground. Default and scope unchanged. Rain, native contact, postprocessing, killfeed and POV logic are not removed to avoid the crash. No new material layer or airborne rain source is added. Source whitespace checked; Windows Release x64 and in-game ground-enabled, off/on, pause/seek and recording acceptance required before declaring stability.
