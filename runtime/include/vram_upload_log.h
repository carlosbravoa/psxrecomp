#pragma once
/*
 * vram_upload_log.h — CPU->VRAM (GP0 A0h) transfer log.
 *
 * Every committed upload is recorded as {seq, frame, x, y, w, h, crc32 of the
 * payload, first 4 words}; while a dump directory is armed the raw payload
 * (w*h halfwords, little-endian) is also written to
 *   <dir>/<seq>_f<frame>_<x>_<y>_<w>x<h>.bin
 * so an offline tool can identify where the pixels came from (disc file /
 * archive section) — the asset-extraction measurement in ROADMAP track A —
 * and it is the same texture-identity primitive a texture-replacement pack
 * needs (track B). Debug-server command `vram_upload_log` (op=arm|disarm|
 * list|clear). Off = zero cost beyond one branch per upload.
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t seq;
    uint32_t frame;
    uint16_t x, y, w, h;
    uint32_t crc;
    uint32_t first_words[4];
} VramUploadEntry;

void vram_upload_log_note(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t *pixels, uint32_t frame);
/* Recording of entries is always on (ring of VRAM_UPLOAD_LOG_CAP); the dump
 * directory is optional. dir=NULL disarms the dump. Returns 0 on failure to
 * use the directory. */
int  vram_upload_log_arm(const char *dir);
void vram_upload_log_clear(void);
uint32_t vram_upload_log_total(void);           /* uploads seen since clear */
uint32_t vram_upload_log_count(void);           /* entries retained (<= cap) */
const VramUploadEntry* vram_upload_log_get(uint32_t index);  /* oldest first */
const char* vram_upload_log_dir(void);          /* "" when not dumping */

#ifdef __cplusplus
}
#endif
