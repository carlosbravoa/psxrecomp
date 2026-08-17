/* fmv_pack.h — HD movie packs: present a host frame sequence in place of an
 * MDEC-decoded FMV (docs/FMV_PACKS.md).
 *
 * PRESENT-TIME ONLY, like texture packs and video filters: the game still
 * streams the STR from the disc, the MDEC still decodes every frame into RAM
 * and the game still uploads it to VRAM (24-bit) — savestates, netplay
 * digests, VRAM readbacks and the 1x picture are untouched. Only the present
 * of a 24-bit frame is substituted, and only while a movie the pack knows is
 * playing. Audio is the STR's own XA stream, so sync comes for free: the
 * replacement frame index IS the MDEC decode index within the movie.
 *
 * A pack is a directory of one sub-directory per movie, named after the STR
 * file on the disc (basename, no extension, as ISO9660 spells it, e.g.
 * ROCK8_0), each holding the frames as 00000.png / 00000.jpg, 00001.png, …
 * (0-based, one per MDEC-decoded frame; a missing index shows the native
 * frame). Any size; presented pillarboxed 4:3 like the native FMV. */
#ifndef PSXRECOMP_FMV_PACK_H
#define PSXRECOMP_FMV_PACK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while a pack is loaded (fast gate for the MDEC / present hooks). */
extern int g_fmv_pack_active;

int  fmv_pack_load(const char *dir);      /* returns number of movie directories, 0 = none/failed */
void fmv_pack_unload(void);

/* From the MDEC after a colour (15/24-bit) decode of a full frame: which disc
 * file the CD is streaming (NULL/"" = unknown) decides the movie; consecutive
 * decodes of the same movie count frames, a gap resets. Cheap when inactive. */
void fmv_pack_note_decode(const char *disc_file, uint32_t macroblocks);

/* Present-time: the replacement for the frame currently on screen, ARGB8888
 * (0xAARRGGBB, top-down, `w` x `h`, pitch = w). Returns 1 and keeps the
 * pointer valid until the next fmv_pack_note_decode() / unload; 0 = present
 * the native frame (no pack, movie unknown, frame missing, or not decoded
 * yet — decoding runs on a helper thread and never stalls the present). */
int  fmv_pack_current(const uint32_t **pixels, int *w, int *h);

/* {"loaded":N,"dir":"..","movie":"NAME","frame":N,"shown":N,"missing":N,"late":N,"decoded":N} */
int  fmv_pack_stats_json(char *buf, int cap);

/* Dump: while armed, every colour picture the MDEC decodes is written as
 * <dir>/<MOVIE>/NNNNN.png (the movie the CD is streaming, index counted like
 * the pack does) — the 1:1 native skeleton an HD pack replaces, with the
 * exact frame numbering. Column-major macroblock output is unwrapped with the
 * height inferred from the game's DMA-out column size (else 240). */
int  fmv_dump_arm(const char *dir);
void fmv_dump_disarm(void);
int  fmv_dump_armed(void);
void fmv_dump_note_decode(const char *disc_file, uint32_t macroblocks, const uint8_t *out,
                          uint32_t bytes, int depth, uint32_t dma_chunk_words);
/* {"armed":0|1,"dir":"..","frames":N} */
int  fmv_dump_stats_json(char *buf, int cap);

#ifdef __cplusplus
}
#endif
#endif
