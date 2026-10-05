# CS2 1.41.8.8: independent killfeed and POV flash integration

Base: WangChuDi/advancedfx commit e0e5e0830587765a02b662a873da68ae08fc2c23. The analyzed CS2 client.dll has SHA-256 d7db25d48f1d10c5e0b0296e20ed803426eb9509da41760daeda39dd35ba89b9 and preferred image base 0x180000000. The addresses below are RVAs.

Native behavior: CCSGO_HudDeathNotice dispatch at 0xE7E340 recognizes player_death and routes it to the killfeed handler at 0xE843E0. CCSGO_HudDeathPanel has a distinct listener at 0xE7E4A0. The lifetime and local-player helper signatures still each match once; lifetime offsets resolve to 0x78 and 0x7C. The two flash renderer call sites are 0x11C856B and 0x11DD3E5 and retain the upstream shared-predicate check.

Integration: preserve the current upstream mirv_pov flash and DeathPanel/MVP behavior. Hook HudDeathNotice independently for mirv_deathmsg filter, block, lifetime, local-player and name overrides. Keep the temporary event, controller-name and lifetime changes scoped to the synchronous killfeed call. Keep the DeathPanel listener separate so its native POV logic does not consume killfeed-only overrides. No new visual effect or debug toggle is added.

Static checks: the killfeed-handler, DeathPanel-listener, local-player helper, lifetime-offset, and both flash-context signatures each match once in the analyzed client. The killfeed handler was also traced from the native player_death dispatch. The source merge retained upstream Panorama and MVP music changes. Release x64 compilation and in-game validation remain to be recorded. The previous user test confirmed killfeed block/lifetime on an earlier CS2 client; this current build requires the same two commands and full-white POV flash to be retested in the user's demo.
