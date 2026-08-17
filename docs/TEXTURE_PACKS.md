# Texture packs — identity and dump (B1/B2)

Groundwork for **upscaled / redrawn textures**: an opt-in, present-time
enhancement in the spirit of `SHADOW_ENHANCEMENTS.md` (the faithful renderer
stays authoritative and byte-identical with the feature off). This document
covers what exists now — the texture *identity* and the *dump* — and pins the
design the replacement path (B3+) builds on. Status and plan: the game repo's
`ROADMAP.md`, track B.

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
recorded in the pixels) and a row is appended to `<dir>/textures.tsv`:
`tex_id pal_id w h bpp texpage_x texpage_y clut_x clut_y u v first_frame`.
Stats: notes total / this frame, unique pairs, unique texel ids, files
written. The seen-set holds 32 K entries per run.

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
valid 1× pack). Lookup: exact palette variant first, then the palette-agnostic
file; images whose size is not a whole multiple of the rectangle are ignored.

Where it draws: the software renderer's **hi-res mirror and native-wide
surface only** (`[video] supersampling` ≥ 2, `renderer = "software"`; the
present path shows the hi-res surface). `sw_draw_textured_rect / _scaled /
_triangle` identify the primitive (same texel id as the dump), fetch the
replacement, and the S× rasterisers sample it at the primitive's texel
coordinates instead of VRAM. Per pixel: replacement alpha < 128 → nothing is
drawn; else its 15-bit colour, with the **native texel's STP bit** so
semi-transparency behaves as on PSX; colour modulation, mask bits and blend
modes are the existing `put_textured` path. Native VRAM (`t->s == 1`) never
sees the pack, so VRAM→CPU reads, savestates, netplay digests and the 1×
picture are byte-identical (verified: `screenshot` equal with/without the
pack, `screenshot_hires` differs).

Verified on Mega Man 8 (software, 2×): a 776-image pack made from the dump
(sepia + bicubic 2× as a stand-in for real art) replaces the background,
HUD and tiles it covers; uncovered ids fall back to native texels; 17,780
lookups → 15,715 hits over ~90 frames (~200 lookups/frame; each is one hash
of a 16×16 rect).

Not yet: shaded-textured triangles (3D titles), the GL renderer (B4), a
`[video] texture_pack` key + launcher row (B7), fade handling (a
palette-agnostic replacement shows at full brightness during a palette fade —
supply `<tex>-<pal>.png` variants for the settled palette, or wait for B9's
palette-aware modulation), coverage tooling.

## Replacement — OpenGL renderer (B4)

The GL backend (`gpu_gl_renderer.c`) carries the same replacement through
its textured program: two extra flat vertex attributes per primitive
(`a_rep_org` = the prim's texel rect u0,v0,w,h; `a_rep_atlas` = its rect in
the **pack atlas**), and the fragment shader samples the atlas (RGBA8,
nearest, `texelFetch`) at `(uv - org) / size` when `a_rep_atlas.w > 0`:
alpha < 0.5 discards, the STP bit still comes from the native texel, colour
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
texpack.py starter  DUMPDIR PACKDIR --scale N   one <tex_id>.png per texel id, N x nearest —
                                                the pixel-identical skeleton artists repaint
                                                (--palette all keeps every <tex>-<pal>.png variant)
texpack.py coverage PACKDIR TSV...              covered texel ids / exact-palette pairs, --missing out.tsv
texpack.py validate PACKDIR [--tsv TSV]         names, alpha channel, integer-multiple sizes
texpack.py sheet    DIR OUT.png                 contact sheet of a dump or a pack
```

Workflow: play (or script) through the game with `texture_dump` armed →
`starter` → repaint the PNGs you care about (keep the size an integer
multiple of the native rect; alpha 0 = transparent) → `validate` →
`coverage` against new dumps as you play further → drop the pack directory
where `[video] texture_pack` points and toggle it in the launcher.

## Next (B9+)

DEGRADED logging when a pack entry's native hash no longer matches;
fade-aware palette handling; bilinear pack sampling; SW-vs-GL parity tool.

## Files

`runtime/include/texture_pack.h`, `runtime/src/texture_pack.c` (identity,
dump, pack loading — private static `stb_image` PNG decoder),
`runtime/src/gpu_render.c` (identity hooks), `runtime/src/gpu_sw_renderer.c`
(replacement sampling in the S× rasterisers), `runtime/src/gpu_gl_renderer.c`
(atlas + shader path), `runtime/src/png_write.h`
(`png_write_rgba`), `runtime/src/debug_server.c` (`texture_dump`,
`texture_pack`; `screenshot_hires` pitch fix), `runtime/tests/test_texture_pack.c`,
`recompiler/src/config_loader.{h,cpp}` (`[video] texture_pack`, settings),
`runtime/src/main.cpp` (load + launcher glue), `tools/texpack.py`, and in
recomp-ui `recomp_launcher.h` / `launcher_model.{h,c}` / `launcher_imgui.cpp`
(the "HD textures" row, `RECOMP_LAUNCHER_HAS_TEXTURE_PACK`).
