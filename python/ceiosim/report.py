"""Tables (Markdown + CSV) and figures (PNG) produced by experiments."""

from __future__ import annotations

import csv
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

# Categorical palette (dataviz reference instance, validated for 6 adjacent
# slots in light mode: CVD and normal-vision separation pass; three hues sit
# below 3:1 contrast, so every figure carries a legend, line markers, and a
# table of the same numbers). Colour follows the scheme, never its rank.
SCHEME_COLORS = {
    "baseline": "#2a78d6",
    "hostcc": "#eb6834",
    "shring": "#1baf7a",
    "toasty": "#eda100",
    "ceio": "#e87ba4",
    "ceio-w": "#008300",
}
SCHEME_MARKERS = {"baseline": "o", "hostcc": "s", "shring": "^", "toasty": "D", "ceio": "v", "ceio-w": "P"}
SCHEME_LABELS = {"baseline": "Baseline", "hostcc": "HostCC", "shring": "ShRing", "toasty": "Toasty",
                 "ceio": "CEIO", "ceio-w": "CEIO-W"}
INK, INK2, MUTED, GRID, SURFACE = "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#fcfcfb"


@dataclass
class Table:
    """A titled table; rendered as Markdown and CSV."""

    name: str
    title: str
    columns: list[str]
    rows: list[list[object]] = field(default_factory=list)
    note: str = ""

    def add(self, *cells: object) -> None:
        if len(cells) != len(self.columns):
            raise ValueError(f"{self.name}: {len(cells)} cells for {len(self.columns)} columns")
        self.rows.append(list(cells))

    def markdown(self) -> str:
        def fmt(c: object) -> str:
            if isinstance(c, float):
                return "—" if math.isnan(c) else (f"{c:.2f}" if abs(c) < 10 else f"{c:.1f}")
            return str(c)
        lines = [f"### {self.title}", ""]
        if self.note:
            lines += [self.note, ""]
        lines.append("| " + " | ".join(self.columns) + " |")
        lines.append("|" + "|".join("---" for _ in self.columns) + "|")
        lines += ["| " + " | ".join(fmt(c) for c in row) + " |" for row in self.rows]
        return "\n".join(lines) + "\n"

    def write(self, out: Path) -> None:
        out.mkdir(parents=True, exist_ok=True)
        with open(out / f"{self.name}.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(self.columns)
            w.writerows(self.rows)


def _style(ax, title: str, ylabel: str, xlabel: str | None = None) -> None:
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", fontsize=11, color=INK, pad=10)
    ax.set_ylabel(ylabel, color=INK2, fontsize=9)
    if xlabel:
        ax.set_xlabel(xlabel, color=INK2, fontsize=9)
    ax.tick_params(colors=MUTED, labelsize=8, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(GRID)


def figure(width: float = 7.0, height: float = 3.6, ncols: int = 1):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, ncols, figsize=(width, height), squeeze=False)
    fig.patch.set_facecolor(SURFACE)
    return fig, axes[0]


def grouped_bars(ax, groups: Sequence[str], series: dict[str, Sequence[float]], title: str, ylabel: str,
                 value_labels: bool = True, log: bool = False) -> None:
    """Bars grouped by `groups`, one bar per series (scheme) per group."""
    n = len(series)
    width = 0.8 / max(n, 1)
    for i, (name, values) in enumerate(series.items()):
        xs = [g + (i - (n - 1) / 2) * width for g in range(len(groups))]
        color = SCHEME_COLORS.get(name, MUTED)
        bars = ax.bar(xs, values, width=width * 0.9, color=color, label=SCHEME_LABELS.get(name, name),
                      edgecolor=SURFACE, linewidth=1.0)
        if value_labels:
            for b, v in zip(bars, values):
                if not math.isnan(v):
                    ax.annotate(f"{v:.0f}" if v >= 10 else f"{v:.1f}", (b.get_x() + b.get_width() / 2, b.get_height()),
                                ha="center", va="bottom", fontsize=6.5, color=INK2, xytext=(0, 1),
                                textcoords="offset points")
    ax.set_xticks(range(len(groups)), groups)
    if log:
        ax.set_yscale("log")
    _style(ax, title, ylabel)
    ax.legend(frameon=False, fontsize=8, ncols=min(n, 6), loc="upper left", bbox_to_anchor=(0, -0.12))


def lines(ax, series: dict[str, tuple[Sequence[float], Sequence[float]]], title: str, ylabel: str, xlabel: str,
          logx: bool = False, markevery: int | None = None) -> None:
    for name, (xs, ys) in series.items():
        ax.plot(xs, ys, color=SCHEME_COLORS.get(name, MUTED), linewidth=2, marker=SCHEME_MARKERS.get(name, "o"),
                markersize=5, markevery=markevery, label=SCHEME_LABELS.get(name, name))
    if logx:
        ax.set_xscale("log")
        xs = sorted({x for xs_, _ in series.values() for x in xs_})
        ax.set_xticks(xs, [str(int(x)) if float(x).is_integer() else str(x) for x in xs])
        ax.minorticks_off()
    _style(ax, title, ylabel, xlabel)
    ax.legend(frameon=False, fontsize=8, ncols=min(len(series), 2), loc="upper left", bbox_to_anchor=(0, -0.18))


def save(fig, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(path, dpi=200, facecolor=SURFACE)
    import matplotlib.pyplot as plt

    plt.close(fig)
