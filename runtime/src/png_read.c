#include "png_read.h"
#include <stdlib.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../third_party/stb_image.h"

uint32_t *png_read_argb(const char *path, int *w, int *h) {
    int iw = 0, ih = 0, comp = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    unsigned char *rgba = stbi_load(path, &iw, &ih, &comp, 4);
    if (!rgba || iw <= 0 || ih <= 0) { if (rgba) stbi_image_free(rgba); return NULL; }
    uint32_t *out = (uint32_t*)malloc((size_t)iw * ih * sizeof(uint32_t));
    if (!out) { stbi_image_free(rgba); return NULL; }
    for (size_t i = 0; i < (size_t)iw * ih; i++) {
        const unsigned char *p = rgba + i * 4;
        out[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    stbi_image_free(rgba);
    if (w) *w = iw;
    if (h) *h = ih;
    return out;
}
