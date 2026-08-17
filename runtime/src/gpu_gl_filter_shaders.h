/* gpu_gl_filter_shaders.h — GLSL sources for the present-time video filters
 * (OpenGL backend). Included only by gpu_gl_renderer.c.
 *
 * Contract: every upscaler shader here is the per-output-pixel form of the CPU
 * reference in runtime/src/video_filter.c — same neighbourhood, same rules,
 * same clamped edge handling, same rounding intent — so a window that is an
 * exact integer multiple of the source reproduces the CPU output to within
 * unorm rounding. Keep the two in lock-step; test_video_filter.c pins the CPU
 * side and docs/VIDEO_FILTERS.md describes the in-game parity capture.
 *
 * Coordinate conventions:
 *   - Upscaler passes render a full-viewport triangle into an FBO of exactly
 *     (w*N) x (h*N) texels; gl_FragCoord identifies the output pixel, so no
 *     UVs are needed. Source texel = u_rect.xy + out / N, sub = out mod N.
 *     Texel row 0 is the TOP of the picture for every source we filter (VRAM
 *     texture, wide surface, CPU upload, interp history), so "up" is y-1
 *     exactly as in the CPU reference.
 *   - All neighbourhood fetches clamp to the source rectangle u_rect: nothing
 *     outside the displayed region ever leaks in (matches CPU clamping).
 *   - The final pass draws with PRESENT_VS (v_uv over the letterbox quad,
 *     already flipped by the caller's u_uv_rect) and samples the bound
 *     texture inside u_rect only.
 *
 * Provenance: see video_filter.h. The xBR pass follows Hyllian's MIT-licensed
 * xbr-lv2 shader structure; the CRT pass is modelled on Timothy Lottes'
 * public-domain CRT shader (scanline gaussian, beam blur, aperture mask,
 * gamma-correct blend); the sharp-bilinear formula is the public-domain
 * "prescale + linear" pixel-art scaler. */

#ifndef PSXRECOMP_GPU_GL_FILTER_SHADERS_H
#define PSXRECOMP_GPU_GL_FILTER_SHADERS_H

/* Vertex shader for the upscaler passes: full-viewport triangle, no varyings. */
static const char *VF_UP_VS =
    "#version 330\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  gl_Position = vec4(p*2.0-1.0,0.0,1.0); }\n";

/* Shared prologue of every upscaler fragment shader. */
#define VF_UP_PROLOGUE \
    "#version 330\n" \
    "uniform sampler2D u_tex;\n" \
    "uniform ivec4 u_rect;   /* x0,y0,w,h in texels */\n" \
    "uniform int   u_scale;  /* N */\n" \
    "out vec4 frag;\n" \
    "ivec2 g_src;\n" \
    "vec3 P(int dx, int dy){\n" \
    "  ivec2 p = clamp(g_src + ivec2(dx,dy), u_rect.xy, u_rect.xy + u_rect.zw - 1);\n" \
    "  return texelFetch(u_tex, p, 0).rgb; }\n" \
    "bool eq(vec3 a, vec3 b){ return all(equal(a,b)); }\n" \
    "vec3 avg2(vec3 a, vec3 b){ return (a+b)*0.5; }\n" \
    "vec3 avg4(vec3 a, vec3 b, vec3 c, vec3 d){ return (a+b+c+d)*0.25; }\n"

/* Opens main(): output pixel -> source cell + sub-position. */
#define VF_UP_MAIN \
    "void main(){\n" \
    "  ivec2 o = ivec2(gl_FragCoord.xy);\n" \
    "  ivec2 cell = o / u_scale;\n" \
    "  ivec2 sub = o - cell * u_scale;\n" \
    "  g_src = u_rect.xy + cell;\n"

/* ---- Scale2x (EPX) ------------------------------------------------------- */
static const char *VF_SCALE2X_FS =
    VF_UP_PROLOGUE
    VF_UP_MAIN
    "  vec3 B=P(0,-1), D=P(-1,0), E=P(0,0), F=P(1,0), H=P(0,1);\n"
    "  vec3 r = E;\n"
    "  if (!eq(B,H) && !eq(D,F)) {\n"
    "    if (sub == ivec2(0,0) && eq(D,B)) r = D;\n"
    "    if (sub == ivec2(1,0) && eq(B,F)) r = F;\n"
    "    if (sub == ivec2(0,1) && eq(D,H)) r = D;\n"
    "    if (sub == ivec2(1,1) && eq(H,F)) r = F;\n"
    "  }\n"
    "  frag = vec4(r, 1.0);\n"
    "}\n";

/* ---- Scale3x --------------------------------------------------------------- */
static const char *VF_SCALE3X_FS =
    VF_UP_PROLOGUE
    VF_UP_MAIN
    "  vec3 A=P(-1,-1), B=P(0,-1), C=P(1,-1), D=P(-1,0), E=P(0,0), F=P(1,0),\n"
    "       G=P(-1,1), H=P(0,1), I=P(1,1);\n"
    "  vec3 r = E;\n"
    "  if (!eq(B,H) && !eq(D,F)) {\n"
    "    int k = sub.y*3 + sub.x;\n"
    "    if (k==0 && eq(D,B)) r = D;\n"
    "    if (k==1 && ((eq(D,B) && !eq(E,C)) || (eq(B,F) && !eq(E,A)))) r = B;\n"
    "    if (k==2 && eq(B,F)) r = F;\n"
    "    if (k==3 && ((eq(D,B) && !eq(E,G)) || (eq(D,H) && !eq(E,A)))) r = D;\n"
    "    if (k==5 && ((eq(B,F) && !eq(E,I)) || (eq(H,F) && !eq(E,C)))) r = F;\n"
    "    if (k==6 && eq(D,H)) r = D;\n"
    "    if (k==7 && ((eq(D,H) && !eq(E,I)) || (eq(H,F) && !eq(E,G)))) r = H;\n"
    "    if (k==8 && eq(H,F)) r = F;\n"
    "  }\n"
    "  frag = vec4(r, 1.0);\n"
    "}\n";

/* Kreed's vote helper (GetResult); GetResult2 is its negation. */
#define VF_GET_RESULT \
    "int getr(vec3 A, vec3 B, vec3 C, vec3 D){ int x=0,y=0,r=0;\n" \
    "  if (eq(A,C)) x++; else if (eq(B,C)) y++;\n" \
    "  if (eq(A,D)) x++; else if (eq(B,D)) y++;\n" \
    "  if (x<=1) r++; if (y<=1) r--; return r; }\n"

/* ---- 2xSaI ------------------------------------------------------------------ */
static const char *VF_SAI2X_FS =
    VF_UP_PROLOGUE
    VF_GET_RESULT
    VF_UP_MAIN
    "  /*  I E F J / G A B K / H C D L / M N O .  — A = current */\n"
    "  vec3 I=P(-1,-1), E=P(0,-1), F=P(1,-1), J=P(2,-1);\n"
    "  vec3 G=P(-1,0),  A=P(0,0),  B=P(1,0),  K=P(2,0);\n"
    "  vec3 H=P(-1,1),  C=P(0,1),  D=P(1,1),  L=P(2,1);\n"
    "  vec3 M=P(-1,2),  N=P(0,2),  O=P(1,2);\n"
    "  vec3 p0, p1, p2;\n"
    "  if (eq(A,D) && !eq(B,C)) {\n"
    "    p0 = ((eq(A,E) && eq(B,L)) || (eq(A,C) && eq(A,F) && !eq(B,E) && eq(B,J))) ? A : avg2(A,B);\n"
    "    p1 = ((eq(A,G) && eq(C,O)) || (eq(A,B) && eq(A,H) && !eq(G,C) && eq(C,M))) ? A : avg2(A,C);\n"
    "    p2 = A;\n"
    "  } else if (eq(B,C) && !eq(A,D)) {\n"
    "    p0 = ((eq(B,F) && eq(A,H)) || (eq(B,E) && eq(B,D) && !eq(A,F) && eq(A,I))) ? B : avg2(A,B);\n"
    "    p1 = ((eq(C,H) && eq(A,F)) || (eq(C,G) && eq(C,D) && !eq(A,H) && eq(A,I))) ? C : avg2(A,C);\n"
    "    p2 = B;\n"
    "  } else if (eq(A,D) && eq(B,C)) {\n"
    "    if (eq(A,B)) { p0 = A; p1 = A; p2 = A; }\n"
    "    else {\n"
    "      int r = 0;\n"
    "      p1 = avg2(A,C); p0 = avg2(A,B);\n"
    "      r += getr(A,B,G,E); r -= getr(B,A,K,F); r -= getr(B,A,H,N); r += getr(A,B,L,O);\n"
    "      p2 = (r > 0) ? A : ((r < 0) ? B : avg4(A,B,C,D));\n"
    "    }\n"
    "  } else {\n"
    "    p2 = avg4(A,B,C,D);\n"
    "    if (eq(A,C) && eq(A,F) && !eq(B,E) && eq(B,J)) p0 = A;\n"
    "    else if (eq(B,E) && eq(B,D) && !eq(A,F) && eq(A,I)) p0 = B;\n"
    "    else p0 = avg2(A,B);\n"
    "    if (eq(A,B) && eq(A,H) && !eq(G,C) && eq(C,M)) p1 = A;\n"
    "    else if (eq(C,G) && eq(C,D) && !eq(A,H) && eq(A,I)) p1 = C;\n"
    "    else p1 = avg2(A,C);\n"
    "  }\n"
    "  vec3 r = (sub.y == 0) ? ((sub.x == 0) ? A : p0) : ((sub.x == 0) ? p1 : p2);\n"
    "  frag = vec4(r, 1.0);\n"
    "}\n";

/* Shared 4x4 neighbourhood of Super 2xSaI / Super Eagle:
 *   B0 B1 B2 B3 / c4 c5 c6 S2 / c1 c2 c3 S1 / A0 A1 A2 A3   (c5 = current) */
#define VF_SAI_NBR \
    "  vec3 B0=P(-1,-1), B1=P(0,-1), B2=P(1,-1), B3=P(2,-1);\n" \
    "  vec3 c4=P(-1,0),  c5=P(0,0),  c6=P(1,0),  S2=P(2,0);\n" \
    "  vec3 c1=P(-1,1),  c2=P(0,1),  c3=P(1,1),  S1=P(2,1);\n" \
    "  vec3 A0=P(-1,2),  A1=P(0,2),  A2=P(1,2),  A3=P(2,2);\n" \
    "  vec3 p1a, p1b, p2a, p2b;\n"

/* ---- Super 2xSaI ------------------------------------------------------------ */
static const char *VF_SUPER_SAI2X_FS =
    VF_UP_PROLOGUE
    VF_GET_RESULT
    VF_UP_MAIN
    VF_SAI_NBR
    "  if (eq(c2,c6) && !eq(c5,c3)) { p2b = c2; p1b = c2; }\n"
    "  else if (eq(c5,c3) && !eq(c2,c6)) { p2b = c5; p1b = c5; }\n"
    "  else if (eq(c5,c3) && eq(c2,c6)) {\n"
    "    int r = 0;\n"
    "    r += getr(c6,c5,c1,A1); r += getr(c6,c5,c4,B1); r += getr(c6,c5,A2,S1); r += getr(c6,c5,B2,S2);\n"
    "    vec3 v = (r > 0) ? c6 : ((r < 0) ? c5 : avg2(c5,c6));\n"
    "    p2b = v; p1b = v;\n"
    "  } else {\n"
    "    if (eq(c6,c3) && eq(c3,A1) && !eq(c2,A2) && !eq(c3,A0)) p2b = avg4(c3,c3,c3,c2);\n"
    "    else if (eq(c5,c2) && eq(c2,A2) && !eq(A1,c3) && !eq(c2,A3)) p2b = avg4(c2,c2,c2,c3);\n"
    "    else p2b = avg2(c2,c3);\n"
    "    if (eq(c6,c3) && eq(c6,B1) && !eq(c5,B2) && !eq(c6,B0)) p1b = avg4(c6,c6,c6,c5);\n"
    "    else if (eq(c5,c2) && eq(c5,B2) && !eq(B1,c6) && !eq(c5,B3)) p1b = avg4(c6,c5,c5,c5);\n"
    "    else p1b = avg2(c5,c6);\n"
    "  }\n"
    "  if (eq(c5,c3) && !eq(c2,c6) && eq(c4,c5) && !eq(c5,A2)) p2a = avg2(c2,c5);\n"
    "  else if (eq(c5,c1) && eq(c6,c5) && !eq(c4,c2) && !eq(c5,A0)) p2a = avg2(c2,c5);\n"
    "  else p2a = c2;\n"
    "  if (eq(c2,c6) && !eq(c5,c3) && eq(c1,c2) && !eq(c2,B2)) p1a = avg2(c2,c5);\n"
    "  else if (eq(c4,c2) && eq(c3,c2) && !eq(c1,c5) && !eq(c2,B0)) p1a = avg2(c2,c5);\n"
    "  else p1a = c5;\n"
    "  vec3 r = (sub.y == 0) ? ((sub.x == 0) ? p1a : p1b) : ((sub.x == 0) ? p2a : p2b);\n"
    "  frag = vec4(r, 1.0);\n"
    "}\n";

/* ---- Super Eagle ------------------------------------------------------------- */
static const char *VF_SUPER_EAGLE_FS =
    VF_UP_PROLOGUE
    VF_GET_RESULT
    VF_UP_MAIN
    VF_SAI_NBR
    "  if (eq(c2,c6) && !eq(c5,c3)) {\n"
    "    p1b = c2; p2a = c2;\n"
    "    if (eq(c1,c2) || eq(c6,B2)) { p1a = avg2(c2,c5); p1a = avg2(c2,p1a); }\n"
    "    else p1a = avg2(c5,c6);\n"
    "    if (eq(c6,S2) || eq(c2,A1)) { p2b = avg2(c2,c3); p2b = avg2(c2,p2b); }\n"
    "    else p2b = avg2(c2,c3);\n"
    "  } else if (eq(c5,c3) && !eq(c2,c6)) {\n"
    "    p2b = c5; p1a = c5;\n"
    "    if (eq(B1,c5) || eq(c3,S1)) { p1b = avg2(c5,c6); p1b = avg2(c5,p1b); }\n"
    "    else p1b = avg2(c5,c6);\n"
    "    if (eq(c3,A2) || eq(c4,c5)) { p2a = avg2(c5,c2); p2a = avg2(c5,p2a); }\n"
    "    else p2a = avg2(c2,c3);\n"
    "  } else if (eq(c5,c3) && eq(c2,c6)) {\n"
    "    int r = 0;\n"
    "    r += getr(c6,c5,c1,A1); r += getr(c6,c5,c4,B1); r += getr(c6,c5,A2,S1); r += getr(c6,c5,B2,S2);\n"
    "    if (r > 0) { p1b = c2; p2a = c2; p1a = avg2(c5,c6); p2b = p1a; }\n"
    "    else if (r < 0) { p2b = c5; p1a = c5; p1b = avg2(c5,c6); p2a = p1b; }\n"
    "    else { p2b = c5; p1a = c5; p1b = c2; p2a = c2; }\n"
    "  } else {\n"
    "    vec3 t = avg2(c2,c6);\n"
    "    p2b = avg4(c3,c3,c3,t);\n"
    "    p1a = avg4(c5,c5,c5,t);\n"
    "    vec3 u = avg2(c5,c2);\n"
    "    p2a = avg4(c2,c2,c2,u);\n"
    "    p1b = avg4(c6,c6,c6,u);\n"
    "  }\n"
    "  vec3 r = (sub.y == 0) ? ((sub.x == 0) ? p1a : p1b) : ((sub.x == 0) ? p2a : p2b);\n"
    "  frag = vec4(r, 1.0);\n"
    "}\n";

/* ---- xBR level 2 (Hyllian) --------------------------------------------------- */
static const char *VF_XBR_FS =
    VF_UP_PROLOGUE
    VF_UP_MAIN
    "  /* Integer luma 299R+587G+114B of the 8-bit channels: exact in float on\n"
    "   * both CPU and GPU (see VF_XBR_EQ_THRESHOLD in video_filter.h). */\n"
    "  const float XBR_EQ_THRESHOLD = 79687.5;\n"
    "  const float XBR_LV2_COEFF = 2.0;\n"
    "  const float XBR_EPS = 0.5;\n"
    "  const vec3 rgbw = vec3(299.0, 587.0, 114.0);\n"
    "  #define LUM(c) dot(floor((c)*255.0 + 0.5), rgbw)\n"
    "  const vec4 Ao = vec4( 1.0, -1.0, -1.0,  1.0 );\n"
    "  const vec4 Bo = vec4( 1.0,  1.0, -1.0, -1.0 );\n"
    "  const vec4 Co = vec4( 1.5,  0.5, -0.5,  0.5 );\n"
    "  const vec4 Ax = vec4( 1.0, -1.0, -1.0,  1.0 );\n"
    "  const vec4 Bx = vec4( 0.5,  2.0, -0.5, -2.0 );\n"
    "  const vec4 Cx = vec4( 1.0,  1.0, -0.5,  0.0 );\n"
    "  const vec4 Ay = vec4( 1.0, -1.0, -1.0,  1.0 );\n"
    "  const vec4 By = vec4( 2.0,  0.5, -2.0, -0.5 );\n"
    "  const vec4 Cy = vec4( 2.0,  0.0, -1.0,  0.5 );\n"
    "  const vec4 Ci = vec4( 0.25, 0.25, 0.25, 0.25 );\n"
    "  float S = float(u_scale);\n"
    "  vec4 delta  = vec4(1.0/S);\n"
    "  vec4 deltaL = vec4(0.5/S, 1.0/S, 0.5/S, 1.0/S);\n"
    "  vec4 deltaU = deltaL.yxwz;\n"
    "  vec2 fp = (vec2(sub) + 0.5) / S;\n"
    "  vec3 A1=P(-1,-2), B1=P(0,-2), C1=P(1,-2);\n"
    "  vec3 A0=P(-2,-1), A=P(-1,-1), B=P(0,-1), C=P(1,-1), C4=P(2,-1);\n"
    "  vec3 D0=P(-2, 0), D=P(-1, 0), E=P(0, 0), F=P(1, 0), F4=P(2, 0);\n"
    "  vec3 G0=P(-2, 1), G=P(-1, 1), H=P(0, 1), I=P(1, 1), I4=P(2, 1);\n"
    "  vec3 G5=P(-1, 2), H5=P(0, 2), I5=P(1, 2);\n"
    "  vec4 b  = vec4(LUM(B), LUM(D), LUM(H), LUM(F));\n"
    "  vec4 c  = vec4(LUM(C), LUM(A), LUM(G), LUM(I));\n"
    "  vec4 d  = b.yzwx;\n"
    "  vec4 e  = vec4(LUM(E));\n"
    "  vec4 f  = b.wxyz;\n"
    "  vec4 g  = c.zwxy;\n"
    "  vec4 h  = b.zwxy;\n"
    "  vec4 i  = c.wxyz;\n"
    "  vec4 i4 = vec4(LUM(I4), LUM(C1), LUM(A0), LUM(G5));\n"
    "  vec4 i5 = vec4(LUM(I5), LUM(C4), LUM(A1), LUM(G0));\n"
    "  vec4 h5 = vec4(LUM(H5), LUM(F4), LUM(B1), LUM(D0));\n"
    "  vec4 f4 = h5.yzwx;\n"
    "  vec4 fx      = Ao*fp.y + Bo*fp.x;\n"
    "  vec4 fx_left = Ax*fp.y + Bx*fp.x;\n"
    "  vec4 fx_up   = Ay*fp.y + By*fp.x;\n"
    "  bvec4 lv0 = bvec4(notEqual(e,f).x && notEqual(e,h).x, notEqual(e,f).y && notEqual(e,h).y,\n"
    "                    notEqual(e,f).z && notEqual(e,h).z, notEqual(e,f).w && notEqual(e,h).w);\n"
    "  /* eq(): luma distance below threshold. */\n"
    "  #define EQ(a,b) lessThan(abs((a)-(b)), vec4(XBR_EQ_THRESHOLD))\n"
    "  #define NEQ(a,b) not(EQ(a,b))\n"
    "  bvec4 nfb = NEQ(f,b), nfc = NEQ(f,c), nhd = NEQ(h,d), nhg = NEQ(h,g);\n"
    "  bvec4 eei = EQ(e,i), nff4 = NEQ(f,f4), nfi4 = NEQ(f,i4), nhh5 = NEQ(h,h5), nhi5 = NEQ(h,i5);\n"
    "  bvec4 eeg = EQ(e,g), eec = EQ(e,c);\n"
    "  bvec4 lv1;\n"
    "  for (int k = 0; k < 4; k++) {\n"
    "    /* Corner rule C: keep more 90-degree corners intact. */\n"
    "    lv1[k] = lv0[k] && ((nfb[k] && nfc[k]) || (nhd[k] && nhg[k]) ||\n"
    "                        (eei[k] && ((nff4[k] && nfi4[k]) || (nhh5[k] && nhi5[k]))) ||\n"
    "                        eeg[k] || eec[k]);\n"
    "  }\n"
    "  bvec4 lv2l = bvec4(notEqual(e,g).x && notEqual(d,g).x, notEqual(e,g).y && notEqual(d,g).y,\n"
    "                     notEqual(e,g).z && notEqual(d,g).z, notEqual(e,g).w && notEqual(d,g).w);\n"
    "  bvec4 lv2u = bvec4(notEqual(e,c).x && notEqual(b,c).x, notEqual(e,c).y && notEqual(b,c).y,\n"
    "                     notEqual(e,c).z && notEqual(b,c).z, notEqual(e,c).w && notEqual(b,c).w);\n"
    "  /* Weights are exact multiples of 1/8 (see video_filter.c snap_eighth). */\n"
    "  #define Q4(v) (floor((v)*8.0 + 0.5)*0.125)\n"
    "  vec4 fx45i = Q4(clamp((fx      + delta  - Co - Ci)/(2.0*delta ), 0.0, 1.0));\n"
    "  vec4 fx45  = Q4(clamp((fx      + delta  - Co     )/(2.0*delta ), 0.0, 1.0));\n"
    "  vec4 fx30  = Q4(clamp((fx_left + deltaL - Cx     )/(2.0*deltaL), 0.0, 1.0));\n"
    "  vec4 fx60  = Q4(clamp((fx_up   + deltaU - Cy     )/(2.0*deltaU), 0.0, 1.0));\n"
    "  vec4 wd1 = abs(e-c) + abs(e-g) + abs(i-h5) + abs(i-f4) + 4.0*abs(h-f);\n"
    "  vec4 wd2 = abs(h-d) + abs(h-i5) + abs(f-i4) + abs(f-b) + 4.0*abs(e-i);\n"
    "  vec4 edri = step(wd1, wd2) * vec4(lv0);\n"
    "  vec4 edr  = step(wd1 + XBR_EPS, wd2) * vec4(lv1);\n"
    "  vec4 edr_left = step(XBR_LV2_COEFF*abs(f-g), abs(h-c)) * vec4(lv2l) * edr;\n"
    "  vec4 edr_up   = step(XBR_LV2_COEFF*abs(h-c), abs(f-g)) * vec4(lv2u) * edr;\n"
    "  fx45  = edr*fx45;\n"
    "  fx30  = edr_left*fx30;\n"
    "  fx60  = edr_up*fx60;\n"
    "  fx45i = edri*fx45i;\n"
    "  vec4 px = step(abs(e-f), abs(e-h));\n"
    "  vec4 maximos = max(max(fx30, fx60), max(fx45, fx45i));\n"
    "  /* Blend in the exact 0..255 integer domain (matches the CPU reference). */\n"
    "  #define C255(c) floor((c)*255.0 + 0.5)\n"
    "  vec3 E8 = C255(E), B8 = C255(B), D8 = C255(D), F8 = C255(F), H8 = C255(H);\n"
    "  vec3 res1 = E8;\n"
    "  res1 = mix(res1, mix(H8, F8, px.x), maximos.x);\n"
    "  res1 = mix(res1, mix(B8, D8, px.z), maximos.z);\n"
    "  vec3 res2 = E8;\n"
    "  res2 = mix(res2, mix(F8, B8, px.y), maximos.y);\n"
    "  res2 = mix(res2, mix(D8, H8, px.w), maximos.w);\n"
    "  vec3 dE1 = abs(E8-res1), dE2 = abs(E8-res2);\n"
    "  float c1 = dE1.r + dE1.g + dE1.b, c2 = dE2.r + dE2.g + dE2.b;\n"
    "  vec3 res = mix(res1, res2, step(c1, c2));\n"
    "  frag = vec4(res / 255.0, 1.0);\n"
    "}\n";

/* ---- Final pass: sharp bilinear / scanlines / CRT ------------------------------
 * Draws with PRESENT_VS. v_uv spans the letterbox quad in [0,1]^2 with row 0 =
 * top of the picture (the caller flips via u_uv_rect when the texture is
 * bottom-up). u_rect is the source rectangle in texels of u_tex, u_texsize the
 * texture size, u_out the viewport size in pixels, u_native the unscaled
 * source size in PS1 pixels (scanline pitch), u_mode 0 sharp / 1 scanlines /
 * 2 crt. The bound texture must have GL_LINEAR filtering (sharp/scanline
 * sample bilinearly at computed positions; CRT uses texelFetch). */
static const char *VF_FINAL_FS =
    "#version 330\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4  u_rect;\n"
    "uniform vec2  u_texsize;\n"
    "uniform vec2  u_out;\n"
    "uniform vec2  u_native;\n"
    "uniform int   u_mode;\n"
    "uniform vec3  u_scan;      /* scanline opacity, gap size, glow (video_filter.h) */\n"
    "out vec4 frag;\n"
    "vec3 sample_lin(vec2 pos){\n"
    "  pos = clamp(pos, u_rect.xy + 0.5, u_rect.xy + u_rect.zw - 0.5);\n"
    "  return texture(u_tex, pos / u_texsize).rgb; }\n"
    "vec3 fetch_px(vec2 pn){   /* pn: native pixel coords (rect-local) */\n"
    "  /* Source texels per native pixel is an integer (1..4). Never form it as\n"
    "   * zw/native: the compiler may lower that to zw*(1/native) = 0.99999994,\n"
    "   * which floors one line low on the display buffer at y=0 and not on the\n"
    "   * one at y=240 -> a one-line flicker between double buffers. */\n"
    "  vec2 sc = floor(u_rect.zw / u_native + 0.5);\n"
    "  vec2 t = floor(pn) * sc + u_rect.xy;\n"
    "  ivec2 ti = clamp(ivec2(floor(t + 0.5)), ivec2(u_rect.xy + 0.5), ivec2(u_rect.xy + u_rect.zw + 0.5) - 1);\n"
    "  return texelFetch(u_tex, ti, 0).rgb; }\n"
    "vec2 sharp_pos(vec2 p){\n"
    "  vec2 scale = max(floor(u_out / u_rect.zw + 1e-4), vec2(1.0));\n"
    "  vec2 tf = floor(p); vec2 s = fract(p);\n"
    "  vec2 region_range = 0.5 - 0.5 / scale;\n"
    "  vec2 cd = s - 0.5;\n"
    "  vec2 f = (cd - clamp(cd, -region_range, region_range)) * scale + 0.5;\n"
    "  return tf + f; }\n"
    "vec3 ToLinear(vec3 c){ return mix(c/12.92, pow((c+0.055)/1.055, vec3(2.4)), step(0.04045, c)); }\n"
    "vec3 ToSrgb(vec3 c){ return mix(c*12.92, 1.055*pow(c, vec3(1.0/2.4))-0.055, step(0.0031308, c)); }\n"
    "float Gaus(float pos, float scale){ return exp2(scale*pos*pos); }\n"
    "vec3 Fetch(vec2 pn, vec2 off){ return ToLinear(fetch_px(pn + off)); }\n"
    "vec2 Dist(vec2 pn){ return -((pn - floor(pn)) - 0.5); }\n"
    "vec3 Horz3(vec2 pn, float off){\n"
    "  vec3 b = Fetch(pn, vec2(-1.0, off)), c = Fetch(pn, vec2(0.0, off)), d = Fetch(pn, vec2(1.0, off));\n"
    "  float dst = Dist(pn).x; const float hardPix = -3.0;\n"
    "  float wb = Gaus(dst-1.0, hardPix), wc = Gaus(dst, hardPix), wd = Gaus(dst+1.0, hardPix);\n"
    "  return (b*wb + c*wc + d*wd) / (wb+wc+wd); }\n"
    "vec3 Horz5(vec2 pn, float off){\n"
    "  vec3 a = Fetch(pn, vec2(-2.0, off)), b = Fetch(pn, vec2(-1.0, off)), c = Fetch(pn, vec2(0.0, off)),\n"
    "       d = Fetch(pn, vec2(1.0, off)), e = Fetch(pn, vec2(2.0, off));\n"
    "  float dst = Dist(pn).x; const float hardPix = -3.0;\n"
    "  float wa = Gaus(dst-2.0, hardPix), wb = Gaus(dst-1.0, hardPix), wc = Gaus(dst, hardPix),\n"
    "        wd = Gaus(dst+1.0, hardPix), we = Gaus(dst+2.0, hardPix);\n"
    "  return (a*wa + b*wb + c*wc + d*wd + e*we) / (wa+wb+wc+wd+we); }\n"
    "float Scan(vec2 pn, float off){ float dst = Dist(pn).y; return Gaus(dst + off, -8.0); }\n"
    "vec3 Tri(vec2 pn){\n"
    "  vec3 a = Horz3(pn, -1.0), b = Horz5(pn, 0.0), c = Horz3(pn, 1.0);\n"
    "  float wa = Scan(pn, -1.0), wb = Scan(pn, 0.0), wc = Scan(pn, 1.0);\n"
    "  return a*wa + b*wb + c*wc; }\n"
    "vec3 Mask(vec2 pos){   /* aperture grille, 3 px pitch */\n"
    "  const float maskDark = 0.6, maskLight = 1.4;\n"
    "  vec3 mask = vec3(maskDark);\n"
    "  float x = fract(pos.x / 3.0);\n"
    "  if (x < 0.333) mask.r = maskLight; else if (x < 0.666) mask.g = maskLight; else mask.b = maskLight;\n"
    "  return mask; }\n"
    "void main(){\n"
    "  vec2 p = v_uv * u_rect.zw;              /* continuous texel coords inside the rect */\n"
    "  if (u_mode == 2) {\n"
    "    vec2 pn = v_uv * u_native;             /* native pixel coords */\n"

    "    vec3 c = Tri(pn) * Mask(gl_FragCoord.xy);\n"
    "    frag = vec4(ToSrgb(clamp(c, 0.0, 1.0)), 1.0);\n"
    "    return;\n"
    "  }\n"
    "  vec3 c = sample_lin(u_rect.xy + sharp_pos(p));\n"
    "  if (u_mode == 1) {\n"
    "    /* Scanlines: per native line, a bright core and a gap of `size`\n"
    "     * (u_scan.y) darkened by `opacity` (u_scan.x). `glow` (u_scan.z)\n"
    "     * brightens the core and bleeds the line + its neighbours into the\n"
    "     * gap (a cheap bloom), all in linear light. Energy lost to the gap is\n"
    "     * partly compensated so the picture does not just get darker. */\n"
    "    float opacity = u_scan.x, size = u_scan.y, glow = u_scan.z;\n"
    "    float d = abs(fract(v_uv.y * u_native.y) - 0.5) * 2.0;   /* 0 core .. 1 edge */\n"
    "    float e0 = 1.0 - size;\n"
    "    float soft = 0.18;\n"
    "    float gap = smoothstep(e0 - soft, e0 + soft, d);\n"
    "    float linePx = u_rect.w / u_native.y;                       /* texels per native line */\n"
    "    vec2  pos = u_rect.xy + sharp_pos(p);\n"
    "    vec3 L  = ToLinear(sample_lin(pos));\n"
    "    vec3 Lu = ToLinear(sample_lin(pos - vec2(0.0, linePx)));\n"
    "    vec3 Ld = ToLinear(sample_lin(pos + vec2(0.0, linePx)));\n"
    "    float w = 1.0 - opacity * gap;\n"
    "    vec3 res = L * w;\n"
    "    res += L * (glow * 0.45) * (1.0 - gap);                     /* brighter core */\n"
    "    res += (L + Lu + Ld) * (glow * 0.22) * gap * opacity;      /* bloom into the gap */\n"
    "    res *= 1.0 + 0.35 * opacity * size;                        /* energy compensation */\n"
    "    frag = vec4(ToSrgb(clamp(res, 0.0, 1.0)), 1.0);\n"
    "    return;\n"
    "  }\n"
    "  frag = vec4(c, 1.0);\n"
    "}\n";

#endif /* PSXRECOMP_GPU_GL_FILTER_SHADERS_H */
