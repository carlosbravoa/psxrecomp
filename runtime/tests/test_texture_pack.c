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
    puts("texture_pack_test: OK");
    return 0;
}
