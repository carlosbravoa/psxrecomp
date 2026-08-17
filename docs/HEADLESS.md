# Headless runs and the built-in session script

`--headless` (or `PSX_HEADLESS=1`) runs the runtime with no window, no audio,
software renderer, unpaced (as fast as the host goes), launcher skipped. The
debug server (debug builds) still listens; savestates, the software present,
CPU video filters and telemetry (bug_report) all work; only the OpenGL/Vulkan
presenters do not exist (no window).

## Session script (`runtime/src/psx_script.c`)

`--script "<steps>"`, `--script-file <path>`, or `PSX_SCRIPT` /
`PSX_SCRIPT_FILE`. Steps are separated by `;` or newlines; `#` lines are
comments. One step runs per simulated vblank on the emulation thread — the
same safe point the debug server polls — so **any debug-server command line is
a step** (JSON, `id` optional), plus:

| step | effect |
|---|---|
| `wait:N` | idle N vblanks |
| `wait_ms:N` | idle N wall-clock ms (prefer `wait:` — headless is unpaced) |
| `echo:text` | print |
| `expect:substr` | exit 3 unless the previous response contains substr |
| `fail:text` | print, exit 3 |
| `quit` | clean shutdown, exit 0 |

Each step and its response are printed as `[script] <step> -> <response>` on
stdout and appended to `PSX_SCRIPT_LOG` when set. Works in release builds:
the script uses `debug_server_run_local()`, and the input override
(`press`) is honoured in production too. A script implies `--no-launcher`.

Example (Mega Man 8, `tools/mm8_headless.sh` wraps this):

```
--headless --script 'wait:30;{"cmd":"savestate","op":"load","slot":3};wait:60;
  {"cmd":"video_filter","name":"xbr2x"};
  {"cmd":"present_capture","path":"out/present.png","companions":0};wait:2;
  {"cmd":"present_capture"};expect:written;
  {"cmd":"bug_report","trigger":"headless"};wait:30;quit'
```

Headless `present_capture` / bug-report `screen.png` resolve the scanout the
software present would show and run the CPU video filter on it (the CPU
reference), so filtered output is testable without a GPU. GL shader parity
still needs a windowed run (`tools/video_filter_check.py`).

Typical uses: reproduce a bug bundle's `state.pst` (`savestate op=load_path`)
and dump telemetry; regression-check a savestate → screenshot; drive input with
`press` and assert RAM with `read_ram` + `expect:`.
