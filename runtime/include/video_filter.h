/* video_filter.h — present-time pixel-art upscalers and display filters.
 *
 * PRESENT-TIME ONLY. A video filter sits between the finished PSX frame (the
 * scanout region of VRAM, already converted to RGB) and the window. It never
 * touches VRAM, the GPU command stream, the CPU-visible mirror, savestates,
 * netplay digests or the differential-verify path: every hashed / oracle-
 * compared frame is defined on the raw scanout and stays byte-identical no
 * matter which filter the player has selected. VF_NONE (the default) is a
 * true passthrough — with the feature off, the present is exactly what it was
 * before this module existed.
 *
 * Two families of filter exist:
 *
 *   UPSCALERS  — integer-factor "edge logic" scalers designed for hand-drawn
 *                tile/sprite art: Scale2x/3x (EPX), the 2xSaI family (2xSaI,
 *                Super 2xSaI, Super Eagle) and xBR level 2 at 2x/3x/4x. Each
 *                produces an (N*w) x (N*h) image which is then fitted to the
 *                window (sharp-bilinear on GL, the SDL scale mode on the
 *                software present).
 *   FINAL PASS — filters that render straight at window resolution: sharp
 *                bilinear (pixel-perfect scaling at non-integer factors),
 *                scanlines, and a CRT look (scanline beam + shadow mask +
 *                gamma). On the software (SDL_Renderer) present these are
 *                approximated on the CPU by an integer nearest prescale (plus
 *                the scanline mask) followed by SDL's linear scaling.
 *
 * The CPU implementations in video_filter.c are the REFERENCE: the OpenGL
 * fragment shaders in gpu_gl_renderer.c implement the same arithmetic per
 * output pixel, so a window sized to an exact integer multiple of the source
 * must reproduce the CPU result to within unorm rounding (see
 * runtime/tests/test_video_filter.c and docs/VIDEO_FILTERS.md).
 *
 * ── Attribution ───────────────────────────────────────────────────────────
 * The algorithms are published designs; every implementation here is written
 * from the algorithm descriptions, not copied from a GPL source:
 *   - Scale2x / Scale3x (EPX): Eric Johnston (LucasArts, 1992), popularised
 *     by Andrea Mazzoleni's AdvanceMAME Scale2x.
 *   - 2xSaI / Super 2xSaI / Super Eagle: Derek Liauw Kie Fa ("Kreed"), 1999.
 *   - xBR level 2: Hyllian (2011). The per-output-pixel formulation follows
 *     Hyllian's MIT-licensed xbr-lv2 fragment shader (libretro glsl-shaders).
 *   - Sharp bilinear: the widely used "prescale + linear" pixel-art formula
 *     (as in libretro's sharp-bilinear-simple, public domain).
 *   - CRT look: modelled on Timothy Lottes' public-domain CRT shader (scanline
 *     gaussian, horizontal beam blur, shadow mask, gamma-correct blend).
 */

#ifndef PSXRECOMP_VIDEO_FILTER_H
#define PSXRECOMP_VIDEO_FILTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VF_NONE = 0,       /* raw present (nearest / linear per antialiasing)   */
    VF_SHARP,          /* sharp bilinear (pixel-perfect non-integer fit)   */
    VF_SCALE2X,        /* EPX / AdvMAME2x                                   */
    VF_SCALE3X,        /* AdvMAME3x                                         */
    VF_SAI2X,          /* 2xSaI                                             */
    VF_SUPER_SAI2X,    /* Super 2xSaI                                       */
    VF_SUPER_EAGLE,    /* Super Eagle                                       */
    VF_XBR2X,          /* xBR level 2, 2x                                   */
    VF_XBR3X,          /* xBR level 2, 3x                                   */
    VF_XBR4X,          /* xBR level 2, 4x                                   */
    VF_SCANLINES,      /* sharp + scanline mask                             */
    VF_CRT,            /* CRT look: scanline beam, shadow mask, gamma       */
    VF_COUNT
} VideoFilterKind;

/* Config token ("none", "sharp", "scale2x", ..., "crt") and a human label. */
const char* video_filter_name(int kind);
const char* video_filter_label(int kind);
/* Parse a config/env token (case-insensitive; accepts a few aliases such as
 * "2xsai"/"sai", "super2xsai", "supereagle", "xbr"=xbr2x, "off"/"raw"=none).
 * Returns 1 and writes *out on success, 0 if unrecognised. */
int  video_filter_from_name(const char* name, int* out);

/* Integer factor of the upscaler pass: 2/3/4 for the upscalers, 1 for VF_NONE
 * and the final-pass filters (sharp/scanlines/crt). */
int  video_filter_scale(int kind);
int  video_filter_is_upscaler(int kind);

/* Runtime-wide selection (present-time only). Default VF_NONE. Values outside
 * [0, VF_COUNT) clamp to VF_NONE. Safe to call from the main thread at any
 * time; the presenters pick the new value up on their next frame. */
void video_filter_set(int kind);
int  video_filter_get(void);

/* Scanline look parameters (VF_SCANLINES; the CRT look ignores them):
 *   opacity : darkness of the gap between lines, 0 = invisible, 1 = black
 *   size    : gap thickness as a fraction of one native line (0.1 .. 0.8)
 *   glow    : brightens the line core and bleeds neighbouring lines into the
 *             gap (bloom), 0 = flat scanlines, 1 = strong glow
 * Defaults are a medium, slightly glowing look. Values are clamped. */
typedef struct {
    float opacity;   /* default 0.60 */
    float size;      /* default 0.35 */
    float glow;      /* default 0.50 */
} VideoScanlineParams;
void video_filter_scanline_set(const VideoScanlineParams* p);
void video_filter_scanline_get(VideoScanlineParams* out);
#define VF_SCAN_OPACITY_DEFAULT 0.60f
#define VF_SCAN_SIZE_DEFAULT    0.35f
#define VF_SCAN_GLOW_DEFAULT    0.50f

/* Factor the CPU path produces for `kind`: video_filter_scale() for the
 * upscalers, VF_SW_PRESCALE (nearest prescale + optional scanline mask) for the
 * final-pass filters, 1 for VF_NONE. */
#define VF_SW_PRESCALE 3
int  video_filter_cpu_scale(int kind);

/* CPU reference / software-present implementation.
 *   src : ARGB8888 (0xAARRGGBB) source, `w` x `h`, `src_pitch` in PIXELS.
 *   dst : receives (w*N) x (h*N), N = video_filter_cpu_scale(kind),
 *         `dst_pitch` in pixels. Alpha is forced to 0xFF.
 * Border pixels are clamped (the neighbourhood never reads outside the
 * source). Returns N, or 0 if kind is VF_NONE / arguments invalid (dst
 * untouched). */
int  video_filter_apply_cpu(int kind, const uint32_t* src, int src_pitch,
                            int w, int h, uint32_t* dst, int dst_pitch);

/* xBR tuning shared by the CPU reference and the GL shaders. Lumas are the
 * INTEGER 299*R + 587*G + 114*B of 8-bit channels (0..255000): every luma,
 * difference and weighted sum is then an exact small integer in float on both
 * the CPU and the GPU, so the edge decisions (which are comparisons and are
 * full of exact ties on pixel art) cannot flip between the two through
 * rounding-order differences. Hyllian's canonical thresholds are on the
 * 48-weight/0..1 scale — EQ 15 out of 48 — which maps to 15/48*255*1000. */
#define VF_XBR_EQ_THRESHOLD   79687.5f     /* 15/48 * 255000 */
#define VF_XBR_LV2_COEFF       2.0f
#define VF_XBR_EPS             0.5f        /* strict-< guard on integer sums */

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_VIDEO_FILTER_H */
