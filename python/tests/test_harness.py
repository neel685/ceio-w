"""Unit tests for the harness (no simulator needed: a fake binary echoes JSON)."""

import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from ceiosim import report  # noqa: E402
from ceiosim.experiments import CATALOGUE, pick  # noqa: E402
from ceiosim.runner import Run, Runner, SimulationError, args_with, phase_means  # noqa: E402

FAKE = """#!/bin/sh
for a in "$@"; do case "$a" in --fail) echo boom >&2; exit 2;; esac; done
echo "$@" >> "$(dirname "$0")/calls.log"
printf '{"wall_seconds": 0.0, "flows": [], "samples": [], "args_seen": "%s"}\\n' "$*"
"""


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.bin = self.tmp / "fake_run"
        self.bin.write_text(FAKE)
        self.bin.chmod(self.bin.stat().st_mode | stat.S_IEXEC)
        self.runner = Runner(self.bin, self.tmp / "cache", jobs=4)

    def calls(self):
        log = self.tmp / "calls.log"
        return log.read_text().splitlines() if log.exists() else []

    def test_runs_and_caches(self):
        r = Run.of("--a=1", tag="x")
        self.assertEqual(self.runner.run(r)["args_seen"], "--a=1")
        self.runner.run(r)
        self.assertEqual(len(self.calls()), 1)
        self.assertEqual((self.runner.executed, self.runner.cached), (1, 1))

    def test_rebuilt_binary_invalidates_cache(self):
        r = Run.of("--a=1")
        k1 = self.runner.key(r)
        self.bin.write_text(FAKE + "# rebuilt\n")
        self.assertNotEqual(Runner(self.bin, self.tmp / "cache").key(r), k1)

    def test_parallel_and_deduplicated(self):
        runs = [Run.of(f"--i={i}") for i in range(10)] + [Run.of("--i=3")]
        out = self.runner.run_all(runs)
        self.assertEqual(len(out), 10)
        self.assertEqual(len(self.calls()), 10)

    def test_failure_is_reported(self):
        with self.assertRaises(SimulationError):
            self.runner.run(Run.of("--fail"))

    def test_missing_binary(self):
        with self.assertRaises(FileNotFoundError):
            Runner(self.tmp / "nope", self.tmp / "cache")


class HelpersTest(unittest.TestCase):
    def test_args_with(self):
        self.assertEqual(args_with(("--x=1",), cpu_flows=4), ("--x=1", "--cpu-flows=4"))

    def test_pick_by_labels(self):
        a, b = Run.of("--a", s="ceio", n=2), Run.of("--b", s="ceio-w", n=2)
        res = {a: {"v": 1}, b: {"v": 2}}
        self.assertEqual(pick(res, s="ceio-w", n=2)["v"], 2)
        with self.assertRaises(KeyError):
            pick(res, s="toasty")

    def test_phase_means(self):
        r = {"flows": [{"id": 1}, {"id": 101}],
             "samples": [{"t_ms": t, "flow_gbps": [1.152, 99.0]} for t in (1.5, 2.5, 3.5, 4.5)]}
        # 1.152 Gbps of 144 B packets = 1 Mpps; bypass flow (id 101) excluded.
        self.assertEqual(phase_means(r, 2.0, 2), [1.0, 1.0])

    def test_table_markdown_and_csv(self):
        t = report.Table("t", "Title", ["a", "b"])
        t.add("x", 1.234)
        t.add("y", float("nan"))
        md = t.markdown()
        self.assertIn("| x | 1.23 |", md)
        self.assertIn("| y | — |", md)
        with self.assertRaises(ValueError):
            t.add("only one")
        out = Path(tempfile.mkdtemp())
        t.write(out)
        self.assertEqual((out / "t.csv").read_text().splitlines()[0], "a,b")

    def test_catalogue_is_complete(self):
        for name in ("toasty_saturation", "toasty_microburst", "ceio_dynamic", "ceio_burst", "ceio_mixed",
                     "memory_pressure", "footprint", "ceio_w_ablation", "flow_churn", "seeds"):
            self.assertIn(name, CATALOGUE)
            self.assertTrue(CATALOGUE[name].runs(True))


if __name__ == "__main__":
    unittest.main()
