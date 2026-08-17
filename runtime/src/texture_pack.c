/* texture_pack.c — see texture_pack.h. */
#include "texture_pack.h"
#include "png_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int g_texture_pack_active = 0;
int g_texture_pack_replace = 0;

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


/* (texel, palette) pairs, with the pair's ids and a draw counter: pairs.tsv
 * lets tools pick the palette a texture is drawn with MOST of the time (the
 * settled one) rather than a fade/flash step. */
static uint64_t s_seen_tex[SEEN_CAP], s_seen_pal[SEEN_CAP];
static uint32_t s_seen_count[SEEN_CAP];

static int seen_insert(uint64_t id, uint64_t tex, uint64_t pal) {   /* 1 = newly inserted */
    if (id == 0) id = 1;
    uint32_t i = (uint32_t)(id ^ (id >> 29)) & (SEEN_CAP - 1);
    for (uint32_t k = 0; k < SEEN_CAP; k++) {
        if (s_seen[i] == id) { s_seen_count[i]++; return 0; }
        if (s_seen[i] == 0) {
            if (s_seen_n + 1 >= SEEN_CAP / 2) return 0;   /* full: stop dumping new ones */
            s_seen[i] = id; s_seen_tex[i] = tex; s_seen_pal[i] = pal; s_seen_count[i] = 1; s_seen_n++; return 1;
        }
        i = (i + 1) & (SEEN_CAP - 1);
    }
    return 0;
}

static void write_pairs_tsv(void) {
    if (!s_dir[0]) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/pairs.tsv", s_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "tex_id\tpal_id\tdraws\n");
    for (uint32_t i = 0; i < SEEN_CAP; i++)
        if (s_seen[i])
            fprintf(f, "%016llx\t%016llx\t%u\n", (unsigned long long)s_seen_tex[i],
                    (unsigned long long)s_seen_pal[i], s_seen_count[i]);
    fclose(f);
}

void texture_pack_set_vram(const uint16_t *v) { s_vram = v; }

static void check_env(void) {
    if (s_env_checked) return;
    s_env_checked = 1;
    const char *d = getenv("PSX_TEXTURE_DUMP");
    if (d && d[0]) texture_dump_arm(d);
    /* PSX_TEXTURE_PACK is applied by the host at startup (main.cpp), where it
     * overrides [video] texture_pack; nothing to do here. */
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
    memset(s_seen_count, 0, sizeof s_seen_count);
    memset(s_tex_seen, 0, sizeof s_tex_seen); s_tex_unique = 0;
    s_notes = s_frame_notes = s_dumped = 0;
    g_texture_pack_active = 1;
    return 1;
}

void texture_dump_disarm(void) {
    write_pairs_tsv();
    if (s_tsv) { fclose(s_tsv); s_tsv = NULL; }
    s_dir[0] = 0;
    g_texture_pack_active = 0;
}

int texture_dump_stats_json(char *buf, int cap) {
    write_pairs_tsv();     /* keep pairs.tsv current whenever someone looks */
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
static void hash_rect_used(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                           int u, int v, int w, int h, int depth, uint64_t *tex, uint64_t *pal,
                           uint64_t used[4]) {
    uint64_t hh = 0xcbf29ce484222325ull;
    hh = fnv1a(hh, (uint32_t)depth); hh = fnv1a(hh, (uint32_t)w); hh = fnv1a(hh, (uint32_t)h);
    if (used) used[0] = used[1] = used[2] = used[3] = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t t = texel(texpage, depth, u + x, v + y);
            hh = fnv1a(hh, t);
            if (used && depth < 2) used[(t >> 6) & 3] |= 1ull << (t & 63);   /* palette indices this rect draws */
        }
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
static void hash_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                      int u, int v, int w, int h, int depth, uint64_t *tex, uint64_t *pal) {
    hash_rect_used(texpage, clut_x, clut_y, u, v, w, h, depth, tex, pal, NULL);
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
    if (depth < 2) {   /* the CLUT the texture was seen with (BGR555 LE), for fade-aware packs */
        snprintf(path, sizeof path, "%s/%016llx-%016llx.clut", s_dir, (unsigned long long)id, (unsigned long long)pal);
        FILE *cf = fopen(path, "wb");
        if (cf) {
            const int n = depth == 0 ? 16 : 256;
            for (int i = 0; i < n; i++) {
                const uint16_t c = s_vram[(size_t)(clut_y & 511) * 1024 + ((clut_x + i) & 1023)];
                fputc(c & 0xFF, cf); fputc(c >> 8, cf);
            }
            fclose(cf);
        }
    }
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
    if (s_dir[0] && seen_insert(id ^ (pal * 0x9E3779B97F4A7C15ull), id, pal))
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

/* ---- replacement pack (B3) ---- */

/* Private, static PNG decoder (psx_window_icon.cpp keeps its own static copy too). */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"
#include <dirent.h>

typedef struct PackEntry {
    uint64_t tex, pal;        /* pal = 0: any palette */
    TexPackImage img;
    struct PackEntry *next;
} PackEntry;

#define PACK_BUCKETS 4096u
static PackEntry *s_pack[PACK_BUCKETS];
static int s_pack_n = 0;
static char s_pack_dir[1024];
static uint64_t s_lookups = 0, s_hits = 0, s_nofit = 0;   /* nofit = draws left native: palette not a fade of any variant */
static uint32_t s_pack_gen = 0;

uint32_t texture_pack_generation(void) { return s_pack_gen; }
void texture_pack_for_each(void (*fn)(TexPackImage *img, void *ctx), void *ctx) {
    for (unsigned b = 0; b < PACK_BUCKETS; b++)
        for (PackEntry *e = s_pack[b]; e; e = e->next) fn(&e->img, ctx);
}

static int parse_hex16(const char *p, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = p[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v; return 1;
}

void texture_pack_unload(void) {
    if (s_pack_n) {
        int used = 0;
        for (unsigned b = 0; b < PACK_BUCKETS; b++)
            for (const PackEntry *e = s_pack[b]; e; e = e->next) used += e->img.hits ? 1 : 0;
        fprintf(stdout, "psxrecomp: HD texture pack %s: %d of %d images were drawn (%llu lookups, %llu hits, %llu draws left native: palette is not a fade of any variant)\n",
                s_pack_dir, used, s_pack_n, (unsigned long long)s_lookups, (unsigned long long)s_hits, (unsigned long long)s_nofit);
    }
    for (unsigned b = 0; b < PACK_BUCKETS; b++) {
        PackEntry *e = s_pack[b];
        while (e) { PackEntry *n = e->next; free((void*)e->img.rgba); free(e); e = n; }
        s_pack[b] = NULL;
    }
    s_pack_n = 0; s_pack_dir[0] = 0; s_lookups = s_hits = s_nofit = 0;
    g_texture_pack_replace = 0;
    s_pack_gen++;
}

int texture_pack_load(const char *dir) {
    texture_pack_unload();
    if (!dir || !dir[0]) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        size_t len = strlen(n);
        uint64_t tex, pal = 0;
        if (len == 20 && strcmp(n + 16, ".png") == 0) {            /* <tex>.png */
            if (!parse_hex16(n, &tex)) continue;
        } else if (len == 37 && n[16] == '-' && strcmp(n + 33, ".png") == 0) {   /* <tex>-<pal>.png */
            if (!parse_hex16(n, &tex) || !parse_hex16(n + 17, &pal)) continue;
        } else continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, n);
        int w = 0, h = 0, comp = 0;
        unsigned char *px = stbi_load(path, &w, &h, &comp, 4);
        if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); continue; }
        PackEntry *e = (PackEntry*)calloc(1, sizeof *e);
        if (!e) { stbi_image_free(px); break; }
        e->tex = tex; e->pal = pal; e->img.w = w; e->img.h = h; e->img.rgba = px;
        e->img.atlas_x = e->img.atlas_y = -1;
        e->img.ref_n = 0; e->img.hits = 0;
        {
            /* <tex>.clut / <tex>-<pal>.clut: the CLUT this image was authored against */
            snprintf(path, sizeof path, "%s/%.*s.clut", dir, (int)(len - 4), n);
            FILE *cf = fopen(path, "rb");
            if (cf) {
                uint8_t raw[512];
                size_t got = fread(raw, 1, sizeof raw, cf);
                fclose(cf);
                if (got == 32 || got == 512) {
                    e->img.ref_n = (int)(got / 2);
                    for (int i = 0; i < e->img.ref_n; i++) e->img.ref_clut[i] = (uint16_t)(raw[i * 2] | (raw[i * 2 + 1] << 8));
                }
            }
        }
        unsigned b = (unsigned)(tex ^ (tex >> 23)) & (PACK_BUCKETS - 1);
        e->next = s_pack[b]; s_pack[b] = e; s_pack_n++;
    }
    closedir(d);
    snprintf(s_pack_dir, sizeof s_pack_dir, "%s", dir);
    g_texture_pack_replace = s_pack_n > 0;
    s_pack_gen++;
    return s_pack_n;
}

float texture_pack_palette_mod(const uint16_t *ref, int n, uint16_t clut_x, uint16_t clut_y, float mod[6]) {
    return texture_pack_palette_mod_used(ref, n, clut_x, clut_y, NULL, mod);
}
float texture_pack_palette_mod_used(const uint16_t *ref, int n, uint16_t clut_x, uint16_t clut_y,
                                    const uint64_t used[4], float mod[6]) {
    mod[0] = mod[1] = mod[2] = 1.0f; mod[3] = mod[4] = mod[5] = 0.0f;
    if (!ref || n <= 0 || !s_vram) return TEXPACK_NO_FIT;
#define TP_USED(i) (!used || (used[((i) >> 6) & 3] >> ((i) & 63)) & 1)
    /* Two uniform-fade models, fitted per channel over the entries the texture
     * uses (all of them without a mask). Entry 0 / a 0x0000 reference is the
     * transparent colour and never drawn, so it is skipped; an opaque-black
     * reference (0x8000, or a channel at 0) DOES take part — the subtractive
     * model can brighten it, which is how a solid tile authored dark reads the
     * live palette's colour.
     *   multiplicative  cur = ref * k          (dimming, brightness)
     *   subtractive     cur = clamp(ref - d)   (the classic PSX fade / flash:
     *                                           every channel steps by d,
     *                                           clamping at 0 / 31; the step
     *                                           comes from the unclamped
     *                                           entries, all clamped = fully
     *                                           black / white)
     * The model whose per-entry residual is smaller wins, and only if that
     * residual is below ~2 levels rms; otherwise the palette is a
     * permutation / cycle / recolour (TEXPACK_NO_FIT, mod = identity). */
    float mul[3] = {1, 1, 1}, sub[3] = {0, 0, 0};
    float err_mul = 0.0f, err_sub = 0.0f;
    int   m = 0;
    for (int k = 0; k < 3; k++) {
        float sr = 0, sc = 0, sd = 0; int cnt = 0, cntd = 0;
        for (int i = 0; i < n; i++) {
            const uint16_t r = ref[i];
            if (i == 0 || r == 0 || !TP_USED(i)) continue;
            const uint16_t c = s_vram[(size_t)(clut_y & 511) * 1024 + ((clut_x + i) & 1023)];
            const int rv = (r >> (5 * k)) & 31, cv = (c >> (5 * k)) & 31;
            sr += (float)rv; sc += (float)cv; cnt++;
            if (cv > 0 && cv < 31) { sd += (float)(rv - cv); cntd++; }   /* unclamped entries define the step */
        }
        if (!cnt) continue;
        mul[k] = sr > 0 ? sc / sr : (sc > 0 ? 99.0f : 1.0f);          /* ref channel all 0: mul cannot brighten */
        if (cntd) sub[k] = sd / (float)cntd;
        else      sub[k] = sc / (float)cnt <= 0.5f ? 31.0f : -31.0f;   /* all clamped: fully black / white */
        for (int i = 0; i < n; i++) {
            const uint16_t r = ref[i];
            if (i == 0 || r == 0 || !TP_USED(i)) continue;
            const uint16_t c = s_vram[(size_t)(clut_y & 511) * 1024 + ((clut_x + i) & 1023)];
            const int rv = (r >> (5 * k)) & 31, cv = (c >> (5 * k)) & 31;
            float pm = (float)rv * mul[k]; if (pm > 31.0f) pm = 31.0f;
            float ps = (float)rv - sub[k]; if (ps < 0.0f) ps = 0.0f; if (ps > 31.0f) ps = 31.0f;
            err_mul += ((float)cv - pm) * ((float)cv - pm);
            err_sub += ((float)cv - ps) * ((float)cv - ps);
            m++;
        }
    }
    if (!m) return 0.0f;                          /* nothing drawn but the transparent colour */
    /* residual per entry-channel; a fit is accepted below ~2 levels rms */
    const float rms_mul = (float)sqrt((double)err_mul / m);
    const float rms_sub = (float)sqrt((double)err_sub / m);
    if (rms_sub <= rms_mul && rms_sub < 2.0f) {
        for (int k = 0; k < 3; k++) { mod[k] = 1.0f; mod[3 + k] = -sub[k] * (255.0f / 31.0f); }
        return rms_sub;
    } else if (rms_mul < 2.0f) {
        for (int k = 0; k < 3; k++) { float f = mul[k]; if (f > 2.0f) f = 2.0f; if (f < 0.0f) f = 0.0f; mod[k] = f; mod[3 + k] = 0.0f; }
        return rms_mul;
    }
    return TEXPACK_NO_FIT;
#undef TP_USED
}

static int img_fits_rect(const TexPackImage *im, int w, int h) {
    return im->w % w == 0 && im->h % h == 0 && im->w / w == im->h / h;
}

const TexPackImage *texture_pack_lookup_rect_mod(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                                                 int u, int v, int w, int h, float mod[6]) {
    if (mod) { mod[0] = mod[1] = mod[2] = 1.0f; mod[3] = mod[4] = mod[5] = 0.0f; }
    if (!g_texture_pack_replace || !s_vram || w <= 0 || h <= 0) return NULL;
    if (w > 256) w = 256;
    if (h > 256) h = 256;
    uint64_t tex, pal, used[4];
    const int depth = (texpage >> 7) & 3;
    hash_rect_used(texpage, clut_x, clut_y, u, v, w, h, depth, &tex, &pal, used);
    s_lookups++;
    /* 1. the exact palette variant; 2. else the entry whose reference CLUT
     *    best explains the live one as a uniform fade — over the palette entries
     *    THIS rect actually uses (a solid tile only cares about its one index) —
     *    so a stage fade-in of a genuine recolour dims THAT recolour; 3. an entry
     *    with a reference that does NOT fit is a recolour the pack has no art
     *    for: draw the NATIVE texels rather than the wrong colours (degrade to
     *    the original, never guess); 4. entries without any reference palette
     *    (packs made before sidecars) draw as authored. Every candidate must be
     *    a whole multiple of the rect. */
    PackEntry *any = NULL, *best = NULL;
    int refs = 0;
    float best_rms = TEXPACK_NO_FIT, best_mod[6];
    PackEntry *chain = s_pack[(unsigned)(tex ^ (tex >> 23)) & (PACK_BUCKETS - 1)];
    for (PackEntry *e = chain; e; e = e->next) {
        if (e->tex != tex || !img_fits_rect(&e->img, w, h)) continue;
        if (e->pal == pal) { s_hits++; e->img.hits++; return &e->img; }
        if (e->pal == 0 && !any) any = e;
        if (e->img.ref_n > 0) refs++;
    }
    if (depth < 2 && refs) {
        float m[6];
        for (PackEntry *e = chain; e; e = e->next) {
            if (e->tex != tex || e->img.ref_n <= 0 || !img_fits_rect(&e->img, w, h)) continue;
            float rms = texture_pack_palette_mod_used(e->img.ref_clut, e->img.ref_n, clut_x, clut_y, used, m);
            if (rms < best_rms) { best_rms = rms; best = e; memcpy(best_mod, m, sizeof m); }
        }
        if (best) { s_hits++; best->img.hits++; if (mod) memcpy(mod, best_mod, sizeof best_mod); return &best->img; }
        s_nofit++;                       /* recolour without a variant: native texels */
        return NULL;
    }
    if (any)  { s_hits++; any->img.hits++; return &any->img; }
    return NULL;
}

const TexPackImage *texture_pack_lookup_rect(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                                             int u, int v, int w, int h) {
    return texture_pack_lookup_rect_mod(texpage, clut_x, clut_y, u, v, w, h, NULL);
}

int texture_pack_write_usage(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "tex_id\tpal_id\tw\th\thits\n");
    int rows = 0;
    for (unsigned b = 0; b < PACK_BUCKETS; b++)
        for (const PackEntry *e = s_pack[b]; e; e = e->next) {
            fprintf(f, "%016llx\t%016llx\t%d\t%d\t%llu\n", (unsigned long long)e->tex,
                    (unsigned long long)e->pal, e->img.w, e->img.h, (unsigned long long)e->img.hits);
            rows++;
        }
    fclose(f);
    return rows;
}

int texture_pack_stats_json(char *buf, int cap) {
    int used = 0;
    for (unsigned b = 0; b < PACK_BUCKETS; b++)
        for (const PackEntry *e = s_pack[b]; e; e = e->next) used += e->img.hits ? 1 : 0;
    return snprintf(buf, (size_t)cap, "{\"loaded\":%d,\"dir\":\"%s\",\"lookups\":%llu,\"hits\":%llu,\"used\":%d,\"native_recolour\":%llu}",
                    s_pack_n, s_pack_dir, (unsigned long long)s_lookups, (unsigned long long)s_hits, used,
                    (unsigned long long)s_nofit);
}
