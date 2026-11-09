"""The experiment catalogue: every table and figure in docs/results.md.

Each experiment declares the simulations it needs (`runs`) and turns their
results into tables and figures (`build`). `quick=True` shrinks sweeps so the
whole catalogue runs in about a minute; the published numbers use the
full sweeps.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from . import report
from .runner import Run, buffers_touched, bypass_gbps, kv_hit, kv_mpps, phase_means

SCHEMES = ["baseline", "hostcc", "shring", "toasty", "ceio", "ceio-w"]


@dataclass
class Experiment:
    name: str
    title: str
    runs: Callable[[bool], list[Run]]
    build: Callable[[dict[Run, dict], bool, Path], list[report.Table]]


CATALOGUE: dict[str, Experiment] = {}


def experiment(name: str, title: str):
    def wrap(fn_pair):
        runs, build = fn_pair()
        CATALOGUE[name] = Experiment(name, title, runs, build)
        return fn_pair
    return wrap


def pick(results: dict[Run, dict], **labels: object) -> dict:
    """The result of the run whose labels include all of `labels`."""
    want = {k: str(v) for k, v in labels.items()}
    for run, res in results.items():
        have = dict(run.labels)
        if all(have.get(k) == v for k, v in want.items()):
            return res
    raise KeyError(f"no run with labels {want}")


# ---------------------------------------------------------------------------
# Toasty
# ---------------------------------------------------------------------------

TOASTY = ("--testbed=toasty", "--warmup-ms=1", "--measure-ms=2", "--saturate=0.001")
TOASTY_CONFIGS = {
    "default": ("--pool=16384", "--ring=4096"),
    "sized": ("--pool=4096", "--ring=4096"),
    "toasty": ("--pool=16384", "--ring=4096", "--toasty=full"),
    "toasty-user": ("--pool=16384", "--ring=4096", "--toasty=user"),
    "toasty-kernel": ("--pool=16384", "--ring=4096", "--toasty=kernel"),
}
CONFIG_LABELS = {"default": "Default (16384 FIFO)", "sized": "Pool sized to DDIO (4096)", "toasty": "Toasty",
                 "toasty-user": "LIFO pool only", "toasty-kernel": "Adaptive refill only"}
# Colours: the default pool is the baseline (blue), Toasty keeps its scheme hue.
report.SCHEME_COLORS.update({"default": "#2a78d6", "sized": "#1baf7a", "toasty-user": "#e87ba4",
                             "toasty-kernel": "#eb6834"})
report.SCHEME_LABELS.update(CONFIG_LABELS)


@experiment("toasty_saturation", "Toasty Fig. 7 and 18: saturation throughput (at most 0.1 % loss)")
def _toasty_saturation():
    apps = ["nat", "l2fwd", "ids"]

    def runs(quick: bool) -> list[Run]:
        out = [Run.of(*TOASTY, f"--app={a}", *TOASTY_CONFIGS[c], app=a, config=c)
               for a in (apps[:1] if quick else apps) for c in ("default", "sized", "toasty")]
        out += [Run.of(*TOASTY, "--app=nat", *TOASTY_CONFIGS[c], app="nat", config=c)
                for c in ("toasty-user", "toasty-kernel")]
        return out

    def build(res, quick, figdir):
        used = apps[:1] if quick else apps
        t = report.Table("toasty_saturation", "Toasty Fig. 7: saturation throughput, PCIe write hits and DRAM writes",
                         ["App", "Config", "Gbps", "PCIe write hit", "Dead writebacks", "DRAM writes GB/s",
                          "Buffers touched"])
        for a in used:
            for c in ("default", "sized", "toasty"):
                r = pick(res, app=a, config=c)
                t.add(a, CONFIG_LABELS[c], r["offered_gbps"], r["cache"]["io_write_hit_rate"],
                      r["cache"]["dead_writebacks"], r["memory"]["dram_write_gbytes_s"], buffers_touched(r))
        abl = report.Table("toasty_ablation", "Toasty Fig. 18: Toasty's two parts (NAT, saturation Gbps)",
                           ["Toasty, full", "LIFO pool only", "Adaptive refill only", "Default"])
        abl.add(*(pick(res, app="nat", config=c)["offered_gbps"]
                  for c in ("toasty", "toasty-user", "toasty-kernel", "default")))
        fig, (ax,) = report.figure(7, 3.6)
        report.grouped_bars(ax, [a.upper() if a != "l2fwd" else "L2Fwd" for a in used],
                            {c: [pick(res, app=a, config=c)["offered_gbps"] for a in used]
                             for c in ("default", "sized", "toasty")},
                            "Saturation throughput (Gbps, loss ≤ 0.1 %)", "Gbps")
        report.save(fig, figdir / "toasty_saturation.png")
        return [t, abl]

    return runs, build


@experiment("toasty_microburst", "Toasty Table 7: drops during bursts")
def _toasty_microburst():
    base = ("--testbed=toasty", "--app=nat", "--rate=8", "--pattern=microburst", "--burst-pkts=2048",
            "--burst-period-us=1000", "--warmup-ms=1", "--measure-ms=4")
    cfgs = {"default": ("--pool=16384", "--ring=4096"), "small": ("--pool=512", "--ring=512"),
            "toasty": ("--pool=16384", "--ring=4096", "--toasty=full")}

    def runs(quick):
        return [Run.of(*base, *a, config=c) for c, a in cfgs.items()]

    def build(res, quick, figdir):
        t = report.Table("toasty_microburst", "Toasty Table 7: drops during bursts of 2048 packets (NAT)",
                         ["Default (16384)", "Small pool (512)", "Toasty (16384)"])
        drop = lambda r: 100.0 * (r["drops_nic"] + r["drops_switch"]) / max(1, r["sent_packets"])
        t.add(*(f"{drop(pick(res, config=c)):.2f} %" for c in cfgs))
        return [t]

    return runs, build


# ---------------------------------------------------------------------------
# CEIO paper scenarios (all schemes)
# ---------------------------------------------------------------------------

def _phase_experiment(preset: str, phases: int, title: str, short: str, phase_names: list[str]):
    def runs(quick):
        return [Run.of(f"--preset={preset}", f"--scheme={s}", "--phase-ms=2", scheme=s) for s in SCHEMES]

    def build(res, quick, figdir):
        t = report.Table(f"ceio_{preset}", title,
                         ["Scheme", *[f"Phase {i}" for i in range(phases)], "P99.9 µs", "NIC drops", "Reordered"])
        means = {}
        for s in SCHEMES:
            r = pick(res, scheme=s)
            means[s] = phase_means(r, 2.0, phases)
            t.add(report.SCHEME_LABELS[s], *means[s], r["latency"]["p999_us"], r["drops_nic"], r["reordered"])
        # Per-phase means as grouped bars: six noisy time series would hide the comparison.
        fig, (ax,) = report.figure(7.5, 3.6)
        report.grouped_bars(ax, phase_names, means, short, "KV Mpps (phase mean)", value_labels=False)
        report.save(fig, figdir / f"ceio_{preset}.png")
        return [t]

    return runs, build


experiment("ceio_dynamic", "CEIO Fig. 4a and 10a: dynamic flows")(
    lambda: _phase_experiment("dynamic", 3, "Dynamic flow distribution: 8 KV → 6 KV + 2 LineFS → 4 KV + 4 LineFS",
                              "Dynamic flow distribution", ["8 KV", "6 KV + 2 LineFS", "4 KV + 4 LineFS"]))
experiment("ceio_burst", "CEIO Fig. 4b and 10b: bursts")(
    lambda: _phase_experiment("burst", 4, "Network burst: 8 KV, + 2 burst KV flows in phases 1 and 3",
                              "Network burst", ["8 KV", "+2 burst", "8 KV", "+2 burst"]))


@experiment("ceio_mixed", "CEIO Table 4: mixed flows")
def _ceio_mixed():
    ratios = [6, 4, 2]

    def runs(quick):
        return [Run.of("--preset=mixed", f"--cpu-flows={n}", f"--scheme={s}", ratio=n, scheme=s)
                for n in ratios for s in SCHEMES]

    def build(res, quick, figdir):
        t = report.Table("ceio_mixed", "CEIO Table 4: KV Mpps / LineFS Gbps / P99.9 µs",
                         ["CPU : bypass", *[report.SCHEME_LABELS[s] for s in SCHEMES]])
        for n in ratios:
            cells = []
            for s in SCHEMES:
                r = pick(res, ratio=n, scheme=s)
                cells.append(f"{kv_mpps(r):.1f} / {bypass_gbps(r):.0f} / {r['latency']['p999_us']:.0f}")
            t.add(f"{n} : {8 - n}", *cells)
        fig, (ax,) = report.figure(7.5, 3.6)
        report.grouped_bars(ax, [f"{n}:{8 - n}" for n in ratios],
                            {s: [kv_mpps(pick(res, ratio=n, scheme=s)) for n in ratios] for s in SCHEMES},
                            "Mixed flows: CPU-involved throughput", "KV Mpps")
        report.save(fig, figdir / "ceio_mixed.png")
        return [t]

    return runs, build


@experiment("memory_pressure", "8 KV flows with and without a 100 GB/s memory co-runner")
def _pressure():
    def runs(quick):
        return [Run.of("--preset=mixed", "--cpu-flows=8", f"--pressure={p}", f"--scheme={s}", pressure=p, scheme=s)
                for p in (0, 100) for s in SCHEMES]

    def build(res, quick, figdir):
        t = report.Table("memory_pressure", "Memory pressure: KV Mpps (P99.9 µs)",
                         ["Co-runner", *[report.SCHEME_LABELS[s] for s in SCHEMES]])
        for p in (0, 100):
            t.add("none" if p == 0 else f"{p} GB/s",
                  *[f"{kv_mpps(r):.1f} ({r['latency']['p999_us']:.0f})"
                    for r in (pick(res, pressure=p, scheme=s) for s in SCHEMES)])
        fig, (ax,) = report.figure(7.5, 3.6)
        report.grouped_bars(ax, ["no co-runner", "100 GB/s co-runner"],
                            {s: [kv_mpps(pick(res, pressure=p, scheme=s)) for p in (0, 100)] for s in SCHEMES},
                            "8 KV flows: throughput with a memory co-runner", "KV Mpps")
        report.save(fig, figdir / "memory_pressure.png")
        return [t]

    return runs, build


# ---------------------------------------------------------------------------
# CEIO-W research questions
# ---------------------------------------------------------------------------

KV1 = ("--testbed=ceio", "--app=kv", "--pkt=512", "--pool=8192", "--ring=4096", "--warmup-ms=1", "--measure-ms=2")


@experiment("footprint", "Cache footprint (one KV core, 4096 ring, no CC)")
def _rq1():
    schemes = ["baseline", "ceio", "ceio-w"]

    def runs(quick):
        return [Run.of(*KV1, f"--rate={g}", f"--scheme={s}", gbps=g, scheme=s) for g in (15, 26) for s in schemes]

    def build(res, quick, figdir):
        t = report.Table("footprint", "Cache footprint",
                         ["Load", "Scheme", "Goodput Gbps", "KV hit", "PCIe write hit", "Dead writebacks",
                          "DRAM writes GB/s", "Buffers touched", "P99.9 µs"])
        for g in (15, 26):
            for s in schemes:
                r = pick(res, gbps=g, scheme=s)
                t.add(f"{g} Gbps", report.SCHEME_LABELS[s], r["goodput_gbps"], kv_hit(r),
                      r["cache"]["io_write_hit_rate"], r["cache"]["dead_writebacks"],
                      r["memory"]["dram_write_gbytes_s"], buffers_touched(r), r["latency"]["p999_us"])
        fig, axes = report.figure(10, 3.4, ncols=3)
        groups = ["15 Gbps", "26 Gbps"]
        def ser(f):
            return {s: [f(pick(res, gbps=g, scheme=s)) for g in (15, 26)] for s in schemes}
        report.grouped_bars(axes[0], groups, ser(buffers_touched), "Buffers touched", "buffers (log)", log=True)
        report.grouped_bars(axes[1], groups, ser(lambda r: r["cache"]["io_write_hit_rate"]), "PCIe write hit rate",
                            "hit rate", value_labels=False)
        report.grouped_bars(axes[2], groups, ser(lambda r: r["goodput_gbps"]), "Goodput", "Gbps")
        for ax in axes[1:]:
            ax.get_legend().remove()
        report.save(fig, figdir / "footprint.png")
        return [t]

    return runs, build


ABLATION = [("CEIO", ("--scheme=ceio",)), ("CEIO + warm buffers only", ("--scheme=ceio", "--warm=1")),
            ("CEIO-W (all mechanisms)", ("--scheme=ceio-w",)), ("CEIO-W, equal split instead of adaptive allocation", ("--scheme=ceio-w", "--w-equal=1")),
            ("CEIO-W, fixed budget", ("--scheme=ceio-w", "--w-static=1")),
            ("CEIO-W, static Eq. (1) budget", ("--scheme=ceio-w", "--w-static=1", "--w-budget=1"))]


@experiment("ceio_w_ablation", "CEIO-W ablation")
def _ablation():
    def runs(quick):
        out = []
        for name, a in ABLATION:
            out.append(Run.of(*KV1, "--rate=26", *a, variant=name, case="overload"))
            out.append(Run.of("--preset=dynamic", "--phase-ms=2", *a, variant=name, case="dynamic"))
            out.append(Run.of("--preset=mixed", "--cpu-flows=8", "--pressure=100", *a, variant=name, case="pressure"))
        return out

    def build(res, quick, figdir):
        t = report.Table("ceio_w_ablation", "CEIO-W ablation",
                         ["Variant", "Overload Gbps", "Dynamic KV Mpps", "Dynamic dead writebacks",
                          "Pressure KV Mpps", "Pressure P99.9 µs"])
        for name, _ in ABLATION:
            o, d, p = (pick(res, variant=name, case=c) for c in ("overload", "dynamic", "pressure"))
            t.add(name, o["goodput_gbps"], kv_mpps(d), d["cache"]["dead_writebacks"], kv_mpps(p),
                  p["latency"]["p999_us"])
        return [t]

    return runs, build


@experiment("flow_churn", "Flow churn ([CEIO] Fig. 12)")
def _rq3():
    variants = [("baseline", ("--scheme=baseline",)), ("ceio", ("--scheme=ceio",)),
                ("ceio-timer", ("--scheme=ceio",)), ("ceio-w", ("--scheme=ceio-w",))]
    report.SCHEME_LABELS["ceio-timer"] = "CEIO (idle timer = slot)"
    report.SCHEME_COLORS["ceio-timer"] = "#6250d6"
    report.SCHEME_MARKERS["ceio-timer"] = "X"

    def sweep(quick):
        return ([16, 512] if quick else [16, 128, 512, 1024, 2048]), ([100] if quick else [100, 1000])

    def runs(quick):
        flows, slots = sweep(quick)
        out = []
        for n in flows:
            for slot in slots:
                for v, a in variants:
                    extra = (f"--ceio-idle-us={slot}",) if v == "ceio-timer" else ()
                    out.append(Run.of("--preset=churn", f"--flows={n}", f"--slot-us={slot}", *a, *extra,
                                      flows=n, slot=slot, variant=v))
        return out

    def build(res, quick, figdir):
        flows, slots = sweep(quick)
        t = report.Table("flow_churn", "Flow churn: slow-path share % / P99 µs (16 active flows, 60 Gbps)",
                         ["Flows", "Slot", *[report.SCHEME_LABELS.get(v, v) for v, _ in variants]])
        slow = lambda r: 100.0 * r.get("ceio", {}).get("slow_packets", 0) / max(1, r["sent_packets"])
        for slot in slots:
            for n in flows:
                t.add(n, f"{slot} µs", *[f"{slow(r):.0f} % / {r['latency']['p99_us']:.1f}"
                                          for r in (pick(res, flows=n, slot=slot, variant=v) for v, _ in variants)])
        fig, axes = report.figure(5 * len(slots), 3.6, ncols=len(slots))
        for ax, slot in zip(axes, slots):
            report.lines(ax, {v: (flows, [slow(pick(res, flows=n, slot=slot, variant=v)) for n in flows])
                              for v, _ in variants},
                         f"Slow-path share, {slot} µs slots", "% of packets", "flows (log)", logx=True)
        report.save(fig, figdir / "flow_churn.png")
        return [t]

    return runs, build


@experiment("seeds", "Different seeds")
def _seeds():
    cases = {"pressure": ("--preset=mixed", "--cpu-flows=8", "--pressure=100"),
             "dynamic": ("--preset=dynamic", "--phase-ms=2")}
    schemes = ["toasty", "ceio", "ceio-w"]

    def seeds(quick):
        return [1, 2] if quick else [1, 2, 3, 4, 5]

    def runs(quick):
        return [Run.of(*a, f"--scheme={s}", f"--seed={k}", case=c, scheme=s, seed=k)
                for c, a in cases.items() for s in schemes for k in seeds(quick)]

    def build(res, quick, figdir):
        t = report.Table("seeds", "Different seeds: KV Mpps mean (lowest to highest)",
                         ["Case", *[report.SCHEME_LABELS[s] for s in schemes]])
        for c in cases:
            cells = []
            for s in schemes:
                v = [kv_mpps(pick(res, case=c, scheme=s, seed=k)) for k in seeds(quick)]
                cells.append(f"{statistics.mean(v):.1f} ({min(v):.1f} to {max(v):.1f})")
            t.add(c, *cells)
        t.note = f"Seeds: {', '.join(map(str, seeds(quick)))}."
        return [t]

    return runs, build
