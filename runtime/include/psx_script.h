/* psx_script.h — built-in session script (headless / CI / bug-report tooling).
 *
 * A script is a list of steps separated by ';' or newlines, given with
 * `--script "<steps>"`, `--script-file <path>`, or the PSX_SCRIPT /
 * PSX_SCRIPT_FILE environment variables. One step is executed per simulated
 * vblank, on the emulation thread, at the same safe point the debug server
 * polls — so a step can be ANY debug-server command line, plus a few verbs:
 *
 *   {"cmd":"savestate","op":"load","slot":3}     any debug-server command
 *   {"cmd":"press","buttons":8,"frames":4}       (JSON; ids are optional)
 *   {"cmd":"screenshot","path":"a.png"}
 *   {"cmd":"bug_report","trigger":"script"}
 *   wait:N            idle N vblanks (let the game advance)
 *   wait_ms:N         idle N wall-clock milliseconds (headless is unpaced,
 *                     so prefer wait:N frames for determinism)
 *   echo:text         print a line
 *   quit              clean shutdown, exit code 0
 *   fail:text         print text, exit code 3 (for asserting scripts)
 *   expect:substr     exit code 3 unless the previous command's response
 *                     contains substr (e.g. expect:"ok":true)
 *
 * Every response is written to stdout as `[script] <step> -> <response>`
 * (and to PSX_SCRIPT_LOG when set), so a test can grep the transcript.
 * With --headless the script drives an unpaced, windowless run: the whole
 * runtime, savestates, telemetry (bug_report), the software present with the
 * CPU video filters and the debug-server vocabulary are all available; only
 * the OpenGL/Vulkan presenters are not (no window). Works in release builds
 * too — nothing here needs PSX_DEBUG_TOOLS. */

#ifndef PSXRECOMP_PSX_SCRIPT_H
#define PSXRECOMP_PSX_SCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install a script (steps text) or a script file. Returns 1 if a non-empty
 * script is now armed. Later calls replace earlier ones. */
int  psx_script_set(const char* text);
int  psx_script_set_file(const char* path);
/* Convenience: --script / --script-file already parsed by main; env fallback. */
int  psx_script_init_from_env(void);
/* 1 if a script is armed and not yet finished. */
int  psx_script_active(void);
/* Run at most one step. Call once per simulated vblank at a safe point. */
void psx_script_poll(void);

/* Frontend hooks (main.cpp). */
void psx_frontend_request_quit(int exit_code);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_SCRIPT_H */
