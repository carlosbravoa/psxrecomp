#ifndef PSXRECOMP_PNG_READ_H
#define PSXRECOMP_PNG_READ_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Decode a PNG (or JPEG) file to host ARGB8888 (A<<24|R<<16|G<<8|B), malloc'd;
 * NULL on failure. Generic helper for small host-side images (border art,
 * overlays); the packs keep their own decoders. */
uint32_t *png_read_argb(const char *path, int *w, int *h);
#ifdef __cplusplus
}
#endif
#endif
