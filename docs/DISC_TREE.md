# Disc trees — running from extracted files

A **disc tree** is a bin/cue dump unpacked into a plain directory that the
runtime mounts *as the disc*. Nothing in the CD-ROM controller, the BIOS or
the game changes: `PS1::ISOReader::Open()` accepts the directory and
re-synthesises the raw 2352-byte sectors on demand from the extracted files —
ISO9660 volume descriptor, path tables and directory records rebuilt from a
manifest, Mode 2 Form 1 sectors re-armoured with subheader + EDC + ECC, raw
Form 2 / XA / STR sectors and CD-DA tracks served verbatim.

Two properties make it more than a convenience:

* **Byte-identical while pristine.** An untouched tree reproduces the dump
  exactly — every sector of every track, including licence area, ECC bytes
  and pregaps. `psx-disc-tree verify <tree> <original.cue>` compares all of
  them (Mega Man 8: 177,992 sectors, 0.8 s, `IDENTICAL`). Netplay's TOC
  fingerprint, the disc-identity badge and the CD-ROM timing are therefore
  unchanged.
* **Editable.** Replace, edit or add files under `cdrom/`, swap a CD-DA track's
  WAV, and the next launch serves the new bytes in place. A file that still
  fits its original extent keeps its LBA; one that grew is relocated after the
  original data area (the ISO metadata follows). Titles that address files by
  hardcoded LBA rather than by name get their table rewritten in the served
  boot EXE (see *LBA tables* below).

The engine lives in `runtime/src/disc_tree.cpp` (`runtime/include/disc_tree.h`);
`tools/disc_tree.py` extracts, `psx-disc-tree` (built next to every runtime
from `runtime/tools/disc_tree_cli.cpp`) verifies/builds/inspects.

## Layout

```
<tree>/
  disc.toml                manifest (below)
  cdrom/                   the ISO9660 tree, one host file per disc file
    SLUS_004.53            Mode 2 Form 1 files are stored "cooked" (2048 B/sector,
    SYSTEM.CNF             i.e. exactly the file's bytes)
    MOVIE/ROCK8_0.STR      Form 2 / XA / interleaved files are stored raw:
                           2336 B/sector = subheader + data + EDC/ECC (what
                           jPSXdec / mkpsxiso / MC32 use). 2352-byte STRs
                           (sync + header included) are accepted too.
  audio/track02.wav        CD-DA tracks: 44.1 kHz s16le stereo WAV
  meta/system_area.bin     LBA 0..15 raw (licence text + Sony logo data)
  meta/pvd.bin             the primary volume descriptor (2048 B); further
                           descriptors as meta/descriptor_<lba>.bin
  meta/raw_<lba>_<n>.bin   any run of sectors that is neither a file nor an
                           empty sector (mastering artefacts) — kept raw so
                           the tree still round-trips
```

Extract:

```sh
python3 psxrecomp/tools/disc_tree.py extract "<dump>.cue" <tree>     # ~2 s for a 400 MB disc
python3 psxrecomp/tools/disc_tree.py list <tree>                     # layout
python3 psxrecomp/tools/disc_tree.py status <tree>                   # modified/added/missing vs pristine
psx-disc-tree verify <tree> "<dump>.cue" [--game-toml game.toml]     # byte-identity proof
psx-disc-tree layout <tree> [--game-toml game.toml]                  # what the runtime will serve
psx-disc-tree build  <tree> out.cue [--split] [--game-toml ...]      # bin/cue for an emulator
psx-disc-tree md5    <tree>                                          # md5 of the synthesized data track
```

`extract` refuses non-`MODE2/2352` data tracks (a cooked ISO has already lost
the Form 2 sectors) and non-BINARY cue payloads. It ends with a data-level
self-check (every directory sector, path table and file payload regenerated
and compared); the full raw comparison including ECC is `psx-disc-tree verify`.

## Manifest (`disc.toml`)

Everything the extractor learned that a file tree cannot express:

| key | meaning |
|---|---|
| `[source]` | cue name, volume id, serial, data-track sector count and digests |
| `[layout]` | `system_area`, `descriptors` (PVD first), `descriptor_lba`, `terminator_lba`, `path_table_l` / `path_table_m` (LBAs of the L/M tables and their copies), `postgap_sectors` |
| `[[raw]]` | raw sector runs (`lba`, `sectors`, `file`) |
| `[[track]]` | `number`, `type = "data" \| "audio"`, `sectors` (pregap + payload), audio: `file`, `pregap_sectors`, optional `pregap_raw`, `md5` |
| `[[dir]]` | `path` ("" = root), pristine `lba` + `sectors`, `date` (7-byte ISO record date, hex), `xa` (system-use area, hex), `entries` (record order) |
| `[[file]]` | `path`, pristine `lba` + `size`, `form = "1"` (cooked) / `"raw"` (2336) / `"cdda"` (+ `track`: the record aliases an audio track), `date`, `xa`, `md5`, optional `version` |

You normally never edit it. Files you add get their form from the extension
(`.STR/.XA/.IKI/.MOV` whose size is a multiple of 2336 or 2352-with-sync are
raw; everything else Form 1), and their date/XA attributes from a sibling of
the same kind. To force a form or attribute for an added file, give it a
`[[file]]` entry with `lba = 0`, `size = 0` and the `form` / `xa` / `date` you
want: sizes always come from the host file, and a zero-sector pristine extent
simply sends it to the append region.

## Layout rules (what the runtime serves)

1. Every object with a pristine LBA whose new size still fits its original
   sector allocation stays where it was. Unchanged trees therefore reproduce
   the dump exactly; a shrunken file leaves empty sectors behind.
2. Everything else — grown files, new files and directories, a directory
   whose records no longer fit, path tables that outgrew their sector — is
   appended after the last pristine sector of the data track, in manifest
   order then by path. The data track then ends with the manifest's
   `postgap_sectors` of empty sectors.
3. Audio tracks follow the data track; each keeps its pregap. A track's length
   follows its WAV (`pregap + ceil(pcm / 2352)`), so a replaced song may be
   shorter or longer than the original. WAVs that are not 44.1 kHz/16-bit/
   stereo are converted at mount (mono → stereo, 8 → 16 bit, linear resample);
   a headerless file is taken as raw s16le stereo PCM.
4. Records that alias an audio track (`form = "cdda"`, e.g. Mega Man 8's
   `END1.DA` / `ZNULL.DAT`) point at the track's INDEX 01 with size =
   payload sectors × 2048.
5. Empty (unallocated) sectors are Mode 2 with a zero subheader, zero payload
   and zero EDC/ECC — what every mastering tool writes.
6. Missing files are dropped from their directory (a warning is logged; the
   game reads empty sectors at the old LBA).

`psx-disc-tree layout` prints the resulting extent map and every note the
runtime will log (`psxrecomp:   disc tree: ...`).

## LBA tables (titles that do not use filenames)

Many Capcom-era games never call `CdSearchFile`: the EXE carries a table of
`{LBA, size, ...}` per file and reads by sector number. Relocating a file
would silently break such a title, so `game.toml` can describe the table(s):

```toml
[disc_tree]
dir = "game-assets/disc"          # the tree; mounted when it exists
[[disc_tree.lba_table]]
address     = "0x80136F7C"        # RAM address of entry 0
count       = 139
stride      = 12                  # bytes per entry
lba_offset  = 0                   # u32 LE sector number (or BCD MSF with lba_is_msf = true)
size_offset = 4                   # u32 LE size; -1 = none
```

At mount the engine reads the table from the tree's boot EXE (name from
`[game] exe`, else `SYSTEM.CNF`), matches each entry's LBA against the
pristine LBAs of the manifest, and rewrites the entries of files that moved
or changed size — in the served EXE sectors, so the BIOS loads the patched
program from "disc" whether HLE or LLE boots. The size unit is inferred from
the pristine value (bytes, sectors × 2336 for STR DMA lengths, sectors × 2048,
sectors × 2352, sector count) and rewritten in kind; an unrecognised unit is
left alone and noted. The rewritten bytes are reported to `main.cpp`, which
blesses them into the dirty-RAM text-image guard so the patched table is not
mistaken for self-modifying code.

Verified on Mega Man 8: `OVL/STAGE00.BIN` grown by 4 KB → relocated to LBA
142096, entry 9 rewritten (LBA + size), title → GAME START → intro stage
loads and runs from the new location, 0 dispatch misses.

## Selection and precedence

* `[disc_tree] dir` (relative to the project root) — used when it exists and
  holds `disc.toml`, in place of `[game] disc` / the launcher's disc path.
* `PSX_DISC_TREE=<dir>` overrides the directory; `PSX_DISC_TREE=0` disables
  the tree for one run.
* `--disc <path>` always wins; it may itself name a tree directory.
* The launcher's disc setting, `disc.cfg` and the first-run picker are never
  written with the tree path.

`identify_disc()` recognises trees (serial, region, volume id, TOC
fingerprint via the mounted reader), `resolve_disc_path()` treats one as
"from cue" for the netplay gate. Mod packages that patch disc bytes are keyed
on the stock image's SHA-256, so they stay inert on a tree — edit the files
instead.

## Performance

Mount = manifest parse + host scan + layout ≈ 15–25 ms for a 140-file disc;
no data is read until requested. A Form 1 sector costs one 2048-byte read plus
EDC/ECC (~10 µs); at 2× speed the drive asks for 150 sectors/s. Raw and audio
sectors are plain reads. Host files are opened lazily and kept open.

## Files

| | |
|---|---|
| `runtime/include/disc_tree.h`, `runtime/src/disc_tree.cpp` | engine: manifest, host scan, layout, ISO9660 synthesis, EDC/ECC, EXE table patch, sector service |
| `runtime/src/iso_reader.cpp` | `ISOReader::Open()` tree branch, `SetDiscTreeHints()`, `LastDiscTreeMount()` |
| `runtime/src/disc_identity.cpp`, `runtime/src/disc_path.cpp` | identity + resolution of tree paths |
| `runtime/src/main.cpp` | `[disc_tree]` selection, hints, layout log, guard blessing |
| `recompiler/src/config_loader.{h,cpp}` | `[disc_tree]` parsing (`GameConfig::disc_tree_dir`, `disc_tree_lba_tables`) |
| `tools/disc_tree.py` | extract / list / status / check |
| `runtime/tools/disc_tree_cli.cpp` → `psx-disc-tree` | layout / verify / build / md5 |
| `runtime/tests/test_disc_tree.cpp` | EDC/ECC vectors, synthetic tree round-trip, relocation + table patch |
