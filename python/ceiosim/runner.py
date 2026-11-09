"""Running ceio_run: one simulation per argument list, cached and parallel.

The harness talks to the simulator only through `ceio_run`'s command line and
its JSON output, so the C++ and Python sides can evolve independently.

Results are cached on disk under a key made of the simulator binary's hash and
the arguments, so rebuilding the simulator invalidates stale results
automatically and re-running a figure only simulates what changed.
"""

from __future__ import annotations

import concurrent.futures as cf
import hashlib
import json
import os
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

REPO = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO / "build" / "release" / "ceio_run"
DEFAULT_CACHE = REPO / "results" / "cache"


class SimulationError(RuntimeError):
    """ceio_run exited with an error."""


@dataclass(frozen=True)
class Run:
    """One simulation: the ceio_run arguments plus labels for reporting."""

    args: tuple[str, ...]
    labels: tuple[tuple[str, str], ...] = field(default_factory=tuple)

    @staticmethod
    def of(*args: str, **labels: object) -> "Run":
        return Run(tuple(args), tuple((k, str(v)) for k, v in labels.items()))

    def label(self, key: str) -> str:
        return dict(self.labels)[key]


class Runner:
    """Executes Runs with an on-disk cache and a thread pool.

    Each simulation is a separate process, so threads are enough for
    parallelism. Simulations are deterministic, which is what makes caching
    sound.
    """

    def __init__(self, binary: Path = DEFAULT_BINARY, cache_dir: Path = DEFAULT_CACHE,
                 jobs: int | None = None, verbose: bool = False) -> None:
        self.binary = Path(binary)
        if not self.binary.exists():
            raise FileNotFoundError(
                f"{self.binary} not found; build it with: cmake --preset release && cmake --build --preset release")
        self.cache_dir = Path(cache_dir)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.jobs = jobs or max(1, (os.cpu_count() or 2) - 1)
        self.verbose = verbose
        self._binary_hash = _file_hash(self.binary)
        self.executed = 0
        self.cached = 0

    def key(self, run: Run) -> str:
        h = hashlib.sha256(self._binary_hash.encode())
        h.update("\0".join(run.args).encode())
        return h.hexdigest()[:24]

    def run(self, run: Run) -> dict:
        path = self.cache_dir / f"{self.key(run)}.json"
        if path.exists():
            self.cached += 1
            return json.loads(path.read_text())
        proc = subprocess.run([str(self.binary), *run.args], capture_output=True, text=True)
        if proc.returncode != 0:
            raise SimulationError(f"ceio_run {' '.join(run.args)} failed:\n{proc.stderr.strip()}")
        result = json.loads(proc.stdout)
        result["_args"] = list(run.args)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(result))
        tmp.replace(path)  # atomic: concurrent workers never see half-written files
        self.executed += 1
        if self.verbose:
            print(f"  ran {' '.join(run.args)}  ({result.get('wall_seconds', 0):.1f}s)", flush=True)
        return result

    def run_all(self, runs: Iterable[Run]) -> dict[Run, dict]:
        runs = list(dict.fromkeys(runs))  # de-duplicate, keep order
        out: dict[Run, dict] = {}
        with cf.ThreadPoolExecutor(max_workers=self.jobs) as pool:
            futures = {pool.submit(self.run, r): r for r in runs}
            for fut in cf.as_completed(futures):
                out[futures[fut]] = fut.result()
        return out


def _file_hash(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Small helpers over result JSON
# ---------------------------------------------------------------------------

def kv_flows(r: dict) -> list[dict]:
    """CPU-involved flows of a preset run (bypass flows have ids >= 100)."""
    return [f for f in r["flows"] if f["id"] < 100] if any(f["id"] >= 100 for f in r["flows"]) else r["flows"]


def kv_mpps(r: dict) -> float:
    return sum(f["mpps"] for f in kv_flows(r))


def bypass_gbps(r: dict) -> float:
    return sum(f["goodput_gbps"] for f in r["flows"] if f["id"] >= 100)


def kv_hit(r: dict) -> float:
    qs = [q for q in r["queues"] if q["app"] != "rdma_sink"]
    return sum(q["llc_read_hit_rate"] for q in qs) / max(1, len(qs))


def buffers_touched(r: dict) -> int:
    return sum(q["distinct_buffers_used"] for q in r["queues"])


def phase_means(r: dict, phase_ms: float, phases: int, lead_ms: float = 1.0, pkt_bytes: int = 144) -> list[float]:
    """Mean CPU-involved Mpps per phase from a preset's time series."""
    idx = [i for i, f in enumerate(r["flows"]) if f["id"] < 100]
    buckets: list[list[float]] = [[] for _ in range(phases)]
    for s in r["samples"]:
        t = s["t_ms"] - lead_ms
        if t <= 0:
            continue
        k = min(int((t - 1e-9) // phase_ms), phases - 1)
        buckets[k].append(sum(s["flow_gbps"][i] for i in idx) * 1e9 / 8 / pkt_bytes / 1e6)
    return [sum(b) / len(b) if b else float("nan") for b in buckets]


def timeseries(r: dict, pkt_bytes: int = 144) -> tuple[list[float], list[float]]:
    """(t_ms, CPU-involved Mpps) samples of a preset run."""
    idx = [i for i, f in enumerate(r["flows"]) if f["id"] < 100]
    ts = [s["t_ms"] for s in r["samples"]]
    mpps = [sum(s["flow_gbps"][i] for i in idx) * 1e9 / 8 / pkt_bytes / 1e6 for s in r["samples"]]
    return ts, mpps


def args_with(base: Sequence[str], **kv: object) -> tuple[str, ...]:
    """base args plus --key=value options (underscores become dashes)."""
    return tuple(base) + tuple(f"--{k.replace('_', '-')}={v}" for k, v in kv.items())
