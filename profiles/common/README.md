# Shared host code

Host code that is not specific to any one game, kept beside the profiles rather
than inside one of them.

The profiles under `profiles/` began as copies of each other and have since
drifted a long way apart — the two renderers no longer resemble each other
closely enough to share a file, and probably never will. That is fine for code
that is really about one game. It is waste for code that is not: a replacement
texture loader does not know or care which game asked for the texture, and
keeping one copy per profile means finding every copy each time something is
wrong with it.

So anything here has to be genuinely game-agnostic, and in practice that means
three things:

* It does not include a profile's headers, and no profile name appears in it.
* It is in `namespace psp`, not a profile's namespace.
* It could be dropped into another PSP port with no edits at all.

Things that fail that test — the GE state machine, the renderer, the kernel —
stay in their profile even where two profiles look similar today, because the
similarity is a coincidence of history rather than a shared design.

## What is here

* `texture_pack` — replacement textures read from `<data>/textures` by a
  64-bit content key, and the dump that produces those files in the first
  place. Needs FFmpeg for PNG; a profile without FFmpeg simply does not list it.
* `texture_scale` — enlarges a decoded texture before it is uploaded, either
  plain bicubic or an edge-preserving pass for art drawn texel by texel.

## `tools/upscale_textures.py`

Turns a texture dump into a replacement pack. It needs a dump first, and a dump
only exists once the game has run: the renderer writes one texture per file as
it decodes them, so there is nothing to enlarge until the game has actually
drawn the things you want enlarged.

```
MGA_DUMP_TEXTURES=1 out/mga/bin/MGAcid.exe      # play through what you care about
pip install torch pillow numpy
curl -LO https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth
python3 profiles/common/tools/upscale_textures.py <data>/textures/dump <data>/textures --list
python3 profiles/common/tools/upscale_textures.py <data>/textures/dump <data>/textures
```

`--list` classifies everything and writes nothing, which is the right first run:
it shows what would be enlarged and what would be left alone, and the answer to
"why did it skip that one" is easier to argue with before an hour of CPU time
than after.

It enlarges four times with Real-ESRGAN and deliberately does not touch the
interface art — cards, text, icons, panels. Those were drawn texel by texel to
be shown at one texel per pixel, and a model that invents plausible detail
invents it on hand-drawn edges too. The file's own comments explain the test and
why each signal is or is not trusted. `--all` overrides it if you disagree.

The weights are not in the repository: they are 67 MB, they belong to the
Real-ESRGAN project, and they are one download.

## Using it from a profile

There is no library target, so a profile that wants none of this links nothing
extra. A profile opts in by naming the files it wants in its own source list and
putting `profiles/` on its include path, so the headers are reached the same way
everywhere:

```cmake
set(PSP_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")

# ... in the profile's source list:
    "${PSP_COMMON_DIR}/texture_pack.cpp"
    "${PSP_COMMON_DIR}/texture_scale.cpp"

target_include_directories(<target> PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
```

```cpp
#include "common/texture_pack.hpp"
```

A profile whose own code lives in its own namespace can name what it uses rather
than qualifying every mention:

```cpp
namespace mygame::gpu {
using psp::gpu::TexturePack;
}
```

## Taking it to another repository

Copy this directory and add the two blocks above. Nothing here refers to
anything outside it apart from the standard library and, for `texture_pack`,
FFmpeg — so there is no other end of the wire to connect.
