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

`libfont.prx` in `game/root/PSP_GAME/USRDIR/module/` starts with `~SCE`, so it
is encrypted and cannot be passed directly to `psp_analyze`/`psp_recomp`.
Native load/start still fake the module UID; the game accepts that enough to
proceed.

## Next steps

1. Continue from the verified title screen: test input (`PSPRECOMP_TOW1_PRESS_START=1`),
   title menu transitions, and the first gameplay scene.
2. Re-check `cSoundManager`: it should resume at `0x08B1CE44` once the title
   frame loop runs; silent SAS is still acceptable until audio is required.
3. Compare native frames with PPSSPP during the movie/title transition.
4. Re-check the GE output after decode:
   - `PSPRECOMP_GE_STATS=1`
   - `PSPRECOMP_GE_DIAG=1`
   - `PSPRECOMP_TOW1_FRAME_DIAG=1`
   - `PSPRECOMP_TOW1_FRAME_DUMP=...`
5. Real `libfont.prx` remains optional until font rendering needs it; obtain a
   decrypted dump from PPSSPP if needed.
6. If timer preemption is needed later, re-enable it with
   `PSPRECOMP_TOW1_TIMESLICE=20000` and verify the module queue does not get
   preempted between `0x08806100` and `0x08B17F8C`.

## Corpus regeneration

The title path needs a function that the heuristic analyzer misses:

```bat
set PSPRECOMP_EXTRA_FUNCTIONS=0x08805FDC
out\framework\psp_recomp.exe profiles\tow-1\game\eboot.dec ^
  --auto profiles\tow-1\generated 0x08804000 0x2000
```

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
- `PSPRECOMP_TOW1_FRAME_DIAG=1`
- `PSPRECOMP_TOW1_FRAME_DUMP=...`
- `PSPRECOMP_EXTRA_FUNCTIONS=0x...` (generator extra entry seeds)
