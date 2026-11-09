# Results

Everything on this page comes from the simulator; [spec.md](spec.md)
describes the model and how it was calibrated. `scripts/reproduce.sh`
regenerates all of these tables into [generated/RESULTS.md](generated/RESULTS.md)
and the figures into [figures/](figures/). If this page and the generated
file ever disagree, trust the generated one. A run with the same seed (1 by
default) always gives the same numbers.

In short: Toasty's results reproduce well. CEIO's mechanisms work as the
paper describes, but its large speedups over the baseline don't show up in
this model. Section 3 goes into why.

## 1. Toasty

These runs use the Toasty testbed settings: a 100 Gbps NIC, one core running
a network function, and 512-byte packets.

### Saturation throughput (Toasty Fig. 7)

This is the highest load each setup handles while losing at most 0.1 % of
packets.

```
ceio_run --testbed=toasty --app=APP --pool=P --ring=4096 [--toasty=full] --saturate=0.001 --warmup-ms=1 --measure-ms=2
```

| App | Default pool (16384, FIFO) | Pool sized to the cache (4096) | Toasty (16384) | Toasty PCIe write hits (default) | Toasty DRAM writes (default) |
|---|---|---|---|---|---|
| NAT | 43.4 Gbps | 50.4 | 52.7 | 0.22 (0.12) | 5.8 GB/s (5.7) |
| L2Fwd | 77.0 | 77.3 | 77.7 | 0.90 (0.10) | 1.0 GB/s (9.8) |
| IDS | 28.1 | 28.1 | 28.5 | 1.00 (0.10) | 0.0 GB/s (3.6) |

The order is the same as in the paper: Toasty does at least as well as a
hand-sized pool, and both beat the default. The reason is also the same.
Toasty ends up using only 90 to 620 buffers instead of all 16,384, almost
every DMA write finds its buffer already in the cache, and the dead
writebacks that waste DRAM bandwidth go away.

The throughput gains are smaller than in the paper, though: 21 % for NAT,
where the paper reports up to 78 %. The model has no private L2 cache or
hardware prefetcher, so a wasted writeback only costs DRAM bandwidth here
(spec.md, Sec. 3.5).

### Toasty's two parts (Toasty Fig. 18)

NAT, saturation throughput in Gbps:

| Full Toasty | LIFO pool only | Adaptive refill only | Default |
|---|---|---|---|
| 52.7 | 49.6 | 43.8 | 43.4 |

As in the paper, neither part gets the full gain on its own.

### Bursts (Toasty Table 7)

Bursts of 2048 packets of 512 bytes every millisecond, 8 Gbps on average:

| Default pool (16384) | Small pool (512) | Toasty (16384) |
|---|---|---|
| 0 % dropped | 29.6 % | 0 % |

Toasty keeps the burst tolerance of a large pool, as the paper claims.

## 2. CEIO

These runs use the CEIO testbed: a 200 Gbps link between two machines, an
eRPC-style key-value server with 144-byte requests, and DCTCP on the senders.

### CEIO's mechanisms

The tests in `tests/policy/test_ceio.cpp` check each of these.

| Claim in the paper | What the model shows |
|---|---|
| Credits stop leaky DMA (Fig. 9) | On an overloaded KV core with a 4096-entry ring, the CPU hit rate goes from 0.29 to 0.50, goodput goes up, and nothing is dropped |
| The slow path is only slow for small messages (Fig. 11) | It reaches 33 % of fast-path throughput at 512 B to 1 KB, 55 % at 2 KB and 100 % at 4 KB |
| The slow path adds some latency (Table 3) | About 1.5 µs at low load |
| `async_recv` is faster than `recv` (Sec. 4.2) | 23.9 vs 19.6 Gbps when the slow path is busy |
| Large RDMA messages get pushed to the slow path (Sec. 4.1) | They do, and the KV core's hit rate goes from 0.31 to 0.73 with no drops |
| Buffering on the NIC means no drops (Sec. 3) | No drops in any scenario below; every other scheme drops some |
| Packets stay in order (Sec. 4.2) | No reordered packets in any CEIO run |

### Dynamic flows (CEIO Fig. 4a and 10a)

Eight KV flows run at first. Every 2 ms, two of them are replaced by LineFS
flows that write over RDMA. The phase columns are KV throughput in Mpps.

```
ceio_run --preset=dynamic --scheme=S --phase-ms=2
```

| Scheme | 8 KV | 6 KV + 2 LineFS | 4 KV + 4 LineFS | P99.9 latency | NIC drops |
|---|---|---|---|---|---|
| Baseline | 30.6 | 22.1 | 12.7 | 510 µs | 4187 |
| HostCC | 23.4 | 15.2 | 10.3 | 510 µs | 1241 |
| ShRing | 35.0 | 41.0 | 28.2 | 265 µs | 771 |
| Toasty | 30.7 | 31.1 | 23.1 | 2147 µs | 1062 |
| CEIO | 16.4 | 21.6 | 12.4 | 1132 µs | 0 |

### Bursts (CEIO Fig. 4b and 10b)

Eight KV flows, plus two extra KV flows on two extra cores in the second and
fourth phases.

```
ceio_run --preset=burst --scheme=S --phase-ms=2
```

| Scheme | 8 KV | +2 burst | 8 KV | +2 burst | P99.9 latency | NIC drops |
|---|---|---|---|---|---|---|
| Baseline | 32.0 | 35.1 | 34.6 | 35.3 | 531 µs | 2572 |
| HostCC | 23.2 | 21.7 | 35.4 | 22.7 | 535 µs | 961 |
| ShRing | 35.4 | 34.6 | 38.6 | 34.4 | 2298 µs | 2393 |
| Toasty | 30.7 | 38.5 | 37.9 | 38.5 | 2161 µs | 1217 |
| CEIO | 16.5 | 25.0 | 29.2 | 20.6 | 1166 µs | 0 |

### Mixed flows (CEIO Table 4)

Eight flows in total, some KV and some LineFS, running steadily. Each cell is
KV Mpps / LineFS Gbps / P99.9 latency in µs.

```
ceio_run --preset=mixed --cpu-flows=N --scheme=S
```

| KV : LineFS | Baseline | HostCC | ShRing | Toasty | CEIO |
|---|---|---|---|---|---|
| 6 : 2 | 25.3 / 46 / 80 | 13.3 / 49 / 44 | 26.9 / 49 / 359 | 23.7 / 49 / 2147 | 21.4 / 49 / 192 |
| 4 : 4 | 16.5 / 96 / 126 | 10.1 / 100 / 44 | 16.5 / 100 / 533 | 16.9 / 100 / 2122 | 14.6 / 100 / 298 |
| 2 : 6 | 7.1 / 141 / 466 | 8.4 / 150 / 69 | 8.0 / 149 / 1107 | 8.2 / 150 / 85 | 8.1 / 149 / 280 |

### How this compares to the paper

Several things agree. The baseline gets worse as LineFS flows replace KV
flows: four KV cores reach 12.7 Mpps when they would manage about 4 Mpps each
on their own. ShRing beats the baseline, as the paper says (up to 1.7x
there). CEIO is the only scheme that never drops a packet, and the LineFS
flows keep their throughput under it.

The main disagreement is throughput. Here CEIO does not beat the baseline,
ShRing or Toasty on KV throughput, while the paper reports 2 to 2.9 times
more. HostCC also ends up below the baseline, where the paper has it up to
1.3 times above.

## 3. Why CEIO's speedups don't reproduce

These are the reasons we found, roughly in order of how much they matter.

**The baseline hardly suffers from leaky DMA here.** DCTCP reacts to drops
quickly enough that each core's backlog stays at a few hundred packets, and
the baseline KV cores keep a cache hit rate of 0.98. CEIO gains by removing
cache misses, and the paper reports 88 % misses for its baseline. We only
see backlogs like that under sustained overload with no congestion control,
and there CEIO does win (hit rate 0.29 to 0.50, with more goodput).

**CEIO's congestion trigger trades throughput against latency, and the paper
doesn't give its setting.** CEIO tells the sender to slow down once a flow's
backlog in NIC memory passes some threshold. We limited these notifications
to one per 50 µs per flow and tried a few thresholds:

| Threshold | KV throughput | Median latency |
|---|---|---|
| 32 KB | 22.6 Mpps | 1.8 µs |
| 512 KB or more | 33 to 34 Mpps (the baseline gets 31.6) | about 1.5 ms |

We couldn't find a setting that gives both. We use 32 KB because it matches
the paper's description: slow the sender down as soon as the slow path fills
faster than it drains.

**The slow path is expensive for small packets.** With the NIC-core cost set
to match the paper's Fig. 11, the slow path runs at about a third of the fast
path for KV-sized packets. CEIO's credit total also lets in too many small
packets (spec.md, Sec. 5.4), so more of them overflow to the slow path than
should.

**The paper leaves several details out.** These include how hard the KV
clients push, ring sizes, DCTCP settings, the LineFS message size, and
whether ShRing's shared ring keeps the cores of flows that have left. We
picked values for each (they're in `src/exp/presets.cpp`), and each one
shifts the absolute numbers.

Three of these points line up with what CEIO-W changes: credits that count
buffers but not cache footprint, no say over which buffers get reused, and a
congestion trigger that can't be tuned for both throughput and latency. All
CEIO-W comparisons below are against this simulated CEIO, in the same model.

## 4. CEIO-W

CEIO-W ([ceio-w.md](ceio-w.md)) runs on the same model, workloads and
congestion control as everything else, including CEIO's 32 KB trigger.

### Cache footprint

One KV core with 512-byte packets and a 4096-entry ring, without congestion
control.

| Load | Scheme | Goodput | KV hit rate | PCIe write hits | Dead writebacks | DRAM writes | Buffers used |
|---|---|---|---|---|---|---|---|
| 15 Gbps (below capacity) | CEIO | 15.0 Gbps | 0.995 | 0.10 | 56,456 | 1.85 GB/s | 8,192 |
| 15 Gbps | CEIO-W | 15.0 | 0.996 | 0.99 | 0 | 0.00 | 20 |
| 26 Gbps (overloaded) | CEIO | 16.2 | 0.46 | 0.52 | 14,230 | 1.82 | 8,192 |
| 26 Gbps | CEIO-W | 23.9 | 1.00 | 1.00 | 1 | 0.00 | 907 |

CEIO limits how many packets sit in the cache, but not which buffers they
land in, so it touches all 8,192 of them. With CEIO-W the buffers in use
stay within the credit limit, and the total (posted plus unprocessed) never
went above CEIO's credit total.

### Throughput in the paper's scenarios

KV Mpps, with P99.9 latency in µs in brackets where it's useful. Neither CEIO
nor CEIO-W dropped a packet in any of these runs.

| Scenario | Baseline | HostCC | ShRing | Toasty | CEIO | CEIO-W |
|---|---|---|---|---|---|---|
| Dynamic flows, by phase | 30.6 / 22.1 / 12.7 | 23.4 / 15.2 / 10.3 | 35.0 / 41.0 / 28.2 | 30.7 / 31.1 / 23.1 | 16.4 / 21.6 / 12.4 (1132) | 29.3 / 19.4 / 17.5 (283) |
| Bursts, by phase | 32.0 / 35.1 / 34.6 / 35.3 | 23.2 / 21.7 / 35.4 / 22.7 | 35.4 / 34.6 / 38.6 / 34.4 | 30.7 / 38.5 / 37.9 / 38.5 | 16.5 / 25.0 / 29.2 / 20.6 (1166) | 29.3 / 26.1 / 37.9 / 22.7 (438) |
| Mixed 6:2 | 25.3 | 13.3 | 26.9 | 23.7 | 21.4 | 22.3 |
| Mixed 4:4 | 16.5 | 10.1 | 16.5 | 16.9 | 14.6 | 14.3 |
| Mixed 2:6 | 7.1 | 8.4 | 8.0 | 8.2 | 8.1 | 10.5 |
| 8 KV flows | 35.9 | 34.7 | 36.5 | 31.5 | 25.5 | 30.3 |
| 8 KV flows next to a 100 GB/s memory-heavy program | 2.8 (3255) | 5.4 (10) | 12.0 (696) | 18.1 (1124) | 2.8 (11226) | 25.3 (94) |

CEIO-W does better than CEIO in almost every row. In the dynamic-flow
scenario it averages 1.33 times CEIO's throughput with a quarter of its tail
latency, and in the 2:6 mixed case it gets 1.3 times as much.

The clearest result is the run next to a memory-heavy program. With DRAM
close to saturated, every DMA write into a cold buffer has to evict a dirty
line first, and the writeback stalls the PCIe write path. The baseline and
CEIO both fall from about 36 Mpps to 2.8. CEIO-W keeps reusing a small set of
buffers that are already in the cache, so it avoids most of those
writebacks, and it gets 25.3 Mpps. That's 1.4 times Toasty and 2.1 times
ShRing. Only HostCC has a lower tail latency, and it gets there by slowing
the senders down to 5.4 Mpps. During this run CEIO-W's budget shrank from
1536 buffers to 128.

Without memory pressure CEIO-W isn't the best scheme. ShRing and Toasty match
or beat it in several phases, and the baseline wins the plain 8-flow run.
CEIO-W uses CEIO's congestion trigger, which slows senders down more than
necessary, and section 3 showed that changing the threshold moves throughput
around in ways that are hard to predict.

### Which parts matter

Each row adds or removes one part of CEIO-W. Values are KV Mpps unless
marked otherwise.

| Variant | Overloaded core | Dynamic flows, whole run | Memory pressure |
|---|---|---|---|
| CEIO | 16.2 Gbps | 16.6, 455 K dead writebacks | 2.8 |
| CEIO with warm buffers only | 16.5 | 21.6, 321 K | 13.1 |
| CEIO-W | 23.9 | 22.1, 25 K | 25.3 (P99.9 94 µs) |
| CEIO-W with credits split evenly | 21.1 | 22.9, 52 K | 24.6 |
| CEIO-W with a fixed budget | 23.9 | 21.5, 22 K | 22.1 (P99.9 392 µs) |
| CEIO-W with a fixed budget equal to CEIO's total | 23.9 | 21.1, 29 K | not run |

Most of the gain comes from using the ring as the credit pool together with
warm buffers. Warm buffers alone help CEIO a little, but its credits still
let the set of buffers grow. Following demand matters when one queue could
otherwise take the whole budget: it's worth 13 % on the overloaded core. The
self-adjusting budget only matters when the usable cache really shrinks, as
it does under memory pressure, where it adds 14 % throughput and cuts the
tail latency by 4 times. In the other runs the targets never reach the
budget, so it doesn't do anything.

### Flow churn (CEIO Fig. 12)

N echo flows share 8 cores. In every time slot a new random set of 16 of
them is active, sending 60 Gbps of 512-byte packets in total, with no
congestion control. Each cell is the share of packets that went through the
slow path, then P99 latency in µs.

| Flows | Slot | Baseline | CEIO (1 s idle timer) | CEIO (idle timer = slot) | CEIO-W |
|---|---|---|---|---|---|
| 16 | 100 µs | 0 % / 1.4 | 0 % / 1.4 | 0 % / 1.4 | 0 % / 1.4 |
| 128 | 100 µs | 0 % / 1.4 | 0 % / 1.4 | 81 % / 2.6 | 0 % / 1.4 |
| 512 | 100 µs | 0 % / 1.5 | 0 % / 1.5 | 95 % / 2.7 | 0 % / 1.5 |
| 2048 | 100 µs | 0 % / 1.5 | 19 % / 2.5 | 98 % / 2.7 | 0 % / 1.5 |
| 2048 | 1 ms | 0 % / 1.4 | 28 % / 3.4 | 13 % / 2.5 | 0 % / 1.4 |

CEIO gives credits to flows, and that doesn't work well when flows keep
coming and going. With the 1-second idle timer from the paper, idle flows
hold on to their credits; at 2048 flows each one has about 1.5, so active
flows run out and spill to the slow path. Setting the timer to the slot
length frees credits in time, but flows are reactivated round-robin, 32
every 100 µs, which can't keep up, and up to 98 % of packets end up on the
slow path. The CEIO paper reports the same problem.

CEIO-W keeps its credits per receive queue, which is 8 entries here instead
of 2048. It never used the slow path and kept the baseline's latency
everywhere. Throughput was 60 Gbps for every scheme, since the slow path had
enough capacity at this load, so churn only showed up as extra latency.

### Different seeds

Five seeds, KV Mpps as the mean with the lowest and highest in brackets:

| Run | Toasty | CEIO | CEIO-W |
|---|---|---|---|
| Next to a 100 GB/s memory-heavy program | 18.6 (17.8 to 19.4) | 2.8 (2.8 to 2.8) | 25.2 (25.1 to 25.3) |
| Dynamic flows, whole run | 28.2 (28.1 to 28.2) | 17.1 (16.6 to 17.6) | 22.2 (22.0 to 22.6) |

### Caveats

- This is one simulated model. Absolute numbers depend on the calibration in
  spec.md. CEIO-W's gains come mostly from cache and DRAM behaviour that the
  model represents directly, but it has no private L2 or prefetcher, so its
  throughput numbers should be read with care (spec.md, Sec. 3.5).
- Flows that share a receive queue share its credits. How fairly they share
  them is up to the application, as with any shared ring.
- CEIO-W needs a NIC that can send a packet to its own memory when a receive
  ring is empty, instead of dropping it. That hasn't been tested on real
  hardware.
- CEIO-W uses CEIO's congestion trigger. A trigger designed for CEIO-W could
  change the comparisons without memory pressure, in either direction.
