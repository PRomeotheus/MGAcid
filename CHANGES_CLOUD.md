# Changed in the cloud, to copy back

## Fast loading (Video > Fast loading, OFF by default)

From upstream, and the one ported thing this session that could actually be
RUN here rather than only compiled.

Reading the disc is instant on a PC, yet a load still takes as long as it did
on the hardware: the game's loader reads a piece, unpacks it, and waits on the
emulated clock in between, and the kernel holds that clock to the PSP's pace.
This lets the hold go, and only while the game is loading.

A load is recognised from what the game DOES, not from a timer: it is reading
the disc and it is silent. Anything that could be play keeps real time -- the
first audible sample, a held button or direction, the menu, Game speed
Unlimited.

- host/kernel/fast_loading.hpp, load_detector.cpp  the decision, with no host
                            around it. Ported close to upstream's, including
                            its thresholds: a 500 ms window after a disc read,
                            250 ms of quiet before a load may run fast, and
                            silence meaning exactly zero after volume.
- host/kernel/fast_loading.cpp  the running game's instance and the log line.
- host/kernel/kernel.cpp    real_time_clock_active() consults it.
- host/hle/hle_io.cpp       sceIoRead feeds the reads.
- host/hle/hle_media.cpp    the audio path feeds the peak and drops silence
                            handed over while time runs ahead; the pad read
                            feeds the buttons; a vblank hook re-evaluates.
- tests/fast_loading_tests.cpp  new, and check.sh builds and RUNS it.

One deliberate difference. Upstream scales the clock by up to sixteen. This
rides on the free-run path the fast-forward key already uses instead: simpler,
already proven in this port, and the same answer in practice, because what
bounds a load is how fast the loader threads can be emulated rather than the
cap.

The audio hook is not optional and is worth saying why. Without it the
detector never learns that sound exists, the quiet test never fires, and any
disc read during play -- with music going -- would let time run free. Silence
is judged after the channel's volume, so a full-scale buffer at zero volume
counts as silent and the first audible sample of anything ends the fast
stretch at once.

Off by default, where upstream ships it on: their thresholds were measured
against their game's loader, and whether Ac!d's loader waits on the clock the
same way is exactly the thing nobody has checked.

## What was NOT taken, and why

- **"Record a frame while the GPU still draws"**: 508 lines rewriting the
  frame lifecycle in vulkan_renderer.cpp, the file with the most changes this
  session. It is also what makes their ImGui-buffer and texture-pack-lifetime
  fixes necessary -- take it and those two stop being irrelevant to us. This
  is the first thing to do after the current batch has been run, not before.
- **Background texture decode**: 190 lines of threading in the texture path.
  Threading bugs are the worst kind to find from a description of a symptom,
  and nothing here can run it.
- **The alpha-test specialization constant**: it buys early depth testing on
  tiled GPUs -- phones and Apple -- and this runs on a desktop NVIDIA part. It
  also touches ge.frag, which has had more changes than anything else here.
- **GPU breadcrumbs**: they need VK_AMD_buffer_marker and are the large part
  of that commit. The lost-device report already added is what turns a silent
  exit into a sentence; breadcrumbs are what to reach for if a sentence turns
  out not to be enough.

## Inside the .dar, and a pipeline cache

**The whole interface compiles here now.** font_menu.cpp and layer.cpp needed
AddFontDefaultVector and ImTextureDataQueueUpload, which no release tag has --
the docking branch does. One header set (imguidock/) replaces the two that were
there, check.sh compiles all ten ui/*.cpp files plus the renderer, and the
stub directories are gone. Cloud-only scaffolding; do not copy imguidock/ into
the repo.

**tools/dar.py**, new: the model archives. The container is the same
named-entry layout the _zar uses internally, and all 42 walk cleanly -- 1,749
entries, every archive's declared count matching the walk. 782 models, 315
animations, 144 each of three motion formats, 116 effects, and two ordinary
PNGs.

The useful negative result first: **no texture data and no texture name appears
anywhere in an MDP.** A model's art is in the stage's .qar, which qar.py
already extracts, so nothing in here is needed for a texture pack.

What the MDP header gives up: a kind field, a node count, three offset slots,
a scale, and two bounds corners. The nodes are 80 bytes each and begin at the
LARGEST of the three offsets -- which holds for all 782 models, and is the
reason the rule is written that way rather than as a fixed offset.

Two mistakes on the way there, both from reading three files and generalising:

- The first field of a node looked like a magic, because the three models I
  opened first happened to share the value 0x007A07AA. It is a per-node id:
  eleven other values turn up as soon as more files are read. What IS true of
  all 782 is that every node's id has a zero high byte and is never zero, and
  that is the check the walk uses.
- The header field at +0x10 was "the same in every file seen" on the strength
  of those same three files. It differs per model.

Reading a fixed offset for the node table gave 502 models of 782, 8,014 nodes
and 271,281 vertices, and reported the other 280 as having unreadable headers.
The corrected rule gives 782, 13,618 and 631,148. The first set of numbers
looked perfectly reasonable, which is the point worth keeping: a walk that
fails on a third of its input still prints a total.

## Pipeline cache (from upstream, partially)

Pipelines were created with no VkPipelineCache, so every run compiled each one
again the first time the game drew with it. It now lives in the data directory
as pipeline_cache.bin, seeded at start only when its header names this device
and this driver, and written out at shutdown -- beside the file and renamed
over it, so a run that ends badly leaves the previous cache whole. All four
pipeline creation sites go through it. MGA_NO_PIPELINE_CACHE starts cold.

Deliberately saved at shutdown rather than a few seconds after new pipelines
appear, as upstream does: the place that creates a pipeline is in the middle of
recording a frame, and a file write there lands at exactly the moment the game
first draws something new -- a stutter in the place the cache exists to remove.

**Not taken from that commit: the alpha-test specialization constant.** It
compiles the discard out of pipelines that do not need it, which matters on
tiled GPUs -- phones and Apple -- by letting early depth testing work. On a
desktop NVIDIA part the gain is small, and it touches ge.frag, which has had
more changes this session than anything else and none of them run yet.

## The selected row is inverted, the project is PSPRecomp, and a lost GPU says so

**Inverted selection.** Metal Gear Solid 4's selected row is a near-white bar
with dark text, and that was the last visible difference from the screenshot.
Sampled from the same frame: the bar reads (243, 249, 238) and its ink
(28, 36, 27). The work was not the bar, it was everything else a row draws --
the label, the value, the arrows, the switch, the slider track, the disabled
note -- each of which picked its own colour from the palette and would have
stayed pale on a pale bar. Row now carries the colours its contents should use
and the six row helpers take them from there: fifty references rethreaded, so a
row cannot end up half inverted. on_selected() maps the odd ones out, because a
red "Quit game" on white needs a darker red, not the same red.

**Yakumo -> PSPRecomp**, in 49 user-visible strings across twelve files: the
menu's own panel title, the setup screens, every installer error, the ad hoc
default player name, the Vulkan application name, the log header and the boot
banner. Two traps in there, both avoided: the usage text names the EXECUTABLE,
which is MGAcid, not the project; and "Yakumo input script" is compared with
strcmp in vulkan_renderer.cpp, so both ends had to move together or the renderer
would stop recognising the script. Left alone: kYakumoVersion (generated by
write_version.cmake, so renaming it is a build change I cannot verify here) and
comments that refer to the upstream project by name, which are accurate.

Still yours: profiles/mga/packaging/windows/Yakumo.rc.in sets the Windows
executable's product name and description, and is not in my snapshot.

## A lost GPU now says so (from upstream 0750318)

Two of the three parts, and not the third.

**Robust buffer access**, where the device offers it: a read past the end of a
buffer returns zeros or stays inside it rather than reading whatever memory
follows, which on some drivers faults and on others hangs the GPU. Costs
nothing measurable and turns a class of crash into a wrong pixel.
MGA_NO_ROBUST_BUFFERS leaves it off.

**Lost device reporting.** VK_ERROR_DEVICE_LOST means the GPU was reset under
us. Every call after the first one returns it too, so what matters is where it
was first seen -- and all four sites that can return it (the frame's submit and
three fence waits) were ignoring their result. A lost device therefore looked
like the window closing on its own with nothing in the log, which is exactly
what you described twice. It now names where, says plainly that it is a driver
reset rather than something the game did, and ends instead of drawing into a
dead device and showing a frozen window.

That is a hypothesis about your crashes, not a diagnosis: if they were lost
devices, the next one will say so; if the window still closes silently, the
cause is elsewhere and that is worth knowing too.

**Not taken: the GPU breadcrumbs.** They need VK_AMD_buffer_marker, and they
are the large part of that commit. The simpler report above is what turns a
silent exit into a sentence; if it turns out not to be enough, breadcrumbs are
the next step rather than the first one.

## The 4K pack can now be built without playing the game

Three things were in the way, and the first two were the same lie told twice.

**The pack was keyed by address.** common/texture_pack.hpp says a replacement
is found "by a 64-bit key, derived from its bytes rather than its address, so
the same image gets the same key wherever the game happens to have loaded it",
and common/README.md repeats it. Neither was true: the renderer passed
texture_key(), which mixes the guest address as its first term, because that
function exists to notice a texture rewritten in place -- the opposite job. A
pack keyed that way matches only while the art stays where it was first seen,
and needs a file for every place an image appears. It now passes content_key(),
which mixes the size and the decoded texels and nothing else.

**Nothing could produce a dump without a playthrough.** `qar.py pack` writes
the archives straight into the pack's own naming -- sixteen lowercase hex
digits and .png, the same key the renderer will look up -- plus an index.txt
that also names the stage each image came from. Run over the whole USRDIR:
**2,356 files from 9,933 appearances, 69 seconds.** The difference between
those two numbers is what the content key buys.

The whole route is now:

    python3 profiles/mga/tools/qar.py pack <dump dir> <extracted USRDIR>
    python3 profiles/common/tools/upscale_textures.py <dump dir> <data>/textures
    turn Texture pack on

**The upscaler was skipping everything worth enlarging.** Its pixel-art test
asked whether a texture had fewer colours than `max(32, width * height / 64)`
-- 256 on a 128x128 texture, which no CLUT8 texture can have and no CLUT4
texture can approach. Every character, weapon and wall in this game was
therefore classified as interface art and left alone; the four textures checked
by name (both enemy atlases, the MP5, a hair texture) were all skipped.

Measured over the archives, 200 textures either side: interface art has a
median of 1 distinct colour and 90% of it has 10 or fewer, while 3D art has a
median of exactly 16 -- exactly CLUT4's palette, because most of it is CLUT4.
That is what makes the cliff. At a threshold of 16 the rule keeps 31% of the 3D
art; at 12 it keeps 76%, and the interface rejection only moves from 96% to
94%. So the line goes below 16, not above it, and a fixed number is right where
an area-derived one is not: the palette caps the colour count, so the area says
nothing.

Result: **486 textures would have been enlarged before, 1,349 now**, and the
art you actually wanted is in the second number.

The threshold is tuned on this game's archives. profiles/common is shared, so
if the MHP3rd profile is ever run from this tree its textures are classified by
a number chosen for a palettised game; --all still overrides either way.

## Metal Gear's palette, and a field of view slider

The amber and brown came from the profile this was forked from, where they
suited that game. Sampled a frame of Metal Gear Solid 4's shop screen rather
than picking colours by eye, which is why the numbers are not round: its panel
reads (30, 41, 29), its text (251, 254, 248) -- white with just enough green
left in it to belong to the panel -- and its labels (159, 174, 146). Green is
the highest channel everywhere, including in the greys, and that is what stops
the result looking merely dark rather than looking like a screen.

host/ui/widgets.hpp carries the palette and was the bulk of it, but seven more
colours were hardcoded past it in widgets.cpp (the popup, the modal dim, the
scrollbar, the separator, the selection) and four in text_input.cpp, so the
grep mattered more than the edit. What is deliberately still warm is kDanger: a
warning has to stop reading as part of the panel, and in a green interface the
one thing that does that is not being green.

Not done, and it is the one visible difference from the screenshot: MGS4
inverts its selected row -- a near-white bar with dark text. Ours keeps a pale
wash and the accent edge, because inverting means flipping the text colour
inside the row widget rather than changing a constant. Say the word if you want
it; it is a widget change, not a palette one.

## Field of view (Video > Picture > Field of view, 80-160%)

Camera movement broke the game, but the field of view is a property of the
projection rather than of where the camera is, so nothing moves: the game still
frames every shot itself and this only changes how much of the room fits in
that frame.

Widened properly rather than by scaling the matrix terms. A perspective matrix
holds the cotangent of half the field in its diagonal, so the field can be
recovered, multiplied and put back. The difference is not academic -- from a
45 degree field, at 1.3x a real widening gives 58.5 degrees and scaling the
terms gives 56.6, and at 1.6x it is 72 against 67. Both axes take the same
factor so the game's aspect ratio survives, and an orthographic projection is
left alone, told apart by its last term being one rather than zero: that test
is what keeps this off the interface and everything else laid out in 2D.

Checked against the arithmetic: the field lands within 0.1 degrees of what is
asked for across the whole range, the aspect is unchanged to 1e-6, and the
orthographic case is returned untouched.

The warning worth repeating from the row's own description: past about 120%
expect to meet the edge of what the game bothered to draw. Ac!d culls to its
own field, not to this one, so a wider view can show where the scenery stops.

## Also from upstream: the flipbook guard (73248e5, 23 Sep)

Their commit adds two guards to frame interpolation. We already had one of
them: a pair that moves more than kMaxBlendPixels (80 px of the PSP's screen)
is given up, which is the same idea as their 120-unit own-motion test, done in
screen space instead of eye space. Nothing to take there.

The other we did not have. A texture coordinate that steps a large fraction of
the texture in one game frame is a flipbook turning its page, not a scroll, and
blending it sweeps the in-between image across the cells between the two --
upstream saw it as fire running too fast. Held now, per axis and per draw,
since every vertex of a draw moves together when the page turns.

Two differences from theirs, both forced by this game:

- They guard the GE's texture offset register, which is in units of the
  texture, so a tenth of it is a fixed 0.1. We blend per-vertex coordinates,
  and this game's are whatever the GE was handed -- normalised on one draw and
  in texels on another -- so a fixed number would mean different things on
  different draws. The step is measured against the draw's own coordinate span
  instead, which stands in for how much of the texture it covers. Their 0.1
  is kept.
- Held at the NEWER frame's value, not the older one's. Theirs interpolates
  between two frames, so the older is the one to hold; ours carries the newest
  frame forward, so the newest is the one the extra image should agree with.

The threshold is reasoned, not measured: nobody has yet seen a flipbook in
Ac!d, and the one thing known about its texture animation is that it scrolls
water and menu panels, which is the case that must keep blending. So:
MGA_NO_FLIPBOOK_GUARD turns it off without a rebuild, and MGA_TRACE_DISPLAY=1
now prints how many coordinate axes were looked at and what percentage were
held. Zero held means Ac!d has no flipbook and the guard costs nothing; a high
percentage means the threshold is wrong for this game, not that the game is
full of flipbooks.

Known false positive, accepted: a draw covering a small part of its texture
and scrolling normally can reach a tenth of its own small span and be held.
The failure is the safe direction -- holding shows the newest real coordinate
instead of an extrapolated one, which costs a little smoothness on that draw,
where blending a real flipbook shows an image the game never drew.

## Taken from upstream (TeamGDB/Yakumo, 24 Sep)

Their DXT fix, 453f843, because our decoder was byte-identical to the version
they fixed. Three defects, all ours too:

- decode_dxt_block() read the PC's block layout. The PSP puts the 2-bit
  indices before the two RGB565 endpoints, and DXT3/DXT5 alpha after the
  colour part rather than before it, and the endpoints keep red in the top
  bits unlike the GE's own 5650 texels. Read the PC way, a DXT texture comes
  out as coloured 4x4 noise. Our function is now byte-identical to their
  fixed one, checked by diff rather than by eye.
- texture_key() mixed the CLUT into every texture's key. A direct-colour or
  block texture is drawn with whatever CLUT address the game last left set,
  and upstream found theirs pointing at the framebuffer -- whose first word
  changes every frame, so the key changed every frame and the texture was
  decoded and uploaded again on every frame it appeared. Now mixed only for
  the four indexed formats.
- the size used for the content sample treated every block format as half a
  byte per texel. That is DXT1; DXT3 and DXT5 are a byte.

Metal Gear Ac!d may draw no DXT texture at all -- all 9,933 in the .qar
archives are CLUT4 or CLUT8 -- so this may fix something the game never
reaches. It is still the right layout: a decoder wrong only where nothing
looks is worse than a correct one, because on the day something does look,
nobody thinks of the decoder.

Deliberately NOT taken, though they look tempting:

- "Give the interface's ImGui buffers room for every frame in flight" and
  "Keep texture pack images for four frames" both begin "now that two are in
  flight". We have one frame_fence and wait on it at the top of every frame,
  so neither hazard exists here. Porting them would be carrying a fix for a
  premise we do not share.

## The interface can now be compiled here

The one file I had been changing blind all along was menu.cpp -- every row
added this session went in unchecked. The real Dear ImGui headers (v1.92.1)
plus two small stubs for headers the build generates or vendors fixed that, and
check.sh now compiles eight of the ten ui/*.cpp files. font_menu.cpp and
layer.cpp are left out: they use AddFontDefaultVector and
ImTextureDataQueueUpload, which are newer than that tag, so a failure there
would be the header version rather than the code. The stub headers live in
imguireal/ and imguistub/ and are cloud-only scaffolding -- do not copy them
into the repo.

## Video tab regrouped, MH3rd wording gone

Sections in Video, using the section() the menu already had rather than a new
widget: Picture, Textures, Lighting, Post-processing, Shadows, Frame pacing,
and Text. Four rows moved to sit with what they belong to -- Texture pack into
Textures, Contact shadows into Shadows, Frame smoothing into Frame pacing, and
Light intensity after the Tonemap row it feeds. No behaviour changed.

The Monster Hunter leftovers, all of them user-visible:
- three strings sending the player to the "Online Guild Hall" now say Link
  Battle.
- group_name() translated codes beginning "MHP3Q" into "Hall 01". That prefix
  never matches on Ac!d, so the row already fell through to the raw code. The
  translation is deleted rather than rewritten: nothing here knows what Ac!d's
  group codes mean, and inventing a mapping that might be wrong is worse than
  showing the code the game chose.
- the save screen said no saves of Monster Hunter Portable 3rd were found, and
  suggested folder ULJM05800 -- MHP3rd's disc id. Both now come from
  install::kGameTitle and kDiscIdDisplay.
- the setup screen named Monster Hunter Portable 3rd HD Ver. (NPJB-40001) as
  the only supported release. Same two constants.
- exported saves landed in a folder called "MHP3rd saves <time>".
- the Controls tab had a section called "Hunter name".
- the About row was labelled Yakumo. Now PSPRecomp. The boot banner still
  prints "Yakumo PSP bootstrap"; that is outside these files.

The font rows are a different case and are hidden rather than deleted. They
choose the typeface the GAME's text is drawn with, which only means anything
for a game drawing text through sceLibFont. Ac!d predates that library and
carries art for every character it shows, so the rows offered a choice that
changed nothing -- worse than an absent setting, because a player tries it and
concludes the port is broken. fonts::guest_uses_fonts() is set on the guest's
first sceFontNewLib and the rows appear only then, so a title that does use
fonts gets them without anyone having to add it back. Worth confirming with
MGA_TRACE_FONT=1 that Ac!d really never calls it.

## The interface blinking over a menu

One defect found and fixed; whether it is the one you saw depends on whether
frame smoothing was on.

host/hle/hle_media.cpp, present_between_frames(). Every present consumes the
interface's draw data and clears it -- submit_and_present() reads ui_draw_data
and nulls it, by design, because that data is only valid until imgui starts its
next frame. The game's own flip draws the menu. The extra present, arriving a
vblank later, finds nothing left to draw and shows the same picture without it.
The menu therefore appears on every second image: a menu blinking at thirty.

The intent was already right -- the comment there says a menu at thirty beats a
menu on every second image -- but not blending and not presenting were run
together, so the fallback presented anyway. It now returns without presenting
at all while the interface is up. That costs the steady present rate it was
holding, which is the right trade while a menu is open: the game is paused
behind it and there is no motion for an in-between image to smooth.

This fires only with Frame smoothing on, because present_between_frames()
returns immediately when it is off.

Why the same interface is steady over gameplay and blinks over the game's load
list, given that both are the same imgui layer drawn by the same code: it is
not the interface that differs, it is whether the guest is still flipping
behind it. With the port menu open over gameplay the guest is paused, so
nothing presents again and the last image -- the one carrying the menu -- stays
on screen. During the load list the guest is not paused: it polls and flips,
every flip presents, and every extra present strips the menu off.

Second fix, same area, host/hle/hle_media.cpp should_present(). While one of
our dialogs is up the guest often draws nothing at all -- on a PSP the utility
drew itself, so the game only polls and flips -- and with no display list
nothing sets drawn_since_present. The only presents left were the idle ones,
one every 100 ms. Combined with the above that put the menu on screen for about
16 ms in every 100, which is the blinking; on its own it still leaves a dialog
sampling input and animating ten times a second. The interface being on screen
is now itself a reason to present. It is self-limiting: the flag is set by
draw_over_game(), which runs from the present it causes, and clears itself the
first time that finds nothing to draw.

## What the lighting work did to the shadows

Three interactions, found by tracing the shadow path against each change
rather than by assuming they composed. One was a real regression.

**The baked shadow got 55% lighter, silently.** A shadow on geometry with no
light term to subtract is applied as `color.rgb *= 1.0 - blocked * 0.55`, and
that 0.55 was chosen by eye against display values. The multiply now happens
before the encode instead of after it, and the same number applied to light
rather than to an encoded value darkens by 0.70 where it used to darken by
0.45 -- purely from moving the arithmetic, with nothing in the shadow code
changed. Raised to the gamma on the linear path, which restores what was
originally chosen instead of asking for the constant to be picked a second
time. Worth noting this is not only the baked scenery: ge.vert also takes the
darkening path for LIT geometry whose shadow direction was inferred rather
than taken from a light of the game's own, so it reaches more than its name
suggests.

**The directional ambient was not being occluded.** Ambient that favours the
lit side is light that bounced off whatever the main light is hitting, so
something standing in that light blocks it too. Only the boost is taken away,
never the flat part -- that arrives by every other path and is what stops a
shadow going black. Left alone, a shadow reaches 62% of the unshadowed value
where it should reach 75%, and it falls short by an amount that depends on
which way the surface faces, since a surface facing away never had the boost.
One shadow reading deeper than another for no visible reason is the worse half
of that.

**The shafts were adding in display values.** By the time the volumetric pass
runs, a scene shaded in linear space has already been tonemapped and encoded,
so a plain additive blend clips flat at white -- undoing, in the brightest part
of the picture, the exact rolloff the curve exists to provide. The source is
now scaled by what the target has left, giving dst + src * (1 - dst): still
additive where the picture is dark, which is where shafts belong, and
asymptotic to white rather than clamped at it.

Checked and found to need nothing: the light scale sits after the light loop,
so the shadow subtraction still works in the game's own units; the relief
perturbs the normal for shading but not the shadow lookup, which stays
geometric, so it cannot cause acne; the Fresnel term is folded into the
specular before that specular is added to blocked_light, so a shadowed
highlight is removed at its new strength rather than its old one; and the HDR
path, by not clamping the light sum, makes the subtraction more correct than
it was, not less -- blocked_light was never clamped, so the old path could
subtract more light than the clamped colour held.

## Four smaller things the lights were missing

- host/gpu/shaders/ge.frag  a real view direction and a Schlick Fresnel term;
                            ambient with a direction to it; a light scale ahead
                            of the tonemap; triangular dither.
- host/gpu/shaders/ge.vert  the two new vec4s in the shared block.
- host/gpu/vulkan_renderer.cpp  EnvironmentBlock 624 -> 656 (camera, shading);
                            kPushAccurateSpecular as bit 5; the eye taken from
                            the inverse of the view matrix.
- host/settings, host/ui/menu.cpp  three rows and five settings.

**The specular was using a viewer that does not exist.** The half vector was
`normalize(to_light + vec3(0, 0, 1))` -- the GE's infinite-viewer shortcut,
which was fine in its own space and is simply wrong in the world space the
light loop now runs in. A highlight that cannot depend on a viewing direction
does not move when the camera does. Measured on a sphere with the camera swung
from one side to the other: the old highlight moves **0 pixels**, the corrected
one moves 64. The eye position comes from the translation of the inverse view
matrix; when the game hands over a view that will not invert, which it does
during some transitions, the flag drops for that draw and the GE's own
approximation stands in rather than the eye silently sitting at the origin.

**Ambient had no direction.** One constant for every surface means a wall
facing the light and a wall facing away are lifted identically, so nothing in
shadow has any shape. The axis is the dominant light's own direction rather
than a world up vector, and that is deliberate: nothing here knows which way is
up in the game's world space, and a guess wrong by ninety degrees would shade
every surface backwards.

**Nothing ever got bright enough to need the tonemap.** The game's lights and
materials are all fractions of one, so the picture rarely passed 1.0 and the
shoulder had nothing to roll off -- half of the linear-lighting work was
theoretical. The scale is applied after the light loop rather than inside it,
so it stays clear of the shadow subtraction, which works in the game's units
and would otherwise remove more light than it added.

**Dither, and why triangular.** Eight bits across a dark scene bands, and the
tonemap lifts exactly the range where the steps are widest. Banding is visible
because the rounding error follows the signal: the same shade always rounds the
same way, so the mistake lines up into an edge. Measured on a dark ramp, error
against signal: none 0.033, flat dither 0.011, triangular 0.009. A flat dither
shrinks the correlation, a triangular one removes it, which is the difference
between narrower edges and no edges. Default is one level; the setting goes to
four if banding still shows.

NOT verified: none of it has been run.

## Surface relief  (Video > Surface relief, off by default)

The third item, and not the one that was planned. Normal maps for 2,356
textures would mean authoring or generating 2,356 files, a second binding on
every texture descriptor, a default flat map for everything without one, and a
manifest to tie them together. This gets most of the way there with none of it,
by reading the relief out of the albedo the game already has.

The reasoning: on geometry this coarse a wall is two flat triangles and the
only record of its surface is the painting on it. Art drawn for a fixed camera
has its shading baked in, and the dark parts of a concrete or grating texture
are, nearly always, the parts that were recessed when someone drew it. Reading
brightness as height is a guess -- but one that agrees with the artist far more
often than not.

- host/gpu/shaders/ge.frag  surface_height and relief_normal; applied inside
                            light_fragment, before anything reads the normal,
                            and only on a textured draw.
- host/gpu/shaders/ge.vert  the matching vec4 in the shared block.
- host/gpu/vulkan_renderer.cpp  EnvironmentBlock gains surface, 608 -> 624;
                            surface_relief, forced to zero unless per-pixel
                            lighting is on, since that is where it is applied.
- host/gpu/vulkan_renderer.hpp  set_surface_relief.
- host/settings/settings.{hpp,cpp}  video.surface_relief.
- host/ui/menu.cpp          the row; disabled without per-pixel lighting.

The tangent frame is taken from how the world position and texture coordinate
change between neighbouring pixels, so nothing has to be stored on a vertex --
which matters, because the game supplies no tangents and there would be nowhere
to get them. Degenerate cases (a triangle edge-on, a triangle with no texture
area) fall back to the vertex normal rather than dividing by zero.

Because it reads the same texture the game draws, a texture pack improves it
for nothing: replace an albedo with one drawn at four times the size and there
is four times as much relief to find.

Checked: run over the dumped textures, the derived normals are clean and
plausible -- crisp bars on grating, drawer and panel edges on lockers, fine
tooth on concrete. The sign was verified separately against a known height
field, because an inverted normal map is the classic silent failure here: a
bright ridge lights on the flank facing the light, so bright reads as raised.

Where it does badly, stated plainly: anything whose dark patches are dark paint
rather than depth. The character atlases pick up relief from markings that are
flat in reality. They have real normals from their geometry already, so this
adds noise to them rather than detail -- which is what the strength dial and
the Off setting are for.

NOT verified: never run.

## Volumetric light shafts  (Video > Light shafts, off by default)

Light in the air rather than on surfaces. The shadow map already answers the
question this needs -- for a point in the world, was anything between it and
the light -- so this asks it repeatedly along the line from the eye to whatever
it is looking at and adds up the answers. No new geometry, no second map, and
nothing needs to know where the light is.

- host/gpu/shaders/volumetric.frag  new.
- host/gpu/vulkan_renderer.cpp  invert() for 4x4; VolumetricPush (112 bytes,
                            every offset asserted, and asserted under the 128
                            Vulkan guarantees); create_volumetric_pipeline and
                            volumetric_descriptor_for; a second draw inside
                            record_scene_ao; the frame's world-to-clip kept at
                            the last 3D draw; pool and teardown grown to match.
- host/gpu/vulkan_renderer.hpp  set_volumetric, volumetric_available.
- host/settings/settings.{hpp,cpp}  video.volumetric. MGA_VOLUMETRIC_STEPS
                            overrides the 24-sample march.
- host/ui/menu.cpp          the row; disabled without cast shadows.
- profiles/mga/CMakeLists.txt  the new shader in the embed list and DEPENDS.

It shares the occlusion pass's render pass and framebuffers -- same single
colour attachment, loaded and stored -- and runs as a second draw in it, after
the occlusion and before the interface. Occlusion multiplies, this adds, and
that order matters: the other way round the crease darkening eats the shafts.

The march never touches world space. Handing the shader shadow_transform *
inverse(view_projection) as one matrix, plus the row of the inverse that gives
w, lets it turn a pixel straight into the light's clip space at both ends of
the ray. Dividing each end by its own w before interpolating is what makes the
steps equal distances in the WORLD rather than bunching against the near plane;
checked against marching in world space over 300 random camera and light pairs
and it agrees to 1.6e-14, which is to say exactly.

invert() was checked against numpy on 400 realistic view-projections: it lands
on the float32 optimum to 1.4e-8 per element, and the residual |M*inv-I| of
1.2e-3 is what float32 gives for these matrices, numpy included.

The dither deserves a note. The usual sin-of-a-dot hash makes coarse, blotchy
grain, because neighbouring pixels land on unrelated values and the error
clumps where the eye is most sensitive. Interleaved gradient noise puts it at a
high frequency and reads as a fine weave. A local-difference measure prefers
the hash and is simply wrong: it rewards smooth blotches over fine structure.
This was settled by rendering both and looking.

NOT verified: never run. The algorithm was simulated end to end in Python on a
synthetic room -- shadow map, depth buffer, the same march -- and produced the
shafts it should, but no frame of the game has gone through it.

## Linear lighting and tonemap  (Video > Linear lighting and tonemap, off by default)

The first of the three lighting items. What it changes, in one line: the GE
multiplied an encoded texture value by a light level and clamped at one, which
treats a number authored by eye on a display as though it were an amount of
light. This converts the texture to linear first, applies the light there, and
brings the result back through a film curve.

- host/gpu/shaders/ge.frag  srgb_to_linear, linear_to_srgb, aces and tonemap;
                            light_fragment takes an hdr flag and stops clamping
                            the light when it is set; main() linearises the
                            texel, drops the three intermediate clamps, mixes
                            fog in linear, and maps at the end.
- host/gpu/shaders/ge.vert  the Environment block gains the same vec4, because
                            both stages declare it and the declarations have to
                            match.
- host/gpu/vulkan_renderer.cpp  kPushLinearLight (bit 4 of the enables);
                            EnvironmentBlock grows a tonemap vec4, so the
                            layout assert moves 592 -> 608; linear_light and
                            tonemap_curve, read at initialize() and settable.
- host/gpu/vulkan_renderer.hpp  set_linear_light, set_tonemap.
- host/settings/settings.{hpp,cpp}  video.linear_light, video.tonemap_curve.
- host/ui/menu.cpp          two rows; the Tonemap row disables itself without
                            the toggle, as the bloom row does without post.

Only lit geometry takes the path. The interface and the baked scenery -- which
is most of a level -- carry finished colours with no light in them to work out
more correctly, and are left byte for byte alone. That also means the change
shows up on characters and dynamic lights first.

The curve is ACES divided by its own value at the same point, which pins fully
lit white back at white; applied raw it sits at about 0.80 and everything goes
milky. The one dial is how hard it bends, and as it goes to zero the two calls
converge into the identity, so the same number runs from linear-and-clipped to
fully filmic with no second switch. Checked numerically before it went in:

    light x        0.25    1.0     2.0     4.0
    old            0.125   0.500   1.000   1.000     <- clipped, and flat after
    new (k=0.47)   0.172   0.500   0.739   0.960

Neutral is k = 0.47 because that leaves a mid grey exactly where the old path
had it, so the toggle can be judged on falloff rather than on having made the
picture brighter. White stays white at every setting, the curve is monotonic
over the whole range, black stays black, and the transfer functions round trip
to 3e-16. Worth noting that even at k = 0, linear shading alone moves light x2
from a clipped 1.000 to 0.684 -- the blowout was the gamma-space multiply, not
the missing curve.

NOT verified: it has never been run. The shaders compile, the C++ compiles, and
the numbers above are from the same formulas the shader uses, but nobody has
looked at a frame.

Verified here: shaders compile individually and as the set the build embeds;
every C++ file below compiles with g++ (Vulkan and SDL3 headers fetched, imgui
stubbed). `~/tools/check.sh` runs all of it.

NOT verified: ui/menu.cpp, which needs imgui headers that are not here. Its
changes are three menu rows and three lines in the restore-defaults block,
matching the rows either side of them.

One thing to glance at after copying back: `git diff --stat`. Every file I
edited is uniformly one line ending, not mixed, so this should be small. If
post.frag shows all 216 lines changed rather than about 20, its endings flipped
and `unix2dos` on it puts that right. ao.frag and post.frag are LF while the
other four shaders are CRLF, but that is how the snapshot arrived, not
something from tonight.

## Per-pixel lighting  (Video > Per-pixel lighting, on by default)
- host/gpu/shaders/ge.vert   light loop moves out; passes world position and
                             normal instead. The per-vertex path is kept and
                             chosen by bit 3 of the enables, so the two can be
                             compared.
- host/gpu/shaders/ge.frag   gains the Object uniform block and the light loop.
- host/gpu/vulkan_renderer.cpp  kPushLightPerPixel; Object block visible to the
                             fragment stage; light_per_pixel plumbing.
- host/gpu/vulkan_renderer.hpp  set_light_per_pixel.
- host/settings/settings.{hpp,cpp}  video.light_per_pixel, default on.
- host/ui/menu.cpp           the row.

## Bloom  (Video > Bloom, off by default, needs post-processing)
- host/gpu/shaders/post.frag  bloom_params, and a 24-tap golden-angle spiral
                              over light above a threshold.
- host/gpu/vulkan_renderer.cpp  PostPush gains a vec4 (now 64 bytes) with every
                              offset asserted, not just the size; bloom radius
                              scales with the internal resolution.
- host/gpu/vulkan_renderer.hpp  set_bloom.
- host/settings/settings.{hpp,cpp}  video.bloom.
- host/ui/menu.cpp            the row.

## The .qar texture archives are cracked: 9,933 textures, no playthrough

`tools/qar.py`, new, stdlib only (no Pillow), works on a .qar or on a stage
`_zar` or on a directory of them:

    python3 profiles/mga/tools/qar.py extract --manifest dumps/textures <the extracted USRDIR>

One command over the whole USRDIR is enough: it recurses, and picks up both the
per-stage `_zar` archives and a loose `resident.qar`. Tested that way here, 45
directories out. `list` instead of `extract` prints sizes and formats without
writing anything.

Run over all 44 stage archives plus resident.qar: **9,933 PNGs, zero failures,
111 seconds**. Every stage carries a `cache.qar`, so the output is one
directory per stage. Spot-checked by eye: character atlases, weapon cards,
the title art and the portraits all come out clean with correct alpha.

The format, since nothing documents it. A .qar has no header; the index is at
the end, so the only way in is to walk the chunks and notice when one stops
looking like a chunk, which works because each chunk carries enough to compute
its own length. That also makes the walk self-checking: it ends on a u32 count
that has to equal the number of chunks it found.

    chunks, each padded to 128 bytes
    u32 count
    count * (u32 hash, u32 chunk size)
    count * null-terminated name

A chunk is 32 bytes (zero, flags, count, 0x20, count, 0x20 + count*16, 0, 0),
then `count` 16-byte texture descriptors, then `count` 48-byte draw-time
records nothing here needs. A descriptor is:

    u16 width    (log2(stride) << 12) | width      low 12 bits are the real size
    u16 height   (log2(padded) << 12) | height
    u16 format   a GE format: 4 is CLUT4, 5 is CLUT8
    u16 flags    2 means deflated: a u32 length then a zlib stream
    u32 data, u32 end                              the palette starts at end

Rows are packed at the real width, not the stride. The palette is RGBA8888,
64 bytes for CLUT4 and 1024 for CLUT8. **Every texture is swizzled** (16-byte
by 8-row blocks) and all 9,933 are a whole number of blocks, so there is no
partial-block case. Worth knowing what getting that wrong looks like: a
swizzled texture read as linear comes out as horizontal streaks in roughly the
right colours, which reads as a bad palette rather than a layout problem.

Checks that all passed on the way: the chunk walk ends exactly on the count
word; every chunk's computed size equals its size in the index; all 386
deflated textures inflate to exactly the size the descriptor implies; the gap
after every texture is exactly its palette size, 445 at 64 and 392 at 1024.

## What that means for the 4K pack

9,933 texture instances are only **2,356 unique images** — 1,535 keys appear in
more than one stage. So the pack needs 2,356 sources, not ten thousand.

    longest side <= 32   441        unique pixels    25.3 M
                 <= 64   710        the pack at 2x  405 MB RGBA8
                 <= 128  773                  at 4x 1618 MB
                 <= 256  303
                 <= 512  129

1618 MB sits inside the 2674 MB texture budget the eviction work already set
up, so a 4x pack fits in the 4 GB card without changing it.

`--manifest` writes `textures.tsv`: key, stage, name, width, height, format.
The key is `content_key()`, added to host/gpu/texture_decode.{hpp,cpp} and
computed identically in qar.py — FNV-1a over the width, the height and the
decoded RGBA texels, nothing else. No address, no layout, no palette, so the
same art keys the same however the game stored it. The two implementations were
checked against each other on five sizes and agree byte for byte; a test is
easy to redo, but **they cannot change independently**. This is the hook a
replacement path would use: hash a texture once at upload, look the key up.

Still to decide (yours, not mine): where a pack lives on disk, whether it
preloads or streams, and what happens to the textures a pack does not replace.
I did not build the load path blind.

## Colour grading as a lookup table (Video > Grade, Built-in by default)

The grade that was there is a contrast and saturation lift with two constants
in it. That is not what a grade is, and there was no way to change it without
recompiling. A .cube lookup table is the form every colour grading tool
exports, so now a grade can be made somewhere that shows you the picture while
you turn the knobs, saved, and dropped into a folder.

The Colour row is now the *strength* -- how far the picture is taken towards
the grade -- and the new Grade row chooses which grade that is.

- host/gpu/colour_lut.{hpp,cpp}  new. Reads Adobe .cube. Refuses, with a reason
                            that says which line: a 1D table (it cannot move a
                            hue, so applying one would look like the grade had
                            been applied when most of it had not), a domain
                            that is not 0..1 (a log or scene-referred grade,
                            which this renderer is not showing), a size outside
                            2..64, a count that disagrees with LUT_3D_SIZE, and
                            a file that is not a .cube at all.
- tests/colour_lut_tests.cpp  new, in CMakeLists as mga_colour_lut_tests, and
                            check.sh builds and RUNS it. Nine refusals and six
                            positive cases.
- host/gpu/vulkan_renderer.cpp  a 3D image at post descriptor binding 2, always
                            bound: the identity table when no file is loaded,
                            so the shader never branches on whether it exists.
                            Two entries a side is an EXACT identity, not a
                            coarse one -- trilinear between the eight corners
                            of the colour cube reproduces every colour in it.
                            The cached post sets now key on the table's view as
                            well, or a set built for one grade would be reused
                            for the next.
- host/gpu/shaders/post.frag  the lookup, with the half-texel correction. Not a
                            detail: sampling at the colour itself puts 0 and 1
                            half a texel outside the cube, and black and white
                            are the two colours a grade is judged by. Verified
                            by round-tripping all 256 levels through an
                            identity table on the CPU -- largest error 0.
- host/ui/menu.cpp          the Grade row: Built-in, then every .cube in the
                            grades folder. A grade named in settings.ini whose
                            file has gone says "file not found" against the
                            name rather than quietly resetting.
- tools/make_grade.py       new. Writes three starter grades so the row is not
                            empty. They are arithmetic, not colour science.

## Contrast-adaptive sharpening (Video > Sharpening, OFF by default)

The game draws 480x272 and it is shown several times that size. Every
magnification filter that avoids stair-steps produces softness instead; that is
one trade seen from two ends. This takes the softness back out, with the amount
decided per pixel from how much room the neighbourhood has left before it would
clip -- which is what stops it leaving the pale outlines that make a picture
look sharpened rather than sharp.

- host/gpu/shaders/post.frag  sharpen_delta(). Reads the scene, not the frame
                            this shader has been building, because neighbours
                            are only available from the texture; what it
                            returns is the correction, added to the finished
                            colour.
- host/gpu/vulkan_renderer.cpp  PostPush grows 64 -> 80 bytes, with the new
                            offset asserted like the rest.

**A bug I put in and then found by measuring.** The weight started at -1/4.
The convolution divides by `4 * weight + 1`, so at full amplitude that is
exactly zero -- a NaN on the GPU, reaching the screen as a white or black speck
on the sharpest edges in the picture. It is -1/8 to -1/5 now, which holds the
divisor at 1/5 or above. I found this by simulating the shader's arithmetic on
real textures, not by reading it back, and I would not have found it by
reading it back.

Measured against a plain unsharp mask at the same strength, on pixels that were
not already at black or white: 7.3% newly clipped against 14.1% on grating,
0.02% against 0.08% on Snake's head, 0.00% against 0.00% on concrete.

## Depth of field (Video > Depth of field, OFF by default)

At the seam between the world and the interface, not in the post pass: by the
time the post pass runs the heads-up display is in the same target, and
blurring there would blur the display along with the room behind it.

A far-field blur with an autofocus plane, not a simulation of a lens.
Everything nearer than the focal plane stays sharp. A real lens blurs the near
field too, and here that would mostly be Snake's own shoulder; the asymmetry is
the point, not a shortcut. The focal plane is the depth under the middle of the
screen, read in the shader with a five-tap cross so a railing crossing the
centre cannot pull focus for a frame. There is no smoothing across frames, so a
camera cut refocuses instantly -- keeping history would mean a pixel read back
to the host, a fence and a frame of lag, and an instant refocus on a cut is
what the eye does anyway.

- host/gpu/shaders/dof.frag  new. Each blur tap is weighted by how out of focus
                            IT is, not by how out of focus this pixel is.
                            Without that the blur reaches backwards and a sharp
                            figure gets smeared into the wall behind it.
- host/gpu/vulkan_renderer.cpp  a half-size copy of the world, taken at the
                            seam with a linear blit (which is a box downsample
                            for free). Half size is not a compromise: a blur of
                            a given width costs a quarter of the taps on it.
                            The SHARP picture is never copied and never
                            sampled -- it is already in the attachment, and the
                            pipeline blends with SRC_ALPHA, so the shader hands
                            over the blurred colour and how much of it to use
                            and the hardware mixes the two. That is the same
                            trick the occlusion pass uses to avoid a copy.
                            The copy is rebuilt when the internal resolution
                            changes, and the sets that hold its view are
                            dropped first, because creating it destroys that
                            view.

## Floor reflections (Video > Floor reflections, OFF by default)

The least certain thing here, and the reasons are about the game rather than
the code. Said plainly at the top of the shader and repeated here.

A screen-space reflection can only reflect what is on the screen. In a corridor
seen from above, most of what a floor should reflect was never drawn. In a
simulation of a floor and a lit wall, using the shader's own arithmetic, 29% of
floor pixels find anything at all -- and every one of those 8,734 hits was the
wall rather than the floor itself, which is the part that says the signs are
right.

Nothing in a PSP display list says which surfaces are polished, so this asks
the geometry instead: it reflects only where the surface faces up. That makes
it a floor reflection specifically, which is the case it can do well. World up
is +Y -- not a guess: shadow_map.cpp builds its light basis around {0, 1, 0}
and falls back to overhead when a light dips below the floor, and that code
puts shadows in the right place.

Normals come from the derivatives of the reconstructed world position, because
there is no normal buffer. Exact on a flat surface, wrong wherever two surfaces
meet in one quad. The up-facing test hides most of that and does not hide a
floor seen through a railing.

- host/gpu/shaders/reflect.frag  new. Three fades -- screen edge, ray length,
                            and Fresnel -- each hiding a different way it can
                            be wrong.
- host/gpu/vulkan_renderer.cpp  ReflectPush is 128 bytes, which is the most
                            Vulkan REQUIRES an implementation to offer, so it
                            runs on a device with the minimum. It fits because
                            the forward matrix only ever transforms a
                            DIRECTION, and a direction has w = 0 and never
                            touches the fourth column -- three columns, 48
                            bytes, not 64. The ray marches in clip space,
                            which a projective transform makes legal because
                            it takes straight lines to straight lines, and
                            which makes the step size scale-free: this game's
                            world units are in the thousands and nothing in the
                            renderer knows that.
                            Verified numerically before it was wired up: the
                            reconstruction, the clip identity, the
                            three-column direction transform and the march all
                            agree with the full matrices to 3e-13.
- Both passes share one descriptor set layout and one pipeline builder; they
  differ only in their fragment shader and the size of their push block.

## Line endings

The repository is CRLF. Nine files had flipped to LF across the cloud sessions
-- post.frag, ao.frag, hle_media.cpp, kernel.cpp and others -- each of which
would have shown its whole length as changed. Every text file in the tree is
CRLF now, 152 of them, and the general way to spot a flip in either direction
is in TESTING_NEXT.md. The note there that named post.frag specifically was
both too narrow and, by then, out of date.

## Not done, and why
- Soft shadows: already there, and better than what I would have added.
  shadow_reach() in ge.frag does a blocker search then a variable-radius
  13-tap filter, widening with the measured gap between caster and receiver.
  That is percentage-closer soft shadows with contact hardening.
- Per-pixel fog: would change nothing. The fog factor is linear in view-space z
  and a non-flat varying is perspective-correct, so the interpolated per-vertex
  value is already exact at every fragment, and the clamp is already per-pixel.
