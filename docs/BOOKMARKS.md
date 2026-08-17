# "Start at" bookmarks (testing aid)

A bookmark is a savestate the launcher can resume right after boot: a stage
select that works for every stage of every game without touching game code —
you take the savestate where you want to start, name it, and it appears in
the launcher.

**This is a development / testing artifact, not a player feature.** A
savestate is only valid for the exact disc image + BIOS combination it was
taken with (the loader checks the BIOS checksum and entry point and refuses
otherwise), so bookmarks made on one machine do not travel to a different
dump or BIOS. The row only appears when a `bookmarks/` folder with `.pst`
files exists, so a normal install never sees it.

```
<memcard_dir>/bookmarks/<label>.pst        e.g. saves/bookmarks/01 Tengu Man.pst
```

* The runtime scans that folder at startup (file stems, sorted — prefix labels
  with numbers to order them) and offers them to the launcher as
  `GameInfo.bookmark_labels` / `num_bookmarks`.
* Launcher: SYSTEM card → **Start at** — a cycle button, *Normal boot* →
  first bookmark → … Not persisted: every launch defaults to a normal boot,
  and choosing one is a one-off. From the **in-game** launcher (ESC) the
  chosen bookmark loads immediately on Apply.
* CLI / scripts: `--start-state <file.pst>` or `PSX_START_STATE=<file.pst>`
  stage the load after boot (headless too); the debug server's
  `{"cmd":"savestate","op":"load_path","path":..}` does the same at any time.
* Loading uses the normal savestate blob path (`savestate_request_load_blob_protocol`),
  so the usual rules apply: same game / BIOS variant as when the state was
  taken, refused during netplay.

Bookmarks hold the game's memory image — like memory cards and savestates
they are local files, shared privately, never committed. A game repo can ship
a helper to make them from slots (Mega Man 8: `tools/bookmark.sh add <slot>
"<label>"`, `list`, `rm`, `run` for a headless smoke test).

Files: `runtime/src/main.cpp` (`scan_bookmarks`, `stage_start_state`,
`--start-state`, `PSX_START_STATE`, launcher glue), recomp-ui
`recomp_launcher.h` / `launcher_model.{h,c}` / `launcher_imgui.cpp`
(`RECOMP_LAUNCHER_HAS_BOOKMARKS`, the "Start at" row).
