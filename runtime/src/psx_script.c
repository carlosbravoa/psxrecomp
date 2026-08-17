/* psx_script.c — built-in session script. See psx_script.h. */

#include "psx_script.h"

#include "debug_server.h"
#include "host_time.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_MAX_STEPS 4096

static char*  s_text = NULL;              /* owned copy, steps NUL-separated in place */
static char*  s_steps[SCRIPT_MAX_STEPS];
static int    s_count = 0;
static int    s_index = 0;
static int    s_wait_frames = 0;
static double s_wait_until_ms = 0.0;
static char*  s_last_response = NULL;
static FILE*  s_log = NULL;
static int    s_log_tried = 0;

static void log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    if (!s_log_tried) {
        s_log_tried = 1;
        const char* p = getenv("PSX_SCRIPT_LOG");
        if (p && p[0]) s_log = fopen(p, "ab");
    }
    if (s_log) {
        va_start(ap, fmt);
        vfprintf(s_log, fmt, ap);
        va_end(ap);
        fputc('\n', s_log);
        fflush(s_log);
    }
}

static void split_steps(void) {
    s_count = 0;
    s_index = 0;
    if (!s_text) return;
    char* p = s_text;
    int in_str = 0, depth = 0;
    char* start = p;
    for (;; p++) {
        char c = *p;
        int end = 0;
        if (c == 0) end = 1;
        else if (c == '"' && (p == s_text || p[-1] != '\\')) in_str = !in_str;
        else if (!in_str && (c == '{' )) depth++;
        else if (!in_str && (c == '}') && depth > 0) depth--;
        else if (!in_str && depth == 0 && (c == ';' || c == '\n' || c == '\r')) end = 1;
        if (end) {
            *p = 0;
            /* trim */
            while (*start == ' ' || *start == '\t') start++;
            char* e = p;
            while (e > start && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            if (*start && *start != '#' && s_count < SCRIPT_MAX_STEPS)
                s_steps[s_count++] = start;
            if (c == 0) break;
            start = p + 1;
        }
    }
}

int psx_script_set(const char* text) {
    free(s_text);
    s_text = NULL;
    s_count = s_index = 0;
    s_wait_frames = 0;
    if (!text || !text[0]) return 0;
    s_text = strdup(text);
    if (!s_text) return 0;
    split_steps();
    if (s_count > 0)
        log_line("[script] armed: %d step(s)", s_count);
    return s_count > 0;
}

int psx_script_set_file(const char* path) {
    if (!path || !path[0]) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) { log_line("[script] cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (8 << 20)) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    int ok = psx_script_set(buf);
    free(buf);
    return ok;
}

int psx_script_init_from_env(void) {
    if (s_count > 0) return 1;              /* CLI already armed one */
    const char* t = getenv("PSX_SCRIPT");
    if (t && t[0]) return psx_script_set(t);
    const char* f = getenv("PSX_SCRIPT_FILE");
    if (f && f[0]) return psx_script_set_file(f);
    return 0;
}

int psx_script_active(void) { return s_count > 0 && s_index < s_count; }

static void finish(void) {
    if (s_index >= s_count && s_count > 0) {
        log_line("[script] done (%d step(s))", s_count);
        s_count = 0;                        /* inert from here on */
    }
}

void psx_script_poll(void) {
    if (!psx_script_active()) return;
    if (s_wait_frames > 0) { s_wait_frames--; return; }
    if (s_wait_until_ms > 0.0) {
        if ((double)psx_host_mono_ms() < s_wait_until_ms) return;
        s_wait_until_ms = 0.0;
    }
    const char* step = s_steps[s_index++];

    if (strncmp(step, "wait:", 5) == 0) {
        s_wait_frames = atoi(step + 5);
        if (s_wait_frames < 0) s_wait_frames = 0;
        log_line("[script] wait %d frame(s)", s_wait_frames);
    } else if (strncmp(step, "wait_ms:", 8) == 0) {
        s_wait_until_ms = (double)psx_host_mono_ms() + atof(step + 8);
        log_line("[script] wait %s ms", step + 8);
    } else if (strncmp(step, "echo:", 5) == 0) {
        log_line("[script] %s", step + 5);
    } else if (strcmp(step, "quit") == 0) {
        log_line("[script] quit");
        psx_frontend_request_quit(0);
    } else if (strncmp(step, "fail:", 5) == 0) {
        log_line("[script] FAIL: %s", step + 5);
        psx_frontend_request_quit(3);
    } else if (strncmp(step, "expect:", 7) == 0) {
        const char* needle = step + 7;
        if (!s_last_response || !strstr(s_last_response, needle)) {
            log_line("[script] EXPECT FAILED: %s (last response: %s)", needle,
                     s_last_response ? s_last_response : "(none)");
            psx_frontend_request_quit(3);
        } else {
            log_line("[script] expect ok: %s", needle);
        }
    } else {
        /* Anything else is a debug-server command line (JSON or bare name). */
        char* resp = debug_server_run_local(step);
        if (resp) {
            size_t n = strlen(resp);
            while (n && (resp[n - 1] == '\n' || resp[n - 1] == '\r')) resp[--n] = 0;
        }
        log_line("[script] %s -> %s", step, resp ? resp : "(no response)");
        free(s_last_response);
        s_last_response = resp;
    }
    finish();
}
