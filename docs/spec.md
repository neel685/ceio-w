# Simulator specification

This document describes what the simulator models, which parameters it uses,
and where each value comes from. Every number in a default configuration
should be listed here with its source. Values in the calibration tables don't
come from either paper; they are taken from datasheets or common
measurements, and they are the first things to revisit when comparing against
real hardware.

The two papers referred to throughout:

- [CEIO] B. Liu et al. CEIO: A Cache-Efficient Network I/O Architecture for
  NIC-CPU Data Paths. SIGCOMM 2025.
- [Toasty] Preeti et al. Toasty: Speeding Up Network I/O with Cache-Warm
  Buffers. ASPLOS 2026.

## 1. Scope

The simulator models the receive path of one server, plus the transmit path
where replies are needed:

```
 senders --network--> NIC (flow rules, NIC cores, on-NIC memory)
                        |  PCIe (writes, credits, DMA reads, MMIO)
                        v
                     IIO buffer --> LLC (DDIO ways) <--> DRAM
                                        ^
                                   CPU cores (driver + application)
```

Time is kept in picoseconds, the LLC is tracked one 64-byte line at a time,
and every packet buffer has a physical address. That last part is what lets
the model tell apart which buffer a packet lands in, not only how many
packets are in flight.

The model leaves out the private L1 and L2 caches (their cost is folded into
a per-packet CPU cost), cache-coherence details, PCIe link-layer retries, and
anything inside the NIC below the level of flow rules, counters and the NIC
cores' polling loop.

## 2. Questions the model needs to answer

| Topic | Question | What has to be measured |
|---|---|---|
| Cache footprint | How much write-allocate and writeback traffic does CEIO leave behind when it keeps the number of packets in the cache bounded, but its buffers are spread over more cache than DDIO has? | PCIe write hit rate, DRAM write bytes, dead writebacks |
| Throughput | Do warm-buffer credits (CEIO-W) improve throughput and DRAM bandwidth, especially when another program is using the cache? | Throughput, CPU cache misses, DRAM bandwidth |
| Flow churn | Does keeping credits per receive queue avoid CEIO's fall to the slow path when more than about a thousand flows come and go quickly? | Fast/slow split, throughput against flow count |
| Loss and ordering | Does CEIO-W keep CEIO's tolerance of bursts: no drops and no extra congestion signals? | Drops, ECN marks, tail latency |

## 3. Parameters

### 3.1 CEIO testbed

| Parameter | Value | Source |
|---|---|---|
| Link rate | 200 Gbps | [CEIO] Sec. 2.3 |
| CPU | 2x Xeon Silver 4309Y, 16 cores per socket, 2.8 GHz | [CEIO] Sec. 2.3 |
| LLC per socket | 12 MB, 12 ways | [CEIO] Sec. 4.1 ("6 out of 12 cache ways") and Sec. 6.1 ("LLC capacity (12MB)") |
| DDIO capacity | 6 MB (6 of the 12 ways) | [CEIO] Sec. 4.1 |
| I/O buffer size | 2 KB (MTU 1500 B) | [CEIO] Sec. 4.1 |
| Total credits | 3072 (6 MB / 2 KB; the paper rounds to 3000) | [CEIO] Sec. 4.1, Eq. 1 |
| PCIe | Gen5 x16 | [CEIO] Sec. 2.3 |
| DRAM | DDR4-3200, 8 channels | [CEIO] Sec. 2.3 |
| NIC | BlueField-3: 16 GB of on-NIC memory, Arm cores, flow-rule engine | [CEIO] Sec. 3 and 5 |
| Time per 1024 B packet at 200 Gbps | 41.8 ns | [CEIO] Sec. 1 |
| Cost of an LLC miss | more than 100 ns | [CEIO] Sec. 1 |
| Slow-path trip between NIC and CPU | up to 1000 ns | [CEIO] Sec. 3 |
| ShRing shared ring | 4096 entries | [CEIO] Sec. 6.1 |
| Idle-flow timer | 1 s | [CEIO] Sec. 4.1 |

### 3.2 Toasty testbed

| Parameter | Value | Source |
|---|---|---|
| Link rate | 100 Gbps (Intel E810) | [Toasty] Sec. 2 |
| CPU | Xeon Gold 5418Y | [Toasty] Sec. 2 |
| LLC | 45 MB, 15 ways; DDIO uses the last 2 ways (6 MB) | [Toasty] Sec. 2 |
| Buffer size and packet size | 4 KB and 512 B | [Toasty] Sec. 2 |
| Buffers that fit in the DDIO ways | at least 1536 | [Toasty] Sec. 2 |
| Receive ring | 4096 descriptors | [Toasty] Sec. 2 |
| Default buffer pool (UMEM) | 16384 buffers | [Toasty] Sec. 2 |
| Refill multiplier `k` | 10 | [Toasty] Sec. 3.2 |
| Link-utilisation threshold | 50 % | [Toasty] Sec. 3.2 |
| Microburst | 2048 packets of 512 B every 1 ms | [Toasty] Sec. 2 |
| Poisson traffic | rate redrawn every 5 ms around the app's capacity | [Toasty] Sec. 2 |
| Cost of a LIFO swap | 4.9 ns | [Toasty] Sec. 4.6 |

### 3.3 Hardware calibration (not in either paper)

| Parameter | Value | Reasoning |
|---|---|---|
| LLC hit latency | 20 ns | Typical for Ice Lake server L3 |
| DRAM latency when idle | 90 ns | Typical DDR4; adds up to more than 100 ns once queueing is included |
| DRAM bandwidth per channel | 19.2 GB/s | 75 % of DDR4-3200's 25.6 GB/s peak |
| Cache misses a core can have outstanding | 10 | Number of line-fill buffers |
| PCIe Gen5 x16 rate per direction | 504 Gbps | 32 GT/s x 16 lanes x 128/130 encoding |
| PCIe max payload size | 256 B | Common server default |
| Overhead per PCIe packet (TLP) | 24 B | Framing, header, CRC and link-layer packets |
| PCIe one-way latency | 300 ns | Half of a 600 to 1000 ns register read round trip |
| IIO write buffer | 64 KB | Rough size of real IIO buffers |
| IIO time per line, cache hit | 0.5 ns | Faster than an allocation |
| IIO time per line, allocation | 1 ns | 64 GB/s, so the IIO never limits a Gen5 x16 link while DRAM is idle |
| DRAM write queue | 32 lines per channel | An IIO write that forces a writeback only waits if more than this is queued |

### 3.4 CPU and application calibration

| Parameter | Value | Checked against |
|---|---|---|
| Core clock | 2.8 GHz (CEIO), 2.0 GHz (Toasty) | Testbed CPUs |
| Poll loop | 60 cycles per poll, up to 32 packets per batch, 20 ns to wake up | DPDK and AF_XDP busy polling |
| Driver refill | 8 cycles plus one 16 B descriptor write per buffer | |
| Prefetching | While handling one packet, fetch the next packet's completion entry and first 128 B | DPDK and AF_XDP loops |
| NAT | 51 cycles, a 64 B read and write, one lookup in a 1 MB table | 52.9 Gbps at the best pool size ([Toasty] Sec. 4.6); the model gets 50.4 |
| L2Fwd | 90 cycles, a 64 B read and write, then forward | About 78 Gbps ([Toasty] Fig. 6); the model gets 77.3 |
| IDS, echo, KV, RDMA sink | see `src/host/app.cpp` | Reasonable values, not fitted to any published number |
| Memory-heavy co-runner | random 64 B accesses over 512 MB at a set rate | Intel MLC, as in [Toasty] Sec. 4.3 |
| NIC on-chip buffer | 1 MB shared; a queue with no posted buffers may hold 32 KB | Matches ConnectX's per-queue out-of-buffer drops |

Saturation throughput means the highest load, found by bisection, at which
no more than 0.1 % of packets are dropped and the backlog doesn't keep
growing (`find_saturation`), as in [Toasty] Sec. 4.1. Offering more than a
core can handle keeps its ring full of unread packets and exaggerates leaky
DMA, which is why we search for this point instead.

### 3.5 What the model gets wrong

- **Wasted writebacks cost less throughput than on real hardware.** The
  model shows the same cause as Toasty: with a large FIFO pool, PCIe write
  hits drop from about 0.9 to 0.1, dead writebacks go up 4 to 10 times, and
  DRAM write traffic roughly triples. But throughput only falls by 13 % for
  NAT, against up to 78 % in the paper. Private L2 caches, hardware
  prefetchers and the non-inclusive LLC aren't modelled, so a wasted
  writeback only costs DRAM bandwidth here. Results about cache footprint and
  DRAM traffic are more trustworthy than throughput, which is probably on the
  low side.
- **DRAM is simple.** It is modelled as bandwidth plus queueing, with no
  banks or row buffers, so latency under load grows more slowly than it
  would on real hardware.

## 4. How the cache works in the model

The LLC is set-associative with true LRU in each set and 64-byte lines. Sets
are picked the way Intel does it: a hash of the address picks the slice, and
the low address bits pick the set inside the slice. Because of this, a packet
much smaller than its buffer only ever lands in a fraction of the sets. That
is why Toasty counts DDIO capacity in whole buffers (6 MB / 4 KB = 1536).
Plain hashing and plain modulo indexing are also available for tests.

- A DMA write from the NIC that finds its line anywhere in the cache updates
  it in place, which counts as a PCIe write hit. Otherwise the line is
  allocated in the DDIO ways, evicting the least recently used line there.
- A DMA read by the NIC is served from the cache if the line is there and
  from DRAM if not. It never allocates.
- CPU reads and writes can allocate in any way. Writes mark the line dirty.
- Evicting a dirty line writes it back to DRAM.

Two flags per line are used only for counting, never for decisions:

- `io_unread` marks a line the NIC wrote that the CPU hasn't read yet.
  Evicting one is a leaky-DMA eviction.
- `dead` marks a line whose buffer the driver has already recycled. Writing
  one back is a dead (unnecessary) writeback.

## 5. Algorithms

### 5.1 CEIO's credit management ([CEIO] Algorithm 1)

With `n` existing flows and `m` new ones, each flow's share is
`C_flow = C_total / (n+m)`.

When new flows join, each existing flow `i`:

- gives `C_flow/n` credits to each new flow, if it has at least
  `(m/n) * C_flow`;
- otherwise splits what it has evenly among the new flows and records that it
  still owes each of them the difference.

When flow `i` gets `g` credits back: if it owes nothing, it keeps them all. If
it owes other flows, it first pays each of them up to `g/m` of what it owes,
and keeps the rest. Once every debt is paid, it's back to normal.

The algorithm as printed has three problems, and here is how we read each:

- Lines 21 and 22 use `max(o_ij, g/m)`, which would make debts and released
  credits negative. We use `min`.
- The loops run `for j = n to n+m`, one step too many. We use `n+1` to `n+m`.
- Line 12 takes `c_i/m` per new flow and line 14 subtracts it each time, which
  shrinks geometrically instead of splitting evenly. We split evenly, and keep
  the literal version behind a flag (`literal_split`) for comparison.

Owed credits are paid directly to the flow they're owed to, not into a shared
pool.

### 5.2 CEIO's other behaviour ([CEIO] Sec. 4.1 and 4.2)

- Each packet on the fast path uses one credit, which stands for one buffer.
- Credits come back late: only when the driver moves the ring's head pointer
  after a batch. The NIC cores notice by polling.
- When a flow runs out, its flow rule is switched to write into on-NIC memory
  (the slow path). The NIC cores notice by polling the rule's hit counter, so
  there is some delay.
- To switch back, the fast path is paused, the slow-path backlog is moved to
  host memory, and then the fast path is turned back on. This keeps packets
  in order.
- If the slow path fills faster than it drains, the sender is told to slow
  down.
- A flow counts as idle after 1 s without packets, or when its fast-path
  packets are still unprocessed when a slow-path drain starts. Idle flows'
  credits are handed out again, and idle flows are reactivated round-robin.
- The application reads through one ring that merges the fast and slow
  paths. `recv()` waits for slow-path data; `async_recv()` returns fast-path
  packets right away and fetches slow-path ones in the background.

### 5.3 Choices we made where the paper is vague (`src/policy/ceio.cpp`)

| Topic | What we did | Why |
|---|---|---|
| Credit unit | One credit is one buffer; the total is DDIO size / buffer size (3072 for 6 MB and 2 KB) | Eq. 1 |
| Noticing that credits ran out | NIC cores poll the hit counter every 200 ns plus 10 ns per flow, so flows can overdraw. An `ideal_detection` option checks every packet instead | Sec. 4.1 says the controller "continuously polls counters" |
| Returning credits | The host returns credits once per batch with a register write; the NIC cores apply them on their next pass | Sec. 4.2 |
| Fetching slow-path data | `recv()` only fetches when the fast ring is empty, and waits. `async_recv()` keeps up to 64 slow-path packets in flight. The host only fetches for flows that don't have enough credits for their backlog | Sec. 4.2 |
| Switching back to the fast path | Once a slow flow has credits for its whole backlog, the NIC holds new arrivals on-chip, pushes the backlog into the fast ring (one credit each), then lets the held packets through and switches the rule back. Held packets beyond the available credits go back to on-NIC memory | Sec. 4.1 describes pausing, draining and re-enabling; this keeps every flow in order without any reordering |
| RDMA flows on the slow path | Drained by the NIC cores when the last packet of a message arrives | Sec. 4.1 |
| NIC cores | 16 Arm cores; each flow is pinned to one; moving a slow-path packet takes 150 ns, so about 6.7 Mpps per flow | Sec. 4.1 assigns a NIC core to each flow. This puts one flow's slow path at about a third of the fast path at 512 B to 1 KB and equal at 4 KB, as in Fig. 11 |
| On-NIC memory | 16 GB, 500 ns latency, 400 Gbps | BlueField-3. The slow path adds about 1.5 µs at low load, in line with Table 3 |
| Telling the sender to slow down | Once a flow has more than 32 KB waiting in NIC memory, its slow-path packets are ECN-marked and the NIC notifies the sender directly, at most once per 50 µs per flow | Sec. 4.1; the 50 µs matches DCQCN's notification timer |
| Idle flows | 1 s timer; credits handed out again; 32 flows reactivated every 100 µs | Sec. 4.1 |
| RDMA packet size | 4 KB | The usual RoCE MTU; also keeps the per-packet NIC-core cost reasonable for large transfers |

### 5.4 What we found while reproducing CEIO

**Eq. 1 sets the credit total too high for small packets.** With 512-byte
packets in 2 KB buffers, the way Intel picks cache sets puts every packet on
a quarter of the sets, so 3072 credits overfill the DDIO ways. On an
overloaded KV core with a 4096-entry ring:

| Credits | Core hit rate | Goodput |
|---|---|---|
| 3072 | 0.50 | 16.3 Gbps |
| 768 | 0.85 | 18.8 Gbps |
| 128 (almost everything pulled from the slow path) | 1.00 | 23.9 Gbps |

The credits count buffers, not how much cache those buffers take up. This is
the main motivation for CEIO-W.

**CEIO is sensitive to the size of the buffer pool.** CEIO limits how many
packets sit in the cache, but its rings and drain buffers still cycle through
the whole pool in FIFO order. Growing the pool from 8 K to 10 K buffers
lowers CEIO's hit rate in the same scenario.

**Mixed flows need congestion control to compare fairly.** If senders never
slow down, an overloaded KV flow stays on the slow path for good, shares NIC
cores with RDMA drains, and loses throughput. For that reason the mixed-flow
comparisons are all run with DCTCP.

### 5.5 Baselines (`src/net/congestion.cpp`, `src/policy/hostcc.cpp`, `src/exp/presets.cpp`)

**DCTCP** is modelled by rate rather than window. Every 20 µs (one RTT) the
sender looks at the acknowledgements from the last window, which arrive 5 µs
after delivery. If some were ECN-marked, it cuts its rate by alpha/2, where
alpha is a running average of the marked fraction (gain 1/16). A loss halves
the rate. A clean window adds one packet per RTT, up to what the application
wants to send.

**HostCC** checks the IIO buffer every microsecond and treats more than 8 KB
as congestion. While congested, it ECN-marks incoming packets and halves the
memory bandwidth of the co-runner at each check, never below 10 %. Once the
congestion clears, the co-runner gets 5 % of its bandwidth back per check.

**ShRing** gives all the CPU-involved cores one shared 4096-entry ring
([CEIO] Sec. 6.1) and one 8192-buffer pool. Any core can pick up any flow's
packet, so packets of one flow can finish out of order. That comes with
sharing a ring.

**Toasty** uses a LIFO pool, the refill rule from section 5.6 (k = 10, 50 %
threshold, never fewer than 64 buffers posted before traffic starts), and
sends replies as soon as they're ready. The other schemes send replies in one
burst at the end of each receive batch, as DPDK's `tx_burst` does.

**The paper scenarios** use a direct link between the two machines (no
switch ECN), DCTCP, and a 1 ms warm-up. KV flows send 144-byte packets and
each wants 8 Gbps; each core has a 4096-entry ring and 8192 buffers. LineFS
flows write 25 Gbps over RDMA in 1 MB messages of 4 KB packets. Every flow
starts at a slightly different, fixed random time, so identical flows don't
stay in lockstep.

### 5.6 Toasty ([Toasty] Sec. 3)

**LIFO fill queue.** After the application recycles a batch of buffers, each
one is swapped with the descriptor at the front of the queue, so the next
buffer handed to the NIC is the one freed most recently (Listing 1). Each swap
costs 4.9 ns.

**Adaptive refill** (Pseudocode 1, checked against the PDF) runs once per
driver iteration and uses:

- `N_FQ`, buffers recycled since the last iteration
- `N_RXQ`, packets the NIC wrote since the last iteration
- `N_avail`, free descriptors currently in the receive ring
- `k`, the refill multiplier

```
if N_avail < k*N_RXQ:
    if rate(N_RXQ) < 50% of link:  post min(k*N_RXQ, N_FQ)
    else:                          post k*N_RXQ - N_avail
else:
    if rate(N_RXQ) < 50% of link:  post nothing
    else:                          post min(k*N_RXQ, N_FQ)
```

The ring's capacity is always the upper limit. On the transmit side, sent
buffers are freed right away so warm buffers come back to the pool sooner.

## 6. What the reproduction should show

We aim to reproduce trends and the order of results, not the exact numbers
measured on hardware.

| Paper figure | Expected trend |
|---|---|
| [CEIO] Fig. 4 | HostCC and ShRing fall short when flows change and during bursts |
| [CEIO] Fig. 9 | The baseline misses the cache a lot (88 % in the paper) and CEIO hardly at all; throughput goes CEIO, ShRing, HostCC, baseline |
| [CEIO] Fig. 11 | The fast path is as fast as plain RDMA; the slow path is within about 22 % for messages of 4 KB or more |
| [CEIO] Table 3 | 1.10 to 1.48 times the latency; the slow path is worse for large messages |
| [CEIO] Fig. 12 | Throughput holds with time slots of 1 ms or more, and drops to slow-path level beyond about 1K flows with 100 to 500 µs slots |
| [Toasty] Fig. 2 | Throughput rises and then falls with pool size, peaking near the DDIO capacity |
| [Toasty] Fig. 4 | Toasty keeps reusing a small set of buffers; FIFO uses all of them evenly |
| [Toasty] Tables 2, 3 and 7 | A small fixed pool drops packets in bursts; Toasty drops about as few as a large pool |
| [Toasty] Fig. 18 | Neither the LIFO pool nor the refill rule alone reaches full Toasty |

## 7. Reproducibility

- All randomness comes from one seeded xoshiro256** generator. Each part of
  the simulator draws from its own named stream, so adding a new part doesn't
  change the numbers of the others.
- The random distributions are written by hand, because the standard
  library's differ between compilers.
- Events at the same time run in the order they were scheduled.
- Every run's results include its full configuration and seed.
