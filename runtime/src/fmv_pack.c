/* fmv_pack.c — HD movie packs (fmv_pack.h, docs/FMV_PACKS.md).
 *
 * The MDEC tells us "frame N of movie M was just decoded" (the CD is streaming
 * file M; N counts colour decodes since the movie started); a helper thread
 * decodes <pack>/M/N.png|jpg (and prefetches N+1) into ARGB buffers; the
 * presenter asks for the frame that matches the latest decode and gets it if
 * it is ready — never waits. Everything the game sees is untouched. */
#include "fmv_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../third_party/stb_image.h"
#include "png_write.h"

extern uint64_t s_frame_count;   /* debug_server.c: host frame counter */

int g_fmv_pack_active = 0;

/* ---- pack contents ------------------------------------------------------- */
#define MAX_MOVIES 64
typedef struct {
    char name[64];            /* ISO basename without extension, upper case */
    int  offset;              /* movie.toml: frame index offset (pack index = decode index + offset) */
    int  decodes_per_frame;   /* movie.toml: MDEC decodes per picture (strip decoders) */
    int  frames;              /* count of NNNNN.png/jpg present (informational) */
} Movie;
static char  s_dir[1024];
static Movie s_movies[MAX_MOVIES];
static int   s_movie_n = 0;

/* ---- playback state (main thread) ------------------------------------------ */
static int      s_cur_movie = -1;      /* index into s_movies, -1 = none */
static int      s_cur_index = -1;      /* MDEC decode index within the movie */
static uint32_t s_decodes_in_frame = 0;
static uint64_t s_last_decode_frame = 0;
static uint64_t s_shown = 0, s_missing = 0, s_late = 0, s_decoded = 0;

/* ---- decoded-frame cache shared with the worker ----------------------------- */
#define SLOTS 3
typedef struct {
    int movie, index;         /* -1 = empty */
    int w, h;
    int missing;              /* file absent / undecodable: remember, do not retry */
    uint32_t *px;
    size_t cap;               /* pixels allocated */
} Slot;
static Slot s_slot[SLOTS];
static int  s_want_movie = -1, s_want_index = -1;   /* what the main thread wants next */
static int  s_quit = 0, s_thread_live = 0;

#ifdef _WIN32
static CRITICAL_SECTION s_lock; static CONDITION_VARIABLE s_cv; static HANDLE s_thread;
static void lock_(void)   { EnterCriticalSection(&s_lock); }
static void unlock_(void) { LeaveCriticalSection(&s_lock); }
static void wait_(void)   { SleepConditionVariableCS(&s_cv, &s_lock, INFINITE); }
static void wake_(void)   { WakeAllConditionVariable(&s_cv); }
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv   = PTHREAD_COND_INITIALIZER;
static pthread_t       s_thread;
static void lock_(void)   { pthread_mutex_lock(&s_lock); }
static void unlock_(void) { pthread_mutex_unlock(&s_lock); }
static void wait_(void)   { pthread_cond_wait(&s_cv, &s_lock); }
static void wake_(void)   { pthread_cond_broadcast(&s_cv); }
#endif

static int slot_find(int movie, int index) {
    for (int i = 0; i < SLOTS; i++)
        if (s_slot[i].movie == movie && s_slot[i].index == index) return i;
    return -1;
}

/* Decode one frame file into a temporary buffer (worker thread, no lock). */
static int decode_file(int movie, int index, uint32_t **out, int *w, int *h) {
    static const char *ext[] = { "png", "jpg", "jpeg" };
    char path[1400];
    for (unsigned e = 0; e < sizeof ext / sizeof ext[0]; e++) {
        snprintf(path, sizeof path, "%s/%s/%05d.%s", s_dir, s_movies[movie].name, index, ext[e]);
        int iw = 0, ih = 0, comp = 0;
        unsigned char *rgba = stbi_load(path, &iw, &ih, &comp, 4);
        if (!rgba) continue;
        if (iw <= 0 || ih <= 0 || iw > 8192 || ih > 8192) { stbi_image_free(rgba); return 0; }
        uint32_t *px = (uint32_t *)malloc((size_t)iw * ih * sizeof(uint32_t));
        if (!px) { stbi_image_free(rgba); return 0; }
        for (size_t i = 0; i < (size_t)iw * ih; i++) {
            const unsigned char *p = rgba + i * 4;
            px[i] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
        stbi_image_free(rgba);
        *out = px; *w = iw; *h = ih;
        return 1;
    }
    return 0;
}

static void store_result(int movie, int index, uint32_t *px, int w, int h) {
    /* Replace the slot furthest from what is wanted (never the wanted one). */
    lock_();
    int victim = -1, worst = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i].movie == movie && s_slot[i].index == index) { victim = i; break; }
        int d = (s_slot[i].movie < 0) ? 1 << 30
              : (s_slot[i].movie != s_want_movie) ? (1 << 29)
              : abs(s_slot[i].index - s_want_index) + (s_slot[i].index < s_want_index ? 1000 : 0);
        if (d > worst) { worst = d; victim = i; }
    }
    Slot *s = &s_slot[victim];
    free(s->px);
    s->px = px; s->w = w; s->h = h; s->cap = px ? (size_t)w * h : 0;
    s->movie = movie; s->index = index; s->missing = px == NULL;
    if (px) s_decoded++;
    unlock_();
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        int movie, index;
        lock_();
        for (;;) {
            if (s_quit) { unlock_(); return NULL; }
            if (s_want_movie >= 0) {
                if (slot_find(s_want_movie, s_want_index) < 0) { movie = s_want_movie; index = s_want_index; break; }
                if (slot_find(s_want_movie, s_want_index + 1) < 0) { movie = s_want_movie; index = s_want_index + 1; break; }
            }
            wait_();
        }
        unlock_();
        uint32_t *px = NULL; int w = 0, h = 0;
        decode_file(movie, index, &px, &w, &h);
        store_result(movie, index, px, w, h);
    }
}

static void thread_start(void) {
    if (s_thread_live) return;
    s_quit = 0;
#ifdef _WIN32
    static int init = 0;
    if (!init) { InitializeCriticalSection(&s_lock); InitializeConditionVariable(&s_cv); init = 1; }
    s_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)worker, NULL, 0, NULL);
    s_thread_live = s_thread != NULL;
#else
    s_thread_live = pthread_create(&s_thread, NULL, worker, NULL) == 0;
#endif
}
static void thread_stop(void) {
    if (!s_thread_live) return;
    lock_(); s_quit = 1; wake_(); unlock_();
#ifdef _WIN32
    WaitForSingleObject(s_thread, INFINITE); CloseHandle(s_thread);
#else
    pthread_join(s_thread, NULL);
#endif
    s_thread_live = 0;
}

/* ---- pack loading ------------------------------------------------------------ */
static void read_movie_toml(Movie *m) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s/movie.toml", s_dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64]; long v;
        if (sscanf(line, " %63[a-z_] = %ld", key, &v) == 2) {
            if (!strcmp(key, "offset")) m->offset = (int)v;
            else if (!strcmp(key, "decodes_per_frame") && v >= 1 && v <= 64) m->decodes_per_frame = (int)v;
        }
    }
    fclose(f);
}

static int count_frames(const Movie *m) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", s_dir, m->name);
    DIR *d = opendir(path);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        size_t len = strlen(nm);
        if (len >= 9 && isdigit((unsigned char)nm[0]) && (strcasecmp(nm + len - 4, ".png") == 0 ||
            strcasecmp(nm + len - 4, ".jpg") == 0 || (len >= 10 && strcasecmp(nm + len - 5, ".jpeg") == 0)))
            n++;
    }
    closedir(d);
    return n;
}

int fmv_pack_load(const char *dir) {
    fmv_pack_unload();
    if (!dir || !dir[0]) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_movie_n < MAX_MOVIES) {
        if (de->d_name[0] == '.') continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        Movie *m = &s_movies[s_movie_n];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", de->d_name);
        for (char *p = m->name; *p; p++) *p = (char)toupper((unsigned char)*p);
        m->decodes_per_frame = 1;
        read_movie_toml(m);
        m->frames = count_frames(m);
        if (m->frames > 0) s_movie_n++;
    }
    closedir(d);
    if (!s_movie_n) { s_dir[0] = 0; return 0; }
    for (int i = 0; i < SLOTS; i++) { s_slot[i].movie = s_slot[i].index = -1; }
    s_cur_movie = -1; s_cur_index = -1; s_want_movie = -1; s_want_index = -1;
    s_shown = s_missing = s_late = s_decoded = 0;
    thread_start();
    g_fmv_pack_active = s_thread_live ? 1 : 0;
    return g_fmv_pack_active ? s_movie_n : 0;
}

void fmv_pack_unload(void) {
    g_fmv_pack_active = 0;
    thread_stop();
    for (int i = 0; i < SLOTS; i++) { free(s_slot[i].px); memset(&s_slot[i], 0, sizeof s_slot[i]); s_slot[i].movie = s_slot[i].index = -1; }
    s_movie_n = 0; s_dir[0] = 0;
    s_cur_movie = -1; s_cur_index = -1; s_want_movie = -1; s_want_index = -1;
}

/* ---- hooks ---------------------------------------------------------------- */
static int movie_for_file(const char *disc_file) {
    if (!disc_file || !disc_file[0]) return -1;
    const char *base = strrchr(disc_file, '/');
    base = base ? base + 1 : disc_file;
    if (strrchr(base, '\\')) base = strrchr(base, '\\') + 1;
    char name[64];
    size_t n = 0;
    for (; base[n] && base[n] != '.' && base[n] != ';' && n < sizeof name - 1; n++)
        name[n] = (char)toupper((unsigned char)base[n]);
    name[n] = 0;
    for (int i = 0; i < s_movie_n; i++)
        if (!strcmp(s_movies[i].name, name)) return i;
    return -1;
}

void fmv_pack_note_decode(const char *disc_file, uint32_t macroblocks) {
    if (!g_fmv_pack_active || macroblocks < 4) return;
    const int movie = movie_for_file(disc_file);
    const uint64_t now = s_frame_count;
    if (movie < 0) { s_cur_movie = -1; s_cur_index = -1; return; }
    if (movie != s_cur_movie || now - s_last_decode_frame > 30) {
        s_cur_movie = movie; s_cur_index = 0; s_decodes_in_frame = 1;
    } else if (++s_decodes_in_frame >= (uint32_t)s_movies[movie].decodes_per_frame) {
        s_decodes_in_frame = 0; s_cur_index++;
    } else {
        s_last_decode_frame = now;
        return;                                   /* a strip, not a new picture */
    }
    s_last_decode_frame = now;
    lock_();
    s_want_movie = movie; s_want_index = s_cur_index + s_movies[movie].offset;
    wake_();
    unlock_();
}

int fmv_pack_current(const uint32_t **pixels, int *w, int *h) {
    if (!g_fmv_pack_active || s_cur_movie < 0) return 0;
    if (s_frame_count - s_last_decode_frame > 30) return 0;   /* movie over: native again */
    int ok = 0;
    lock_();
    int i = slot_find(s_want_movie, s_want_index);
    if (i >= 0) {
        if (s_slot[i].missing) s_missing++;
        else { *pixels = s_slot[i].px; *w = s_slot[i].w; *h = s_slot[i].h; ok = 1; s_shown++; }
    } else {
        s_late++;
    }
    unlock_();
    return ok;
}

int fmv_pack_stats_json(char *buf, int cap) {
    return snprintf(buf, (size_t)cap,
        "{\"loaded\":%d,\"dir\":\"%s\",\"movie\":\"%s\",\"frame\":%d,\"shown\":%llu,\"missing\":%llu,\"late\":%llu,\"decoded\":%llu}",
        s_movie_n, s_dir, s_cur_movie >= 0 ? s_movies[s_cur_movie].name : "", s_cur_index,
        (unsigned long long)s_shown, (unsigned long long)s_missing, (unsigned long long)s_late,
        (unsigned long long)s_decoded);
}

/* ---- dump ------------------------------------------------------------------- */
static char     s_dump_dir[1024];
static int      s_dump_movie_known = 0;
static char     s_dump_movie[64];
static int      s_dump_index = -1;
static uint64_t s_dump_last_frame = 0, s_dump_frames = 0;

int fmv_dump_armed(void) { return s_dump_dir[0] != 0; }
int fmv_dump_arm(const char *dir) {
    if (!dir || !dir[0]) return 0;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    snprintf(s_dump_dir, sizeof s_dump_dir, "%s", dir);
    s_dump_movie[0] = 0; s_dump_movie_known = 0; s_dump_index = -1; s_dump_frames = 0;
    return 1;
}
void fmv_dump_disarm(void) { s_dump_dir[0] = 0; }

static void movie_name_of(const char *disc_file, char *out, size_t cap) {
    const char *base = disc_file ? strrchr(disc_file, '/') : NULL;
    base = base ? base + 1 : (disc_file ? disc_file : "");
    size_t n = 0;
    for (; base[n] && base[n] != '.' && base[n] != ';' && n < cap - 1; n++) out[n] = (char)toupper((unsigned char)base[n]);
    out[n] = 0;
    if (!n) snprintf(out, cap, "UNKNOWN");
}

void fmv_dump_note_decode(const char *disc_file, uint32_t macroblocks, const uint8_t *out,
                          uint32_t bytes, int depth, uint32_t dma_chunk_words) {
    if (!s_dump_dir[0] || !out || macroblocks < 4) return;
    const int bpp = depth == 2 ? 3 : 2;               /* 24-bit RGB, else 15-bit halfwords */
    if (bytes < macroblocks * 256u * (uint32_t)bpp) return;
    char name[64];
    movie_name_of(disc_file, name, sizeof name);
    const uint64_t now = s_frame_count;
    if (strcmp(name, s_dump_movie) != 0 || now - s_dump_last_frame > 30) {
        snprintf(s_dump_movie, sizeof s_dump_movie, "%s", name);
        s_dump_index = 0;
        char dpath[1200];
        snprintf(dpath, sizeof dpath, "%s/%s", s_dump_dir, s_dump_movie);
        mkdir(dpath, 0755);
    } else {
        s_dump_index++;
    }
    s_dump_last_frame = now;
    /* geometry: one DMA-out chunk = one 16-pixel column -> height */
    int h = 0;
    if (dma_chunk_words) {
        const uint32_t col_bytes = dma_chunk_words * 4u;
        if (col_bytes % (16u * (uint32_t)bpp) == 0) h = (int)(col_bytes / (16u * (uint32_t)bpp));
    }
    if (h <= 0 || h % 16 || (macroblocks % (uint32_t)(h / 16)) != 0) h = 240;
    if (macroblocks % (uint32_t)(h / 16) != 0) h = 16;   /* last resort: one row of macroblocks */
    const int mb_rows = h / 16, mb_cols = (int)macroblocks / mb_rows, w = mb_cols * 16;
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const size_t mb = (size_t)(x / 16) * mb_rows + (size_t)(y / 16);
            const size_t off = (mb * 256 + (size_t)(y % 16) * 16 + (size_t)(x % 16)) * (size_t)bpp;
            uint8_t *d = rgb + ((size_t)y * w + x) * 3;
            if (bpp == 3) { d[0] = out[off]; d[1] = out[off + 1]; d[2] = out[off + 2]; }
            else {
                const uint16_t c = (uint16_t)(out[off] | (out[off + 1] << 8));
                d[0] = (uint8_t)(((c) & 31) * 255 / 31); d[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31); d[2] = (uint8_t)(((c >> 10) & 31) * 255 / 31);
            }
        }
    char path[1300];
    snprintf(path, sizeof path, "%s/%s/%05d.png", s_dump_dir, s_dump_movie, s_dump_index);
    FILE *f = fopen(path, "wb");
    if (f) { if (png_write_rgb(f, rgb, (uint32_t)w, (uint32_t)h)) s_dump_frames++; fclose(f); }
    free(rgb);
}

int fmv_dump_stats_json(char *buf, int cap) {
    return snprintf(buf, (size_t)cap, "{\"armed\":%d,\"dir\":\"%s\",\"movie\":\"%s\",\"index\":%d,\"frames\":%llu}",
                    s_dump_dir[0] ? 1 : 0, s_dump_dir, s_dump_movie, s_dump_index, (unsigned long long)s_dump_frames);
}
