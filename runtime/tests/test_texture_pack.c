/* test_texture_pack.c — texture identity (B1) + dump (B2), no game data. */
#include "texture_pack.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t s_frame_count = 7;   /* normally debug_server.c */

static uint16_t vram[1024 * 512];

static void put4(int px, int py, int u, int v, int idx) {   /* 4bpp texel at page (px,py) */
    uint16_t *hw = &vram[(py + v) * 1024 + px + (u >> 2)];
    const int sh = (u & 3) * 4;
    *hw = (uint16_t)((*hw & ~(0xF << sh)) | (idx << sh));
}
static uint16_t tpage(int px, int py, int depth) { return (uint16_t)((px / 64) | ((py / 256) << 4) | (depth << 7)); }

int main(void) {
    memset(vram, 0, sizeof vram);
    texture_pack_set_vram(vram);
    /* CLUT A at (0,480): 16 colours; CLUT B at (16,480): identical; CLUT C differs in entry 3 */
    for (int i = 0; i < 16; i++) { vram[480 * 1024 + i] = (uint16_t)(0x8000 | (i * 2)); vram[480 * 1024 + 16 + i] = (uint16_t)(0x8000 | (i * 2)); vram[480 * 1024 + 32 + i] = (uint16_t)(0x8000 | (i * 2)); }
    vram[480 * 1024 + 32 + 3] = 0x7FFF;
    /* texture T (16x16, 4bpp) at page (512,0) u=0..15,v=0..15 and the same texels at page (576,256) u=32,v=64 */
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        int idx = (x * 3 + y * 5) & 15;
        put4(512, 0, x, y, idx);
        put4(576, 256, 32 + x, 64 + y, idx);
    }
    const uint64_t t1 = texture_pack_hash_rect(tpage(512, 0, 0), 0, 480, 0, 0, 16, 16);
    const uint64_t t2 = texture_pack_hash_rect(tpage(576, 256, 0), 16, 480, 32, 64, 16, 16);
    assert(t1 != 0 && t1 == t2);                       /* same art, anywhere in VRAM, any CLUT */
    const uint64_t p1 = texture_pack_hash_palette(tpage(512, 0, 0), 0, 480);
    const uint64_t p2 = texture_pack_hash_palette(tpage(512, 0, 0), 16, 480);
    const uint64_t p3 = texture_pack_hash_palette(tpage(512, 0, 0), 32, 480);
    assert(p1 == p2 && p1 != p3);                      /* palette id follows the CLUT contents */
    /* a different texel -> different texel id, size matters too */
    put4(576, 256, 32 + 5, 64 + 5, 0);
    assert(texture_pack_hash_rect(tpage(576, 256, 0), 16, 480, 32, 64, 16, 16) != t1);
    assert(texture_pack_hash_rect(tpage(512, 0, 0), 0, 480, 0, 0, 16, 8) != t1);
    /* 8bpp: two 4bpp texels per byte -> the same halfwords read as 8bpp differ from 4bpp */
    assert(texture_pack_hash_rect(tpage(512, 0, 1), 0, 480, 0, 0, 8, 16) != t1);
    /* inactive: notes are free and do nothing */
    texture_pack_note_rect(tpage(512, 0, 0), 0, 480, 0, 0, 16, 16);
    char st[512];
    texture_dump_stats_json(st, sizeof st);
    assert(strstr(st, "\"active\":0") && strstr(st, "\"notes\":0"));
    /* dump: arm on a temp dir, note the same texture 3 times (twice via triangle bbox), one PNG */
    char dir[256];
    snprintf(dir, sizeof dir, "%s/psxrecomp-texture-pack-test", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir); assert(system(cmd) == 0);
    assert(texture_dump_arm(dir));
    texture_pack_note_rect(tpage(512, 0, 0), 0, 480, 0, 0, 16, 16);
    texture_pack_note_tri(tpage(512, 0, 0), 0, 480, 0, 0, 15, 0, 0, 15);      /* inclusive edge -> 16x16 */
    texture_pack_note_tri(tpage(512, 0, 0), 0, 480, 16, 16, 0, 0, 16, 0);     /* exclusive edge -> 16x16 */
    texture_pack_note_rect(tpage(512, 0, 0), 32, 480, 0, 0, 16, 16);          /* other palette -> second file */
    texture_dump_stats_json(st, sizeof st);
    assert(strstr(st, "\"unique_texels\":1") && strstr(st, "\"dumped\":2") && strstr(st, "\"notes\":4"));
    char path[400];
    snprintf(path, sizeof path, "%s/%016llx-%016llx.png", dir, (unsigned long long)t1, (unsigned long long)p1);
    FILE *f = fopen(path, "rb"); assert(f); uint8_t sig[8]; assert(fread(sig, 1, 8, f) == 8 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G'); fclose(f);
    snprintf(path, sizeof path, "%s/textures.tsv", dir);
    f = fopen(path, "r"); assert(f); int lines = 0; char line[512];
    while (fgets(line, sizeof line, f)) lines++;
    fclose(f);
    assert(lines == 3);   /* header + 2 rows */
    texture_dump_disarm();
    assert(!g_texture_pack_active);
    /* B3: the dump directory is a valid (1x) pack: load it, look the texture up */
    assert(texture_pack_load(dir) == 2);
    assert(g_texture_pack_replace);
    const TexPackImage *img = texture_pack_lookup_rect(tpage(512, 0, 0), 0, 480, 0, 0, 16, 16);
    assert(img && img->w == 16 && img->h == 16 && img->rgba);
    /* palette A -> exact variant; palette B (same contents) -> same id -> hit; a rect the
     * pack does not have -> NULL; a wrong-size request -> NULL */
    assert(texture_pack_lookup_rect(tpage(576, 256, 0), 16, 480, 32, 64, 16, 16) == NULL); /* texel 5,5 was changed above */
    assert(texture_pack_lookup_rect(tpage(512, 0, 0), 0, 480, 0, 0, 16, 8) == NULL);
    /* colour 0 -> transparent in the dumped PNG (index 0 = 0x8000|0 -> STP set, non-zero -> opaque
     * in this test), and every other pixel opaque */
    int opaque = 0; for (int i = 0; i < 256; i++) opaque += img->rgba[i * 4 + 3] == 255;
    assert(opaque == 256);
    texture_pack_stats_json(st, sizeof st);
    assert(strstr(st, "\"loaded\":2") && strstr(st, "\"hits\":1"));
    texture_pack_unload();
    assert(!g_texture_pack_replace && texture_pack_lookup_rect(tpage(512, 0, 0), 0, 480, 0, 0, 16, 16) == NULL);
    /* B9: fade-aware palette factor. Reference CLUT = CLUT A; live CLUT D at (48,480) = A at half
     * brightness -> factor ~0.5 per channel; against A itself -> exactly 1. */
    {
        uint16_t ref[16];
        for (int i = 0; i < 16; i++) {
            ref[i] = (uint16_t)(0x8000 | ((i * 2) & 31) | (((i * 2) & 31) << 5) | (((i * 2) & 31) << 10));
            vram[480 * 1024 + i] = ref[i];                                        /* CLUT A (rewrite) */
            const int hv = (i * 2) / 2;
            vram[480 * 1024 + 48 + i] = (uint16_t)(0x8000 | hv | (hv << 5) | (hv << 10)); /* CLUT D = half */
        }
        float m[6];
        texture_pack_palette_mod(ref, 16, 0, 480, m);
        assert(m[0] == 1.0f && m[1] == 1.0f && m[2] == 1.0f && m[3] == 0.0f);
        texture_pack_palette_mod(ref, 16, 48, 480, m);
        /* half brightness: the multiplicative fit wins (offset 0) with ~0.5 */
        assert(m[3] == 0.0f && m[0] > 0.4f && m[0] < 0.6f && m[1] > 0.4f && m[1] < 0.6f && m[2] > 0.4f && m[2] < 0.6f);
        /* subtractive PSX fade: CLUT E at (64,480) = A minus 6 per channel, clamped -> offset ~ -6*255/31 */
        for (int i = 0; i < 16; i++) {
            int v = (i * 2) & 31; int d = v - 6; if (d < 0) d = 0;
            vram[480 * 1024 + 64 + i] = (uint16_t)(0x8000 | d | (d << 5) | (d << 10));
        }
        texture_pack_palette_mod(ref, 16, 64, 480, m);
        assert(m[0] == 1.0f && m[3] < -40.0f && m[3] > -58.0f && m[4] == m[3] && m[5] == m[3]);
        /* a permuted palette (reversed) is NOT a fade: identity */
        for (int i = 0; i < 16; i++) vram[480 * 1024 + 80 + i] = ref[15 - i];
        texture_pack_palette_mod(ref, 16, 80, 480, m);
        assert(m[0] == 1.0f && m[1] == 1.0f && m[2] == 1.0f && m[3] == 0.0f);
        /* a palette-agnostic entry with a .clut sidecar (<tex>.clut = the CLUT the
         * art was authored against — here the grey ramp `ref` now live in CLUT A) */
        char src[400], dst[400];
        snprintf(src, sizeof src, "%s/%016llx-%016llx.clut", dir, (unsigned long long)t1, (unsigned long long)p1);
        snprintf(dst, sizeof dst, "%s/%016llx.clut", dir, (unsigned long long)t1);
        FILE *fi = fopen(src, "rb"); assert(fi);
        uint8_t raw[32]; assert(fread(raw, 1, 32, fi) == 32); fclose(fi);      /* dump wrote 32 bytes */
        FILE *fo = fopen(dst, "wb"); assert(fo);
        for (int i = 0; i < 16; i++) { uint8_t b2[2] = { (uint8_t)(ref[i] & 0xFF), (uint8_t)(ref[i] >> 8) }; fwrite(b2, 1, 2, fo); }
        fclose(fo);
        char any[400];
        snprintf(any, sizeof any, "%s/%016llx.png", dir, (unsigned long long)t1);
        snprintf(src, sizeof src, "%s/%016llx-%016llx.png", dir, (unsigned long long)t1, (unsigned long long)p1);
        assert(rename(src, any) == 0);           /* make it the palette-agnostic image */
        assert(texture_pack_load(dir) >= 1);
        /* the texture at (512,0) still has palette A live: exact hash -> factor 1 */
        const TexPackImage *im2 = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 0, 480, 0, 0, 16, 16, m);
        assert(im2 && im2->ref_n == 16 && m[0] == 1.0f);
        /* draw the same texels with the half-bright CLUT D -> ~0.5 factor, and hits counted */
        im2 = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 48, 480, 0, 0, 16, 16, m);
        assert(im2 && m[0] > 0.4f && m[0] < 0.6f && im2->hits == 2);
        texture_pack_stats_json(st, sizeof st);
        assert(strstr(st, "\"used\":1"));
        char up[400]; snprintf(up, sizeof up, "%s/usage.tsv", dir);
        assert(texture_pack_write_usage(up) >= 1);
        texture_pack_unload();

        /* Genuine recolour variant: <tex>-<palR>.png + .clut where R = the reversed
         * palette (not a fade of A). Live CLUT F = R minus 4 (a fade of the
         * RECOLOUR): the lookup must pick the variant, with a subtractive mod,
         * not the palette-agnostic entry. */
        uint16_t refR[16];
        for (int i = 0; i < 16; i++) refR[i] = ref[15 - i];
        for (int i = 0; i < 16; i++) vram[480 * 1024 + 80 + i] = refR[i];          /* CLUT R (80,480) */
        const uint64_t pR = texture_pack_hash_palette(tpage(512, 0, 0), 80, 480);
        char var[400], varc[400];
        snprintf(var, sizeof var, "%s/%016llx-%016llx.png", dir, (unsigned long long)t1, (unsigned long long)pR);
        snprintf(varc, sizeof varc, "%s/%016llx-%016llx.clut", dir, (unsigned long long)t1, (unsigned long long)pR);
        snprintf(cmd, sizeof cmd, "cp '%s' '%s'", any, var); assert(system(cmd) == 0);
        fo = fopen(varc, "wb"); assert(fo);
        for (int i = 0; i < 16; i++) { uint8_t b2[2] = { (uint8_t)(refR[i] & 0xFF), (uint8_t)(refR[i] >> 8) }; fwrite(b2, 1, 2, fo); }
        fclose(fo);
        for (int i = 0; i < 16; i++) {                                              /* CLUT F (96,480) = R - 4 */
            int r = refR[i] & 31, g = (refR[i] >> 5) & 31, b = (refR[i] >> 10) & 31;
            r -= 4; g -= 4; b -= 4; if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
            vram[480 * 1024 + 96 + i] = (uint16_t)(0x8000 | r | (g << 5) | (b << 10));
        }
        assert(texture_pack_load(dir) >= 2);
        const TexPackImage *imR = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 80, 480, 0, 0, 16, 16, m);
        const TexPackImage *imA = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 0, 480, 0, 0, 16, 16, m);
        assert(imR && imA && imR != imA);                                           /* exact variants */
        const TexPackImage *imF = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 96, 480, 0, 0, 16, 16, m);
        assert(imF == imR && m[0] == 1.0f && m[3] < -25.0f && m[3] > -40.0f);        /* fade of R -> R, offset ~ -4*255/31 */
        imF = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 48, 480, 0, 0, 16, 16, m);
        assert(imF == imA && m[0] > 0.4f && m[0] < 0.6f);                            /* half of A -> A */
        /* CLUT G (112,480): a genuine recolour of neither A nor R (entries scrambled,
         * not uniform) -> the pack has references but none fits: NATIVE texels
         * (NULL), not the authored art in the wrong colours; counted in stats. */
        for (int i = 0; i < 16; i++) vram[480 * 1024 + 112 + i] = (uint16_t)(0x8000 | ((i * 7) & 31) | (((i * 3) & 31) << 5) | (((31 - i) & 31) << 10));
        assert(texture_pack_lookup_rect_mod(tpage(512, 0, 0), 112, 480, 0, 0, 16, 16, m) == NULL);
        texture_pack_stats_json(st, sizeof st);
        assert(strstr(st, "\"native_recolour\":1"));
        /* used-index fit: a texture that only draws index 5 (solid tile) with CLUT G
         * where entry 5 equals A's entry 5 -> identical picture -> A, identity. */
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) put4(512, 0, 64 + x, y, 5);
        vram[480 * 1024 + 112 + 5] = ref[5];
        const uint64_t ts = texture_pack_hash_rect(tpage(512, 0, 0), 0, 480, 64, 0, 16, 16);
        assert(ts != t1);
        char sp[400], sc[400];
        snprintf(sp, sizeof sp, "%s/%016llx.png", dir, (unsigned long long)ts);
        snprintf(sc, sizeof sc, "%s/%016llx.clut", dir, (unsigned long long)ts);
        snprintf(cmd, sizeof cmd, "cp '%s' '%s' && cp '%s' '%s'", any, sp, dst, sc); assert(system(cmd) == 0);
        assert(texture_pack_load(dir) >= 3);
        const TexPackImage *imS = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 112, 480, 64, 0, 16, 16, m);
        assert(imS && m[0] == 1.0f && m[3] == 0.0f);
        /* one used entry: ANY change of it is a per-channel fade (the art has one
         * colour; the modulation reproduces the live colour exactly) */
        vram[480 * 1024 + 112 + 5] = (uint16_t)(0x8000 | 1 | (20 << 5) | (5 << 10));
        imS = texture_pack_lookup_rect_mod(tpage(512, 0, 0), 112, 480, 64, 0, 16, 16, m);
        assert(imS && m[3] < 0.0f && m[4] > 0.0f && m[5] < 0.0f);
        /* two used entries moving in opposite directions -> not a fade -> native */
        for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) put4(512, 0, 64 + x, y, 6);
        vram[480 * 1024 + 112 + 5] = (uint16_t)(0x8000 | 1 | (1 << 5) | (1 << 10));
        vram[480 * 1024 + 112 + 6] = 0xFFFF;
        const uint64_t ts2 = texture_pack_hash_rect(tpage(512, 0, 0), 0, 480, 64, 0, 16, 16);
        snprintf(cmd, sizeof cmd, "cp '%s' '%s/%016llx.png' && cp '%s' '%s/%016llx.clut'", any, dir, (unsigned long long)ts2, dst, dir, (unsigned long long)ts2);
        assert(system(cmd) == 0);
        assert(texture_pack_load(dir) >= 4);
        assert(texture_pack_lookup_rect_mod(tpage(512, 0, 0), 112, 480, 64, 0, 16, 16, m) == NULL);
        assert(texture_pack_lookup_rect_mod(tpage(512, 0, 0), 0, 480, 64, 0, 16, 16, m) != NULL);   /* palette A itself */
        texture_pack_unload();
    }
    puts("texture_pack_test: OK");
    return 0;
}
