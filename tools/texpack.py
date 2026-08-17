#!/usr/bin/env python3
"""texpack.py — texture-pack authoring helpers (docs/TEXTURE_PACKS.md).

A dump directory (runtime `texture_dump`) holds <tex_id>-<pal_id>.png files
+ textures.tsv; a pack directory holds <tex_id>.png (any palette) and/or
<tex_id>-<pal_id>.png (one palette) at an integer multiple of the native size.

    texpack.py summary  DUMPDIR                        what the dump contains
    texpack.py starter  DUMPDIR PACKDIR --scale N      one <tex_id>.png per texel id, N x nearest
                        [--palette first|all|<pal_id>] (the pixel-identical skeleton artists repaint)
    texpack.py coverage PACKDIR TSV [TSV...]           which dumped textures the pack covers
    texpack.py validate PACKDIR [--tsv TSV]            filenames / sizes / alpha sanity
    texpack.py sheet    DIR OUT.png [--cols 48]        contact sheet of a dump or pack

Only Pillow is required (`pip install pillow`).
"""

from __future__ import annotations

import argparse
import collections
import csv
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("texpack.py needs Pillow (pip install pillow)")

NAME_RE = re.compile(r"^([0-9a-fA-F]{16})(?:-([0-9a-fA-F]{16}))?\.png$")


def read_tsv(path: Path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    return rows


def pack_index(pack: Path):
    """tex_id -> {pal_id or None: Path}"""
    idx: dict[str, dict] = collections.defaultdict(dict)
    for p in pack.iterdir():
        m = NAME_RE.match(p.name)
        if not m:
            continue
        idx[m.group(1).lower()][m.group(2).lower() if m.group(2) else None] = p
    return idx


def cmd_summary(a):
    d = Path(a.dump)
    rows = read_tsv(d / "textures.tsv")
    by_tex = collections.defaultdict(list)
    for r in rows:
        by_tex[r["tex_id"]].append(r)
    sizes = collections.Counter((r["w"], r["h"]) for r in rows)
    bpp = collections.Counter(r["bpp"] for r in rows)
    npal = collections.Counter(len(v) for v in by_tex.values())
    print(f"{d}: {len(rows)} (texel, palette) pairs, {len(by_tex)} texel ids")
    print(f"  bpp: {dict(bpp)}")
    print(f"  sizes: {sizes.most_common(8)}")
    print(f"  palettes per texel id: {sorted(npal.items())[:12]}")
    pages = collections.Counter((r["texpage_x"], r["texpage_y"]) for r in rows)
    print(f"  texture pages: {pages.most_common(10)}")
    frames = sorted(int(r["first_frame"]) for r in rows)
    print(f"  first-seen frames: {frames[0]}..{frames[-1]}")
    return 0


def cmd_starter(a):
    d, out = Path(a.dump), Path(a.pack)
    rows = read_tsv(d / "textures.tsv")
    out.mkdir(parents=True, exist_ok=True)
    by_tex = collections.defaultdict(list)
    for r in rows:
        by_tex[r["tex_id"]].append(r)
    n = 0
    for tex, rs in by_tex.items():
        if a.palette == "first":
            picks = [(rs[0], None)]
        elif a.palette == "all":
            picks = [(r, r["pal_id"]) for r in rs]
        else:
            rr = [r for r in rs if r["pal_id"].lower() == a.palette.lower()]
            picks = [(rr[0], None)] if rr else []
        for r, pal in picks:
            src = d / f"{r['tex_id']}-{r['pal_id']}.png"
            if not src.exists():
                continue
            im = Image.open(src).convert("RGBA")
            im = im.resize((im.width * a.scale, im.height * a.scale), Image.NEAREST)
            name = f"{tex}.png" if pal is None else f"{tex}-{pal}.png"
            im.save(out / name)
            n += 1
    print(f"starter pack: {n} images at {a.scale}x -> {out}")
    return 0


def cmd_coverage(a):
    idx = pack_index(Path(a.pack))
    total = 0
    by_tex_seen = set()
    hit_tex = set()
    hit_exact = 0
    missing = []
    for t in a.tsv:
        for r in read_tsv(Path(t)):
            total += 1
            tex, pal = r["tex_id"].lower(), r["pal_id"].lower()
            by_tex_seen.add(tex)
            v = idx.get(tex)
            if v and (pal in v or None in v):
                hit_tex.add(tex)
                if pal in v:
                    hit_exact += 1
            elif not v:
                missing.append(r)
    ids = len(by_tex_seen)
    print(f"pack {a.pack}: {len(idx)} texel ids; dump: {total} pairs, {ids} texel ids")
    print(f"  covered texel ids: {len(hit_tex)}/{ids} ({100.0 * len(hit_tex) / max(1, ids):.1f}%), exact-palette pairs: {hit_exact}/{total}")
    if a.missing:
        with open(a.missing, "w") as f:
            f.write("tex_id\tpal_id\tw\th\tfirst_frame\n")
            seen = set()
            for r in missing:
                if r["tex_id"] in seen:
                    continue
                seen.add(r["tex_id"])
                f.write(f"{r['tex_id']}\t{r['pal_id']}\t{r['w']}\t{r['h']}\t{r['first_frame']}\n")
        print(f"  missing texel ids written to {a.missing}")
    return 0


def cmd_validate(a):
    pack = Path(a.pack)
    native = {}
    if a.tsv:
        for r in read_tsv(Path(a.tsv)):
            native.setdefault(r["tex_id"].lower(), (int(r["w"]), int(r["h"])))
    ok = bad = skipped = 0
    for p in sorted(pack.iterdir()):
        if p.suffix.lower() != ".png":
            continue
        m = NAME_RE.match(p.name)
        if not m:
            print(f"  IGNORED (name is not <16hex>[-<16hex>].png): {p.name}")
            skipped += 1
            continue
        im = Image.open(p)
        problems = []
        if im.mode not in ("RGBA", "LA", "P") and "transparency" not in im.info:
            problems.append(f"no alpha channel ({im.mode}) — colour 0 texels cannot be transparent")
        nat = native.get(m.group(1).lower())
        if nat:
            w, h = nat
            if im.width % w or im.height % h or im.width // w != im.height // h:
                problems.append(f"{im.width}x{im.height} is not an integer multiple of native {w}x{h}")
        if problems:
            bad += 1
            print(f"  BAD {p.name}: " + "; ".join(problems))
        else:
            ok += 1
    print(f"validate {pack}: {ok} ok, {bad} bad, {skipped} ignored")
    return 1 if bad else 0


def cmd_sheet(a):
    d = Path(a.dir)
    files = sorted(p for p in d.iterdir() if NAME_RE.match(p.name))
    if not files:
        sys.exit("no <tex_id>[-<pal_id>].png files")
    ims = [Image.open(p).convert("RGBA") for p in files]
    cw = max(im.width for im in ims) + 2
    ch = max(im.height for im in ims) + 2
    cols = a.cols
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cw, rows * ch), (40, 40, 60, 255))
    for i, im in enumerate(ims):
        sheet.alpha_composite(im, ((i % cols) * cw + 1, (i // cols) * ch + 1))
    sheet.convert("RGB").save(a.out)
    print(f"sheet: {len(ims)} images -> {a.out} ({sheet.width}x{sheet.height})")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("summary"); p.add_argument("dump"); p.set_defaults(fn=cmd_summary)
    p = sub.add_parser("starter"); p.add_argument("dump"); p.add_argument("pack")
    p.add_argument("--scale", type=int, default=2); p.add_argument("--palette", default="first")
    p.set_defaults(fn=cmd_starter)
    p = sub.add_parser("coverage"); p.add_argument("pack"); p.add_argument("tsv", nargs="+")
    p.add_argument("--missing", help="write the uncovered texel ids to this TSV"); p.set_defaults(fn=cmd_coverage)
    p = sub.add_parser("validate"); p.add_argument("pack"); p.add_argument("--tsv"); p.set_defaults(fn=cmd_validate)
    p = sub.add_parser("sheet"); p.add_argument("dir"); p.add_argument("out"); p.add_argument("--cols", type=int, default=48)
    p.set_defaults(fn=cmd_sheet)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
