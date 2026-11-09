# Reproducible environment for ceio-sim (artifact evaluation).
#
#   docker build -t ceio-sim .
#   docker run --rm -v "$PWD/docs:/ceio-sim/docs" ceio-sim            # full sweeps
#   docker run --rm -v "$PWD/docs:/ceio-sim/docs" ceio-sim --quick    # reduced sweeps
#
# Results (tables, RESULTS.md, figures) are written to docs/generated and
# docs/figures; mounting docs/ keeps them on the host.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates python3 python3-venv python3-pip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /ceio-sim
COPY . .
RUN rm -rf build .venv results \
 && cmake --preset release && cmake --build --preset release \
 && python3 -m venv .venv && .venv/bin/pip install --quiet -e python

ENTRYPOINT ["scripts/reproduce.sh"]
