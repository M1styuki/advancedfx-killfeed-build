# Seven-map storm behavior and validation

The controller extends the existing Dust2, Mirage, Cache, Inferno, Ancient, Nuke and Anubis weather profiles. Existing rain, wet-ground and contact data are unchanged. It preserves the selected mirv_sky material and adds adjustable sunlight reduction, sun-card suppression, scene exposure, grading, native lightning particles, transient illumination, rain audio and thunder.

## Lightning semantics

Kill-triggered lightning requires at least one mirv_deathmsg filter rule. With no filters, no kill triggers lightning, even if deathmsg debug, lifetime or highlighting is configured. With configured filters, only the final nonblocked real, non-self kill triggers. The notification belongs exclusively to the right-top killfeed handler after the complete filter and block decision; the POV DeathPanel does not notify weather. The xTrace rule resolves its player when the rule is added. No camera re-trace is introduced.

Demo-time lightning is independent of the filter list. The demo/kills/both/off modes select event sources. Pausing, disabling, changing maps and seeking clear owned transient particles, delayed thunder and pending kill events. Multiple accepted kills in one render-frame batch share a single bounded flash/thunder burst; the accepted-kill counter records each kill.

## Rendering and audio ownership

Grading uses a private deferred D3D11 context and executes with context-state restoration. It preserves predication, avoids duplicate processing of the same target in one presented frame, and excludes the entire POV death-fade window and active player flash. HUD is drawn later. BeforeUi stream capture and ReShade input include the grade, so additional ReShade grading can compound the appearance. Owned draws bypass HLAE's color/depth blockers and must be included in recording acceptance.

Native scalar overrides are restored only for matching entity identities and values still owned by this controller. Audio playback tracks only its own instances and never issues global stopsound. Rain is a stereo ambient loop, without roof-aware indoor/outdoor routing. Turning both audio effects off stops the instances before releasing their definition references; reload or map reset allows a failed definition lookup to retry. Particle, illumination and thunder switches remain independent.

The seven new effects each have an immediate Release mirv_pov_debug_feature control. Weather does not require mirv_pov. Shared sound-hook initialization does not enable the independently gated POV soundcircle behavior. The guide lists commands and defaults.

## Validation boundary

Independent review identified rendering-state, transient-cleanup and death-fade defects; the fix round isolates drawing, adds synchronous cleanup, and guards the fade transition. The user's explicit no-filter/no-kill-lightning requirement is authoritative. Local static review and whitespace checks are followed by cloud Windows Release x64 and exact embedded HLSL VS/PS compilation. Downloaded DLL/source correspondence and installed-file checks are required before delivery.

Compilation, review and counters do not establish visible or audible success. In-game seven-map indoor/outdoor lighting, lightning, actual audio, filtered-kill behavior, repeated seeks, pause/restore, full-white flash, death fades, HUD and recording remain acceptance items. Existing baked lightmaps/reflections are not rebuilt. Detailed native investigation records remain local rather than being included in the public diagnostic corpus.
