#include "autocompile.h"
#include "overlay_loader.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- portable shims ------------------------------------------------------
 * The pipeline under test is platform-neutral (see the platform-primitive
 * layer in autocompile.c), so this harness is too. */
#ifdef _WIN32
typedef DWORD tid_t;
#  define TID_SELF()      GetCurrentThreadId()
#  define TID_EQ(a, b)    ((a) == (b))
typedef LONG counter_t;
#  define COUNTER_INC(x)  InterlockedIncrement(&(x))
typedef HANDLE worker_t;
#  define WORKER_RET      DWORD WINAPI
static int worker_start(worker_t *w, DWORD (WINAPI *fn)(void *), void *arg) {
    *w = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *w != NULL;
}
static void worker_join(worker_t *w) {
    WaitForSingleObject(*w, INFINITE); CloseHandle(*w);
}
static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
typedef pthread_t tid_t;
#  define TID_SELF()      pthread_self()
#  define TID_EQ(a, b)    pthread_equal((a), (b))
typedef long counter_t;
#  define COUNTER_INC(x)  __atomic_add_fetch(&(x), 1, __ATOMIC_SEQ_CST)
typedef pthread_t worker_t;
#  define WORKER_RET      void *
static int worker_start(worker_t *w, void *(*fn)(void *), void *arg) {
    return pthread_create(w, NULL, fn, arg) == 0;
}
static void worker_join(worker_t *w) { pthread_join(*w, NULL); }
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
static void sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

void autocompile_test_feed_output(const char *buf, int n);
int autocompile_test_start_preparer(void);
void autocompile_test_finish_input(void);
int autocompile_test_join_preparer(unsigned timeout_ms);
int autocompile_test_ready_count(void);
int autocompile_test_ready_highwater(void);
int autocompile_test_preparing_count(void);
void autocompile_test_discard_all(void);

struct OverlayPreparedImage {
    unsigned id;
};

static tid_t s_main_thread;
static counter_t s_prepared;
static counter_t s_committed;
static counter_t s_discarded;
static counter_t s_wrong_prepare_thread;
static counter_t s_wrong_commit_thread;

OverlayPreparedImage *overlay_loader_prepare_published(const char *path) {
    OverlayPreparedImage *image =
        (OverlayPreparedImage *)calloc(1, sizeof(*image));
    if (!image) return NULL;
    image->id = (unsigned)strtoul(strrchr(path, '_') + 1, NULL, 16);
    if (TID_EQ(TID_SELF(), s_main_thread))
        COUNTER_INC(s_wrong_prepare_thread);
    COUNTER_INC(s_prepared);
    return image;
}

int overlay_loader_commit_published(OverlayPreparedImage *image) {
    if (!TID_EQ(TID_SELF(), s_main_thread))
        COUNTER_INC(s_wrong_commit_thread);
    free(image);
    COUNTER_INC(s_committed);
    return 1;
}

void overlay_loader_discard_prepared(OverlayPreparedImage *image) {
    free(image);
    COUNTER_INC(s_discarded);
}

void overlay_loader_rescan(void) {}

typedef struct {
    const char *text;
    int length;
} FeedArgs;

static WORKER_RET feed_thread(void *opaque) {
    FeedArgs *args = (FeedArgs *)opaque;
    autocompile_test_feed_output(args->text, args->length);
    autocompile_test_finish_input();
    return (WORKER_RET)0;
}

int main(void) {
    enum { ITEM_COUNT = 300 };
    s_main_thread = TID_SELF();
    /* The shared runtime polls every title. A title with overlay autocompile
     * disabled must not touch the publication lock before configuration. */
    autocompile_poll_main();
    autocompile_configure("unused", ".");

    size_t capacity = (size_t)ITEM_COUNT * 96u;
    char *text = (char *)malloc(capacity);
    if (!text) return 1;
    int length = snprintf(text, capacity,
        "PSX_SHARD_RESULT ok=17 failed=0 skipped=4\n");
    for (unsigned i = 0; i < ITEM_COUNT; ++i) {
        length += snprintf(text + length, capacity - (size_t)length,
            "PSX_SHARD_PUBLISHED C:\\cache\\00010000_DEADBEEF_%08X.dll\n", i);
    }

    FeedArgs args = { text, length };
    if (!autocompile_test_start_preparer()) { free(text); return 1; }
    worker_t worker;
    if (!worker_start(&worker, feed_thread, &args)) { free(text); return 1; }
    uint64_t deadline = now_ms() + 10000u;
    while (s_committed < ITEM_COUNT && now_ms() < deadline) {
        autocompile_poll_main();
        sleep_ms(1);
    }
    worker_join(&worker);
    if (!autocompile_test_join_preparer(1000)) {
        fprintf(stderr, "FAIL: preparer did not exit\n");
        return 1;
    }
    /* Finalize the synthetic run.  The 300 publication lines are well over
     * AC_OUT_CAP, so the result marker at the beginning is no longer present
     * in output_tail.  Its counters must nevertheless survive streaming. */
    autocompile_poll_main();
    free(text);

    char status[2048];
    autocompile_status_json(status, sizeof(status));
    if (!strstr(status, "\"shard_ok\":17") ||
        !strstr(status, "\"shard_fail\":0") ||
        !strstr(status, "\"shard_skipped\":4") ||
        !strstr(status, "\"shard_result_seen\":1")) {
        fprintf(stderr, "FAIL: streamed shard result was lost after tail eviction: %s\n",
                status);
        return 1;
    }

    if (s_prepared != ITEM_COUNT || s_committed != ITEM_COUNT ||
        autocompile_test_ready_count() != 0 ||
        autocompile_test_preparing_count() != 0 ||
        autocompile_test_ready_highwater() > 1 ||
        s_wrong_prepare_thread != 0 || s_wrong_commit_thread != 0) {
        fprintf(stderr,
                "FAIL: bounded handoff prep=%ld commit=%ld ready=%d "
                "preparing=%d highwater=%d wrong=%ld/%ld\n",
                (long)s_prepared, (long)s_committed, autocompile_test_ready_count(),
                autocompile_test_preparing_count(),
                autocompile_test_ready_highwater(),
                (long)s_wrong_prepare_thread, (long)s_wrong_commit_thread);
        autocompile_test_discard_all();
        return 1;
    }

    /* A prepared-but-uncommitted item owns one reference and must be consumed
     * by reset/shutdown discard exactly once. */
    const char extra[] =
        "PSX_SHARD_PUBLISHED C:\\cache\\00010000_DEADBEEF_FFFFFFFF.dll\n";
    args.text = extra;
    args.length = (int)strlen(extra);
    if (!autocompile_test_start_preparer()) return 1;
    if (!worker_start(&worker, feed_thread, &args)) return 1;
    worker_join(&worker);
    if (!autocompile_test_join_preparer(1000)) return 1;
    autocompile_test_discard_all();
    if (s_discarded != 1 || autocompile_test_ready_count() != 0) {
        fprintf(stderr, "FAIL: prepared reference discard count=%ld\n",
                (long)s_discarded);
        return 1;
    }

    /* Shutdown mid-run: feed markers, never signal input-done, and tear down
     * while the preparer may be anywhere in its loop. autocompile_shutdown
     * must join the worker (never abandon it) and conserve every reference:
     * prepared == committed + discarded, with nothing left queued. */
    {
        static char feed[8 * 96];
        int len = 0;
        for (unsigned i = 0; i < 8; ++i)
            len += snprintf(feed + len, sizeof(feed) - (size_t)len,
                "PSX_SHARD_PUBLISHED C:\\cache\\00010000_DEADBEEF_%08X.dll\n",
                0x1000u + i);
        if (!autocompile_test_start_preparer()) return 1;
        autocompile_test_feed_output(feed, len);
        autocompile_shutdown();   /* input never finished — stop must win */
    }
    if (autocompile_test_ready_count() != 0 ||
        autocompile_test_preparing_count() != 0 ||
        s_prepared != s_committed + s_discarded) {
        fprintf(stderr,
                "FAIL: shutdown conservation prep=%ld commit=%ld discard=%ld "
                "ready=%d preparing=%d\n",
                (long)s_prepared, (long)s_committed, (long)s_discarded,
                autocompile_test_ready_count(),
                autocompile_test_preparing_count());
        return 1;
    }

    puts("PASS: publications prepare off-thread, backpressure at one mapped "
         "images, commit on the emulation thread, and shutdown conserves "
         "every prepared reference");
    return 0;
}
