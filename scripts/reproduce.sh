#!/usr/bin/env bash
# Build, test, and regenerate every table and figure of docs/results.md.
#
#   scripts/reproduce.sh           # full sweeps (~tens of minutes on 8 cores)
#   scripts/reproduce.sh --quick   # reduced sweeps (a few minutes)
#
# Extra arguments are passed to `python -m ceiosim` (e.g. --jobs 16, --only flow_churn).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== build (release)"
cmake --preset release >/dev/null
cmake --build --preset release

echo "== unit and emergent-behaviour tests"
ctest --preset release

echo "== python environment"
if [[ ! -x .venv/bin/python ]]; then
  python3 -m venv .venv
fi
.venv/bin/pip install --quiet --upgrade pip
.venv/bin/pip install --quiet -e python
.venv/bin/python -m unittest discover -s python/tests

echo "== experiments"
.venv/bin/python -m ceiosim "$@"
