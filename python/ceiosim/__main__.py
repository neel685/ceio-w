"""python -m ceiosim: regenerate the tables and figures of docs/results.md.

  python -m ceiosim --list
  python -m ceiosim                      # everything, full sweeps
  python -m ceiosim --quick              # reduced sweeps (about a minute)
  python -m ceiosim --only footprint,flow_churn --jobs 8
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

from .experiments import CATALOGUE
from .runner import DEFAULT_BINARY, DEFAULT_CACHE, REPO, Runner, SimulationError


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ceiosim", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--list", action="store_true", help="list experiments and exit")
    p.add_argument("--only", default="", help="comma-separated experiment names")
    p.add_argument("--quick", action="store_true", help="reduced sweeps (minutes instead of tens of minutes)")
    p.add_argument("--jobs", type=int, default=None, help="parallel simulations (default: cores - 1)")
    p.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    p.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    p.add_argument("--out", type=Path, default=None,
                   help="tables + RESULTS.md (default docs/generated, or docs/generated/quick with --quick)")
    p.add_argument("--figures", type=Path, default=None, help="figures (default docs/figures, or <out>/figures with --quick)")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args(argv)

    # Quick sweeps never overwrite the published (full-sweep) tables and figures.
    if a.out is None:
        a.out = REPO / "docs" / "generated" / ("quick" if a.quick else "")
    if a.figures is None:
        a.figures = a.out / "figures" if a.quick else REPO / "docs" / "figures"
    if a.list:
        for name, e in CATALOGUE.items():
            print(f"{name:20s} {e.title}")
        return 0
    names = [n for n in a.only.split(",") if n] or list(CATALOGUE)
    unknown = [n for n in names if n not in CATALOGUE]
    if unknown:
        p.error(f"unknown experiment(s): {', '.join(unknown)} (see --list)")

    try:
        runner = Runner(a.binary, a.cache, a.jobs, a.verbose)
    except FileNotFoundError as e:
        print(e, file=sys.stderr)
        return 2

    sections = ["# Generated results", "",
                "Made by `python -m ceiosim" + (" --quick" if a.quick else "") + "`, so please don't edit it by hand."
                " The discussion of these numbers is in docs/results.md.", ""]
    t0 = time.time()
    for name in names:
        e = CATALOGUE[name]
        runs = e.runs(a.quick)
        print(f"[{name}] {len(runs)} simulations ...", flush=True)
        try:
            results = runner.run_all(runs)
        except SimulationError as err:
            print(err, file=sys.stderr)
            return 1
        tables = e.build(results, a.quick, a.figures)
        sections.append(f"## {e.title}\n")
        for t in tables:
            t.write(a.out / "tables")
            sections.append(t.markdown())
        fig = a.figures / f"{name}.png"
        if fig.exists():
            sections.append(f"![{e.title}]({os.path.relpath(fig, a.out)})\n")
    a.out.mkdir(parents=True, exist_ok=True)
    target = a.out / "RESULTS.md"
    if not a.only:
        target.write_text("\n".join(sections))
        print(f"wrote {target}")
    print(f"done in {time.time() - t0:.0f}s: {runner.executed} simulated, {runner.cached} from cache")
    return 0


if __name__ == "__main__":
    sys.exit(main())
