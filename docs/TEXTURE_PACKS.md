# Texture packs

**Upscaled / redrawn textures** as an opt-in, present-time enhancement in the
spirit of `SHADOW_ENHANCEMENTS.md` (the faithful renderer stays authoritative
and byte-identical with the feature off). This document covers the texture
*identity* and *dump* (B1/B2), the replacement path on both renderers (B3/B4),
configuration and authoring (B7) and palettes/fades/usage accounting (B9).
Status and plan: the game repo's `ROADMAP.md`, track B.

## Where the hook is

Every textured primitive the GPU core executes reaches the renderer backend
through `runtime/src/gpu_render.c` (`gr_draw_textured_rect`,
`gr_draw_textured_rect_scaled`, `gr_draw_textured_triangle`,
`gr_draw_shaded_textured_triangle`). Those four wrappers call
`texture_pack_note_rect/tri()` (`runtime/src/texture_pack.c`) with the
primitive's texpage word, CLUT and texel span — backend-agnostic (software,
OpenGL, Vulkan). Off, the note is one branch on `g_texture_pack_active`.

Texels are read from the CPU-side VRAM mirror the core keeps for every backend
(uploads always land there). Textures a game *renders* into VRAM are current
only on the software renderer; for 2D titles that upload their art this is
moot.

## Identity (B1)

Two 64-bit FNV-1a hashes per primitive:

* **texel id** — over the palette indices (4/8bpp) or halfwords (15bpp) of
  the sampled rectangle, plus its width, height and depth. Independent of
  where the texture sits in VRAM and of the CLUT: the same sprite streamed to
  a different page, or the same tile in a different stage, has the same id.
* **palette id** — over the CLUT entries (16 or 256 halfwords; 0 for 15bpp).

A replacement pack keys on the *texel id* and may carry per-*palette id*
variants (genuine recolours: enemy variants, weapon palettes) — palette fades
and flashes (Mega Man 8 fades every stage in over ~32 palette steps) do not
multiply the asset set. Measured on Mega Man 8, cold boot → intro stage:
1.2 M notes, **9,701 (texel, palette) pairs but 776 texel ids**.

Texel spans: rects are exact; triangles use the uv bounding box, taken as
inclusive when the span is not a multiple of 8 (Mega Man 8 encodes quads
both ways, even within one primitive). Spans are clamped to 256×256.

## Dump (B2)

```
{"cmd":"texture_dump","op":"arm","dir":"/abs/dir"}    debug server (headless scripts too)
PSX_TEXTURE_DUMP=/abs/dir                              environment
{"cmd":"texture_dump","op":"stats"} / "disarm"
```

While armed, the first time a (texel, palette) pair is seen it is written as
`<dir>/<tex_id>-<pal_id>.png` (RGBA, colour 0 = transparent, STP bit not
recorded in the pixels) plus `<tex_id>-<pal_id>.clut` (the CLUT's 16 or 256
halfwords, BGR555 LE — the palette the PNG was rendered with), and a row is
appended to `<dir>/textures.tsv`:
`tex_id pal_id w h bpp texpage_x texpage_y clut_x clut_y u v first_frame`.
`stats` / `disarm` also write `<dir>/pairs.tsv` (`tex_id pal_id draws`): how
many primitives drew each pair, which is what tells a settled palette from a
fade step (a 16×16 title tile: 4,429 draws with its palette, 10–20 with each
of the 31 flash/fade palettes). Stats: notes total / this frame, unique pairs,
unique texel ids, files written. The seen-set holds 32 K entries per run.

Typical use: run through the game once (or a scripted headless route), then
group the TSV by `tex_id` to see the asset set, pick the canonical palette per
texel id, and hand artists one PNG per id.

## Replacement — software renderer (B3)

```
{"cmd":"texture_pack","op":"load","dir":"/abs/pack"}     debug server; "unload"; "stats"
PSX_TEXTURE_PACK=/abs/pack                                environment (loaded on first use)
```

A pack is a directory of `<tex_id>.png` (any palette) and/or
`<tex_id>-<pal_id>.png` (that palette only), each an **integer multiple N of
the native texel rectangle** it replaces (a dump directory is therefore a
valid 1× pack), optionally with `.clut` sidecars (see *Palettes and fades*).
Lookup: exact palette variant first, then the entry whose reference palette
the live one is a fade of, then the palette-agnostic file; images whose size
is not a whole multiple of the rectangle are ignored.

Where it draws: the software renderer's **hi-res mirror and native-wide
surface only** (`[video] supersampling` ≥ 2, `renderer = "software"`; the
present path shows the hi-res surface). `sw_draw_textured_rect / _scaled /
_triangle` identify the primitive (same texel id as the dump), fetch the
replacement, and the S× rasterisers sample it at the primitive's texel
coordinates instead of VRAM. Per pixel: replacement alpha < 128 → nothing is
drawn, and neither is a pixel whose **native texel is 0x0000** (index 0 or a
palette entry faded to black — transparent on the PSX); else its 15-bit
colour, with the **native texel's STP bit** so semi-transparency behaves as on
PSX (a replacement that comes out black over a drawn texel stays opaque
black); colour modulation, mask bits and blend modes are the existing
`put_textured` path. Native VRAM (`t->s == 1`) never
sees the pack, so VRAM→CPU reads, savestates, netplay digests and the 1×
picture are byte-identical (verified: `screenshot` equal with/without the
pack, `screenshot_hires` differs).

Verified on Mega Man 8 (software, 2×): a 776-image pack made from the dump
(sepia + bicubic 2× as a stand-in for real art) replaces the background,
HUD and tiles it covers; uncovered ids fall back to native texels; 17,780
lookups → 15,715 hits over ~90 frames (~200 lookups/frame; each is one hash
of a 16×16 rect).

Not yet: shaded-textured triangles (3D titles). The GL renderer is B4, the
`[video] texture_pack` key + launcher row B7, fade handling B9 (all below).

## Replacement — OpenGL renderer (B4)

The GL backend (`gpu_gl_renderer.c`) carries the same replacement through
its textured program: four extra flat vertex attributes per primitive
(`a_rep_org` = the prim's texel rect u0,v0,w,h; `a_rep_atlas` = its rect in
the **pack atlas**; `a_rep_mod` / `a_rep_off` = the palette-fade scale and
offset of B9), and the fragment shader samples the atlas (RGBA8, nearest,
`texelFetch`) at `(uv - org) / size` when `a_rep_atlas.w > 0`: alpha < 0.5
discards, a native texel of 0x0000 discards (PSX transparency), the STP bit
still comes from the native texel, `rgb = clamp(rgb * mod + off)`, colour
modulation / semi-transparency / mask passes are the existing ones. The
atlas is shelf-packed from every loaded image whenever the pack generation
changes (up to 8192², images that do not fit fall back to native texels and
are counted in the log). Lookups use the same texel id as the dump and the
SW path (`gl_rep_for_rect`, unbumped span; mirrored rects sample the same
image mirrored). Verified on Mega Man 8 (OpenGL, `[video] supersampling = 2`):
identical replacements to the software path, 20,580 lookups → 17,960 hits.

**Divergence (documented, accepted like the others in `gpu_gl_renderer.c`)**:
on GL the hr FBO *is* VRAM — CPU readbacks (VRAM→CPU transfers, GPUREAD,
`screenshot`, savestate VRAM) re-encode it, so replaced pixels of *rendered*
framebuffer content are visible to them (at 5-bit precision), whereas the
software renderer keeps native VRAM untouched. Textures the game uploads are
never altered (uploads bypass the shader). Same behaviour as upscaled
emulators; irrelevant to titles that do not read their framebuffer back.
Replaced colours keep 8-bit precision on GL (the software path quantises to
15-bit), so the two backends differ by ≤ 1 LSB of 5-bit for replaced texels.

Not on GL yet: shaded-textured triangles, bilinear sampling of pack images
(nearest only), the Vulkan backend (unfiltered by design, like video filters).

## Configuration and authoring (B7)

```toml
[video]
texture_pack = "game-assets/textures/pack"   # relative to the project root; offered only when the dir exists
texture_pack_enabled = true                  # default of the launcher toggle
```

The launcher's Display page gains an **"HD textures"** checkbox (with the
pack directory name) when the directory exists; the choice persists in
`settings.toml` (`[video] texture_pack = true|false`) and applies live from
the in-game launcher too. `PSX_TEXTURE_PACK=<dir>` still overrides for one
run; the `texture_pack` debug command loads/unloads at runtime. Remember the
pack is only *visible* at `[video] supersampling` ≥ 2 (launcher: Display →
Supersampling) — at 1× the renderers draw native texels.

`tools/texpack.py` (Pillow only):

```
texpack.py summary  DUMPDIR                     pairs / texel ids / sizes / pages
texpack.py starter  DUMPDIR PACKDIR --scale N   one <tex_id>.png (+ .clut) per texel id, N x nearest —
                                                the pixel-identical skeleton artists repaint —
                                                plus <tex_id>-<pal_id>.png (+ .clut) for genuine recolours;
                                                --palette common (default, most-drawn palette per pairs.tsv)
                                                | first | last | all | <pal_id>; --no-variants
texpack.py coverage PACKDIR TSV...              covered texel ids / exact-palette pairs, --missing out.tsv
texpack.py validate PACKDIR [--tsv TSV]         names, alpha channel, integer-multiple sizes, .clut sizes
texpack.py sheet    DIR OUT.png                 contact sheet of a dump or a pack
```

Workflow: play (or script) through the game with `texture_dump` armed →
`starter` → repaint the PNGs you care about (keep the size an integer
multiple of the native rect; alpha 0 = transparent) → `validate` →
`coverage` against new dumps as you play further → drop the pack directory
where `[video] texture_pack` points and toggle it in the launcher.

## Palettes, fades and usage (B9)

A pack image is keyed by texel content, but the colours the player sees come
from the *live* CLUT — and PSX games fade, flash and dim by rewriting CLUTs
(Mega Man 8 fades every stage in over ~32 palette steps; its title screen
adds a white flash that steps back over ~60 frames, both by re-uploading the
palette bank every frame). Without B9 a replacement showed at full authored
brightness through all of that.

**Reference palettes.** Every pack entry may carry the CLUT it was authored
against: `<tex_id>.clut` next to `<tex_id>.png`, `<tex_id>-<pal_id>.clut`
next to a variant (32 bytes for 4bpp, 512 for 8bpp, BGR555 little-endian —
exactly what the dump writes, so `starter` just copies them). Entries without
a sidecar behave as before (as authored, any palette).

**Fade model.** When the live palette id differs from the reference's, the
runtime fits the live CLUT against the reference per channel — **over the
palette entries the primitive's texels actually use** (a solid tile cares
about one entry; entry 0 / a 0x0000 reference is the transparent colour and
is skipped; an opaque-black reference takes part) — with two uniform models
and keeps the closer one if its residual is below 2 levels rms:
*multiplicative* `cur = ref × k` (dimming) and *subtractive*
`cur = clamp(ref − d)` (the classic PSX fade / flash: every channel steps by
the same amount, clamping at 0 or 31 — the step is estimated from the
unclamped entries, all-clamped means fully black / fully white; a negative
step brightens, which is how art authored dark reads a brighter live entry).
The replacement is then drawn as `clamp(rgb × scale + offset)` — on the
software path in `rep_sample`, on GL through the `a_rep_mod` / `a_rep_off`
attributes.

**Variant selection — and degrading to native.** Lookup order per primitive:
(1) the exact `<tex>-<pal>` variant; (2) among all entries of that texel id
that carry a sidecar, the one whose reference the live palette is a fade of
(smallest residual) — so a stage fade-in of a *recolour* dims that recolour,
not the common art; (3) if the pack has reference palettes for this texel id
but the live palette is a fade of none of them, the pack has **no art for
this recolour: the native texels are drawn** (counted as
`native_recolour` in `texture_pack stats` — the number that says "this needs
a `<tex>-<pal>` variant"); (4) entries without any sidecar (packs made
before B9) draw as authored. Showing authored art under a foreign palette is
never right — the same solid/border/font tiles recur across the title, the
stage select, text bubbles and the pause menu with unrelated CLUTs, and
"as authored" painted them in the intro stage's colours (pink bubble
borders, black squares on the stage select). `starter --palette common`
(the default) writes the most-drawn palette per texel id (from `pairs.tsv`)
as `<tex>.png` and every palette the fade model cannot reach as a
`<tex>-<pal>.png` variant with its sidecar.

**Result.** With a 2× nearest starter pack (pixel-identical art) the software
hi-res picture is **pixel-identical to native** through the title fade-in,
the white flash and its fade back, the intro stage, the stage select, a
later stage, the pause/weapon menu and the title menu; on OpenGL the
difference is the documented ≤ 2/255 of 5→8-bit expansion. Verified with the
game repo's `tools/mm8_headless.sh`-style scripts capturing `screenshot_hires`
with and without `PSX_TEXTURE_PACK` (the config pack must be empty for the
"native" run — a `[video] texture_pack` directory loads at boot).

**Usage accounting.** `texture_pack stats` reports `used` (images drawn at
least once) next to lookups/hits, `{"cmd":"texture_pack","op":"usage",
"path":...}` writes `tex_id pal_id hits` per image, and unloading logs
"N of M images were drawn" — the numbers that tell an artist which of the
files they painted the game actually reached. `PSX_TEXTURE_PACK` is applied
at startup (it overrides the config pack and the launcher toggle for that run).

**Debug aid.** `vram_peek` takes `"hires":1` to read the software hi-res
mirror at the same native coordinates — the quickest way to tell a renderer
divergence from a pack one.

Limits: the fit is per primitive per draw (a few hundred per frame, 16 or 256
entries each — negligible); 15bpp textures have no CLUT and no fade support;
palette *cycling* (water, energy) is a recolour to the model and needs
variants; recolour detection in `starter` needs the `.clut` files of a fresh
dump (`--no-variants` skips it).

## Presenting at S× (B5)

Replacements are only visible at `[video] supersampling` ≥ 2, where the S×
picture is what gets presented. Video filters at that scale follow
`VIDEO_FILTERS.md` → *With supersampling*: the pixel-art upscalers stand
down (they would misread HD art as staircases), the display looks
(sharp / scanlines / crt) apply at the native line pitch on both backends.

## Next

Bilinear pack sampling; SW-vs-GL parity tool; shaded-textured triangles.

## Files

`runtime/include/texture_pack.h`, `runtime/src/texture_pack.c` (identity,
dump, pack loading — private static `stb_image` PNG decoder),
`runtime/src/gpu_render.c` (identity hooks), `runtime/src/gpu_sw_renderer.c`
(replacement sampling + fade modulation in the S× rasterisers),
`runtime/src/gpu_gl_renderer.c` (atlas + shader path + fade attributes),
`runtime/src/png_write.h` (`png_write_rgba`), `runtime/src/debug_server.c`
(`texture_dump`, `texture_pack` incl. `usage`; `screenshot_hires` pitch fix;
`vram_peek hires`), `runtime/tests/test_texture_pack.c`,
`recompiler/src/config_loader.{h,cpp}` (`[video] texture_pack`, settings),
`runtime/src/main.cpp` (load + launcher glue), `tools/texpack.py`, and in
recomp-ui `recomp_launcher.h` / `launcher_model.{h,c}` / `launcher_imgui.cpp`
(the "HD textures" row, `RECOMP_LAUNCHER_HAS_TEXTURE_PACK`).
