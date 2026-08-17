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

Only Pillow is required; `from-video` shells out to ffmpeg/ffprobe.
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
    fps = a.frames / duration
    vf = f"fps={fps:.6f}"
    if a.size:
        w, h = a.size.lower().split("x")
        vf += f",scale={int(w)}:{int(h)}"
    ext = "jpg" if a.jpg else "png"
    cmd = [ffmpeg, "-v", "error", "-y", "-i", a.video, "-vf", vf, "-frames:v", str(a.frames),
           "-start_number", "0"]
    if a.jpg:
        cmd += ["-q:v", "3"]
    cmd.append(str(out / f"%05d.{ext}"))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("ffmpeg failed")
    got = len(movie_frames(out))
    print(f"{a.video}: {duration:.2f}s -> {got} frames at {fps:.3f} fps in {out}"
          + ("" if got == a.frames else f" (WARNING: wanted {a.frames})"))
    return 0 if got == a.frames else 1


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
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
