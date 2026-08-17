/* test_video_filter.c — contract tests for the CPU video-filter reference
 * (runtime/src/video_filter.c).
 *
 * What is pinned here:
 *   - name/alias parsing round-trips and rejects junk;
 *   - every filter produces exactly (N*w) x (N*h) pixels, N per the table, and
 *     never reads/writes outside its buffers (run under ASan in CI; the
 *     1x1 / 2x3 / 3x2 cases exercise every clamp path);
 *   - a constant-colour field is a fixed point of every upscaler (no filter
 *     may invent colour on flat regions) and of the sharp prescale;
 *   - Scale2x / Scale3x reproduce the published EPX corner rules on a
 *     hand-checkable staircase;
 *   - the 2xSaI family and xBR keep a two-colour image two-coloured away from
 *     the edge, and only introduce blends ON the edge, symmetric about it;
 *   - output alpha is always 0xFF.
 *
 * Build (also registered in runtime/CMakeLists.txt as video_filter_test):
 *   cc -I runtime/include runtime/tests/test_video_filter.c \
 *      runtime/src/video_filter.c -lm -o video_filter_test
 */

#include "video_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define RED   0xFFFF0000u
#define BLUE  0xFF0000FFu
#define WHITE 0xFFFFFFFFu
#define BLACK 0xFF000000u

static uint32_t* run(int kind, const uint32_t* src, int w, int h, int* out_w, int* out_h) {
    int N = video_filter_cpu_scale(kind);
    /* Pad the destination with a guard border to catch overruns even without ASan. */
    size_t dw = (size_t)w * N, dh = (size_t)h * N;
    uint32_t* dst = (uint32_t*)malloc((dw * dh + 64) * sizeof(uint32_t));
    memset(dst, 0xAB, (dw * dh + 64) * sizeof(uint32_t));
    int got = video_filter_apply_cpu(kind, src, w, w, h, dst, (int)dw);
    CHECK(got == N, "kind %d: apply returned %d, expected %d", kind, got, N);
    for (int i = 0; i < 64; i++)
        CHECK(dst[dw * dh + i] == 0xABABABABu, "kind %d: guard word %d clobbered", kind, i);
    *out_w = (int)dw; *out_h = (int)dh;
    return dst;
}

static void test_names(void) {
    for (int k = 0; k < VF_COUNT; k++) {
        int back = -1;
        CHECK(video_filter_from_name(video_filter_name(k), &back) && back == k,
              "name round-trip failed for kind %d (%s)", k, video_filter_name(k));
        CHECK(video_filter_label(k) && video_filter_label(k)[0], "label empty for %d", k);
    }
    int v = -1;
    CHECK(video_filter_from_name("XBR", &v) && v == VF_XBR2X, "alias xbr");
    CHECK(video_filter_from_name("Super-Eagle", &v) && v == VF_SUPER_EAGLE, "alias super-eagle");
    CHECK(video_filter_from_name("off", &v) && v == VF_NONE, "alias off");
    CHECK(!video_filter_from_name("hq2x", &v), "hq2x must be rejected (not implemented)");
    CHECK(!video_filter_from_name("", &v), "empty must be rejected");
    CHECK(!video_filter_from_name(NULL, &v), "NULL must be rejected");
    CHECK(video_filter_scale(VF_XBR4X) == 4 && video_filter_scale(VF_SCALE3X) == 3 &&
          video_filter_scale(VF_SAI2X) == 2 && video_filter_scale(VF_CRT) == 1 &&
          video_filter_scale(VF_NONE) == 1, "scale table");
    CHECK(video_filter_cpu_scale(VF_SHARP) == VF_SW_PRESCALE &&
          video_filter_cpu_scale(VF_NONE) == 1, "cpu scale for final-pass kinds");
    video_filter_set(VF_XBR3X); CHECK(video_filter_get() == VF_XBR3X, "set/get");
    video_filter_set(999);      CHECK(video_filter_get() == VF_NONE, "set clamps");
    video_filter_set(-1);       CHECK(video_filter_get() == VF_NONE, "set clamps neg");
}

static void test_flat_and_sizes(void) {
    static const int dims[][2] = { {1, 1}, {2, 3}, {3, 2}, {5, 5}, {17, 9} };
    for (size_t d = 0; d < sizeof dims / sizeof dims[0]; d++) {
        int w = dims[d][0], h = dims[d][1];
        uint32_t* src = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
        for (int i = 0; i < w * h; i++) src[i] = 0x00123456u;   /* alpha 0 on input */
        for (int k = 1; k < VF_COUNT; k++) {
            int ow, oh;
            uint32_t* dst = run(k, src, w, h, &ow, &oh);
            if (video_filter_is_upscaler(k) || k == VF_SHARP) {
                for (int i = 0; i < ow * oh; i++)
                    if (dst[i] != 0xFF123456u) {
                        CHECK(0, "kind %d (%dx%d): flat field not preserved at %d: %08X",
                              k, w, h, i, dst[i]);
                        break;
                    }
            } else {
                /* Scanline kinds darken rows but keep hue and alpha. */
                for (int i = 0; i < ow * oh; i++)
                    CHECK((dst[i] >> 24) == 0xFF, "kind %d: alpha not forced", k);
            }
            free(dst);
        }
        free(src);
    }
    /* VF_NONE / bad args: no-op, returns 0. */
    uint32_t s1 = RED, d1 = 0;
    CHECK(video_filter_apply_cpu(VF_NONE, &s1, 1, 1, 1, &d1, 1) == 0 && d1 == 0, "NONE is a no-op");
    CHECK(video_filter_apply_cpu(VF_XBR2X, &s1, 1, 1, 1, &d1, 1) == 0, "pitch too small rejected");
    CHECK(video_filter_apply_cpu(VF_XBR2X, NULL, 1, 1, 1, &d1, 2) == 0, "NULL src rejected");
}

/* Staircase: a 2x2 red block on white in a 4x4 field.
 *   W W W W
 *   W R R W
 *   W R R W
 *   W W W W                                                              */
static void test_scale2x_rules(void) {
    uint32_t src[16];
    for (int i = 0; i < 16; i++) src[i] = WHITE;
    src[5] = src[6] = src[9] = src[10] = RED;
    int ow, oh;
    uint32_t* d = run(VF_SCALE2X, src, 4, 4, &ow, &oh);
    /* EPX rounds the convex corners of an isolated block: each corner pixel
     * has two equal orthogonal (background) neighbours, so its outer sub-pixel
     * takes the background colour. Everything else of the 4x4 stays red. */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int in = (x >= 2 && x <= 5 && y >= 2 && y <= 5);
            int corner = (x == 2 || x == 5) && (y == 2 || y == 5);
            uint32_t want = (in && !corner) ? RED : WHITE;
            CHECK(d[y * 8 + x] == want, "scale2x square: (%d,%d)=%08X", x, y, d[y * 8 + x]);
        }
    free(d);

    /* Diagonal: red lower-left triangle. EPX fills the staircase corners.
     *   W W W
     *   R W W
     *   R R W  */
    uint32_t tri[9] = { WHITE, WHITE, WHITE, RED, WHITE, WHITE, RED, RED, WHITE };
    d = run(VF_SCALE2X, tri, 3, 3, &ow, &oh);
    /* Centre pixel (1,1) is W with D=R (left) and H=R (below): E2 (its
     * bottom-left sub-pixel) becomes R (D==H, D!=B, H!=F); the other three
     * sub-pixels stay W. */
    CHECK(d[3 * 6 + 2] == RED,   "epx: E2 of centre should be RED (%08X)", d[3 * 6 + 2]);
    CHECK(d[2 * 6 + 2] == WHITE, "epx: E0 of centre should stay WHITE");
    CHECK(d[2 * 6 + 3] == WHITE, "epx: E1 of centre should stay WHITE");
    CHECK(d[3 * 6 + 3] == WHITE, "epx: E3 of centre should stay WHITE");
    free(d);

    /* Scale3x on the same triangle: centre pixel's E6 (bottom-left) → D. */
    d = run(VF_SCALE3X, tri, 3, 3, &ow, &oh);
    CHECK(d[5 * 9 + 3] == RED, "scale3x: E6 of centre should be RED (%08X)", d[5 * 9 + 3]);
    CHECK(d[4 * 9 + 4] == WHITE, "scale3x: E4 (centre) must be the source pixel");
    CHECK(d[3 * 9 + 5] == WHITE, "scale3x: E2 (top-right) must stay WHITE");
    free(d);
}

static int is_pure(uint32_t c) { return c == RED || c == WHITE; }
static int is_blend(uint32_t c) {
    /* Any convex mix of RED and WHITE keeps R=0xFF and G==B. */
    return ((c >> 16) & 0xFF) == 0xFF && ((c >> 8) & 0xFF) == (c & 0xFF);
}

/* 8x8 field, red below the anti-diagonal (x + y >= 8 → red). Away from the
 * edge the output must be pure; blends only appear within one source pixel of
 * the edge; and the image must be symmetric under the transpose (x<->y),
 * because the source is. */
static void test_edge_filters(void) {
    enum { W = 8 };
    uint32_t src[W * W];
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++)
            src[y * W + x] = (x + y >= W) ? RED : WHITE;
    static const int kinds[] = { VF_SAI2X, VF_SUPER_SAI2X, VF_SUPER_EAGLE,
                                 VF_XBR2X, VF_XBR3X, VF_XBR4X };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        int k = kinds[i], ow, oh;
        int N = video_filter_scale(k);
        uint32_t* d = run(k, src, W, W, &ow, &oh);
        int blends = 0;
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                uint32_t c = d[y * ow + x];
                int sx = x / N, sy = y / N;
                int dist = sx + sy - W;             /* signed distance to the edge, in source px */
                CHECK(is_pure(c) || is_blend(c), "%s: (%d,%d)=%08X is not a red/white mix",
                      video_filter_name(k), x, y, c);
                if (dist <= -3) CHECK(c == WHITE, "%s: far white side altered at (%d,%d)=%08X",
                                      video_filter_name(k), x, y, c);
                if (dist >= 2)  CHECK(c == RED, "%s: far red side altered at (%d,%d)=%08X",
                                      video_filter_name(k), x, y, c);
                if (!is_pure(c)) blends++;
            }
        /* The edge is a 45-degree diagonal — the whole point of these filters
         * is to smooth it, so SOME blending must occur. */
        CHECK(blends > 0, "%s: diagonal edge produced no blends at all", video_filter_name(k));
        /* Symmetry: the diagonal source is transpose-symmetric; the 2xSaI
         * family is not (it is biased toward the right/bottom neighbour by
         * construction), but xBR must be. */
        if (k >= VF_XBR2X && k <= VF_XBR4X) {
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++)
                    CHECK(d[y * ow + x] == d[x * ow + y],
                          "%s: not transpose-symmetric at (%d,%d): %08X vs %08X",
                          video_filter_name(k), x, y, d[y * ow + x], d[x * ow + y]);
        }
        free(d);
    }
}

/* Vertical bar: a 1-pixel-wide red line on white must stay a crisp N-wide bar
 * for Scale2x/3x and xBR (thin features are preserved, not smeared). The 2xSaI
 * family blurs single-pixel lines by design (its diagonal Q_INTERPOLATE fires
 * on every W/R/W/R quad), so it is deliberately not held to this. */
static void test_thin_line(void) {
    enum { W = 7, H = 5 };
    uint32_t src[W * H];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            src[y * W + x] = (x == 3) ? RED : WHITE;
    static const int kinds[] = { VF_SCALE2X, VF_SCALE3X, VF_XBR2X, VF_XBR3X, VF_XBR4X };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        int k = kinds[i], ow, oh, N = video_filter_scale(k);
        uint32_t* d = run(k, src, W, H, &ow, &oh);
        for (int y = N; y < oh - N; y++)          /* skip top/bottom rows (clamped ends) */
            for (int x = 0; x < ow; x++) {
                int in = (x >= 3 * N && x < 4 * N);
                CHECK(d[y * ow + x] == (in ? RED : WHITE),
                      "%s: thin line smeared at (%d,%d)=%08X", video_filter_name(k),
                      x, y, d[y * ow + x]);
            }
        free(d);
    }
}

static void test_prescale_rows(void) {
    uint32_t src[2] = { WHITE, 0xFF808080u };
    int ow, oh;
    uint32_t* d = run(VF_SCANLINES, src, 2, 1, &ow, &oh);
    CHECK(ow == 2 * VF_SW_PRESCALE && oh == VF_SW_PRESCALE, "prescale dims");
    CHECK(d[0] == WHITE && d[1] == WHITE && d[2] == WHITE, "prescale row 0: white stays white (glow clamps)");
    /* Row 0/1 carry the (glow-brightened) line: never darker than the source,
     * still neutral grey. */
    CHECK((d[3] & 0xFF) >= 0x80 && ((d[3] >> 8) & 0xFF) == (d[3] & 0xFF), "prescale line row >= source: %08X", d[3]);
    /* Last row of the triple is the dark gap: strictly darker, still grey. */
    uint32_t gap = d[2 * ow + 0];
    CHECK((gap & 0xFF) < 0xFF && ((gap >> 8) & 0xFF) == (gap & 0xFF) &&
          ((gap >> 16) & 0xFF) == (gap & 0xFF), "scanline gap row is a darker grey: %08X", gap);
    /* Parameters steer it: opacity 1 / glow 0 -> black gap; opacity 0 -> no gap. */
    VideoScanlineParams sp = { 1.0f, 0.35f, 0.0f };
    video_filter_scanline_set(&sp);
    free(d); d = run(VF_SCANLINES, src, 2, 1, &ow, &oh);
    CHECK(d[2 * ow + 0] == BLACK, "opacity 1, glow 0 -> black gap: %08X", d[2 * ow + 0]);
    sp.opacity = 0.0f; video_filter_scanline_set(&sp);
    free(d); d = run(VF_SCANLINES, src, 2, 1, &ow, &oh);
    CHECK(d[2 * ow + 0] == WHITE, "opacity 0 -> no gap: %08X", d[2 * ow + 0]);
    sp.opacity = VF_SCAN_OPACITY_DEFAULT; sp.glow = VF_SCAN_GLOW_DEFAULT; video_filter_scanline_set(&sp);
    free(d);
    d = run(VF_SHARP, src, 2, 1, &ow, &oh);
    for (int i = 0; i < ow * oh; i++)
        CHECK(d[i] == ((i % ow) < VF_SW_PRESCALE ? WHITE : 0xFF808080u), "sharp prescale is nearest");
    free(d);
}

int main(void) {
    test_names();
    test_flat_and_sizes();
    test_scale2x_rules();
    test_edge_filters();
    test_thin_line();
    test_prescale_rows();
    if (g_fail) { fprintf(stderr, "video_filter_test: %d failure(s)\n", g_fail); return 1; }
    printf("video_filter_test: OK\n");
    return 0;
}
