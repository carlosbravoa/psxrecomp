#!/usr/bin/env python3
"""disc_tree.py — extract a PS1 bin/cue into a *disc tree* the runtime can mount.

A disc tree is a plain directory:

    <tree>/disc.toml            manifest: layout + per-entry metadata (see docs/DISC_TREE.md)
    <tree>/cdrom/...            the ISO9660 file tree, one host file per disc file
                                (Mode 2 Form 1 files cooked = 2048 B/sector,
                                 Form 2 / XA / STR files raw = 2336 B/sector)
    <tree>/audio/trackNN.wav    Red Book CD-DA tracks (44.1 kHz s16le stereo)
    <tree>/meta/...             raw sectors that are not files (system/licence
                                area, primary volume descriptor, any unexplained
                                sector runs)

The runtime (`PS1::ISOReader::Open(<tree>)`, see runtime/src/disc_tree.cpp)
re-synthesises the disc from the tree on the fly — same LBAs, same subheaders,
same EDC/ECC — so an unmodified tree is *byte-identical* to the dump it came
from, and a modified/added file is served in place (relocated when it grew).
`psx-disc-tree build|verify` (runtime/tools/disc_tree_main.cpp) is the same
engine as a CLI: rebuild a bin/cue from a tree, or prove a tree round-trips.

Sub-commands:

    extract <cue> <tree>  [--force] [--no-audio] [--skip-hash]
    list    <tree>        show the manifest layout
    status  <tree>        which files differ from the pristine dump (md5)
    check   <tree> [cue]  data-level regeneration self-check (no ECC): rebuild
                          every metadata sector's 2048-byte payload + every
                          file payload and compare with the source image

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

RAW = 2352
USER = 2048
FORM2_USER = 2336          # subheader(8) + data(2324) + EDC(4)
SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])
FORMAT_ID = "psxrecomp-disc-tree"
FORMAT_VERSION = 1


# ────────────────────────────────────────────────────────────────────────────
# small helpers
# ────────────────────────────────────────────────────────────────────────────

def bcd(v: int) -> int:
    return ((v // 10) << 4) | (v % 10)


def lba_to_msf_bcd(lba: int) -> bytes:
    a = lba + 150
    return bytes([bcd(a // 4500), bcd((a // 75) % 60), bcd(a % 75)])


def msf_to_frames(msf: str) -> int:
    m, s, f = (int(x) for x in msf.split(":"))
    return m * 4500 + s * 75 + f


def both_endian32(v: int) -> bytes:
    return struct.pack("<I", v) + struct.pack(">I", v)


def both_endian16(v: int) -> bytes:
    return struct.pack("<H", v) + struct.pack(">H", v)


def md5_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def toml_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# ────────────────────────────────────────────────────────────────────────────
# cue sheet
# ────────────────────────────────────────────────────────────────────────────

@dataclass
class CueTrack:
    number: int
    mode: str                     # "MODE2/2352", "AUDIO", ...
    file: Path
    file_index: int
    index00: int | None = None    # frames, file-relative
    index01: int = 0


@dataclass
class CueSheet:
    files: list[Path]
    tracks: list[CueTrack]


def parse_cue(cue_path: Path) -> CueSheet:
    files: list[Path] = []
    tracks: list[CueTrack] = []
    cur_file = -1
    text = cue_path.read_text(encoding="utf-8", errors="replace")
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.match(r'FILE\s+"(.*)"\s+(\S+)', line, re.I) or re.match(r"FILE\s+(\S+)\s+(\S+)", line, re.I)
        if m:
            if m.group(2).upper() != "BINARY":
                raise SystemExit(f"cue FILE type {m.group(2)} is not BINARY (WAVE/MP3 cues are not supported)")
            files.append(cue_path.parent / m.group(1))
            cur_file = len(files) - 1
            continue
        m = re.match(r"TRACK\s+(\d+)\s+(\S+)", line, re.I)
        if m:
            tracks.append(CueTrack(int(m.group(1)), m.group(2).upper(), files[cur_file], cur_file))
            continue
        m = re.match(r"INDEX\s+(\d+)\s+(\d+:\d+:\d+)", line, re.I)
        if m and tracks:
            n = int(m.group(1))
            fr = msf_to_frames(m.group(2))
            if n == 0:
                tracks[-1].index00 = fr
            elif n == 1:
                tracks[-1].index01 = fr
    if not files or not tracks:
        raise SystemExit(f"{cue_path}: no FILE/TRACK lines")
    return CueSheet(files, tracks)


# ────────────────────────────────────────────────────────────────────────────
# ISO9660 walk
# ────────────────────────────────────────────────────────────────────────────

@dataclass
class Rec:
    """One directory record as found on disc."""
    name: str            # identifier as stored (files keep ';1')
    lba: int
    size: int
    flags: int
    date: bytes          # 7 bytes
    sua: bytes           # system-use area (XA: 14 bytes), may be empty
    length: int
    ext_attr_len: int
    seq: int
    interleave_unit: int
    interleave_gap: int
    parent_path: str     # ISO path of the containing directory ("" = root)

    @property
    def is_dir(self) -> bool:
        return bool(self.flags & 2)

    @property
    def base_name(self) -> str:
        return self.name.split(";")[0]

    @property
    def version(self) -> int:
        return int(self.name.split(";")[1]) if ";" in self.name else 1

    @property
    def path(self) -> str:
        return (self.parent_path + "/" + self.base_name) if self.parent_path else self.base_name

    @property
    def xa_attr(self) -> int | None:
        if len(self.sua) >= 14 and self.sua[6:8] == b"XA":
            return struct.unpack(">H", self.sua[4:6])[0]
        return None


class RawImage:
    def __init__(self, path: Path):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()
        if self.size % RAW:
            raise SystemExit(f"{path}: size {self.size} is not a multiple of {RAW} (need a raw MODE2/2352 dump)")
        self.sectors = self.size // RAW

    def raw(self, lba: int, count: int = 1) -> bytes:
        self.f.seek(lba * RAW)
        return self.f.read(RAW * count)

    def user(self, lba: int) -> bytes:
        return self.raw(lba)[24:24 + USER]


def parse_records(data: bytes, parent_path: str) -> list[Rec]:
    """All records of one directory extent (multi-sector; records never straddle sectors)."""
    out: list[Rec] = []
    for so in range(0, len(data), USER):
        off = so
        end = so + USER
        while off < end:
            ln = data[off]
            if ln == 0:
                break
            r = data[off:off + ln]
            il = r[32]
            ident = r[33:33 + il]
            name = ident.decode("latin1")
            if ident == b"\x00":
                name = "\x00"
            elif ident == b"\x01":
                name = "\x01"
            sua_off = 33 + il + (1 if il % 2 == 0 else 0)
            out.append(Rec(
                name=name,
                lba=struct.unpack("<I", r[2:6])[0],
                size=struct.unpack("<I", r[10:14])[0],
                flags=r[25],
                date=bytes(r[18:25]),
                sua=bytes(r[sua_off:ln]),
                length=ln,
                ext_attr_len=r[1],
                seq=struct.unpack("<H", r[28:30])[0],
                interleave_unit=r[26],
                interleave_gap=r[27],
                parent_path=parent_path,
            ))
            off += ln
    return out


@dataclass
class DirInfo:
    path: str            # "" root
    lba: int
    size: int
    self_rec: Rec        # the '.' record (dates / SUA of the directory itself)
    records: list[Rec]   # children in on-disc order (without . and ..)
    parent_lba: int


def walk_iso(img: RawImage):
    pvd = img.user(16)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise SystemExit("no primary volume descriptor at LBA 16")
    root_rec = parse_records(pvd[156:190] + b"\x00" * (USER - 34), "")[0]
    dirs: list[DirInfo] = []
    files: list[Rec] = []

    def visit(path: str, lba: int, size: int, parent_lba: int):
        nsec = (size + USER - 1) // USER
        data = b"".join(img.user(lba + i) for i in range(nsec))
        recs = parse_records(data, path)
        self_rec = recs[0]
        children = [r for r in recs if r.name not in ("\x00", "\x01")]
        d = DirInfo(path, lba, size, self_rec, children, parent_lba)
        dirs.append(d)
        for r in children:
            if r.is_dir:
                visit(r.path, r.lba, r.size, lba)
            else:
                files.append(r)

    visit("", root_rec.lba, root_rec.size, root_rec.lba)
    return pvd, root_rec, dirs, files


# ────────────────────────────────────────────────────────────────────────────
# metadata regeneration (data payloads only) — the Python twin of the runtime
# builder, used for the extraction-time self-check.
# ────────────────────────────────────────────────────────────────────────────

def iso_sort_key(name: str) -> tuple:
    """ECMA-119 9.3 ordering: name, then extension, then version — each padded with spaces."""
    base, _, ver = name.partition(";")
    stem, dot, ext = base.partition(".")
    return (stem.ljust(30, " "), ext.ljust(30, " "), int(ver) if ver else 0)


def build_record(name: bytes, lba: int, size: int, flags: int, date: bytes, sua: bytes) -> bytes:
    il = len(name)
    body = bytearray()
    body += b"\x00"                     # ext attr len
    body += both_endian32(lba)
    body += both_endian32(size)
    body += date
    body += bytes([flags, 0, 0])        # flags, unit size, gap
    body += both_endian16(1)            # volume sequence number
    body += bytes([il]) + name
    if il % 2 == 0:
        body += b"\x00"
    body += sua
    rec = bytes([len(body) + 1]) + bytes(body)
    return rec


def build_directory(records: list[bytes]) -> bytes:
    """Pack records into 2048-byte sectors, never straddling a sector."""
    out = bytearray()
    cur = bytearray()
    for r in records:
        if len(cur) + len(r) > USER:
            out += cur + bytes(USER - len(cur))
            cur = bytearray()
        cur += r
    out += cur + bytes(USER - len(cur))
    return bytes(out)


# ────────────────────────────────────────────────────────────────────────────
# extraction
# ────────────────────────────────────────────────────────────────────────────

def classify_file(img: RawImage, lba: int, nsec: int) -> tuple[str, str]:
    """Return (form, note). form '1' = every sector is Mode2 Form1 with the canonical
    subheader pattern (file/chan/coding 0, submode 0x08 / 0x89 on the last sector),
    'raw' = anything else (Form 2 / interleaved XA / STR / non-canonical)."""
    if nsec == 0:
        return "1", ""
    for i in range(nsec):
        s = img.raw(lba + i)
        if s[:12] != SYNC or s[15] != 2:
            return "raw", f"sector {lba + i} is not a Mode 2 sector"
        sub = s[16:24]
        if sub[:4] != sub[4:8]:
            return "raw", f"sector {lba + i} subheader copies differ"
        want = 0x89 if i == nsec - 1 else 0x08
        if sub[0] != 0 or sub[1] != 0 or sub[2] != want or sub[3] != 0:
            return "raw", f"sector {lba + i} subheader {sub[:4].hex()} is not canonical Form 1 ({want:02x})"
    return "1", ""


def is_empty_sector(s: bytes) -> bool:
    return s[:12] == SYNC and s[15] == 2 and s[16:24] == bytes(8) and s[24:RAW] == bytes(RAW - 24)


def write_wav(path: Path, pcm: bytes):
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16)
    hdr += b"data" + struct.pack("<I", len(pcm))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(pcm)


def cmd_extract(args) -> int:
    cue_path = Path(args.cue).resolve()
    tree = Path(args.tree)
    if tree.exists() and any(tree.iterdir()) and not args.force:
        print(f"error: {tree} exists and is not empty (use --force)", file=sys.stderr)
        return 2
    sheet = parse_cue(cue_path)
    data_tracks = [t for t in sheet.tracks if t.mode.startswith("MODE")]
    if not data_tracks:
        print("error: cue has no data track", file=sys.stderr)
        return 2
    t1 = data_tracks[0]
    if t1.mode != "MODE2/2352":
        print(f"error: data track is {t1.mode}; only MODE2/2352 raw dumps can be extracted losslessly", file=sys.stderr)
        return 2
    if t1.file_index != 0 or t1.number != 1:
        print("error: expected the data track to be TRACK 01 in the first FILE", file=sys.stderr)
        return 2
    img = RawImage(t1.file)
    print(f"[disc_tree] data track: {t1.file.name}  ({img.sectors} sectors)")

    pvd, root_rec, dirs, files = walk_iso(img)
    print(f"[disc_tree] ISO9660: {len(dirs)} directories, {len(files)} files")

    # Track table (disc-relative LBAs)
    tracks_out = []
    disc_lba = 0
    file_sectors = {}
    for i, fpath in enumerate(sheet.files):
        sz = fpath.stat().st_size
        if sz % RAW:
            print(f"error: {fpath.name}: size {sz} not a multiple of {RAW}", file=sys.stderr)
            return 2
        file_sectors[i] = sz // RAW
    file_start = {}
    for i in range(len(sheet.files)):
        file_start[i] = disc_lba
        disc_lba += file_sectors[i]
    total_sectors = disc_lba
    for t in sheet.tracks:
        start = file_start[t.file_index] + t.index01
        pre = file_start[t.file_index] + (t.index00 if t.index00 is not None else t.index01)
        # length: up to next track's pregap start (or end of file when files are per-track)
        tracks_out.append({"n": t.number, "audio": t.mode == "AUDIO", "start": start, "pregap": pre,
                           "file_index": t.file_index, "index00": t.index00, "index01": t.index01})
    for k, t in enumerate(tracks_out):
        nxt = tracks_out[k + 1]["pregap"] if k + 1 < len(tracks_out) else total_sectors
        t["end"] = nxt   # exclusive, disc-relative
    # sanity: multi-file cues must have one track per file (redump style)
    if len(sheet.files) > 1:
        if len(sheet.files) != len(sheet.tracks) or any(t.file_index != t.number - 1 for t in sheet.tracks):
            print("error: multi-FILE cue must have exactly one TRACK per FILE (redump layout)", file=sys.stderr)
            return 2

    # ── directories to host ────────────────────────────────────────────────
    (tree / "cdrom").mkdir(parents=True, exist_ok=True)
    (tree / "meta").mkdir(exist_ok=True)
    (tree / "audio").mkdir(exist_ok=True)

    covered = bytearray(img.sectors)      # 1 = explained by an object

    def cover(lba: int, n: int, what: str):
        for i in range(lba, lba + n):
            if 0 <= i < img.sectors:
                covered[i] = 1

    # system area (LBA 0..15) raw
    with open(tree / "meta" / "system_area.bin", "wb") as f:
        f.write(img.raw(0, 16))
    cover(0, 16, "system_area")

    # volume descriptor set: LBA 16.. until terminator (type 255)
    descriptors = []
    lba = 16
    while True:
        u = img.user(lba)
        if u[1:6] != b"CD001":
            print(f"error: descriptor set broken at LBA {lba}", file=sys.stderr)
            return 2
        if u[0] == 255:
            terminator_lba = lba
            break
        name = "pvd.bin" if u[0] == 1 else f"descriptor_{lba}.bin"
        with open(tree / "meta" / name, "wb") as f:
            f.write(u)
        descriptors.append((lba, name))
        lba += 1
        if lba > 64:
            print("error: runaway volume descriptor set", file=sys.stderr)
            return 2
    cover(16, terminator_lba - 16 + 1, "descriptors")
    # the terminator must be canonical (we regenerate it)
    term = img.user(terminator_lba)
    if term[:7] != b"\xffCD001\x01" or term[7:] != bytes(USER - 7):
        print(f"warning: non-canonical terminator at LBA {terminator_lba}; storing raw", file=sys.stderr)
        # keep it as an extra descriptor file so it round-trips
        with open(tree / "meta" / f"descriptor_{terminator_lba}.bin", "wb") as f:
            f.write(term)
        descriptors.append((terminator_lba, f"descriptor_{terminator_lba}.bin"))
        terminator_lba = -1

    # path tables
    pt_size = struct.unpack("<I", pvd[132:136])[0]
    pt_l = [struct.unpack("<I", pvd[140:144])[0], struct.unpack("<I", pvd[144:148])[0]]
    pt_m = [struct.unpack(">I", pvd[148:152])[0], struct.unpack(">I", pvd[152:156])[0]]
    pt_l = [x for x in pt_l if x]
    pt_m = [x for x in pt_m if x]
    pt_sectors = (pt_size + USER - 1) // USER
    for x in pt_l + pt_m:
        cover(x, pt_sectors, "path_table")

    # directories
    dir_entries = []
    for d in dirs:
        nsec = (d.size + USER - 1) // USER
        cover(d.lba, nsec, "dir " + (d.path or "/"))
        dir_entries.append(d)

    # files
    file_entries = []
    n_form1 = n_raw = n_cdda = 0
    for r in files:
        nsec = (r.size + USER - 1) // USER
        xa = r.xa_attr
        host = tree / "cdrom" / Path(*r.path.split("/"))
        host.parent.mkdir(parents=True, exist_ok=True)
        entry = {"rec": r, "form": "1", "note": "", "md5": "", "track": 0}
        if xa is not None and (xa & 0x4000):
            # CD-DA "file": the entry points into an audio track; no data on the data track.
            trk = next((t for t in tracks_out if t["audio"] and t["start"] == r.lba), None)
            if trk is None:
                # some masters point at the pregap; accept start-150 too
                trk = next((t for t in tracks_out if t["audio"] and abs(t["start"] - r.lba) <= 150), None)
            if trk is None:
                print(f"warning: {r.path}: CDDA attribute but no audio track starts at LBA {r.lba}; treating as data",
                      file=sys.stderr)
            else:
                entry["form"] = "cdda"
                entry["track"] = trk["n"]
                n_cdda += 1
                file_entries.append(entry)
                continue
        if r.lba + nsec > img.sectors:
            print(f"error: {r.path}: extent {r.lba}+{nsec} beyond data track", file=sys.stderr)
            return 2
        form, note = classify_file(img, r.lba, nsec)
        entry["form"] = form
        entry["note"] = note
        h = hashlib.md5()
        with open(host, "wb") as out:
            if form == "1":
                remaining = r.size
                for i in range(nsec):
                    chunk = img.user(r.lba + i)[:min(USER, remaining)]
                    out.write(chunk)
                    h.update(chunk)
                    remaining -= len(chunk)
                n_form1 += 1
            else:
                for i in range(nsec):
                    chunk = img.raw(r.lba + i)[16:16 + FORM2_USER]
                    out.write(chunk)
                    h.update(chunk)
                n_raw += 1
        entry["md5"] = h.hexdigest()
        cover(r.lba, nsec, r.path)
        file_entries.append(entry)
    print(f"[disc_tree] files: {n_form1} Form 1 (cooked), {n_raw} raw Form 2/XA, {n_cdda} CD-DA aliases")

    # Unexplained sectors: "empty" ones (Mode 2, zero subheader, zero payload, zero
    # EDC/ECC — what every mastering tool writes for unallocated space) are simply
    # regenerated; anything else is stored raw so the tree still round-trips.
    # Runs are split into maximal empty / non-empty sub-runs (Mega Man 8, for
    # one, has 149 empty postgap sectors followed by ONE whose ECC was computed
    # over the header — a burner artifact worth 2352 bytes of meta/, not 150).
    raw_runs = []
    postgap = 0
    i = 0
    while i < img.sectors:
        if covered[i]:
            i += 1
            continue
        empty = is_empty_sector(img.raw(i))
        j = i + 1
        while j < img.sectors and not covered[j] and is_empty_sector(img.raw(j)) == empty:
            j += 1
        if empty:
            # regenerated as empty sectors; the LAST empty run of the track is the
            # postgap the builder re-creates after appended (grown/new) files
            postgap = j - i
        else:
            name = f"raw_{i}_{j - i}.bin"
            with open(tree / "meta" / name, "wb") as f:
                for k in range(i, j):
                    f.write(img.raw(k))
            raw_runs.append((i, j - i, name))
            print(f"[disc_tree] note: sectors {i}..{j - 1} are not files and not empty -> stored raw as meta/{name}")
        i = j
    if not postgap:
        print("[disc_tree] note: data track has no empty run; grown files will get a 150-sector postgap")
        postgap = 150

    # audio tracks → WAV
    audio_out = []
    if not args.no_audio:
        for t in tracks_out:
            if not t["audio"]:
                continue
            fpath = sheet.files[t["file_index"]]
            pre_frames = (t["start"] - t["pregap"])
            with open(fpath, "rb") as f:
                if len(sheet.files) > 1:
                    blob = f.read()
                else:
                    f.seek(t["pregap"] * RAW)
                    blob = f.read((t["end"] - t["pregap"]) * RAW)
            pre = blob[:pre_frames * RAW]
            pcm = blob[pre_frames * RAW:]
            name = f"track{t['n']:02d}.wav"
            write_wav(tree / "audio" / name, pcm)
            pre_raw = ""
            if pre != bytes(len(pre)):
                pre_raw = f"track{t['n']:02d}_pregap.bin"
                with open(tree / "meta" / pre_raw, "wb") as f:
                    f.write(pre)
                print(f"[disc_tree] note: track {t['n']} pregap is not silent -> meta/{pre_raw}")
            audio_out.append({"n": t["n"], "file": "audio/" + name, "pregap": pre_frames,
                              "sectors": t["end"] - t["pregap"], "pregap_raw": pre_raw,
                              "md5": hashlib.md5(blob).hexdigest() if not args.skip_hash else ""})
            print(f"[disc_tree] audio track {t['n']:02d}: {len(pcm) // RAW} sectors -> audio/{name}")

    # source digests
    src_md5 = src_sha1 = ""
    if not args.skip_hash:
        h5, h1 = hashlib.md5(), hashlib.sha1()
        with open(t1.file, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h5.update(chunk)
                h1.update(chunk)
        src_md5, src_sha1 = h5.hexdigest(), h1.hexdigest()

    # SYSTEM.CNF serial (informational)
    serial = ""
    cnf = next((e for e in file_entries if e["rec"].path.upper() == "SYSTEM.CNF"), None)
    if cnf is not None and cnf["form"] == "1":
        text = (tree / "cdrom" / "SYSTEM.CNF").read_bytes().decode("latin1", "replace")
        m = re.search(r"cdrom:\\?([A-Za-z0-9_.]+)", text)
        if m:
            tok = m.group(1).replace("_", "").replace(".", "")
            if len(tok) >= 9:
                serial = tok[:4] + "-" + tok[4:9]

    # ── manifest ───────────────────────────────────────────────────────────
    L = []
    L.append(f"# Generated by psxrecomp/tools/disc_tree.py from {cue_path.name}")
    L.append("# Do not hand-edit lba/size of pristine entries; add [[file]] entries only for")
    L.append("# files that need a non-default form/xa. See psxrecomp/docs/DISC_TREE.md.")
    L.append(f"format = {toml_str(FORMAT_ID)}")
    L.append(f"format_version = {FORMAT_VERSION}")
    L.append("")
    L.append("[source]")
    L.append(f"cue = {toml_str(cue_path.name)}")
    vol = pvd[40:72].decode("latin1").rstrip()
    L.append(f"volume_id = {toml_str(vol)}")
    if serial:
        L.append(f"serial = {toml_str(serial)}")
    L.append(f"data_track_sectors = {img.sectors}")
    if src_md5:
        L.append(f"data_track_md5 = {toml_str(src_md5)}")
        L.append(f"data_track_sha1 = {toml_str(src_sha1)}")
    L.append("")
    L.append("[layout]")
    L.append('system_area = "meta/system_area.bin"')
    L.append("descriptors = [" + ", ".join(toml_str("meta/" + n) for _, n in descriptors) + "]")
    L.append(f"descriptor_lba = {descriptors[0][0] if descriptors else 16}")
    L.append(f"terminator_lba = {terminator_lba}")
    L.append(f"path_table_l = [{', '.join(str(x) for x in pt_l)}]")
    L.append(f"path_table_m = [{', '.join(str(x) for x in pt_m)}]")
    L.append(f"postgap_sectors = {postgap}")
    L.append("")
    for lba0, n, name in raw_runs:
        L.append("[[raw]]")
        L.append(f"lba = {lba0}")
        L.append(f"sectors = {n}")
        L.append(f"file = {toml_str('meta/' + name)}")
        L.append("")
    for t in tracks_out:
        L.append("[[track]]")
        L.append(f"number = {t['n']}")
        if t["audio"]:
            a = next((x for x in audio_out if x["n"] == t["n"]), None)
            L.append('type = "audio"')
            if a:
                L.append(f"file = {toml_str(a['file'])}")
                L.append(f"pregap_sectors = {a['pregap']}")
                L.append(f"sectors = {a['sectors']}")
                if a["pregap_raw"]:
                    L.append(f"pregap_raw = {toml_str('meta/' + a['pregap_raw'])}")
                if a["md5"]:
                    L.append(f"md5 = {toml_str(a['md5'])}")
            else:
                L.append(f"pregap_sectors = {t['start'] - t['pregap']}")
                L.append(f"sectors = {t['end'] - t['pregap']}")
        else:
            L.append('type = "data"')
            L.append(f"sectors = {t['end'] - t['pregap']}")
        L.append("")
    for d in dir_entries:
        L.append("[[dir]]")
        L.append(f"path = {toml_str(d.path)}")
        L.append(f"lba = {d.lba}")
        L.append(f"sectors = {(d.size + USER - 1) // USER}")
        L.append(f"date = {toml_str(d.self_rec.date.hex())}")
        L.append(f"xa = {toml_str(d.self_rec.sua.hex())}")
        L.append("entries = [" + ", ".join(toml_str(r.base_name) for r in d.records) + "]")
        L.append("")
    for e in file_entries:
        r = e["rec"]
        L.append("[[file]]")
        L.append(f"path = {toml_str(r.path)}")
        L.append(f"lba = {r.lba}")
        L.append(f"size = {r.size}")
        if e["form"] == "cdda":
            L.append('form = "cdda"')
            L.append(f"track = {e['track']}")
        else:
            L.append(f"form = {toml_str(e['form'])}")
        if r.version != 1:
            L.append(f"version = {r.version}")
        L.append(f"date = {toml_str(r.date.hex())}")
        L.append(f"xa = {toml_str(r.sua.hex())}")
        if e["md5"]:
            L.append(f"md5 = {toml_str(e['md5'])}")
        if e["note"]:
            L.append(f"# {e['note']}")
        L.append("")
    (tree / "disc.toml").write_text("\n".join(L) + "\n", encoding="utf-8")

    (tree / "README.md").write_text(
        "# psxrecomp disc tree\n\n"
        f"Extracted from `{cue_path.name}` by `psxrecomp/tools/disc_tree.py`.\n\n"
        "* `cdrom/` — the disc's ISO9660 files. Edit or replace them freely; the runtime\n"
        "  rebuilds the disc image on the fly (same LBAs while a file fits its original\n"
        "  extent, relocated when it grows). Form 1 files are plain data; `.STR`/XA files\n"
        "  are raw 2336-byte sectors (jPSXdec / mkpsxiso compatible).\n"
        "* `audio/trackNN.wav` — CD-DA tracks (44.1 kHz, 16-bit, stereo). Replace to change\n"
        "  the CD music.\n"
        "* `meta/` — licence area / volume descriptor / raw sector runs. Leave alone.\n"
        "* `disc.toml` — the manifest. Pristine LBAs, sizes, dates and XA attributes.\n\n"
        "Verify a tree still round-trips: `psx-disc-tree verify <tree> <original.cue>`;\n"
        "write a bin/cue for an emulator: `psx-disc-tree build <tree> <out.cue>`.\n",
        encoding="utf-8")

    print(f"[disc_tree] wrote {tree / 'disc.toml'}")
    if not args.skip_check:
        args2 = argparse.Namespace(tree=str(tree), cue=str(cue_path))
        return cmd_check(args2)
    return 0


# ────────────────────────────────────────────────────────────────────────────
# manifest loading (tomllib) + layout twin used by `check`
# ────────────────────────────────────────────────────────────────────────────

def load_manifest(tree: Path) -> dict:
    import tomllib
    with open(tree / "disc.toml", "rb") as f:
        m = tomllib.load(f)
    if m.get("format") != FORMAT_ID:
        raise SystemExit(f"{tree}/disc.toml: not a {FORMAT_ID} manifest")
    return m


def cmd_list(args) -> int:
    tree = Path(args.tree)
    m = load_manifest(tree)
    src = m.get("source", {})
    print(f"tree: {tree}\nsource: {src.get('cue')}  volume {src.get('volume_id')}  serial {src.get('serial', '?')}")
    for t in m.get("track", []):
        print(f"  track {t['number']:02d} {t['type']:5s} sectors={t.get('sectors')} " +
              (f"pregap={t.get('pregap_sectors')} file={t.get('file')}" if t["type"] == "audio" else ""))
    print(f"{'LBA':>8} {'size':>10}  form  path")
    for d in m.get("dir", []):
        print(f"{d['lba']:8d} {d['sectors'] * USER:10d}  dir   {d['path'] or '/'}/")
    for f in m.get("file", []):
        print(f"{f['lba']:8d} {f['size']:10d}  {str(f['form']):4s}  {f['path']}")
    return 0


def cmd_status(args) -> int:
    tree = Path(args.tree)
    m = load_manifest(tree)
    changed = missing = added = 0
    known = set()
    for f in m.get("file", []):
        known.add(f["path"])
        if f["form"] == "cdda":
            continue
        host = tree / "cdrom" / Path(*f["path"].split("/"))
        if not host.exists():
            print(f"MISSING  {f['path']}")
            missing += 1
            continue
        if f.get("md5") and md5_file(host) != f["md5"]:
            sz = host.stat().st_size
            print(f"MODIFIED {f['path']}  ({sz} bytes, pristine {f['size']})")
            changed += 1
    for p in (tree / "cdrom").rglob("*"):
        if p.is_file():
            rel = "/".join(p.relative_to(tree / "cdrom").parts)
            if rel not in known:
                print(f"ADDED    {rel}")
                added += 1
    for t in m.get("track", []):
        if t["type"] == "audio" and t.get("md5"):
            wav = tree / t["file"]
            if not wav.exists():
                print(f"MISSING  {t['file']}")
                missing += 1
    print(f"{changed} modified, {added} added, {missing} missing "
          f"({'pristine' if not (changed or added or missing) else 'customised'})")
    return 0


def cmd_check(args) -> int:
    """Data-level self-check: regenerate every metadata payload from the manifest and
    compare with the source image; compare every file payload too. No EDC/ECC here —
    that is the runtime's job (psx-disc-tree verify does the full raw compare)."""
    tree = Path(args.tree)
    m = load_manifest(tree)
    cue = Path(args.cue) if args.cue else None
    if cue is None:
        print("check: need the source cue", file=sys.stderr)
        return 2
    sheet = parse_cue(cue)
    img = RawImage(sheet.tracks[0].file)
    bad = 0

    dirs = {d["path"]: d for d in m["dir"]}
    files = m["file"]
    by_parent: dict[str, list] = {}
    for d in m["dir"]:
        by_parent.setdefault(d["path"], [])
    for d in m["dir"]:
        if d["path"]:
            parent = d["path"].rsplit("/", 1)[0] if "/" in d["path"] else ""
            by_parent[parent].append(("dir", d))
    for f in files:
        parent = f["path"].rsplit("/", 1)[0] if "/" in f["path"] else ""
        by_parent[parent].append(("file", f))

    tracks = {t["number"]: t for t in m["track"]}
    audio_start = {}
    lba = 0
    for t in m["track"]:
        pre = t.get("pregap_sectors", 0)
        audio_start[t["number"]] = lba + pre
        lba += t["sectors"] if t["type"] == "data" else t["sectors"]

    def dir_records(d) -> list[bytes]:
        recs = []
        parent_path = d["path"].rsplit("/", 1)[0] if "/" in d["path"] else ""
        parent = dirs[parent_path] if d["path"] else d
        recs.append(build_record(b"\x00", d["lba"], d["sectors"] * USER, 2, bytes.fromhex(d["date"]), bytes.fromhex(d["xa"])))
        recs.append(build_record(b"\x01", parent["lba"], parent["sectors"] * USER, 2, bytes.fromhex(parent["date"]), bytes.fromhex(parent["xa"])))
        children = {}
        for kind, e in by_parent[d["path"]]:
            children[e["path"].rsplit("/", 1)[-1]] = (kind, e)
        for name in d["entries"]:
            kind, e = children[name]
            if kind == "dir":
                recs.append(build_record(name.encode("latin1"), e["lba"], e["sectors"] * USER, 2,
                                         bytes.fromhex(e["date"]), bytes.fromhex(e["xa"])))
            else:
                if e["form"] == "cdda":
                    lba_e = audio_start[e["track"]]
                    size_e = (tracks[e["track"]]["sectors"] - tracks[e["track"]].get("pregap_sectors", 0)) * USER
                else:
                    lba_e, size_e = e["lba"], e["size"]
                ident = f"{name};{e.get('version', 1)}".encode("latin1")
                recs.append(build_record(ident, lba_e, size_e, 0, bytes.fromhex(e["date"]), bytes.fromhex(e["xa"])))
        return recs

    for d in m["dir"]:
        data = build_directory(dir_records(d))
        want = b"".join(img.user(d["lba"] + i) for i in range(d["sectors"]))
        if len(data) != len(want):
            print(f"check: dir {d['path'] or '/'} regenerates to {len(data)} bytes, on disc {len(want)}")
            bad += 1
        elif data != want:
            i = next(k for k in range(len(data)) if data[k] != want[k])
            print(f"check: dir {d['path'] or '/'} differs at byte {i}: {data[i:i+16].hex()} vs {want[i:i+16].hex()}")
            bad += 1

    # path tables
    def path_table(big: bool) -> bytes:
        out = bytearray()
        numbering = {}
        order = sorted(m["dir"], key=lambda d: (d["path"].count("/") + (1 if d["path"] else 0), d["path"]))
        # ECMA-119: ordered by level, then parent number, then name
        level = {}
        for d in m["dir"]:
            level[d["path"]] = 1 if not d["path"] else d["path"].count("/") + 2
        # assign numbers breadth-first with the sort inside each level by (parent number, name)
        numbered = []
        for lv in sorted(set(level.values())):
            same = [d for d in m["dir"] if level[d["path"]] == lv]
            same.sort(key=lambda d: (numbering.get(d["path"].rsplit("/", 1)[0] if "/" in d["path"] else "", 1) if d["path"] else 0,
                                     iso_sort_key(d["path"].rsplit("/", 1)[-1] if d["path"] else "\x00")))
            for d in same:
                numbering[d["path"]] = len(numbered) + 1
                numbered.append(d)
        for d in numbered:
            name = b"\x00" if not d["path"] else d["path"].rsplit("/", 1)[-1].encode("latin1")
            parent = numbering[d["path"].rsplit("/", 1)[0] if "/" in d["path"] else ""] if d["path"] else 1
            ent = bytes([len(name), 0]) + (struct.pack(">I", d["lba"]) if big else struct.pack("<I", d["lba"]))
            ent += struct.pack(">H", parent) if big else struct.pack("<H", parent)
            ent += name + (b"\x00" if len(name) % 2 else b"")
            out += ent
        return bytes(out)

    lay = m["layout"]
    for big, lbas in ((False, lay["path_table_l"]), (True, lay["path_table_m"])):
        pt = path_table(big)
        for x in lbas:
            want = img.user(x)[:len(pt)]
            if want != pt:
                print(f"check: path table at LBA {x} differs")
                bad += 1
            rest = img.user(x)[len(pt):]
            if rest != bytes(len(rest)):
                print(f"check: path table at LBA {x} has trailing non-zero bytes")
                bad += 1
    pvd = (tree / lay["descriptors"][0]).read_bytes()
    if struct.unpack("<I", pvd[132:136])[0] != len(path_table(False)):
        print("check: PVD path table size differs from regenerated size")
        bad += 1

    # files
    for f in files:
        if f["form"] == "cdda":
            continue
        host = tree / "cdrom" / Path(*f["path"].split("/"))
        data = host.read_bytes()
        nsec = (f["size"] + USER - 1) // USER
        if str(f["form"]) == "1":
            want = b"".join(img.user(f["lba"] + i) for i in range(nsec))[:f["size"]]
        else:
            want = b"".join(img.raw(f["lba"] + i)[16:16 + FORM2_USER] for i in range(nsec))
        if data != want:
            print(f"check: file {f['path']} payload differs from disc")
            bad += 1
    print(f"[disc_tree] check: {'OK — metadata and payloads regenerate byte-for-byte' if not bad else str(bad) + ' problem(s)'}")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract", help="extract a bin/cue into a disc tree")
    e.add_argument("cue")
    e.add_argument("tree")
    e.add_argument("--force", action="store_true", help="write into a non-empty directory")
    e.add_argument("--no-audio", action="store_true", help="skip CD-DA track extraction")
    e.add_argument("--skip-hash", action="store_true", help="do not record md5/sha1 digests")
    e.add_argument("--skip-check", action="store_true", help="skip the regeneration self-check")
    e.set_defaults(fn=cmd_extract)
    l = sub.add_parser("list", help="print the manifest layout")
    l.add_argument("tree")
    l.set_defaults(fn=cmd_list)
    s = sub.add_parser("status", help="show modified/added/missing files vs the pristine dump")
    s.add_argument("tree")
    s.set_defaults(fn=cmd_status)
    c = sub.add_parser("check", help="data-level regeneration self-check against the source cue")
    c.add_argument("tree")
    c.add_argument("cue", nargs="?")
    c.set_defaults(fn=cmd_check)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
