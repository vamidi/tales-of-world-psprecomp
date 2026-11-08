# PSP Project State — tow-3

> Auto-maintained. Read at session start, update after every major action.
> This is the canonical state for `profiles/tow-3`.

## Repository

- PSPRecomp path: `D:\Personal\C++\tales-of-world-psprecomp`
- Git branch / commit: `master` / `a068b45`
- OS / compiler / generator: Windows / MSVC 14.51 / Ninja
- Build directory: not configured yet (`out/tow-3` absent)

## Target

- Title / region: Tales of the World: Radiant Mythology 3 (Japan)
- Product code: `NPJH50353`
- Profile: `tow-3`
- Executable: `profiles/tow-3/game/NPJH50353_EBOOT.BIN`
- ELF type / load base: `ET_EXEC` / `0x08804000`
- SHA-256: `59ada394a8e25894a1d44c1a62236c715e9f5ee4bea02513fc4f9e58ea94fb2a`
- Entry: `0x08804128`
- Module / gp: `EXP_ALFASYSTEM` / `0x08DA2070`
- Import stubs: `0x08B204EC`–`0x08B21538`
- Current phase: 2 — corpus generation / profile bring-up.

## Analysis and corpus

- Report: `profiles/tow-3/analysis/tow3_report.json`
- Imports: 391
- Function seeds: 18,505
- Truncated functions: 153
- Relocations: 0 total; unsupported 0; invalid 0
- Generated: only `generated/generated_units.hpp`; no translation units yet
- Profile: no `CMakeLists.txt`, no `host/`, no runnable executable yet

## Build and run

After generating the corpus and adding the profile host:

```bat
cmake -S . -B out/tow-3 -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DPSPRECOMP_PROFILE=tow-3 ^
  -DPSPRECOMP_MSVC_MP_JOBS=8 ^
  -DPSPRECOMP_GENERATED_OPT_LEVEL=2 ^
  -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
cmake --build out/tow-3
```

Generator command:

```bat
out\framework\psp_recomp.exe profiles/tow-3/game/NPJH50353_EBOOT.BIN ^
  --auto profiles/tow-3/generated 0x08804000 0x2000
```

Last verified result: analysis only; no build or runtime yet.

## Current diagnosis

- Observation: `psp_analyze` succeeds; the corpus has only the registry
  header.
- Layer: profile/bootstrap and corpus generation.
- Hypothesis: this profile is still at the Phase 2/3 boundary; it has not yet
  received the tow-1 host/HLE template.
- Prediction if true: generating the corpus and adding `CMakeLists.txt` plus
  `host/` should reach first dispatch.
- Next: generate units, copy the minimal host layout from tow-1, then reach
  first dispatch before feature work.

## HLE surface

None yet in this profile. The intended first pass is the tow-1 host/HLE
template, trimmed to the libraries in `tow3_report_imports.csv`.

## Changes and verification

- Input identity and analysis artifacts exist.
- `relocations.unsupported` / `.invalid` are both 0.
- No profile build, tests, or runtime verification has been performed.

## Learned patterns

- Do not carry tow-1 addresses into tow-3; the executable is a different
  revision.
- The 153 truncated functions are boundary data to explain, not a decoder
  problem by default.
- The `tow-3` profile cannot configure until `CMakeLists.txt` exists.
