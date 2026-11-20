# PSP Project State — tow-2

> Auto-maintained. Read at session start, update after every major action.
> This is the canonical state for `profiles/tow-2`. The old
> `progress/ASSESSMENT.md` is retained only as a redirect.

## Repository

- PSPRecomp path: `D:\Personal\C++\tales-of-world-psprecomp`
- Git branch / commit: `master` / `de04a3e`
- Working tree: tow-2 profile and host are present; framework bring-up patch
  is pinned under `progress/`.
- OS / compiler / generator: Windows / MSVC 14.51 / Ninja
- Build directories: `out/framework`, `out/tow-2`

## Target

- Title / region: Tales of the World: Radiant Mythology 2 (Japan,
  English-patched ISO)
- Product code: `ULJS00175`
- Profile: `tow-2`
- Executable: `profiles/tow-2/game/eboot.dec`
- ELF type / load base: `0xFFA0` (PSP PRX) / `0x08804000`
- SHA-256: `5848ccfaa3291f1dcbef8b04f74de471f5a20387340a8a835c5fb95644f52b80`
- Module / gp: `TOW2_ALFASYSTEM` / `0x08CB5FA0`
- Entry: `0x08804124`
- Current phase: 7 — post-movie title transition / gameplay stability.

## Analysis and corpus

- Report: `profiles/tow-2/analysis/tow2_report.json`
- Relocations: 213,047; unsupported 0; invalid 0
- Imports: 416 across SDK and per-character/enemy data modules
- Function seeds: 38,213
- Corpus: `generated/`, 567 units / 100.3 MB / 416 import wrappers
- Secondary corpus: `generated_libfile/`, 1 unit / 10 functions, load base
  `0x08000000` for the real `libfile.prx`
- Framework bring-up patch:
  `progress/framework-tow1-bringup.patch`
  (SHA-256 `147cd0c97d66be8e113f33ccf0949e075968b272f9a7688513895abcc68d7548`)

## Build and run

```bat
cmake -S . -B out/tow-2 -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DPSPRECOMP_PROFILE=tow-2 ^
  -DPSPRECOMP_MSVC_MP_JOBS=8 ^
  -DPSPRECOMP_GENERATED_OPT_LEVEL=2 ^
  -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
cmake --build out/tow-2 --target Tow2Native
out\tow-2\bin\Tow2Native.exe profiles\tow-2\game\eboot.dec profiles\tow-2\game\root
```

Scripts: `profiles\tow-2\scripts\build.bat`, `generate.bat`, `play.bat`.

Last verified result:

- `Tow2Native` builds; the opening movie decodes and returns `0`.
- The title-renderer GE crash is fixed by a profile-local no-op at
  `0x08A76ED8` (null GE context).
- Scene-manager writers traced: `0x08817AD0` initializer,
  `0x08814848` reset, `0x08AA33C0` memset.
- Missing seeds `0x0896FFB8` and `0x08970A00` were added via
  `PSPRECOMP_EXTRA_FUNCTIONS`; corpus regenerated (5 units) and rebuilt.
- The title task entry was corrected to `0x0896FFB8`.
- **`title.arc` is reached**: the engine's own synchronous open wrapper
  `0x08A7384C` opens `disc0:/PSP_GAME/USRDIR/title/title.arc` (fd 6) and the
  host's native EZBIND reader parses it (version 4, 21 entries).
- The run then continues to the dispatch limit at `0x08A5F69C` without a
  guest fault when the mount/find probes are disabled.

## EZBIND and the title archive (2026-11-17)

Decoded from `title/title.arc` and the engine code; `EZBIND` is a plain
container format, **not** PSP-specific, so the recompiled guest parser does not
need a hardware override.

- On-disk layout (v4):
  - `0x00` `char magic[8]` = `"EZBIND\0\0"`
  - `0x08` `u32 count`
  - `0x0C` `u32 version` (4)
  - `0x10` `entry entries[count]`, 16 bytes each:
    `{ u32 name_offset; u32 size; u32 data_offset; u32 hash; }`
  - `name_offset` and `data_offset` are absolute file offsets; names are
    NUL-terminated; data starts 4-byte aligned.
  - `hash` is an engine hash: it is **not** CRC32 of the data and **not**
    CRC32 of the name.
- `title/title.arc`: 1,369,296 bytes, version 4, 21 entries
  (`sky.ppt`, `layer_01.ppt`, ..., `lisence.ppt`). Entry names omit the
  `TitleArchive/` prefix used by the EBOOT string pool.
- Entry payloads are **individual gzip members** (magic `1f 8b`, with the
  original filename in the gzip header). The entry `size` field is the
  **stored/compressed** length, not the uncompressed length; for example
  `sky.ppt` stores 162,064 bytes and inflates to 522,272.
- Title archive mount routine: **`0x088206D8`**. It passes
  `"title/title.arc"` (`0x08C23BB0`) and the mount point `"TitleArchive/"`
  (`0x08C23BC0`) to the VFS mount **`0x08A59210(manager, prefix, source)`**.
  - The earlier diagnostic used `0x08C239B0`, which is 0x200 bytes low and
    points at `"/water_02.ppt"`; that is why the first forced open missed.
  - `0x088206D8` is referenced only from a relocated data pointer, so it never
    runs in the current native boot path.
- Engine file I/O:
  - sync open wrapper **`0x08A7384C`** (UMD drive-stat check, then
    `sceIoOpen` at `0x08AEDB48`); this is the path used by the verified run.
  - async open `sceIoOpenAsync` (`0x08AEDB18`) has exactly one call site
    (`generated_unit_0304.cpp`), inside the async file worker.
- Native reader: `host/tow2_ezbind.{hpp,cpp}`. Every `.arc` opened by the
  guest is parsed by `inspect_ezbind_archive()` and cached in
  `g_ezbind_archives` (lookup via `find_ezbind_archive`).
- Diagnostic runtime notes:
  - `Runtime::invoke_isolated_aot` executes **one** dispatch only; a guest
    routine that calls an import returns at the import PC. Use the profile
    helper `run_isolated_aot()` (loops dispatch until a return sentinel).
  - `PSPRECOMP_TOW2_EZBIND_PROBE=1` calls `0x08A7384C` with the full
    `disc0:` path and proves the archive is reachable.
  - `PSPRECOMP_TOW2_TITLE_MOUNT` / `PSPRECOMP_TOW2_VFS_FIND` currently do not
    complete (`mount ok=0`) and corrupt guest state; keep them off.

## Scene-change path (2026-11-17)

Traced the id field and the real scene-start API.

- Scene id: `u16` at `scene+4` on the object at `[0x08D14F2C]`.
  - `0xFFFF` means "no scene".
  - Valid ids are `0..27`: `0x0881460C(scene)` returns the id when
    `< 28`, else `-1`.
  - `0x08814460(scene)` returns `scene+4` when the id is not `0xFFFF`, else
    `0`. This is the gate that makes `0x08813874` exit early.
- Id setter: `0x08817298(dst, descriptor)` stores
  `[dst+4] = 0x08818FB4(descriptor)` and copies the descriptor name; it is the
  only non-reset writer of `scene+4`. `0x08818FB4` reads `u16 [descriptor+0]`
  when `u32 [descriptor+728]` is 1 or 2. This function has **no static
  caller** and never runs in the current flow.
- Real scene start: **`0x08814628(scene, mode)`**.
  - The title script calls it at `0x0897C684` with `mode=1`, immediately after
    `scene = 0x08AC5428()`.
  - It requires `[scene+15348] != 0`; otherwise it returns `-1` and leaves the
    id at `0xFFFF`.
  - `[scene+15348]` is the live scene object, set by
    `0x08814AEC(scene, p5, p6, p7, p8, p9)` together with `+15340`,
    `+15344`, `+15352`, `+15356`. The registration path runs at
    `0x08810194`..`0x088101E8` from a descriptor held in `gpr18`.
- Forcing `[scene+4] = 0` (`PSPRECOMP_TOW2_SCENE_ID=0`) does get
  `0x08813874` past the gate, but the state2 thread then faults in unit 0318
  (`0x00000001`) because the scene struct is empty after the reset.
- `PSPRECOMP_TOW2_SCENE_START=1` calls `0x08AC5428()` then
  `0x08814628(manager, 1)`. Verified result:
  `start(1) ok=1 ret=0xFFFFFFFF`, scene id still `0xFFFF`.
- Verification: no `TitleArchive/...` request in any configuration; only
  `title/title.pmf` is opened.

Next step: populate the scene via `0x08814AEC` (the
`0x08810194` registration path) before `0x08814628`, or find the script call
that sets `[scene+15348]`, then re-run the start and watch for
`TitleArchive` opens.

### Registration + start attempt (verified 2026-11-17)

- `0x08814AEC(scene, 0, 0, scene, 0, 0)` installs the manager itself as the
  scene object; fields read back as `+15340=0`, `+15348=0x08D9B920`.
- With that registration:
  - `0x08814628(scene, 0)` returns `0x08D10000` (a real pointer, not `-1`).
  - `0x08814628(scene, 1)` returns `-1` **and moves the scene id off
    `0xFFFF` to `0`**; mode 2 keeps `id=0`.
- Combined with `PSPRECOMP_TOW2_TITLE_MOUNT_THREAD=1` and
  `PSPRECOMP_TOW2_HOLD_USER_MAIN=1` the run is stable (no guest fault) and
  reaches the dispatch limit at `0x08A780BC` (title actor update).
- New evidence of title asset setup: `0x0881F5B0` (title asset list builder)
  executes **2 times**, and `0x0881EC38` (title object update) ~29,600 times.
- Still **no `TitleArchive` open**: `0x08A7384C` (sync open wrapper) hits 0,
  the file worker stays idle, and `PSPRECOMP_FILE_DIAG=1` shows only
  `title/title.pmf`.
- Environment for this configuration:
  `PSPRECOMP_TOW2_TITLE_TRIGGER=1 PSPRECOMP_TOW2_TITLE_MOUNT_THREAD=1
   PSPRECOMP_TOW2_HOLD_USER_MAIN=1 PSPRECOMP_TOW2_SCENE_START=1
   PSPRECOMP_TOW2_SCENE_REG=1`

Next: find what submits the built asset list (the state machine around
`0x0881EC38` / the title state thread). The list is built but never handed to
the file manager.

### Asset-list next state (traced 2026-11-17)

- The list built by `0x0881F5B0` is consumed by the title object's update
  loop, which constructs **`c2c2Sprite`** objects:
  - vtables `0x08C6CEA0` and `0x08C44078`
  - virtual hook at `vtable+180` = **`0x08A5FD30`** (sprite update/draw)
- `0x08A5FA28` is **not** a file submit. It stores `a1..a4` at `this+80..+92`
  and calls `[vtable+180]`. Traced calls:
  `self=0x08D9F850 vtable=0x08C44078 hook=0x08A5FD30 a1=0x40 a2=0x30 ...`
  (small geometry values, not paths).
- It runs **633,578 times** in a 300M-dispatch run, i.e. the title update
  loop is iterating sprites, not requesting files.
- VFS activity in that configuration:
  - `0x08A59378` (VFS find) = **0**
  - `0x08A7384C` (sync open) = **0**
  - `0x08A59210` (mount) = 4
  - `0x08A59768` (file worker loop) = 7,910
- `PSPRECOMP_FILE_DIAG=1` shows no `.arc` open; only `title/title.pmf`.

So the title object is in its **sprite/actor update state**, not its asset
load state. The VFS submission is upstream of `0x08A5FA28`; it must happen in
the title object's load state machine before the sprites are configured.

Next: identify the state that turns the built list into VFS requests (likely
the title object's `0x0881EC38` state machine / `0x0881C224` load path) and
drive that transition, then re-check `0x08A59378`/`0x08A7384C` and
`[tow2][ezbind]`.

### `0x0881C224` / update trace (2026-11-17)

- `0x0881C224` is a **registration method**, not a loader. It has no direct
  callers (virtual only). It:
  1. `0x0881BFB0(this)`
  2. `scene = 0x08AC535C()`
  3. `[scene_vtable+176](scene, this)`
  4. `[scene_vtable+176](scene, this+240)`
  5. `0x0881F5B0(this+12032)` — build the asset list
  6. `[scene_vtable+176](scene, this+12032)`
  7. `L_0881D768(this+13280)`
  8. `[scene_vtable+176](scene, this+13280)`
  9. `0x0881FBB0(this+12032, this+13280)` (sets `[+12032+1224]`)
- `0x0881EC38` is the epilogue of the per-frame update `0x0881E79C`, not a
  state switch. That function reads bytes at `0x08CAE068` and `0x08CAE070`
  (both `1` at the trigger) and dispatches virtuals; no state variable.
- The object addressed as `g_title_object + 12032` has **vtable 0** at both
  trigger and stop time. So the registered `this` is *not* `0x08D51A70`; the
  asset container belongs to a different scene-registered object.
- Consequence: the load branch cannot be driven by poking
  `g_title_object+12032`; its owner must be taken from the scene registration
  (`[scene_vtable+176]` call at `0x0881C248`/`0x0881C268`).
- Verification in this configuration is still negative: `0x08A59378` (VFS
  find) and `0x08A7384C` (open) are 0; no `TitleArchive` open.

Next: capture the `this` argument of `0x0881C224` when the scene manager
invokes it, dump that object's `+12032` container (vtable/state), and drive
its load virtual before the sprite update.

### Captured container + drive attempt (2026-11-17)

- The registered asset container is found by scanning for vtable
  `0x08C44B08` (set by its constructor `0x08AC5C98`):
  - container `0x08DA4460`, owner `0x08DA1560` (container = owner + 12032)
  - state `+1228` = `3` at stop (was `9` in another run)
  - `+1232` = 1 (list built), `+1233` = 0
  - `+1224` = `0x08DA4940` (owner + 13280)
- Container vtable `0x08C44B08` (selected slots):
  - `+0`  `0x08AC5BC0`
  - `+4`  `0x08AC5C98`  — constructor/reset (re-installs the vtable)
  - `+8`  `0x08A6158C`
  - `+20..+100` `0x08A5Exxx` (c2c2Sprite methods)
  - `+104` `0x0881F4E8` — clears the five sprites at `+576` (stride 128)
  - `+108..+124` more `0x08A5Exxx`
- Drive attempt: after 20,000 dispatches from the mount-thread start the
  container exists (state 0) and the probe invoked `[vtable+4]`
  (`0x08AC5C98`) with `a0=container`. It faulted:
  `Guest memory access ... 0x00000009` (null + 9), because that slot is the
  constructor/reset, not a load method, and the container was mid-init.
- No `TitleArchive` open in this attempt.

Next: do not call `+4`. Identify the owner-side load call (the owner
`0x08DA1560` is the object whose virtual dispatch advances `+1228` from 0 to
3), or call a non-constructor slot (`+8`/`+104`) once the state is settled.

### `+1228` is an animation frame index, not a load state (2026-11-17)

State watching (`PSPRECOMP_TOW2_STATE_WATCH=1`, container located by vtable
scan) shows a monotonic counter:

```
[tow2-state] 0x00000000 -> 0x00000001 after_pc=0x0881F404
[tow2-state] 0x00000001 -> 0x00000002 after_pc=0x0881F404
...                                        (wraps at 31)
```

`0x0881F404` is an animation tick:

```
0x0881F404:
    if (*(u8*)(this+1232) == 0) return;
    frame = (*(u32*)(this+1228) + 1) & 31;
    *(u32*)(this+1228) = frame;                 // 0..31
    ... interpolate using frame and this+432 ...
```

So the container at `owner+12032` is an **animation/sprite player**:
- `+1228` = current frame (0..31)
- `+1232` = animation enabled
- `+104` (`0x0881F4E8`) clears the five sprite records
- `+4` is the constructor/reset that faulted when driven

There is no asset-load virtual in this container. The `ppt`/`eai` pairs are
animation frames referencing textures loaded elsewhere (sprite texture path),
which is why driving `+4` never produced a `TitleArchive` open.

Next: inspect the sprite constructor/texture setter (`0x08A5F1A4`,
`0x08A5EF88`, `0x08A5FA28` → `0x08A5FD30`) for the texture source call, or
find the texture manager that consumes the `ppt` name from the pair.

### Texture source trace (2026-11-17)

The mount is **valid**; the earlier VFS check used the wrong lookup API.

- Mount list node `0x08F11D40` (manager `0x08D4F120`):
  - `+0/+4` list links, sentinel `0x08D4F130`
  - `+8` = `0x08F11B40` → `"title/title.arc"` (source path)
  - `+0x80` = `13` (prefix length)
  - `+0x84..` = `"TitleArchive/"` stored inline
  - `+0x40` = `0x08F11D00`, `+0x44` = `0x08F11F00` (source objects)
- `0x08A59378(manager, "TitleArchive/water_02.ppt")` returns `0` because it
  compares `[node+8]` (the *source* path) — it is a find-by-source helper, not
  a virtual-path resolver. The mount itself succeeded.
- Five sprite records at `container+576` (stride 128):
  ```
  sprite 0 @0x08DA46A0: vtable=0x08C44078 owner=0x08DA4460 +16=0x58 +20=0xF8
  sprite 1 @0x08DA4720: vtable=0x08C44078 owner=0x08DA4460 +16=0x98 +20=0xF8
  sprite 2 @0x08DA47A0: ... +16=0xD8
  sprite 3 @0x08DA4820: ... +16=0x118
  sprite 4 @0x08DA48A0: ... +16=0x158
  ```
  `+16` steps by `0x40`, `+20` is `0xF8` — atlas UV rectangles, not paths.
  No texture pointer is bound yet (those fields are `0`).
- Therefore the texture source is resolved through the mount prefix
  (`node+0x80`/`node+0x84`), by a resolver other than `0x08A59378`, and it is
  never reached because the sprites have no texture assigned.

Next: find the virtual-path resolver that compares the inline prefix at
`node+0x80`, and the sprite texture setter that feeds it the `ppt` name.

### Sprite setter + prefix resolver (2026-11-17)

- Sprite vtable `0x08C44078` (dumped at runtime):
  ```
  +0  0x08AC53E4   +4  0x08AC53F4   +8  0x08A6158C   +12 0x08A61600
  +16 0x08A61644   +20 0x08A5DBD0   +24 0x08A5DBD8   +28 0x08A5DC28
  +32 0x08A5DC30   +36 0x08A5FAD4   +40 0x08A5FA44   +44 0x08A5FA74
  +48 0x08A5FAA4   +52 0x08A5DC58   +56 0x08A5DC50   +60 0x08A5DB40
  +64 0x08A5DB94   +68 0x08A61A38   +72 0x08A616D4   +76 0x08A61724
  ```
- The asset-list builder's per-sprite call is `[sprite_vtable+36]` =
  **`0x08A5FAD4(sprite, 64, 16)`**. It is a thin forwarder to
  `0x08A5DBBC` + `[sprite_vtable+180]`; it is **not** a texture bind.
- The sprite record at `container+576` (stride 128) holds only:
  `[0]=vtable 0x08C44078`, `[1]=container`, UV pairs at `+16`/`+20`
  (`0x58/0x98/0xD8/0x118/0x158`, `0xF8`). No texture or name pointer is
  assigned in the fields that were dumped.
- The mount entry's prefix is inline: `+0x80` = 13, `+0x84` = `"TitleArchive/"`.
  The resolver that compares this inline prefix is **not** `0x08A59378`
  (that compares `[node+8]`, the source path).
- Verification: `PSPRECOMP_FILE_DIAG=1` still shows only `title/title.pmf`;
  no `TitleArchive` request.

Next: locate the function that walks the mount list and compares the inline
prefix at `node+0x80`, and find where the sprite's texture/name field is
written (not the `+36` update call).

### Walker / texture-assignment search (2026-11-17)

- The mount node's `+0x40`/`+0x44` fields point into heap free-list nodes
  (`0x08F11C80`/`0x08F11D80`), not to source objects:
  ```
  obj 0x08F11D00: +0=0x08F11C80 +4=0x08F11D80, rest 0
  0x08F11C80:     +0=0x08F11B00 +4=0x08F11D00
  ```
- The `+132` references in the 0x08A5Cxxx/0x08A5Dxxx code are vtable slot
  offsets (`vtable+132`), not the mount prefix.
- No function in the corpus compares the inline prefix at `node+0x84`;
  `0x08A59378` remains a find-by-source helper.
- No sprite texture/name assignment was found; the only per-sprite call in
  the builder is `[vtable+36] = 0x08A5FAD4(sprite, 64, 16)`, a forwarder.
- Verification unchanged: `PSPRECOMP_FILE_DIAG=1` shows only
  `title/title.pmf`; no `TitleArchive` open, no `[tow2][ezbind]`.

The mount and the sprite records are both real and correctly populated, but
the resolution/assignment path that would request an entry has not been
located in the generated corpus. Next options: trace `0x08AAAF48` (the hash
used when creating the mount entry) and search for comparisons against that
hash, or use PPSSPP as an oracle to capture the real request call site.

### `0x08AAAF48` is `strlen`, not a hash (2026-11-17)

Disassembly of `0x08AAAF48`:

```
0x08AAAF48:
    v0 = a0 + 1
loop:
    v1 = *(u8*)a0
    a0 += 1
    if (v1 != 0) goto loop
    return a0 - v0        // length
```

So the mount entry does **not** store a name hash. `0x08A59210` stores
`strlen("TitleArchive/") = 13` at the entry, and the prefix bytes inline:
that is exactly the `+0x80 = 13` / `+0x84 = "TitleArchive/"` pair seen in the
mount node. The neighbouring `0x08AAAF60` lowercases characters using the
table at `0x08BF24FD`, i.e. case normalisation for path comparison.

Consequence: the resolver is a **length + case-normalised byte prefix match**,
not a hash comparison. Searching for hash comparisons is a dead end.

### PPSSPP oracle session (2026-11-17)

PPSSPP v1.20.4 is reachable with the same title (ULJS00175, v1.01) and
execution breakpoints work.

- Breakpoint on `sceIoOpen` (`0x08AEDB48`) after reset caught, in order:
  - `a0=0x08C3B5CC "umd1:"`, `ra=0x08A5A358` (FileReadingTask)
  - `a0=0x08C23AD0 "ms0:/PSP/SAVEDATA/..."`, `ra=0x0881D7E0`
- Breakpoint on the archive mount `0x08A59210` ran for >2 minutes without a
  hit. The game idles with `[0x08D14F2C] -> 0x08D9B9A0`, `scene+4 = 0xFFFF`,
  and `pc = 0x08A763A0` (the same `0x08A763D4` family called by
  `0x088206D8`). Scene `0xFFFF` is therefore not unique to the native port.
- Screenshot capture is broken in this MCP build (`Could not download output`),
  and the memory-scan tool is not exposed, so the title screen state cannot be
  confirmed visually from here.

No `TitleArchive` open was observed in either the native port or the PPSSPP
window.

### Narrower resolver test (2026-11-17)

Found a candidate resolver at `0x08A58D3C(a0=manager, a1=name)`:

```
0x08A58D3C:
    sema_wait(manager+576)
    node = [manager+16]
    sentinel = manager+16
    while (node != sentinel):
        name = [node+8]
        node = [node]
        if (strcmp(name, a1) == 0):        // 0x08AA9BE4
            source = [name+264]
            [source->vtable+36](source, 2)
            ...
            return
```

It is the only VFS-region walker that compares `[node+8]` and then uses the
source field at `+264` (the same field `0x08A59210` stores). Driving it:

```
[tow2-resolve] query="title/title.arc" manager_ok=1 ok=1 ret=0x00000000
```

Result: no match, no open (`ret=0`). So either `[node+8]` is not the plain
source string, or this walker expects a different key/typed string object.

Follow-up controls:

- `0x08AA9BE4` is a plain **strcmp** (byte loop with null handling), so a raw
  `char*` is the correct argument type.
- The walker does find the mount node and reads its source:
  `name=0x08F11B40 "title/title.arc" src=0x08DA1270 svt=0x08C703F0`.
- The return value is **always 0**: a negative control
  (`RESOLVE="zzz_not_a_file"`) also returns `0`, so `ret` is not a match
  signal. The walker is not usable as a probe of "found/not found".
- Neither query produced any I/O (`ret=0`, no `[io]` line).

Conclusion: `0x08A58D3C` is a *source-name* walker whose matched virtual is not
the archive body reader. The mount source object is `0x08DA1270`
(vtable `0x08C703F0`); its `vtable+36` slot needs to be identified directly.

### Mount-source vtable dump (2026-11-17)

The vtable stores **image-relative** offsets; add `0x08804000` at runtime.

```
0x08C703F0: 0x002567E0 0x002567E8 0x002567F0 0x002567F8
            0x00258480 0x00256B14 0x00258218 0x0025828C
            0x00258368 0x002567D8 0x00256AC4 0x00256B68
            0x002561C0 0x002564AC 0x002568AC 0x00256A00
```

Resolved slots:

| slot | target | role |
|------|--------|------|
| +0  | 0x08A5A7E0 | get `[this+16]` |
| +4  | 0x08A5A7E8 | get `[this+8]` |
| +8  | 0x08A5A7F0 | get `[this+12]` |
| +12 | 0x08A5A7F8 | get `[this+4]` |
| +16 | 0x08A5C480 | `0x08AC5338()` + `0x08A58ACC(manager, source)` — attach/register |
| +20 | 0x08A5AB14 | set `[this+284]=1` (mode) |
| +24 | 0x08A5C218 | virtual dispatch |
| +28 | 0x08A5C28C | — |
| +32 | 0x08A5C368 | — |
| +36 | 0x08A5A7D8 | set `[this+16] = arg` (mode setter) |

So `0x08A58D3C`'s call `[source+36](source, 2)` is a **mode setter**, not an
open. The source is a file abstraction attached through `0x08A5C480` /
`0x08A58ACC`; its read path is manager-side, not this vtable.

### PPSSPP SKU mismatch (2026-11-17)

After a `ppsspp_reset`, the emulator reports a **different title**:

```
Title: Radiant Mythology   Disc ID: ULES00837   Version: 1.00
```

Earlier in the same session it reported `ULJS00175` v1.01. The tow-2 profile
targets `ULJS00175`. Therefore:
- JP address breakpoints (`0x08A59210`, `0x088206D8`) cannot be trusted in
  the current emulator state — the loaded module may be a different build.
- The earlier "mount breakpoint never fired" result is inconclusive.
- To use PPSSPP as an oracle for this profile, the JP/English-patched ISO
  (`ULJS00175`) must be the loaded disc before setting breakpoints.

### PPSSPP ULJS00175: first TitleArchive request captured (2026-11-18)

Emulator confirmed on the target SKU (`ULJS00175` v1.01).

- Breakpoint `0x08A59210` (mount) hit during boot:
  ```
  a0 = 0x08D4F1A0   manager
  a1 = 0x08C23BC0   "TitleArchive/"
  a2 = 0x09FB4A40   path object (source = title/title.arc)
  ra = 0x08820740   inside 0x088206D8
  ```
- Immediately after, `0x08A59378` (the lookup) is called from
  `ra=0x08A59C38` (inside `sub_08A59C10`) with **full virtual paths**:
  ```
  a1 = "TitleArchive/layer_01.ppt"   <-- first TitleArchive request
  a1 = "TitleArchive/layer_02.ppt"
  ```
- Earlier lookups in the same API used other mounts:
  `"system/common.eai"`, `"system/skit.ppt"`, `"game/savedata.bin"`.
- So `0x08A59378` **is** the resolver, and a query carries the mount prefix.
  The earlier "relative entry name" reading was specific to those mounts.
- This also re-interprets the native probe result `find=9` for
  `"TitleArchive/water_02.ppt"` as a real (non-zero) match return, not a
  leftover.

Native gap: the port never reaches `0x08A59C10` → `0x08A59378` with a
`TitleArchive/...` query. The mount itself is valid.

### Fixed: title path now issues and resolves TitleArchive assets (2026-11-18)

The requests were never missing — they were invisible. They go through the
VFS resolve API `0x08A59C10`, not `sceIoOpen`, so `FILE_DIAG` could not see
them. With the title scene started
(`TITLE_TRIGGER` + `TITLE_MOUNT_THREAD` + `HOLD_USER_MAIN` + `SCENE_REG` +
`SCENE_START`), the port issues the full title asset sequence:

```
system/common.ppt, system/skit.ppt, game/savedata.bin,
system/Nowloading.ppt/eai,
title/title.arc,
TitleArchive/layer_01.ppt, layer_02.ppt, bird.ppt, bird.eai, sky.ppt,
water_01.ppt/eai, water_02.ppt/eai, cloud_01.ppt, cloud_02.ppt,
spore.ppt/eai, BNG_for_psp.ppt, NAMCO_for_psp.ppt, alfalogo.ppt,
titlelogo_back.ppt, title.eai, titlelogo.ppt, lisence.ppt
```

Counters (`200M` run): `0x08A59C10 hits=64`, `0x08A583D8 (fallback) hits=0`,
`0x08A59378` executes inline from `0x08A59C10`. So every resolve matched.

Reverse chain proven in PPSSPP and mirrored in the port:

```
0x088206D8 -> 0x0881CDD8(obj) -> 0x0881AB98(obj+240)
           -> 0x0881E5CC(loader) -> 0x08A59C10(manager, "TitleArchive/...")
```

Fix added: `[tow2-open]` logging in the pre-dispatch hook, and when a
resolved path ends in `.arc` the host opens and parses it natively:

```
[tow2-open] path="title/title.arc" ra=0x08A4DABC uid=8
[tow2][ezbind] archive="title/title.arc" version=4 entries=21
```

Remaining gap: the guest still never opens `title.arc` through the I/O HLE
(no `[io] open` line), so entry payloads are not yet served to the guest.
The source object returns from `find` but has no archive index of its own;
the next step is to back it with the host `EzbindArchive` (serve entry
bytes/decompression) instead of relying on the guest parser.

### title.arc is loaded via raw UMD sectors (2026-11-18)

The “title.arc is not loaded” symptom was an I/O-visibility artifact.

- ISO scan: `title.arc` starts at **127,172,608** (sector **62096**).
- Native log at boot:
  ```
  [tow2][archive] raw device load start=127172608 bytes=1370112
                  buf=0x09BF8780 head="EZBIND"
  ```
  669 sectors = 1,370,112 bytes (>= 1,369,296), starting exactly at the
  archive offset, and the buffer begins with `EZBIND`.
- The engine reads archives through the raw `umd1:` device via
  `FileReadingTask` (sector-addressed `sceIoLseek`/`sceIoRead`), **not**
  through `sceIoOpen`/`disc0:`. Watching `sceIoOpen` therefore missed it.
- Consequence: **no custom loader function is required**. The guest already
  reads the archive into guest RAM, and the host also parses it on VFS
  resolve.
- Remaining check: confirm the guest mount source references the loaded
  buffer (`0x09BF8780`) and that entry payloads are served from it.

### Mount source points at the loaded archive (2026-11-18)

Probe at the first `TitleArchive/...` resolve, walking the manager list:

```
[tow2-mount] manager=0x08D4F120 guest_buffer=0x09BF8780 bytes=1370112
[tow2-mount] node=0x08F11D40 name="title/title.arc" src=0x08DA1270
             0x08C703F0 0x09BF8780 0x0014E4D0 0x0014E800
```

Decoded:

| offset | value | meaning |
|--------|-------|---------|
| `src+0`  | `0x08C703F0` | source vtable |
| `src+4`  | `0x09BF8780` | **guest buffer holding `EZBIND`** |
| `src+8`  | `0x0014E4D0` | 1,369,296 = exact `title.arc` size |
| `src+12` | `0x0014E800` | 1,370,112 = 669 sectors read |

And the buffer itself:

```
[tow2][archive] name_table[0]="sky.ppt" entry_data_head=0x08088B1F
```

`0x08088B1F` = bytes `1f 8b 08 08` — the gzip member header of the first
entry (`sky.ppt`).

Conclusion: `title.arc` is loaded, mounted, and its source is wired to the
in-memory EZBIND image with the correct size. No custom loader function is
needed. The earlier “not loaded” symptom was purely I/O-visibility
(`sceIoOpen` vs the raw sector-addressed `umd1:` read).

### End-to-end usage verified (2026-11-18)

Decompressed payload headers, which exist **only after inflating** (the archive
buffer holds gzip bytes), were found in guest RAM at the end of the run:

```
[tow2-ppt] found sky.ppt      at 0x08F13240 0x08F92B80 0x09026840
                                   0x090A6100 0x0913C300 0x09268780
[tow2-ppt] found titlelogo.ppt at 0x093C7200 0x093E5280
[tow2-ppt] found bird.ppt      at 0x090125C0 0x09408000 0x09BD8700
[tow2-ppt] found title.eai     at 0x08F084C0 0x09026680 0x0913C240
                                   0x091D2540 0x092A4CC0 0x092A4DC0
```

Patterns used: `70 70 74 00` + dimensions (`sky` 512x512
`00 02 00 02`, `titlelogo` `00 02 80 00`, `bird` `00 01 80 00`) and
`45 41 49 00` for EAI.

Since the compressed archive contains none of these headers, their presence
proves the game decompressed the entries. Full chain now verified:

```
raw UMD read (title.arc -> 0x09BF8780)
  -> VFS mount TitleArchive/
  -> 0x08A59C10 resolve of every title asset
  -> gzip inflate into guest heap
  -> title objects/textures
```

No custom loader or parser override is required; the port is end-to-end
serving the title archive.

### Port status poster (2026-11-18)

`profiles/tow-2/scripts/make_port_status_image.py` renders
`profiles/tow-2/PORT_STATUS.png` from `analysis/tow2_report.json` plus the
on-disk corpus:

- AOT: **38,213 seeds**, **38,123 generated (99.76%)**, 90 analyzer-truncated
- imports: **416 across 144 libraries** — 32 SDK libraries (284 imports) and
  112 game modules (132 imports, character/asset entries aggregated)
- corpus: 567 AOT units, 99.93 MB; 213,047 relocations
- gates: 5/9 — boot, intro movie, title archive mount, title render and
  idle→intro replay verified; input/menus, character creation, field and
  battle pending

### SleepThreadCB wakeup: vblank-paced pump resume (2026-11-18)

- The module/quest pump `0x08A6C3D8` either waits on `SleepSema`
  (`ra=0x08A70524`, empty queue) or signals `SleepSema` and calls
  `sceKernelSleepThreadCB` (`ra=0x08A70420`, non-empty queue).
- A full-boot semaphore trace shows only two participants: `wait` by
  `user_main` (uid 1) and `signal` by `ModuleDispatcher` (uid 8). No
  `sceKernelWakeupThread` call ever targets uid 1 (372 wakeups, all uid 8),
  and `SleepSema` ends with `value=1 waiters=0` — the phase-1 signal is
  never consumed.
- The EBOOT imports **no** alarm / sub-interrupt / vblank-handler API: the
  registry has only `sceDisplayWaitVblank*`, `sceGeSetCallback`, power and UMD
  callbacks; there is no `sceKernelSetAlarm` or
  `sceKernelRegisterSubIntrHandler`. The only remaining frame-paced source is
  the vblank tick.
- Implemented: `sceKernelSleepThreadCB` called from `0x08A70420` (the pump
  yield) delays one vblank (16683 us) instead of sleeping indefinitely; every
  other `SleepThreadCB` caller keeps true indefinite-sleep semantics.
  `PSPRECOMP_TOW2_SLEEP_CB_INFINITE=1` restores the old behaviour.
- Result: plain boot now runs the title with no diagnostic env — 11,983
  presented frames (the earlier `PSPRECOMP_TOW2_SLEEP_CB_NOW` hack is no
  longer needed).

### Title idle -> intro replay: the cSoundMusic gate (2026-11-18)

The boot flow is a loop (`0x0881C8E4`):

```
do {
    movie scene (0x088206A0 -> title/title.pmf + pump)
    fade
    iVar1 = title scene (0x08818598)
} while (iVar1 == 1);
```

The title scene returns 1 on its idle timeout, which makes the loop replay the
intro movie — this is the idle-triggered replay.

The title's exit path ends with:

```
while (0x08A4AE08([0x08D14F10]) != 0) pump(0x08A6C3D8);
```

- `[0x08D14F10]` is the sound manager; `+0x208` is an embedded **cSoundMusic**
  (constructor `0x08A6E7D8` installs the `"cSoundMusic"` name).
- `0x08A4AE08` calls `0x08A6FC9C`: `([obj+0x24] ^ [obj+0x28]) != 0`, i.e.
  current state vs target state.
- Trace during the white screen:
  `manager=0x08D51A70 state=0x8000 target=0x00000000`, unchanged for 1000+
  frames; the title object polls the same pair every frame
  (`ra=0x0881C6FC`).
- The audio backend is silent/stubbed, so the guest music task can never
  finish the requested stop: the title scene never returns 1, the boot loop
  never re-enters the movie, and the screen stays on the white fade.

Retiring the task (state -> target when `state==0x8000`, `target==0`):

- only two retirements occur, at frames 4166 and 8330;
- the intro movie replays: `sceMpegCreate` count 1 -> 3, `sceMpegAvcDecode`
  resumes at `frame=9836`, 32-bit movie presentations return at ~6000 and
  ~11000;
- captured frames show the cloud movie (6000) and the title again (9000).

The port now retires that music task by default so the idle replay works on a
silent backend; `PSPRECOMP_TOW2_SOUND_WAIT=1` restores the true wait for audio
bring-up.

### Title idle -> white transition (historical note, 2026-11-18)

- Before the sound gate was understood, the post-title screen looked like a
  scene teardown: at stop the quest manager had `scene_impl=0x00000000`,
  `scene_id=0xFFFF`, no file opens and no further MPEG decodes.
- That is the *wait* symptom, not a quest transition: the title's exit is
  blocked inside the `cSoundMusic` drain loop described above.
- The title-trigger configuration
  (`TITLE_TRIGGER` + `TITLE_MOUNT_THREAD` + `HOLD_USER_MAIN`) showed the same
  pre-fix timeline (11,983 presents, title then white). In that configuration
  `user_main` is held at `0x08A63398` and the probe's mount thread (uid 10)
  runs. It was measured before the sound gate was retired and has not been
  re-measured since.

### GE renderer ported; title screen renders (2026-11-18)

- Ported tow-1's software GE stack (`host/ge/*`, `tow2_ge.{hpp,cpp}`) into
  tow-2 and wired `sceGe_user` / `sceDisplay` to it. Display lists now execute
  for real: clears, points/lines/triangles, texture decode and EDRAM writes.
  `sceGeSetCallback` is captured and its signal callback (guest `0x08ABA63C`)
  fires after list syncs.
- **The old scene probe was destructive.** `0x08814628` is the cQuestManager
  scene *teardown*: it dynamic-casts `[manager+0x3BF4]`, calls the scene
  vtable `+4` destructor and clears `+0x3BF0/+0x3BF4`. The companion
  `0x08814AEC` only writes manager fields, and the probe wrote the manager
  pointer itself into `+0x3BF4`. Any run with `PSPRECOMP_TOW2_SCENE_START` /
  `PSPRECOMP_TOW2_SCENE_REG` therefore renders a white screen with no scene.
- **Plain boot reaches and renders the title screen.** Timeline
  (`PSPRECOMP_TOW2_FRAME_DIAG`):
  - frames 0..~2500: opening movie (PSMF), 32-bit buffers at
    `0x04088000` / `0x04110000`;
  - ~3000: title scene starts, RGB565 at `0x04000000` / `0x04044000`;
  - ~3500..5000: full title screen, logo, "PRESS START BUTTON", world-tree
    background with animated birds and copyright line.
  Captured frame: `seq-4000.ppm` / `.png` (480x272).
- Remaining stall: the title's main loop calls `sceKernelSleepThreadCB` from
  `0x08A70420` and is never woken. Without `PSPRECOMP_TOW2_SLEEP_CB_NOW=1` the
  title renders exactly one frame (the pale fade-in frame) and freezes. With
  the diagnostic that turns the sleep into a 1 ms delay, the title animates for
  ~2500 frames, then fades to white (next scene/attract transition).
- The 5-sprite container at `0x08DA4460` (+576, stride 128, vtable
  `0x08C44078`) is the **spore particle player**, not the title background.
  Its draw hook `0x08A5FD30` bails when the sprite texture slot `+68` is zero;
  the slot is the object resolved from `TitleArchive/spore.eai` by `0x0881E5CC`.
- `0x0881B21C` is the title background constructor (layer_01/02, bird + eai,
  sky, water_01/02 + eai, cloud_01/02). It is the object that draws the visible
  title art.

### Input path: title -> character creation (2026-11-18)

- The pad is polled every frame from `0x08A7C484`
  (`sceCtrlReadBufferPositive`, `ra=0x08A7C4A0`, buffer `0x08D04638`); the
  title's Start edge detector `0x08A63704` reads the same buffer and tests
  bit 3 (`0x0008`).
- **Start at the title reaches the menu state machine.** With Start at
  ~10 s guest time the title loop exits, the movie scene is torn down
  (`scene_id -> 0xFFFF`) and the menu opens
  `system/menuback.ppt`, `system/common.eai`, `common/charmake_bk.arc`.
  The next frame is the character-creation class grid; the 15 class icons and
  the message window render (captured at present 2500).
- Title timing: frame 288 = 10.03 s guest time. The title becomes interactive
  after the opening movie; with no input it times out and re-enters the intro
  movie (`0x088206A0`) at ~frame 300. Periodic Start+Cross from boot skips the
  opening movie and reaches the class grid at frame 300.
- Missing imports hit immediately after the transition, now implemented:
  - `UtilsForUser 0x27CC57F0` `sceKernelLibcTime`
  - `UtilsForUser 0x37FB5C42` `sceKernelGetGPI`
  - plus `0x91E4F6A7` `sceKernelLibcClock` and `0x71EC4271`
    `sceKernelLibcGettimeofday` for parity with tow-1.
- New diagnostics:
  - `PSPRECOMP_TOW2_PAD_SCRIPT="t_us:mask[:hold_us],..."` deterministic pad
    events (same syntax as tow-1's `PSPRECOMP_TOW1_PAD_SCRIPT`).
  - `PSPRECOMP_TOW2_CTRL_DIAG=1` logs pad reads with frame, guest time,
    button mask and return address.
- Next stall: the class grid is static under Start, Cross, Down and analog
  input. PPSSPP delivers the same Cross press (pad state `0x08D0463C` reads
  `0x4000`); the PPSSPP visual comparison is weak because its Vulkan
  `PrintWindow` capture is stale.
- **Ghidra build mismatch:** the instance at port 8193 is a different EBOOT
  build than `profiles/tow-2/game/eboot.dec`. The code at `0x08A63984` does
  not match the generated corpus, the charmake UI constants `0x1ED0/0x1ED4`
  are absent from the corpus, and the input-state global `0x08B1F42C` reads
  `0x4A`. Use the generated corpus and runtime traces for tow-2 work; Ghidra
  addresses in the `0x08A63xxx` range are not authoritative.

### Class-grid input trace (2026-11-20)

Added present-time guest tracing in `tow2_hle.cpp`:

- `PSPRECOMP_TOW2_MEM_DIFF="start_us:end_us[:address[:length]]"` snapshots
  guest RAM at one time and prints changed 32-bit words at the later time.
- `PSPRECOMP_TOW2_INPUT_STATE="1|0xADDR"` reads `[global]` and logs the cached
  button word at `+0x20`.
- `PSPRECOMP_TOW2_WATCH32="addr,..."` logs every change of listed words.
- `PSPRECOMP_TOW2_SCAN_WORD_AT="value,..."` + `_AT_US` scans guest RAM for
  values once at a given guest time.
- `PSPRECOMP_TOW2_DUMP_AT="t_us:address:length"` dumps a region once.
- `PSPRECOMP_TOW2_TRACE_PCS="addr,..."` + `_FROM_US` logs dispatches to the
  listed guest PCs.
- `PSPRECOMP_TOW2_TRACE_RET="value,..."` + `_FROM_US` logs functions that
  return one of the listed values, on both the outer and chained call paths.
- `PSPRECOMP_TOW2_TRACE_UID="uid:from_us[:count]"` logs a thread's dispatches
  in order.

Findings:

- **Correction:** the object at `0x08D4F6B0` is a **cCamera**, not an input
  manager. Its `[+0x10]` typeinfo is `0x08C70E10`, whose string is
  `N3sys7cCamera`. `[+0x20] = 0x08D4F6D0` is a camera/debug control word that
  mirrors the pad: Down `0x40` at 12.05 s, +Circle `0x2040` at 13.06 s,
  +Cross `0x6040` at 14.07 s. It is not the class-grid selection state.
- The only `sceCtrlReadBufferPositive` call sites are the title Start check
  `0x08A63704` (buffer `0x08CB1EF0`) and the per-frame camera update
  `0x08A7C484` (buffer `0x08D04638`). The camera update copies the pad to
  `0x08D04624` and feeds `0x08A76610`; no UI code reads those globals
  directly.
- The pump `0x08A6A7FC` is `cRenderManager::update` (typeinfo
  `0x08C70FB8`, string `N3sys14cRenderManager`). It walks a task list at
  `manager+8` (`manager=0x08D4F3D0`, head `0x08D4F3D8`). The head is
  initialised to self at frame 2 and **never changes**: the render-task list
  is empty for the whole run, so the pump returns immediately.
- `PSPRECOMP_TOW2_TRACE_PCS` after 10 s enters only `0x08A6A524` once; the
  camera/vtable menu methods are not the class-grid consumer. A `TRACE_UID`
  trace of uid 8 after the charmake open is dominated by sprite/render calls
  (`0x08A5DD28`, `0x08A772DC`, `0x08A75EC4`, `0x08A5FCE8`) and never reaches
  a class-grid UI constructor or input loop.
- Thread state at the grid: `user_main` (uid 1) is running at the pump yield
  `0x08A70420`, `ModuleDispatcher` (uid 8) is delayed, `FileReadingTask`
  (uid 4) is delayed, and no charmake/input thread exists.
- Conclusion: the class grid is showing the **background** while its UI/task
  was never created. This is an asset/UI-setup gap, not an input-delivery
  failure.

### Charmake setup blocks on load completion (2026-11-20)

- The charmake scene setup is `0x08A27E98` (unit 0273). It runs at
  10.483673 s; its caller return is `0x08A4D2A0`. It opens
  `system/menuback.ppt`, `system/common.eai` and
  `common/charmake_bk.arc`, then continues through `0x08A25004`,
  `0x08A20184` and `0x08A1F2B8` (10.567-10.572 s).
- `common/charmake_bk.arc` is a 2,818,048-byte raw UMD read at
  10.667186 s, issued by uid 4 (`FileReadingTask`, caller `0x08A5A40C`).
- `0x08A1F2B8` opens its asset via `0x08A599C0` (returns the VFS manager
  `0x08D4F120`) and then calls `0x089FB424`.
- `0x089FB424` is a **completion wait loop**: it tags the object field at
  `[this+3168]` (object `0x09FA3190`, field `0x09FA3DF0`) with the loading
  bit, then pumps (`0x08A703D8`) until that bit clears.
- Watch trace: `0x08DA1200 -> 0x08DA1201` at 10.900748 s and **no clear**
  afterwards. The setup never returns to `0x08A1F388`, so the post-loop UI
  creation (`0x08A582B4`, `0x08A4D334`, `0x08A02210`) never runs.
- Root cause: the charmake load's completion callback never clears the
  loading bit. Next target: trace the async completion path for this read
  (the file worker's signal at `0x08A59878`/`0x08A598DC` and the callback
  that should clear `0x09FA3DF0`) and make it fire.
- New diagnostics used:
  - `PSPRECOMP_TOW2_TRACE_RA="ra,..."` logs dispatches whose caller return
    address matches.
  - `PSPRECOMP_TOW2_TRACE_CHAIN_FROM="start:end"` logs chained calls made
    from a return-address range (requires `PSPRECOMP_NO_CHAIN=1` or a
    poisoned unit, because the production direct-chain fast path bypasses
    the chained hooks).
  - `PSPRECOMP_TOW2_CHARMAKE_WAIT_TRACE=1` overrides the charmake wait/open
    helpers (`0x08A703D8`, `0x08A6FEC0`, `0x08A4ADF8`, `0x08A599C0`) to log
    entry/return.
  - `[tow2-open]` and the raw-archive log now include `t_us`, `caller`/`ra`
    and `uid`.

### Charmake wait released; name entry reached (2026-11-20)

- Modeled the charmake load completion: once the 2.8 MB
  `common/charmake_bk.arc` read has landed, the pump wrapper clears the
  loading bit at `[obj+3168]` (`0x09FA3DF0`, `0x08DA1201 -> 0x08DA1200`).
  Enabled by default; `PSPRECOMP_TOW2_CHARMAKE_WAIT=1` keeps the true wait.
- Added the missing `vflush` decoding: the game uses the
  `0x34 / group 1 / op 28` form (`0xD03C8180`), not the decoder's `0x3F`
  form. `vflush` was already a no-op in the codegen. Regenerated the corpus
  with `scripts/generate.bat`.
- Added the OSK HLE: `sceUtilityOskInitStart` (`0xF6269B82`), `Update`
  (`0x4B85C861`), `GetStatus` (`0xF3F76017`), `ShutdownStart`
  (`0x3DFAEBA9`) and `GetSize` (`0x5D5FCB91`). It auto-fills the name with
  `"Radiant"` and completes the dialog.
- Verified flow: the setup continues past the second asset load, the
  class-grid input loop at `0x089F5F50` runs, Circle selects a class, the
  OSK opens and closes, and the run continues to `0x08A790F4` instead of
  stalling. The class-grid UI/task is now created and consumes input.
- Visual confirmation: frames at present 500/1000 show the character
  creation preview (3D character model with sword, class background and UI
  panels), so the charmake UI/task is alive and rendering. Captured to
  `progress/frame_cre.png` (untracked).
- Next frontier: `0x08A790F4` (title/actor update path) after the name
  entry; inspect the post-creation scene transition.

### Font rendering (2026-11-20)

- The character-creation panels rendered but every glyph was blank:
  tow-2's `sceLibFont` HLE returned an 8x8 zeroed glyph and 8x8 metrics.
- Ported tow-1's host font path: GDI `GetGlyphOutlineW` into a `HostGlyph`,
  real `SceFontInfo`/`SceFontCharInfo` metrics (26.6 fixed point), and
  `font_draw_glyph_to_image` for both 4-bit and 8-bit glyph buffers.
- Fixed two host access violations in the GDI path: deselect the old
  `HFONT` before `DeleteObject`, and reject zero-size glyph bitmaps.
- Verified: frames 500/1000 show the full character-creation menu
  ("Rotate Mode", "Sex Male", "Job: Swordsman", "Hair", "Eyes", "Mouth",
  "Skin", "Victory P.", "Voice Type", "Randomize", "OK", "Set character
  name") over the 3D preview. Captured to
  `progress/frame_cre_font.png` (untracked).
- Next: drive the menu cursor to `OK` (D-pad down, then confirm) and check
  the post-creation field transition.

### Character creation -> field (2026-11-20)

- The menu's `OK` opens a "Confirm this character?" dialog whose cursor
  defaults to `No`. Press Up to select `Yes`, then Circle.
- The confirm path calls `0x08AA33C0` (scene set to 0), fades through
  `0x08D4F8D0`, and enters the field.
- Fixed the remaining blockers:
  - Allegrex encodes `msub`/`msubu` at SPECIAL funct `0x2E`/`0x2F`
    (`0x0064002E` at `0x08838330`), not `0x1E`/`0x1F`. Added to the decoder
    and regenerated.
  - `scePower 0xEBD177D6` plus the battery/clock queries are stubbed as a
    healthy AC-powered PSP.
  - Extra seeds `0x0896FEC4` (title task prologue) and `0x08A0DF14` (field
    function prologue) are now set in `scripts/generate.bat`.
- Verified: frames 1000/1500/2000 show the field dialogue with Kanonno and
  Pan ("Ah, Pan... I brought the laundry in.") and correctly rendered text.
  Captured to `progress/frame_field.png` (untracked).
- Next: advance the dialogue and check the next gameplay stall.

### Skit dialogue advance (2026-11-20)

- The field conversation is a **skit** (`system/skit.ppt`), not the standard
  message window. The text lives in the field UI's buffer
  (`0x08DC1810+0x0F8`, length at `+0x10C`) and the skit text window is at
  `fieldUI+0x74` (`0x08DC1884`).
- The field UI text position is `fieldUI+0x11C` (`0x08DC192C`); the reveal
  counter runs to `0x147`, then the text-complete byte
  `fieldUI+0x718` (`0x08DC1F28`) becomes 1.
- The standard advance path (`0x08961814`) only closes the message window at
  `fieldUI+0x704` (`0x08DC1F14`), which the skit leaves at 0. So the text
  position never becomes `-1` and the skit controller stays in state 1.
- Model: once the field UI latches an advance (`+0x12D == 1`) and the skit
  text is fully revealed, retire the text (set `+0x11C = -1`, clear
  `+0x12C`, `+0x12D`, `+0x718`). The controller then runs the next command.
  `PSPRECOMP_TOW2_SKIT_WAIT=1` keeps the true wait.
- Verified: the first line ("Ah, Pan... I brought the laundry in.") advances
  to the portrait-less line "Kanonno: ...!" and the skit portraits close.
  Captured to `progress/frame_skit_next.png` (untracked).
- Each skit line has its own text object, reached through the skit
  controller's `+0x08` pointer (`0x08DA0348`). The first line uses
  `0x08DC1810`; the "…!" line uses `0x08DA1520`. The advance model must
  follow that pointer, not the global field UI. With that, the "…!" line
  also advances.
- The skit completes after the second line: the portraits close, the text
  window disappears, and the game returns to the field (the character
  standing in the blue-tiled room, no HUD). Captured to
  `progress/frame_field_idle.png` (untracked).
- Added `PSPRECOMP_TOW2_PAD_ANALOG="time_us:lx:ly[:hold_us],..."`
  (0x80 center, Y grows downward) to inject the analog stick.
- Next stall: the field is static. D-pad and analog-stick movement do not
  move the character. Trace (2026-11-20):
  - The field module runs on uid 8 (`0x08A12898` -> `0x08A11D20`) and reads
    the D-pad for interaction/menu (`0x08979708`, `0x08977D38`), but the
    player controller never consumes movement.
  - The charmake -> field transition **memsets** the quest/scene manager
    `0x08D9B920` (`0x08AA33C0` is memset, not a scene setter). It ends with
    `scene_impl=0`, `scene_id=0xFFFF`, and an empty actor array.
  - The field object `0x09FA02F0` (map 12) has an empty actor-id list
    (`+0x0C = 0xFFFFFFFF`). Its entity array `0x08F37000` still holds the
    player model `0x08DBC820` and the other entities.
  - The leader/camera `0x09FA1100` is in **event mode**
    (`+0x1151` bit 0 set) and has **no player target** (`+0x1174 = 0`). Its
    update (`0x08A185EC`) takes the event/camera path.
  - The skit controller reaches state `-1` after the second line
    (frame 1138, `0x08DA0350 = 0xFFFFFFFF`), so the skit itself is done.
  - The missing piece is the **field scenario** (the quest manager's
    `scene_impl`). The charmake path bypasses the normal field-scene load,
    so the event-end cleanup (clear event mode, attach the player, spawn the
    actors) never runs.
  - The opening-event trigger is `leader+0x1134` (id 117). Retiring it
    (set `-1`) is safe and is modeled by `PSPRECOMP_TOW2_FIELD_CONTROL=1`.
  - The leader target `leader+0x1174` is **not** the raw model: it must be a
    trigger-compatible actor. Writing `0x08DBC820` there crashes the
    collision check `0x08A801E4` (the model's RTTI is `"USRD"`).
  - The player actor is missing: the scene manager's actor header array
    (`0x08D9B924`) and controller array (`0x08D9BBE8`) are empty. The field
    scenario (`scene_impl`) is what creates them.
  - The normal field-follow state is `leader+0x1151 = 0x0D` (bit 0 set);
    the field path `0x08A185D4` uses the target at `+0x1174`.
  - Next: implement the actor spawn (create the player actor header and
    controller, populate the field actor list at `field+0x0C`) or load the
    field scenario.

## Title scene ordering (2026-11-17)
Goal: start the title mount before `user_main` resumes, then observe
`TitleArchive/...` requests.

- Ordering control: `PSPRECOMP_TOW2_TITLE_MOUNT_THREAD=1` plus
  `PSPRECOMP_TOW2_HOLD_USER_MAIN=1` marks the `user_main` thread Delayed with
  an infinite deadline and removes it from the ready queue. Verified state at
  the dispatch limit:
  `uid=1 user_main state=delayed`, `uid=10 (mount) state=running`.
  No guest fault occurs in this configuration.
- Scheduler fix: semaphore wake order is now **priority-ordered** (highest
  priority first, FIFO within equal priority) for both `sceKernelSignalSema`
  and the vblank model. The previous FIFO wake could resume `user_main`
  (63) ahead of the mount thread (17).
- Result: the mount runs, but **no `TitleArchive/...` asset is requested**.
  `PSPRECOMP_FILE_DIAG=1` shows only `title/title.pmf`; no `.arc` open and no
  `[tow2][ezbind]` line. A 2,000,000,000-dispatch run ends at
  `0x08A5FBCC` with the same behaviour.
- Evidence that `0x088206D8` is a **per-frame title entry**, not a one-shot
  mount:
  - `0x08A59210` (VFS mount) executes a handful of times, then stops.
  - The thread then loops `0x08A5FB98 -> 0x08A5FBBC -> 0x08A5FBCC`, calling
    `0x08A774B0`, the GE draw stub `0x08A76ED8`, and returning through
    `0x08AB8C30`.
  - `0x08A703D8` (queue-lock + signal + sleep) and `0x08A57DB0` (VFS pump)
    each execute tens of thousands of times in that loop.
  - The scene object `[0x08D9B920]` still has `+4 = 0xFFFF` (movie scene), so
    the title state machine never advances.
- `0x088206D8` is referenced from the module's `user_main` descriptor block at
  `0x08C71E00` (`0x4000` stack, `0x28` priority, `"user_main"`), not from a
  scene table. It is the main thread's frame/entry callback.
- Conclusion: the correct ordering is **scene switch first, then the per-frame
  callback**. Running the frame callback in a fresh thread against scene
  `0xFFFF` cannot produce asset requests. Next step: find the scene-change
  path that moves `[scene+4]` off `0xFFFF` and drive that instead.


## Title mount trace: first dead-end in `0x088206D8` (2026-11-17)

- `0x088206D8` has **no unresolvable call**. Executed synchronously it runs
  into the signal/wait pair at `0x08A703D8`:
  `sceKernelSignalSema(0x08AEDCB8)` then `sceKernelSleepThreadCB(0x08AEDD18)`.
  `run_isolated_aot` hits the 200,000-step limit there, because the isolated
  dispatcher never lets another guest thread run. The routine is asynchronous
  and must execute as a guest thread.
- `PSPRECOMP_TOW2_TITLE_MOUNT_THREAD=1` starts it as a real priority-17 thread.
  It reaches the VFS (`0x08A57DB0`, `0x08A59210`) and then signals semaphore
  `0x200`, waking `user_main` (uid 1).
- `user_main` resumes and faults in the game's `free()`:
  - `0x08A633BC` -> `0x08A63D04` -> `0x08A7680C` (`free`) with `a0=0x08D10000`
  - `0x08A7C608` reads the block header at `0x08D0FFC0`, gets `0`, and
    dereferences `[0 + 8]`.
- Heap state at the fault:
  - live heap = `0x08EC7000`, size `0x00E80000`, first block `0x08EC7080`,
    free list head `0x08F0DAC0`;
  - `0x08A7C820` (heap constructor) ran **once**, at boot, with those args;
  - `0x08D0FFC0`..`0x08D10040` is all zero and lies **outside** the live heap.
- So `0x08D10000` is not a block of the current heap: freeing it against the
  live heap is invalid. `PSPRECOMP_TOW2_SKIP_INVALID_FREE=1` installs a
  profile override for `0x08A7680C` that leaks such a block instead of
  faulting; the same alloc/free pair then repeats, so it is not a one-shot
  leak but a stale-state loop.
- Conclusion: the title mount is a handshake with `user_main`, not a
  standalone initialiser. Forcing it from the post-movie
  `sceKernelDeleteThread` callback re-enters `user_main` with stale state.
  It has to run in the game's real scene-transition order.

## Current diagnosis

- Observation: after the missing seeds and a 1 ms `sceKernelSleepThreadCB`
  diagnostic delay, the title task now runs the title-screen loop.
  `0x08A77560` is called from `0x08A780F0` with uid 1/8; the title task is
  doing actor updates, not scene activation.
- Layer: profile HLE title-task pacing and title-screen path.
- Evidence: title-task entry `0x0896FFB8`; uid 10 state `running`,
  pc `0x08A77560`; caller `ra=0x08A780F0`; `0x08970A00`, `0x08814BBC`, and
  `0x08817E68` still have zero hits.
- Scene state: `[0x08D14F2C] = 0x08D9B920`, `[+4] = 0xFFFF`; the scene-active
  byte `[scene+0x3C80]` is forced to 1.
- `0x08970A00` has **no direct caller** in the regenerated corpus, and a
  runtime scan of guest memory at movie cleanup found **no pointer** to it.
  The activation function is orphaned from the current title path.
- `0x08A780F0` / `0x08A77560` are title-screen actor updates, not the loader.
- The title task currently runs the title-screen loop without the activation.
- Direct entry at `0x08970A00` with `v0=scene` crashes at `0x00000000` in
  unit 0018: it is a **mid-function block**, not a valid standalone entry.
  The invalid activation trampoline was removed.
- `0x08970000` is a mid-function block reached by fall-through from
  `0x0896FFFC`, not an entry.
- `0x0896FFB8` is a loop label inside the same function, not the prologue.
  The real title-task function starts **before `0x0896FF00`**.
- `0x08970A00` is a separate block/function with no caller.
- The real prologue is **`0x0896FEC4`** (`addiu sp,-0x50`, saves
  `ra`/`s0`–`s5`); it was added as an extra function seed and the corpus was
  regenerated/rebuilt.
- The native `title_creator_thread_entry` works: uid 10 is **running** at
  `0x08A77560` (title-screen actor update). The prologue executes; the
  count-PC zero hit is because the native entry is not counted.
- `0x08A5F69C` belongs to the stopped/delayed **FileReadingTask** (uid 4),
  not the title task.
- `0x08970A00` remains a separate uncalled function; the title task is in
  the actor-update loop.
- `0x08A780F0` is a **return address**, not a caller; the real actor update
  function starts at **`0x08A780BC`**.
- `0x08A5FE64` is a block inside the larger title-screen setup function.
- `0x08A5FDBC` is **also a mid-function block**; its one-shot caller hook
  never fired, confirming there is no outer dispatch at that address.
- The setup function’s real start is earlier than `0x08A5FDBC`; the current
  title-screen path is actor/layout code, not the archive loader.
- `0x08A5FDBC` was added as an extra function seed and is registered in
  `generated_unit_0301.cpp`; build is current.
- Static references to `0x08A5FD30` are from C++ vtable/typeinfo tables
  (e.g. `0x08C4412C`) with adjacent class strings (`c2c2Sprite`, etc.).
- `0x08A5FD30` is the first method of that scene/actor class; it is not the
  EZBIND/archive loader.
- The vtable addresses (`0x08C4412C`, `0x08C49C3C`, `0x08C6CF54`) have no
  direct generated references; the constructor installs them through
  relocated data pointers.
- `0x08A5FDBC` is a sub-block and has no static reference.
- **`title.arc` is now open and parsed** (see the EZBIND section above).
- Remaining title-path gate: the automatic scene transition still never asks
  the VFS for a `TitleArchive/...` file, so the archive is reached only
  through the `PSPRECOMP_TOW2_EZBIND_PROBE` diagnostic. The scene id stays
  `0xFFFF` and the title creator runs actor updates only.
- Next test: run the title mount in the real scene-transition order (before
  `user_main` resumes), rather than forcing it from the post-movie delete
  callback; then the scene should request `TitleArchive/...` itself.

## HLE surface

- Framework/profile: scheduler, threads, stacks, partition memory, module
  load/start stubs, semaphores, mutexes, event flags, mailboxes, callbacks.
- I/O: `IoFileMgrForUser`; raw UMD sector semantics; extracted `game/root`.
- Media: `sceMpeg` lifecycle/ringbuffer/stream/AU + FFmpeg H.264/Atrac3+;
  `sceDmacMemcpy`; presenter; Win32 input.
- Audio: silent `sceSasCore` with end-flag behavior; silent `sceAudio`.
- Utility/font: savedata stub, system params, `sceLibFont`.
- Real `libfile.prx` corpus and dispatch at `0x08000000`.

## Changes and verification

- Movie-end fixes: ring-buffer `Put` cap; `AvcDecode` EOF returns success;
  `sceKernelDeleteThread` deletes the target UID, not the caller.
- Title trigger is a diagnostic (`PSPRECOMP_TOW2_TITLE_TRIGGER`), not a final
  fix.
- Tests: framework `psprecomp_tests` 1/1 passed at the pinned framework
  revision. Re-run after any framework change.

## Learned patterns

- `sceKernelDeleteThread` must delete the requested UID; completing the
  caller kills `user_main` during movie cleanup.
- The movie task's real end result is `0`; returning `0x80628002` changes the
  cleanup path.
- `sceMpegRingbufferPut` must cap writes to the PMF packet count; 19 extra
  packets prevent the movie-player state-1 transition.
- The title child argument is a pointer/state, not a mode enum.

## Historical material

The original root state section and the full port assessment are preserved
below verbatim.

---

## Legacy root-state section (verbatim)

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

### tow-2 post-movie title blocker (2026-11-07)

- Made the silent SAS `cSoundEffect` 0x15 publication unconditional after a
  grain. Result: `cSoundEffect` now reaches `completed`.
- 600M/700M stop:

```text
Runtime stopped: dispatch limit at 0x08A59638
uid=1 user_main     sleeping at 0x08A70420
uid=4 FileReadingTask running at 0x08A59768
uid=7 cSoundEffect  completed
cOnMediaStream pattern=0
```

- No `title.arc`.
- PPSSPP at the movie end has the same task state:
  `[0x08D0317C] = 0x040CC000`, `[0x08D03198] = 0`.
- The early task path is `0x08804A64`; the title state is `0x08A4B118`, and
  the title-thread creator is `0x08A4B168` (`sceKernelCreateThread` via
  `0x08AA94F0`). Callers of `0x08A4B168` include `0x088138D8`,
  `0x08813ED0`, `0x08813F98`, and `0x0898BF48`. These are not reached in
  the native run.
- Next test: trace/count the title callers and identify the missing module/
  task request that should create the title thread and open `title.arc`.
  PPSSPP breakpoints on `sceIoOpen`/file-helper destabilize the debugger
  socket, so use the stable movie/task breakpoints or static trace.

### tow-2 title counted-PC result (2026-11-07)

`PSPRECOMP_COUNT_PC` at 600M:

```text
0x08A4B118 (title update)          750
0x08A4B0FC (title load helper)       0
0x08A4B168 (title thread creator)    0
0x08813874 (title state)             0
0x08813F00 (title state)             0
0x08A4C818 (movie wrapper)          748
0x08804A64 (task loop)                1
```

The title update runs but returns early on its `+0x4F68` flag. The title
state `0x08813874` is called from `0x08970AE8` (unit 0182), and the title
thread creator is downstream of it. Neither runs.

Next: count/trace `0x08970000`/`0x08970A00` and its caller to identify the
missing post-movie task/module request; then rerun for `title.arc`.

### tow-2 title trigger result (2026-11-07)

- Title object global: `[0x08D14F10] = 0x08D51A70`.
- At movie cleanup the trigger clears `[obj+0x4F68]`, runs the title load
  helper `0x08A4B0FC` (`ok=1`), then starts a native trampoline thread that
  enters the title task `0x08970000` with the title object.
- The title task runs, calls `0x08A4B168`; the title renderer child is
  created as uid 10.
- The child crashes immediately:

```text
Guest memory access outside PSP RAM/EDRAM at 0x00000010
recomp_unit_0313 pc=0x08A76EE8 a0=0x00000005 s0=0x08DA1A70
ra=0x08A5FBC0
```

The title state is reached, but the title renderer child receives `a0=5`
(a mode/index) and treats it as a pointer. `title.arc` is still not opened.

- The trigger is gated by `PSPRECOMP_TOW2_TITLE_TRIGGER` and is a diagnostic,
  not a final fix.
- Next test: inspect `0x08A76EE8` and the title object/task argument contract
  (`a0=5` vs the title object at `0x08DA1A70`), fix the child argument/state,
  then rerun until `title.arc` opens.

---

## Port assessment (verbatim)

# ToW2 port assessment — PSPRecomp

> Local, git-ignored. Written before starting the tow-2 port.
> Reference profile: `profiles/tow-1` (the working Alfasystem title).

## 1. What this project actually is

PSPRecomp is **static ahead-of-time recompilation**, not emulation and not a JIT.

The pipeline is:

```text
decrypted ELF/PRX (Allegrex/MIPS)
        │  psp_analyze
        ▼
functions + relocations + imports + module info
        │  psp_recomp --auto
        ▼
profiles/tow-2/generated/*.cpp          (the whole game, translated to C++)
        │  CMake + psprecomp_core
        ▼
Tow2Native.exe                           (native PC executable)
```

At runtime `Tow2Native` does **not** interpret PSP instructions or emulate a PSP CPU.
It executes the generated C++ and supplies the PSP OS/hardware services through the
profile host: threads, memory, file I/O, GE, audio, MPEG, font, utility, input.

The boundary is deliberate:

| Layer | Owns |
|---|---|
| Framework (`src/`, `include/psprecomp/`, `tools/`) | Allegrex decode, ELF/PRX load + relocation, guest memory, dispatch, scheduler visibility, generic codegen. No game addresses. |
| Profile (`profiles/tow-2/`) | Generated corpus, HLE/NID contracts, bootstrap, GE renderer/interpreter, MPEG/Atrac, presenter, input, patches, tests. |

So "porting ToW2" = generate the AOT corpus + write a tow-2 host that replays the
PSP services the game needs. Decrypting the EBOOT was only input preparation.

## 2. Current tow-2 state (verified)

### Input identity — ready

| Field | Value |
|---|---|
| Disc ID | `ULJS00175` (Japan; English-patched ISO) |
| Input | `profiles/tow-2/game/eboot.dec` |
| Size / SHA-256 | 6,595,314 bytes / `5848ccfaa3291f1dcbef8b04f74de471f5a20387340a8a835c5fb95644f52b80` |
| ELF type / load base | `0xFFA0` / `0x08804000` |
| Entry | `0x08804124` |
| Module / gp | `TOW2_ALFASYSTEM` / `0x08CB5FA0` |
| Relocations | 213,047 — 0 invalid, 0 unsupported |
| Imports | 416 (225 core system + 191 net/runtime-module) |
| Functions | 38,213; 90 truncated; 4.2M unsupported occurrences (mostly data-as-code) |

The ISO also provides `title/title.pmf` (13.66 MB), `title/title.arc` (1.37 MB),
`module/libfont.prx`, `module/libcrypt.prx`, `module/libfile.prx`, `battle_prx/*.prx`,
`puzzle_prx`, and directory-based data (`game/`, `dungeon/`, `model/`, `sound/`, ...).

### Profile — skeleton only

- `CMakeLists.txt` is wired: `tow2_generate` exists; `Tow2Native` is only added when
  both `generated/*.cpp` and `host/*.cpp` exist.
- `scripts/generate.bat`, `build.bat`, `play.bat` exist and point at the right paths.
- `generated/` is **empty**.
- `host/` is **empty**.
- No `tests/`, no profile HLE, no renderer/audio/presenter.

### Framework state — usable, but not pinned

- `out/framework` is configured with `PSPRECOMP_PROFILE=""` and Ninja.
- `psp_recomp.exe` is current (built after the tow-1 codegen changes).
- `psp_analyze.exe` is **older than `src/program_analysis.cpp`**; rebuild before trusting
  seed/count differences.
- The working tree has uncommitted tow-1 bring-up changes:
  `include/psprecomp/decoder.hpp`, `program_analysis.hpp`, `src/decoder.cpp`,
  `program_analysis.cpp`, `tools/codegen_main.cpp`.
  Pin/commit these before generating a corpus so the corpus is reproducible.

## 3. tow-1 reference inventory

| Item | Value |
|---|---|
| Generated corpus | 657 files / 122.5 MB (655 units) |
| Host code | ~465 KB across 14 TUs |
| HLE registrations | 185 handlers / 22 libraries |
| Imports | 252 |
| Functions | 48,179 |
| Status | Boots through archives, module load, savedata gate, `title.pmf` H.264 + Atrac3+; GE frames and presenter verified |
| Current blocker | Post-movie: `cSoundManager` spins because silent SAS never clears the guest sound-busy flag; `title.arc` not opened |

Reusable tow-1 subsystems (as reference, not drop-in): bootstrap, scheduler/thread HLE,
IoFileMgr/UMD sector semantics, partition memory, module stubs, `sceMpeg` + FFmpeg,
ATRAC3+ demux/decode, GE software rasterizer, Win32 presenter, input mapping.

**Do not copy wholesale.** tow-1's HLE contains title-specific addresses and timing
workarounds (e.g. sound flag `0x08E12440`, savedata state gate, module UID behaviour).
Those must be re-derived for tow-2.

## 4. Import overlap (the main leverage)

| Comparison | Count |
|---|---|
| tow-2 imports | 416 |
| tow-1 imports | 252 |
| Shared `library::NID` | **219** |
| tow-2 only | 197 |
| tow-1 only | 33 |

Shared core: ThreadManForUser 33, sceMpeg 25, sceSasCore 25, sceUtility 19,
IoFileMgrForUser 14, sceGe_user 11, sceDisplay 9, sceAudio 9, sceLibFont 8,
ModuleMgrForUser 7, sceUmdUser 5, SysMemUserForUser 5, plus DMA/Stdio/etc.

tow-2-only is dominated by: `sceNetInet` 15, `sceNetApctl` 7, `sceNetAdhoc*` 10,
extra `sceAtrac3plus` 6, and ~150 character/battle/puzzle PRX imports
(`amp*`, `bst*`, `bom*`, `enm*`, `hum*`, `pla*`, player names, `puzzle`).

Implication: **most of the core OS/media HLE logic is reusable in shape**, but the
runtime PRX module surface is much larger than tow-1's.

## 5. Readiness verdict

| Area | State |
|---|---|
| Input identity | ✅ Ready |
| Generator wiring | ✅ Ready |
| Corpus | ❌ Empty |
| Host bootstrap | ❌ Empty |
| tow-2 HLE | ❌ Empty (219 imports have a tow-1 reference shape) |
| Data/archive mapping | ❌ Not mapped |
| GE/first frame | ❌ Not started |
| Media | ❌ Not started |
| Runtime PRX policy | ❌ Undecided |

**Yes, we are ready to start the porting workflow — but at Phase 2, not "run the game".**
The correct next step is corpus generation + a minimal host that reaches first dispatch.
Porting all 416 imports up front is an anti-pattern.

## 6. Main risks / unknowns

1. **Runtime PRX modules (biggest scope risk).** ~150 character/battle/puzzle imports.
   Boot/title probably does not need most; battle does. Decide per phase:
   HLE-stub first, load/recompile selected PRX later, defer the rest.
2. **Inherited SAS/audio blocker.** tow-1's silent SAS never signals completion and
   blocks the post-movie title transition. tow-2 will likely hit the same wall if the
   same stub is copied. Plan a minimal real SAS/audio output earlier, not after the wall.
3. **GE command coverage.** tow-1's `tow1_ge.cpp` interprets a subset
   (VADDR/IADDR, PRIM, BBOX/BJUMP, JUMP/CALL/RET, OFFSET/ORIGIN, matrices, END/FINISH).
   `title.arc` will probably need textures/CLUT/blend paths from the VCS renderer.
4. **Data/archive layout.** tow-1 uses EZBIND archives; tow-2 mixes directories,
   `title.arc`, and PRX modules. UMD sector semantics and `translate_path` must be
   re-verified for tow-2.
5. **Corpus build cost.** Expect a tow-1-sized corpus (~500–650 units, ~100+ MB).
   `/Ob0` and capped `/MP` are already set; clean rebuilds are expensive decisions.
6. **Analyzer/generator drift.** Rebuild `psp_analyze`; use the same framework revision
   for analysis, generation and the host. Commit/pin the tow-1 bring-up diff.
7. **Unsupported-instruction noise.** 4.2M occurrences are not automatically real gaps
   (tow-1 had 12.9M and still booted). Triage only reachable PCs after first dispatch.

## 7. Recommended phase plan

| Phase | Work | Exit gate |
|---|---|---|
| A. Corpus | Rebuild framework tools; pin SHA; run `tow2_generate`; compile units | `generated_units.hpp` + units compile; truncated/unsupported reviewed |
| B. Bootstrap | Adapt tow-1 `main.cpp`; load/relocate; bind module/gp; register generated units | First dispatch, no missing function |
| C. Core HLE | I/O + UMD + partition memory + threads + module stubs (share 219 NIDs) | Archive/file opens; boot proceeds |
| D. First frame | GE interpreter + presenter (reuse tow-1 reference) | Non-black title/movie frame |
| E. Media | `title.pmf` H.264 + ATRAC3+ | Movie plays |
| F. Title transition | Minimal SAS/audio output; clear sound-busy; open `title.arc` | Title screen |
| G. Gameplay | Input, savedata, runtime PRX policy, stability | N minutes stable |

## 8. Decisions needed before Phase A

1. **Runtime PRX strategy:** defer, HLE-stub, or recompile selected battle/puzzle modules?
2. **HLE sharing:** copy tow-1's host and adapt (fast, profile-local) vs extract a shared
   Alfasystem host library (cleaner, but a framework/profile-boundary decision).
3. **Target milestone:** first frame / title screen first, or gameplay?
4. **Pin the framework:** commit the current tow-1 bring-up changes before generating?

## 9. Immediate next action

```bat
REM 1. rebuild framework tools so analysis and codegen match
cmake --build out\framework

REM 2. regenerate tow-2 analysis with the rebuilt analyzer
out\framework\psp_analyze.exe profiles\tow-2\game\eboot.dec ^
  profiles\tow-2\analysis\tow2_report.json 0x08804000

REM 3. generate the AOT corpus
out\framework\psp_recomp.exe profiles\tow-2\game\eboot.dec ^
  --auto profiles\tow-2\generated 0x08804000 0x2000
```

Then add `profiles/tow-2/host/main.cpp` and the first HLE scaffold, and go for
first dispatch before touching media or gameplay.

## 10. Phase A result — complete

Date: 2026-11-06

- Framework rebuilt from the current working tree; `psprecomp_tests` 1/1 passed.
- `psp_analyze` regenerated the tow-2 analysis with the rebuilt tool.
- Corpus generated by `profiles/tow-2/scripts/generate.bat`:
  - 567 units / 100.3 MB / 416 import wrappers
  - 791,167 emitted code PCs, 222,433 registered blocks
  - `auto_codegen_report.json`: 567/567 translation units rewritten
- Added a profile-local `Tow2Corpus` object-library target (corpus-only compile gate).
- `Tow2Corpus` compiled 569/569 build steps; 568 objects (567 units + registry).
  Only benign `D9025` warnings (`/Ob2` overridden by `/Ob0`).
- Pinned the framework diff:
  `profiles/tow-2/progress/framework-tow1-bringup.patch`
  SHA-256 `147cd0c97d66be8e113f33ccf0949e075968b272f9a7688513895abcc68d7548`
  at HEAD `2c2aadc`.

**Phase A gate: passed.** The corpus exists, is reproducible, and compiles.

Next: Phase B — add `profiles/tow-2/host/main.cpp` (load/relocate, module/gp,
register generated units) and a minimal HLE scaffold, then reach first dispatch.

## 11. Phase B result — complete

Date: 2026-11-06

- Added `profiles/tow-2/host/main.cpp` (load/relocate, module/gp, arena, module
  stack, generated-unit registration) and `tow2_hle.{h,cpp}`.
- Added the `Tow2Corpus` object library to `Tow2Native`, so generated units are
  compiled once and reused.
- `Tow2Native.exe` builds and reaches **first dispatch**:
  - entry `0x08804124`, module `TOW2_ALFASYSTEM`, gp `0x08CB5FA0`
  - arena `0x08D15000`, 222,433 functions registered
  - relocations 213,047 (0 invalid/unsupported)
- Boot-triage chain observed so far:
  1. `SysMemUserForUser::0xEBD5C3E6` — `sceKernelSetCompiledSdkVersion395`
     (implemented, returns 0)
  2. `SysMemUserForUser::0xF77D77CB` — `sceKernelSetCompilerVersion`
     (implemented, returns 0)
  3. `ThreadManForUser::0x446D8DE6` — `sceKernelCreateThread` — **next blocker**

**Phase B gate: passed.** Guest code executes; the first missing import is a
thread contract, not a missing generated function.

Next: Phase C — thread scheduler (create/start/exit/delay/wait, stacks,
contexts) and partition memory, adapted from the tow-1 reference.

## 12. Phase C result — complete

Date: 2026-11-06

- Extracted the English ISO into `profiles/tow-2/game/root`:
  10,816 files / 794.6 MB (`PSP_GAME`, `UMD_DATA.BIN`, USRDIR data).
- Ported the scheduler and kernel objects into `tow2_hle.cpp`:
  - threads: create/start/exit/delete, stacks, contexts, priorities, delay,
    wait-thread-end, sleep/wakeup
  - semaphores, mutexes, event flags, mailboxes, callbacks
  - partition memory + raw block allocator (`AllocMemoryBlock` /
    `GetMemoryBlockAddr` / `FreeMemoryBlock`)
  - UMD-ready, power callback, controller sampling, boot-level GE/display
- Boot chain now reaches:
  - `module_start` (uid 0, priority 32) → `user_main` (uid 1, priority 40)
    → `cModuleManager` (uid 2, priority 17)
  - `UserSbrk` partition 0x32000 @ `0x08D15000`
  - `MainMemory` 16 MB @ `0x08D47000`
  - `AbortCallBack`, `PowerCallBack`, `vd power callback`,
    `SceGuSignal` event flag
  - `sceDisplaySetMode` / `SetFrameBuf` / vblank, GE lists accepted
- Next blocker: `ModuleMgrForUser::0x977DE386` (`sceKernelLoadModule`).

**Phase C gate: passed.** Multi-threaded guest execution is stable and the
title reaches its module manager. GE lists are accepted but **not rasterized**
yet; that remains a later rendering phase.

## 13. Phase D result — complete

Date: 2026-11-06

- Created `profiles/tow-2/game/tow2.iso` as a hardlink to the English ISO, so
  raw UMD device paths resolve without duplicating the 1.06 GB image.
- Implemented `IoFileMgrForUser`:
  - `sceIoOpen`, `OpenAsync`, `Close`, `Read`, `ReadAsync`, `WaitAsyncCB`,
    `Lseek`, `Getstat`, `Dopen`, `Dread`, `Dclose`, `Devctl`, `Ioctl`,
    `Write`, `Rename`
  - normal files use byte counts; raw `umd0:`/`umd1:` uses 2048-byte sectors
  - paths resolve through `Runtime::translate_path` into `game/root`
- Implemented `ModuleMgrForUser`:
  - `sceKernelLoadModule`, `StartModule`, `StopModule`, `UnloadModule`,
    `StopUnloadSelfModuleWithStatus`, `GetModuleId`, `GetModuleIdByAddress`
  - observed loads: `libfont.prx`, `libfile.prx` (stubbed UIDs)
- Added supporting boot contracts: `sceLibFont`, `sceUtility` system params +
  non-interactive savedata, `sceSuspendForUser` power/volatile memory, and
  silent `sceAudio` channel bookkeeping.
- Boot now creates and runs:
  - `FileReadingTask` (uid 4, priority 17)
  - `cSoundMusic` / `cSoundVoice` / `cSoundEffect` (uid 5–7, priority 16)
  - `cOnMediaStream`, `cSoundMusic`, `cSoundVoice`, `cSoundEffect` event flags
- Next blocker: `sceSasCore::0x42778A9F` (`__sceSasInit`).

**Phase D gate: passed.** File I/O and module loading are wired. The title has
not opened an archive yet, so the I/O path is implemented but not yet exercised
by this run; it will be on the first `sceIoOpen` of the data archives.

Next: Phase E — silent/real `sceSasCore`, then title media/archive loading.

## 14. Phase E (in progress) — SAS + real libfile module

Date: 2026-11-06

- Implemented silent `sceSasCore`:
  - `__sceSasInit`, `__sceSasCore`, `__sceSasCoreWithMix`, grain/output-mode
    queries, end/pause/envelope queries, and the voice/ADSR/volume setters.
- Discovered `libfile.prx` is a **plain ELF/PRX** (only `libfont.prx` is `~SCE`).
  It is the title's path-hash/file-index module, not a normal SDK library.
- Generated a secondary AOT corpus for it:
  - `profiles/tow-2/generated_libfile` (1 unit, 10 functions)
  - generated with load base `0x08000000`, symbols prefixed `libfile_` to avoid
    collisions with the main EBOOT corpus
- Added `Tow2ModuleCorpus` to the profile build and linked it into `Tow2Native`.
- `libfile.prx` is loaded/relocated at `0x08000000`; `libfile::0xD8E69C7D` now
  trampolines into the module's real export at `base+0x10` with the module's
  gp/stack via `Runtime::invoke_isolated_aot`.
- Result: the file-reading path is alive; the title creates
  `UMD_SAMPLE_IO_SEMA` and reaches `sceMpegInit`.

**Next blocker:** `sceMpeg::0x682A619B` (`sceMpegInit`). The next step is the
`sceMpeg`/`sceAtrac3plus` + FFmpeg media port (tow-1 `tow1_mpeg.cpp` /
`tow1_atrac.cpp` as reference).

## 15. Phase E — FFmpeg media decode

Date: 2026-11-06

- Ported `tow1_mpeg.{hpp,cpp}` and `tow1_atrac.{hpp,cpp}` to
  `tow2_mpeg.*` / `tow2_atrac.*` (namespace `tow2`).
- Linked the shared FFmpeg build into `Tow2Native` (auto-detected WinGet
  LGPL build; DLLs copied next to the executable).
- Ported the tow-1 MPEG HLE surface:
  - `sceMpegInit`, ringbuffer construct/destruct/put/available-size,
    `sceMpegCreate`, query stream offset/size, regist/unregist stream,
    `malloc/free` ES buffer, `InitAu`, `GetAvcAu`, `GetAtracAu`,
    `AvcDecode`, `AvcDecodeDetail`, `AvcDecodeStop`, `AvcDecodeMode`,
    `AvcDecodeFlush`, `QueryAtracEsSize`, `AtracDecode`, `Delete`, `Finish`,
    `FlushAllStream`
  - `sceDmacMemcpy` for decoded-frame copies into VRAM
  - `StdioForUser` stdout/stderr/stdin handles
- Verified against the real game data:
  - `title.pmf` opens at
    `game/root/PSP_GAME/USRDIR/title/title.pmf`
  - H.264 frames decode (480x272) and are written into the guest video
    buffers; the game DMA-copies them to `0x04088000` / `0x04110000`
  - the private Atrac stream decodes **2,042** frames and feeds 8192-byte
    PCM buffers through `sceMpegAtracDecode`
- The movie reaches video EOF. The post-movie loop then spins on
  `sceMpegRingbufferAvailableSize` and `sceDisplayGetVcount`; the
  `cSoundMusic` / `cSoundVoice` event flags still have waiters. This is the
  same class of post-movie sound/transition blocker as tow-1.

**Presentation caveat:** there is no tow-2 window/presenter yet. Frames are
decoded and copied into VRAM, but the GE/display handlers are still boot-level
stubs, so nothing is shown on screen.

**Next:** post-movie sound/event transition + GE interpreter/presenter.

## 16. Phase E — presenter + visible opening movie

Date: 2026-11-06

- Ported `tow1_window.{hpp,cpp}` to `tow2_window.*`:
  - Win32 GDI window, 480x272, integer scale
  - framebuffer formats 0/1/3 (16-bit and ABGR8888)
  - keyboard → PSP pad mapping
  - frame diagnostics and PPM dump support
- Hooked `sceDisplaySetFrameBuf` to `window_present()`, and
  `sceCtrlReadBufferPositive` to `window_buttons()`.
- Added `window_start()` / `window_shutdown()` around `runtime.run()`.
- Because the GE blit is still a stub, the movie buffer is presented directly
  after `sceDmacMemcpy` when the destination is `0x04088000` / `0x04110000`
  (512x272 ABGR8888). This is a presentation bridge, not the final GE path.
- Verified with a real frame dump: present #374 is a recognizable 480x272
  opening-movie frame (blue sky/ocean). The live window shows the movie.

Environment knobs:

```text
PSPRECOMP_TOW2_NO_WINDOW=1
PSPRECOMP_TOW2_SCALE=N
PSPRECOMP_TOW2_FRAME_DIAG=1
PSPRECOMP_TOW2_FRAME_DUMP=path.ppm
PSPRECOMP_TOW2_FRAME_DUMP_AT=N
PSPRECOMP_TOW2_FRAME_DUMP_ALL=1
PSPRECOMP_TOW2_PRESS_START=1
```

Remaining blockers:
- post-movie `cSoundMusic` / `cSoundVoice` / ringbuffer loop
- GE interpreter for the title/menus (the direct movie present is temporary)

## 17. Post-movie transition + SAS completion

Date: 2026-11-06

Confirmed the 500M-dispatch pre-fix run: it stayed in the `cSoundEffect`
audio-block loop (`delay` / `audio-block` cycling) until stopped. It never
reached the title.

Fixes applied:

- **Ringbuffer accounting**: `sceMpegRingbufferPut` now adds only `num`
  packets, and `GetAvcAu` / `GetAtracAu` consume one packet each.
- **MPEG stream-end**: `GetAvcAu` returns `SCE_MPEG_ERROR_NO_DATA`
  (`0x80618001`) at EOF; `AvcDecode` returns
  `SCE_MPEG_ERROR_AVC_DECODE_FATAL` (`0x80628002`). This is the non-zero
  return the movie player needs to leave its decode loop.
- **SAS completion signaling**:
  - `SasState::end_flag` is cleared by `__sceSasSetKeyOn` (`0x76F01ACA`) and
    set to `0xFFFFFFFF` after a rendered grain.
  - `__sceSasGetEndFlag` returns it.
  - Post-movie `sas_output` publishes `cSoundEffect` bits `0x15`.
  - `AvcDecode` EOF publishes `cOnMediaStream` bits `0x0F`.

Result:

```text
user_main    completed
soundThread  completed
cSoundEffect completed
```

The sound manager is no longer stuck. `cSoundMusic` / `cSoundVoice` remain
idle waiters and are not blocking.

**New blocker:** `FileReadingTask` is now running and polling
`cOnMediaStream` with bits `0x0F`, but the flag is left at `0x10` after the
first poll consumes the published bits. No `title.arc` open is issued yet.

A gated probe (`PSPRECOMP_TOW2_FORCE_MEDIA_EVENTS`) that re-publishes the
media event on every poll caused a **776k `sceIoOpen`/`lseek`/`close` storm**
against `umd1:`/`tow2.iso`; that is not a fix. The real media-event/title
request source must be modeled instead of spamming the flag.

Diagnostics added (gated): `PSPRECOMP_TOW2_SOUND_DIAG` logs the post-movie
event-flag wait/poll/signal sequence.

## 18. `cOnMediaStream` handshake trace

Date: 2026-11-06

Exact handshake, verified from the post-movie logs and disassembly:

- `FileReadingTask` (uid 4) owns the flag.
- It sets bit `0x10` through `sceKernelSetEventFlag` at return addresses
  `0x08A5AF40`, `0x08A5AF90`, `0x08A5B0E0`.
- It polls bits `0x0F` through `sceKernelPollEventFlag` at
  `0x08A5ACE4` (helper `0x08A5ACD0`), with mode `0x21` (AND + CLEAR).
- The consumer that must publish `0x0F` is the movie/main thread. At video
  EOF `user_main` completes (`pc=0x08A63384`), so nobody publishes `0x0F`
  for the file task's leftover request.
- Resetting the flag to `0x0F` once does not help: the file task re-arms
  bit `0x10` and waits again.
- Re-publishing `0x0F` on every poll makes the poll succeed, but triggers a
  **776k `sceIoOpen` / `sceIoLseek` / `sceIoClose` storm** against
  `umd1:`/`tow2.iso`. It is a probe, not a fix.

**Root-cause hypothesis:** `user_main` exits after the movie instead of
continuing into the title-loader/consumer path (or creating the thread that
consumes the media stream). The file task is left holding a pending request.
The next trace is the `user_main` return path after the movie player and
whether the fatal `AvcDecode` EOF return is causing an early exit.

## 19. `user_main` post-movie trace

Date: 2026-11-06

Call site and branching (`0x08A63000` movie wrapper):

```text
0x08A632A0  jal 0x08A638F0          ; movie player
0x08A632A8  sw v0, 0x2C(fp)         ; result
0x08A632B0  bne v0, 0, 0x08A632EC
  result == 0  -> WaitThreadEnd (0x08AEDC78) -> 0x08A63358
  result  > 0  -> WaitThreadEnd + sceMpegFlushAllStream (0x08AEE380)
                  + clear 0x08CB1F00 -> 0x08A63358
  result  < 0  -> direct to 0x08A63358 (bltz at 0x08A632F0)
```

All three paths converge at `0x08A63358`. From `0x08A63384` onward the
function runs the full cleanup: `sceMpegFreeAvcEsBuf`, `UnRegistStream` x2,
`sceMpegDelete`, `RingbufferDestruct`, `sceMpegFinish`, a final call to
`0x08A65064`, then returns at `0x08A635AC`.

**Conclusion:** the EOF return does **not** cause an immediate early exit
inside the movie wrapper. However, the earlier `user_main` completion was
**not** normal: it was caused by the `sceKernelDeleteThread` HLE bug
documented in §20. The **negative** EOF codes also take the `result < 0`
path, which skips `WaitThreadEnd` and the `FlushAllStream` / `0x08CB1F00`
status clear. That leaves the media/file consumer state inconsistent
(`FileReadingTask` still waiting on `cOnMediaStream` 0x0F).

See §20 for the fixed delete-thread behavior and the media experiments.

## 20. `sceKernelDeleteThread` fix and media EOF experiments

Date: 2026-11-06

### Fixed: `sceKernelDeleteThread` completed the caller

The HLE for `0x9FA03CD3` called `complete_current_thread`. The movie cleanup
deletes the sound thread at `0x08A63390` (`a0 = [0x08CB1F0C]`, uid 9), so the
old HLE completed `user_main` at `0x08A63384` instead of deleting uid 9.
`delete_thread()` now removes the target uid, removes it from the ready
queue, serves its end-waiters, and returns 0. If the target is the current
thread, the old exit behavior is preserved.

Verified:

```text
[tow2] delete thread uid=9 name="soundThread"
```

`user_main` resumes after the delete and reaches the end of the movie task;
it then sleeps in the idle loop at `0x08A70420`. `title.pmf` is opened, but
no `title.arc` yet.

### Confirmed movie result

Probe at the wrapper's delete call:

```text
[tow2] movie result=0x80628002 fp=0x09FEF8D0
```

That is the negative `AvcDecode` EOF code. The wrapper takes the
`result < 0` path and skips both `WaitThreadEnd` and
`FlushAllStream` / the `0x08CB1F00` clear. The file task's final `0x10`
request is never consumed.

### Media experiments (all negative)

- **One-shot `0x0F` acknowledgement** when the file task arms `0x10` after
  host EOF: the poll succeeds, then the file task arms `0x10` again and
  polls forever.
- **Drain mode** (ack every armed `0x10`): 75,000+ acks in 30M dispatches,
  still no title load. Draining is not the fix.
- **Positive EOF** (`AvcDecode` returns 1): the movie player retry-spins at
  `0x08A72430`; the `> 0` wrapper path is not the EOF path. Reverted.

### Next test

Trace the caller of the movie wrapper `0x08A63000` (movie task around
`0x08A62A78`) and the guest cleanup/cancel path for the `FileReadingTask`'s
media request. The worker must be cancelled/stopped when the movie ends;
neither acking nor flushing stops it. Identify the request object's stop
signal (or the caller's error-path handling) and model it in the HLE.

## 21. Movie-end producer trace (2026-11-07)

- The movie task entry is `0x08A62D18`; `PSPRECOMP_TRACE_PC` shows
  `ra=0x08A4C848` and `a0` = `disc0:/PSP_GAME/USRDIR/title/title.pmf`.
  The caller is a one-shot wrapper at `0x08A4C818`; it calls the movie task
  and returns without cancelling the file worker.
- The same entry constructs the MPEG ring buffer:
  `ring=0x09FEF90C packets=960 data=0x09428380 cb=0x08A62C20 arg=0x09FEFA14`.
  `arg` is the movie task's `fp+0x144` stream object. `0x08A62C20` is a
  copy/read helper, not the file-task stop.
- The file worker's stream object is `0x08D4F364`: `+0x10=0x501`
  (`cOnMediaStream`), `+0x14=0x80020323`, other fields zero.
- Only three guest `cOnMediaStream` sets occur in the whole run: `0x10` at
  `ra=0x08A5B0E0`, `0x08A5AF40`, `0x08A5AF90`, all in `FileReadingTask`.
  No guest code ever publishes `0x0F`.
- At host video EOF: `packets=960 read=2854 written=6688 used=0`. The worker
  has already written the whole file; the final `0x10` is a completion
  request, not a missing packet.
- Ack experiments are falsified: one-shot `0x0F` is consumed and the worker
  re-arms; per-frame/per-consume `0x0F` does not stop it; drain mode reaches
  75k+ acks and loops. The HLE is bypassing the guest ring-buffer/read-pointer
  protocol, so the worker never reaches EOF cleanly.
- Next test: PPSSPP A/B at the movie end (`0x08A5AEC0`/`0x08A5ACE4`,
  `0x08A62C20`, `0x08D4F364`) to capture the real `0x0F` producer, callback
  arguments, and worker stop condition. Then model that state in the HLE.

## 22. PPSSPP tow-2 A/B — real movie contract (2026-11-07)

PPSSPP `ULJS00175` v1.01, debugger port `52814`, instance 2.

Real movie result at the cleanup breakpoint `0x08A63384`:

```text
[fp+0x2C]  = 0x00000000
[fp+0x30]  = 0x00010000
[fp+0x164] = 0x08F0E800
```

This is the key divergence. The native HLE returned
`0x80628002` (`AvcDecode` fatal) from the same path. The real task ends with
result 0 and enters the caller's post-movie path.

Real media objects and flags:

- `cOnMediaStream` UID is `0x121` in PPSSPP (dynamic; native HLE uses
  `0x501`). The flag is addressed by name in the HLE, so the UID difference
  is expected.
- PPSSPP stream object `0x08D4F3E4` (native `0x08D4F364 + 0x80`):
  `+0x10 = 0x121`, `+0x14 = 0x80020323`.
- Post-cleanup consumer publishes `0x121` bits `0x01`, `0x02`, `0x04`, and
  `0x10`; it **never publishes `0x0F`**. The worker polls `0x0F` as a mask,
  so the individual bits are the real acknowledgement.
- During the movie, `0x136`/`0x08` is set 2,848 times; that is a different
  flag (control/vblank), not the media stream.

Native WIP after matching the real EOF success:

- `AvcDecode` EOF returns 0 instead of `0x80628002`.
- `AtracPlayer::at_end()` added; `GetAtracAu` returns `0x80618001` when video
  EOF or PCM exhausted.
- 60M and 120M runs now stop at `0x08A63B54` in the movie player, with
  `cOnMediaStream = 0x0F` and `FileReadingTask` delayed. The file-worker
  stall is gone; the movie player still spins in its AU-fetch loop.

Next test: capture the real `GetAvcAu`/`GetAtracAu` return at the end
(`0x08A63B54` after GetAvcAu, `0x08A63B10` after GetAtracAu) and make the
guest-side `GetAvcAu` return `NO_DATA` at the true guest stream end.

A/B scripts: `profiles/tow-2/progress/ppsspp_tow2_ab.mjs` and
`ppsspp_tow2_result.mjs`.

### AU-return A/B result

PPSSPP capture at the movie-player AU return sites:

```text
0x08A63B54 (after sceMpegGetAvcAu): last returns all 0x00000000
0x08A63B10 (after sceMpegGetAtracAu): last returns 0x00000000 / 0x00000001
```

Neither returns `NO_DATA` (`0x80618001`) at the end. The native `used == 0`,
host-EOF, and PMF-completion AU gates were therefore wrong and have been
reverted. The verified `AvcDecode` EOF return `0` is kept.

Current native state after the revert:

```text
60M: stop at 0x08A63B54
user_main: running at 0x08AEE260
FileReadingTask: delayed at 0x08A59768
```

The old file-worker `cOnMediaStream` 0x10 polling stall is gone. The remaining
divergence is in the movie-player state machine/exit. Next test: capture the
`0x08A72550` return at `0x08A63BB8` and the movie-player counters
(`fp+0x18`, `fp+0x1C`, `fp+0x30`) in PPSSPP at the real cleanup, then compare
with the native loop.

## 23. 500M/520M stop-state diagnosis (2026-11-07)

`PSPRECOMP_TOW2_MOVIE_STOP=1` reports:

```text
video_eof=1 decoded_frames=2847 vcount=6434
fp+0x18=0x00D10000 +0x1C=0x00001921 +0x30=0x00D06000 +0x34=0
ring: packets=960 read=2854 written=6688 used=0
```

PPSSPP at the real exit path `0x08A63BB8`:

```text
v0=0
fp+0x18=0x00D06000 +0x1C=0x00001977 +0x30=0x00D06000 +0x34=1
```

The movie player copies `fp+0` to `fp+0x34` at `0x08A63A04`. State `1` is
the exit state; the transition at `0x08A63A44` requires
`fp+0x18 == fp+0x30`.

Native: `fp+0x18 - fp+0x30 = 0xA000` (20 packets).
PPSSPP: equality.

The worker wrote 6688 ring-buffer packets, but `title.pmf` is 6669 packets —
19 extra. That matches the ~20-packet overshoot. The equality never fires,
the movie player never enters state 1 / sets `+0x34=1`, and the AU-fetch/
vblank loop continues.

Next test: align or cap the ring-buffer packet accounting to the PMF packet
count (exclude the 19 extra packets), or correct the guest `fp+0x18` end
increment. Then rerun 500M and expect state 1, `[fp+0x2C]=0`, the post-movie
consumer bits, and `title.arc`.

## 24. PMF Put cap — movie end fixed (2026-11-07)

Implemented in `sceMpegRingbufferPut`:

- cumulative ring-buffer writes cap at
  `g_pmf_packet_count - 1` (the first 2048-byte PMF packet is the PSMF header);
- `sceMpegRingbufferPut` returns the actual remaining count;
- `g_pmf_packet_count` is set from the `title.pmf` file size at open.

500M result:

```text
[tow2] movie result=0x00000000 fp=0x09FEF8D0
```

The movie task returns **0**, matching PPSSPP. The movie-player counters are
equal (`+18=+30=0xD06000`), state 1/cleanup is reached, and the old
`FileReadingTask` stall is gone.

600M stop:

```text
Runtime stopped: dispatch limit at 0x08AC0C40
uid=1 user_main     sleeping at 0x08A70420
uid=4 FileReadingTask delayed
uid=7 cSoundEffect  running at 0x08ABDDD8
cOnMediaStream pattern=0
cSoundEffect pattern=0
```

No `title.arc` yet. The post-movie/title transition is the active work.

Next test: extend the post-movie run and inspect `cSoundEffect`/SAS completion.
The silent SAS completion signal is gated on `g_mpeg_video_eof`; after movie
cleanup that flag is reset, so title sound effects may never publish
`cSoundEffect` completion. Test signaling `cSoundEffect` 0x15 unconditionally
after a grain, or identify the post-movie title requester.

## 25. Post-movie title blocker (2026-11-07)

SAS completion was made unconditional after a grain. `cSoundEffect` now
completes, and the 600M/700M stop is:

```text
Runtime stopped: dispatch limit at 0x08A59638
user_main     sleeping at 0x08A70420
FileReadingTask running at 0x08A59768
cSoundEffect  completed
cOnMediaStream pattern=0
```

No `title.arc` yet.

PPSSPP at the movie end has the same task state:
`[0x08D0317C] = 0x040CC000`, `[0x08D03198] = 0`.

The early task path is `0x08804A64`. The title state is `0x08A4B118`; the
title-thread creator is `0x08A4B168` (calls `0x08AA94F0`, which creates a
thread). Its callers include `0x088138D8`, `0x08813ED0`, `0x08813F98`, and
`0x0898BF48`. None are reached in the native run.

Next test: trace/count the title callers and identify the missing module/task
request that should create the title thread and open `title.arc`. PPSSPP
breakpoints on `sceIoOpen`/the file helper destabilize the debugger socket;
use the stable movie/task breakpoints or static traces.

### Counted-PC result

600M with `PSPRECOMP_COUNT_PC`:

```text
0x08A4B118 (title update)          750
0x08A4B0FC (title load helper)       0
0x08A4B168 (title thread creator)    0
0x08813874 (title state)             0
0x08813F00 (title state)             0
0x08A4C818 (movie wrapper)          748
0x08804A64 (task loop)                1
```

The title update runs 750 times but returns early on its `+0x4F68` flag. The
title state `0x08813874` is called from `0x08970AE8` (unit 0182); it and the
title-thread creator never run. The missing request is upstream of
`0x08970A00`.

Next: count/trace `0x08970000`/`0x08970A00` and its caller, identify the
post-movie task/module request, then rerun for `title.arc`.

### Title trigger result

- `[0x08D14F10] = 0x08D51A70` is the title object.
- At movie cleanup the trigger clears `[obj+0x4F68]`, invokes the title load
  helper `0x08A4B0FC` (`ok=1`), then starts a native trampoline thread into
  the title task `0x08970000` with that object.
- The title task runs and calls `0x08A4B168`; the title renderer child is
  created as uid 10.
- The child crashes:

```text
Guest memory access outside PSP RAM/EDRAM at 0x00000010
recomp_unit_0313 pc=0x08A76EE8 a0=0x00000005 s0=0x08DA1A70
ra=0x08A5FBC0
```

The title state is reached, but the title renderer child gets `a0=5` and
dereferences it as a pointer. `title.arc` is still not opened.

- Trigger gate: `PSPRECOMP_TOW2_TITLE_TRIGGER` (diagnostic only, not a fix).
- Next test: inspect `0x08A76EE8` and the title object/task argument contract
  (`a0=5` vs the object at `0x08DA1A70`), fix the child argument/state, then
  rerun until `title.arc` opens.
