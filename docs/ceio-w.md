# CEIO-W

CEIO-W keeps everything CEIO does: packets normally go straight into the LLC,
packets that don't fit wait in the NIC's own memory, and a flow switches
between the two paths without its packets getting out of order. What changes
is what a credit means and who keeps track of it. The idea comes from
Toasty: it matters which buffers get reused, not only how many packets are in
flight.

## Why change CEIO

These are problems with CEIO that showed up while reproducing it in the
simulator (the numbers are in [results.md](results.md)).

- **Credits ignore which buffers are used.** A credit stands for one packet
  in flight. Buffers that are posted but still empty don't count, and a FIFO
  pool keeps cycling through all of its buffers. With the same credits, a
  pool of 10 K buffers gets a lower cache hit rate than a pool of 8 K.
- **The credit total is too high for small packets.** CEIO sizes the total as
  the DDIO cache size divided by the buffer size (Eq. 1 in the paper). That
  assumes buffers fill the cache perfectly. With 512-byte packets in 2 KB
  buffers they only land in a quarter of the cache sets. On an overloaded
  key-value core, 3072 credits give a hit rate of 0.50 and 768 credits give
  0.85.
- **Running out of credits is noticed late.** The NIC cores find out by
  polling packet counters, so a flow can overdraw while they catch up. The
  slower the polling loop, the larger the overdraw (more than 10 credits at a
  20 µs loop).
- **Credits are split evenly**, and bulk RDMA flows only lose theirs because
  CEIO returns credits late, once a whole message is done. That works for
  4 MB messages but not for 64 KB ones.

## What CEIO-W does

### The ring is the credit pool

The driver never posts more buffers to a receive queue than the queue's
target. A credit is a buffer that is either posted or holding a packet the
application hasn't finished yet, so the limit covers every buffer the queue
can touch, empty ones included.

When the ring is empty, the NIC sends the packet to its own memory instead,
just like CEIO's slow path, and the rest of that flow's packets follow until
the backlog is cleared. There is no counter to poll: an empty ring is the
signal. Switching back to the fast path works the same way as in CEIO. The
NIC holds new arrivals for a moment, pushes the backlog into the ring, and
then lets the held packets through, so nothing is reordered.

This needs one thing from the hardware: when a receive queue has no posted
buffer, the NIC must be able to redirect the packet rather than drop it. On
BlueField this would be done by the data-path accelerator or a flow-table
miss rule. We think that is a smaller ask than CEIO's, which needs NIC cores
polling counters while packets are flowing, but it hasn't been tried on a
real NIC.

### Warm buffers

Freed buffers go on a stack (LIFO) and replies are sent as soon as they are
built, as in Toasty. Refills, slow-path drains and fast-path re-enables all
pick up the most recently freed buffers, which are likely still in the cache.
As a result the set of buffers in use stays close to the credit limit.

### Credits follow demand

Each queue keeps a running average of how many packets it handles per driver
batch, and its target is ten times that (`k = 10`). This is the same estimate
Toasty uses to decide how many buffers to post. If the targets add up to more
than the budget, they are all scaled down, but no queue goes below 16
buffers.

RDMA flows get a fixed cap of 64 credits. The CPU never reads their data, so
keeping it in the cache doesn't help anyone. They are recognised by their
transport type, so applications don't have to label anything.

### The budget adjusts itself

The budget starts at half of CEIO's credit total. Every 100 µs the host looks
at its own cores' LLC hit and miss counters, which real CPUs expose through
performance counters. If more than 5 % of loads miss, the budget is cut to
80 %. Otherwise it grows back by 5 % of CEIO's total, up to that total. This
loop only estimates how much cache is really available. It doesn't throttle
the traffic itself, which is what made HostCC slow to react.

### Where the state lives

Credits are tracked per receive queue, not per flow, so the bookkeeping grows
with the number of cores and not with the number of flows. Flows that share a
queue share its credits, but each flow still switches between the fast and
slow path on its own and keeps its packets in order. All of this runs in the
host driver, at the same points where CEIO already returns credits. The NIC
cores only move data on the slow path.

## How it is evaluated

- **Cache footprint.** CEIO and CEIO-W under overload and in the paper's
  scenarios. We look at how many different buffers are used, the PCIe write
  hit rate, dead writebacks and DRAM write traffic.
- **Throughput and latency.** The paper's dynamic-flow, burst and mixed-flow
  scenarios, plus a run next to a memory-heavy program, compared against all
  the other schemes.
- **Loss and ordering.** In every run, no packet may be dropped or delivered
  out of order.
- **Flow churn.** 16 to 2048 flows spread over 8 queues, with a new random
  set of 16 active flows every 100 µs or 1 ms.
- **Ablation.** CEIO with only the LIFO pool added, and CEIO-W with an even
  split of credits, with a fixed budget, and with a fixed budget equal to
  CEIO's total.

## Settings

These live in `policy::CeioConfig`, with `variant = kCeioW`:

| Field | Default | Meaning |
|---|---|---|
| `w_k` | 10 | target = this many times the recent packets per batch |
| `w_min_target` | 16 | smallest target a queue can get |
| `w_bulk_credits` | 64 | cap for RDMA flows |
| `w_budget_fraction` | 0.5 | starting budget, as a fraction of CEIO's total |
| `w_alloc_period` | 20 µs | how often targets are recomputed |
| `w_budget_period` | 100 µs | how often the budget is adjusted |
| `w_miss_target` | 0.05 | miss rate above which the budget shrinks |
| `w_adaptive_alloc` | true | turn off to split the budget evenly |
| `w_adaptive_budget` | true | turn off to keep the budget fixed |
