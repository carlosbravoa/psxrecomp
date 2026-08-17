/* vram_upload_log.c — see vram_upload_log.h. */
#include "vram_upload_log.h"
#include "crc32.h"
#include <stdio.h>
#include <string.h>

#define VRAM_UPLOAD_LOG_CAP 16384u

static VramUploadEntry s_ring[VRAM_UPLOAD_LOG_CAP];
static uint32_t s_total = 0;
static char     s_dir[1024];

void vram_upload_log_note(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t *pixels, uint32_t frame)
{
    const size_t bytes = (size_t)w * (size_t)h * 2u;
    VramUploadEntry *e = &s_ring[s_total % VRAM_UPLOAD_LOG_CAP];
    e->seq = s_total;
    e->frame = frame;
    e->x = x; e->y = y; e->w = w; e->h = h;
    e->crc = crc32_compute((const uint8_t*)pixels, bytes);
    memset(e->first_words, 0, sizeof e->first_words);
    memcpy(e->first_words, pixels, bytes < 16 ? bytes : 16);
    if (s_dir[0]) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%06u_f%u_%u_%u_%ux%u.bin",
                 s_dir, s_total, frame, x, y, w, h);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(pixels, 1, bytes, f); fclose(f); }
    }
    s_total++;
}

int vram_upload_log_arm(const char *dir)
{
    if (!dir || !dir[0]) { s_dir[0] = 0; return 1; }
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/.vram_upload_log", dir);
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    return 1;
}

void vram_upload_log_clear(void) { s_total = 0; }
uint32_t vram_upload_log_total(void) { return s_total; }
uint32_t vram_upload_log_count(void)
{
    return s_total < VRAM_UPLOAD_LOG_CAP ? s_total : VRAM_UPLOAD_LOG_CAP;
}
const VramUploadEntry* vram_upload_log_get(uint32_t index)
{
    const uint32_t n = vram_upload_log_count();
    if (index >= n) return NULL;
    const uint32_t first = s_total > VRAM_UPLOAD_LOG_CAP ? s_total - VRAM_UPLOAD_LOG_CAP : 0;
    return &s_ring[(first + index) % VRAM_UPLOAD_LOG_CAP];
}
const char* vram_upload_log_dir(void) { return s_dir; }
