# Bug-report bundles (one-key telemetry)

Press **F9** in game (config.ini `[KeyMap] BugReport=` to rebind) or send
`{"cmd":"bug_report"}` to the debug server. The runtime writes

```
<save root>/bugreports/<YYYYMMDD-HHMMSS>_<trigger>/
  frame.png        native scanout of the frame on screen (widescreen-aware)
  frame_hires.png  presented surface (supersampled; == frame.png at 1x)
  screen.png       the window as shown (after the video filter, before OSD)
  state.pst        savestate at that instant — reload to reproduce
  report.json      host settings + every read-only debug query below
  README.txt       what the files are / how to reproduce
```

`report.json` = `bundle` (dir/time/trigger), `host` (platform, build type,
renderer, supersampling, AA, texture filter, screen model, video filter, aspect,
widescreen, fullscreen, window/drawable size, vsync, interpolation, HLE, FMV
skip, turbo loads) and `runtime` — the JSON answers of `ping` (frame counter,
dispatch misses), `gpu_state`, `video_filter`, `window_size`, `gl_interp`,
`gl_present_ring` (last 16), `overlay_loader_status`, `autocompile_status`,
`pad_status`, `turbo_state`, `display_ring_stats`, `gpu_ring_stats`,
`input_route_status`, plus the **thread / IRQ / CD trail** for "the picture
froze or went black but the game keeps running" reports: `sched_escape_ring`
(the deterministic scheduler's last 512 structured escapes — every
ChangeThread / RFE yield / resume-at, with `safety_net_resumes` = times a
thread's top-level dispatch returned pc==0 and control fell back to its
yielder, reason 100), `thread_trace` (last 2048 thread events),
`thread_ctx_ring`, `irqctx_ring` (last 256 IRQ contexts), `event_ring_tail`,
`cdrom_state`, `cdrom_command_history`, `irq_state`, `dma_state`,
`get_registers`, `phase_hot` (static hot PCs), `cycles_to_next_event`.
`tools/bugreport_threads.py <bundle>` prints that trail readably (escape
cadence per frame, non-routine thread events, hot PCs). It is built with
`debug_server_run_local()`, so a bundle
from a release build is directly comparable with a live debug session, and
adding telemetry = adding a debug command (`runtime/src/bug_report.c`).

Reproducing from a bundle: `{"cmd":"savestate","op":"load_path","path":".../state.pst"}`
on a debug build (or copy it over a slot file). `op:"save_path"` is the
matching slot-less save. `screen.png` and `state.pst` land within a frame of the
key press (present + safe-point staging); the OSD toast names the folder.
Nothing here changes emulation.
