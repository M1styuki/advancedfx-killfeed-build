## Independent killfeed dispatch fix

Source comparison: WangChuDi/advancedfx 04e50c9ec98fcd67fca1ce3ee777636892e71c42 and advancedfx/advancedfx 4afc5d88b32fe82b39f90fe2066bd11782919d55.

Restore the upstream HudDeathNotice signature and void-return player_death handler independently of the existing pointer-return HudDeathPanel listener. Filter/block/lifetime/localPlayer belong to the killfeed; POV DeathPanel retains its native playback and remapping path. Temporary names, lifetime fields and active wrapper state are restored after synchronous handling. Separate Detours transactions isolate installation failures and report missing addresses.

Lifetime offsets were already resolved by Addresses_InitClientDll before HookDeathMsg; the removed getDeathMsgAddrs function did not initialize them. This patch restores the correct object/handler connection rather than duplicating offset initialization. No new effect or control is added.

Static source checks and git diff --check passed. Cloud compilation and runtime playback validation are pending. Class attribution and ABI are inherited from the pinned sources, not newly verified by disassembly. The event wrapper ABI, synchronous UI name consumption, and behavior across game updates remain runtime validation requirements. No module addresses are asserted as verified in this public note.
