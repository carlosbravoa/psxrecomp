#!/usr/bin/env python3
"""texpack.py — texture-pack authoring helpers (docs/TEXTURE_PACKS.md).

A dump directory (runtime `texture_dump`) holds <tex_id>-<pal_id>.png files
+ textures.tsv; a pack directory holds <tex_id>.png (any palette) and/or
<tex_id>-<pal_id>.png (one palette) at an integer multiple of the native size.

    texpack.py summary  DUMPDIR                        what the dump contains
    texpack.py starter  DUMPDIR PACKDIR --scale N      one <tex_id>.png (+ .clut sidecar) per texel id, plus
                        [--palette common|first|last|all|<pal_id>]  <tex_id>-<pal_id>.png for genuine recolours;
                        [--no-variants]                 N x nearest: the pixel-identical skeleton artists repaint
    texpack.py merge    OUTDUMP DUMP [DUMP...]         union of dumps (first PNG/.clut per pair wins,
                                                      pairs.tsv draw counts summed) -> one dump to build from
    texpack.py coverage PACKDIR TSV [TSV...]           which dumped textures the pack covers
    texpack.py validate PACKDIR [--tsv TSV]            filenames / sizes / alpha sanity
    texpack.py sheet    DIR OUT.png [--cols 48]        contact sheet of a dump or pack
                        [--names TSV] [--group PREFIX] (captioned / filtered by human names)
    texpack.py export   PACKDIR OUTDIR [--names TSV] [--group PREFIX]
                                                      working copies under human names:
                                                      OUTDIR/<name>[-<pal>].png (unnamed -> _unnamed/)
    texpack.py import   OUTDIR PACKDIR [--names TSV]  the repainted copies back to <tex>[-<pal>].png

Names: `names.tsv` (tex_id, name, aliases) written by a game's asset tool
(Mega Man 8: tools/pac_texpack.py — STAGE00/tile0123, PLAYER/strip014_cell02);
merge unions it, starter copies it into the pack, export/import default to
PACKDIR/names.tsv.

Only Pillow is required (`pip install pillow`).
"""

from __future__ import annotations

import argparse
import collections
import csv
import os
import re
import shutil
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
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


def read_clut(path: Path):
    b = path.read_bytes()
    return [int.from_bytes(b[i:i + 2], "little") for i in range(0, len(b), 2)]


def fade_fits(ref, live, tol=0.75):
    """Mirror of texture_pack_palette_mod (runtime/src/texture_pack.c): is `live`
    a uniform fade of `ref` — multiplicative (cur = ref*k) or subtractive
    (cur = clamp(ref - d)) per channel — or a genuine recolour? Keep the model
    in sync with the C. The runtime accepts a fit below 2 levels rms (it then
    APPROXIMATES the colours, better than falling back to native art); the
    starter uses a tighter 0.75 so that palettes the game really uses which are
    not exact fades get their own exact variant and the skeleton stays
    pixel-identical (a true PSX fade fits with rms 0 / <= 0.5)."""
    import math
    idx = [i for i in range(len(ref)) if ref[i] & 0x7FFF]
    if not idx:
        return True
    err_mul = err_sub = 0.0
    m = 0
    for k in range(3):
        rv = [(ref[i] >> (5 * k)) & 31 for i in idx]
        cv = [(live[i] >> (5 * k)) & 31 for i in idx]
        pairs = [(r, c) for r, c in zip(rv, cv) if r > 0]
        if not pairs:
            continue
        sr = sum(r for r, _ in pairs)
        sc = sum(c for _, c in pairs)
        unc = [(r - c) for r, c in pairs if 0 < c < 31]
        mul = sc / sr if sr else 1.0
        sub = sum(unc) / len(unc) if unc else (31.0 if sc / len(pairs) <= 0.5 else -31.0)
        for r, c in pairs:
            err_mul += (c - min(31.0, r * mul)) ** 2
            err_sub += (c - min(31.0, max(0.0, r - sub))) ** 2
            m += 1
    if not m:
        return True
    return min(math.sqrt(err_mul / m), math.sqrt(err_sub / m)) < tol


def cmd_starter(a):
    d, out = Path(a.dump), Path(a.pack)
    rows = read_tsv(d / "textures.tsv")
    out.mkdir(parents=True, exist_ok=True)
    by_tex = collections.defaultdict(list)
    for r in rows:
        by_tex[r["tex_id"]].append(r)
    n = nvar = 0
    # pairs.tsv (written by texture_dump on stats/disarm) counts how often each
    # (texel, palette) pair was DRAWN: the most-drawn palette is the settled one
    draws = {}
    pairs = d / "pairs.tsv"
    if pairs.exists():
        for r in read_tsv(pairs):
            draws[(r["tex_id"].lower(), r["pal_id"].lower())] = int(r["draws"])
    elif a.palette == "common":
        print("note: no pairs.tsv in the dump (end the dump with texture_dump op=stats/disarm); using --palette first", file=sys.stderr)
    for tex, rs in by_tex.items():
        if a.palette == "common":
            best = max(rs, key=lambda r: draws.get((r["tex_id"].lower(), r["pal_id"].lower()), 0))
            picks = [(best, None)]
            # palettes that are NOT a fade of the common one are genuine recolours
            # (enemy variants, the same tile in another stage): keep them as
            # <tex>-<pal>.png variants with their own .clut, so the runtime shows
            # (and fades) the right colours instead of the common ones
            bc = d / f"{best['tex_id']}-{best['pal_id']}.clut"
            if bc.exists() and not a.no_variants:
                ref = read_clut(bc)
                for r in rs:
                    if r is best:
                        continue
                    c = d / f"{r['tex_id']}-{r['pal_id']}.clut"
                    if c.exists() and not fade_fits(ref, read_clut(c)):
                        picks.append((r, r["pal_id"]))
                        nvar += 1
        elif a.palette == "first":
            picks = [(rs[0], None)]
        elif a.palette == "last":
            # the palette seen LAST is the settled one after a fade-in (fades step
            # through many palettes first) — the right reference for fade-aware packs
            picks = [(max(rs, key=lambda r: int(r["first_frame"])), None)]
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
            # every entry carries the CLUT it was authored against so the runtime
            # can dim/flash it with the live palette and pick the variant a fade
            # belongs to (docs/TEXTURE_PACKS.md, "Palettes and fades")
            clut = d / f"{r['tex_id']}-{r['pal_id']}.clut"
            if clut.exists():
                (out / (name[:-4] + ".clut")).write_bytes(clut.read_bytes())
    if (d / "names.tsv").exists():
        shutil.copyfile(d / "names.tsv", out / "names.tsv")
    print(f"starter pack: {n} images at {a.scale}x -> {out}" + (f" ({nvar} recolour variants)" if nvar else ""))
    return 0


def read_names(path):
    """tex_id (lower) -> name; aliases ignored (first name wins)."""
    out = {}
    if not path or not Path(path).exists():
        return out
    for r in read_tsv(Path(path)):
        out.setdefault(r["tex_id"].lower(), r["name"])
    return out


def cmd_export(a):
    pack, out = Path(a.pack), Path(a.out)
    names = read_names(a.names or (pack / "names.tsv"))
    if not names:
        sys.exit("no names (pass --names or put names.tsv in the pack)")
    idx = pack_index(pack)
    n = un = 0
    for tex, variants in idx.items():
        name = names.get(tex)
        if a.group and not (name or "").startswith(a.group):
            continue
        for pal, src in variants.items():
            if name:
                rel = Path(name + (f"-{pal}" if pal else "") + ".png")
            else:
                rel = Path("_unnamed") / src.name
                un += 1
            dst = out / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if not dst.exists():
                try:
                    os.link(src, dst)
                except OSError:
                    shutil.copyfile(src, dst)
            n += 1
    with open(out / "index.tsv", "w") as f:
        f.write("name\ttex_id\n")
        for tex, name in sorted(names.items(), key=lambda kv: kv[1]):
            if tex in idx and (not a.group or name.startswith(a.group)):
                f.write(f"{name}\t{tex}\n")
    print(f"exported {n} images ({un} unnamed) -> {out}; index.tsv maps names back")
    return 0


def cmd_import(a):
    src_root, pack = Path(a.src), Path(a.pack)
    names = read_names(a.names or (pack / "names.tsv"))
    by_name = {v: k for k, v in names.items()}
    idx_file = src_root / "index.tsv"
    if idx_file.exists():
        for r in read_tsv(idx_file):
            by_name.setdefault(r["name"], r["tex_id"].lower())
    n = bad = 0
    for p in src_root.rglob("*.png"):
        rel = p.relative_to(src_root).with_suffix("")
        parts = rel.as_posix()
        if parts.startswith("_unnamed/"):
            m = NAME_RE.match(p.name)
            if not m:
                continue
            dst = pack / p.name
        else:
            pal = None
            m = re.match(r"^(.*)-([0-9a-fA-F]{16})$", parts)
            if m:
                parts, pal = m.group(1), m.group(2).lower()
            tex = by_name.get(parts)
            if not tex:
                print(f"  no texel id for {parts} (not in names / index) — skipped", file=sys.stderr)
                bad += 1
                continue
            dst = pack / (f"{tex}-{pal}.png" if pal else f"{tex}.png")
        if dst.exists() and os.path.samefile(p, dst):
            continue
        shutil.copyfile(p, dst)
        n += 1
    print(f"imported {n} images into {pack}" + (f", {bad} unresolved" if bad else ""))
    return 1 if bad else 0


def cmd_merge(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    seen: dict = {}
    draws = collections.Counter()
    merged_names: dict = {}
    header = None
    rows_out = []
    for d in a.dumps:
        d = Path(d)
        tsv = d / "textures.tsv"
        if not tsv.exists():
            print(f"skip {d}: no textures.tsv", file=sys.stderr)
            continue
        with open(tsv, newline="") as f:
            rd = csv.DictReader(f, delimiter="\t")
            header = header or rd.fieldnames
            n_new = 0
            for r in rd:
                key = (r["tex_id"].lower(), r["pal_id"].lower())
                if key in seen:
                    continue
                src = d / f"{r['tex_id']}-{r['pal_id']}.png"
                if not src.exists():
                    continue
                seen[key] = d
                rows_out.append(r)
                n_new += 1
                dst = out / src.name
                if not dst.exists():
                    try:
                        os.link(src, dst)
                    except OSError:
                        shutil.copyfile(src, dst)
                clut = src.with_suffix(".clut")
                if clut.exists() and not (out / clut.name).exists():
                    try:
                        os.link(clut, out / clut.name)
                    except OSError:
                        shutil.copyfile(clut, out / clut.name)
        pairs = d / "pairs.tsv"
        if pairs.exists():
            for r in read_tsv(pairs):
                draws[(r["tex_id"].lower(), r["pal_id"].lower())] += int(r["draws"])
        nm = d / "names.tsv"
        if nm.exists():
            for r in read_tsv(nm):
                merged_names.setdefault(r["tex_id"].lower(), (r["name"], r.get("aliases", "")))
        print(f"{d}: {n_new} new pairs")
    with open(out / "textures.tsv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=header or ["tex_id", "pal_id", "w", "h", "bpp", "texpage_x", "texpage_y", "clut_x", "clut_y", "u", "v", "first_frame"], delimiter="\t")
        w.writeheader()
        for r in rows_out:
            w.writerow(r)
    with open(out / "pairs.tsv", "w") as f:
        f.write("tex_id\tpal_id\tdraws\n")
        for (t, p), n in draws.items():
            f.write(f"{t}\t{p}\t{n}\n")
    if merged_names:
        with open(out / "names.tsv", "w") as f:
            f.write("tex_id\tname\taliases\n")
            for t, (nm, al) in merged_names.items():
                f.write(f"{t}\t{nm}\t{al}\n")
    print(f"merged: {len(rows_out)} pairs, {len({t for t, _ in seen})} texel ids -> {out}" + (f", {len(merged_names)} named" if merged_names else ""))
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
        if p.suffix.lower() == ".clut":
            sz = p.stat().st_size
            if sz not in (32, 512) or not re.match(r"^[0-9a-fA-F]{16}(-[0-9a-fA-F]{16})?\.clut$", p.name):
                print(f"  BAD {p.name}: a .clut sidecar must be <tex_id>[-<pal_id>].clut of 32 (4bpp) or 512 (8bpp) bytes")
                bad += 1
            continue
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
    names = read_names(a.names or (d / "names.tsv"))
    files = sorted(p for p in d.iterdir() if NAME_RE.match(p.name))
    if names:
        files.sort(key=lambda p: names.get(NAME_RE.match(p.name).group(1).lower(), "~" + p.name))
    if a.group:
        files = [p for p in files if names.get(NAME_RE.match(p.name).group(1).lower(), "").startswith(a.group)]
    if not files:
        sys.exit("no <tex_id>[-<pal_id>].png files" + (f" in group {a.group}" if a.group else ""))
    ims = [Image.open(p).convert("RGBA") for p in files]
    cap = 10 if names else 0                       # caption row height
    caps = [names.get(NAME_RE.match(p.name).group(1).lower(), "").split("/")[-1] for p in files] if names else []
    cw = max(im.width for im in ims) + 2
    if caps:
        cw = max(cw, 6 * max(len(c) for c in caps) + 2)   # default bitmap font ~6 px per char
    ch = max(im.height for im in ims) + 2 + cap
    cols = a.cols
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cw, rows * ch), (40, 40, 60, 255))
    draw = ImageDraw.Draw(sheet) if names else None
    for i, (im, p) in enumerate(zip(ims, files)):
        x, y = (i % cols) * cw + 1, (i // cols) * ch + 1
        sheet.alpha_composite(im, (x, y))
        if draw:
            draw.text((x, y + im.height), caps[i], fill=(200, 200, 200, 255))
    sheet.convert("RGB").save(a.out)
    print(f"sheet: {len(ims)} images -> {a.out} ({sheet.width}x{sheet.height})")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("summary"); p.add_argument("dump"); p.set_defaults(fn=cmd_summary)
    p = sub.add_parser("starter"); p.add_argument("dump"); p.add_argument("pack")
    p.add_argument("--scale", type=int, default=2)
    p.add_argument("--palette", default="common", help="common (most-drawn palette + genuine recolour variants, needs pairs.tsv; default) | first | last | all | <pal_id>")
    p.add_argument("--no-variants", action="store_true", help="with --palette common: do not emit <tex>-<pal>.png recolour variants")
    p.set_defaults(fn=cmd_starter)
    p = sub.add_parser("merge"); p.add_argument("out"); p.add_argument("dumps", nargs="+"); p.set_defaults(fn=cmd_merge)
    p = sub.add_parser("coverage"); p.add_argument("pack"); p.add_argument("tsv", nargs="+")
    p.add_argument("--missing", help="write the uncovered texel ids to this TSV"); p.set_defaults(fn=cmd_coverage)
    p = sub.add_parser("validate"); p.add_argument("pack"); p.add_argument("--tsv"); p.set_defaults(fn=cmd_validate)
    p = sub.add_parser("sheet"); p.add_argument("dir"); p.add_argument("out"); p.add_argument("--cols", type=int, default=48)
    p.add_argument("--names"); p.add_argument("--group", help="only names starting with this prefix (e.g. STAGE00/ or PLAYER/)")
    p.set_defaults(fn=cmd_sheet)
    p = sub.add_parser("export"); p.add_argument("pack"); p.add_argument("out"); p.add_argument("--names"); p.add_argument("--group")
    p.set_defaults(fn=cmd_export)
    p = sub.add_parser("import"); p.add_argument("src"); p.add_argument("pack"); p.add_argument("--names")
    p.set_defaults(fn=cmd_import)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
