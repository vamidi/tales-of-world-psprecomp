# Tales of the World: Radiant Mythology (Europe) — `tow-1`

Profile for the first PSP *Tales of the World: Radiant Mythology* title, built with the
PSPRecomp static-recompilation framework.

## Target identity (pinned)

| Field | Value |
|-------|-------|
| Disc ID | `ULES00837` |
| Region | Europe |
| Input executable | `game/eboot.dec` (local, git-ignored) |
| SHA-256 | `105280ceba06338bd6bfdd2142c7950a6cb683c1798f3668c9df8898a2e5b89d` |
| ELF type / load base | `0xFFA0` (PSP PRX) / `0x08804000` |
| Module / gp | `TOW_MAIN_ALFASYSTEM` / `0x08DBA950` |

## Game data

The Europe ISO is extracted into `game/root`:

- 3,528 files / 357,709,964 bytes
- `PSP_GAME/PARAM.SFO`, `PSP_GAME/SYSDIR/EBOOT.BIN`, `PSP_GAME/USRDIR/...`, `UMD_DATA.BIN`
- Raw ISO hardlink: `game/tow1.iso` (439,648,256 bytes)

`umd1:`/`umd0:` device paths are backed by the ISO; normal paths use
`Runtime::translate_path()` into `game/root`.

## Build and run

```bat
profiles\tow-1\scripts\build.bat
profiles\tow-1\scripts\play.bat
```

Manual:

```bash
cmake -S . -B out/tow-1 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPSPRECOMP_PROFILE=tow-1 \
  -DPSPRECOMP_MSVC_MP_JOBS=8 \
  -DPSPRECOMP_GENERATED_OPT_LEVEL=2 \
  -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
cmake --build out/tow-1 --target Tow1Native

out/tow-1/bin/Tow1Native.exe \
  profiles/tow-1/game/eboot.dec \
  profiles/tow-1/game/root \
  1000000
```

The profile links the shared LGPL FFmpeg libraries for `title.pmf` decoding.
CMake auto-detects the WinGet `BtbN.FFmpeg` shared build; otherwise set
`FFMPEG_ROOT` to an FFmpeg tree with `include/`, `lib/`, and `bin/`. The build
copies the required DLLs next to `Tow1Native.exe`.

Regenerate the corpus (the title path needs the explicit `0x08805FDC` function
seed that the heuristic analyzer misses):

```bat
set PSPRECOMP_EXTRA_FUNCTIONS=0x08805FDC
out\framework\psp_recomp.exe profiles\tow-1\game\eboot.dec ^
  --auto profiles\tow-1\generated 0x08804000 0x2000
```

## Current status

| Phase | State |
|-------|-------|
| 1. Input identity | **Complete** |
| 2. Corpus generation | **Complete** (654 units) |
| 3. Import triage / HLE | **In progress** — archives, module load/start, title asset probe |
| 4. First dispatch | **Complete**, multi-threaded |
| 5. First frame | **Complete** — `title.pmf` H.264 frames present and verified |
| 6. Media | H.264 via FFmpeg; ATRAC3+ decoded to guest PCM; SAS output silent |
| 7. Gameplay | Not reached; post-movie title transition in progress |

### Verified I/O path

`ModuleLoadingTask` loads `disc0:/PSP_GAME/USRDIR/module/libfont.prx` (via the
module HLE stubs), then the archive loader reads the language archives:

```text
[tow1][io] open device fd=3 image="...tow1.iso" size=439648256
[tow1][io] lseek fd=3 offset=28816 whence=1 -> 59015168
[tow1][io] read  fd=3 request=189 sectors bytes=387072
```

`request=189` is the **sector count** of the `EZBIND` archive at ISO offset
`sector 28816 * 2048 = 59,015,168`; it maps to 387,072 bytes (the archive is
385,988 bytes, rounded up to its 0x800-byte allocation).

### Resolved blocker

The previous null-allocation fault at `0x08AFB608` was caused by treating the
raw `umd1:` device's `sceIoRead` count as bytes. The game's file worker passes a
**sector count** and expects a **sector count** back. Reading only 189 bytes into
a 387,072-byte archive buffer left the rest uninitialized; the archive parser then
interpreted heap pointers as counts and requested 591,863,536 bytes, which
correctly failed.

After fixing raw-device reads, the archives load, the language tables initialize,
and the game enters its main vblank loop at `0x0880574C`. A 20M-dispatch run stops
on the dispatch cap, not a guest fault.

### GE and presenter

- `host/ge/ge_renderer.cpp` is the portable VCS software GE rasterizer, linked
  with inert VCS config/GPU stubs (`host/ge/vcs_stubs.cpp`).
- `host/tow1_ge.cpp` interprets ToW1's display lists: VADDR/IADDR, PRIM, BBOX,
  BJUMP, JUMP/CALL/RET, OFFSET/ORIGIN, matrix commands, END/FINISH. It writes
  into guest VRAM.
- `host/tow1_window.cpp` opens a Win32 GDI window, converts the guest
  framebuffer (8888/565/5551/4444), presents it, and maps keyboard input to the
  PSP pad.
- Window is on by default. `PSPRECOMP_TOW1_NO_WINDOW=1` disables it and
  `PSPRECOMP_TOW1_SCALE=N` sets integer scale.
- Frame inspection: `PSPRECOMP_TOW1_FRAME_DUMP=out.ppm` (add
  `PSPRECOMP_TOW1_FRAME_DUMP_ALL=1` for every present, or
  `PSPRECOMP_TOW1_FRAME_DUMP_AT=N` for one frame).
- GE diagnostics: `PSPRECOMP_GE_DIAG`, `PSPRECOMP_GE_STATS`,
  `PSPRECOMP_TOW1_FRAME_DIAG`.

Startup now passes the savedata gate and opens the title movie:

```text
[tow1][io] open fd=12 size=28094464 "disc0:/PSP_GAME/USRDIR/title/title.pmf"
[tow1][io] read fd=12 request=2048 bytes bytes=2048 pos=2048 ...
```

That was the module-loading gate: `sceUtilitySavedataInitStart` must report the
completed `SHUTDOWN` state (`3`). The title now calls
`sceKernelLoadModule("disc0:/PSP_GAME/USRDIR/module/libfont.prx")` and
`sceKernelStartModule`. An 8M-dispatch default run reaches the dispatch cap
after opening `title.pmf`.

`host/tow1_mpeg.cpp` now demuxes and decodes `title.pmf` with FFmpeg. The
decoded H.264 frame is written into the guest's 8888 video buffer, copied by
`sceDmacMemcpy` into VRAM, and presented. A 20M-dispatch run reaches the
dispatch cap with recognizable movie frames (verified with
`PSPRECOMP_TOW1_FRAME_DUMP`).

The framebuffer dump/presenter previously interpreted `format=3` as 16-bit;
PSP `sceDisplaySetFrameBuf` uses `3 = ABGR8888`. That mapping is fixed, which
removed the colorful stripe/noise artifact.

The PMF contains a private audio stream (`0xBD`). `host/tow1_atrac.cpp`
demuxes it and FFmpeg decodes 2,238 ATRAC3+ frames to interleaved 16-bit stereo
PCM; `sceMpegAtracDecode` writes that PCM into the guest's 8 KiB buffers.

With real Atrac PCM, `soundThread` now completes (`state=completed`) and
`user_main` reaches the end of the movie path. `title.arc` is still not opened.

`cSoundManager` is stuck in `0x08B31BD0`. The SAS setup wrapper `0x08B31CA8`
initializes SAS and leaves `0x08E12440 = 1`; forcing that flag to `0x40`
crashes early (`0x08821694` / `0x08B17F8C`).

A fresh PPSSPP oracle shows that `0x08E12440 == 1` is normal; PPSSPP also has
`0x08DB61D0 == 0x08BB4A60`, `0x08E0E3D4 == 0xA0000`, and
`cSoundManager` waiting at `0x08B1CE44` (not native `0x08B2F2A8`).

PPSSPP initializes the title pointer via:
`0x08821818` → `0x08821658` (return `0x08821820`).
Native `PSPRECOMP_COUNT_PC` shows **0 hits** for that path, `0x08821738`, and
the menu/state function `0x088055DC`, so the native title state machine is never
entered.

`libfont.prx` starts with `~SCE` — it is encrypted. It cannot be passed to
`psp_analyze`/`psp_recomp` until a decrypted dump is available (for example from
PPSSPP while it loads the module).

## Implemented HLE (highlights)

- Threads: create/start/exit/delay/wait, priorities, timeslice, sleep/wakeup,
  **exit status propagation**
- Mutexes, semaphores, event flags, message boxes, callbacks
- Partition memory; module IDs; module load/start/stop/unload stubs
- UMD ready state; system params; utility module load; savedata dialog stubs; power; WLAN
- **IoFileMgrForUser**: open/openAsync/close/read/readAsync/waitAsyncCB/lseek/
  getstat/dopen/dread/dclose/devctl/ioctl/write/rename
- Cache no-ops; Libc clock/time; SysClock2USecWide/USec2SysClock
- `StdioForUser` descriptors; `sceMpeg` lifecycle, ringbuffer, stream and AU calls
- **FFmpeg H.264 movie decode** (`host/tow1_mpeg.cpp`), on-demand per `AvcDecode`
- **ATRAC3+ private-stream decode** (`host/tow1_atrac.cpp`) into guest PCM
- Silent audio with real blocking delay; silent SAS; minimal `sceLibFont`
- **GE software rasterizer** (portable VCS renderer + ToW1 command interpreter),
  display list queue/sync, EDRAM, mode/framebuffer, vblank/vcount
- **Win32 GDI presenter** with framebuffer conversion and keyboard pad input

## Verified fixes

- **Raw UMD `sceIoRead` is sector-addressed**: `umd1:`/`umd0:` read counts and
  return values are 2048-byte sectors. This fixed the EZBIND null-allocation crash.
- `WaitThreadEnd` must return the completed thread's **exit status**. The module
  loader exits with the module UID as its status; returning 0 caused a
  6.6M-thread retry loop.
- `sceIoLseek` ABI: 64-bit offset in **a2/a3**, `whence` in **t0**. Device offsets
  are in **2048-byte sectors**.
- `sceKernelCreateMutex` for this game treats `initCount=1` as an available count,
  not pre-owned. Default is unowned; `PSPRECOMP_TOW1_OWNED_MUTEX=1` restores strict
  PSP ownership.
- Vblank tick is default-on; `PSPRECOMP_TOW1_NO_VBLANK_SEMA=1` disables it.
- `sceUtilitySavedataInitStart` now reports `SHUTDOWN` (`3`), the completed-dialog
  state. That is the gate that makes the loader call `sceKernelLoadModule` and
  `sceKernelStartModule`; `RUNNING -> NONE` and immediate `NONE` both skipped it.
- Newlib `$k0` module context is propagated into new threads: the module-start
  thread stores a module-info pointer at `[k0+4]`, and `sceKernelStartThread`
  copies it into each new thread's kernel context. This removes the
  `libc:%s: no reent structure` warning from `_getmodreent`.
- `analyze_program` accepts extra entry seeds; `psp_recomp --auto` reads
  `PSPRECOMP_EXTRA_FUNCTIONS`. `0x08805FDC`, a real function missed by the
  heuristic corpus, is now generated.
- `madd`/`maddu`/`msub`/`msubu` are lowered as 64-bit accumulator operations.
- Minimal `sceMpeg`/`StdioForUser` HLE is registered; the title movie now opens
  and its header is read.
- Fixed the `sceKernelReferEventFlagStatus` NID (`0xA66B0120`), which had been
  mistakenly mapped to `sceKernelGetSystemTimeWide`; the sound thread reads this
  status structure.
- FFmpeg H.264 decode is wired into `sceMpegAvcDecode`; decoded frames are
  converted to PSP ABGR8888 and written to the guest video buffer.
- `sceDmacMemcpy` is implemented; the title copies decoded frames into VRAM.
- Fixed the presenter/frame-dump format mapping: `3` is ABGR8888 (4 bytes),
  `0/1/2` are the 16-bit formats. This removed the stripe/noise output.

## Next work

1. Route the decoded Atrac PCM through a real SAS/audio output path and clear
   the guest sound-busy flag at `0x08E12440`. `cSoundManager` currently spins
   in `0x08B31BD0` because low six bits stay set.
2. After that release, verify the title/asset loader opens `title/title.arc`.
3. Compare the native frame sequence against PPSSPP during the title transition;
   check the GE display list `0x48E0F740` and its textures.
4. Replace the savedata stub with real save behavior.
5. Use the window keyboard mapping / `PSPRECOMP_TOW1_PRESS_START=1` for
   title/menu input once the title renders.
