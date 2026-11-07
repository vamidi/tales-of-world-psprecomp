# PSP Project State

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
- Current phase: Phase 5 — file/archive loading and the software GE + presenter are wired; the game still waits in its startup state before title content loads

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
- **Title screen verified.** The missing piece was cooperating vblank time, not the PPT loader. `sceDisplayGetVcount` now advances a virtual vblank every 1000 calls by default (`PSPRECOMP_TOW1_VCOUNT_TICK` override). A default 200M-dispatch run reaches the title screen and presents the full “Tales of the World: Radiant Mythology” logo over the cloud/landscape background.
- Title assets are loaded (`title.arc`, BNG and logo480 PPTs). The active title sublist is `0x084A0000`; textured PRIM `0x084A0238` uses `format=5` CLUT8, `texaddr=0x08FD45C0`, `clut=0x08FF4400`, `texsize=512x512`, buffer width 480.
- PPSSPP’s equivalent targets are `0x08FD4EC0` / `0x08FF4D00`; the `0x900` difference is heap layout. Host-side PPT upload diagnostics are no longer required for the title frame.
- **Movie pacing:** `sceMpegAvcDecode` now advances at most one video frame per `33,366 us` of **wall-clock** time (29.97 fps), and the window presenter is paced at 60 Hz. Previously it decoded the next H.264 frame on every display call while the host ran unthrottled, making the opening movie play at roughly 2x. `PSPRECOMP_TOW1_MPEG_UNPACED=1` disables decode pacing; `PSPRECOMP_TOW1_VSYNC=0` disables presenter pacing.
- The next work is input/menu transition and the first gameplay scene.
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

## tow-2 (English `ULJS00175`) — Phase A complete (2026-11-06)

- Input: `profiles/tow-2/game/eboot.dec`
  - SHA-256 `5848ccfaa3291f1dcbef8b04f74de471f5a20387340a8a835c5fb95644f52b80`
  - ELF PRX `0xFFA0`, load base `0x08804000`, entry `0x08804124`
  - Module `TOW2_ALFASYSTEM`, gp `0x08CB5FA0`
  - 213,047 relocations (0 invalid/unsupported), 416 imports, 38,213 function seeds
  - Source was the ISO's already-decrypted `EBOOT.BIN`; a synthetic section table was
    added because the ISO copy had `e_shoff = 0`.
- Corpus: `profiles/tow-2/generated/`
  - 567 units / 100.3 MB, 416 import wrappers
  - `auto_codegen_report.json`: 567/567 units rewritten
- Added `Tow2Corpus` object-library target in `profiles/tow-2/CMakeLists.txt`;
  it compiles the corpus without a host (569/569 build steps, 568 objects).
- Framework: HEAD `2c2aadc`, rebuilt; `psprecomp_tests` 1/1 passed.
  tow-1 bring-up diff pinned at
  `profiles/tow-2/progress/framework-tow1-bringup.patch`
  (SHA-256 `147cd0c97d66be8e113f33ccf0949e075968b272f9a7688513895abcc68d7548`).
- Analysis: `profiles/tow-2/analysis/tow2_report.json` + import/seed/function CSVs.
- Next: Phase B — `profiles/tow-2/host/main.cpp` bootstrap + minimal HLE; reach
  first dispatch before media/gameplay.

### tow-2 Phase B — first dispatch (2026-11-06)

- Added `profiles/tow-2/host/main.cpp`, `tow2_hle.h`, `tow2_hle.cpp`.
- `Tow2Corpus` objects are now reused by `Tow2Native` (no double compile).
- `Tow2Native.exe` builds and reaches first dispatch:
  entry `0x08804124`, gp `0x08CB5FA0`, arena `0x08D15000`,
  222,433 functions, 213,047 relocations (0 invalid/unsupported).
- Triage chain:
  1. `SysMemUserForUser::0xEBD5C3E6` (`sceKernelSetCompiledSdkVersion395`) — implemented, returns 0
  2. `SysMemUserForUser::0xF77D77CB` (`sceKernelSetCompilerVersion`) — implemented, returns 0
  3. `ThreadManForUser::0x446D8DE6` (`sceKernelCreateThread`) — next blocker
- Next: Phase C — thread scheduler + partition memory HLE.

### tow-2 Phase C — scheduler + kernel objects (2026-11-06)

- ISO extracted to `profiles/tow-2/game/root`: 10,816 files / 794.6 MB.
- `tow2_hle.cpp` now implements:
  - threads/stacks/contexts, create/start/exit/delete, priorities, delay,
    wait-thread-end, sleep/wakeup
  - semaphores, mutexes, event flags, mailboxes, callbacks
  - partition memory + raw block allocator
  - UMD-ready, power callback, controller sampling, boot-level GE/display
- Boot chain: `module_start` (uid 0) → `user_main` (uid 1) →
  `cModuleManager` (uid 2, priority 17).
- Guest memory: `UserSbrk` 0x32000 @ `0x08D15000`; `MainMemory` 16 MB @
  `0x08D47000`; `SceGuSignal` event flag created.
- Next blocker: `ModuleMgrForUser::0x977DE386` (`sceKernelLoadModule`).
- Note: GE lists are accepted but not rasterized yet; renderer still pending.

### tow-2 Phase D — I/O + module loading (2026-11-06)

- `game/tow2.iso` hardlink created for raw UMD paths.
- `IoFileMgrForUser` implemented: open/openAsync/close/read/readAsync/
  waitAsyncCB/lseek/getstat/dopen/dread/dclose/devctl/ioctl/write/rename.
  Normal files are byte-addressed; raw `umd0:`/`umd1:` is sector-addressed.
- `ModuleMgrForUser` implemented: load/start/stop/unload/stop-unload-self,
  get module id / id-by-address. Observed module loads:
  `libfont.prx`, `libfile.prx` (stubbed UIDs).
- Added `sceLibFont`, `sceUtility` system params + savedata stub,
  `sceSuspendForUser` power/volatile memory, silent `sceAudio` bookkeeping.
- Boot now runs `FileReadingTask` and `cSoundMusic`/`cSoundVoice`/
  `cSoundEffect`; event flags `cOnMediaStream`, `cSoundMusic`, `cSoundVoice`,
  `cSoundEffect` created.
- Next blocker: `sceSasCore::0x42778A9F` (`__sceSasInit`).
- I/O is implemented but not yet exercised; no archive open has occurred.

### tow-2 Phase E — SAS + real libfile module (2026-11-06)

- Silent `sceSasCore` implemented (init/core/grain/output-mode/envelope/setters).
- `libfile.prx` is a plain ELF/PRX; it is the title's file-index module.
- Generated a secondary corpus at `profiles/tow-2/generated_libfile`
  (1 unit, 10 functions, load base `0x08000000`, `libfile_` symbol prefix).
- `Tow2ModuleCorpus` is linked into `Tow2Native`; `libfile.prx` is loaded and
  relocated at `0x08000000`, and `libfile::0xD8E69C7D` calls the real export at
  `base+0x10` through `Runtime::invoke_isolated_aot`.
- Boot now creates `UMD_SAMPLE_IO_SEMA` and reaches `sceMpegInit`.
- Next blocker: `sceMpeg::0x682A619B` (`sceMpegInit`); next work is the
  `sceMpeg`/`sceAtrac3plus` + FFmpeg media port (tow-1 reference).

### tow-2 Phase E — FFmpeg media decode (2026-11-06)

- Ported `tow1_mpeg.*` / `tow1_atrac.*` to `tow2_mpeg.*` / `tow2_atrac.*`.
- Linked the shared FFmpeg build into `Tow2Native` (WinGet LGPL build;
  runtime DLLs copied post-build).
- Ported the full tow-1 `sceMpeg` HLE surface + `sceDmacMemcpy` and
  `StdioForUser`.
- Verified `title.pmf`: H.264 480x272 decodes; decoded frames are written to
  guest video buffers and DMA-copied to `0x04088000` / `0x04110000`; the
  private Atrac stream decodes 2,042 frames to 8192-byte PCM buffers.
- The movie reaches EOF; the post-movie loop spins on
  `sceMpegRingbufferAvailableSize` / `sceDisplayGetVcount`, and the
  `cSoundMusic` / `cSoundVoice` event flags still have waiters (same class as
  tow-1's post-movie sound blocker).
- No tow-2 presenter yet: frames are in VRAM but GE/display are boot stubs.
- Next: post-movie sound/event transition + GE interpreter/presenter.

### tow-2 Phase E — presenter + visible movie (2026-11-06)

- Ported `tow1_window.*` to `tow2_window.*` (Win32 GDI, 480x272, formats
  0/1/3, keyboard pad input, frame diagnostics/dumps).
- `sceDisplaySetFrameBuf` now presents; `sceCtrlReadBufferPositive` reads the
  live window buttons.
- Added a temporary direct movie-buffer present after `sceDmacMemcpy` for
  `0x04088000`/`0x04110000` (512x272 ABGR8888), because the GE blit is still a
  stub.
- Verified: frame dump present #374 is a recognizable opening-movie frame
  (blue sky/ocean); live window displays the movie.
- Env knobs: `PSPRECOMP_TOW2_NO_WINDOW`, `_SCALE`, `_FRAME_DIAG`,
  `_FRAME_DUMP`, `_FRAME_DUMP_AT`, `_FRAME_DUMP_ALL`, `_PRESS_START`.
- Remaining: post-movie `cSoundMusic`/`cSoundVoice`/ringbuffer loop, then the
  GE interpreter for the title/menus.

### tow-2 post-movie + SAS completion (2026-11-06)

- Confirmed the 500M-dispatch pre-fix run stayed in the `cSoundEffect`
  audio-block loop until stopped.
- Ringbuffer accounting fixed (`Put` adds num; `GetAvcAu`/`GetAtracAu`
  consume one) and MPEG EOF returns:
  `GetAvcAu -> 0x80618001`, `AvcDecode -> 0x80628002`.
- SAS completion: end flag cleared by `__sceSasSetKeyOn`, set after a grain;
  post-movie `cSoundEffect` bits `0x15` published; `cOnMediaStream` bits
  `0x0F` published at video EOF.
- Verified: `user_main`, `soundThread`, and `cSoundEffect` all complete.
- New blocker: `FileReadingTask` polls `cOnMediaStream` `0x0F` while the flag
  is stuck at `0x10`; `title.arc` is not opened yet.
- Do not use `PSPRECOMP_TOW2_FORCE_MEDIA_EVENTS` as a fix: it causes a
  776k UMD open/lseek/close storm.
- Diagnostics: `PSPRECOMP_TOW2_SOUND_DIAG` for event-flag wait/poll/signal.

### tow-2 `cOnMediaStream` handshake trace (2026-11-06)

- `FileReadingTask` sets `cOnMediaStream` bit `0x10` via
  `sceKernelSetEventFlag` at `ra=0x08A5AF40`, `0x08A5AF90`, `0x08A5B0E0`.
- It polls bits `0x0F` via `sceKernelPollEventFlag` at `ra=0x08A5ACE4`
  (helper `0x08A5ACD0`, mode `0x21` = AND + CLEAR).
- `user_main` completes at video EOF (`pc=0x08A63384`), so no consumer
  publishes `0x0F`; the file task keeps polling with the flag at `0x10`.
- Resetting to `0x0F` once does not help (the task re-arms `0x10`).
  Re-signaling on every poll causes a 776k UMD open/lseek/close storm.
- Root-cause hypothesis: `user_main` exits after the movie instead of
  continuing into the title-loader/consumer path. Next: trace its return path
  after the movie player and the `AvcDecode` EOF error handling.

### tow-2 `user_main` post-movie trace (2026-11-06)

- `0x08A632A0 jal 0x08A638F0` plays the movie; result stored at `fp+0x2C`.
- Branching:
  - result == 0: `WaitThreadEnd` -> `0x08A63358`
  - result > 0: `WaitThreadEnd` + `sceMpegFlushAllStream` + clear
    `0x08CB1F00` -> `0x08A63358`
  - result < 0: direct to `0x08A63358`, skipping both
- All paths run the full cleanup at `0x08A63384`..`0x08A635AC`
  (`FreeAvcEsBuf`, `UnRegistStream` x2, `sceMpegDelete`,
  `RingbufferDestruct`, `sceMpegFinish`, `0x08A65064`) and return.
- Corrigendum: the earlier `user_main` completion was **not** normal. The
  `sceKernelDeleteThread` HLE (`0x9FA03CD3`) completed the caller; the movie
  cleanup deletes sound thread uid 9, so it terminated `user_main` at
  `0x08A63384`. Fixed to delete the target uid; verified
  `[tow2] delete thread uid=9 name="soundThread"` and `user_main` resumes.
- Movie result at `fp+0x2C` is `0x80628002` (negative `AvcDecode` EOF), so
  the wrapper skips `WaitThreadEnd` + `FlushAllStream`/`0x08CB1F00` clear.
- Media experiments all negative: one-shot `0x0F` ack is consumed but the
  file task re-arms `0x10`; drain mode reaches 75k+ acks without a title
  load; positive EOF makes the movie player retry-spin at `0x08A72430`.
- Next test: trace the movie wrapper caller (`0x08A63000`, movie task around
  `0x08A62A78`) and the guest cancel path for the `FileReadingTask` media
  request. The worker must be stopped; acking/flushing does not stop it.

### tow-2 movie-end producer trace (2026-11-07)

- Movie task entry is `0x08A62D18`; `PSPRECOMP_TRACE_PC` shows caller
  `ra=0x08A4C848` and `a0` = `disc0:/PSP_GAME/USRDIR/title/title.pmf`.
  The caller is a one-shot wrapper at `0x08A4C818`; it calls the task and
  returns, with no file-worker cancellation.
- `0x08A62D18` is also where the MPEG ring buffer is constructed:
  `ring=0x09FEF90C packets=960 data=0x09428380 cb=0x08A62C20 arg=0x09FEFA14`.
  `arg` is the movie task's `fp+0x144` stream object. Callback `0x08A62C20`
  is a copy/read helper, not the `FileReadingTask` stop.
- The file worker's stream object is `0x08D4F364`: `+0x10=0x00000501`
  (`cOnMediaStream` event flag), `+0x14=0x80020323`, other fields zero.
- Only **three** guest `cOnMediaStream` sets occur in the entire run:
  `bits=0x10` at `ra=0x08A5B0E0`, `0x08A5AF40`, `0x08A5AF90` (all
  `FileReadingTask`). No guest code ever publishes `0x0F`.
- At host video EOF: `packets=960 read=2854 written=6688 used=0`.
  The worker has already written the whole file; the final `0x10` is a
  completion request, not a missing packet.
- Ack experiments are falsified: one-shot `0x0F` is consumed and the worker
  re-arms `0x10`; per-frame/per-consume `0x0F` does not stop it; drain mode
  reaches 75k+ acks and loops. The HLE is bypassing the guest ring-buffer /
  read-pointer protocol, so the worker's state never reaches EOF cleanly.
- Next test: PPSSPP A/B at the movie end (`0x08A5AEC0`/`0x08A5ACE4`,
  `0x08A62C20`, `0x08D4F364`) to capture the real `0x0F` producer, ring-buffer
  callback arguments, and worker stop condition. Then model that state in the
  HLE instead of synthesizing event bits.

### tow-2 PPSSPP A/B — real movie contract (2026-11-07)

- PPSSPP instance: `ULJS00175`, v1.01, debugger port `52814`.
- Real movie-task result at cleanup `0x08A63384`:
  - `[fp+0x2C] = 0`
  - `[fp+0x30] = 0x00010000`
  - `[fp+0x164] = 0x08F0E800`
- The native HLE returned `0x80628002` from `AvcDecode` at EOF. That is the
  contract divergence: the real task ends with result 0, not a fatal error.
- Real `cOnMediaStream` event flag UID is `0x121` (dynamic); the native HLE
  assigned `0x501`. The flag is identified by name, so the UID difference is
  expected.
- Real stream object in PPSSPP is at `0x08D4F3E4` (native `0x08D4F364 + 0x80`):
  `+0x10 = 0x121`, `+0x14 = 0x80020323`.
- Real post-cleanup consumer publishes `0x121` bits `0x01`, `0x02`, `0x04`,
  `0x10` — **never `0x0F`**. These occur after `0x08A63384`; the guest worker
  polls `0x0F` as a mask, so the individual bits are the acknowledgement.
- Native WIP after matching the real EOF success:
  - `AvcDecode` EOF now returns 0 (success, no frame).
  - `AtracPlayer::at_end()` added; `GetAtracAu` returns `0x80618001` when
    video EOF or PCM exhausted.
  - 60M and 120M runs now stop at `0x08A63B54` in the movie player, with
    `cOnMediaStream = 0x0F` and `FileReadingTask` delayed. The file-worker
    stall is gone, but the movie player still spins in its AU-fetch loop.
- Next test: capture the real `GetAvcAu`/`GetAtracAu` return at the end
  (`0x08A63B54` after GetAvcAu, `0x08A63B10` after GetAtracAu) and make the
  guest-side `GetAvcAu` return `NO_DATA` at the true guest stream end.
- A/B scripts: `profiles/tow-2/progress/ppsspp_tow2_ab.mjs`,
  `ppsspp_tow2_result.mjs`.

### tow-2 exit-path counters A/B (2026-11-07)

PPSSPP at `0x08A63BB8` (real movie-player exit path):

```text
v0       = 0x00000000
fp       = 0x09FFF450
fp+0x18  = 0x00D06000
fp+0x1C  = 0x00001977   (6007 vblanks, full movie)
fp+0x30  = 0x00D06000
fp+0x34  = 0x00000001
movie end [fp+0x2C] = 0
```

Native at 60M (`0x08A63B54`, `sceDisplayGetVcount` loop):

```text
vcount   = 983
fp       = 0x09FEF880
fp+0x18  = 0x00960000
fp+0x1C  = 0x000003D6   (982)
fp+0x30  = 0x00D06000
```

`fp+0x30` matches. The native is at vblank ~983 (~16 s of the ~100 s
movie), while the real exit is at 6007. The native is progressing, not
stuck. The 500M run still stops at `0x08A63B54` with `FileReadingTask`
delayed; it is slow, not deadlocked at the old file-worker gate.

Next test: capture the 500M/stop-state counters (log when vcount crosses
5900/6000/6100) and determine whether the native reaches the real exit
condition or whether the host-decoder EOF signal is still missing. A full
run may need ~1.2B dispatches at the current frame rate.

### tow-2 AU-return A/B and current WIP (2026-11-07)

- PPSSPP capture at the AU returns:
  - `0x08A63B54` after `sceMpegGetAvcAu`: last values are all `0x00000000`.
  - `0x08A63B10` after `sceMpegGetAtracAu`: last values alternate
    `0x00000000` / `0x00000001`.
  - Neither ever returns `NO_DATA` (`0x80618001`) at the movie end.
- Therefore the native `used == 0`, host-EOF, and PMF-completion AU gates were
  all wrong and were reverted.
- Verified change kept: `AvcDecode` at host EOF returns `0` (the real movie
  task result is 0).
- Current 60M state: stop at `0x08A63B54`, `FileReadingTask` delayed,
  `user_main` running at `0x08AEE260`. The old `cOnMediaStream` 0x10 polling
  stall is gone.
- Remaining divergence is the movie-player state machine/exit, not the AU
  returns. Next test: capture the `0x08A72550` return at `0x08A63BB8` and the
  movie-player counters (`fp+0x18`, `fp+0x1C`, `fp+0x30`) at the real cleanup
  vs the native loop. The A/B script is
  `profiles/tow-2/progress/ppsspp_tow2_au.mjs`.

### tow-2 500M stop-state diagnosis (2026-11-07)

Run with `PSPRECOMP_TOW2_MOVIE_STOP=1`, 500M and 520M:

```text
video_eof=1
decoded_frames=2847
vcount=6434
fp+0x18=0x00D10000
fp+0x1C=0x00001921        (6433)
fp+0x30=0x00D06000
fp+0x34=0x00000000
ring: packets=960 read=2854 written=6688 used=0
```

PPSSPP at the real `0x08A63BB8` exit path:

```text
v0=0
fp+0x18=0x00D06000
fp+0x1C=0x00001977        (6007)
fp+0x30=0x00D06000
fp+0x34=0x00000001
```

The movie player copies `fp+0` to `fp+0x34` at `0x08A63A04`; state `1` is
the exit path. The state-1 transition at `0x08A63A44` requires:

```text
fp+0x18 == fp+0x30
```

Native has `fp+0x18 - fp+0x30 = 0xA000` (20 packets); PPSSPP has equality.
The worker wrote 6688 packets but `title.pmf` is 6669 packets — 19 extra
packets, matching the ~20-packet overshoot. This is the likely root cause of
the stuck state: the ring-buffer write count overshoots, so the equality
never fires, state 1 / `+0x34=1` is never set, and the movie player loops.

Next test: align/cap the ring-buffer packet accounting to the PMF packet count
(exclude the 19 extra packets) or correct the guest `fp+0x18` end increment;
then rerun 500M and expect state 1, `[fp+0x2C]=0`, post-movie consumer bits,
and `title.arc`.

### tow-2 PMF Put cap — movie end fixed (2026-11-07)

- `sceMpegRingbufferPut` now caps the cumulative ring-buffer write at
  `g_pmf_packet_count - 1` (the first 2048-byte PMF packet is the PSMF header)
  and returns the actual number written.
- 500M result:

```text
[tow2] movie result=0x00000000 fp=0x09FEF8D0
```

The movie task now returns **0**, matching PPSSPP. The movie-player counters
equal (`+18=+30=0xD06000`), and the cleanup is reached.

- 600M stop:

```text
Runtime stopped: dispatch limit at 0x08AC0C40
uid=1 user_main     sleeping at 0x08A70420
uid=4 FileReadingTask delayed
uid=7 cSoundEffect  running at 0x08ABDDD8
cOnMediaStream pattern=0
cSoundEffect pattern=0
```

No `title.arc` yet. The post-movie/title transition is now the active work.
Next test: extend the post-movie run and inspect `cSoundEffect`/SAS completion.
The silent SAS completion signal is currently gated on `g_mpeg_video_eof`;
after movie cleanup that flag is reset, so title sound effects may never
publish `cSoundEffect` completion. Test signaling `cSoundEffect` 0x15
unconditionally after a grain, or identify the post-movie title requester.
