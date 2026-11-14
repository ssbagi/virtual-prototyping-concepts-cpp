# Virtual-Prototyping-Concepts-C++

C++ examples for exploring Virtual Prototyping concepts and system behaviors in chip/SoC modelling, including buffer backpressure, livelock, starvation, and related performance scenarios.

This also fits your longer-term approach nicely: concept → small C++ model → observe behavior → understand mechanism → document → share as proof of learning.

<!--
Author  : Shreyas S Bagi
Note    : Developed with AI assistance (IBM Bob).
Purpose : Created for learning and knowledge purposes only.
-->

# starvation_demo

Three small, self-contained C++17 models that show how **starvation**, **livelock** and
**backpressure** problems arise in a shared-resource design, and which rules prevent them. Each model has a narrated demo and a
self-checking test suite. There are no external dependencies.

> Developed with AI assistance (IBM Bob) for learning and knowledge purposes only.

## What is in here

| File | Role |
|---|---|
| `arbiter.h` | One-grant-per-cycle arbiter model with three policies: `Fixed`, `RoundRobin`, `Aging` |
| `bounded_buffer.h` | Bounded-buffer + single-port model with three variants: `Buggy`, `ResultKept`, `ReservedSlot` |
| `backpressure.h` | Producer -> bounded FIFO -> slow consumer model with five flow-control schemes: `None`, `ReadyValid`, `DelayedStall`, `Watermark`, `Credit` |
| `logger.h` | Tiny UVM-style reporting facility (severities, verbosity, ids, trace window, summary) and the run-time author banner |
| `arbitration_starvation.cc` | Demo 1: which arbitration policy starves whom |
| `bounded_buffer_livelock.cc` | Demo 2: full-buffer livelock and head-of-queue starvation |
| `backpressure_scenarios.cc` | Demo 3: six backpressure scenarios (data loss, late stall, watermark, credits) |
| `tests.cc` | Self-checking tests (no framework); exits non-zero on any failed check |
| `Makefile` | Build, run, test and log-generation targets |
| `build/` | Generated binaries (`make`) |
| `log/` | Generated logs (`make logs`) |

## Quick start

```sh
make          # build all programs into build/
make run      # run all three demos
make test     # build and run the tests (non-zero exit on failure)
make logs     # write every demo at two verbosity levels, plus the test run, into log/
make clean    # remove build/ and log/
```

Requires a C++17 compiler (`g++` by default; override with `CXX=clang++ make`).

## Demo 1: arbitration starvation

Setup: 4 requesters all bid for one shared resource every cycle. The resource has a single port,
so exactly one request is granted per cycle.

| Policy | Rule | Outcome |
|---|---|---|
| `Fixed` | Lowest index always wins | Requester 0 gets every grant; requesters 1..3 **starve** |
| `RoundRobin` | A pointer rotates over the requesters | Perfectly fair; wait bounded by `requesters - 1` cycles; no priority |
| `Aging` | Fixed priority, but a requester that has waited `>= age_limit` cycles is promoted (longest waiter first) | Priority is kept and nobody starves; wait is bounded by about `age_limit + requesters` |

Takeaway: starvation is a property of the **selection rule**, not of the resource.

## Demo 2: bounded-buffer livelock

Model:

- `q`: pending queue, oldest request first. A request leaves `q` only when it completes.
- `buf`: bounded FIFO of request ids (capacity 4 in the demo).
- `port`: one service per cycle. A service pops `buf.front()`.

Each cycle every request in `q` is visited oldest-first. A request not in `buf` tries to insert; a
request in `buf` tries the port and completes only if `buf.front()` is its own entry.

| Variant | Behaviour | Outcome |
|---|---|---|
| `Buggy` | A queue-only service pops the front but **drops the result** | **Livelock**: port busy every cycle, 0 completions |
| `ResultKept` | The popped owner is marked ready and completes without the port | Others progress, but younger requests refill the buffer first, so the **oldest request starves** |
| `ReservedSlot` | `ResultKept` plus one slot reserved for the oldest request (others fill `<= cap-1`) | Oldest request completes within a few cycles, then about 1 completion per cycle |

Takeaway: never waste a service (keep the result) **and** guarantee the oldest requester a slot.

## Demo 3: backpressure scenarios

Setup: a producer has data **every** cycle and feeds a FIFO of 8 entries. The consumer drains one
item every 4 cycles, so the producer is 4x faster. A stall flag or a returned credit needs 3 cycles
to reach the producer. The best possible result is 100% consumer utilisation with 0 dropped items.

| Scenario | Rule | Outcome |
|---|---|---|
| `None` | Producer ignores the FIFO state | FIFO overflows; most sent items are **dropped** (data loss) |
| `ReadyValid` | Producer sends only when the FIFO has space this cycle | Lossless, consumer 100% busy; needs an immediate ready path |
| `DelayedStall` | Stall flag raised only when the FIFO is full, seen 3 cycles late | Still **overflows**: the signal latency is not covered |
| `Watermark` | Same delayed flag, raised early at a high watermark (`cap - latency`), released at a low one (half) | Lossless, consumer 100% busy, peak occupancy below capacity |
| `Credit` (credits = capacity) | One credit per FIFO entry, returned 3 cycles after each pop | Lossless by construction, consumer 100% busy |
| `Credit` (1 credit, 6-cycle return) | Credit round trip longer than the drain interval | No data loss, but the consumer is starved: **throughput loss** (bubbles) |

Takeaway: a stall that arrives late overflows, a stall that is too cautious starves the consumer.
Size the buffer for the round-trip latency: capacity >= flow-control latency + items needed to keep
the consumer busy.

## Block diagrams (RTL view)

The models are cycle-based C++, but each one maps onto ordinary synchronous hardware. These
diagrams show the same structures as an RTL engineer would draw them. Everything is clocked by a
single `clk`; `rst_n` (reset) is omitted from the boxes to keep them readable.

### How the pieces fit together (software structure)

```
   +--------------------------+   +--------------------------+   +--------------------------+
   |       arbiter.h          |   |    bounded_buffer.h      |   |      backpressure.h      |
   |  (DUT model 1: ARB)      |   |   (DUT model 2: BUF)     |   |   (DUT model 3: BP)      |
   +------------+-------------+   +-------------+------------+   +-------------+------------+
                |  RPT_INFO / RPT_WARNING / RPT_ERROR                          |
                +------------------------------+-------------------------------+
                                               v
                                 +----------------------------+
                                 |         logger.h           |    verbosity filter, per-id
                                 |  (report server, "UVM-lite")|   level, trace window,
                                 +-------------+--------------+    severity / id counters
                                               v
                               stdout  ->  log/*.log   (+ author banner)

   *_demo.cc / *_scenarios.cc  = "testbench" that drives a model and prints the RESULT block
   tests.cc                    = self-checking regression (CHECK == assert, exit code = pass/fail)
```

Think of each `*.h` model as the **DUT**, each demo `.cc` as a **directed testbench**, `tests.cc`
as the **regression**, and `logger.h` as a tiny **report server**.

### Demo 1: arbiter (`arbiter.h`)

```
                         +-----------------------------------------------------+
   req[0] -------------->|                                                     |
   req[1] -------------->|              ARBITER   (N = 4 requesters)           |-----> gnt[3:0]
   req[2] -------------->|                                                     |      one-hot,
   req[3] -------------->|    policy = FIXED | ROUND_ROBIN | AGING              |      1 grant / cycle
                         |                                                     |
                         |   +------------------+     +---------------------+  |
                         |   | rr_ptr           |     | wait_cnt[i]  (x N)  |  |
                         |   | (ROUND_ROBIN)    |     | +1 if req && !gnt   |  |
                         |   | rr_ptr <= rr_ptr+1|    | =0 if gnt           |  |
                         |   |       mod N      |     | (AGING reads this)  |  |
                         |   +------------------+     +---------------------+  |
                         +-------------------------^---------------------------+
                                                   |
                                                  clk
```

Datapath of the three policies (what is behind the `policy` switch):

```
  FIXED          req[N-1:0] ---> [ priority encoder, lowest index wins ] ---> gnt

  ROUND_ROBIN    rr_ptr ---> [ one-hot decode ] ---------------------------> gnt
                      ^                                                       |
                      +------------- rr_ptr <= (rr_ptr + 1) mod N <-----------+

  AGING          wait_cnt[i] ---> [ >= AGE_LIMIT ] ---> aged[i] ---+
                                                                   v
                              +------------------------------------------------+
                              | any(aged) ?  longest-waiter(aged)  :  lowest idx |---> gnt
                              +------------------------------------------------+
                                   (priority-encoder fallback = FIXED behaviour)
```

Why FIXED starves: the priority encoder output depends only on `req`. If requester 0 asks every
cycle, the encoder returns 0 forever, and `wait_cnt[1..3]` only count up. Round-robin and aging
add **state** (`rr_ptr`, `wait_cnt`) that makes the winner change, so the wait is bounded.

### Demo 2: bounded buffer and single port (`bounded_buffer.h`)

```
   oldest                                                   youngest
  +--------+--------+--------+--------+ ......... +--------+
  | req 0  | req 1  | req 2  | req 3  |           | req 7  |   PENDING QUEUE  q
  +---+----+---+----+---+----+---+----+           +---+----+   (stays full: constant demand)
      |        |        |        |                    |
      |   visited oldest-first, every cycle           |
      v        v        v        v                    v
  +---------------------------------------------------------+
  |  1. not in buf ?  -> try INSERT   (blocked when full)   |
  |  2. in buf     ?  -> try PORT     (only 1 use per cycle)|
  +----------+--------------------------------+-------------+
             | insert                         | port grant
             v                                v
        +-------------------------+     +--------------------+
  push  | buf  FIFO, depth = 4    |---->|  SERVICE PORT      |  pop buf.front()
 ------>| [ f0 | f1 | f2 | f3 ]    | pop |  1 service / cycle |-----------+
        +-------------------------+     +--------------------+           |
                                                                          v
                      owner of the popped entry:  Buggy        : result DROPPED (must redo)
                                                  ResultKept   : mark "ready" -> completes, no port
                                                  ReservedSlot : same + last slot only for oldest
```

What goes wrong, in hardware terms:

| Variant | Hardware equivalent | Failure / fix |
|---|---|---|
| `Buggy` | Port pops the FIFO but the read data is **not latched** anywhere | Work is done and thrown away, port is busy every cycle, 0 completions (livelock) |
| `ResultKept` | A `ready[id]` bit is set when the entry is popped | Lossless, but younger requests refill the freed slot first; the oldest never gets in (starvation) |
| `ReservedSlot` | Insert limit is `DEPTH-1` for everyone except the oldest request | The oldest request always finds a free slot, so progress is guaranteed |

### Demo 3: backpressure (`backpressure.h`)

Top level, the classic producer / FIFO / consumer with a flow-control path back to the producer:

```
              data + valid                                           data + valid
  +----------+ ----------------->  +------------------------+ --------------------> +----------+
  | PRODUCER |       wr_en         |        FIFO            |        rd_en          | CONSUMER |
  | 1 item   |                     |  depth = 8             |   (1 pop / 4 clk)     | slow:    |
  | / clk    |                     |  wr_ptr  rd_ptr  count |                       | 1 per 4  |
  +----^-----+                     +----+-----------+-------+                       |  clk     |
       |                              count/full   rd_en                            +----------+
       |                                  |           |
       |        ready | stall | credit    v           v
       |                          +-----------------------+
       +--------------------------|  FLOW-CONTROL BLOCK   |   latency = 3 clk
          (arrives LATENCY clk    |  (one of 5 schemes)   |   on the way back
           after it was decided)  +-----------------------+
```

The producer is 4x faster than the consumer, so without flow control the FIFO fills and
`wr_en` while `full` means **data loss**. The five schemes differ only inside the flow-control
block:

```
  1) NONE            producer ---------------------------------------> wr_en      (always 1)

  2) READY / VALID   count ---> [ count < DEPTH ] ---------> ready ---> wr_en = valid & ready
                                 (combinational, same cycle)

  3) DELAYED STALL   count ---> [ count == DEPTH ] ---> stall_raw ---> [z^-3] ---> stall_seen
                                                                       3 flops       |
                                                       wr_en = valid & ~stall_seen <-+

  4) WATERMARK       count ---> [ >= HIGH set / <= LOW clear ] ---> stall_raw ---> [z^-3]
                                 SR flag with hysteresis            (HIGH = DEPTH-LAT,  |
                                                                     LOW  = HIGH/2)     v
                                                       wr_en = valid & ~stall_seen

  5) CREDIT          producer side: credits  (init = CREDITS, <= DEPTH)
                       send        : credits <= credits - 1      (needs credits != 0)
                       credit_ret  : credits <= credits + 1      (arrives LAT clk after a pop)
                     consumer side: every rd_en generates credit_ret through a [z^-3] delay
```

Illustrative RTL for the two interesting cases (a sketch to explain the model, **not part of the
build**; stage counts are approximate because the C++ model is cycle-based):

```systemverilog
// 4) Watermark stall with hysteresis. HIGH = DEPTH - LAT, LOW = HIGH / 2
always_ff @(posedge clk or negedge rst_n)
  if (!rst_n)                              stall_raw <= 1'b0;
  else if (!stall_raw && count >= HIGH)    stall_raw <= 1'b1;   // raise EARLY
  else if ( stall_raw && count <= LOW )    stall_raw <= 1'b0;   // release LATE

logic [LAT-1:0] stall_pipe;                                     // wire / register delay
always_ff @(posedge clk) stall_pipe <= {stall_pipe[LAT-2:0], stall_raw};
assign stall_seen = stall_pipe[LAT-1];
assign wr_en      = valid & ~stall_seen;

// 5) Credit counter at the producer
assign can_send = (credits != '0);
assign send     = valid & can_send;
always_ff @(posedge clk or negedge rst_n)
  if (!rst_n) credits <= CREDITS;
  else        credits <= credits - send + credit_ret;
```

#### Timing: why the delayed stall overflows

Cycle-by-cycle output of the `DELAYED STALL` scenario (cycles 8 to 16, FIFO depth 8, consumer pops
every 4th cycle). Values are taken from the model's own `high` log.

```
 cycle                  :   8    9   10   11   12   13   14   15   16
                           |    |    |    |    |    |    |    |    |
 consumer rd_en         :   1    0    0    0    1    0    0    0    1      pops at 4,8,12,16
 producer wr_en         :   1    1    1    1    1    0    0    0    0
 FIFO count (end)       :   6    7    8    8    8    8    8    8    7
 full / stall_raw (end) :   0    0    1    1    1    1    1    1    0
 stall_seen (used)      :   0    0    0    0    0    1    1    1    1      3 clk after raw rose
 outcome                :  ok   ok   ok  DROP  ok  STALL STALL STALL STALL
                                       |    |
                                       |    +-- cycle 11: FIFO full, producer still sending -> item LOST
                                       +------- cycle 10: FIFO becomes full, flag raised
```

Reading the waveform:

1. **Cycle 10**: the FIFO reaches 8/8, so `stall_raw` goes high.
2. **Cycles 11 and 12**: the flag is still travelling back (3 clk), so `stall_seen` is still 0 and
   the producer keeps sending. In cycle 11 the FIFO is full and the item is **dropped**.
   In cycle 12 the consumer pops first, so one more item fits.
3. **Cycle 13**: the producer finally sees the stall and stops. Too late for the lost item.

With `WATERMARK` the same 3-clock flag is raised at `count >= 5` (`DEPTH - LAT`), so there are
still free entries for the items that are in flight while the flag travels back, and nothing is
lost. The measured peak occupancy in that scenario is 6 of 8.

#### Sizing rule

```
                      |<--------- flow-control round trip (LAT clk) --------->|
   producer sends ... items in flight ........................ flag / credit arrives
                          each of these still needs a free FIFO entry

   headroom needed  >=  LAT x (producer rate)        ->  HIGH watermark = DEPTH - LAT
   items to keep the consumer busy  >=  credit round trip / consumer period
```

If the headroom is too small the FIFO overflows (data loss). If the threshold or the credit count
is too conservative the FIFO runs dry and the consumer wastes slots (throughput loss). The
under-provisioned credit scenario shows exactly that: with 1 credit and a 6-clock return, the
producer gets one item through per credit round trip (7 clocks, which the consumer's 4-clock slots
round up to 8). The consumer wants one item every 4, so utilisation drops to 50% (25 of 50 slots).

### Model term to RTL term

| In the C++ model | In RTL |
|---|---|
| `Requester::wait` | Per-requester wait counter |
| `age_limit` | Compare constant on the wait counter |
| `rr_ptr` | Round-robin pointer register |
| `buf` (deque) | Synchronous FIFO with `wr_ptr`, `rd_ptr`, `count` |
| `cap` / `cfg.cap` | FIFO `DEPTH` parameter |
| `port_used` | "Only one use per cycle" mux or arbiter on the port |
| `ready` set | Per-entry "result valid" bit |
| `stall_pipe` | `LAT` flops on the stall wire |
| `credits_` | Credit counter at the producer |
| `period` | Consumer rate (a clock-enable every N cycles) |
| `res_.dropped` | Overflow event: `wr_en && full` (an assertion in a real testbench) |
| `RPT_ERROR` | `$error` / failing assertion |
| `RPT_WARNING` | `$warning` / coverage or protocol-check hint |

## Reporting and verbosity

`logger.h` mirrors the UVM reporting concepts using its own `rpt::` / `RPT_*` names:

| Macro | Behaviour |
|---|---|
| `RPT_INFO(ID, VERB, MSG)` | Printed only if `VERB` is at or below the active verbosity |
| `RPT_WARNING(ID, MSG)` | Always printed and counted |
| `RPT_ERROR(ID, MSG)` | Always printed and counted |
| `RPT_FATAL(ID, MSG)` | Printed, counted, and throws `rpt::FatalError` |

Verbosity levels: `V_NONE=0`, `V_LOW=100`, `V_MEDIUM=200`, `V_HIGH=300`, `V_FULL=400`, `V_DEBUG=500`.

A **trace window** (`set_trace_window(first, last)`) limits all messages above `V_LOW` to a range of
simulation cycles, so a long run narrates only its first few cycles while results, warnings and
errors still print. `report_summary()` prints counts by severity and by id.

Message format:

```
RPT_INFO @ 7 [ARB] GRANT -> requester 0
^severity  ^time ^id ^message
```

Both demos take an optional verbosity argument (default `high`):

```sh
build/arbitration_starvation  [none|low|medium|high|full|debug]
build/bounded_buffer_livelock [none|low|medium|high|full|debug]
build/backpressure_scenarios  [none|low|medium|high|full|debug]
```

Report ids used by the models: `ARB` (arbiter), `BUF` (bounded buffer), `BP` (backpressure), `DEMO`, `TEST`.

| Condition | Severity |
|---|---|
| A requester has waited 100 cycles without a grant | `WARNING` |
| A requester was never granted during the whole run | `ERROR` |
| No request completed for 50 consecutive cycles | `WARNING` (livelock suspected) |
| The oldest request (id 0) never completed | `ERROR` (deadlock) |
| First FIFO overflow in the backpressure model | `WARNING` |
| Any item lost in the backpressure model | `ERROR` |
| Consumer slots wasted on an empty FIFO (bubbles) | `WARNING` |

## Run-time banner

Every program prints the author banner as its first output, so it also lands in each log file:

```
// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
```

The banner is printed by `rpt::print_banner()` straight to the output stream, so it does not
affect the report counts.

## Tests

`make test` runs 39 tests covering the logger, the arbiter, the bounded buffer and the backpressure model, and also
asserts on what the models report (warnings, errors, narration at high verbosity only). Expected
final line:

```
RPT_INFO @ 39 [TEST] 39 tests, 166 checks, 0 failed checks
```

## Generated logs

`make logs` writes into `log/`:

| File | Content |
|---|---|
| `*_high.log` | Narrated decisions of the first cycles (`V_HIGH`) |
| `*_low.log` | Results only (`V_LOW`), plus warnings and errors |
| `tests.log` | Test run |













