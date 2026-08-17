/* texture_pack.c — see texture_pack.h. */
#include "texture_pack.h"
#include "png_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_texture_pack_active = 0;

static const uint16_t *s_vram = NULL;
static char s_dir[1024];
static FILE *s_tsv = NULL;
static int s_env_checked = 0;
static uint64_t s_notes = 0, s_frame_notes = 0, s_dumped = 0;
static uint64_t s_last_frame = ~0ull;
extern uint64_t s_frame_count;   /* debug_server.c */

/* seen-set: open addressing over 64-bit ids */
#define SEEN_CAP (1u << 16)
static uint64_t s_seen[SEEN_CAP];
static uint32_t s_seen_n = 0;
static uint32_t s_tex_unique = 0;
static uint64_t s_tex_seen[SEEN_CAP];
static int tex_seen_insert(uint64_t id) {
    if (id == 0) id = 1;
    uint32_t i = (uint32_t)(id ^ (id >> 31)) & (SEEN_CAP - 1);
    for (uint32_t k = 0; k < SEEN_CAP; k++) {
        if (s_tex_seen[i] == id) return 0;
        if (s_tex_seen[i] == 0) { if (s_tex_unique + 1 >= SEEN_CAP / 2) return 0; s_tex_seen[i] = id; s_tex_unique++; return 1; }
        i = (i + 1) & (SEEN_CAP - 1);
    }
    return 0;
}


static int seen_insert(uint64_t id) {          /* 1 = newly inserted */
    if (id == 0) id = 1;
    uint32_t i = (uint32_t)(id ^ (id >> 29)) & (SEEN_CAP - 1);
    for (uint32_t k = 0; k < SEEN_CAP; k++) {
        if (s_seen[i] == id) return 0;
        if (s_seen[i] == 0) {
            if (s_seen_n + 1 >= SEEN_CAP / 2) return 0;   /* full: stop dumping new ones */
            s_seen[i] = id; s_seen_n++; return 1;
        }
        i = (i + 1) & (SEEN_CAP - 1);
    }
    return 0;
}

void texture_pack_set_vram(const uint16_t *v) { s_vram = v; }

static void check_env(void) {
    if (s_env_checked) return;
    s_env_checked = 1;
    const char *d = getenv("PSX_TEXTURE_DUMP");
    if (d && d[0]) texture_dump_arm(d);
}

int texture_dump_arm(const char *dir) {
    if (!dir || !dir[0]) return 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/textures.tsv", dir);
    FILE *f = fopen(path, "a");
    if (!f) return 0;
    if (s_tsv) fclose(s_tsv);
    s_tsv = f;
    long pos = ftell(f);
    if (pos == 0)
        fprintf(f, "tex_id\tpal_id\tw\th\tbpp\ttexpage_x\ttexpage_y\tclut_x\tclut_y\tu\tv\tfirst_frame\n");
    fflush(f);
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    memset(s_seen, 0, sizeof s_seen); s_seen_n = 0;
    memset(s_tex_seen, 0, sizeof s_tex_seen); s_tex_unique = 0;
    s_notes = s_frame_notes = s_dumped = 0;
    g_texture_pack_active = 1;
    return 1;
}

void texture_dump_disarm(void) {
    if (s_tsv) { fclose(s_tsv); s_tsv = NULL; }
    s_dir[0] = 0;
    g_texture_pack_active = 0;
}

int texture_dump_stats_json(char *buf, int cap) {
    return snprintf(buf, (size_t)cap,
        "{\"active\":%d,\"dir\":\"%s\",\"notes\":%llu,\"unique\":%u,\"unique_texels\":%u,\"dumped\":%llu,\"frame_notes\":%llu}",
        g_texture_pack_active, s_dir, (unsigned long long)s_notes, s_seen_n, s_tex_unique,
        (unsigned long long)s_dumped, (unsigned long long)s_frame_notes);
}

/* ---- identity ---- */

static inline uint64_t fnv1a(uint64_t h, uint32_t v) {
    h ^= v & 0xFF;          h *= 0x100000001B3ull;
    h ^= (v >> 8) & 0xFF;   h *= 0x100000001B3ull;
    return h;
}

/* Fetch the raw texel (palette index for 4/8bpp, halfword for 15bpp). */
static inline uint32_t texel(uint16_t texpage, int depth, int u, int v) {
    const int px = (texpage & 15) * 64, py = ((texpage >> 4) & 1) * 256;
    u &= 0xFF; v &= 0xFF;
    const uint16_t hw = s_vram[(size_t)((py + v) & 511) * 1024 + ((px + (depth == 0 ? u >> 2 : depth == 1 ? u >> 1 : u)) & 1023)];
    if (depth == 0) return (hw >> ((u & 3) * 4)) & 0xF;
    if (depth == 1) return (hw >> ((u & 1) * 8)) & 0xFF;
    return hw;
}

/* Two hashes: the TEXEL id (indices + size + depth: the art) and the PALETTE
 * id (CLUT contents; 0 for 15bpp). A replacement pack keys on the texel id and
 * may carry per-palette variants; palette fades / flashes therefore do not
 * multiply the asset set. */
static void hash_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                      int u, int v, int w, int h, int depth, uint64_t *tex, uint64_t *pal) {
    uint64_t hh = 0xcbf29ce484222325ull;
    hh = fnv1a(hh, (uint32_t)depth); hh = fnv1a(hh, (uint32_t)w); hh = fnv1a(hh, (uint32_t)h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            hh = fnv1a(hh, texel(texpage, depth, u + x, v + y));
    *tex = hh;
    uint64_t hp = 0;
    if (depth < 2) {
        hp = 0xcbf29ce484222325ull;
        const int n = depth == 0 ? 16 : 256;
        for (int i = 0; i < n; i++)
            hp = fnv1a(hp, s_vram[(size_t)(clut_y & 511) * 1024 + ((clut_x + i) & 1023)]);
    }
    *pal = hp;
}

uint64_t texture_pack_hash_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                                int u, int v, int w, int h) {
    if (!s_vram) return 0;
    uint64_t t, p;
    hash_rect(texpage, clut_x, clut_y, u, v, w, h, (texpage >> 7) & 3, &t, &p);
    return t;
}
uint64_t texture_pack_hash_palette(uint16_t texpage, uint16_t clut_x, uint16_t clut_y) {
    if (!s_vram) return 0;
    uint64_t t, p;
    hash_rect(texpage, clut_x, clut_y, 0, 0, 1, 1, (texpage >> 7) & 3, &t, &p);
    return p;
}

/* ---- dump ---- */

static void dump_png(uint64_t id, uint64_t pal, uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                     int u, int v, int w, int h, int depth) {
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t t = texel(texpage, depth, u + x, v + y);
            uint16_t c = depth < 2 ? s_vram[(size_t)(clut_y & 511) * 1024 + ((clut_x + t) & 1023)] : (uint16_t)t;
            uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(((c) & 31) * 255 / 31);
            p[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31);
            p[2] = (uint8_t)(((c >> 10) & 31) * 255 / 31);
            p[3] = (c & 0x7FFF) == 0 && !(c & 0x8000) ? 0 : 255;   /* colour 0 = transparent */
        }
    char path[1300];
    snprintf(path, sizeof path, "%s/%016llx-%016llx.png", s_dir, (unsigned long long)id, (unsigned long long)pal);
    FILE *f = fopen(path, "wb");
    if (f) { png_write_rgba(f, rgba, (uint32_t)w, (uint32_t)h); fclose(f); s_dumped++; }
    free(rgba);
    if (s_tsv) {
        fprintf(s_tsv, "%016llx\t%016llx\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%llu\n",
                (unsigned long long)id, (unsigned long long)pal, w, h, depth == 0 ? 4 : depth == 1 ? 8 : 15,
                (texpage & 15) * 64, ((texpage >> 4) & 1) * 256, clut_x, clut_y, u & 0xFF, v & 0xFF,
                (unsigned long long)s_frame_count);
        fflush(s_tsv);
    }
}

static void note(uint16_t texpage, uint16_t clut_x, uint16_t clut_y, int u, int v, int w, int h) {
    if (!s_vram || w <= 0 || h <= 0) return;
    if (w > 256) w = 256;
    if (h > 256) h = 256;
    if (s_frame_count != s_last_frame) { s_last_frame = s_frame_count; s_frame_notes = 0; }
    s_notes++; s_frame_notes++;
    const int depth = (texpage >> 7) & 3;
    uint64_t id, pal;
    hash_rect(texpage, clut_x, clut_y, u, v, w, h, depth, &id, &pal);
    tex_seen_insert(id);
    if (s_dir[0] && seen_insert(id ^ (pal * 0x9E3779B97F4A7C15ull)))
        dump_png(id, pal, texpage, clut_x, clut_y, u, v, w, h, depth);
}

void texture_pack_note_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                            int u, int v, int w, int h) {
    check_env();
    if (!g_texture_pack_active) return;
    note(texpage, clut_x, clut_y, u, v, w, h);
}

void texture_pack_note_tri(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                           int u0, int v0, int u1, int v1, int u2, int v2) {
    check_env();
    if (!g_texture_pack_active) return;
    int umin = u0 < u1 ? (u0 < u2 ? u0 : u2) : (u1 < u2 ? u1 : u2);
    int umax = u0 > u1 ? (u0 > u2 ? u0 : u2) : (u1 > u2 ? u1 : u2);
    int vmin = v0 < v1 ? (v0 < v2 ? v0 : v2) : (v1 < v2 ? v1 : v2);
    int vmax = v0 > v1 ? (v0 > v2 ? v0 : v2) : (v1 > v2 ? v1 : v2);
    /* Games encode a quad's texel span either exclusively (u1 = u0 + 16) or
     * inclusively (u1 = u0 + 15) — Mega Man 8 mixes both in one primitive. A
     * span that is a whole multiple of 8 is taken as exclusive, else inclusive. */
    int w = umax - umin, h = vmax - vmin;
    if (w <= 0 || (w & 7)) w += 1;
    if (h <= 0 || (h & 7)) h += 1;
    note(texpage, clut_x, clut_y, umin, vmin, w, h);
}
