"""Synthèse des CSV du banc : tableau par série et figure charge/overruns vs nombre de sources.

    python tools/perf/scripts/analyze.py results/20261012_1000/*.csv
"""
import csv
import glob
import sys
from collections import defaultdict
from pathlib import Path


def load(paths):
    rows = []
    for p in paths:
        for f in glob.glob(p):
            with open(f, newline="", encoding="utf-8") as fh:
                rows += list(csv.DictReader(fh))
    return rows


def num(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError):
        return 0.0


def main():
    rows = load(sys.argv[1:])
    if not rows:
        sys.exit("Aucune ligne CSV.")

    agg = defaultdict(lambda: defaultdict(list))  # label -> N -> rows
    for r in rows:
        agg[r["label"]][int(num(r, "n_sources"))].append(r)

    sec_names = [k[4:-8] for k in rows[0] if k.startswith("sec_") and k.endswith("_mean_us")]
    print(f"{'série':28} {'N':>3} {'bloc':>5} {'charge%':>8} {'pic%':>7} {'overr.':>7} {'dénorm':>7} "
          f"{'jitter':>7} {'migr':>5} {'log':>5}  section dominante")
    summary = {}
    for label in sorted(agg):
        first_glitch = None
        for n in sorted(agg[label]):
            rs = agg[label][n]
            budget = max(num(rs[0], "budget_us"), 1e-9)
            load_ = sum(num(r, "mean_us") for r in rs) / len(rs) / budget * 100
            peak = max(num(r, "max_us") for r in rs) / budget * 100
            over = sum(num(r, "overruns") for r in rs)
            den = sum(num(r, "denormal_hits") for r in rs)
            jit = max(num(r, "max_jitter_us") for r in rs)
            mig = sum(num(r, "cpu_migrations") for r in rs)
            logs = sum(num(r, "log_audio") for r in rs)
            secs = {s: sum(num(r, f"sec_{s}_mean_us") for r in rs) / len(rs) for s in sec_names}
            dom = max(secs, key=secs.get) if secs and max(secs.values()) > 0 else "-"
            print(f"{label:28} {n:>3} {int(num(rs[0], 'block_size')):>5} {load_:>8.1f} {peak:>7.1f} {int(over):>7} "
                  f"{int(den):>7} {jit:>7.0f} {int(mig):>5} {int(logs):>5}  {dom} ({secs.get(dom, 0):.0f} µs)")
            summary.setdefault(label, []).append((n, load_, peak, over))
            if over > 0 and first_glitch is None:
                first_glitch = n
        print(f"{'':28} → premier palier avec overruns : {first_glitch}\n")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib absent : pas de figure.")
        return

    fig, ax = plt.subplots(1, 2, figsize=(13, 5))
    for label, pts in summary.items():
        pts.sort()
        ns = [p[0] for p in pts]
        ax[0].plot(ns, [p[1] for p in pts], marker="o", label=label)
        ax[1].plot(ns, [p[2] for p in pts], marker="o", label=label)
    for a, t in zip(ax, ["Charge moyenne du callback (% du budget)", "Pic de charge (% du budget)"]):
        a.axhline(100, color="red", lw=1, ls="--")
        a.set_xlabel("Nombre de sources")
        a.set_title(t)
        a.grid(alpha=0.3)
    ax[1].legend(fontsize=7)
    out = Path(glob.glob(sys.argv[1])[0]).parent / "synthese.png"
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"Figure : {out}")


if __name__ == "__main__":
    main()
