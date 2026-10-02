#!/usr/bin/env python3
"""Turns the raw output of bench/run_all.sh into tables and charts.

    python3 bench/report.py bench/results/<date>

Reads raw.jsonl and machine.txt from that directory and writes report.md and
one SVG per chart next to them. Standard library only.
"""
import json
import math
import statistics
import sys
from html import escape
from pathlib import Path

# --- Drawing. Colors are the roles of one validated palette, light and dark.

STYLE = """<style>
svg{--surface:#fcfcfb;--t1:#0b0b0b;--t2:#52514e;--mu:#898781;--grid:#e1e0d9;--axis:#c3c2b7;
--s1:#2a78d6;--s2:#eb6834;--ctx:#c3c2b7;
--o1:#86b6ef;--o2:#5598e7;--o3:#2a78d6;--o4:#1c5cab;--o5:#0d366b}
@media (prefers-color-scheme: dark){svg{--surface:#1a1a19;--t1:#ffffff;--t2:#c3c2b7;
--grid:#2c2c2a;--axis:#383835;--s1:#3987e5;--s2:#d95926;--ctx:#52514e;
--o1:#184f95;--o2:#256abf;--o3:#5598e7;--o4:#86b6ef;--o5:#cde2fb}}
text{font-family:system-ui,-apple-system,"Segoe UI",sans-serif}
.surface{fill:var(--surface)} .t1{fill:var(--t1)} .t2{fill:var(--t2)} .mu{fill:var(--mu)}
.grid{stroke:var(--grid)} .axis{stroke:var(--axis)}
</style>"""


def text(x, y, s, cls="t2", anchor="start", size=12, weight=400):
    return (f'<text class="{cls}" x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" '
            f'font-size="{size}" font-weight="{weight}">{escape(str(s))}</text>')


def fmt(v):
    """Three significant figures: 345, 10.2k, 14.4M."""
    for limit, suffix in ((1e6, "M"), (1e3, "k"), (1, "")):
        if abs(v) >= limit or not suffix:
            x = v / limit
            s = f"{x:.0f}" if abs(x) >= 100 else f"{x:.1f}" if abs(x) >= 10 else f"{x:.2f}"
            return (s.rstrip("0").rstrip(".") if "." in s else s) + suffix


def nice(top, n=4):
    """A round tick step and axis maximum covering 0..top in about n steps."""
    raw = (top if top > 0 else 1) / n
    mag = 10 ** math.floor(math.log10(raw))
    step = next(m * mag for m in (1, 2, 2.5, 5, 10) if m * mag >= raw * 0.999)
    return step, math.ceil(top / step - 1e-9) * step


def y_axis(x0, x1, bottom, top, top_value, yfmt):
    step, hi = nice(top_value)
    scale = lambda v: bottom - (bottom - top) * v / hi
    out = []
    for i in range(round(hi / step) + 1):
        y = scale(i * step)
        out.append(f'<line class="{"axis" if i == 0 else "grid"}" x1="{x0}" x2="{x1}" '
                   f'y1="{y:.1f}" y2="{y:.1f}"/>')
        out.append(text(x0 - 8, y + 4, yfmt(i * step), "mu", "end"))
    return "".join(out), scale


def legend(x, y, entries):
    out = []
    for var, name in entries:
        out.append(f'<rect x="{x}" y="{y - 9}" width="10" height="10" rx="2" fill="var(--{var})"/>')
        out.append(text(x + 16, y, name))
        x += 16 + len(name) * 6.6 + 18
    return "".join(out)


def save(path, w, h, title, subtitle, body):
    path.write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
        f'role="img" aria-label="{escape(title)}">{STYLE}'
        f'<rect class="surface" width="{w}" height="{h}" rx="8"/>'
        + text(24, 32, title, "t1", size=15, weight=600) + text(24, 51, subtitle)
        + "".join(body) + "</svg>\n")


def line_chart(path, title, subtitle, series, xticks, xlabel, yfmt=fmt, marks=(), w=760, h=340):
    """series: [(name, color var, [(x, y), ...])]. marks: [(x, label)] vertical rules."""
    left, right, top, bottom = 64, w - 150, 96, h - 52
    xs = [x for _, _, pts in series for x, _ in pts]
    sx = lambda x: left + (right - left) * (x - min(xs)) / ((max(xs) - min(xs)) or 1)
    grid, sy = y_axis(left, right, bottom, top, max(y for _, _, pts in series for _, y in pts), yfmt)
    body = [grid]
    for x, label in marks:
        body.append(f'<line class="grid" x1="{sx(x):.1f}" x2="{sx(x):.1f}" y1="{top}" y2="{bottom}"/>')
        body.append(text(sx(x), top - 8, label, "mu"))
    for x, label in xticks:
        body.append(text(sx(x), bottom + 18, label, "mu", "middle"))
    body.append(text((left + right) / 2, bottom + 40, xlabel, "t2", "middle"))
    for name, var, pts in series:
        d = "".join(f'{"L" if i else "M"}{sx(x):.1f},{sy(y):.1f}' for i, (x, y) in enumerate(pts))
        if len(series) == 1:
            body.append(f'<path d="{d}L{sx(pts[-1][0]):.1f},{bottom}L{sx(pts[0][0]):.1f},{bottom}Z" '
                        f'fill="var(--{var})" opacity="0.1"/>')
        body.append(f'<path d="{d}" fill="none" stroke="var(--{var})" stroke-width="2" '
                    f'stroke-linejoin="round" stroke-linecap="round"/>')
        if len(pts) <= 16:
            for x, y in pts:
                body.append(f'<circle cx="{sx(x):.1f}" cy="{sy(y):.1f}" r="4" fill="var(--{var})" '
                            f'stroke="var(--surface)" stroke-width="2"><title>{escape(name)}: '
                            f'{yfmt(y)}</title></circle>')
        x, y = pts[-1]
        end = yfmt(y) if len(series) == 1 else f"{name}  {yfmt(y)}"
        body.append(text(sx(x) + 10, sy(y) + 4, end))
    if len(series) >= 2:
        body.append(legend(left, 76, [(var, name) for name, var, _ in series]))
    save(path, w, h, title, subtitle, body)


def column_chart(path, title, subtitle, panels, colors, names, xlabel, yfmt=fmt, cap=fmt,
                 w=760, h=374):
    """panels: [(panel title, [(column label, [segment values, bottom first])])]."""
    left, right, top, bottom, gutter = 56, w - 24, 126, h - 52, 16
    grid, sy = y_axis(left, right, bottom, top,
                      max(sum(segs) for _, cols in panels for _, segs in cols), yfmt)
    body = [grid, text((left + right) / 2, bottom + 40, xlabel, "t2", "middle")]
    if len(names) >= 2:
        body.append(legend(left, 76, list(zip(colors, names))))
    panel_w = (right - left) / len(panels)
    for p, (panel_title, cols) in enumerate(panels):
        px = left + p * panel_w
        body.append(text(px + panel_w / 2, top - 26, panel_title, "t2", "middle", weight=600))
        slot = (panel_w - 2 * gutter) / len(cols)
        bw = min(24, slot * 0.6)
        for c, (label, segs) in enumerate(cols):
            x = px + gutter + slot * (c + 0.5) - bw / 2
            body.append(text(x + bw / 2, bottom + 18, label, "mu", "middle"))
            last = max((i for i, v in enumerate(segs) if v > 0), default=-1)
            base = 0
            for i, v in enumerate(segs):
                y0, y1 = sy(base + v), sy(base) - (2 if base > 0 else 0)  # 2px gap between segments
                base += v
                if y1 - y0 < 0.5:
                    continue
                r = min(4, y1 - y0) if i == last else 0  # round only the data end
                tip = f"{label}, {names[i]}: {yfmt(v)}" if len(names) > 1 else f"{label}: {cap(v)}"
                body.append(f'<path d="M{x:.1f},{y1:.1f}V{y0 + r:.1f}Q{x:.1f},{y0:.1f} {x + r:.1f},'
                            f'{y0:.1f}H{x + bw - r:.1f}Q{x + bw:.1f},{y0:.1f} {x + bw:.1f},'
                            f'{y0 + r:.1f}V{y1:.1f}Z" fill="var(--{colors[i]})">'
                            f'<title>{escape(tip)}</title></path>')
            if base > 0 and cap(base):
                body.append(text(x + bw / 2, sy(base) - 6, cap(base), "t2", "middle", size=11))
    save(path, w, h, title, subtitle, body)


def bar_panels(path, title, subtitle, panels, w=760):
    """Small multiples, each with its own scale: [(panel title, [(label, value, emphasised)])]."""
    cols, label_w, row_h = 3, 70, 20
    panel_w = (w - 48) / cols
    panel_h = 34 + row_h * max(len(bars) for _, bars in panels) + 22
    body = []
    for i, (panel_title, bars) in enumerate(panels):
        px, py = 24 + (i % cols) * panel_w, 76 + (i // cols) * panel_h
        body.append(text(px, py + 12, panel_title, "t1", weight=600))
        x0, room = px + label_w, panel_w - label_w - 62
        top_value = max(v for _, v, _ in bars)
        for j, (label, v, emphasised) in enumerate(bars):
            y, length = py + 24 + j * row_h, max(room * v / top_value, 1)
            r = min(4, length)
            body.append(text(x0 - 8, y + 11, label, "t2", "end"))
            body.append(f'<path d="M{x0:.1f},{y}H{x0 + length - r:.1f}Q{x0 + length:.1f},{y} '
                        f'{x0 + length:.1f},{y + r}V{y + 12 - r}Q{x0 + length:.1f},{y + 12} '
                        f'{x0 + length - r:.1f},{y + 12}H{x0:.1f}Z" '
                        f'fill="var(--{"s1" if emphasised else "ctx"})">'
                        f'<title>{escape(panel_title)}, {escape(label)}: {v:,.0f}</title></path>')
            body.append(text(x0 + length + 6, y + 11, fmt(v), "t2", size=11))
        body.append(f'<line class="axis" x1="{x0:.1f}" x2="{x0:.1f}" y1="{py + 20}" '
                    f'y2="{py + 24 + len(bars) * row_h - 4}"/>')
    rows = math.ceil(len(panels) / cols)
    save(path, w, 76 + rows * panel_h, title, subtitle, body)


# --- The report.

ENGINES = [("lsm", "lsm"), ("leveldb", "LevelDB"), ("leveldb-noverify", "LevelDB*"),
           ("rocksdb", "RocksDB")]
COMPARISON = ["fillseq", "fillrandom", "overwrite", "fillsync", "readrandom", "readmissing",
              "readseq", "readreverse", "seekrandom", "readwhilewriting"]


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join(["---"] + ["---:"] * (len(header) - 1)) + "|"]
    return "\n".join(out + ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]) + "\n"


def histogram(s):
    return [int(part.split(":")[1]) for part in s.split()]


def main(out):
    rows = [json.loads(line) for line in (out / "raw.jsonl").read_text().splitlines() if line]
    pick = lambda label, bench: [r for r in rows if r["label"] == label and r["benchmark"] == bench]
    engines = [(label, name) for label, name in ENGINES if pick(label, "fillrandom")]
    md = ["# Benchmark results\n", "```\n" + (out / "machine.txt").read_text() + "```\n"]

    def cell(label, bench, field, scale):
        values = [r[field] / scale for r in pick(label, bench)]
        if not values:
            return "–"
        spread = f" ({fmt(min(values))}–{fmt(max(values))})" if len(values) > 1 else ""
        # Runs that disagree by more than 2x were not measuring the same thing.
        middle = statistics.median(values)
        unstable = " †" if middle > 0 and (max(values) > 2 * middle or min(values) < middle / 2) else ""
        return fmt(middle) + spread + unstable

    def comparison(title, suffix, benches, field="ops_per_sec", scale=1):
        md.append(f"## {title}\n")
        md.append(table(["benchmark"] + [name for _, name in engines],
                        [[b] + [cell(label + suffix, b, field, scale) for label, _ in engines]
                         for b in benches]))

    note = ("Median of the runs, with the lowest and highest in brackets. "
            "LevelDB* is LevelDB with its default of not verifying block checksums on reads. "
            "† marks a cell whose runs disagree by more than 2×: something else was using the "
            "machine during at least one of them, and the cell should not be used to compare "
            "engines.\n")
    comparison("Throughput, operations per second", "", COMPARISON)
    md.append(note)
    comparison("Median latency, microseconds", "", COMPARISON, "p50_us")
    comparison("99th percentile latency, microseconds", "", COMPARISON, "p99_us")
    comparison("99.9th percentile latency, microseconds", "", COMPARISON, "p999_us")
    comparison("Megabytes the process wrote", "", COMPARISON, "bytes_written", 1e6)
    md.append("Everything passed to a write call while the benchmark ran: log, flushes, and "
              "compactions. Compaction still running when a benchmark ends is not counted.\n")
    comparison("Megabytes read from the storage device", "", COMPARISON, "bytes_read", 1e6)
    md.append("Reads the page cache could not serve. The page cache was not dropped between "
              "benchmarks, so these are warm-cache numbers.\n")
    md.append("## Seconds lsm's writers spent stalled\n")
    md.append(table(["benchmark", "lsm"], [[b, cell("lsm", b, "stall_micros", 1e6)]
                                           for b in ("fillseq", "fillrandom", "overwrite")]))
    md.append("Included in the throughput above. The other engines do not report theirs "
              "through this harness.\n")
    comparison("YCSB workloads, operations per second", "-ycsb", [f"ycsb{c}" for c in "abcdef"])
    comparison("4 KiB values, operations per second", "-large", ["fillrandom", "readrandom"])

    panels = []
    for bench in COMPARISON[:9]:
        bars = [(name, statistics.median(r["ops_per_sec"] for r in pick(label, bench)), label == "lsm")
                for label, name in engines if pick(label, bench)]
        panels.append((bench, bars))
    bar_panels(out / "comparison.svg", "Throughput by engine, operations per second",
               "Median of the runs. Each panel has its own scale. LevelDB* reads without verifying checksums.",
               panels)
    md.append("![Throughput by engine](comparison.svg)\n")

    # Group commit.
    threads = sorted({r["threads"] for r in pick("groupcommit-1", "fillrandom")})
    if threads:
        rate = lambda gc, t: next(r["ops_per_sec"] for r in pick(f"groupcommit-{gc}", "fillrandom")
                                  if r["threads"] == t)
        md.append("## Group commit: synced writes per second\n")
        md.append(table(["threads", "group commit", "one sync per write", "ratio"],
                        [[t, fmt(rate(1, t)), fmt(rate(0, t)), f"{rate(1, t) / rate(0, t):.1f}×"]
                         for t in threads]))
        line_chart(out / "group_commit.svg", "Synced writes per second, by writer threads",
                   "Every write waits for its sync. With group commit, waiting writers share one.",
                   [("group commit", "s1", [(i, rate(1, t)) for i, t in enumerate(threads)]),
                    ("one sync per write", "s2", [(i, rate(0, t)) for i, t in enumerate(threads)])],
                   [(i, str(t)) for i, t in enumerate(threads)], "writer threads")
        md.append("![Group commit](group_commit.svg)\n")

    # Write amplification by level multiplier: where each rewritten byte went.
    names = ["log + flush", "into level 1", "into level 2", "into level 3", "into level 4 and deeper"]
    panels, table_rows, skipped = [], [], []
    for pace, panel_title in ((0, "writing flat out"), (8000, "paced, never stalled")):
        cols = []
        for r in sorted(pick(f"wamp-pace{pace}", "settled"), key=lambda r: r["level_size_multiplier"]):
            # A paced run whose writer stalled was not paced (the machine was
            # suspended, say, and the writer then ran flat out to catch up).
            if pace and any(w["stall_micros"] for w in pick(r["label"], "overwrite")
                            if w["level_size_multiplier"] == r["level_size_multiplier"]):
                skipped.append(str(r["level_size_multiplier"]))
                continue
            user = r["user_bytes_written"]
            levels = [line.split() for line in r["level_stats"].split("|")[1:] if line]
            written = {int(l[0]): int(l[6]) / user for l in levels}
            segs = [r["wal_bytes_written"] / user + written.get(0, 0), written.get(1, 0),
                    written.get(2, 0), written.get(3, 0),
                    sum(v for level, v in written.items() if level >= 4)]
            cols.append((str(r["level_size_multiplier"]), segs))
            table_rows.append([f'{r["level_size_multiplier"]}, {panel_title}',
                               sum(1 for l in levels if int(l[1]) > 0)]
                              + [f"{v:.2f}" for v in segs] + [f"{sum(segs):.2f}"])
        if cols:
            panels.append((panel_title, cols))
    if panels:
        md.append("## Write amplification by level size multiplier\n")
        md.append("Bytes written per byte of user data while overwriting a populated database.\n")
        md.append(table(["multiplier", "levels in use"] + names + ["total"], table_rows))
        if skipped:
            md.append(f"The paced run at multiplier {', '.join(skipped)} is left out: its writer "
                      "stalled, so it was not paced.\n")
        column_chart(out / "write_amp.svg", "Write amplification by level size multiplier",
                     "Bytes written per user byte while overwriting a full database, split by where they went.",
                     panels, ["o1", "o2", "o3", "o4", "o5"], names, "level size multiplier",
                     cap=lambda v: f"{v:.1f}")
        md.append("![Write amplification](write_amp.svg)\n")

    # Space amplification over time.
    space = [r for r in rows if r["label"] == "space" and r["space_series"]]
    if space:
        live = space[0]["user_bytes"]
        points, marks, offset, suspended = [], [], 0.0, 0.0
        for i, r in enumerate(space):
            marks.append((offset, "fill" if i == 0 else f"overwrite {i}"))
            previous = 0.0
            for t, size in r["space_series"]:
                # Samples are 0.25 s apart. A gap of seconds means the machine
                # was suspended; that time is taken out of the axis.
                if t - previous > 2:
                    suspended += t - previous - 0.25
                    offset -= t - previous - 0.25
                points.append((offset + t, size / live))
                previous = t
            offset = points[-1][0]
        step, hi = nice(offset)
        line_chart(out / "space_amp.svg", "Space amplification over time",
                   "Size of the database directory over the size of the live data, while everything is overwritten three times.",
                   [("space amplification", "s1", points)],
                   [(i * step, f"{i * step:g} s") for i in range(round(hi / step) + 1) if i * step <= offset],
                   "seconds since the start", yfmt=lambda v: f"{v:.1f}×", marks=marks)
        md.append("## Space amplification\n")
        md.append(f"Live data: {live / 1e6:.0f} MB. Largest directory size seen: "
                  f"{max(y for _, y in points):.2f}× that; at the end: {points[-1][1]:.2f}×.\n")
        if suspended:
            md.append(f"The machine was suspended for {suspended:,.0f} seconds during this run; "
                      "that time is removed from the chart.\n")
        md.append("![Space amplification](space_amp.svg)\n")

    # Table files consulted per Get.
    panels, table_rows = [], []
    for bench, panel_title in (("readrandom", "keys that exist"), ("readmissing", "keys that do not")):
        runs = [r for r in pick("lsm", bench) if r["files_probed_after"]]
        if not runs:
            continue
        counts = [sum(histogram(r["files_probed_after"])[n] - histogram(r["files_probed_before"])[n]
                      for r in runs) for n in range(8)]
        share = [100 * c / sum(counts) for c in counts]
        labels = [str(n) for n in range(7)] + ["7+"]
        panels.append((panel_title, [(label, [s]) for label, s in zip(labels, share)]))
        table_rows.append([panel_title] + [f"{s:.1f}%" for s in share])
    if panels:
        md.append("## Table files consulted per Get\n")
        md.append("A table is consulted when its key range covers the key. Its bloom filter is "
                  "checked first, and a data block is read only if the filter says the key may "
                  "be there.\n")
        md.append(table(["reads of"] + [str(n) for n in range(7)] + ["7+"], table_rows))
        column_chart(out / "files_probed.svg", "Table files consulted per Get",
                     "Share of Gets by how many tables' key ranges covered the key. Each is asked; its bloom filter answers first.",
                     panels, ["s1"], ["share of Gets"], "table files consulted",
                     yfmt=lambda v: f"{v:g}%", cap=lambda v: f"{v:.0f}%" if v >= 0.5 else "")
        md.append("![Files consulted per Get](files_probed.svg)\n")

    (out / "report.md").write_text("\n".join(md))
    print(f"wrote {out / 'report.md'}")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
