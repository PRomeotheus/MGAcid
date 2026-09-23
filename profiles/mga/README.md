# Metal Gear Ac!d profile (ULUS-10006)

A static recompilation of **Metal Gear Ac!d** (USA, `ULUS-10006`) on the
PSPRecomp framework, built from Yakumo's MHP3rd host. The target platforms are
Windows and Linux (Steam Deck).

Nothing from the game is in this repository. You need your own disc image.

## Status: early bring-up

- The executable (`PSP_GAME/SYSDIR/BOOT.BIN`, a plain ELF identical to
  `USRDIR/program.prx`) is recompiled: 5,108 functions in 76 units.
- The game's own PRX modules (`KCEJ_FS`, `KCEJ_SOUND`, `ZLIB`, every stage)
  are loaded, relocated and linked for real by `host/kernel/module_loader.cpp`,
  and run through the interpreter until they get recompiled corpora.
- Sony's libraries on the disc (`sce*` module names) stay on the host HLE.

The host code under `host/` started as a mechanical copy of
`profiles/mhp3rd/host` (namespace `mhp3rd` → `mga`, `MHP3RD_*` → `MGA_*`).
Much of it is still MHP3rd-specific (save data, fonts, ad hoc, the in-game
menu texts) and is adapted as the game needs it. `profiles/mhp3rd/README.md`
documents the host features and diagnostics it inherited; the environment
variables there work here with the `MGA_` prefix.

## Build

Same toolchain as Yakumo: see [`docs/BUILDING.md`](../../docs/BUILDING.md).
On Windows, run these in Git Bash started from the x64 Native Tools prompt.

```bash
# 1. Configure and build the bootstrap
cmake -S . -B out/mga -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mga \
      -DCMAKE_PREFIX_PATH="/path/to/SDL3"
cmake --build out/mga --target MGAcid

# 2. Game data: extracts BOOT.BIN as EBOOT.ELF and links the image
profiles/mga/scripts/prepare_game.sh "/path/to/Metal Gear Acid (USA).iso"

# 3. Generate the recompiled executable and build it
profiles/mga/scripts/generate.sh
cmake -S . -B out/mga
cmake --build out/mga --target MGAcid

# 4. Recompile the game's own libraries (kjfs, zlib, sound) and rebuild
profiles/mga/scripts/generate_modules.sh
cmake -S . -B out/mga
cmake --build out/mga --target MGAcid

# 5. Run (on Windows, copy SDL3.dll next to MGAcid.exe first)
out/mga/bin/MGAcid
```

Supported executable: SHA-256
`f0886cc094dcab4001383fdae5c347cf0365be1deddb88008c1ee66ea6299c29`.

## Recompiling the stages

Every stage (`prologue`, `st01`, ...) is a PRX the game loads, links by itself and then
unloads. Until a stage is recompiled it runs in the interpreter. Because the game links a
stage only after loading it, a corpus can be generated only from the module's memory:

```bash
# 1. Play with dumping on; each stage you reach is written to profiles/mga/game/dumps
MGA_DUMP_MODULES=profiles/mga/game/dumps out/mga/bin/MGAcid

# 2. Recompile whatever has been dumped, then rebuild
profiles/mga/scripts/generate_modules.sh
cmake -S . -B out/mga && cmake --build out/mga --target MGAcid
```

`MGA_STAGES="title prologue"` limits it to certain stages; the default is every stage with
a dump. The script names the stages it has no dump for.

## Diagnostics

- `MGA_NO_RENDER=1 MGA_NO_AUDIO=1 timeout 60 out/mga/bin/MGAcid` — headless boot check.
- `MGA_TRACE_MODULES=1` — every export a loaded module links.
- `MGA_TRACE_FPL=1` — fixed-size pool allocations.
- `MGA_LOAD_ALL_MODULES=1` — also load Sony's plain-ELF libraries instead of the HLE.
- `MGA_TRACE_EXPORTS=1` — every call into a loaded module's exports, with text arguments;
  `=2` also dumps what pointer arguments point at.
- `MGA_NO_MODULE_CORPORA=1` — interpret the loaded modules even when recompiled ones exist;
  with a list (`=KCEJ_SOUND,ZLIB`) only those, which pins a bug on one corpus.
- `MGA_DUMP_ON_ERROR=sp:0x100,095DF050:0x40` — dump guest memory when the runtime stops on an error.
- `MGA_WATCH=0886F5CC:4,08870678:12f` — log the arguments of guest functions, and what pointer
  arguments point at: `:4` as signed bytes, `:12f` as floats. Where the function returns to its
  caller in one go, its result and what it wrote through those pointers follow. Only addresses the
  runtime dispatches to can be watched (see `host/kernel/call_watch.cpp`).
- `MGA_PAD_SCRIPT="10:START,12:CIRCLE"` — presses buttons at those emulated seconds, so a headless
  run can reach the part of the game a bug lives in. Buttons: `START SELECT UP DOWN LEFT RIGHT
  L R TRIANGLE CIRCLE CROSS SQUARE`, joined with `+`.
- `PSPRECOMP_STRICT_MEMORY=1` — stop on a guest access outside PSP memory. By default such an
  access reads zero and is reported, because the game reads one word below its own stack frame
  during the prologue and hardware gets away with it. Every one is still printed, so set this
  when hunting a recompiler bug and leave it unset to play.
- `MGA_HANG_REPORT_S` (default 10, 0 disables) — when no frame has been presented for that long,
  print what every thread is doing and where the running one is, a few times over, so a spin
  shows up as a pc that does not move.
- `PSPRECOMP_WATCH_WRITE=<address>` — reports every write to a guest address, with the old and
  new values; `PSPRECOMP_TRACE_ON_ERROR=1` prints the last 64 dispatches when the runtime stops.
- `MGA_TRACE_DISPLAY=1` — once a second, how the game paces itself: `sceDisplaySetFrameBuf`
  calls, how many of them were real flips, vblank waits and vblanks.
- `MGA_STARVATION_INTERVAL` (default 200) — dispatches between preemptions of a thread that
  makes no kernel call. The game's sound threads need a fine grain during long loads.
- `MGA_UMD_KBPS` (default 1800) — simulated disc speed; 0 makes reads instant.
- `MGA_CPU_SCALE` — how many PSP microseconds a microsecond of *translated code* is worth.
  Unset, it is measured every frame (see `guest_time_for_uninterrupted_work` in
  `host/kernel/kernel.cpp`) and `MGA_TRACE_DISPLAY` reports where it settles; set, it is
  pinned. It does not set the game's speed, which its vblank waits and pacing decide -- it
  sets how emulated time passes inside a burst of guest code, which is what the sound
  driver's delay-paced threads depend on.
- `MGA_STARVATION_QUANTUM_US` (default 4000) — upper bound on the guest time one preemption
  boundary may charge, a guard against host stalls rather than a tuning knob.
- `MGA_DUMP_MODULES=<dir>` — write each loaded module's memory, after the game has linked it.
  `scripts/generate_modules.sh` recompiles the stages from these dumps.
- `MGA_TRACE_IO=1`, `MGA_TRACE_SYNC=1` and the other `*_TRACE_*` switches from the
  MHP3rd README.
