# Recompiling Tales of the World: Radiant Mythology (`tow-1`)

> **Audience:** a beginner who has never used a static recompiler and has
> little or no MIPS/Allegrex assembly experience.
>
> **Goal:** after working through this guide, you can build the `tow-1`
> profile, run it, capture frames, classify a failure, make one small HLE
> fix, and continue the port one verified milestone at a time.
>
> **Not the canonical status file:** current verified state is in
> [`PSP_PROJECT_STATE.md`](PSP_PROJECT_STATE.md). This file explains the
> *method* and the *toolchain*.
>
> **Provisional time estimate:** roughly 4–8 focused hours for a first pass
> through the labs and one small HLE fix, depending on your existing C++
> experience. This is an estimate, not a measured pilot result; reaching a
> playable ending is a separate, much larger effort.

---

## 1. What “static recompilation” means here

A normal emulator decodes and executes PSP instructions while the game runs.
A **static recompiler** reads the game executable ahead of time and writes
ordinary C++ functions that do the same work.

The simplified pipeline is:

```text
decrypted PSP executable
        |
        v
  psp_analyze           find functions, imports, relocations
        |
        v
  psp_recomp             emit generated C++ translation units
        |
        v
  profile host           scheduler, memory, HLE, renderer, audio, input
        |
        v
  Tow1Native.exe         runs the recompiled game on Windows
```

Three words you will see constantly:

| Term | Meaning in this project |
|---|---|
| **AOT** | Ahead-of-time generated C++ for guest functions |
| **HLE** | High-level emulation: a host C++ replacement for a PSP library call |
| **NID** | The 32-bit identifier a PSP executable uses to import a library function |

The game's own code is already recompiled. Most porting work is making the
**runtime contract** around that code accurate enough.

---

## 2. The three layers

Always identify which layer is failing before editing code.

| Layer | Location | Examples |
|---|---|---|
| Framework | `include/psprecomp/`, `src/`, `tools/` | decoder, ELF loader, scheduler primitives, generated-function dispatch |
| Profile host | `profiles/tow-1/host/` | HLE functions, input, GE interpreter, MPEG/Atrac, window |
| Generated game code | `profiles/tow-1/generated/` | recompiled game functions; **do not edit by hand** |

The generated corpus is output. If a generated function is wrong, fix the
generator, the analysis seed, or the runtime, then regenerate.

---

## 3. Prerequisites

You need:

- Windows 10/11 x64
- Visual Studio with the C++ workload
- CMake 3.20 or newer
- Ninja
- Python 3 (optional, useful for log/search scripts)
- FFmpeg shared build with `include/`, `lib/`, and `bin/`
- Your own legally obtained game files
- Optional but strongly recommended: PPSSPP, used only as an oracle
- Optional: Ghidra, for inspecting a function or import call site

The profile build also needs the Win32 GDI libraries for host font rendering.

### Important data rules

- The decrypted executable and extracted game data belong under
  `profiles/tow-1/game/` and are git-ignored.
- Do not commit the game, ISO, executable, extracted assets, or generated
  analysis dumps.
- `profiles/tow-1/generated/` is the checked-in recompiled corpus.
- `profiles/tow-1/analysis/` and `profiles/tow-1/progress/` are local
  working areas.

### Cleanup

Frame dumps, `.png` conversions, and temporary logs can be deleted after a
session. Do **not** delete `game/`, `generated/`, or the profile source as a
cleanup step. A clean rebuild of the generated corpus is a deliberate,
expensive action.

The optional PPSSPP oracle is a separate emulator. It is useful for comparing
expected behavior; it is not part of the generated profile build.

---

## 4. Build the framework and profile

Run from the repository root.

### Framework tools

```bat
cmake -S . -B out/framework -G Ninja -DPSPRECOMP_PROFILE=""
cmake --build out/framework
ctest --test-dir out/framework -C Release --output-on-failure
```

Expected tools:

```text
out/framework/psp_analyze.exe
out/framework/psp_recomp.exe
out/framework/dump_function.exe
```

### The tow-1 profile

```bat
cmd /c profiles\tow-1\scripts\build.bat
```

The script currently hardcodes this Visual Studio path:

```text
C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat
```

If your Visual Studio is installed elsewhere, edit `VSDEV` in
`profiles/tow-1/scripts/build.bat`. Do not assume the path is universal.

Manual equivalent:

```bat
cmake -S . -B out/tow-1 -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DPSPRECOMP_PROFILE=tow-1 ^
  -DPSPRECOMP_MSVC_MP_JOBS=8 ^
  -DPSPRECOMP_GENERATED_OPT_LEVEL=2 ^
  -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
cmake --build out/tow-1 --target Tow1Native
```

Expected final line:

```text
BUILD OK: ...\out\tow-1\bin\Tow1Native.exe
```

### FFmpeg

The profile auto-detects the WinGet `BtbN.FFmpeg` shared build. If yours is
elsewhere:

```powershell
$env:FFMPEG_ROOT = 'C:\path\to\ffmpeg'
cmd /c profiles\tow-1\scripts\build.bat
```

The DLLs must be available beside `Tow1Native.exe`; the build normally copies
them.

---

## 5. First run, and what a bounded run means

Run the game for a fixed number of guest dispatches:

```bat
out\tow-1\bin\Tow1Native.exe ^
  profiles\tow-1\game\eboot.dec ^
  profiles\tow-1\game\root ^
  1000000
```

The last number is the **dispatch cap**, not seconds and not frames. It is a
safety limit so a broken run stops instead of spinning forever.

For a visible window:

```bat
profiles\tow-1\scripts\play.bat 1000000
```

For headless work:

```powershell
$env:PSPRECOMP_TOW1_NO_WINDOW = '1'
$env:PSPRECOMP_TOW1_FRAME_DUMP = 'frame.ppm'
$env:PSPRECOMP_TOW1_FRAME_DUMP_AT = '3423'
$env:PSPRECOMP_TOW1_EXIT_AFTER_FRAME_DUMP = '1'
```

The `.ppm` file is a simple image format. Convert it with:

```bat
ffmpeg -y -i frame.ppm frame.png
```

### Start with a known milestone

For this profile:

```text
title frame:              ~3423
title menu:                5000
character creation:        15000
first field:               6000 presents after OSK
first soldier battle:      ~13175 presents after OSK
```

These numbers are **build- and input-dependent**. Always record the exact
environment and input sequence with a capture.

---

## 6. Analyze the executable

The profile has already been analyzed, but the repeatable command is:

```bat
out\framework\psp_analyze.exe ^
  profiles\tow-1\game\eboot.dec ^
  profiles\tow-1\analysis\tow1_report.json
```

Open `tow1_report.json` and check:

| Field | Expected for this profile |
|---|---|
| `sha256` | `105280ce...e5b89d` |
| `elf_type` | `65440` (`0xFFA0`, PSP PRX) |
| `load_base` | `0x08804000` |
| `relocations.unsupported` | `0` |
| `relocations.invalid` | `0` |
| `import_count` | `252` |
| `function_seed_count` | `48,179` |
| `truncated_functions` | `300` |

**Hard stop:** if relocations are unsupported or invalid, do not debug game
behavior yet. The loaded image is wrong.

### Generate the corpus

Only regenerate when seeds or the analyzer/generator changed:

```bat
set PSPRECOMP_EXTRA_FUNCTIONS=0x08805FDC
out\framework\psp_recomp.exe profiles\tow-1\game\eboot.dec ^
  --auto profiles\tow-1\generated 0x08804000 0x2000
```

The corpus is large: about 654 translation units and 122 MB of C++ for this
profile. Do not treat a clean rebuild as a casual step.

---

## 7. A two-minute assembly survival guide

You do **not** need to become a MIPS programmer to port this game. You mostly
need to recognize a few patterns.

### Registers

Allegrex is a 32-bit MIPS-family CPU. In the PSP calling convention:

| Register | Common meaning |
|---|---|
| `$a0`–`$a3` | first four function arguments |
| `$v0` | return value |
| `$sp` | stack pointer |
| `$ra` | return address |
| `$pc` | program counter |
| `$gp` | global/small-data base |

In the host HLE API these appear as:

```cpp
ctx.gpr[4]   // a0
ctx.gpr[5]   // a1
ctx.gpr[6]   // a2
ctx.gpr[7]   // a3
ctx.gpr[2]   // v0 return value
ctx.gpr[29]  // sp
ctx.gpr[31]  // ra
```

### Instructions you will see constantly

```asm
addiu sp, sp, -0x20     ; make a stack frame
sw    ra, 0x1c(sp)      ; save return address
li    a0, 0x4000        ; first argument = 0x4000
lw    a0, 0x10(s0)      ; load a field from a structure
jal   0x08B7BCF0        ; call a function/import
nop                     ; delay-slot filler
jr    ra                ; return
```

A `jal` call is not always a call to game code. The target may be an
**import stub**. The runtime resolves that import to a host HLE function.

### Why import stubs matter

A stub is a bridge:

```text
guest code calls stub
        |
        v
Runtime::invoke_import(library, NID, context)
        |
        v
profile HLE C++ function
```

If a NID is not registered, the runtime stops with:

```text
Missing HLE import <library>::<NID>
```

That is a **binding** problem. Do not invent a successful return value just
to silence it.

### What “a0 = 7” meant in this port

At one point the movie cleanup called:

```asm
jal sceKernelDeleteThread
li  a0, 7
```

`a0 = 7` is the **thread UID argument**. The old HLE ignored `a0` and deleted
the *currently running* thread, which was `user_main`. The correct
implementation had to delete the requested thread and wake its waiters.

This is why reading the argument matters even if the call “looks like a
shutdown.”

---

## 8. The debugging loop

Use one loop for every blocker.

### Step 1 — reproduce

Record:

- exact command,
- dispatch cap,
- environment variables,
- input sequence,
- first bad frame or first stop message,
- whether PPSSPP behaves differently.

### Step 2 — classify the layer

| Symptom | Likely layer | First action |
|---|---|---|
| `Missing HLE import` | HLE/binding | implement or correct that NID |
| `Missing AOT function` | seeds/generator | check address, seed it, regenerate |
| Same PC forever | scheduler/timing | identify the wait and its producer |
| Wrong values after an HLE | HLE contract | log args, returns, output writes |
| Runs but black frame | GE/presenter | count lists, inspect framebuffer |
| Assert/crash in guest memory | memory/contract | inspect address, structure, alignment |
| Asset missing | I/O/archive | inspect open/read path and sector semantics |

### Step 3 — make a falsifiable hypothesis

Example:

> The OSK status loop is waiting because it never sees `QUIT (3)`. If true,
> returning `VISIBLE (2)` from init, then `QUIT (3)` from update, will produce
> a shutdown call and a `NONE (0)` status.

Then log exactly those values.

### Step 4 — change one layer, one thing

Do not “fix” three things in one run. You will not know which one mattered.

### Step 5 — verify at the lowest layer first

```text
compile -> focused test -> profile run -> game milestone -> PPSSPP comparison
```

---

## 9. What we actually did for the current milestone

This is the worked example. It is also a template for the rest of the game.

### 9.1 Establish a known-good frame

The first task was not “implement everything.” It was:

1. Build the profile.
2. Run with a frame dump.
3. Confirm the title screen.
4. Add deterministic input.
5. Confirm the title menu.

The menu capture used Start input and a bounded dispatch run. A frame number
alone is not evidence; the image is the evidence.

### 9.2 Diagnose the OSK loop

Character creation reached name entry. The OSK HLE was called, but the game
kept calling:

```text
sceUtilityOskGetStatus -> 4
sceUtilityOskGetStatus -> 4
sceUtilityOskGetStatus -> 4
```

The old HLE returned `FINISHED (4)` immediately from init. The game's loop
does not advance from that state.

The fix was to model the actual dialog sequence:

```text
InitStart       -> VISIBLE (2)
GetStatus       -> 2
Update          -> QUIT (3)
GetStatus       -> 3
ShutdownStart   -> NONE (0)
GetStatus       -> 0
```

This is a **contract** fix, not a “return success” fix. The state sequence is
what made the game continue.

Relevant diagnostics:

```powershell
$env:PSPRECOMP_TOW1_OSK_DIAG = '1'
```

### 9.3 Diagnose the GE list lifecycle

With GE enabled, the port eventually printed:

```text
[tow1-ge] list 0x48E0F740 exceeded the command limit
```

The useful trace was:

```text
list=0x48E0F740 stall=0x48E0F740
stall update -> 0x48E0F78C
stall update -> 0x48E0F794
```

The root list has a `FINISH` at `+0x4C`. The sublists (`0x084A0000`,
`0x084A02F0`) are valid command buffers, not empty memory. The list is
submitted while the guest is still filling or reusing its command buffer.

The fix was to stop executing a stall-bearing list immediately:

- `sceGeListEnqueue` returns a real list id and queues lists with a non-zero
  stall.
- `sceGeListUpdateStallAddr` updates the queued stall.
- `sceGeListSync` and `sceGeDrawSync` execute queued work.
- `sceDisplaySetFrameBuf` flushes any remaining queue before presenting.

After this, 20M dispatches produced **zero** command-limit warnings, and the
title/boot logo still rendered.

Diagnostics:

```powershell
$env:PSPRECOMP_TOW1_GE_STALL_DIAG = '1'
$env:PSPRECOMP_TOW1_GE_SUBLIST_TRACE = '1'
$env:PSPRECOMP_TOW1_GE_POST_OSK_DIAG = '1'
```

### 9.4 Make input deterministic

Manual key presses are not reproducible. The profile supports scheduled input
after OSK:

```powershell
$env:PSPRECOMP_TOW1_POST_OSK_INPUT_SEQUENCE = `
  '6000:10:0x0200,6200:1500:0x0040,7800:1500:0x0020,' + `
  '9400:1500:0x0010,11000:1500:0x0080,12600:3000:0x0040'
```

Format:

```text
delay_frames:hold_frames:button_mask,...
```

Button masks are PSP pad bits:

```text
Up      0x0010
Right   0x0020
Down    0x0040
Left    0x0080
Start   0x0008
Select  0x0001
Cross   0x4000
Circle  0x2000
Triangle 0x1000
Square  0x8000
L       0x0100
R       0x0200
```

The route that reached the first soldier was:

```text
Down -> Right -> Up -> Left -> Down
```

### 9.5 Capture the exact battle frame

A single target frame can miss a short event. The profile supports a range:

```powershell
$env:PSPRECOMP_TOW1_POST_OSK_FRAME = 'battle_frame.ppm'
$env:PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_START = '13174'
$env:PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_END = '13182'
$env:PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_STEP = '1'
$env:PSPRECOMP_TOW1_POST_OSK_EXIT = '1'
```

This produced frames `battle_frame.ppm.13174.ppm` through
`battle_frame.ppm.13182.ppm`; frame `13175` was the first soldier battle.

### 9.6 Capture the traces

```powershell
$env:PSPRECOMP_HLE_HISTOGRAM = '1'
$env:PSPRECOMP_TOW1_GE_POST_OSK_DIAG = '1'
```

The battle run reported:

```text
unique=157 imports
calls=5,512,735
```

with no missing HLE import and no GE command-limit warning. The GE queue
contained `0x490964C0` and `0x48E0F740`; both were flushed by `draw-sync`
before presentation.

---

## 10. Step-by-step plan to finish the recompilation

Do these in order. Do not skip a gate.

### Phase 0 — environment and identity

1. Build the framework; run its tests.
2. Build `tow-1`.
3. Verify the executable SHA-256.
4. Read `profiles/tow-1/PSP_PROJECT_STATE.md`.
5. Confirm relocations are clean.

**Exit:** you can build, run, and capture a frame reproducibly.

### Phase 1 — corpus

1. Run `psp_analyze`.
2. Check import count, seeds, and truncated functions.
3. Regenerate only when seeds or generator logic changed.
4. Keep the generated corpus out of hand-editing.

**Exit:** the profile links and dispatches generated game code.

### Phase 2 — imports and HLE

1. Make a list of imported libraries and NIDs.
2. For each one used by the current milestone, implement the **contract**:
   arguments, return codes, output writes, blocking behavior.
3. Use `PSPRECOMP_HLE_HISTOGRAM=1`.
4. Treat a missing HLE import as a binding task, not a reason to return zero.
5. Replace temporary “triage” stubs before calling a milestone done.

Priority for this game:

```text
I/O/UMD -> scheduler/memory -> display/GE -> MPEG/Atrac/SAS
        -> font/utility/input -> save/network-edge cases
```

**Exit:** the current milestone runs with no missing import and no known
contract shortcut.

### Phase 3 — first frame / presenter

1. Verify `sceDisplaySetFrameBuf`.
2. Verify GE list counts.
3. Verify framebuffer address/stride/format.
4. Capture a frame and compare it with PPSSPP.

**Exit:** a recognizable frame from a cold start.

### Phase 4 — media

1. H.264 via FFmpeg for PMF video.
2. Atrac3+ PCM for movie audio.
3. SAS for voice/effects; decide explicitly whether silent SAS is acceptable
   for the current milestone.
4. Keep media behavior behind clear HLE contracts.

**Exit:** opening movie/title and character-creation media do not block logic.

### Phase 5 — gameplay milestones

For each milestone, record a frame number, input sequence, HLE histogram, and
GE queue trace:

```text
field movement
camera
field menu
NPC dialogue
first soldier battle
battle menu
victory/defeat
post-battle dialogue
town/world map
save/load
second battle
```

**Exit:** the game loop survives a long run without a missing binding, missing
AOT function, or unexplained wait.

### Phase 6 — save/audio/edge features

1. Real savedata read/write, not just a “dialog finished” stub.
2. Real SAS output if audio matters for the milestone.
3. Real Atrac3plus BGM/voice if required.
4. Network/ad-hoc can stay deferred for single-player.
5. Japanese text only if the target release needs it; the English release
   currently uses the host Latin font.

### Phase 7 — long-run verification

1. Cold start.
2. Fixed input route.
3. Frame captures at every milestone.
4. PPSSPP A/B at the first divergence.
5. Record exact environment.
6. Re-run framework tests and profile build.

---

## 11. Guided practice

### Lab 1 — prove your build

**Do:** run the build script and the framework tests.

**Checkpoint:**

```text
BUILD OK
psprecomp_tests ... Passed
```

### Lab 2 — capture the field

**Do:** run with `PSPRECOMP_TOW1_POST_OSK_FRAME` and a delay of `6000`.

**Checkpoint:** the image contains the character and compass HUD, not a
dialogue box.

### Lab 3 — read one stop message

**Do:** run with `PSPRECOMP_HLE_HISTOGRAM=1` and a low dispatch cap.

**Checkpoint:** you can identify the highest-count HLE call and state whether
it is a wait, a render loop, or a real workload.

### Lab 4 — change one HLE value

**Do:** pick a non-critical diagnostic HLE (for example a volume query),
log its arguments and return value, and rebuild.

**Checkpoint:** the log shows the call, the profile still builds, and the
title/field frame is unchanged.

Do not start with `sceKernelDeleteThread`, `sceGeListEnqueue`, or MPEG.

### Lab 5 — capture the first battle

**Do:** reproduce the route:

```text
Down -> Right -> Up -> Left -> Down
```

and use a frame range around `13175`.

**Checkpoint:** `battle_first.png` shows the battle HUD and soldier.

---

## 12. Independent challenge

**Challenge:** capture the first **post-battle** Chester/Kanonno dialogue and
identify whether any new HLE import or GE list appears after the battle.

Requirements:

1. A deterministic input sequence.
2. A frame capture after the battle.
3. An HLE histogram.
4. A GE queue trace.
5. A one-paragraph diagnosis in `PSP_PROJECT_STATE.md`.

Hints:

- The post-battle dialogue begins after the battle frame.
- Use a later post-OSK frame range rather than a single frame.
- Compare the HLE histogram with the battle run.
- Check for `Missing HLE import` and GE command-limit warnings.

Success criteria:

- The dialogue image is visibly correct.
- The run has no missing HLE import.
- The new state records the frame number and exact input sequence.

A solution outline is in section 14.

---

## 13. Troubleshooting

| Error / symptom | Likely cause | Recovery |
|---|---|---|
| `VsDevCmd.bat not found` | Visual Studio path differs | edit `VSDEV` in `profiles/tow-1/scripts/build.bat` |
| FFmpeg configure failure | headers/libs not found | set `FFMPEG_ROOT` to a shared build with `include/`, `lib/`, `bin/` |
| `Missing HLE import` | NID not registered, or wrong library/NID | log library/NID/pc/ra, verify against the import CSV, implement the contract |
| `Missing AOT function` | address was not seeded, or wrong function boundary | seed the address, regenerate, verify with `dump_function` |
| Same PC forever | a wait/semaphore/event with no producer | log the import and arguments; fix the producer, not the loop |
| Black frame | GE list not executed, framebuffer wrong, presenter disabled | count `sceGeListEnqueue`; check list/stall; check framebuffer format |
| Game stops after movie | thread/event cleanup wrong | check `sceKernelDeleteThread` UID, movie EOF result, sound thread |
| OSK loops on status | dialog state sequence wrong | expect VISIBLE -> QUIT -> shutdown -> NONE |
| GE command-limit warning | list executed while incomplete/reused, or stall ignored | queue stall-bearing lists; flush at sync/draw sync |
| Text missing | font glyph buffer/pixel format wrong | log glyph image format, x/y, buffer pointer; draw into caller buffer |
| Input ignored | input gated on wrong guest time, or window not focused | use scheduled post-OSK input; verify `sceCtrlReadBufferPositive` count |

---

## 14. Solution outline for the challenge

1. Keep the same route and extend the capture range beyond the battle.
2. Use `PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_START` after `13175`.
3. Run with `PSPRECOMP_HLE_HISTOGRAM=1` and
   `PSPRECOMP_TOW1_GE_POST_OSK_DIAG=1`.
4. Convert the PPM/PNG and confirm the dialogue portraits/text.
5. Compare the histogram with the battle run; a new high-count wait or a
   missing import is the next blocker.
6. Record the result in `PSP_PROJECT_STATE.md` with the exact frame range.

---

## 15. Validation evidence from this repository

These were actually run during the current port:

```text
Tow1Native: BUILD OK
psprecomp_tests: 1/1 passed
```

Verified captures:

```text
title/menu                 menu frame 5000
character creation         frame 15000
first field                post-OSK present 6000
field movement             field_right.png
camera                    field_camera2.png
field menu                field_menu.png
first soldier battle       battle_first.png (post-OSK present 13175)
```

The `tow-1` README's “Current status” table is older than the profile state;
the state file and the captures are the authority.

---

## 16. Limitations

- This is not a complete port; battle input/combat, post-battle flow, town,
  save/load, and final audio are still ahead.
- The current audio path is partly silent; SAS/Atrac behavior is not a
  finished audio implementation.
- The host font fallback covers Latin text; Japanese glyphs need a CJK font or
  real PGF support.
- Exact frame numbers and timings depend on the executable revision, build,
  scheduler settings, and input route.
- The tutorial is a method guide, not a guarantee that every game will behave
  identically.
- Generated C++ must never be patched by hand; fix the generator or runtime
  and regenerate.

---

## 17. Where to read next

Local sources:

- [`PSP_PROJECT_STATE.md`](PSP_PROJECT_STATE.md) — current verified state
- [`README.md`](README.md) — profile setup
- [`HANDOFF.md`](HANDOFF.md) — historical redirect
- `../../docs/ARCHITECTURE.md` — framework/profile boundary
- `../../docs/PROFILE_GUIDE.md` — profile conventions

Skill references (if installed under `.opencode/skills/psp-recomp/`):

- `resources/03-psprecomp-pipeline.md` — analyze → recompile → integrate → run
- `resources/04-hle-nid-imports.md` — NIDs and HLE contracts
- `resources/11-operational-phases.md` — phase gates
- `resources/19-stripped-game-walkthrough.md` — boot-to-draw loop
- `resources/06-game-porting-playbook.md` — failure families and ordering
- `resources/db-ge.md` — GE command streams

Primary upstream:

- <https://github.com/jessicanataliagta/PSPRecomp>

---

## 18. Glossary

| Term | Meaning |
|---|---|
| **AOT** | ahead-of-time generated C++ |
| **Allegrex** | the PSP’s MIPS-based CPU |
| **dispatch** | one guest function/block execution step |
| **EBOOT** | the PSP game executable |
| **GE** | the PSP graphics engine command stream |
| **HLE** | host replacement for a PSP library function |
| **NID** | import identifier derived from a function name |
| **PGF** | Sony’s PSP font format |
| **PPSSPP** | emulator used as a behavioral oracle |
| **PRX** | PSP relocatable module |
| **stall** | GE command-buffer address where execution may pause |
| **VFPU** | PSP vector floating-point unit |
