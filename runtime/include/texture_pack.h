#pragma once
/*
 * texture_pack.h — texture identity + dump (ROADMAP track B, steps B1/B2).
 *
 * Every textured primitive the GPU core hands to the renderer backend
 * (gpu_render.c gr_draw_textured_*) is reduced to a TEXTURE KEY: the texel
 * rectangle it samples (texpage x/y, colour depth, u/v/w/h) plus its CLUT.
 * The identity is two 64-bit FNV-1a hashes: the TEXEL id over the palette
 * indices (4/8bpp) or halfwords (15bpp) + size + depth — the art, independent
 * of where it sits in VRAM — and the PALETTE id over the CLUT entries. A
 * texture-replacement pack (B3+) keys on the texel id, with optional
 * per-palette variants; palette fades and flashes therefore do not multiply
 * the asset set (Mega Man 8: 9,845 (texel,palette) pairs but far fewer texel
 * ids from boot to the intro stage).
 *
 * Dump mode ({"cmd":"texture_dump","op":"arm","dir":D} or PSX_TEXTURE_DUMP=D;
 * PSX_TEXTURE_PACK=D is applied by the host at startup)
 * writes each (texel,palette) pair the first time it is seen as
 * D/<tex_id>-<pal_id>.png (RGBA, colour 0 = transparent) and appends a row to
 * D/textures.tsv (tex_id, pal_id, w, h, bpp, texpage x, y, clut x, y, u, v,
 * first frame). Off = one branch per primitive.
 *
 * Note: texels are read from the CPU-side VRAM mirror; textures the game
 * RENDERS into VRAM (not uploaded) are only current on the software renderer.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_texture_pack_active;   /* 0 = every note returns immediately (dump) */
extern int g_texture_pack_replace;  /* 1 = a replacement pack is loaded (B3) */
/* How pack images are sampled at S x: 0 nearest, 1 linear (bilinear inside the
 * image, alpha-weighted), 2 auto = linear unless the image has exactly one
 * pixel per hi-res pixel (an N x pack at supersampling N stays pixel-exact).
 * [video] texture_pack_filter = "nearest" | "linear" | "auto" (default). */
extern int g_texture_pack_filter;

/* ---- replacement pack (B3) ----
 * A pack is a directory of <tex_id>.png (any palette) and/or
 * <tex_id>-<pal_id>.png (that palette only), each an integer multiple N of the
 * native texel rectangle. The software renderer's hi-res / wide targets sample
 * these instead of VRAM; native VRAM (and everything at 1x) is untouched. */
typedef struct {
    int w, h;                 /* pixels */
    const uint8_t *rgba;      /* w*h*4, top-down */
    int atlas_x, atlas_y;     /* renderer-owned: placement in its atlas (-1 = not resident) */
    /* an entry may carry the CLUT it was authored against (<tex_id>.clut or
     * <tex_id>-<pal_id>.clut sidecar, BGR555 LE): with it, a different live
     * CLUT (fade, flash) modulates the replacement instead of showing it at
     * full brightness, and the lookup can pick the variant whose palette the
     * live one is a fade of. ref_n = 0 -> none. */
    int ref_n;
    uint16_t ref_clut[256];
    uint64_t hits;            /* draws that used this image */
} TexPackImage;

int  texture_pack_load(const char *dir);        /* returns number of images, 0 = none/failed */
void texture_pack_unload(void);
/* Identify the primitive's texel rect and return its replacement (or NULL).
 * mod[6] (may be NULL) receives the colour transform to apply to the
 * replacement (scale rgb, offset rgb): identity for an exact palette variant
 * or without a reference CLUT; otherwise the uniform fade the live palette
 * applies to the reference (fades then dim the HD art like native texels). */
const TexPackImage *texture_pack_lookup_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                                             int u, int v, int w, int h);
const TexPackImage *texture_pack_lookup_rect_mod(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                                                 int u, int v, int w, int h, float mod[6]);
/* Fit the live CLUT against a reference CLUT: mod[0..2] = per-channel scale,
 * mod[3..5] = per-channel offset in 0..255 units (colour' = colour*scale + offset,
 * clamped). Returns the residual (rms over entry-channels, 5-bit levels) of the
 * accepted model — multiplicative or subtractive, whichever is closer, below
 * 2 levels — or TEXPACK_NO_FIT with mod = identity when the change is not a
 * uniform fade (palette cycling / recolours keep the authored art). */
#define TEXPACK_NO_FIT 99.0f
float texture_pack_palette_mod(const uint16_t *ref, int n, uint16_t clut_x, uint16_t clut_y, float mod[6]);
/* Same, restricted to the palette indices in `used` (4 x 64-bit mask, NULL = all):
 * only the entries a texture draws decide whether the live palette is a fade of
 * the reference (a solid tile cares about one entry). */
float texture_pack_palette_mod_used(const uint16_t *ref, int n, uint16_t clut_x, uint16_t clut_y,
                                    const uint64_t used[4], float mod[6]);
/* {"loaded":N,"dir":"..","lookups":N,"hits":N,"used":N,"native_recolour":N}
 * (used = images drawn at least once; native_recolour = draws that had a pack
 * entry but were left native because the live palette is not a fade of any
 * variant's reference — the count that says "this needs a <tex>-<pal> variant") */
int  texture_pack_stats_json(char *buf, int cap);
/* Write <path> (TSV: tex_id, pal_id, w, h, hits) for every loaded image; returns rows or -1. */
int  texture_pack_write_usage(const char *path);
/* Bumped by every load/unload; renderers rebuild their atlas when it changes. */
uint32_t texture_pack_generation(void);
/* Visit every loaded image (renderers place them in an atlas and record atlas_x/y). */
void texture_pack_for_each(void (*fn)(TexPackImage *img, void *ctx), void *ctx);

void texture_pack_set_vram(const uint16_t *vram_1024x512);
void texture_pack_note_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                            int u, int v, int w, int h);
void texture_pack_note_tri(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                           int u0, int v0, int u1, int v1, int u2, int v2);

int  texture_dump_arm(const char *dir);   /* 0 = cannot write there */
void texture_dump_disarm(void);
/* {"active":..,"dir":"..","notes":N,"unique":N,"dumped":N,"frame_notes":N} */
int  texture_dump_stats_json(char *buf, int cap);
uint64_t texture_pack_hash_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                                int u, int v, int w, int h);   /* texel id (for tests) */
uint64_t texture_pack_hash_palette(uint16_t texpage, uint16_t clut_x, uint16_t clut_y); /* CLUT id */

#ifdef __cplusplus
}
#endif
