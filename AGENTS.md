# Project Agent Notes

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
