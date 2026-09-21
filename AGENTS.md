# Project Agent Notes

## Reverse-engineering records and independent effect controls

For every reverse-engineering investigation from now on, update
`doc/notes/cs2-pov-native-logic.md` in the same change. Summarize the game's
original behavior separately from the POV implementation: conditions, thresholds,
event order, native functions, and relevant state. Record the analyzed module's
SHA-256/build identity, image base, RVAs/analysis VAs, hook/call sites, signatures
or vtable slots, source locations, evidence, validation and remaining uncertainty.
Distinguish a hooked function from an unhooked helper used for native playback.
Do not carry addresses forward as verified when a DLL changes; keep previous
build records and add a new versioned entry after verification.

Every newly added user-visible effect must have its own user-accessible debug
on/off control, available in normal Release builds independently of diagnostic
logging. Register it with `mirv_pov_debug_feature <effect> <0|1>`; reuse this
shared feature registry instead of adding a separate debug command system.
Document the command, default, scope, when it takes effect, and state
cleanup. Disabling one effect must not disable unrelated effects. Verify both
off and on, re-enabling, and interaction with the main POV switch. Do not hide
these controls behind `AFX_MIRV_POV_DIAGNOSTICS` or rely solely on a coarse module
switch such as `feedback` or `hud`.

## Commit message policy

For commits that change `mirv_pov`, use a user-facing title that identifies the
primary behavior being fixed. Avoid generic titles such as “HUD cleanup” or
“misc fixes”.

The commit body must enumerate each user-visible fix included in the change. In
particular, when applicable, explicitly mention:

- flash and grenade deafening feedback;
- grenade-throw voice playback/fallback;
- hiding enemy health bars in the top HUD;
- red C4 rendering from the CT POV radar view.

Also mention hook-compatibility hardening or data-isolation changes when they
are part of the implementation, and record the validation performed (for
example, Release x64 build, cold-start CS2/HLAE test, or local demo playback).

Before committing, run `git diff --check` and review `git status`. Keep
`.agents/`, `.codex/`, `diagnostics/`, and `tests/` analysis artifacts out of a
commit unless the user explicitly asks for them.

If a pushed commit's note is incomplete, amend that commit and update the fork
with `git push --force-with-lease` rather than leaving an inaccurate message.
