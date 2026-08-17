#!/usr/bin/env python3
"""texpack_parity.py — software vs OpenGL parity of texture-pack replacement
(docs/TEXTURE_PACKS.md, B11).

Runs the game four times from a savestate at the same internal scale S —
software / OpenGL, each with the pack off and on — captures the S x picture of
each run, and checks that the pack does not ADD divergence between the two
renderers: the SW-vs-GL pixel differences with the pack on must be no worse
than the SW-vs-GL differences with the pack off (the renderers' own baseline:
5-bit expansion, edge/blend rounding). Also reports what the pack changed
(SW off vs SW on) so an "identity" starter pack can be checked for zero.

  software: --headless, `screenshot_hires` = the S x hi-res mirror, exact.
  OpenGL:   windowed (no GL without a window); `present_capture` with
            companions under the `sharp` filter writes `<path>.src.png` = an
            exact glReadPixels of the S x FBO rect the presenter consumed
            (the plain present itself is a half-texel-inset linear fit and is
            NOT pixel-exact, so the drawable is not compared).

    python3 tools/texpack_parity.py --exe build-debug/Game --game game.toml \
        --bios bios.bin --disc game-assets/disc --pack game-assets/textures/pack \
        --slot 3 [--slot 0 ...] [--scale 2] [--settle 60] [--tolerance 8] [--out DIR]

Needs a debug-tools build (script mode + debug server). Exit 1 on failure.
Environment the tool sets for each run: PSX_SUPERSAMPLING, PSX_VIDEO_FILTER
(none for software, sharp for OpenGL — the capture hook), PSX_TEXTURE_PACK
(pack-on runs); pack-off runs unload any config pack first.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image, ImageChops
except ImportError:  # pragma: no cover
    sys.exit("needs Pillow (pip install pillow)")


def run_capture(a, renderer: str, pack_on: bool, slot: int, out_png: Path):
    env = dict(os.environ)
    env["PSX_SUPERSAMPLING"] = str(a.scale)
    env["PSX_VIDEO_FILTER"] = "none" if renderer == "software" else "sharp"
    env.pop("PSX_TEXTURE_PACK", None)
    if pack_on:
        env["PSX_TEXTURE_PACK"] = str(Path(a.pack).resolve())
    steps = [f"wait:{a.boot_wait}"]
    if not pack_on:
        steps.append('{"cmd":"texture_pack","op":"unload"}')
    steps.append(f'{{"cmd":"savestate","op":"load","slot":{slot}}}')
    steps.append('expect:"ok":true')
    steps.append(f"wait:{a.settle}")
    if renderer == "software":
        steps.append(f'{{"cmd":"screenshot_hires","path":"{out_png}"}}')
        steps.append('expect:"ok":true')
    else:
        steps.append(f'{{"cmd":"present_capture","path":"{out_png}","companions":1}}')
        steps.append("wait:3")
        steps.append('{"cmd":"present_capture"}')
        steps.append("expect:written")
    steps.append('{"cmd":"texture_pack","op":"stats"}')
    steps.append("quit")
    cmd = [a.exe, "--game", a.game, "--renderer", renderer, "--no-launcher", "--script", ";".join(steps)]
    if a.bios:
        cmd += ["--bios", a.bios]
    if a.disc:
        cmd += ["--disc", a.disc]
    if renderer == "software":
        cmd.append("--headless")
    log = out_png.with_suffix(".log")
    env["PSX_SCRIPT_LOG"] = str(out_png.with_suffix(".script.log"))
    with open(log, "w") as lf:
        r = subprocess.run(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=a.timeout, cwd=a.cwd or None)
    stats = ""
    try:
        for line in open(env["PSX_SCRIPT_LOG"]):
            if '"stats":{' in line:
                stats = line[line.index('"stats":{'):].strip()[:200]
    except OSError:
        pass
    if r.returncode != 0 or not out_png.exists():
        sys.exit(f"run failed ({renderer}, pack {'on' if pack_on else 'off'}, slot {slot}): exit {r.returncode}, see {log}")
    if renderer == "opengl":
        src = Path(str(out_png) + ".src.png")
        if not src.exists():
            sys.exit(f"OpenGL run wrote no {src.name} companion (filter capture path not taken?), see {log}")
        src.replace(out_png)                     # the exact S x FBO rect is what we compare
    return stats


def diff_stats(a_path: Path, b_path: Path, tol: int):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        return None, f"size mismatch {a.size} vs {b.size}"
    d = ImageChops.difference(a, b)
    over = worst = 0
    total = a.size[0] * a.size[1]
    for p in d.getdata():
        m = max(p)
        if m > worst:
            worst = m
        if m > tol:
            over += 1
    return (over, worst, total), f"max {worst}, {over}/{total} px over {tol}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--bios")
    ap.add_argument("--disc")
    ap.add_argument("--pack", required=True)
    ap.add_argument("--slot", type=int, action="append", required=True, help="savestate slot(s) to check")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--settle", type=int, default=60, help="frames after the load before capturing")
    ap.add_argument("--boot-wait", type=int, default=30, help="frames before the load")
    ap.add_argument("--tolerance", type=int, default=8, help="per-channel 8-bit difference that counts as a divergence (8 = one 5-bit level)")
    ap.add_argument("--margin", type=float, default=0.005, help="pack may add at most this fraction of pixels over tolerance vs the no-pack baseline")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--cwd", help="working directory for the runtime (project root)")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    out = Path(a.out or (os.path.join(os.environ["CLAUDE_JOB_DIR"], "tmp", "texpack_parity") if os.environ.get("CLAUDE_JOB_DIR") else "texpack_parity"))
    out.mkdir(parents=True, exist_ok=True)
    a.exe = str(Path(a.exe).resolve())
    a.game = str(Path(a.game).resolve())
    if a.bios:
        a.bios = str(Path(a.bios).resolve())
    if a.disc:
        a.disc = str(Path(a.disc).resolve())
    failed = 0
    for slot in a.slot:
        caps = {}
        for renderer in ("software", "opengl"):
            for pack_on in (False, True):
                name = f"s{slot}_{renderer}_{'pack' if pack_on else 'nopack'}.png"
                png = out / name
                if png.exists():
                    png.unlink()
                st = run_capture(a, renderer, pack_on, slot, png)
                caps[(renderer, pack_on)] = png
                im = Image.open(png)
                print(f"slot {slot} {renderer:8s} pack {'on ' if pack_on else 'off'}: {im.width}x{im.height} {st}")
        (base, msg_base) = diff_stats(caps[("software", False)], caps[("opengl", False)], a.tolerance)
        (pk, msg_pack) = diff_stats(caps[("software", True)], caps[("opengl", True)], a.tolerance)
        (chg, msg_chg) = diff_stats(caps[("software", False)], caps[("software", True)], 0)
        print(f"  SW vs GL, pack off (baseline): {msg_base}")
        print(f"  SW vs GL, pack on            : {msg_pack}")
        print(f"  SW pack off vs on (what the pack changed): {msg_chg}")
        if base is None or pk is None:
            print("  FAIL: capture sizes differ (different display mode between the runs?)")
            failed += 1
            continue
        allowed = base[0] + int(a.margin * base[2])
        if pk[0] > allowed:
            print(f"  FAIL: the pack adds SW/GL divergence ({pk[0]} px over tolerance vs {base[0]} baseline, allowed {allowed})")
            failed += 1
        else:
            print(f"  OK: pack-on divergence {pk[0]} <= {allowed} (baseline {base[0]} + margin)")
    print(f"captures in {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
