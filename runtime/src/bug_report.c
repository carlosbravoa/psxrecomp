/* bug_report.c — one-key diagnostics bundle. See bug_report.h.
 *
 * Deliberately built ON TOP of the debug server's command handlers
 * (debug_server_run_local): every fact in report.json is the same JSON a
 * developer would get over TCP, so a bundle from a player's release build is
 * directly comparable with a live debug session, and adding telemetry means
 * adding a debug command — one implementation, two consumers. */

#include "bug_report.h"

#include "debug_server.h"
#include "gpu_gl_renderer.h"
#include "host_osd.h"
#include "savestate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define bug_mkdir(p) _mkdir(p)
#else
#define bug_mkdir(p) mkdir((p), 0755)
#endif

extern int psx_present_request_capture(const char* path);   /* main.cpp */

static char s_last_dir[600];
static int  s_state_pending = 0;

const char* bug_report_last_dir(void) { return s_last_dir; }

static int mkdir_p(const char* path) {
    char tmp[600];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp) return 0;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            if (bug_mkdir(tmp) != 0 && errno != EEXIST) { /* keep going: a parent may be a drive root */ }
            tmp[i] = c;
        }
    }
    if (bug_mkdir(tmp) != 0 && errno != EEXIST) return 0;
    struct stat st;
    return stat(tmp, &st) == 0;
}

/* Strip the trailing newline(s) of a debug-server response so it embeds as a
 * JSON value; an empty/NULL response becomes null. */
static void write_json_response(FILE* f, const char* key, char* resp) {
    fprintf(f, "    \"%s\": ", key);
    if (!resp || !resp[0]) { fputs("null", f); free(resp); return; }
    size_t n = strlen(resp);
    while (n && (resp[n - 1] == '\n' || resp[n - 1] == '\r')) resp[--n] = 0;
    /* Multi-line responses (rare) become an array of lines. */
    if (strchr(resp, '\n')) {
        fputs("[", f);
        int first = 1;
        char* p = resp;
        while (p && *p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = 0;
            if (*p) { fprintf(f, "%s%s", first ? "" : ",", p); first = 0; }
            p = nl ? nl + 1 : NULL;
        }
        fputs("]", f);
    } else {
        fputs(resp, f);
    }
    free(resp);
}

/* Every query that goes into report.json. All are read-only diagnostics.
 * A command missing from a build simply answers {"ok":false} and is kept as
 * such (the absence is itself information). */
static const struct { const char* key; const char* line; } s_queries[] = {
    { "ping",                  "{\"cmd\":\"ping\"}" },
    { "gpu_state",             "{\"cmd\":\"gpu_state\"}" },
    { "video_filter",          "{\"cmd\":\"video_filter\"}" },
    { "window_size",           "{\"cmd\":\"window_size\"}" },
    { "gl_interp",             "{\"cmd\":\"gl_interp\"}" },
    { "gl_present_ring",       "{\"cmd\":\"gl_present_ring\",\"n\":16}" },
    { "overlay_loader_status", "{\"cmd\":\"overlay_loader_status\"}" },
    { "autocompile_status",    "{\"cmd\":\"autocompile_status\"}" },
    { "pad_status",            "{\"cmd\":\"pad_status\"}" },
    { "turbo_state",           "{\"cmd\":\"turbo_state\"}" },
    { "display_ring_stats",    "{\"cmd\":\"display_ring_stats\"}" },
    { "gpu_ring_stats",        "{\"cmd\":\"gpu_ring_stats\"}" },
    { "input_route_status",    "{\"cmd\":\"input_route_status\"}" },
};

int bug_report_capture(const char* trigger) {
    const char* root = savestate_root_dir();
    if (!root || !root[0]) root = ".";
    if (!trigger || !trigger[0]) trigger = "manual";

    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);

    char base[600], dir[600];
    const char last = root[strlen(root) - 1];
    snprintf(base, sizeof base, "%s%sbugreports", root,
             (last == '/' || last == '\\') ? "" : "/");
    if (!mkdir_p(base)) return 0;
    snprintf(dir, sizeof dir, "%s/%s_%s", base, stamp, trigger);
    /* Second press within the same second: suffix. */
    for (int k = 1; k < 10; k++) {
        struct stat st;
        if (stat(dir, &st) != 0) break;
        snprintf(dir, sizeof dir, "%s/%s_%s-%d", base, stamp, trigger, k);
    }
    if (!mkdir_p(dir)) return 0;
    strncpy(s_last_dir, dir, sizeof s_last_dir - 1);
    s_last_dir[sizeof s_last_dir - 1] = 0;

    char path[700], line[900];

    /* 1. Native frame + presented-surface frame (synchronous, via the same
     *    handlers the debug server exposes). */
    snprintf(path, sizeof path, "%s/frame.png", dir);
    snprintf(line, sizeof line, "{\"cmd\":\"screenshot\",\"path\":\"%s\"}", path);
    free(debug_server_run_local(line));
    snprintf(path, sizeof path, "%s/frame_hires.png", dir);
    snprintf(line, sizeof line, "{\"cmd\":\"screenshot_hires\",\"path\":\"%s\"}", path);
    free(debug_server_run_local(line));

    /* 2. Presented window (async: next present) and a reloadable savestate
     *    (async: next safe point). */
    snprintf(path, sizeof path, "%s/screen.png", dir);
    gl_renderer_present_capture_companions(0);
    const int screen_queued = psx_present_request_capture(path);
    snprintf(path, sizeof path, "%s/state.pst", dir);
    s_state_pending = savestate_request_save_path(path);

    /* 3. report.json */
    snprintf(path, sizeof path, "%s/report.json", dir);
    FILE* f = fopen(path, "wb");
    if (f) {
        char iso[40];
        strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", &tmv);
        fprintf(f, "{\n  \"bundle\": {\n    \"dir\": \"%s\",\n    \"time\": \"%s\",\n"
                   "    \"trigger\": \"%s\",\n    \"screen_png_queued\": %s,\n"
                   "    \"state_pst_queued\": %s\n  },\n",
                dir, iso, trigger, screen_queued ? "true" : "false",
                s_state_pending ? "true" : "false");
        char host[4096];
        host[0] = 0;
        psx_host_report_json(host, (int)sizeof host);
        fprintf(f, "  \"host\": {%s},\n", host);
        fprintf(f, "  \"runtime\": {\n");
        for (size_t i = 0; i < sizeof s_queries / sizeof s_queries[0]; i++) {
            write_json_response(f, s_queries[i].key, debug_server_run_local(s_queries[i].line));
            fputs(i + 1 < sizeof s_queries / sizeof s_queries[0] ? ",\n" : "\n", f);
        }
        fprintf(f, "  }\n}\n");
        fclose(f);
    }

    /* 4. README */
    snprintf(path, sizeof path, "%s/README.txt", dir);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f,
            "psxrecomp bug-report bundle (%s, trigger: %s)\n\n"
            "frame.png        the frame the game drew when you pressed the key (native scanout)\n"
            "frame_hires.png  the presented surface (supersampled / same as frame.png at 1x)\n"
            "screen.png       the window as shown (after the video filter, before OSD)\n"
            "state.pst        savestate at that moment - reload it to reproduce:\n"
            "                 copy it over saves/<bios>/state_<entry>_slotNN.pst and load slot NN,\n"
            "                 or on a debug build: {\"cmd\":\"savestate\",\"op\":\"load_path\",\"path\":\"...state.pst\"}\n"
            "report.json      settings, window/video state, frame counter, dispatch misses,\n"
            "                 GPU/overlay/autocompile status, present ring\n\n"
            "Attach the whole folder to the report. Nothing in it is personal beyond the\n"
            "file paths and the memory-card directory name.\n", stamp, trigger);
        fclose(f);
    }

    char toast[128];
    snprintf(toast, sizeof toast, "Bug report: bugreports/%s_%s", stamp, trigger);
    host_osd_push(toast, 3500);
    fprintf(stdout, "psxrecomp: bug report bundle -> %s\n", dir);
    fflush(stdout);
    return 1;
}

void bug_report_on_state_saved(int ok) {
    if (!s_state_pending) return;
    s_state_pending = 0;
    if (!ok) host_osd_push("Bug report: state.pst FAILED", 3000);
}
