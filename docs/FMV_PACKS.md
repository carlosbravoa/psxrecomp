# HD movie packs (FMV replacement)

Present a host frame sequence in place of an MDEC-decoded FMV. Same
discipline as texture packs and video filters (`SHADOW_ENHANCEMENTS.md`):
**present-time only**. The game still streams the STR from the disc, the
MDEC still decodes every frame into RAM, the game still uploads it to VRAM
as a 24-bit picture — savestates, netplay digests, VRAM readbacks and the
1× picture are untouched. Only the *present* of a 24-bit (depth24) frame is
substituted, and only while a movie the pack knows is playing. Audio stays
the STR's own XA stream, which is also what makes sync free: the replacement
frame index **is** the MDEC decode index within the movie. Off = the present
is exactly what it was.

## Pack layout

```
<pack>/
  ROCK8_0/            one directory per movie, named like the STR file on the
    00000.png         disc (ISO basename, no extension, upper case)
    00001.jpg         one image per MDEC-decoded frame, 0-based, png or jpg,
    ...               any size (presented pillarboxed 4:3 like the native FMV)
    movie.toml        optional: offset = N (pack index = decode index + N),
                      decodes_per_frame = N (strip decoders: N MDEC decodes = 1 picture)
  CAPCOM15/
    ...
```

A missing index shows the native frame; a movie without a directory plays
natively. Frames are decoded on a helper thread with one frame of prefetch;
a frame that is not ready when the present asks for it is skipped for that
present (`late` in the stats), never waited for.

## How the runtime knows what to show

* **Which movie:** the CD-ROM model knows the disc sector it last delivered;
  `cdrom_current_file()` maps it to the ISO path through a one-time walk of
  the directory tree (`iso_path_for_lba`, works for images and disc trees).
  A streaming FMV reads its STR continuously, so the basename of that file
  names the movie.
* **Which frame:** every colour (15/24-bit) MDEC decode of a picture
  (`fmv_pack_note_decode`, called from `execute_decode` in `mdec.c`) counts;
  a different file or a gap of more than 30 host frames starts a new movie at
  index 0. Mega Man 8 decodes one 300-macroblock picture per frame
  (320×240, 15 fps, 24-bit); a game that decodes in strips sets
  `decodes_per_frame` in `movie.toml`.
* **Where it is drawn:** the present path (`main.cpp`) asks
  `fmv_pack_current()` once per frame when the display is depth24; the
  software (SDL texture of the frame's own size, linear), OpenGL (present
  texture upload) and Vulkan CPU presents show it in place of the depth24
  rows, pinned 4:3; the headless `present_capture` renders the same
  decision. Video filters do not apply to it (it is not pixel art).

## Configuration

```toml
[video]
fmv_pack = "game-assets/movies/pack"   # relative to the project root; offered only when the dir exists
fmv_pack_enabled = true                # default of the launcher toggle
```

Launcher: Display → **HD movies** checkbox (when the directory exists;
persisted as `settings.toml [video] fmv_pack = true|false`, applied live from
the in-game launcher). `PSX_FMV_PACK=<dir>` overrides for one run. Debug
server: `{"cmd":"fmv_pack","op":"load","dir":..}` / `"unload"` / `"stats"`
→ `{loaded, dir, movie, frame, shown, missing, late, decoded}` plus the
current `disc_file`.

## Authoring

1. **Dump the native frames** — `PSX_FMV_DUMP=<dir>` at startup (or
   `{"cmd":"fmv_dump","op":"arm","dir":..}` *before* the movie starts —
   indices count from the first decode seen) writes every decoded picture as
   `<dir>/<MOVIE>/NNNNN.png`, column-major macroblock output unwrapped with
   the height inferred from the game's DMA-out column size. That is the 1:1
   skeleton with the exact numbering an HD pack must follow.
2. `tools/fmv_pack.py`:
   ```
   fmv_pack.py info    PACK                              movies / frame counts / sizes / gaps
   fmv_pack.py upscale DUMP PACK --scale N [--filter]    N x every dumped frame (identity skeleton)
   fmv_pack.py from-video VIDEO PACK/MOVIE --frames N [--size WxH] [--jpg]
                                                         resample any video to exactly N frames (ffmpeg)
   fmv_pack.py check   PACK DUMP                         every dumped frame covered?
   ```
   Typical: dump once → `from-video your_upscaled_ROCK8_0.mp4 pack/ROCK8_0
   --frames <count from info>` (the video may have any fps/length; the
   frames are distributed evenly, so an upscale of the original keeps sync
   exactly) → `check`.
3. Point `[video] fmv_pack` at the directory and tick **HD movies**.

Sizes: 12,500 frames of Mega Man 8's six movies at 640×480 are ~1 GB as
JPEG, ~4 GB as PNG; at 1280×960 four times that. JPEG decodes faster too.

## Verified (Mega Man 8)

Synthetic pack (frame index encoded in the picture): headless software,
windowed software and windowed OpenGL all present the pack frame whose index
matches the MDEC decode count of the CAPCOM logo movie
(`fmv_pack stats`: `movie CAPCOM15`, `late 0`), the native VRAM
(`screenshot_file`) still holds the MDEC picture; `fmv_dump` frames of the
logo and intro movies unwrap correctly (320×240).

## Files

`runtime/include/fmv_pack.h`, `runtime/src/fmv_pack.c` (pack, worker thread,
dump), `runtime/src/mdec.c` (decode hook, DMA-out column size),
`runtime/src/cdrom.c` (`cdrom_current_file`), `runtime/src/iso_reader_c.cpp`
(`iso_path_for_lba`), `runtime/src/main.cpp` (config / env / launcher glue,
present substitution, headless capture), `runtime/src/debug_server.c`
(`fmv_pack`, `fmv_dump`), `recompiler/src/config_loader.{h,cpp}`
(`[video] fmv_pack`, settings), `tools/fmv_pack.py`, recomp-ui "HD movies"
row (`RECOMP_LAUNCHER_HAS_FMV_PACK`).

Not yet: a video-file container (frames only — no ffmpeg in the runtime),
per-movie fps different from the STR's (the game's timing rules), the
Vulkan present is unverified (no context here).
