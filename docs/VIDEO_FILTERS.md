# Video filters (present-time upscalers and display looks)

Opt-in presentation filters applied between the finished PSX frame and the
window. **Presentation only**: they never touch VRAM, the GPU command stream,
savestates, netplay digests or oracle/diff frames. `none` (default) is the
historical present path, byte-identical to a build without the feature.

| token | label | family | GL | software present |
|---|---|---|---|---|
| `none` | None | — | plain present | plain present |
| `sharp` | Sharp | final pass | sharp-bilinear shader | 3x nearest prescale + linear |
| `scale2x`, `scale3x` | Scale2x / Scale3x | upscaler (EPX) | shader, 2x/3x FBO | CPU |
| `2xsai`, `super2xsai`, `supereagle` | 2xSaI / Super 2xSaI / Super Eagle | upscaler | shader, 2x FBO | CPU |
| `xbr2x`, `xbr3x`, `xbr4x` | xBR 2x/3x/4x | upscaler (xBR lv2, corner rule C) | shader, N x FBO | CPU |
| `scanlines` | Scanlines | final pass | sharp + parametric scanlines (opacity / size / glow, gamma-correct bloom) | prescale + darkened rows |
| `crt` | CRT | final pass | Lottes-style beam/mask/gamma | prescale + darkened rows |

Upscalers render an integer-factor image (pass A) that is then fitted to the
window with sharp-bilinear (pass B, GL) or SDL's scale mode (software).
Vulkan does not filter yet (its present is a `vkCmdBlitImage`; the backend is
hidden/experimental).

## Selecting

* Launcher: Settings → Display → **Video filter** (cycles the runtime's
  vocabulary; persisted to `settings.toml` as `[video] filter = "<token>"`).
* In game: **ESC menu → VIDEO FILTER** (Left/Right/Enter cycle; applied live
  and persisted to `settings.toml`).
* `game.toml`: `[video] filter = "<token>"` (default for the title).
* Env override (debug): `PSX_VIDEO_FILTER=<token>`.
* Mods: `psx_mod_set_video_filter("<token>")` (`mod_plugins.h`).
* Live: debug server `{"cmd":"video_filter","name":"xbr2x"}`.

Precedence: game.toml → settings.toml → env → launcher → mod/debug.

### Scanline parameters

`scanlines` has three live parameters (`VideoScanlineParams`, video_filter.h):

| | range | default | meaning |
|---|---|---|---|
| opacity | 0..1 | 0.60 | darkness of the gap between lines (1 = black) |
| size | 0.1..0.8 | 0.35 | gap thickness as a fraction of one native line |
| glow | 0..1 | 0.50 | brightens the line core and bleeds the line + neighbours into the gap (bloom, in linear light) |

Set them in the **ESC menu**: while the scanlines filter is active a single row
`[DARK 60%] SIZE 35% GLOW 50%` appears under VIDEO FILTER — LEFT/RIGHT step the
bracketed value by 5 %, ENTER moves the bracket to the next parameter
(persisted). Or in `settings.toml` / `game.toml` (`[video]
scanline_opacity`, `scanline_size`, `scanline_glow`), or live over the debug
server (`{"cmd":"video_filter","scan_opacity":"0.9","scan_size":"0.5","scan_glow":"0"}`).
Lost energy is partly compensated, so the default look keeps the source's
average brightness and glow makes it brighter, not darker. The software-present
approximation maps them onto its 3-row prescale weights. Output scale matters:
at 2x a 35 % gap is one row; 3x–4x+ windows show the profile properly.

### Fixed: CRT one-line flicker

The CRT pass computed the source line as `floor(pn) * (rect.h / native_h)`;
GLSL compilers lower that division to a reciprocal multiply (0.99999994), so on
the display buffer at VRAM y=0 the line index floored one too low while at
y=240 the float add rounded back — a one-line vertical jump every frame under a
fixed scanline grid. `fetch_px` now uses the exact integer texels-per-line and
`floor(x + 0.5)`; verified with consecutive `present_capture`s (shift 0).

## Where it lives

* `runtime/include/video_filter.h`, `runtime/src/video_filter.c` — kinds,
  names, the **CPU reference** implementation (also the software present).
* `runtime/src/gpu_gl_filter_shaders.h` — GLSL twins of the CPU reference;
  `gpu_gl_renderer.c` `vf_present()` runs them from `present_target_quad`
  (VRAM / wide / hold-native), the CPU-readout present (FMV) and the
  interpolation thread's blend.
* `runtime/tests/test_video_filter.c` — contract tests for the CPU side
  (`video_filter_test`).

## Parity contract and how to check it

The GL shaders must reproduce the CPU reference to within 1 LSB when the
window is an exact integer multiple of the source (pass B is then an identity).
xBR is written so that all edge decisions are exact integer comparisons on both
sides (integer luma 299R+587G+114B, dyadic blend weights snapped to 1/8) —
tie-breaking on pixel art would otherwise flip between CPU and GPU.

Debug-server support:

* `present_capture {"path":P}` — next presented drawable → `P` (GL: post-filter,
  pre-OSD) plus `P.src.png` (source rect the filter consumed), `P.up.png` (raw
  pass-A output) and `P.ref.png` (CPU reference of the same input) for the
  upscalers; software present: the exact post-filter buffer.
* `window_size {"w":..,"h":..}` — pin an integer scale.
* A game repo tool such as MegaMan8Recomp `tools/video_filter_check.py` drives
  those three and diffs drawable vs reference for every upscaler.

## Provenance

Scale2x/EPX (E. Johnston / A. Mazzoleni), 2xSaI family (D. Liauw Kie Fa) are
independent implementations from the published algorithms (the original code
of both is GPL and was not used). xBR level 2 follows Hyllian's MIT-licensed
xbr-lv2 shader; the CRT look is modelled on Timothy Lottes' public-domain CRT
shader; sharp-bilinear is the public-domain prescale+linear formula.
hqx (LGPL) and xBRZ (GPL) are deliberately not included.
