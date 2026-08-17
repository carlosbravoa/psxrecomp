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
 * Dump mode ({"cmd":"texture_dump","op":"arm","dir":D} or PSX_TEXTURE_DUMP=D)
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

extern int g_texture_pack_active;   /* 0 = every note returns immediately */

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
