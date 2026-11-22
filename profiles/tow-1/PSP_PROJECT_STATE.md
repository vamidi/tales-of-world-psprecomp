# PSP Project State — tow-1

> Auto-maintained. Read at session start, update after every major action.
> This is the canonical state for `profiles/tow-1`. A beginner-oriented
> recompilation walkthrough is in
> [`RECOMPILATION_TUTORIAL.md`](RECOMPILATION_TUTORIAL.md). The old
> `HANDOFF.md` and `analysis/gameplay_port_plan.md` are retained only as
> redirects.

## Repository

- PSPRecomp path: `D:\Personal\C++\tales-of-world-psprecomp`
- Git branch / commit: `master` / `bbc01a0`
- Working tree: tow-1 host changes for font, OSK, MsgDialog, Fpl, SAS end
  flag, Atrac3plus stubs, pad script, and diagnostics; docs conversion in
  progress.
- OS / compiler / generator: Windows / MSVC 14.51 / Ninja
- Build directories: `out/framework`, `out/tow-1`

## Target

- Title / region: Tales of the World: Radiant Mythology (Europe)
- Product code: `ULES00837`
- Profile: `tow-1`
- Executable: `profiles/tow-1/game/eboot.dec`
- ELF type / load base: `0xFFA0` (PSP PRX) / `0x08804000`
- SHA-256: `105280ceba06338bd6bfdd2142c7950a6cb683c1798f3668c9df8898a2e5b89d`
- Module / gp: `TOW_MAIN_ALFASYSTEM` / `0x08DBA950`
- Current phase: 7 — gameplay stability. Character creation, field movement,
  camera reset, the field menu, the first soldier battle, basic combat,
  battle item/Artes/Unison menus, post-battle dialogue, town NPC interaction,
  and the Ailily guild entrance/cutscene are verified. The guild quest
  counter/list is the next gate.

## Analysis and corpus

- Report: `profiles/tow-1/analysis/tow1_report.json`
- Relocations: 277,206; unsupported 0; invalid 0
- Imports: 252 across 27 libraries
- Function seeds: 48,179
- Truncated functions: 300 (concentrated around data-like `0x08C3...`)
- Generator: `psp_recomp --auto`; 654 units; 8,192-byte span; 122.5 MB
- Import wrappers: 252
- Extra seed known to be required: `0x08805FDC`
  (`PSPRECOMP_EXTRA_FUNCTIONS`)

## Build and run

```bat
cmd /c profiles\tow-1\scripts\build.bat
out\tow-1\bin\Tow1Native.exe profiles\tow-1\game\eboot.dec profiles\tow-1\game\root 1000000
```

Framework regression:

```bat
ctest --test-dir out\framework -C Release --output-on-failure
```

Last verified result:

- `Tow1Native` builds (`BUILD OK`).
- `psprecomp_tests` 1/1 passed after the current HLE/GE diagnostic batch.
- Menu frame 5000: NEW GAME / CONTINUE / OPTIONS.
- Notice/dialogue frame 10000: host-font text renders.
- Character-creation frame 15000: skin-color screen renders.
- Logic-only run (`PSPRECOMP_TOW1_GE_DISABLE=1`) reaches frame 15500 after
  the OSK, with no missing HLE import and a full HLE histogram.
- Character creation reaches the field:
  - 300 presents after OSK: Confirm screen.
  - 600 presents: transition cutscene.
  - 3000 presents: first field dialogue (Mermo / “Yacon”).
  - 6000 presents: controllable field with character and compass HUD.
- Field movement verified: scheduled Right input moves the character and
  changes the camera view (`field_right.png`).
- Camera input verified: `R + Left` rotates/resets the camera independently of
  character movement (`field_camera2.png`).
- Field menu verified: Triangle opens the Artes/control screen
  (`field_menu.png`), with HP/TP and arte list rendered.
- First soldier battle reached and captured:
  - `battle_first.ppm` / `battle_first.png` show the battle HUD, player
    character, soldier, and the combat field.
  - The encounter is triggered by the scripted field route
    `Down → Right → Up → Left → Down`.
  - Post-OSK range capture (`..._FRAME_RANGE_START/END/STEP`) isolated the
    battle frames.
  - Combat input/flow verified:
    - `battle_attack.png` — sword slash and hit effect, HP 215.
    - `battle_critical.png` — “Critical Damage”, HP 212.
    - Additional damage exchanges drop HP to 210 and 206.
    - `battle_results.png` — victory results: EXP 23, Gald 100, Wand
      obtained, time and grace fields.
  - Battle HLE histogram: 157 unique imports / 5.68M calls; no missing HLE
    import and no GE command-limit warning.
  - Battle GE trace: lists `0x490964C0` and `0x48E0F740`, each queued with
    `stall == list`, flushed by `draw-sync` before presentation.
- Battle menu/ability flow verified:
  - `battle_menu_items.png` — battle item/command menu.
  - `battle_menu_unison.png` — “Select artes used for Unison attack”.
  - `arte_12000.png` / `arte_12200.png` — battle attack/effect frames while
    the party is in combat.
  - Menu input uses Triangle; battle HUD remains live behind the menu.
- Post-battle scene captured:
  - `post_battle_chester.png` — Chester/Kanonno/Mormo dialogue after the
    victory.
  - `town_return.png` — Mormo/town view after the scene.
  - `town_interaction.png` — town NPC dialogue after the return
    (“M-my daughter is…”), confirming the field-to-town interaction path.
  - `town_move.png` / `town_guild.png` / `town_guild2.png` / `town_guild5.png`
    — town movement and NPC dialogue.
  - `town_guild7.png` / `guild_counter2.png` — the diagonal town route entered
    a **Weapon Shop**, not the guild; the shop owner dialogue was captured.
  - `town_left_building.png` — alternate left-side route (benches/stairs).
  - **Ailily guild reached.** From the plaza, hold `Up` to the red-carpet
    stairs, then `Up+Right` to enter the Ad Libitum guild:
    `22000:4000:0x0010, 26000:4000:0x0030`.
    - `town_climb.ppm.27000`-`32000` — guild interior cutscene with Raine,
      Kratos, Kanonno, and Mormo.
    - `guild_cutscene.ppm.27000`-`52000` — extended Raine/Kanonno dialogue.
    - After the first-visit cutscene the game transitions back to town/Inn;
      the quest-counter list is not available in this first visit. The story
      requires the Inn/plaza NPC tour and a guild return before the five
      starting quests appear.
  - **Ailily town menu reached.** `shop_stairs.ppm.51500.png` shows the
    world-map-style town menu over the Ailily illustration:
    `Guild`, `Inn`, `Tool shop`, `Accessories`, `Plaza`, `1st House`,
    `2nd House`, `Exit town`.
    The highlighted Guild description is
    **“Accept a new quest or change jobs.”**
  - The weapon-shop owner path is also verified: holding `Up` at the counter
    opens the **Weapon Shop Buy/Sell/Exchange/Cancel** menu
    (`inn_up.ppm.44500`-`50000`). This exercises the generated shop/menu AOT
    functions; it is not the guild counter.
  - **Not yet captured:** selecting Guild from the town menu and opening the
    quest list. The current R/Quit/stairs input sequence is frame-sensitive;
    the same route sometimes lands in the menu and sometimes reopens the shop
    menu. `guild_select.ppm.52000` / `guild_menu_auto.ppm.62000` show the
    post-selection Dalia room, but that room is still the shop/Inn building,
    not the verified guild counter. A stable selector is still needed.
- Earlier field-route attempts (`field_down*`, `field_left*`, `field_right*`,
  `field_south_camera.png`) are retained as route evidence.
- OSK status sequence is now correct:
  `init -> visible(2) -> update -> quit(3) -> shutdown -> NONE(0)`.
- A logic-only run with `PSPRECOMP_TOW1_GE_DISABLE=1` and destabilized
  presenter pacing reaches frame 15500 after OSK. This proves the CPU, HLE,
  input, and character-creation state machine progress; the remaining problem
  is the GE list processing/rendering of the post-OSK screen.
- The post-OSK HLE histogram shows the game still active in the sound and
  display loops: `sceKernelGetSystemTimeWide` 364k, `sceKernelSleepThread`
  364k, `sceSasGetEndFlag` 247k, `sceSasGetEnvelopeHeight` 265k,
  `sceDisplaySetFrameBuf` 15.5k.

## Port coverage (2026-11-18)

A single percentage would mix unrelated layers. Use this scorecard instead;
`scripts/port_status.ps1` prints it and checks that the evidence files exist.

| Layer | Status | Measurement |
|---|---|---|
| ELF load / relocations | **complete** | 277,206 relocations; 0 invalid, 0 unsupported |
| AOT corpus | **complete for this ELF** | 48,179 seeds; 654 units; 122.23 MB; 300 analyzer-truncated functions |
| HLE imports | **present for tested paths** | 252 imports across 27 libraries; 0 missing HLE across 61 diagnostic logs |
| CPU / VFPU / runtime | **working through tested scenes** | boot, title, character creation, field, battle, town, guild; no missing-AOT stop in the 61 logs |
| GE rasterizer | **working through tested scenes** | title/movie, field, battle, town, guild; 0 command-limit warnings in the 61 logs |
| Input / presenter | **working** | visible Win32 window plus scripted pad input; `tow1_visible_*.png` |
| Movie / Atrac3+ | **real for the opening movie** | FFmpeg H.264 + PMF Atrac3+ decode |
| SAS / BGM / voice | **stub** | silent `sceSasCore`; `sceAtrac3plus` game streams are silent |
| Save / load | **stub** | savedata dialog completes headlessly; no real file save/load |
| Network / ad-hoc | **deferred** | 31 imports across `sceNet*` / Adhoc libraries |
| Automated regression | **thin** | `psprecomp_tests` 1/1 passed; gameplay is verified by frame captures |
| Story/gameplay milestones | **12/13 verified** | prologue through the Ailily guild cutscene; guild quest counter pending |

Strict definition of "playable": the port is **not yet end-to-end playable**.
The complete main story, real audio, and real save/load are not verified.
The working slice is:

```text
boot -> title -> character creation -> field -> first battle ->
post-battle scene -> Ailily town -> guild cutscene
```

Measure it with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File profiles\tow-1\scripts\port_status.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File profiles\tow-1\scripts\port_status.ps1 -SkipTests
powershell -NoProfile -ExecutionPolicy Bypass -File profiles\tow-1\scripts\port_status.ps1 -Json
```

The script reports the report/corpus numbers, framework test result, and
missing-HLE/missing-AOT/GE-overflow counts from the local `town_*`, `guild_*`,
`battle_*`, `field_*`, and `inn_*` logs. Those logs are diagnostic artifacts,
not committed source; regenerate them with the capture commands above.

Shareable poster: `PORT_STATUS.png`. Regenerate it with:

```powershell
python profiles\tow-1\scripts\make_port_status_image.py
```

The poster is AOT/recompilation coverage plus HLE and gameplay evidence; it is
not a source-level decompilation percentage.

## Current diagnosis

- Observation: OSK completes and frame 15500 is reachable with GE disabled,
  but the GE-enabled post-OSK run does not produce a clean frame.
- Layer: host GE command-list processing; not the OSK HLE contract.
- Hypothesis: the GE list is submitted before its sublist is fully built, or
  the list's stall/update contract is not modeled; the interpreter therefore
  walks reused/cleared memory and hits the command limit.
- Prediction if true: `0x084A0000` is nonzero at the first CALL, then becomes
  zero later when the guest reuses the buffer; adding stall handling or
  deferring execution should prevent the walk.
- Evidence: `PSPRECOMP_TOW1_GE_SUBLIST_TRACE=1` at 20M shows:
  - `[tow1-ge-sub] 084A0000 became nonzero at list=0x48E0F740`
  - `call ... target=0x084A0000 zero=0 0x55FFFFFF 0x580000FF ...`
  - subsequent calls to `0x084A0000` and `0x084A02F0` also have valid first
    words.
  The failing warning dump later shows `0x084A0000` as zeros, so it is a
  reuse/clear-time artifact, not an initially empty sublist.
- The empty-list guard was therefore removed; it did not address the real
  lifecycle problem.
- Stall logging showed the real contract:
  - `0x48E0F740` is enqueued with `stall = 0x48E0F740` (start).
  - The guest immediately updates it to `0x48E0F78C` / `0x48E0F794`
    (past the `FINISH` at `+0x4C`).
  - The root list has its own `FINISH` at `+76`; the sublists are the
    long-running part.
- GE enqueue now follows the async lifecycle:
  - `sceGeListEnqueue` returns a real list id.
  - Lists with a non-zero stall are queued, not executed immediately.
  - `sceGeListUpdateStallAddr` updates the pending stall.
  - `sceGeListSync` and `sceGeDrawSync` execute queued lists.
  - `sceDisplaySetFrameBuf` flushes any remaining queue before presenting.
- Verification:
  - 20M-dispatch run has **zero** `list ... exceeded the command limit`
    warnings.
  - A title-boot frame at 5000 renders the Bandai Namco Games logo.
  - The first post-OSK frame is captured with GE enabled. It shows the
    full-body character model over the field background, proving the queued
    GE lists render after the dialog transition.
  - `PSPRECOMP_TOW1_POST_OSK_FRAME=path.ppm` arms a one-shot capture of the
    next presented frame; `PSPRECOMP_TOW1_POST_OSK_EXIT=1` exits after it.
  - `PSPRECOMP_TOW1_GE_POST_OSK_DIAG=1` logs enqueue/sync/draw queue activity
    after OSK shutdown.
- Immediate diagnostic knobs: `PSPRECOMP_TOW1_OSK_DIAG=1`,
  `PSPRECOMP_TOW1_GE_COMMAND_LIMIT=N`, `PSPRECOMP_TOW1_GE_DISABLE=1`,
  `PSPRECOMP_TOW1_GE_SUBLIST_TRACE=1`, `PSPRECOMP_TOW1_GE_BAD_LIST_DUMP=1`,
  `PSPRECOMP_TOW1_POST_OSK_FRAME=path`,
  `PSPRECOMP_TOW1_POST_OSK_FRAME_DELAY=N`,
  `PSPRECOMP_TOW1_POST_OSK_BUTTON=mask`,
  `PSPRECOMP_TOW1_POST_OSK_BUTTON_US=N`,
  `PSPRECOMP_TOW1_POST_OSK_INPUT_MASK=mask`,
  `PSPRECOMP_TOW1_POST_OSK_INPUT_DELAY=frames`,
  `PSPRECOMP_TOW1_POST_OSK_INPUT_HOLD=frames`,
  `PSPRECOMP_TOW1_POST_OSK_INPUT_SEQUENCE=delay:hold:mask,...`,
  `PSPRECOMP_TOW1_POST_OSK_FRAME_RANGE_START/END/STEP`,
  `PSPRECOMP_TOW1_LIBFONT_DIAG=1`.

## HLE surface

Implemented or stubbed for single-player:

- scheduler, threads, semaphores, mutexes, event flags, mailboxes, callbacks;
- partition memory, fixed memory pools (`Fpl`), module load/start stubs;
- `IoFileMgrForUser`, UMD raw-sector semantics, archives;
- `sceDisplay`, `sceGe_user`, software GE rasterizer, Win32 presenter;
- `sceMpeg` + FFmpeg H.264, `tow1_atrac` PMF Atrac3+;
- `sceLibFont` host glyph rendering (L4/8bpp, 26.6 metrics);
- `sceSasCore` silent bookkeeping + key-on/off and end-flag completion;
- `sceAtrac3plus` silent stream stubs;
- `sceUtility` savedata headless stub, OSK headless completion, MsgDialog
  headless completion;
- `sceAudio` silent output; controller and display.

Deferred/real-contract work:

- real `sceSasCore` mixing/output and `sceAtrac3plus` BGM/voice;
- real `sceUtilitySavedata*` save/load;
- real MsgDialog choices where the game branches on result;
- `sceNet*` / `sceNetAdhoc*` / `sceNetAdhocMatching` / `sceNetAdhocctl`
  (31 imports; multiplayer only);
- optional `sceUtility` Netconf/HtmlViewer.

## Changes and verification

Recent source changes:

- `host/tow1_hle.cpp`: host-font glyph drawing into guest image buffers;
  OSK/MsgDialog headless HLE; Fpl; Atrac3plus stubs; SAS end flag; pad script;
  title-ready and GE diagnostics.
- `host/tow1_ge.cpp`: configurable GE command limit.
- `host/main.cpp`: HLE histogram report on `std::exit` diagnostics.
- `CMakeLists.txt`: GDI/user32 link for host font rendering.

Verification rung: 4 (character-creation frame; not yet first controllable
field). Tests: see last verified result.

## Learned patterns

- `sceFontGetCharGlyphImage` writes into the caller's 26.6-positioned L4/8bpp
  image buffer; replacing the buffer pointer or its dimensions blanks the UI.
- `sceKernelDeleteThread` must honor the target UID; completing the current
  thread kills `user_main`.
- Character creation uses OSK and voice preview; SAS `GetEndFlag` must
  eventually publish the keyed voice.
- A long input script must continue past the 80-second mark; otherwise the
  character-creation screen sits waiting with no further Cross presses.

## Historical material

The original handoff and gameplay analysis are preserved below verbatim.

---

## Original handoff (verbatim)

# ToW1 Handoff

Continue in a new session by reading this file plus:

- `PSP_PROJECT_STATE.md`
- `profiles/tow-1/README.md`

## Goal

Get Tales of the World: Radiant Mythology (ULES00837) rendering frames in
`Tow1Native`.

## What already works

- Framework builds; profile builds:
  `cmd /c profiles\tow-1\scripts\build.bat`
- The profile links the FFmpeg shared LGPL build (auto-detected from WinGet, or
  set `FFMPEG_ROOT`); the build copies its DLLs beside `Tow1Native.exe`.
- `out/tow-1/bin/Tow1Native.exe` runs the game through:
  - archive/language loading
  - savedata completed-state gate
  - module HLE stubs (libfont load/start observed)
  - newlib `$k0` propagation
  - title.pmf open + PSMF header read
  - FFmpeg H.264 decode per `sceMpegAvcDecode`
  - DMA copy of decoded frames into VRAM
  - software GE rasterizer
  - Win32 GDI presenter
- GE display lists execute and report written pixels.
- Window opens, frame dumps work; verified movie frames are correct.

Run with window:

```text
out\tow-1\bin\Tow1Native.exe profiles\tow-1\game\eboot.dec profiles\tow-1\game\root 2000000
```

Headless + frame dump:

```text
set PSPRECOMP_TOW1_NO_WINDOW=1
set PSPRECOMP_TOW1_FRAME_DUMP=frame.ppm
set PSPRECOMP_TOW1_FRAME_DUMP_ALL=1
```

## Current blocker

Startup is now past the module gate. The key was the savedata dialog state:

- `sceUtilitySavedataInitStart` must report `SHUTDOWN` (`3`), the
  completed-dialog state. `RUNNING (2) -> NONE (0)` and immediate `NONE` both
  make `ModuleLoadingTask` skip module loading.
- With `3`, the native run calls:
  `sceKernelLoadModule("disc0:/PSP_GAME/USRDIR/module/libfont.prx")` and
  `sceKernelStartModule`.
- `FUN_08b1f12c` then returns 1, the `0x08E0E3C8` heap block is allocated with
  size `0x08E0E3D4 = 0xA0000`, and the title proceeds.
- The title opens `disc0:/PSP_GAME/USRDIR/title/title.pmf`
  (28,094,464 bytes), reads its first 2048-byte PSMF header, and requests
  `sceMpegAvcDecode` repeatedly.
- `host/tow1_mpeg.cpp` opens the host PMF through FFmpeg, decodes the next H.264
  frame on each `AvcDecode`, and writes ABGR8888 pixels to the guest buffer.
- The game copies those buffers to VRAM with `sceDmacMemcpy`; the presenter now
  interprets `format=3` correctly as ABGR8888.
- A 20M-dispatch run reaches the dispatch cap with recognizable movie frames,
  verified via `PSPRECOMP_TOW1_FRAME_DUMP`.
- `host/tow1_atrac.cpp` demuxes the PMF private `0xBD` stream and FFmpeg decodes
  2,238 ATRAC3+ frames to interleaved 16-bit stereo PCM. `sceMpegAtracDecode`
  supplies that PCM to the guest.
- With real Atrac PCM, `soundThread` completes and `user_main` reaches the end
  of the movie path.
- The module/title transition now runs:
  - the ModuleDispatcher queue constructor `0x08B17DE4` runs and publishes the
    queue at `0x08D42224` (`0x08E20FA0` in the current heap layout);
  - the Title state callback `0x088216AC` runs;
  - `0x08821658` calls the title setter at `0x08821670`, and
    `0x08DB61D0` becomes `0x08BB4A60` exactly as in PPSSPP.
- With the scheduler cooperative by default (see below), the archive loader
  opens and reads `title/title.arc`, then `TitleArchive/BNG_for_psp.ppt` and
  `TitleArchive/logo480-272.ppt`. Verified with
  `PSPRECOMP_TOW1_HEAP_DIAG=1` (the archive-open hook is gated by that flag).
- The previous blocker was **timer preemption inside the module queue/title
  callback**. `starvation_preempt` could switch threads between the paired
  `0x08806100` getter and `0x08B17F8C` queue call. The queue is transiently
  `[queue] = 0` while processing, so the resumed chain either dereferenced a
  null queue (`0x08821694` / `0x08B17F8C`) or stalled in a ModuleDispatcher /
  `user_main` ping-pong. Default execution-driven timeslicing is now **off**;
  `sceKernelSleepThread`, semaphores, delays, and waits still switch threads.
  Set `PSPRECOMP_TOW1_TIMESLICE` to re-enable timer preemption.
- `0x08E12440 == 1` is normal in PPSSPP and is not the blocker. The silent-SAS
  path is untouched; no SAS flag is forced.
- **Title frame verified.** `host/tow1_hle.cpp` now provides a cooperative
  vblank tick in `sceDisplayGetVcount` (default every 1000 calls, override with
  `PSPRECOMP_TOW1_VCOUNT_TICK`). A default 200M-dispatch run reaches the real
  title screen and presents the full “Tales of the World: Radiant Mythology”
  logo over the cloud/landscape background.
- The title assets are loaded:
  - `title.arc` is gzip; its decompressed EZBIND archive is in guest RAM.
  - `TitleArchive/BNG_for_psp.ppt` and `TitleArchive/logo480-272.ppt` open.
  - The active title sublist is `0x084A0000`; its textured PRIM is
    `0x084A0238` (`format=5` CLUT8, `texaddr=0x08FD45C0`,
    `clut=0x08FF4400`, `texsize=512x512`, `texbuf_width=480`).
  - PPSSPP’s equivalent active targets are `0x08FD4EC0` / `0x08FF4D00`; the
    `0x900` difference is heap layout.
- The earlier PPT-loader/upload investigation was a red herring: the title
  frame renders without host-side PPT upload. The `PPT_*` diagnostics remain,
  but are not the title fix; the scheduler/vblank tick was the missing piece.
- **Movie pacing:** `sceMpegAvcDecode` now advances at most one video frame per
  `33,366 us` of **wall-clock** time (29.97 fps), and the window presenter is
  paced at 60 Hz. Previously the guest decoded the next H.264 frame on every
  display call while the host ran unthrottled, making the opening movie play at
  roughly 2x. `PSPRECOMP_TOW1_MPEG_UNPACED=1` disables decode pacing;
  `PSPRECOMP_TOW1_VSYNC=0` disables presenter pacing.
- **Title menu verified.** `sceKernelDeleteThread` now honors its UID instead
  of always completing the current thread. UID 7 is the movie `soundThread`;
  the old approximation killed `user_main` and prevented the menu. After the
  fix, `PSPRECOMP_TOW1_PRESS_START=1` plus
  `PSPRECOMP_TOW1_PRESS_START_ONLY=1` presses Start once as the title settles
  and again 10 s later. A bounded 300M run reaches the **NEW GAME / CONTINUE /
  OPTIONS** menu at frame 5000.
  - `PSPRECOMP_TOW1_FRAME_DUMP_AT=5000` captures the menu.
  - `PSPRECOMP_TOW1_EXIT_AFTER_FRAME_DUMP=1` stops after that capture.
- **Movie skip:** any controller button while `sceMpegAvcDecode` is running drops
  the remaining video stream and reports EOF, matching the game's skip-on-input
  behavior. `PSPRECOMP_TOW1_SKIP_MOVIE=1` auto-skips after 5 s for testing.
- **Font HLE verified.** The new-game notice/dialogue text renders at frame
  10000. `sceFontGetCharGlyphImage` now rasterizes the requested character with
  a host Windows font and draws it directly into the caller's image buffer,
  preserving the 26.6 `xPos64`/`yPos64`, buffer dimensions, pitch, and pixel
  format. Both the game's L4 (4bpp) and 8bpp paths are implemented with the PSP
  additive/saturating blend. `sceFontGetCharInfo` now returns 26.6 metrics
  (advance, ascender, descender, bearings) instead of the old 8x8 placeholder.
  - `PSPRECOMP_TOW1_LIBFONT_DIAG=1` logs font open, metrics, and glyph calls.
  - The host fallback is Segoe UI/Arial. Japanese glyphs still need a CJK host
    fallback or a real PGF.


The old `libfont.prx` note:

`libfont.prx` in `game/root/PSP_GAME/USRDIR/module/` starts with `~SCE`, so it
is encrypted and cannot be passed directly to `psp_analyze`/`psp_recomp`.
Native load/start still fake the module UID; the game accepts that enough to
proceed.

## Gameplay port status (2026-11-09)

The full import/HLE matrix and milestone order live in
`profiles/tow-1/analysis/gameplay_port_plan.md`.

- Character creation is reachable and renders correctly. The sequence is:
  gender, face, skin color, hair color, voice, job, then name entry.
  Frame 15000 shows the skin-color screen with the host-font text working.
- The character-creation flow uses `sceUtilityOsk*` for name entry and
  `sceSasCore` for voice previews. Both were unregistered or incomplete.
- Added headless but contract-shaped HLE:
  - `sceUtilityOskInitStart/Update/GetStatus/ShutdownStart`: completes with
    the supplied initial text, or `Radiant` when it is empty.
  - `sceUtilityMsgDialogInitStart/Update/GetStatus/ShutdownStart/Abort`:
    completes immediately.
  - `sceKernelCreateFpl/AllocateFpl/TryAllocateFpl/FreeFpl/DeleteFpl`: a
    basic fixed-block pool so character/task code does not stop on the missing
    allocation imports.
  - `sceAtrac3plus` set/decode/sample/loop/release: silent but registered; it
    reports an exhausted stream instead of stopping.
  - `sceSasCore` voice key-on/key-off/end-flag: a silent grain publishes the
    ended-voice mask so voice-preview waits terminate.
- Import inventory: the EBOOT has 252 imports across 27 libraries. Single-
  player paths are covered except for real audio and save semantics. The
  `sceNet*`/`sceNetAdhoc*`/`sceNetAdhocMatching`/`sceNetAdhocctl` libraries
  (31 imports) are multiplayer-only and are intentionally deferred.
- Remaining single-player HLE work, in order:
  1. Verify character creation reaches the first controllable field.
  2. Real `sceSasCore` mixing/output and `sceAtrac3plus` BGM/voice decoding.
  3. `sceUtilitySavedata*` real save/load (current stub only completes dialogs).
  4. `sceUtilityMsgDialog*` real choice/result semantics where the game branches
     on the selected button.
  5. `sceKernelStderr` (trivial) and the optional `sceUtility` netconf/html
     viewers; not needed for story play.
- The generated AOT corpus already contains the game code. Do not port game
  functions by hand. Remaining corpus risk is limited to the 300 analyzer
  `truncated_functions` and any indirect target that was never seeded; add a
  seed only when `Runtime` reports a missing AOT address.
- Current runtime workqueue after HLE: full GE field/battle rendering, VFPU
  coverage, and PPSSPP A/B at each milestone.

## Next steps

1. Select **Guild** from the Ailily town menu and capture the quest
   counter/list dialogue (the five starting quests). The menu is verified in
   `shop_stairs.ppm.51500.png`; the current R/Quit/stairs script is
   frame-sensitive, so first make the cursor selection stable (or use a visible
   manual run).
   - Ghidra trace: the town-menu class string is `9cMenuTown` at
     `0x08C19820`. Menu-page constructors `FUN_08a6704c`/`FUN_08a67234` set
     vtable `0x08C1B870`; `FUN_08a6771c`/`FUN_08a68260` set page vtables
     `0x08C1B9D0`/`0x08C1BCE0`.
   - The actual town-menu main loop is `FUN_08a55858`: it updates the selected
     page, returns the selected index on **Circle**, and forces index 7
     (**Exit town**) on **Cross**. Its caller is `FUN_088274b8`, launched by
     mode entry `FUN_08828570`; that is the open-transition path to target.
   - `PSPRECOMP_TOW1_AUTO_GUILD=1` now detects the town-map pixels and pulses
     Circle, so the remaining target is the **open-transition** function, not
     the Guild cursor.
2. Re-check `cSoundManager`: it should resume at `0x08B1CE44` once the title
   frame loop runs; silent SAS is still acceptable until audio is required.
3. Compare native frames with PPSSPP during the movie/title transition.
4. Re-check the GE output after decode:
   - `PSPRECOMP_GE_STATS=1`
   - `PSPRECOMP_GE_DIAG=1`
   - `PSPRECOMP_TOW1_FRAME_DIAG=1`
   - `PSPRECOMP_TOW1_FRAME_DUMP=...`
5. The host font fallback covers Latin text. A real PGF (or a CJK host font)
   is still needed for Japanese glyphs; `libfont.prx` remains encrypted.
6. If timer preemption is needed later, re-enable it with
   `PSPRECOMP_TOW1_TIMESLICE=20000` and verify the module queue does not get
   preempted between `0x08806100` and `0x08B17F8C`.

## Corpus regeneration

The title path needs a function that the heuristic analyzer misses. The Town
task is the second explicit seed:

```bat
set PSPRECOMP_EXTRA_FUNCTIONS=0x08805FDC,0x08828570
out\framework\psp_recomp.exe profiles\tow-1\game\eboot.dec ^
  --auto profiles\tow-1\generated 0x08804000 0x2000
```

Current corpus after adding the Town task seed: 48,180 function seeds, 654
units, 879,040 emitted code PCs. The runtime still rejects a direct `ctx.pc`
redirect to `0x08828570`; the seed is present, but the unit entry id is not
being selected by the host redirect.

`analyze_program` accepts extra seeds, and `madd`/`maddu`/`msub`/`msubu` are now
lowered.

## Frame verification

The decoded H.264 frame was compared against FFmpeg's direct decode of the same
`title.pmf` (identical 480x272 frame) and against the native frame dump after
VRAM DMA. PPSSPP's GPU-buffer readback currently times out through its remote
debugger, so the reference comparison used FFmpeg plus the game's own movie
timeline.

## PPSSPP oracle

`PPSSPPWindows64.exe` is at `C:\PPSSPP-MCP-v0.2.0\`.

Remote debugger is enabled (`RemoteDebuggerOnStartup = True`) and listens on
`ws://127.0.0.1:52814/debugger`.

PPSSPP MCP (`mcp-ppsspp` 0.2.0) exposes no savestate save/load call; its
documented substitute is PPSSPP's own hotkeys/UI. On 2026-11-20 the instance
is on the correct **Radiant Mythology** (`ULES00837` v1.00), currently at the
opening/title. No `.ppst` town state exists.
The MCP `gpu.buffer.*` screenshot call still returns `Could not download
output`. Workaround used here: read VRAM at `0x04088000` in 64 KiB chunks and
decode 512-stride RGBA; the game renders correctly at 480x272
(`ppsspp_now_rgba.png`). The PPSSPP window itself may be minimized, so
`PrintWindow` is not reliable.

Task-manager observation from PPSSPP memory: `0x08D422A8` is a task stack
header:

```text
+0x00  table pointer (0x08D42238)
+0x04  active task count
+0x08  current task index
+0x0C  next request (-1 = none)
```

At the opening, count=1 and current=0. Writing the current index to 2,
setting count=2/current=1, or setting PC directly to `0x08828570`, does not
switch tasks; the manager needs a proper task push/request. This is the
remaining town-transition mechanism. PPSSPP was reset afterward to restore a
clean opening state.

A direct MIPS scan of the code segment found **no** absolute or `gp`-relative
reference to `0x08D42238`, `0x08D422A8`, or the task table. The consumer is
reached through an indirect/global pointer.

The existing native write watch was used:

```text
PSPRECOMP_WATCH_WRITE=0x08D422A8
PSPRECOMP_WATCH_WRITE_SIZE=0x10
```

It named the consumer:

```text
thread ModuleDispatcher uid=5
pc=0x0880624C  writes count/current
pc=0x08807434  writes count/current
pc=0x088069D0  writes header
```

The small request API is:

```text
0x08806270  getter
0x08806280  pending/current getter
0x088062A0  setter: sw a0, -0xB4C(v1)
             -> 0x08D422B4 = requested task index
```

The watch also recorded the observed task sequence at the 30000-frame town
route: current values 7 -> 0 -> 1 -> 9 -> 5 -> 2. Requesting index 1 in
PPSSPP at the opening produced a black frame/reset; the request must be made
from a valid task state with the right 1-based table index.

The setter was then invoked from the native host at the 30000-frame Town state:

```text
PSPRECOMP_TOW1_REQUEST_TASK_AT_PRESENT=30000
PSPRECOMP_TOW1_REQUEST_TASK_INDEX=1
[tow1-request-task] index=1 end_pc=0x08804000 steps=1
```

The request completed without a crash, but the captured screen remained the
Thomas/plaza scene; Circle pulses only advanced Thomas dialogue and never
opened the town menu. The Guild menu is a **story-gated Town submenu**, not the
task-level Town entry.

Plaza tour timing captured: the Thomas skit closes around present 38500 and
the player becomes free. Pressing Circle while still adjacent to Thomas
immediately reopens his dialogue, and moving away did not open the town menu
in the tested window. The real town menu is the **shop/stairs** path
(`shop_stairs.ppm.51500`), not a Thomas-plaza action.

Repeat attempts with the exact shop/stairs route and auto-Circle exceeded the
30-minute capture budget before frame 51000; no town-menu or quest-list frame
was produced. The remaining efficient path is a PPSSPP town save or a faster
native capture at the shop-menu transition.

The native exploration runs are headless (`PSPRECOMP_TOW1_NO_WINDOW=1`):
there is intentionally no visible `Tow1Native` window, and each captured PPM
comes directly from `window_present`. Remove `NO_WINDOW` to watch it live.
The black `TOLRecomp` window on the desktop belongs to the separate
`tales-of-legendia-ps2recomp` project, not to this profile.

If the game does not progress in PPSSPP either, install PSP firmware
6.60/6.61 in PPSSPP before trying to extract the decrypted module.

## Important files

- `profiles/tow-1/host/tow1_hle.cpp` — HLE, scheduler, I/O, startup diagnostics
- `profiles/tow-1/host/tow1_mpeg.cpp` / `.hpp` — FFmpeg PSMF/H.264 playback
- `profiles/tow-1/host/tow1_ge.cpp` / `.hpp` — GE command interpreter
- `profiles/tow-1/host/ge/` — portable VCS GE rasterizer + stubs
- `profiles/tow-1/host/tow1_window.cpp` / `.hpp` — presenter and input
- `profiles/tow-1/host/main.cpp` — bootstrap
- `profiles/tow-1/CMakeLists.txt` — profile build

## Useful diagnostics

- `PSPRECOMP_FILE_DIAG=1`
- `PSPRECOMP_MODULE_DIAG=1`
- `PSPRECOMP_HLE_HISTOGRAM=1`
- `PSPRECOMP_THREAD_DIAG=1`
- `PSPRECOMP_TOW1_WAIT_DIAG=1`
- `PSPRECOMP_TOW1_HEAP_DIAG=1`
- `PSPRECOMP_GE_STATS=1`
- `PSPRECOMP_GE_DIAG=1`
- `PSPRECOMP_WATCH_WRITE=0x08D422A8` and
  `PSPRECOMP_WATCH_WRITE_SIZE=0x10` — log every guest write overlapping the
  task-stack header, including thread name, dispatch PC, operation, address,
  old and new values.
- `PSPRECOMP_TOW1_REQUEST_TASK_AT_PRESENT=N` and
  `PSPRECOMP_TOW1_REQUEST_TASK_INDEX=I` — invoke the task-request setter
  `0x088062A0` with `a0=I` at post-OSK present `N`. Verified at 30000 with
  index 1; transitions the Town task but does not open the story-gated Guild
  submenu.
- `PSPRECOMP_TOW1_GE_SKIP_RASTER_UNTIL_PRESENT=N` — keep GE list execution and
  guest synchronization, but skip the expensive software raster until post-OSK
  present `N`; used to fast-forward long town routes then capture.
- `PSPRECOMP_TOW1_GE_DISABLE_UNTIL_PRESENT=N` — skip GE list execution until
  `N`; faster still, but gameplay can stall on a black frame because list sync
  is discarded. Prefer the skip-raster variable.
- `PSPRECOMP_TOW1_AUTO_GUILD=1` — scan each presented frame for the Ailily
  town-map signature (white left list + pink selected row + blue coastal map);
  when matched, the host pulses Circle for 5 frames with a 30-frame re-arm gap.
  **Circle** is the correct confirmation here: the traced main loop
  `FUN_08a55858` returns the selected index on Circle, while Cross forces the
  index to 7 (**Exit town**). This makes the Guild selection deterministic
  **once the town menu is on screen**; it does not yet make the
  shop-to-town-menu transition deterministic.
- `PSPRECOMP_TOW1_FORCE_TOWN_AT_PRESENT=N` — host pre-dispatch attempt to enter
  the Town task (`0x08828570`) at post-OSK present `N`. The task-table trace is
  `Title -> FUN_088216ac`, `Town -> FUN_08828570`, from the name/function table
  at `0x08D42240`.
  - Direct `ctx.pc` redirect: `invalid internal function entry` (bypasses the
    registered unit entry id).
  - `Runtime::invoke_isolated_aot` loop: 1,000,000 steps, ended at
    `0x08B17FD8` in the scheduler without opening the menu. The Town task needs
    the real task manager/scheduler and cannot be run as an isolated AOT call.
  - Quest list still not captured through this path.
- `PSPRECOMP_TOW1_FRAME_DIAG=1`
- `PSPRECOMP_TOW1_FRAME_DUMP=...`
- `PSPRECOMP_EXTRA_FUNCTIONS=0x...` (generator extra entry seeds)

---

## Gameplay port matrix (verbatim)

# ToW1 gameplay port matrix

Status: character creation reached (frame 15000); first controllable field not
yet verified.

## What already exists

- AOT corpus: 48,180 functions, 654 units, 122.5 MB, 252 import wrappers.
- Loader/relocations: 277,206 relocations, 0 invalid/unsupported.
- Scheduler, threads, semaphores, mutexes, event flags, mailboxes, callbacks.
- Partition memory and module load/start stubs.
- IoFileMgr + UMD raw-sector I/O, archive loading.
- MPEG H.264 + Atrac3+ opening movie; DMA to VRAM; Win32 presenter.
- GE software rasterizer + ToW1 command interpreter.
- sceLibFont host glyph rendering (L4/8bpp, 26.6 metrics).
- Input mapping and frame dumps.

## HLE coverage by import library

| Library | Imports | State |
|---|---:|---|
| IoFileMgrForUser | 15 | implemented |
| Kernel_Library | 2 | implemented |
| LoadExecForUser | 2 | implemented |
| ModuleMgrForUser | 7 | implemented (fake module UIDs) |
| StdioForUser | 3 | implemented |
| SysMemUserForUser | 6 | implemented |
| ThreadManForUser | 44 | implemented incl. Fpl |
| UtilsForUser | 8 | implemented (cache ops) |
| sceAtrac3plus | 5 | silent stub; real decode pending |
| sceAudio | 9 | silent bookkeeping |
| sceCtrl | 2 | implemented |
| sceDisplay | 9 | implemented |
| sceDmac | 1 | implemented |
| sceGe_user | 11 | implemented + software renderer |
| sceImpose | 1 | implemented |
| sceLibFont | 9 | host font HLE |
| sceMpeg | 25 | implemented, FFmpeg |
| sceNet* / Adhoc* | 31 | deferred (multiplayer) |
| scePower | 2 | implemented |
| sceSasCore | 25 | silent; end-flag fixed |
| sceSuspendForUser | 5 | implemented |
| sceUmdUser | 7 | implemented |
| sceUtility | 21 | OSK/MsgDialog/savedata headless; real save pending |
| sceWlanDrv | 2 | implemented |

## Gate order

1. **Character creation** — current gate.
   - gender -> face -> skin -> hair -> voice -> job -> name.
   - Requires OSK, voice preview end flag, Fpl.
2. **First controllable field** — close the text box, load field, capture
   `PSPRECOMP_COUNT_PC` and HLE histogram; compare PPSSPP.
3. **Field movement / camera / collision** — first gameplay math; watch for
   VFPU/unsupported instruction stops and GE state.
4. **Field menu / status / items** — font + GE + save/load entry points.
5. **First NPC dialogue** — font, event flags, script VM.
6. **First battle** — full GE, VFPU, animation threads, SAS effects.
7. **Save/load** — implement real `sceUtilitySavedata` file contract.
8. **Audio** — real SAS mixing and Atrac3plus BGM/voice.
9. **Long-run stability** — cold start, 10+ minutes, no family A/B/D deaths.

## Function-porting rule

Game functions are already in the AOT corpus; do not hand-port them. Add or
fix only:

- HLE imports whose contract is missing/wrong;
- AOT seeds for addresses the analyzer missed;
- GE/VFPU/runtime features that generated code actually exercises.

The 300 `truncated_functions` are concentrated around `0x08C3...` data-like
regions; treat them as a seed/boundary issue only if runtime reaches them.

## Immediate evidence to collect

- Frame after character creation.
- `PSPRECOMP_HLE_HISTOGRAM` at the first field frame.
- `PSPRECOMP_COUNT_PC` for field update/render functions.
- First `Missing HLE import`, `unsupported instruction`, or `Missing AOT
  function` beyond the character-creation gate.

---

## Legacy root-state section (verbatim)

> Auto-maintained. Read at session start, update after every major action.

## Repository
- PSPRecomp path: `D:\Personal\C++\tales-of-world-psprecomp` (fork of Jessica's PSPRecomp)
- Git branch / commit: `master` / `2c2aadc`
- Working tree status: framework modified (`tools/codegen_main.cpp`); untracked `.opencode/skills/psp-recomp/`, `build_ninja.bat`, `run_game_agent.bat`, `profiles/tow-3/`, `profiles/tow-1/`, `profiles/tow-2/`
- OS / compiler / generator: Windows / MSVC 14.51 / Ninja
- Build directories: `out/framework`, `out/tow-1` (Release)
- Machine: 64 GB RAM, 16 logical cores

## Target
- Title / region: Tales of the World: Radiant Mythology (Europe)
- Product code: `ULES00837`
- Profile: `tow-1`
- Executable: `profiles/tow-1/game/eboot.dec`
- ELF type / load base: `0xFFA0` (PSP PRX) / `0x08804000`
- SHA-256: `105280ceba06338bd6bfdd2142c7950a6cb683c1798f3668c9df8898a2e5b89d`
- Module / gp: `TOW_MAIN_ALFASYSTEM` / `0x08DBA950`
- Current phase: Phase 5 ΓÇö file/archive loading and the software GE + presenter are wired; the game still waits in its startup state before title content loads

## Game data
- ISO extracted with NanaZip to `profiles/tow-1/game/root`:
  - 3,528 files / 357,709,964 bytes
  - `PSP_GAME/PARAM.SFO`, `PSP_GAME/SYSDIR/EBOOT.BIN`, `PSP_GAME/USRDIR/...`, `UMD_DATA.BIN`
- Raw ISO available as a hardlink at `profiles/tow-1/game/tow1.iso` (439,648,256 bytes)
- Device path `umd1:` is backed by that ISO; file paths use `translate_path` into `game/root`

## Build and run
- Framework: `cmake -S . -B out/framework -G Ninja -DPSPRECOMP_PROFILE=""`; build; `ctest --test-dir out/framework -C Release`
- Profile: `cmake -S . -B out/tow-1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=tow-1 -DPSPRECOMP_MSVC_MP_JOBS=8 -DPSPRECOMP_GENERATED_OPT_LEVEL=2 -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF`
- Build: `cmake --build out/tow-1 --target Tow1Native`
- Run: `out/tow-1/bin/Tow1Native.exe profiles/tow-1/game/eboot.dec profiles/tow-1/game/root 1000000`
- Diagnostics: `PSPRECOMP_FILE_DIAG`, `PSPRECOMP_MODULE_DIAG`, `PSPRECOMP_HLE_HISTOGRAM`,
  `PSPRECOMP_THREAD_DIAG`, `PSPRECOMP_PARTITION_DIAG`, `PSPRECOMP_TOW1_HEAP_DIAG`,
  `PSPRECOMP_GE_DIAG`, `PSPRECOMP_GE_STATS`, `PSPRECOMP_TOW1_FRAME_DIAG`
- Presentation: window is on by default; `PSPRECOMP_TOW1_NO_WINDOW=1` disables it,
  `PSPRECOMP_TOW1_SCALE=N` sets integer scale, and
  `PSPRECOMP_TOW1_FRAME_DUMP=path.ppm` writes a frame (add `..._DUMP_ALL=1` for every
  present, or `..._DUMP_AT=N` for one frame).

## Current diagnosis
- Resolved: the `0x08AFB608` fault was **not** a heap allocator failure and **not** a malformed EZBIND header.
- Real cause: the raw `umd1:` device is **sector-addressed**. The game's file worker (Ghidra `FUN_08B08568`, read call at `0x08B08778`) passes a **sector count** to `sceIoRead` and expects a **sector count** back. Our HLE interpreted the value as bytes, so the 189-sector read of `english.arc` copied only 189 bytes into a 387,072-byte buffer. The archive loader then `memcpy`'d the full declared size, over-reading uninitialized heap; the EZBIND parser saw pointers as counts and requested 591,863,536 bytes, which correctly failed.
- `sceIoRead` now reads `count * 2048` bytes for raw devices and returns the number of whole sectors read. Normal files remain byte-addressed.
- After the fix the game loads the Japanese/English archives, initializes the language tables, starts `libfont.prx` (via the module HLE stubs), and enters its startup state machine. It now proceeds through the savedata gate, opens `title.pmf`, and plays the decoded opening movie. A 20M-dispatch run stops on the dispatch cap, not a guest fault.
- **GE path now implemented**: ToW1 uses the portable software GE rasterizer from the VCS profile via `host/ge/ge_renderer.cpp` plus inert VCS config/GPU stubs. The command interpreter in `host/tow1_ge.cpp` handles VADDR/IADDR, PRIM, BBOX/BJUMP, JUMP/CALL/RET, OFFSET/ORIGIN, matrix commands, and END/FINISH. Display lists write into guest VRAM.
- **Presenter now implemented**: `host/tow1_window.cpp` opens a Win32 GDI window, converts the guest framebuffer (8888/565/5551/4444), and presents it. Keyboard input is mapped to the PSP pad and ORed into `sceCtrlReadBufferPositive`.
- **Startup state machine is unblocked**. The savedata dialog path was the gate: `sceUtilitySavedataInitStart` must report the **completed dialog** state, `SHUTDOWN` (`3`), not `NONE` (`0`). With that, `ModuleLoadingTask` reaches `sceKernelLoadModule("disc0:/PSP_GAME/USRDIR/module/libfont.prx")` and `sceKernelStartModule`, then `user_main` proceeds past `FUN_08b1f12c`.
- `FUN_08b1f900` allocates the `0x08E0E3C8` heap block (size `0x08E0E3D4 = 0xA0000`); `FUN_08b1f12c` only checks that it is non-null.
- The title opens `disc0:/PSP_GAME/USRDIR/title/title.pmf` (28,094,464 bytes), reads its PSMF header, and now decodes H.264 video with FFmpeg.
- `host/tow1_mpeg.cpp` demuxes/decodes on demand in `sceMpegAvcDecode`; the frame is written to the guest ABGR8888 video buffer, copied to VRAM by `sceDmacMemcpy`, and presented.
- A 20M-dispatch run reaches the dispatch cap with recognizable movie frames (verified by `PSPRECOMP_TOW1_FRAME_DUMP`), not a guest fault. The current 20M run is still inside the 103-second opening movie.
- A 500M/1B-dispatch run reaches video EOF (3,000+ frames). `host/tow1_atrac.cpp` demuxes the private `0xBD` stream and decodes 2,238 ATRAC3+ frames to interleaved 16-bit stereo PCM; `sceMpegAtracDecode` now feeds that PCM to the guest.
- With real Atrac PCM, `soundThread` completes (`uid=7 state=completed`) and `user_main` reaches its final movie path.
- **Module/title transition now runs authentically**:
  - the ModuleDispatcher queue constructor `0x08B17DE4` publishes the queue at `0x08D42224` (`0x08E20FA0` currently);
  - the Title state callback `0x088216AC` runs;
  - `0x08821658` invokes the title setter at `0x08821670`, setting `0x08DB61D0 = 0x08BB4A60` exactly as in PPSSPP;
  - the archive loader opens/reads `title/title.arc`, then `TitleArchive/BNG_for_psp.ppt` and `TitleArchive/logo480-272.ppt`.
  - Verified with `PSPRECOMP_TOW1_HEAP_DIAG=1` because the archive-open hook is gated by that flag.
- The previous crash was **timer preemption inside the module queue/title callback**, not a missing title pointer. `starvation_preempt` could switch threads between `0x08806100` (queue getter) and `0x08B17F8C` (queue process). The queue is transiently zero while processing, so the resumed chain dereferenced null at `0x08821694`/`0x08B17F8C`. Default execution-driven timeslicing is now **off**; explicit sleeps, semaphores, delays, and waits still switch threads. `PSPRECOMP_TOW1_TIMESLICE` re-enables timer preemption.
- `0x08E12440 == 1` is normal in PPSSPP and is not the blocker. No SAS flag is forced; the silent SAS path is unchanged.
- **Title screen verified.** The missing piece was cooperating vblank time, not the PPT loader. `sceDisplayGetVcount` now advances a virtual vblank every 1000 calls by default (`PSPRECOMP_TOW1_VCOUNT_TICK` override). A default 200M-dispatch run reaches the title screen and presents the full ΓÇ£Tales of the World: Radiant MythologyΓÇ¥ logo over the cloud/landscape background.
- Title assets are loaded (`title.arc`, BNG and logo480 PPTs). The active title sublist is `0x084A0000`; textured PRIM `0x084A0238` uses `format=5` CLUT8, `texaddr=0x08FD45C0`, `clut=0x08FF4400`, `texsize=512x512`, buffer width 480.
- PPSSPPΓÇÖs equivalent targets are `0x08FD4EC0` / `0x08FF4D00`; the `0x900` difference is heap layout. Host-side PPT upload diagnostics are no longer required for the title frame.
- **Movie pacing:** `sceMpegAvcDecode` now advances at most one video frame per `33,366 us` of **wall-clock** time (29.97 fps), and the window presenter is paced at 60 Hz. Previously it decoded the next H.264 frame on every display call while the host ran unthrottled, making the opening movie play at roughly 2x. `PSPRECOMP_TOW1_MPEG_UNPACED=1` disables decode pacing; `PSPRECOMP_TOW1_VSYNC=0` disables presenter pacing.
- **Title menu verified.** `sceKernelDeleteThread` now honors UID 7 (`soundThread`) instead of deleting the current thread. After the movie cleanup, Start opens the title menu; a bounded run captures **NEW GAME / CONTINUE / OPTIONS** at frame 5000. The next work is menu input and the first gameplay scene.
- **Movie skip:** any pad button during `sceMpegAvcDecode` drops the remaining stream and reports EOF, matching the real gameΓÇÖs skip-on-input behavior. `PSPRECOMP_TOW1_SKIP_MOVIE=1` auto-skips after 5 s for testing.
- `libfont.prx` in the extracted game tree starts with `~SCE`, i.e. it is **encrypted**. Native `sceKernelLoadModule`/`sceKernelStartModule` still fake the module UID; the game currently accepts that enough to proceed, but real libfont code is not executed.

## Verified fixes in this session
- **Raw UMD `sceIoRead` sector semantics**: count and return value are 2048-byte sectors on `umd1:`/`umd0:`; this fixed the EZBIND null-allocation crash.
- `WaitThreadEnd` now propagates the completed thread's **exit status**. `ModuleLoadingTask` exits with the module UID as its status; returning 0 caused a 6.6M-thread retry loop and `StartModule(0)`.
- `sceIoLseek` ABI: 64-bit offset is in **a2/a3** (MIPS o32 aligned pair), `whence` in **t0**; device offsets are in **2048-byte sectors**.
- `sceKernelCreateMutex` for this game treats `initCount=1` as an available count, not pre-owned; default is unowned. `PSPRECOMP_TOW1_OWNED_MUTEX=1` restores strict PSP ownership.
- Vblank tick is default-on; `PSPRECOMP_TOW1_NO_VBLANK_SEMA=1` disables it.
- `sceUtilitySavedataInitStart` now reports `SHUTDOWN` (`3`), the completed-dialog state. `GetStatus`/`Update`/`ShutdownStart` keep a consistent record. Earlier `RUNNING (2) -> NONE (0)` and immediate `NONE` both made the loader skip module loading.
- **Explicit analysis seeds**: `analyze_program` accepts extra entry addresses and `psp_recomp --auto` reads `PSPRECOMP_EXTRA_FUNCTIONS` (comma-separated hex). `0x08805FDC` is a real function the heuristic corpus missed; it is now seeded and generated.
- **MADD/MSUB lowering**: `madd`, `maddu`, `msub`, and `msubu` are decoded and emitted as signed/unsigned 64-bit accumulator operations. `madd` at `0x08B2CEC8` was the first missing lowered instruction on the title path.
- **MPEG/Stdio HLE**: `sceMpegInit/Finish`, query/ringbuffer/create/query-stream/regist/init-AU/get-AU/AtracDecode, and `StdioForUser` stdio descriptors are registered. H.264 video is decoded through FFmpeg; the PMF private `0xBD` Atrac3+ stream is demuxed and decoded to PCM.
- **DMA and presentation**: `sceDmacMemcpy` copies decoded frames to VRAM. The presenter now treats `format=3` as ABGR8888 and the lower values as 16-bit formats.
- **Event-flag status ABI**: NID `0xA66B0120` is `sceKernelReferEventFlagStatus`, not `sceKernelGetSystemTimeWide`; the status structure is now populated. The sound thread reads this while waiting after movie EOF.
- Newlib `$k0` module context propagation: the module-start thread stores a module-info pointer at `[k0+4]`; `sceKernelStartThread` now copies that pointer into each new thread's kernel context. Without it, `_getmodreent` printed `libc:%s: no reent structure` from the module dispatcher. It now resolves module UID 1 normally.

## HLE surface
- Scheduler, mutexes, semaphores, event flags, message boxes, callbacks, sleep/wakeup, exit status
- Partition memory, module IDs, module load/start/stop/unload stubs, UMD ready state
- `IoFileMgrForUser`: open/openAsync/close/read/readAsync/waitAsyncCB/lseek/getstat/
  dopen/dread/dclose/devctl/ioctl/write/rename. Device `umd*:` is backed by the ISO;
  regular paths map to `game/root`.
- Utility system params/load module, minimal savedata dialog stubs, power, WLAN, cache, Libc time, SysClock
- `sceMpeg` lifecycle/ringbuffer/stream/AU surface with FFmpeg H.264 decode; `StdioForUser`
- Silent audio + blocking output; silent SAS; minimal `sceLibFont` (NewLib/Open/FindOptimum/
  GetFontInfo/GetCharInfo/GetCharGlyphImage/Close/DoneLib/SetAlt)
- GE software rasterizer (portable VCS renderer + ToW1 command interpreter) and
  Win32 GDI presenter with keyboard input

## Verification
- `psprecomp_tests` 1/1 passed
- `Tow1Native` builds and runs
- Default run completes archive loading, module load/start, opens `title/title.pmf`, and decodes the movie; a 20M-dispatch run stops on the cap, not a guest fault
- Default scheduler reaches the Title callback, sets `0x08DB61D0 = 0x08BB4A60`, and opens `title/title.arc` plus both title logo PPT assets
- Newlib reent warning is gone; all threads resolve the module-info block through `$k0`
- GE display lists execute without interpreter errors; PRIM commands report written pixels
- Presenter window opens/closes cleanly; frame dump path produces a 480x272 PPM
- Game root extracted and verified
- Verified movie frame: a recognizable 480x272 H.264 frame (character in a crowded street), not the previous stripe/noise pattern
- Title frame verified: `frame_default.png` at frame 3423 shows the full Radiant Mythology logo and background

## Ghidra
- Separate project created headlessly: `D:\ghidra-projects\TOW1_mips` (Ghidra rejects `+` in project paths, so it cannot live under `D:\Personal\C++`).
- `eboot.dec` imported as `MIPS:LE:32:default` with the ELF loader, `-noanalysis`.
- Image base must be set to `0x08804000` in the CodeBrowser (`Window -> Memory Map -> Set Image Base`); the ELF loader skipped `-loader-baseAddr`.
- `ghidra-allegrex` is installed but built for Java 25 (class file 69); this Ghidra runs Java 21 (65), so it does not load. Plain MIPS is sufficient for the archive parser; VFPU will be raw/unrecognized.
- The PS2 project stays on GhydraMCP port 8192; the new ToW1 CodeBrowser should appear on the next port (`ghydra_instances_list`).

## Next work
1. Verify title input and menu transitions (`PSPRECOMP_TOW1_PRESS_START=1`), then the first gameplay scene.
2. Re-check `cSoundManager`; it should resume at `0x08B1CE44` once the title frame loop advances.
3. Compare native frames with PPSSPP during the movie/title transition; inspect GE list `0x48E0F740` and its textures.
4. Replace the savedata stub with real save-data behavior when the game starts using saves.
5. Use the window keyboard mapping and `PSPRECOMP_TOW1_PRESS_START=1` for title/menu input once the title renders.
6. If timer preemption is required again, enable `PSPRECOMP_TOW1_TIMESLICE=20000` and verify the queue getter/process pairs are not preempted.

