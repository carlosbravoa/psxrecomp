#!/usr/bin/env python3
"""fmv_pack.py — HD movie-pack authoring helpers (docs/FMV_PACKS.md).

A pack is <pack>/<MOVIE>/NNNNN.png|jpg — one sub-directory per STR movie
(named like the file on the disc, without extension), one image per MDEC-decoded
frame, 0-based, any size. The runtime's `fmv_dump` (or PSX_FMV_DUMP=<dir>)
writes the native frames in exactly that layout, so:

    fmv_pack.py info    PACK                          movies, frame counts, sizes, gaps
    fmv_pack.py upscale DUMP PACK --scale N [--filter lanczos|nearest]
                                                      N x every dumped frame: the identity skeleton
    fmv_pack.py from-video VIDEO PACK/MOVIE --frames N [--size WxH] [--jpg]
                                                      resample a video (any fps/length) to exactly N
                                                      frames with ffmpeg — N = the dump's frame count
    fmv_pack.py check   PACK DUMP                     every dumped movie/frame covered by the pack?
    fmv_pack.py align   STR VIDEO [--write PACK/MOVIE]  frame offset between the STR and an edited/upscaled
                                                      video (an upscaler that trims a black lead-in shifts
                                                      everything): prints the best shift and can write
                                                      movie.toml `offset = -N` into the pack directory
    fmv_pack.py export-str STR... OUTDIR [--png] [--crf N]
                                                      decode PSX STR files (as stored in a disc tree,
                                                      raw 2336-byte sectors, or 2352 raw) to MP4
                                                      (video + XA audio, one frame per STR frame)
                                                      or PNG frames + WAV — for upscalers/editors

Only Pillow is required; `from-video` / `export-str` shell out to ffmpeg/ffprobe.
Round trip: export-str -> upscale/edit (keep the cut) -> from-video --frames <STR frames>.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("fmv_pack.py needs Pillow (pip install pillow)")

FRAME_RE = re.compile(r"^(\d{5})\.(png|jpe?g)$", re.I)


def movie_frames(d: Path):
    """index -> path for one movie directory"""
    out = {}
    for p in d.iterdir():
        m = FRAME_RE.match(p.name)
        if m:
            out[int(m.group(1))] = p
    return out


def movies(root: Path):
    return sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith("."))


def cmd_info(a):
    root = Path(a.pack)
    total = 0
    for d in movies(root):
        fr = movie_frames(d)
        if not fr:
            continue
        idx = sorted(fr)
        gaps = [i for i in range(idx[0], idx[-1] + 1) if i not in fr]
        sizes = {}
        for i in idx[:: max(1, len(idx) // 5)]:
            with Image.open(fr[i]) as im:
                sizes[im.size] = sizes.get(im.size, 0) + 1
        toml = d / "movie.toml"
        print(f"{d.name}: {len(idx)} frames [{idx[0]}..{idx[-1]}], sizes {sorted(sizes)}"
              + (f", {len(gaps)} missing" if gaps else "") + (", movie.toml" if toml.exists() else ""))
        total += len(idx)
    print(f"{root}: {total} frames")
    return 0


def cmd_upscale(a):
    dump, pack = Path(a.dump), Path(a.pack)
    flt = Image.NEAREST if a.filter == "nearest" else Image.LANCZOS
    n = 0
    for d in movies(dump):
        fr = movie_frames(d)
        if not fr:
            continue
        out = pack / d.name
        out.mkdir(parents=True, exist_ok=True)
        for i, p in sorted(fr.items()):
            with Image.open(p) as im:
                im = im.convert("RGB").resize((im.width * a.scale, im.height * a.scale), flt)
                im.save(out / f"{i:05d}.png")
            n += 1
        print(f"{d.name}: {len(fr)} frames -> {out}")
    print(f"upscaled {n} frames at {a.scale}x -> {pack}")
    return 0


def cmd_from_video(a):
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        sys.exit("from-video needs ffmpeg and ffprobe on PATH")
    out = Path(a.movie_dir)
    out.mkdir(parents=True, exist_ok=True)
    pr = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "json", a.video],
                        capture_output=True, text=True)
    try:
        duration = float(json.loads(pr.stdout)["format"]["duration"])
    except (KeyError, ValueError, json.JSONDecodeError):
        sys.exit(f"ffprobe could not read the duration of {a.video}: {pr.stderr.strip()}")
    if duration <= 0:
        sys.exit("zero-length video")
    # Exact frame count of the source (nb_frames from the container when present,
    # else a decode-and-count pass): a video that already has the wanted number
    # of frames is copied 1:1 — no fps resampling that could drop/duplicate.
    src_frames = 0
    pr2 = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=nb_frames",
                          "-of", "default=nw=1:nk=1", a.video], capture_output=True, text=True)
    try:
        src_frames = int(pr2.stdout.strip())
    except ValueError:
        pr3 = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
                              "-show_entries", "stream=nb_read_frames", "-of", "default=nw=1:nk=1", a.video],
                             capture_output=True, text=True)
        try:
            src_frames = int(pr3.stdout.strip())
        except ValueError:
            src_frames = 0
    vfs = []
    one_to_one = src_frames == a.frames
    fps = a.frames / duration
    if not one_to_one:
        vfs.append(f"fps={fps:.6f}")
    if a.size:
        w, h = a.size.lower().split("x")
        vfs.append(f"scale={int(w)}:{int(h)}")
    ext = "jpg" if a.jpg else "png"
    cmd = [ffmpeg, "-v", "error", "-y", "-i", a.video]
    if vfs:
        cmd += ["-vf", ",".join(vfs)]
    cmd += ["-frames:v", str(a.frames), "-start_number", "0"]
    if a.jpg:
        cmd += ["-q:v", "3"]
    cmd.append(str(out / f"%05d.{ext}"))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("ffmpeg failed")
    got = movie_frames(out)
    n_got = len(got)
    # A source a few frames short (rounding of the resample, a trimmed tail): hold
    # the last picture so the pack still covers every decode index.
    padded = 0
    if 0 < n_got < a.frames:
        last = got[max(got)]
        for i in range(max(got) + 1, a.frames):
            shutil.copyfile(last, out / f"{i:05d}.{ext}")
            padded += 1
        n_got = len(movie_frames(out))
    print(f"{a.video}: {duration:.2f}s, {src_frames or '?'} source frames -> {n_got} frames in {out}"
          + (" (1:1)" if one_to_one else f" (resampled at {fps:.3f} fps)")
          + (f", last frame held for {padded} missing at the end" if padded else "")
          + ("" if n_got == a.frames else f" (WARNING: wanted {a.frames})"))
    return 0 if n_got == a.frames else 1


def wrap_2352(src: Path, dst: Path) -> int:
    """PSX STR as stored in a disc tree = raw 2336-byte sectors (subheader + data +
    EDC); ffmpeg's psxstr demuxer wants full 2352-byte sectors with sync + header,
    so prepend them (MSF is cosmetic). Returns the sector count; passes 2352 files through."""
    data = src.read_bytes()
    if len(data) % 2352 == 0 and data[:12] == bytes([0] + [0xFF] * 10 + [0]):
        dst.write_bytes(data)
        return len(data) // 2352
    if len(data) % 2336:
        sys.exit(f"{src}: {len(data)} bytes is neither raw 2336 nor 2352 sectors (a cooked 2048 copy has no XA audio; copy the STR raw — disc_tree.py copy)")
    sync = bytes([0] + [0xFF] * 10 + [0])
    def bcd(v): return ((v // 10) << 4) | (v % 10)
    out = bytearray()
    n = len(data) // 2336
    for i in range(n):
        lba = i + 150
        out += sync + bytes([bcd(lba // 4500), bcd((lba // 75) % 60), bcd(lba % 75), 2]) + data[i * 2336:(i + 1) * 2336]
    dst.write_bytes(out)
    return n


def cmd_export_str(a):
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        sys.exit("export-str needs ffmpeg and ffprobe on PATH")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    import tempfile
    rc = 0
    for f in a.strs:
        src = Path(f)
        name = src.stem.upper()
        with tempfile.NamedTemporaryFile(suffix=".str", delete=False) as t:
            tmp = Path(t.name)
        try:
            wrap_2352(src, tmp)
            pr = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
                                 "-show_entries", "stream=nb_read_frames,width,height,r_frame_rate",
                                 "-of", "json", str(tmp)], capture_output=True, text=True)
            try:
                st = json.loads(pr.stdout)["streams"][0]
                frames = int(st.get("nb_read_frames") or 0)
                print(f"{src.name}: {st.get('width')}x{st.get('height')} @ {st.get('r_frame_rate')}, {frames} frames")
            except (KeyError, IndexError, ValueError, json.JSONDecodeError):
                print(f"{src.name}: ffprobe could not read it ({pr.stderr.strip()[:200]})", file=sys.stderr)
                rc = 1
                continue
            if a.png:
                d = out / name
                d.mkdir(parents=True, exist_ok=True)
                r = subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(tmp), "-start_number", "0", str(d / "%05d.png")])
                r2 = subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(tmp), "-vn", "-c:a", "pcm_s16le", str(d / "audio.wav")])
                ok = r.returncode == 0 and r2.returncode == 0
                print(f"   -> {d}/00000.png .. {frames - 1:05d}.png + audio.wav" if ok else "   ffmpeg failed")
            else:
                mp4 = out / f"{name}.mp4"
                r = subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(tmp), "-c:v", "libx264", "-crf", str(a.crf),
                                    "-preset", "slow", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", str(mp4)])
                ok = r.returncode == 0
                print(f"   -> {mp4}  (use --frames {frames} with from-video)" if ok else "   ffmpeg failed")
            rc |= 0 if ok else 1
        finally:
            tmp.unlink(missing_ok=True)
    return rc


def gray_thumbs(ffmpeg: str, src: Path, w: int = 32, h: int = 24):
    """List of w*h grayscale frames (bytes) of a video / wrapped STR."""
    r = subprocess.run([ffmpeg, "-v", "error", "-i", str(src), "-vf", f"scale={w}:{h}", "-pix_fmt", "gray",
                        "-f", "rawvideo", "-"], capture_output=True)
    if r.returncode != 0:
        sys.exit(f"ffmpeg could not decode {src}: {r.stderr.decode(errors='replace')[:200]}")
    n = w * h
    return [r.stdout[i * n:(i + 1) * n] for i in range(len(r.stdout) // n)]


def cmd_align(a):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("align needs ffmpeg on PATH")
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".str", delete=False) as t:
        tmp = Path(t.name)
    try:
        wrap_2352(Path(a.str), tmp)
        A = gray_thumbs(ffmpeg, tmp)
    finally:
        tmp.unlink(missing_ok=True)
    B = gray_thumbs(ffmpeg, Path(a.video))
    n = 32 * 24
    def dist(x, y):
        return sum(abs(p - q) for p, q in zip(x, y)) / n
    print(f"{Path(a.str).name}: {len(A)} frames; {Path(a.video).name}: {len(B)} frames")
    best = None
    step = max(1, len(B) // 400)                       # ~400 sample points
    for k in range(-a.range, a.range + 1):             # video frame j <-> STR frame j + k
        good = tot = 0
        for j in range(0, len(B), step):
            i = j + k
            if 0 <= i < len(A):
                good += 1 if dist(A[i], B[j]) < 6 else 0
                tot += 1
        if tot and (best is None or good > best[1]):
            best = (k, good, tot)
    k, good, tot = best
    print(f"best: video frame j = STR frame j + {k}  ({good}/{tot} sample frames match closely)")
    print(f"pack offset = {-k}  (pack index = decode index {'-' if k > 0 else '+'} {abs(k)})" if k else "in sync: no offset needed")
    if a.write:
        d = Path(a.write)
        d.mkdir(parents=True, exist_ok=True)
        toml = d / "movie.toml"
        lines = [l for l in (toml.read_text().splitlines() if toml.exists() else []) if not l.strip().startswith("offset")]
        if k:
            lines.append(f"offset = {-k}")
        toml.write_text("\n".join(lines) + ("\n" if lines else ""))
        print(f"wrote {toml}")
    return 0


def cmd_check(a):
    pack, dump = Path(a.pack), Path(a.dump)
    bad = 0
    for d in movies(dump):
        dfr = movie_frames(d)
        if not dfr:
            continue
        pdir = pack / d.name
        pfr = movie_frames(pdir) if pdir.is_dir() else {}
        missing = sorted(i for i in dfr if i not in pfr)
        extra = sorted(i for i in pfr if i not in dfr)
        print(f"{d.name}: dump {len(dfr)} frames, pack {len(pfr)}"
              + (f", MISSING {len(missing)} (first {missing[:5]})" if missing else ", complete")
              + (f", {len(extra)} extra" if extra else ""))
        bad += 1 if missing else 0
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info"); p.add_argument("pack"); p.set_defaults(fn=cmd_info)
    p = sub.add_parser("upscale"); p.add_argument("dump"); p.add_argument("pack")
    p.add_argument("--scale", type=int, default=2); p.add_argument("--filter", default="lanczos", choices=["lanczos", "nearest"])
    p.set_defaults(fn=cmd_upscale)
    p = sub.add_parser("from-video"); p.add_argument("video"); p.add_argument("movie_dir", help="PACK/<MOVIE>")
    p.add_argument("--frames", type=int, required=True, help="frame count of the movie (= the dump's)")
    p.add_argument("--size", help="WxH output size (default: the video's)")
    p.add_argument("--jpg", action="store_true", help="write JPEG frames (smaller, faster to decode)")
    p.set_defaults(fn=cmd_from_video)
    p = sub.add_parser("check"); p.add_argument("pack"); p.add_argument("dump"); p.set_defaults(fn=cmd_check)
    p = sub.add_parser("align"); p.add_argument("str"); p.add_argument("video")
    p.add_argument("--range", type=int, default=60, help="max shift searched, frames")
    p.add_argument("--write", help="PACK/MOVIE directory to write movie.toml offset into")
    p.set_defaults(fn=cmd_align)
    p = sub.add_parser("export-str"); p.add_argument("strs", nargs="+"); p.add_argument("--out", required=True)
    p.add_argument("--png", action="store_true", help="PNG frame folder + audio.wav per movie instead of MP4")
    p.add_argument("--crf", type=int, default=10, help="x264 quality for MP4 (lower = better; 10 is visually lossless)")
    p.set_defaults(fn=cmd_export_str)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
