# PSP Project State — vcs

> Auto-maintained. Read at session start, update after every major action.
> This is the canonical state for the working VCS reference profile.

## Repository

- PSPRecomp path: `D:\Personal\C++\tales-of-world-psprecomp`
- Git branch / commit: `master` / `bbc01a0`
- OS / compiler / generator: Windows / MSVC / Ninja or Visual Studio
- Build scripts: `profiles/vcs/scripts/build_fast.bat`,
  `profiles/vcs/scripts/build_release.bat`

## Target

- Title: GTA: Vice City Stories (PSP)
- Product code: `ULUS-10160`
- Profile: `vcs`
- Executable/data: local decrypted ELF and `PSP_GAME/USRDIR` under
  `profiles/vcs/game/` (git-ignored)
- Current phase: reference/working profile; not the active porting target.

## Analysis and corpus

- Generated AOT corpus: `profiles/vcs/generated/` (234 translation units)
- Generator: `vcs_recomp` (profile-specific, address-aware)
- Profile tests: `profiles/vcs/tests/`
- Third-party notices: `profiles/vcs/THIRD_PARTY.md`

## Build and run

```text
profiles\vcs\scripts\build_fast.bat
profiles\vcs\scripts\build_release.bat
profiles\vcs\scripts\play.bat [GAME_ROOT]
profiles\vcs\scripts\bench.bat
```

Resolution is configured in `profiles/vcs/config/VCSNative.ini`.
For the Ninja workflow, see `docs/VCS_NINJA_BUILD.md`.

## Current diagnosis

- VCS is the upstream working profile and the reference for host/runtime
  patterns used by the Tales profiles.
- No VCS-specific failure is being investigated in this session.
- Do not carry VCS game addresses into another profile; only reuse generic
  host/renderer patterns.

## HLE surface

See `profiles/vcs/host/` for the current HLE, renderer, audio, input and native
fast paths. This state intentionally does not duplicate that inventory.

## Changes and verification

- The profile builds and runs per its own README/scripts.
- This state is a conversion of profile documentation, not a new runtime
  verification run.

## Learned patterns

- Keep generated AOT out of whole-program LTCG in the release link; use
  per-unit optimization plus host/runtime LTCG.
- Reuse the host/renderer architecture, not title-specific addresses.
