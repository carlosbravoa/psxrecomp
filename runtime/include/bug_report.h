/* bug_report.h — one-key diagnostics bundle ("telemetry screenshot").
 *
 * When the player hits the Bug-report hotkey (config.ini [KeyMap] BugReport,
 * default F9) or the debug server receives {"cmd":"bug_report"}, the runtime
 * writes a self-contained folder the player can attach to a report later:
 *
 *   <save root>/bugreports/<YYYYMMDD-HHMMSS>_<trigger>/
 *     frame.png        native scanout of the frame on screen (what the game
 *                      drew; widescreen-aware like the debug "screenshot")
 *     frame_hires.png  the supersampled / presented surface (same as
 *                      "screenshot_hires"; identical to frame.png at 1x)
 *     screen.png       the presented window (post video-filter, pre-OSD) —
 *                      written on the NEXT present (GL: drawable readback,
 *                      software: the buffer handed to SDL)
 *     state.pst        a full savestate taken at the next safe point, so the
 *                      exact moment can be reloaded (copy over a slot file, or
 *                      debug {"cmd":"savestate","op":"load_path",...})
 *     report.json      host + runtime telemetry: settings, window, video
 *                      filter, frame counter, dispatch misses, GPU state,
 *                      overlay/autocompile status, present ring, and every
 *                      other debug-server query listed in bug_report.c
 *     README.txt       what the files are and how to reproduce
 *
 * Presentation/diagnostics only: nothing here changes emulation. Everything
 * except screen.png / state.pst is written synchronously on the emu thread;
 * those two land within a frame and are noted in the OSD toast when done. */

#ifndef PSXRECOMP_BUG_REPORT_H
#define PSXRECOMP_BUG_REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start a bundle. `trigger` is a short token for the folder name ("hotkey",
 * "debug", "crash", ...). Returns 1 and fills the last-dir on success, 0 if
 * the folder could not be created. Safe to call from the SDL key handler or
 * a debug-server handler (emu thread). */
int bug_report_capture(const char* trigger);

/* Folder of the last bundle ("" if none this session). */
const char* bug_report_last_dir(void);

/* Frontend hooks: called when the async pieces of the last bundle complete. */
void bug_report_on_state_saved(int ok);

/* Host-supplied JSON fragment (an object body WITHOUT braces, e.g.
 * "\"renderer\":\"opengl\",\"filter\":\"xbr2x\"") appended under "host" —
 * main.cpp implements it with the video/window/settings globals it owns. */
int psx_host_report_json(char* buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_BUG_REPORT_H */
