/* video_filter.c — CPU reference implementations of the present-time video
 * filters (see video_filter.h for scope, provenance and the parity contract
 * with the OpenGL shaders in gpu_gl_renderer.c).
 *
 * All pixels are ARGB8888 in host order (0xAARRGGBB). Colour equality is on
 * the RGB24 payload; alpha is ignored on input and forced to 0xFF on output.
 * Every neighbourhood fetch is clamped to the source rectangle, so the
 * filters never read outside [0,w) x [0,h) and the outermost source pixels
 * simply see themselves repeated — the same rule the shaders apply with
 * clamp-to-edge texel fetches, which keeps the two paths comparable at the
 * borders too. */

#include "video_filter.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Portable case-insensitive compare (MSVC has no <strings.h>). */
static int ci_eq(const char* a, const char* b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
        if (ca == 0) return 1;
    }
}

/* ---- names / metadata --------------------------------------------------- */

typedef struct {
    const char* name;
    const char* label;
    int         scale;      /* upscaler factor (1 = final-pass / none)      */
} VfInfo;

static const VfInfo s_info[VF_COUNT] = {
    [VF_NONE]        = { "none",        "None",         1 },
    [VF_SHARP]       = { "sharp",       "Sharp",        1 },
    [VF_SCALE2X]     = { "scale2x",     "Scale2x",      2 },
    [VF_SCALE3X]     = { "scale3x",     "Scale3x",      3 },
    [VF_SAI2X]       = { "2xsai",       "2xSaI",        2 },
    [VF_SUPER_SAI2X] = { "super2xsai",  "Super 2xSaI",  2 },
    [VF_SUPER_EAGLE] = { "supereagle",  "Super Eagle",  2 },
    [VF_XBR2X]       = { "xbr2x",       "xBR 2x",       2 },
    [VF_XBR3X]       = { "xbr3x",       "xBR 3x",       3 },
    [VF_XBR4X]       = { "xbr4x",       "xBR 4x",       4 },
    [VF_SCANLINES]   = { "scanlines",   "Scanlines",    1 },
    [VF_CRT]         = { "crt",         "CRT",          1 },
};

static int vf_valid(int kind) { return kind >= 0 && kind < VF_COUNT; }

const char* video_filter_name(int kind)  { return vf_valid(kind) ? s_info[kind].name  : "none"; }
const char* video_filter_label(int kind) { return vf_valid(kind) ? s_info[kind].label : "None"; }
int video_filter_scale(int kind)         { return vf_valid(kind) ? s_info[kind].scale : 1; }
int video_filter_is_upscaler(int kind)   { return video_filter_scale(kind) > 1; }

int video_filter_from_name(const char* name, int* out) {
    if (!name || !out) return 0;
    for (int k = 0; k < VF_COUNT; k++) {
        if (ci_eq(name, s_info[k].name)) { *out = k; return 1; }
    }
    /* Aliases people actually type. */
    static const struct { const char* alias; int kind; } al[] = {
        { "off", VF_NONE }, { "raw", VF_NONE }, { "0", VF_NONE },
        { "sharp-bilinear", VF_SHARP }, { "sharp_bilinear", VF_SHARP },
        { "pixel-perfect", VF_SHARP },
        { "epx", VF_SCALE2X }, { "advmame2x", VF_SCALE2X }, { "advmame3x", VF_SCALE3X },
        { "sai", VF_SAI2X }, { "2xsai", VF_SAI2X }, { "sai2x", VF_SAI2X },
        { "super-2xsai", VF_SUPER_SAI2X }, { "super_2xsai", VF_SUPER_SAI2X },
        { "supersai", VF_SUPER_SAI2X },
        { "super-eagle", VF_SUPER_EAGLE }, { "super_eagle", VF_SUPER_EAGLE },
        { "eagle", VF_SUPER_EAGLE },
        { "xbr", VF_XBR2X }, { "xbr-2x", VF_XBR2X }, { "xbr-3x", VF_XBR3X },
        { "xbr-4x", VF_XBR4X }, { "xbr-lv2", VF_XBR2X },
        { "scanline", VF_SCANLINES }, { "crt-lottes", VF_CRT },
    };
    for (size_t i = 0; i < sizeof al / sizeof al[0]; i++) {
        if (ci_eq(name, al[i].alias)) { *out = al[i].kind; return 1; }
    }
    return 0;
}

/* ---- runtime selection -------------------------------------------------- */

static volatile int s_kind = VF_NONE;

void video_filter_set(int kind) { s_kind = vf_valid(kind) ? kind : VF_NONE; }
int  video_filter_get(void)     { return s_kind; }

static VideoScanlineParams s_scan = { VF_SCAN_OPACITY_DEFAULT, VF_SCAN_SIZE_DEFAULT, VF_SCAN_GLOW_DEFAULT };
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
void video_filter_scanline_set(const VideoScanlineParams* p) {
    if (!p) return;
    s_scan.opacity = clampf(p->opacity, 0.0f, 1.0f);
    s_scan.size    = clampf(p->size, 0.10f, 0.80f);
    s_scan.glow    = clampf(p->glow, 0.0f, 1.0f);
}
void video_filter_scanline_get(VideoScanlineParams* out) { if (out) *out = s_scan; }

int video_filter_cpu_scale(int kind) {
    if (!vf_valid(kind) || kind == VF_NONE) return 1;
    if (video_filter_is_upscaler(kind)) return video_filter_scale(kind);
    return VF_SW_PRESCALE;
}

/* ---- pixel helpers ------------------------------------------------------ */

#define RGB_MASK 0x00FFFFFFu
#define OPAQUE   0xFF000000u

/* Clamped fetch of the RGB24 payload. */
static inline uint32_t fetch(const uint32_t* src, int pitch, int w, int h, int x, int y) {
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return src[(size_t)y * (size_t)pitch + (size_t)x] & RGB_MASK;
}

/* Per-channel (a+b+1)/2 — round-half-up average, matches a unorm store of
 * the GPU's float mix(a,b,0.5) on every tie-rounding-up implementation. */
static inline uint32_t avg2(uint32_t a, uint32_t b) {
    return ((a & b) + (((a ^ b) & 0xFEFEFEu) >> 1) + ((a ^ b) & 0x010101u)) & RGB_MASK;
}

/* Per-channel (a+b+c+d+2)/4. Low 2 bits of four channels sum to at most 12
 * (+2 rounding = 14) which fits below the next channel's field, so the whole
 * thing can be done in packed 32-bit arithmetic without cross-talk. */
static inline uint32_t avg4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t hi = ((a >> 2) & 0x3F3F3Fu) + ((b >> 2) & 0x3F3F3Fu) +
                  ((c >> 2) & 0x3F3F3Fu) + ((d >> 2) & 0x3F3F3Fu);
    uint32_t lo = (a & 0x030303u) + (b & 0x030303u) + (c & 0x030303u) +
                  (d & 0x030303u) + 0x020202u;
    return (hi + ((lo >> 2) & 0x030303u)) & RGB_MASK;
}

static inline void put(uint32_t* dst, int pitch, int x, int y, uint32_t rgb) {
    dst[(size_t)y * (size_t)pitch + (size_t)x] = rgb | OPAQUE;
}

/* ---- nearest prescale (+ scanline mask) — software-present final passes -- */

/* Row weights for one source line expanded to VF_SW_PRESCALE rows, in
 * 1/256ths. SCANLINES: bright beam, dark gap. CRT: softer beam roll-off. */
static const uint32_t s_row_w_sharp[VF_SW_PRESCALE]     = { 256, 256, 256 };
static const uint32_t s_row_w_crt[VF_SW_PRESCALE]       = { 190, 256, 150 };
/* Scanline rows follow the live parameters: the bottom third is the gap
 * (darkened by opacity, partially refilled by glow), the core is brightened
 * by glow. A coarse stand-in for the shader's per-pixel beam profile. */
static void scanline_row_weights(uint32_t w[VF_SW_PRESCALE]) {
    const float core = 1.0f + 0.35f * s_scan.glow;
    const float gap  = (1.0f - s_scan.opacity) + 0.35f * s_scan.glow * s_scan.opacity;
    const float mid  = 1.0f + 0.15f * s_scan.glow;
    w[0] = (uint32_t)(mid  * 256.0f + 0.5f);
    w[1] = (uint32_t)(core * 256.0f + 0.5f);
    w[2] = (uint32_t)(gap  * 256.0f + 0.5f);
}

static inline uint32_t scale_rgb(uint32_t rgb, uint32_t wgt) {
    if (wgt == 256) return rgb;
    uint32_t r = ((rgb >> 16) & 0xFF) * wgt >> 8;
    uint32_t g = ((rgb >> 8) & 0xFF) * wgt >> 8;
    uint32_t b = (rgb & 0xFF) * wgt >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

static void prescale_rows(const uint32_t* src, int sp, int w, int h,
                          uint32_t* dst, int dp, const uint32_t* row_w) {
    const int N = VF_SW_PRESCALE;
    for (int y = 0; y < h; y++) {
        const uint32_t* s = src + (size_t)y * (size_t)sp;
        for (int r = 0; r < N; r++) {
            uint32_t* d = dst + ((size_t)y * N + r) * (size_t)dp;
            const uint32_t wgt = row_w[r];
            for (int x = 0; x < w; x++) {
                const uint32_t c = scale_rgb(s[x] & RGB_MASK, wgt) | OPAQUE;
                for (int k = 0; k < N; k++) d[x * N + k] = c;
            }
        }
    }
}

/* ---- Scale2x / Scale3x (EPX) --------------------------------------------- */

static void scale2x(const uint32_t* src, int sp, int w, int h, uint32_t* dst, int dp) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t B = fetch(src, sp, w, h, x, y - 1);
            const uint32_t D = fetch(src, sp, w, h, x - 1, y);
            const uint32_t E = fetch(src, sp, w, h, x, y);
            const uint32_t F = fetch(src, sp, w, h, x + 1, y);
            const uint32_t H = fetch(src, sp, w, h, x, y + 1);
            uint32_t E0 = E, E1 = E, E2 = E, E3 = E;
            if (B != H && D != F) {
                if (D == B) E0 = D;
                if (B == F) E1 = F;
                if (D == H) E2 = D;
                if (H == F) E3 = F;
            }
            put(dst, dp, 2 * x,     2 * y,     E0);
            put(dst, dp, 2 * x + 1, 2 * y,     E1);
            put(dst, dp, 2 * x,     2 * y + 1, E2);
            put(dst, dp, 2 * x + 1, 2 * y + 1, E3);
        }
    }
}

static void scale3x(const uint32_t* src, int sp, int w, int h, uint32_t* dst, int dp) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t A = fetch(src, sp, w, h, x - 1, y - 1);
            const uint32_t B = fetch(src, sp, w, h, x,     y - 1);
            const uint32_t C = fetch(src, sp, w, h, x + 1, y - 1);
            const uint32_t D = fetch(src, sp, w, h, x - 1, y);
            const uint32_t E = fetch(src, sp, w, h, x,     y);
            const uint32_t F = fetch(src, sp, w, h, x + 1, y);
            const uint32_t G = fetch(src, sp, w, h, x - 1, y + 1);
            const uint32_t H = fetch(src, sp, w, h, x,     y + 1);
            const uint32_t I = fetch(src, sp, w, h, x + 1, y + 1);
            uint32_t o[9] = { E, E, E, E, E, E, E, E, E };
            if (B != H && D != F) {
                if (D == B) o[0] = D;
                if ((D == B && E != C) || (B == F && E != A)) o[1] = B;
                if (B == F) o[2] = F;
                if ((D == B && E != G) || (D == H && E != A)) o[3] = D;
                if ((B == F && E != I) || (H == F && E != C)) o[5] = F;
                if (D == H) o[6] = D;
                if ((D == H && E != I) || (H == F && E != G)) o[7] = H;
                if (H == F) o[8] = F;
            }
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++)
                    put(dst, dp, 3 * x + i, 3 * y + j, o[j * 3 + i]);
        }
    }
}

/* ---- 2xSaI family --------------------------------------------------------- */

/* Kreed's vote helpers. GetResult1 == GetResult; GetResult2 is its negation. */
static inline int get_result(uint32_t A, uint32_t B, uint32_t C, uint32_t D) {
    int x = 0, y = 0, r = 0;
    if (A == C) x++; else if (B == C) y++;
    if (A == D) x++; else if (B == D) y++;
    if (x <= 1) r++;
    if (y <= 1) r--;
    return r;
}

static void sai2x(const uint32_t* src, int sp, int w, int h, uint32_t* dst, int dp) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /*  I E F J
             *  G A B K
             *  H C D L
             *  M N O .   — A is the current pixel (P is unused by the rules). */
            const uint32_t I = fetch(src, sp, w, h, x - 1, y - 1);
            const uint32_t E = fetch(src, sp, w, h, x,     y - 1);
            const uint32_t F = fetch(src, sp, w, h, x + 1, y - 1);
            const uint32_t J = fetch(src, sp, w, h, x + 2, y - 1);
            const uint32_t G = fetch(src, sp, w, h, x - 1, y);
            const uint32_t A = fetch(src, sp, w, h, x,     y);
            const uint32_t B = fetch(src, sp, w, h, x + 1, y);
            const uint32_t K = fetch(src, sp, w, h, x + 2, y);
            const uint32_t H = fetch(src, sp, w, h, x - 1, y + 1);
            const uint32_t C = fetch(src, sp, w, h, x,     y + 1);
            const uint32_t D = fetch(src, sp, w, h, x + 1, y + 1);
            const uint32_t L = fetch(src, sp, w, h, x + 2, y + 1);
            const uint32_t M = fetch(src, sp, w, h, x - 1, y + 2);
            const uint32_t N = fetch(src, sp, w, h, x,     y + 2);
            const uint32_t O = fetch(src, sp, w, h, x + 1, y + 2);
            uint32_t p0, p1, p2;   /* right, below, diagonal */

            if (A == D && B != C) {
                if ((A == E && B == L) || (A == C && A == F && B != E && B == J)) p0 = A;
                else p0 = avg2(A, B);
                if ((A == G && C == O) || (A == B && A == H && G != C && C == M)) p1 = A;
                else p1 = avg2(A, C);
                p2 = A;
            } else if (B == C && A != D) {
                if ((B == F && A == H) || (B == E && B == D && A != F && A == I)) p0 = B;
                else p0 = avg2(A, B);
                if ((C == H && A == F) || (C == G && C == D && A != H && A == I)) p1 = C;
                else p1 = avg2(A, C);
                p2 = B;
            } else if (A == D && B == C) {
                if (A == B) {
                    p0 = p1 = p2 = A;
                } else {
                    int r = 0;
                    p1 = avg2(A, C);
                    p0 = avg2(A, B);
                    r += get_result(A, B, G, E);
                    r -= get_result(B, A, K, F);
                    r -= get_result(B, A, H, N);
                    r += get_result(A, B, L, O);
                    if (r > 0) p2 = A;
                    else if (r < 0) p2 = B;
                    else p2 = avg4(A, B, C, D);
                }
            } else {
                p2 = avg4(A, B, C, D);
                if (A == C && A == F && B != E && B == J) p0 = A;
                else if (B == E && B == D && A != F && A == I) p0 = B;
                else p0 = avg2(A, B);
                if (A == B && A == H && G != C && C == M) p1 = A;
                else if (C == G && C == D && A != H && A == I) p1 = C;
                else p1 = avg2(A, C);
            }
            put(dst, dp, 2 * x,     2 * y,     A);
            put(dst, dp, 2 * x + 1, 2 * y,     p0);
            put(dst, dp, 2 * x,     2 * y + 1, p1);
            put(dst, dp, 2 * x + 1, 2 * y + 1, p2);
        }
    }
}

/* Shared 4x4 neighbourhood of Super 2xSaI / Super Eagle:
 *   B0 B1 B2 B3
 *   c4 c5 c6 S2      c5 is the current pixel.
 *   c1 c2 c3 S1
 *   A0 A1 A2 A3 */
typedef struct {
    uint32_t B0, B1, B2, B3, c4, c5, c6, S2, c1, c2, c3, S1, A0, A1, A2, A3;
} SaiNbr;

static inline void sai_nbr(const uint32_t* src, int sp, int w, int h, int x, int y, SaiNbr* n) {
    n->B0 = fetch(src, sp, w, h, x - 1, y - 1); n->B1 = fetch(src, sp, w, h, x, y - 1);
    n->B2 = fetch(src, sp, w, h, x + 1, y - 1); n->B3 = fetch(src, sp, w, h, x + 2, y - 1);
    n->c4 = fetch(src, sp, w, h, x - 1, y);     n->c5 = fetch(src, sp, w, h, x, y);
    n->c6 = fetch(src, sp, w, h, x + 1, y);     n->S2 = fetch(src, sp, w, h, x + 2, y);
    n->c1 = fetch(src, sp, w, h, x - 1, y + 1); n->c2 = fetch(src, sp, w, h, x, y + 1);
    n->c3 = fetch(src, sp, w, h, x + 1, y + 1); n->S1 = fetch(src, sp, w, h, x + 2, y + 1);
    n->A0 = fetch(src, sp, w, h, x - 1, y + 2); n->A1 = fetch(src, sp, w, h, x, y + 2);
    n->A2 = fetch(src, sp, w, h, x + 1, y + 2); n->A3 = fetch(src, sp, w, h, x + 2, y + 2);
}

static void super_sai2x(const uint32_t* src, int sp, int w, int h, uint32_t* dst, int dp) {
    SaiNbr n;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            sai_nbr(src, sp, w, h, x, y, &n);
            uint32_t p1a, p1b, p2a, p2b;
            if (n.c2 == n.c6 && n.c5 != n.c3) {
                p2b = p1b = n.c2;
            } else if (n.c5 == n.c3 && n.c2 != n.c6) {
                p2b = p1b = n.c5;
            } else if (n.c5 == n.c3 && n.c2 == n.c6) {
                int r = 0;
                r += get_result(n.c6, n.c5, n.c1, n.A1);
                r += get_result(n.c6, n.c5, n.c4, n.B1);
                r += get_result(n.c6, n.c5, n.A2, n.S1);
                r += get_result(n.c6, n.c5, n.B2, n.S2);
                if (r > 0) p2b = p1b = n.c6;
                else if (r < 0) p2b = p1b = n.c5;
                else p2b = p1b = avg2(n.c5, n.c6);
            } else {
                if (n.c6 == n.c3 && n.c3 == n.A1 && n.c2 != n.A2 && n.c3 != n.A0)
                    p2b = avg4(n.c3, n.c3, n.c3, n.c2);
                else if (n.c5 == n.c2 && n.c2 == n.A2 && n.A1 != n.c3 && n.c2 != n.A3)
                    p2b = avg4(n.c2, n.c2, n.c2, n.c3);
                else p2b = avg2(n.c2, n.c3);

                if (n.c6 == n.c3 && n.c6 == n.B1 && n.c5 != n.B2 && n.c6 != n.B0)
                    p1b = avg4(n.c6, n.c6, n.c6, n.c5);
                else if (n.c5 == n.c2 && n.c5 == n.B2 && n.B1 != n.c6 && n.c5 != n.B3)
                    p1b = avg4(n.c6, n.c5, n.c5, n.c5);
                else p1b = avg2(n.c5, n.c6);
            }
            if (n.c5 == n.c3 && n.c2 != n.c6 && n.c4 == n.c5 && n.c5 != n.A2)
                p2a = avg2(n.c2, n.c5);
            else if (n.c5 == n.c1 && n.c6 == n.c5 && n.c4 != n.c2 && n.c5 != n.A0)
                p2a = avg2(n.c2, n.c5);
            else p2a = n.c2;

            if (n.c2 == n.c6 && n.c5 != n.c3 && n.c1 == n.c2 && n.c2 != n.B2)
                p1a = avg2(n.c2, n.c5);
            else if (n.c4 == n.c2 && n.c3 == n.c2 && n.c1 != n.c5 && n.c2 != n.B0)
                p1a = avg2(n.c2, n.c5);
            else p1a = n.c5;

            put(dst, dp, 2 * x,     2 * y,     p1a);
            put(dst, dp, 2 * x + 1, 2 * y,     p1b);
            put(dst, dp, 2 * x,     2 * y + 1, p2a);
            put(dst, dp, 2 * x + 1, 2 * y + 1, p2b);
        }
    }
}

static void super_eagle(const uint32_t* src, int sp, int w, int h, uint32_t* dst, int dp) {
    SaiNbr n;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            sai_nbr(src, sp, w, h, x, y, &n);
            uint32_t p1a, p1b, p2a, p2b;
            if (n.c2 == n.c6 && n.c5 != n.c3) {
                p1b = p2a = n.c2;
                if (n.c1 == n.c2 || n.c6 == n.B2) {
                    p1a = avg2(n.c2, n.c5);
                    p1a = avg2(n.c2, p1a);
                } else p1a = avg2(n.c5, n.c6);
                if (n.c6 == n.S2 || n.c2 == n.A1) {
                    p2b = avg2(n.c2, n.c3);
                    p2b = avg2(n.c2, p2b);
                } else p2b = avg2(n.c2, n.c3);
            } else if (n.c5 == n.c3 && n.c2 != n.c6) {
                p2b = p1a = n.c5;
                if (n.B1 == n.c5 || n.c3 == n.S1) {
                    p1b = avg2(n.c5, n.c6);
                    p1b = avg2(n.c5, p1b);
                } else p1b = avg2(n.c5, n.c6);
                if (n.c3 == n.A2 || n.c4 == n.c5) {
                    p2a = avg2(n.c5, n.c2);
                    p2a = avg2(n.c5, p2a);
                } else p2a = avg2(n.c2, n.c3);
            } else if (n.c5 == n.c3 && n.c2 == n.c6) {
                int r = 0;
                r += get_result(n.c6, n.c5, n.c1, n.A1);
                r += get_result(n.c6, n.c5, n.c4, n.B1);
                r += get_result(n.c6, n.c5, n.A2, n.S1);
                r += get_result(n.c6, n.c5, n.B2, n.S2);
                if (r > 0) {
                    p1b = p2a = n.c2;
                    p1a = p2b = avg2(n.c5, n.c6);
                } else if (r < 0) {
                    p2b = p1a = n.c5;
                    p1b = p2a = avg2(n.c5, n.c6);
                } else {
                    p2b = p1a = n.c5;
                    p1b = p2a = n.c2;
                }
            } else {
                p2b = p1a = avg2(n.c2, n.c6);
                p2b = avg4(n.c3, n.c3, n.c3, p2b);
                p1a = avg4(n.c5, n.c5, n.c5, p1a);
                p2a = p1b = avg2(n.c5, n.c2);
                p2a = avg4(n.c2, n.c2, n.c2, p2a);
                p1b = avg4(n.c6, n.c6, n.c6, p1b);
            }
            put(dst, dp, 2 * x,     2 * y,     p1a);
            put(dst, dp, 2 * x + 1, 2 * y,     p1b);
            put(dst, dp, 2 * x,     2 * y + 1, p2a);
            put(dst, dp, 2 * x + 1, 2 * y + 1, p2b);
        }
    }
}

/* ---- xBR level 2 ---------------------------------------------------------- */

/* Per-output-pixel formulation (Hyllian's xbr-lv2 "smooth" variant): the four
 * corners of a source pixel are handled as four rotations of one rule set.
 * Everything that does not depend on the sub-pixel position is computed once
 * per source pixel (edge flags, pixel-select flags); the per-sub-pixel line
 * inequalities fx45/fx30/fx60/fx45i depend only on (N, sub_x, sub_y) and are
 * tabulated once per scale. Colours are float 0..1 (matches the shader),
 * lumas are on the 0..48 xBR scale. */

typedef struct { float r, g, b; } Rgb;

/* xBR works on colours as float INTEGERS 0..255 (exact), blends with dyadic
 * weights (multiples of 1/4, see xbr_sub_tables), so res1/res2 and their
 * distances to E are exact on the CPU and on the GPU alike; only the final
 * 8-bit store rounds (ties may land either way: the 1-LSB parity slack). */
static inline Rgb to_rgb(uint32_t c) {
    Rgb o;
    o.r = (float)((c >> 16) & 0xFF);
    o.g = (float)((c >> 8) & 0xFF);
    o.b = (float)(c & 0xFF);
    return o;
}
/* Integer luma of a packed pixel: 299R + 587G + 114B (Rec.601 weights, the
 * ones xBR uses), exact in float — see VF_XBR_EQ_THRESHOLD. */
static inline float luma_px(uint32_t c) {
    return (float)(((c >> 16) & 0xFF) * 299u + ((c >> 8) & 0xFF) * 587u + (c & 0xFF) * 114u);
}
static inline float df(float a, float b) { return fabsf(a - b); }
static inline int eqf(float a, float b) { return df(a, b) < VF_XBR_EQ_THRESHOLD; }
static inline float wd(float a, float b, float c, float d, float e, float f, float g, float h) {
    return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0f * df(g, h);
}
static inline Rgb mixc(Rgb a, Rgb b, float t) {
    Rgb o; o.r = a.r + (b.r - a.r) * t; o.g = a.g + (b.g - a.g) * t; o.b = a.b + (b.b - a.b) * t; return o;
}
static inline float c_df(Rgb a, Rgb b) { return fabsf(a.r - b.r) + fabsf(a.g - b.g) + fabsf(a.b - b.b); }
static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static inline float clamp255(float v) { return v < 0.f ? 0.f : (v > 255.f ? 255.f : v); }
static inline uint32_t from_rgb(Rgb c) {
    int r = (int)(clamp255(c.r) + 0.5f);
    int g = (int)(clamp255(c.g) + 0.5f);
    int b = (int)(clamp255(c.b) + 0.5f);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
/* Every xBR blend weight is mathematically a multiple of 1/8: the line
 * inequalities evaluate at sub-pixel centres (s+0.5)/N with delta 1/N, so N
 * cancels except through the Ci = 1/4 inner offset, which contributes N/8.
 * Snap the float result to that grid so CPU and GPU agree exactly even where
 * 1/N is inexact (N=3), and every blend stays exact dyadic arithmetic. */
static inline float snap_eighth(float v) { return floorf(v * 8.0f + 0.5f) * 0.125f; }

/* Sub-pixel tables: fx45[N][N][4] etc. Filled per call (cheap). */
typedef struct {
    float f45[4], f45i[4], f30[4], f60[4];
} XbrSub;

static void xbr_sub_tables(int N, XbrSub* tab /* N*N entries */) {
    static const float Ao[4] = { 1.f, -1.f, -1.f,  1.f };
    static const float Bo[4] = { 1.f,  1.f, -1.f, -1.f };
    static const float Co[4] = { 1.5f, 0.5f, -0.5f, 0.5f };
    static const float Ax[4] = { 1.f, -1.f, -1.f,  1.f };
    static const float Bx[4] = { 0.5f, 2.f, -0.5f, -2.f };
    static const float Cx[4] = { 1.f,  1.f, -0.5f, 0.f };
    static const float Ay[4] = { 1.f, -1.f, -1.f,  1.f };
    static const float By[4] = { 2.f,  0.5f, -2.f, -0.5f };
    static const float Cy[4] = { 2.f,  0.f, -1.f, 0.5f };
    const float delta = 1.0f / (float)N;
    const float deltaL[4] = { 0.5f / N, 1.0f / N, 0.5f / N, 1.0f / N };
    const float deltaU[4] = { 1.0f / N, 0.5f / N, 1.0f / N, 0.5f / N };
    for (int sy = 0; sy < N; sy++) {
        for (int sx = 0; sx < N; sx++) {
            /* Sub-pixel centre, exactly what fract(uv * size) gives the shader. */
            const float fpx = ((float)sx + 0.5f) / (float)N;
            const float fpy = ((float)sy + 0.5f) / (float)N;
            XbrSub* t = &tab[sy * N + sx];
            for (int k = 0; k < 4; k++) {
                const float fx      = Ao[k] * fpy + Bo[k] * fpx;
                const float fx_left = Ax[k] * fpy + Bx[k] * fpx;
                const float fx_up   = Ay[k] * fpy + By[k] * fpx;
                t->f45i[k] = snap_eighth(clamp01((fx + delta - Co[k] - 0.25f) / (2.0f * delta)));
                t->f45[k]  = snap_eighth(clamp01((fx + delta - Co[k]) / (2.0f * delta)));
                t->f30[k]  = snap_eighth(clamp01((fx_left + deltaL[k] - Cx[k]) / (2.0f * deltaL[k])));
                t->f60[k]  = snap_eighth(clamp01((fx_up + deltaU[k] - Cy[k]) / (2.0f * deltaU[k])));
            }
        }
    }
}

static void xbr_lv2(const uint32_t* src, int sp, int w, int h, uint32_t* dst, int dp, int N) {
    XbrSub tab[16];   /* N <= 4 */
    xbr_sub_tables(N, tab);

    /* Luma plane once (every neighbourhood read is a luma read; the RGB of
     * only five pixels — E and its four edge-neighbours — is needed, and only
     * where a rule actually fires). */
    float* Y = (float*)malloc((size_t)w * (size_t)h * sizeof(float));
    if (!Y) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            Y[(size_t)y * w + x] = luma_px(src[(size_t)y * sp + x]);

#define YROW(yy) (Y + (size_t)((yy) < 0 ? 0 : ((yy) >= h ? h - 1 : (yy))) * (size_t)w)
#define CX(xx)   ((xx) < 0 ? 0 : ((xx) >= w ? w - 1 : (xx)))

    for (int y = 0; y < h; y++) {
        const float* r_2 = YROW(y - 2);
        const float* r_1 = YROW(y - 1);
        const float* r0  = YROW(y);
        const float* r1  = YROW(y + 1);
        const float* r2  = YROW(y + 2);
        for (int x = 0; x < w; x++) {
            const int xm2 = CX(x - 2), xm1 = CX(x - 1), xp1 = CX(x + 1), xp2 = CX(x + 2);
            /*      A1 B1 C1
             *   A0 A  B  C  C4
             *   D0 D  E  F  F4
             *   G0 G  H  I  I4
             *      G5 H5 I5              */
            const float yA = r_1[xm1], yB = r_1[x], yC = r_1[xp1];
            const float yD = r0[xm1],  e  = r0[x],  yF = r0[xp1];
            const float yG = r1[xm1],  yH = r1[x],  yI = r1[xp1];
            const float yA1 = r_2[xm1], yB1 = r_2[x], yC1 = r_2[xp1];
            const float yA0 = r_1[xm2], yD0 = r0[xm2], yG0 = r1[xm2];
            const float yC4 = r_1[xp2], yF4 = r0[xp2], yI4 = r1[xp2];
            const float yG5 = r2[xm1],  yH5 = r2[x],  yI5 = r2[xp1];

            /* Rotation vectors (component k = corner k: BR, TR, TL, BL). */
            const float b[4]  = { yB, yD, yH, yF };
            const float c[4]  = { yC, yA, yG, yI };
            const float d[4]  = { b[1], b[2], b[3], b[0] };
            const float f[4]  = { b[3], b[0], b[1], b[2] };
            const float g[4]  = { c[2], c[3], c[0], c[1] };
            const float hh[4] = { b[2], b[3], b[0], b[1] };
            const float i[4]  = { c[3], c[0], c[1], c[2] };
            const float i4[4] = { yI4, yC1, yA0, yG5 };
            const float i5[4] = { yI5, yC4, yA1, yG0 };
            const float h5[4] = { yH5, yF4, yB1, yD0 };
            const float f4[4] = { h5[1], h5[2], h5[3], h5[0] };

            float edr[4], edr_l[4], edr_u[4], edri[4], px[4];
            int any = 0;
            for (int k = 0; k < 4; k++) {
                const int lv0 = (e != f[k]) && (e != hh[k]);
                if (!lv0) { edr[k] = edr_l[k] = edr_u[k] = edri[k] = 0.f; px[k] = 0.f; continue; }
                /* Corner rule "C": keep more 90-degree corners intact. */
                const int lv1 =
                    ((!eqf(f[k], b[k]) && !eqf(f[k], c[k])) ||
                     (!eqf(hh[k], d[k]) && !eqf(hh[k], g[k])) ||
                     (eqf(e, i[k]) && ((!eqf(f[k], f4[k]) && !eqf(f[k], i4[k])) ||
                                       (!eqf(hh[k], h5[k]) && !eqf(hh[k], i5[k])))) ||
                     eqf(e, g[k]) || eqf(e, c[k]));
                const int lv2_left = (e != g[k]) && (d[k] != g[k]);
                const int lv2_up   = (e != c[k]) && (b[k] != c[k]);

                const float wd1 = wd(e, c[k], g[k], i[k], h5[k], f4[k], hh[k], f[k]);
                const float wd2 = wd(hh[k], d[k], i5[k], f[k], i4[k], b[k], e, i[k]);

                edri[k]  = (wd2 >= wd1) ? 1.f : 0.f;
                edr[k]   = (wd2 >= wd1 + VF_XBR_EPS) && lv1 ? 1.f : 0.f;
                edr_l[k] = (df(hh[k], c[k]) >= VF_XBR_LV2_COEFF * df(f[k], g[k])) && lv2_left ? edr[k] : 0.f;
                edr_u[k] = (df(f[k], g[k]) >= VF_XBR_LV2_COEFF * df(hh[k], c[k])) && lv2_up ? edr[k] : 0.f;
                px[k]    = (df(e, hh[k]) >= df(e, f[k])) ? 1.f : 0.f;
                if (edri[k] != 0.f || edr[k] != 0.f) any = 1;
            }

            const uint32_t Ergb = src[(size_t)y * sp + x] & RGB_MASK;
            if (!any) {
                /* No rule fires anywhere around this pixel: plain block fill. */
                for (int sy = 0; sy < N; sy++)
                    for (int sx = 0; sx < N; sx++)
                        put(dst, dp, N * x + sx, N * y + sy, Ergb);
                continue;
            }

            const Rgb E = to_rgb(Ergb);
            const Rgb B = to_rgb(fetch(src, sp, w, h, x, y - 1));
            const Rgb D = to_rgb(fetch(src, sp, w, h, x - 1, y));
            const Rgb F = to_rgb(fetch(src, sp, w, h, x + 1, y));
            const Rgb H = to_rgb(fetch(src, sp, w, h, x, y + 1));
            /* Blend partners per rotation: mix(Hk, Fk, px_k). */
            const Rgb Hc[4] = { H, F, B, D };
            const Rgb Fc[4] = { F, B, D, H };
            Rgb P[4];
            for (int k = 0; k < 4; k++) P[k] = mixc(Hc[k], Fc[k], px[k]);

            for (int sy = 0; sy < N; sy++) {
                for (int sx = 0; sx < N; sx++) {
                    const XbrSub* t = &tab[sy * N + sx];
                    float m[4];
                    for (int k = 0; k < 4; k++) {
                        const float a45  = edr[k] * t->f45[k];
                        const float a30  = edr_l[k] * t->f30[k];
                        const float a60  = edr_u[k] * t->f60[k];
                        const float a45i = edri[k] * t->f45i[k];
                        float mm = a30 > a60 ? a30 : a60;
                        const float m2 = a45 > a45i ? a45 : a45i;
                        if (m2 > mm) mm = m2;
                        m[k] = mm;
                    }
                    Rgb res1 = E, res2 = E;
                    res1 = mixc(res1, P[0], m[0]);
                    res1 = mixc(res1, P[2], m[2]);
                    res2 = mixc(res2, P[1], m[1]);
                    res2 = mixc(res2, P[3], m[3]);
                    const Rgb res = (c_df(E, res2) >= c_df(E, res1)) ? res2 : res1;
                    put(dst, dp, N * x + sx, N * y + sy, from_rgb(res));
                }
            }
        }
    }
#undef YROW
#undef CX
    free(Y);
}

/* ---- entry point ----------------------------------------------------------- */

int video_filter_apply_cpu(int kind, const uint32_t* src, int src_pitch,
                           int w, int h, uint32_t* dst, int dst_pitch) {
    if (!vf_valid(kind) || kind == VF_NONE || !src || !dst || w <= 0 || h <= 0)
        return 0;
    const int N = video_filter_cpu_scale(kind);
    if (src_pitch < w || dst_pitch < w * N) return 0;
    switch (kind) {
    case VF_SHARP:       prescale_rows(src, src_pitch, w, h, dst, dst_pitch, s_row_w_sharp); break;
    case VF_SCANLINES: {
        uint32_t rw[VF_SW_PRESCALE];
        scanline_row_weights(rw);
        prescale_rows(src, src_pitch, w, h, dst, dst_pitch, rw);
        break;
    }
    case VF_CRT:         prescale_rows(src, src_pitch, w, h, dst, dst_pitch, s_row_w_crt); break;
    case VF_SCALE2X:     scale2x(src, src_pitch, w, h, dst, dst_pitch); break;
    case VF_SCALE3X:     scale3x(src, src_pitch, w, h, dst, dst_pitch); break;
    case VF_SAI2X:       sai2x(src, src_pitch, w, h, dst, dst_pitch); break;
    case VF_SUPER_SAI2X: super_sai2x(src, src_pitch, w, h, dst, dst_pitch); break;
    case VF_SUPER_EAGLE: super_eagle(src, src_pitch, w, h, dst, dst_pitch); break;
    case VF_XBR2X:       xbr_lv2(src, src_pitch, w, h, dst, dst_pitch, 2); break;
    case VF_XBR3X:       xbr_lv2(src, src_pitch, w, h, dst, dst_pitch, 3); break;
    case VF_XBR4X:       xbr_lv2(src, src_pitch, w, h, dst, dst_pitch, 4); break;
    default: return 0;
    }
    return N;
}
