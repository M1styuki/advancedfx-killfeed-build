# Seven-map command weather

## Profiles and resource provenance

Port the existing command-driven Dust2 weather controller to de_dust2, de_mirage, de_cache, de_inferno, de_ancient, de_nuke and de_anubis. Preserve killfeed/POV and the prior strong post-resource ownership repair. The master defaults off and remains independent of mirv_pov. User preferences survive a map transition; particles, material references, post backups, scene/environment state and profile data do not.

Data comes from DrEAmSs59/CS2-insight-agent commit f05c698c755dc7806855bb13a5c823dbf6a21de6. Decode each verified compiled entity payload and select only client-only path_particle_rope_clientside weather:rain_* hosts referencing rain_single_128 with max_simulation_time=0. Rain counts: Dust2 385, Mirage 832, Cache 969, Inferno 604, Ancient 166, Nuke 221, Anubis 1004. Cache positions independently match the refined de_cache_regions.json within 0.002 units. One shared particle resource is used; no second rain system is introduced.

Wet material counts are 18/12/3/2/0/0/0 in the order above. Retain each original map's authored textures with the reference wet shader payload and exact original-name alias. Ancient keeps native wet terrain; Nuke keeps the reference rain-only ground treatment; Anubis keeps authored water surfaces. The reference's additional static worldnode puddle meshes on some maps are not ported by this DLL controller. Map packages and authored geometry are not rewritten, sky remains under mirv_sky, and no lightning or thunder is added. These distinctions are visible in the provenance and user guide, rather than claiming visual equivalence to the whole reference map package.

delivery/seven-map-weather-resources.zip contains the independent data package with PolyForm Noncommercial 1.0.0 license, author notice and per-profile hashes. It contains no game files from the local machine. Software licenses remain unchanged. DATA payload and catalog sizes/hashes, material payload hashes, particle identity, counts, unique finite positions and Cache coordinates were validated before packaging.

## Current native environment publication

Analyzed client.dll: CS2 1.41.8.9 / SourceRevision 11087167, SHA256 7081D87FD49961A5D82E45ABD0F0B8F8F05DEEB3CB849837C1759716221C6977. Preferred image base 0x180000000. Existing native CMapInfo initialization publishes its boolean contact at RVA 0xD2F943 through scene slot +0x1B8. Adjacent five-float publication starts at RVA 0xD2F945 and calls scene slot +0x1C8 at RVA 0xD2F9B9; unhooked world-group resolver is RVA 0x3B9A40. Scene/world globals resolve to RVA 0x2799E70/0x23BA0E8. No additional detour is installed.

Verified argument order: scene RCX, world RDX, rain strength XMM2, ripple strength XMM3, ripple direction stack+0x20, wetness coverage stack+0x28 and drying amount stack+0x30. Native loads offsets 0x61C/0x620/0x624/0x628/0x62C. The unique full load sequence at RVA 0xD2F950 and cached schema fields must both agree before the helper is used. Schema is resolved before HLAE discards its temporary table. Earlier module addresses are not reused as current evidence.

The verified reference map-info profiles publish [1,1,0,1,0]. This completes the native rain environment path needed where authored Ancient rain strength is weaker; a boolean sky-contact toggle alone does not set rain amount. Publish to the scene without changing entity fields. Serial-bearing handles and matching scene/world identity are tracked; restoration reads current native entity values. Disable, reset, rewind/jump, map change and shutdown restore owned environment state. Missing signatures/schema leave native environment unchanged with a warning.

Independent Release control: mirv_pov_debug_feature weather_environment 0|1, default on under the weather master, immediate on the next render pass and independent of mirv_pov/contact/rain/ground/postprocessing. Existing weather controls remain unchanged. Off/on and interaction require in-game acceptance. Compiled-map source data uses newer KV3 node types in current retail files, so those retail lumps were not claimed fully decoded; only the reference payloads were decoded successfully. No map-file writes or raw process data publication occurred.

## Validation and limits

Validate exact seven profile headers/counts and resource hashes, unique current native signature/ABI and schema layout, balanced post-resource ownership, map reset/config reload flow, source diff and whitespace, then Windows Release x64 cloud build. Inspect generated binary/source/resource identity before installation. Normal full Dust2 weather was accepted in the earlier DLL, but repeated seek acceptance and visuals/performance of the other six maps remain pending. Compiler success is not in-game visual acceptance. Older diagnostic history can contain private workstation paths; this public note carries the sanitized new evidence and is included with corresponding source.
