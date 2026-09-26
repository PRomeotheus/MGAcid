# What to run, now that the batch is in the tree

The files are in and verified, the build configures, and the two data-free test
suites pass. What has never happened is a frame being drawn: everything in the
renderer has been compiled and nothing in it has been *seen*. The order below
is chosen so that a failure points at one change rather than at the pile.

Paths use forward slashes throughout.

## 0. Already done, on this machine

Everything in sections 0 and 1 of the old version of this file has been carried
out. Recorded here so it is not repeated:

- The 46 files are in the tree and were verified byte for byte against the
  tarball after copying. Nothing was lost: your uncommitted work is all present
  (`MGA_PROBE_CODE`, `MGA_SWEEP_AFTER`, `MGA_STAGE_BASE`, `texture_budget` all
  still in place), and `overlays.cpp` and `generate_modules.sh` were never
  shipped and never touched. A `git diff` of the work that was uncommitted
  beforehand is saved at `MGA/Claude outputs/before_batch_uncommitted.patch`,
  588 lines, restorable with `git apply`.
- **Line endings were matched to this tree file by file.** 36 of the 46 were
  wrong in the tarball as first built -- see the section below, which has been
  rewritten because both earlier versions of it were wrong.
- `cmake -S . -B <dir> -DPSPRECOMP_PROFILE=mga` configures cleanly, all 77 AOT
  units and every module. Both new test targets generate, build and pass under
  ctest.
- `dumps/` added to `.gitignore`. It was not ignored, and it now holds 2,356
  PNGs that one `git add -A` would have committed.
- The texture pack is built: `dumps/textures`, 2,356 images from 9,096
  appearances, 76 seconds. `--list` says it would enlarge 1,349 and leave 1,007
  alone, which is the number this file predicted, so the classifier fix came
  across.
- Three starter colour grades are written to `MGA/Claude outputs/grades`.

What could NOT be done from here: the Linux VM this ran in has no SDL3, Vulkan
or glslang, so `-- mga: renderer disabled` and none of the renderer work was
compiled there. It compiles in the cloud against real Dear ImGui headers and
stub SDL/Vulkan ones, and the shaders all pass glslangValidator, but **the MSVC
build of the renderer has never run.** That is section 1.

## 1. Build -- done, and it links

`MGAcid.exe` builds under MSVC. All nine shaders are embedded, `dof.frag` and
`reflect.frag` among them, and both data-free test suites pass.

Two things broke on the way and are fixed, both of a kind the cloud check
cannot see:

- `mga_state_tests` failed to link. `kernel.cpp` asks fast loading whether time
  may run ahead, and the real answer lives in a file that target does not
  link. `-fsyntax-only` cannot see a definition missing from another
  translation unit, so that whole class of error was invisible until a real
  linker ran.
- `mga_colour_lut_tests` segfaulted. Its scratch file was hardcoded to `/tmp`,
  which does not exist on Windows, so every load failed and every check then
  indexed an empty vector. It had been passing on Linux while testing nothing.

Worth running the rest once, since they all build now and none needs game data:

```
ctest --test-dir out/mga --output-on-failure
```

## 2. First run: nothing on

Every new setting defaults off except per-pixel lighting, which was already on.
So this run should look exactly like the build you last played.

```
out/mga/bin/MGAcid.exe > run_base.log 2>&1
```

- The banner reads `PSPRecomp PSP bootstrap`, not Yakumo.
- The menu is dark green with near-white text, and the selected row is a
  near-white bar with dark text on it.
- Video is in sections now: Picture, Textures, Lighting, Post-processing,
  Shadows, Frame pacing.
- The Text section, with Font, Weight and the fonts folder, is still there and
  SHOULD be. I said earlier it was a Monster Hunter leftover and that it had
  been removed. Both were wrong: it was never removed, and removing it would
  have been a mistake. profiles/mga/host/hle/hle_font.cpp implements sceLibFont
  for this game, and every run prints "Fonts: game text from MS Gothic" — the
  game's own text really is drawn through that setting.
- Play a battle. **Anything that looks different here is a regression**, since
  nothing new is switched on.

## 2b. What the first run actually found

Recorded because it is a property of the port worth knowing, not a one-off.

**Texture scaling and textures the game rewrites in place.** A still-image
cutscene with a dialogue window ran at 7 to 15 frames a second with audio
skipping, while 3D gameplay held a steady 60. The cause was Texture scale, and
it is not new: texture_key() mixes the guest address on purpose, so a texture
rewritten in place is seen as a new texture and misses the cache on every
frame. Everything on the miss path is then paid every frame, and at 4x that is
a two-megapixel rescale plus an eight-megabyte upload, per frame, for a picture
about to be overwritten. Steady geometry uploads once and hits forever, which
is why only the cutscene showed it.

Such an address is now detected -- a miss again within two frames of its last
miss -- and skips the upscale, the pack dump and the pack lookup. The count
appears in the video memory line as `rewritten N`.

Two things worth carrying forward from how this was found:

- Frame smoothing was MASKING it. With smoothing on it read as 25-32 fps; with
  smoothing off, 7-15. Turning a feature off and watching the number get worse
  means the feature was compensating, not causing. I read that as inconclusive
  when it was already decisive.
- I assumed a regression and searched my own changes for three rounds before
  Pablo found it. "Anything different here is a regression" is a good rule for
  deciding what to investigate and a bad one for deciding where to look: this
  port had pre-existing costs that nothing had happened to exercise yet.

## 3. The two measurements -- taken, and what they said

Both are done. Recorded here because they change what is worth doing next.

**The flipbook guard is free.** `uv axes 28,182 per second, held 0%`, steady
across the whole session. This game has no flipbook, the threshold never fires,
and `MGA_NO_FLIPBOOK_GUARD` is not needed. Closed.

**Lighting reaches a minority of the picture.** `transformed=217 lit=62` in one
room and `transformed=486 lit=232` in another: between 26% and 48% of drawn
geometry receives light at all. So section 4 is worth doing and will not touch
half to three quarters of what is on screen. That is the ceiling I said to look
for, and it is real without being total.

**The game sets up no usable light.** This was not what the measurement was
for, and it is the more interesting result. Every `[shadow]` line reports
`light=0 worldfixed=1` with a direction of about `(0.32, 0.91, 0.25)` -- almost
straight up, which is shadow_map.cpp's own fallback for a light it could not
resolve. Two things follow:

- Light shafts will draw, but from an invented overhead light rather than from
  anything the game asked for. If they look arbitrary, that is why, and it is
  not a bug in the pass.
- The floor reflections assume +Y is up. This confirms it independently, from
  a different piece of code than the one I reasoned from.

Taken together: shader work on the lit fraction has a hard limit here, and the
deferred relight pass -- giving the unlit majority a light at all -- is the
thing that would move the most picture. That is the standing recommendation
whatever section 4 turns up.

## 4. One at a time, in this order

Dependencies mean some of these do nothing without the one before it.

1. **Per-pixel lighting** — already on. Turn it *off* once and back on to see
   what it is doing.
2. **Linear lighting and tonemap.** The big one. Highlights should stop
   clipping to flat white; shadows should lift slightly. Tonemap Neutral
   deliberately leaves a mid grey where the old path had it, so judge falloff,
   not brightness. **Then look at a cast shadow on scenery with this on and
   off.** That path had a real regression — the darkening constant was tuned
   in display values and this moved the arithmetic before the encode — and the
   fix is the thing most likely to be wrong in a way that only a picture
   shows.
3. **Light intensity**, which needs linear lighting. Faithful is the game's own
   levels; anything higher gives the tonemap's shoulder and bloom something to
   work with.
4. **Surface relief**, which needs per-pixel lighting. Good on stone, panels
   and grating. Expect it to look wrong on characters — their dark patches are
   paint, not depth — which is what the strength dial is for.
5. **Accurate highlights.** Move the camera and watch a highlight. On the old
   path it does not move at all; that is the bug, not the style.
6. **Ambient shape.** Surfaces facing away from the light should gain form.
7. **Cast shadows**, then **Light shafts**, which need them. Shafts need the
   game to actually set up a casting light, so if they never appear, check the
   shadow map has something in it before assuming the pass is broken.
8. **Field of view.** Past about 120% expect to find the edge of what the game
   drew; it culls to its own field, not to this one.

## 5. The blinking

This was diagnosed from reading code, not from seeing it, so it is worth
confirming rather than assuming.

Turn **Frame smoothing on**, go to the main menu, open Ac!d's load-game list.
It should be steady. Both fixes are in: the extra present no longer runs while
the interface is up, and the interface being on screen is now itself a reason
to present, so the dialog does not sit at ten frames a second.

## 6. Fast loading — on its own, and last

Off by default. Turn it on in Video, then:

```
MGA_TRACE_DISPLAY=1 out/mga/bin/MGAcid.exe > load.log 2>&1
```

Each fast stretch logs `[load] ran ahead for N ms of game time, ended by ...`.

What to watch for is not the loads but the **play**: if a `[load]` line appears
while you are playing rather than while a load screen is up, the detector is
mistaking something for a load, and `MGA_FAST_LOADING=0` turns it off. The
guard that makes this safe is the audio one — silence is judged after the
channel's volume — so music playing should make fast loading impossible.

## 6b. The colour grade

The Colour row is no longer a single built-in lift. It is now a strength, and a
new **Grade** row underneath it chooses what that strength is taken towards:
the built-in one, or any `.cube` file in a `grades` folder beside
`settings.ini`.

First make some to choose from:

```
python3 profiles/mga/tools/make_grade.py %LOCALAPPDATA%/MGAcid/grades
```

Three appear: `metal-gear-cold`, `warm-film`, `neutral-contrast`. They are
arithmetic rather than colour science -- a curve, a saturation scale and a
tint, chosen by eye -- and they exist so the row has something in it and so you
can see what a grade does before deciding whether to author one properly.
Anything Resolve or Lightroom exports drops straight in.

What to check:

- With **Colour** off, the Grade row does nothing whatever it says. That is
  correct: the strength is what applies it.
- Switching between Built-in and a file should change the picture and not the
  brightness of flat white or flat black. If white goes grey or black goes
  murky when a grade is selected, the half-texel sampling in `post.frag` is
  wrong -- that is the one thing in this that a screenshot shows and the tests
  cannot. It round-trips exactly through an identity table on the CPU, so if it
  is wrong it is the GPU's sampler and not the formula.
- Delete a `.cube` while the game is closed, start it, and the row should say
  **file not found** against the name rather than quietly resetting to
  Built-in.
- Turn the strength up to Strong on `metal-gear-cold` and look at a dark room.
  A grade at full strength is meant to look like a decision, not like a filter;
  if it looks like a filter, that is the grade to change, not the feature.

## 6c. The two that draw at the seam

Both are off by default, both are new, and neither has ever drawn a frame.

**Sharpening** first, since it is the one that cannot go wrong quietly. Turn it
to Strong and look at the sharpest edges in the picture -- a grating, the edge
of a card. **A white or black speck that sits on an edge and does not move with
the camera is a division by zero**, which is the bug I found in this and fixed;
if it is back, the weight constant went wrong again and `MGA_SHARPEN=0` turns
it off. Otherwise expect crisper edges and no pale outlines around them; the
outlines are what this is built to avoid, and seeing them would mean the
headroom test is not working.

**Depth of field.** Stand in a corridor and look down it: the far end should
soften, the floor at your feet should not. Then:

- **Point at a wall a metre away.** The wall should be sharp and everything
  past it soft. If the whole picture goes soft, the focal plane is not being
  found -- the centre tap reads the depth buffer, so that would mean the depth
  direction is wrong, which is the same flag the occlusion uses.
- **Walk behind a railing.** A floor seen through one is the case the
  derivative normals get wrong, and this is where to look for a halo.
- **Change the internal resolution with it on.** The half-size copy is rebuilt
  there; if the picture goes black or garbage for a frame, or the validation
  layers complain about a destroyed image view, the rebuild order is wrong.
- Nothing nearer than the focal plane is ever blurred. That is deliberate, not
  a bug -- see the note at the top of `dof.frag`.

**Floor reflections.** I expect this one to disappoint, and I would rather say
so now than have it be a surprise.

- Find an open room with a bright light and stand back. That is its best case.
  A corridor seen from above is its worst, and it may do nothing at all there.
- **If reflections appear on walls rather than floors, world up is not +Y.**
  That would be worth knowing on its own -- `shadow_map.cpp` assumes +Y in two
  places and the shadows land correctly, so I would expect it to hold.
- `MGA_REFLECT_STEPS` (4 to 48, 24 by default) trades texture reads for how far
  along the floor a reflection is found.
- If it looks like a decal rather than a surface -- equally strong wherever you
  stand -- the Fresnel term is not working.

If reflections turn out to be worth little here, that is not a reason to keep
tuning them. The measurement in section 3 decides something much larger: if
most of a room never receives light, the deferred relight pass moves more of
the picture than every screen-space effect in this list put together.

## 7. The texture pack -- built, not yet enlarged

`dumps/textures` already holds all 2,356, extracted from the disc in 76
seconds with no playthrough. What is left is the enlarging, which needs a GPU
and so needs to happen here rather than in a VM:

```
python3 profiles/common/tools/upscale_textures.py dumps/textures <data>/textures
```

`<data>` is the folder holding settings.ini -- `%APPDATA%/MGAcid` -- or
anywhere, with `MGA_DATA_DIR` pointing at it. The weights are one download and
are not in the repository. On CPU this took 33 seconds for six textures, so
1,349 of them would be hours; on your GPU it is minutes.

Then turn **Texture pack** on in Video.

Worth knowing before you judge the result: the pack is a clear win on faces and
on hard-edged detail like grating, and it *loses* fine low-amplitude texture --
the floor tiles went nearly flat, and Snake's eyes lost their green and came
back grey. Because the pack is keyed by content, replacing one texture later
with a hand-corrected version is a single-file drop-in; nothing needs
regenerating.

## 8. Commit

The advice that used to be here -- commit the pre-existing work separately from
this batch so a bisect can tell them apart -- is now only half possible, and
that is my doing.

Cleanly separable, because my batch never touched them:

- `profiles/mga/host/overlays.cpp` (`MGA_PROBE_CODE`)
- `profiles/mga/scripts/generate_modules.sh`
- `profiles/mga/host/kernel/module_loader.cpp` (`MGA_STAGE_BASE`)
- `profiles/mga/host/kernel/module_sweep.cpp`, `module_sweep.hpp`
  (`MGA_SWEEP_AFTER`)

Not separable by file: `vulkan_renderer.cpp`, `vulkan_renderer.hpp` and
`hle_media.cpp` now carry your earlier work (the memory budget) and this batch
in the same files, because the copy replaced them wholesale. Splitting those
would mean `git add -p`.

So the honest options are a clean first commit of the five above and one large
second commit, or one commit for the lot. `MGA/Claude
outputs/before_batch_uncommitted.patch` holds the earlier work on its own
either way.

## What I would expect to go wrong

In order of likelihood, and all of it stated so that being wrong is visible:

1. **The baked shadow depth** with linear lighting on. Correct-looking code,
   unchanged shadow logic, a wrong result purely from where the arithmetic
   moved. I found one instance of that; there may be another I did not.
2. **Light shafts not appearing at all**, because the game rarely sets up a
   casting light. That would be the game's lighting, not the pass.
3. **Surface relief on characters**, which I expect to look wrong and left on
   a dial for that reason.
4. **The flipbook guard holding too much**, if this game's texture coordinates
   are in units my threshold does not suit. The trace counter is there to say
   so.
5. **Fast loading triggering during play**, which the audio guard should
   prevent and which the log would show.
