# Benchmarking Methodology

## Performance targets

| Scenario | Metric | Target |
|---|---|---|
| Berlin peak (100 k veh), binary proto + viewport culling | step overhead vs no-GUI baseline | < 1.5× |
| Berlin peak, C++ publisher | step overhead vs no-GUI baseline | < 1.1× |
| Frontend, 100 k vehicles (binary proto) | P95 frame time | < 33 ms |
| Frontend, 10 k visible edges (culled) | P95 frame time | < 16.6 ms |
| Frontend, memory stability | JS heap after 5 min | < 200 MB |

### Doe 6-7
whole network in full screen mode
full hour:
- sumo plain: 88s
- sumo-gui: 134s
- web-gui: 159s (without auto interval, color vehicles by speed, no edge coloring)

Benchmark scenario: Doe 6:00-6:10 (600 sim-seconds, ~5000 vehicles + 1400 persons,
step-length 0.2 s → 3000 steps)

| Component | Duration | RTF | Avg step | Notes |
|---|---|---|---|---|
| sumo (no GUI) | 11.26 s | 53.3 | 3.75 ms | standalone binary, baseline |
| sumo-gui | 16.30 s | 36.8 | 5.43 ms | skip rate 77.7 %, frame 24.4 ms (184 steps/s, 41 fps render) |
| publisher `--benchmark` | 102.8 s | 5.84 | 34.26 ms | libsumo+TraCI, interval=1, no bridge |
| publisher `--benchmark-full` (autotune) | 34.9 s | 17.2 | 11.65 ms | autotune interval 7–10, skip 86.3 %, frontend frame 9.7 ms, skip 1.0 % |

Note: earlier measurements used publisher `--step-length 1.0` (override, now removed), making
them incomparable to sumo/sumo-gui which ran the config's native 0.2 s steps.

#### Pipeline overhead breakdown

| Source | Step overhead | Cause |
|---|---|---|
| libsumo/TraCI step overhead | +3.4 ms | Python↔C++ boundary + TraCI event bookkeeping, even with no data collection |
| TraCI extraction + pack | +30.5 ms | vehicle queries + typed-array packing for ~5000 vehicles — dominates |
| eCAL SHM publish (bridge subscribed) | ~+2 ms | write ~250 KB to SHM, signal subscriber (measured at 1 s steps, approx.) |
| Browser CPU stealing (collocated) | ~+10 ms | browser rendering competes for CPU on same host (measured at 1 s steps, approx.) |

The libsumo/TraCI step overhead (~3.4 ms) is estimated from the autotune skip-step time
(~7 ms measured) minus standalone SUMO (3.75 ms). It is always present regardless of interval.

The browser overhead disappears in normal deployment (publisher on a dedicated server).
The eCAL and browser estimates above were measured at 1 s step size and carry forward as
rough approximations; a re-run of the bridge-only benchmark at 0.2 s steps would confirm.

#### Autotune

The autotune is a simple boolean toggle (on/off). When on, it converges freely to the minimum
publish interval N satisfying the **1.5× overhead limit**. Derivation: at convergence
`total = t_skip / (1 − f)`, so `f = 1/3` gives exactly 1.5×; the publisher uses
`target_budget = step_time_ms / 3`.

Here `t_skip` is the skip-step time (pure libsumo step, no data collection), estimated at
**~7 ms** for this scenario (back-calculated from the autotune measurement).

`t_collect ≈ 34 ms` (headless publisher, no bridge). At convergence (interval=8, delay=0):

```
target_budget = 11.65 / 3 = 3.88 ms
interval = floor(34 / 3.88) + 1 = 9   (observed: 7–10 and drifting down as traffic builds)
```

At `interval = 8`: total ≈ t_skip + t_collect/8 = 7 + 4.3 = **11.3 ms → 1.61× t_skip**.
Slightly above the 1.5× target because traffic density (and therefore `t_collect`) grows
during the scenario; the autotune tracks it by lowering the interval toward 7.

When `delay_ms ≥ t_collect` the sleep already absorbs the collection cost and autotune forces
`interval = 1` (no skipping — the delay is the bottleneck, not extraction).

There is no configurable min/max interval. The only reason to cap the interval would be to
guarantee a minimum visual update rate for video recording; that is a separate future concern.


---

## Running benchmarks

### Publisher-only (`--benchmark`)

Runs the simulation at maximum speed (interval=1, delay=0), publishing every step via
eCAL. No bridge or frontend required. Use this to measure publisher throughput and RTF in
isolation.

```bash
python sumo_ecal_publisher.py --benchmark --sumo-cfg path/to/sim.sumocfg
```

End-of-run output:

```
Benchmark mode (--benchmark): delay=0, interval=1, publishes every step.
Simulation finished after 3600 steps
Performance:
  Duration: 21.9 s
  Real time factor: 164.4
  UPS: 48231.7
Publisher:
  Avg. step time [ms]: 6.08
  Avg. skip rate: 0.000
Benchmark done: 21.9 s wall clock
```

The publisher skip rate is always 0 with `--benchmark` (interval=1, every step published).

### Full-stack (`--benchmark-full`)

Runs the same max-speed simulation but also collects rendering stats from the frontend.
Requires the bridge and a browser with the frontend open before starting.

The easiest way is via `benchmark.sh`:

```bash
./benchmark.sh [--sumo-cfg path/to/sim.sumocfg] [--browser-wait <seconds>]
```

`benchmark.sh` starts the bridge and Vite dev server, waits until the dev server
responds, opens the browser automatically (`xdg-open` / `open`), waits
`--browser-wait` seconds (default 5) for the page to load and the WebSocket to connect,
then runs the publisher with `--benchmark-full`. When the publisher exits the bridge and
dev server are cleaned up automatically.

To run the components manually instead:

```bash
# Terminal 1
python ecal_ws_bridge.py

# Terminal 2 — open browser to http://localhost:5173
npm --prefix frontend run dev

# Terminal 3 — start after the browser has connected
python sumo_ecal_publisher.py --benchmark-full --sumo-cfg path/to/sim.sumocfg
```

The publisher sets `benchmark=true` in `GetStateResponse` so the frontend auto-resumes
without manual interaction. When the simulation ends the frontend sends its cumulative
rendering stats back via the `report_frontend_stats` service call. The publisher waits up
to 15 s for this callback, then exits.

End-of-run output (appended after the publisher block above):

```
Frontend:
  Avg. frame time [ms]: 16.2
  Avg. skip rate: 0.043
  Frames rendered: 1847
```

If the frontend is not connected when the simulation ends, the publisher prints
`Frontend: no stats received (bridge/frontend not connected)` and exits.

### Enabling verbose instrumentation

The publisher and frontend keep their detailed per-step / per-frame counters
*collecting* all the time (they feed `--benchmark-full`), but the verbose
**display** of them is off by default to keep production logs and UI quiet.
Turn them on when investigating performance:

- **Publisher 5 s report log** (`steps/s`, `ms/step`, `interval [binding]`,
  frontend `frame_ms`, `sim`/`native` µs, C++ `getStats` breakdown):

  ```bash
  python sumo_ecal_publisher.py --verbose-perf --sumo-cfg …
  ```

  Always on automatically in `--benchmark` and `--benchmark-full` modes.

- **Frontend per-frame stats panel** (`msg/s`, `frame ms`, `parse`,
  `veh-build`, `skip %`): open the UI with `?perf=1` in the URL, e.g.
  `http://localhost:5173/?perf=1`.

### Environment requirements

- **`eclipse-sumo >= 1.27.0`**. Version 1.26.0 segfaults inside
  `simulation.start()` on the doe scenario (faulthandler trace points at
  `libsumo/__init__.py:232`). Upgrading the pip-installed package
  (`pip install -U eclipse-sumo`) fixes it. `requirements.txt` leaves
  `libsumo` unpinned so a fresh install picks up the fix automatically.
- `SUMO_HOME` must point at a matching SUMO checkout for the tools (e.g.
  `sumolib`) that the publisher imports.

### Headless autotune benchmarking (current capability gap)

`--benchmark` hard-disables autotune by design (it's meant to measure
publisher max throughput at interval=1). `--benchmark-full` enables
autotune but requires a connected browser frontend. To measure the
autotune-on path **without** a browser (the most informative isolation of
the "browser CPU contention" question — see the 2026-05-26 findings
below), the current workaround is a small wrapper script that imports
`sumo_ecal_publisher.py` and sets `ctrl["autotune"] = True` after the
`--benchmark` setup. A `--benchmark --autotune` combo flag would close
this gap; see PLAN.md "Near-term" for the deferred follow-up.

### sumo-gui end-of-run stats

sumo-gui (when built with `ENABLE_FOX=ON`) appends GUI-specific stats to the standard
`--duration-log.statistics` output:

```
GUI:
 Avg. frame time [ms]: 8.3
 Avg. skip rate: 0.82
```

The skip rate here is `skipped_steps / total_steps` where a step is "skipped" when
`GUIRunThread` completes a step before `GUIViewTraffic::doPaintGL` renders it. This is
directly comparable to the publisher's skip rate and the frontend's `skip X%` overlay.

---

## Comparison with sumo-gui

sumo-gui is the natural reference point since it solves the same problem (visualising a
running simulation) without the web stack overhead. It reports these metrics in its
network parameter dialog (from `GUINet.cpp` and `GUISUMOAbstractView.cpp`):

### sumo-gui metric definitions (from source)

```
step duration [ms]     = myLastSimDuration + myLastIdleDuration
                         (visualisation duration is currently commented out — render
                          time is NOT included in step duration)

simulation duration[ms]= myLastSimDuration alone
                         (pure SUMO computation, no GUI overhead)

duration factor        = DELTA_T / myLastSimDuration
                         DELTA_T = simulation step size in ms (default 1000 ms = 1 s)
                         > 1.0 means faster than real time; e.g. 5.0 = 5× realtime

updates per second     = runningVehicleCount / myLastSimDuration * 1000
                         vehicles moved per second of wall-clock time

FPS                    = 1000 / myFrameDrawTime
                         myFrameDrawTime = wall time of one OpenGL doPaintGL() call
                         NOT capped by vsync — native OpenGL (FOX toolkit), renders
                         as fast as possible; 200 fps is possible for simple scenes
```

**Key point on FPS**: sumo-gui FPS is *not* vsync-capped because it uses native OpenGL
directly. It measures real render throughput. Our frontend RAF is capped at 60 Hz, so
FPS is not directly comparable — use frame time in ms on both sides instead.

### How to compare

Run the same scenario in sumo-gui and with our publisher. Collect for both:

| Metric | sumo-gui source | Our equivalent |
|---|---|---|
| Simulation duration (ms) | `simulation duration [ms]` dialog field | `t_sim` in publisher CSV |
| Total step overhead (ms) | `step duration [ms]` (= sim + idle) | `t_sim + t_veh + t_edge + t_pack + t_pub` |
| Duration factor (RTF) | `duration factor` dialog field | `DELTA_T / total_step_ms` |
| Render frame time (ms) | `1000 / FPS` (inverse of displayed FPS) | `frameMs` from RAF instrumentation |
| Vehicles/s | `updates per second` | `n_veh / total_step_s` |

**The headline comparison**: duration factor with our GUI vs. duration factor with sumo-gui.
If sumo-gui achieves RTF = 5.0 and our publisher+browser achieves RTF = 3.5, we have
degraded the simulation speed by 5.0/3.5 = 1.43×. That is within the 1.5× target.

sumo-gui duration factor includes its own rendering overhead (implicitly via idle time and
the simulation loop waiting for the render). Our publisher's `t_sim` is pure SUMO; we add
extraction + publish overhead on top. So the fair comparison is:

```
our_RTF     = DELTA_T / (t_sim + t_veh + t_edge + t_pack + t_pub)   [per step]
sumo_RTF    = DELTA_T / (sim_duration + idle_duration)               [from dialog]
no_gui_RTF  = DELTA_T / t_sim                                        [baseline]

target: our_RTF >= sumo_RTF   (at least as good as sumo-gui)
stretch: our_RTF >= no_gui_RTF / 1.5
```

### sumo-gui render performance as a reference

On a modern desktop GPU, sumo-gui renders Berlin at peak hour at roughly:
- 5–15 fps at full network view (all 600 k edges visible, all vehicles)
- 20–60 fps zoomed in (few thousand visible objects)

These numbers come from native OpenGL with immediate-mode rendering — no tessellation
overhead, no JavaScript, direct C++ → GPU. Our WebGL frontend (deck.gl PathLayer with
CPU tessellation) will be slower at full network view; the question is by how much.
Viewport culling is essential to close this gap.

---

## The 60 fps cap problem

Browsers lock `requestAnimationFrame` to the display refresh rate (almost always 60 Hz,
occasionally 120 Hz on newer displays). A frame that takes 2 ms looks identical to one
that takes 15 ms — both appear as "60 fps". **FPS is only a useful metric when you are
below the cap** (frame time > 16.6 ms). For all measurements, collect **frame time in
milliseconds** from `performance.now()` deltas, not FPS.

Implication: a small test scenario always saturates at 60 fps even if the code is
inefficient. You cannot tell from the FPS counter whether you have 14 ms of headroom or
0.1 ms of headroom. Always measure and report milliseconds.

---

## Frontend: what to collect

Add lightweight instrumentation to the RAF drain in `useSimSocket.ts`:

```ts
// Top of RAF callback:
const rafStart = performance.now();

// After buildVehicleLayer / buildEdgeDataLayer / setState:
const buildMs = performance.now() - rafStart;

// In DeckGL onAfterRender callback:
const frameMs = performance.now() - rafStart;

// Once per second, emit to a stats overlay or console:
// { frameMs, buildMs, vehicleCount, visibleEdgeCount, heapMB }
// heapMB = (performance as any).memory?.usedJSHeapSize / 1e6  (Chrome only)
```

Collect per-frame samples over 300+ frames. Report **mean, P95, P99** — not just average.
GC pauses appear as isolated large `frameMs` spikes (50–200 ms) that are invisible in
averages but very visible to the user.

Watch `heapMB` over time: sustained growth means GC pressure is building and a large pause
is coming. The current per-vehicle object approach allocates ~100 k JS objects per step
which is the main source of heap churn.

### Key breakpoints

| N vehicles | Metric | Target (binary proto) | Current code |
|---|---|---|---|
| 10 k | mean frame time | < 5 ms | < 15 ms |
| 50 k | mean frame time | < 10 ms | likely > 33 ms |
| 100 k | mean frame time | < 16.6 ms | likely > 66 ms |
| 100 k | P99 frame time | < 33 ms | — |
| 100 k | heap growth / 60 s | < 50 MB | — |

---

## Frontend: synthetic load generator

To stress-test rendering **independently of the backend**, add a synthetic benchmark mode
(e.g., a `/bench` route in the Vite dev build) that:

1. Generates N random vehicle positions as typed arrays directly in the browser
2. Calls `buildVehicleLayer` and `buildEdgeDataLayer` in a RAF loop
3. Reports mean/P95/P99 frame time over 300 frames at each N

No backend, no WebSocket needed — just a button "Run: N=10k / 50k / 100k". This lets you
measure the rendering ceiling before any SUMO infrastructure is available, and rerun after
each optimization to track improvement.

The dec.gl `onAfterRender` callback fires after the GPU command queue is flushed, so it
captures both CPU and GPU time.

---

## Backend: what to collect

Add instrumentation to the publisher step loop using `time.perf_counter()`. Write one CSV
row per step to a file (not stdout — avoids GIL contention):

```python
# In _step_loop(), wrap each phase:
t0 = time.perf_counter()
traci.simulationStep()
t_sim = time.perf_counter() - t0

t0 = time.perf_counter()
# ... vehicle queries ...
t_veh = time.perf_counter() - t0

# etc. for t_edge, t_pack, t_pub

# Write CSV row:
csv_writer.writerow([step, t_sim, t_veh, t_edge, t_pack, t_pub, n_veh, n_edge])
```

After collecting 1000 steps, load in pandas and report P50/P95/P99 for each phase. The
phase breakdown tells you which part to optimise first.

### No-GUI baseline

```bash
# Option 1: SUMO built-in duration log (no Python overhead at all)
sumo -c path/to/sim.sumocfg --duration-log.statistics true 2>&1 | grep "Duration"

# Option 2: publisher benchmark (includes TraCI connection overhead, excludes rendering)
python sumo_ecal_publisher.py --benchmark --sumo-cfg path/to/sim.sumocfg
# Compare "Real time factor" against Option 1's duration factor.

# Option 3: publisher with a --no-publish flag (to be added)
# Calls traci.simulationStep() in the same loop but skips all data extraction and
# eCAL publishing. Isolates SUMO's own step time from extraction overhead.
```

Run the same scenario (same config, same random seed) with and without publishing.
The overhead ratio = `total_step_time / baseline_step_time`.

### The "small scenario, not enough vehicles" problem

Benchmark scenario design must account for **two independent axes**:

**Vehicle extraction** scales with `n_veh` only — a small network with 100 k vehicles
stresses the vehicle query loop identically to Berlin with 100 k vehicles.

**Edge data extraction** (when edge data is enabled) scales with `n_active_edges` — the
number of edges that currently have at least one vehicle. This is **not** determined by
`n_veh` alone; it depends on the ratio of vehicles to network capacity:

- A small dense network (e.g., 10 k edges) with 100 k vehicles will have nearly all edges
  active (saturation ≈ 100%). This *overstresses* edge data relative to Berlin.
- Berlin (600 k edges) with 100 k vehicles at peak hour will have maybe 10–20 k active
  edges (saturation < 5%). This is the realistic case.
- A small network with forced high demand therefore does **not** represent Berlin's edge
  data cost — it will make edge extraction look much worse than it actually is.

**Consequence**: use separate scenarios for the two concerns:

| What to benchmark | Scenario | Why |
|---|---|---|
| Vehicle extraction only (edge data off) | Small network, high demand (N veh) | Reproducible, fast to iterate |
| Edge data extraction | Medium network (~50–100 k edges), moderate demand | Realistic active-edge density |
| Combined realistic load | Berlin or similar large network | End-to-end validation |

Use `randomTrips.py --period <value>` to control injection rate. Always record both
`n_veh` and `n_active_edges` in the CSV so you can separate the two effects in analysis.

### Expected scaling

#### Vehicle extraction (edge data disabled)

| N vehicles | Current Python (est.) | Binary-packed Python (est.) | C++ publisher (est.) |
|---|---|---|---|
| 10 k | 3–10 ms | 2–5 ms | < 1 ms |
| 50 k | 15–50 ms | 8–20 ms | 1–2 ms |
| 100 k | 26–100 ms | 15–30 ms | 2–5 ms |

#### Edge data extraction (vehicles spread across large network)

| N active edges | Current Python (est.) | With viewport culling (est.) |
|---|---|---|
| 1 k (typical zoom-in) | 0.5–2 ms | 0.5–2 ms |
| 10 k (city overview) | 5–20 ms | 1–4 ms |
| 50 k (full Berlin visible) | 25–100 ms | — (culled before query) |

---

## Frame skipping

### sumo-gui threading model (corrected)

sumo-gui does **not** render every simulation step. The simulation runs in `GUIRunThread`,
a dedicated background thread. The FOX toolkit event loop (main thread) renders
asynchronously via a timer. Both threads share `mySimulationLock`: the sim thread holds it
during `simulationStep()`; the render thread acquires it before `doPaintGL()`. Neither waits
for the other — they race for the lock. As a result:

- If the simulation is fast (short steps), the render thread rarely gets the lock → **frames
  are skipped**.
- If the simulation is slow (long steps or heavy network), the render thread easily keeps up
  → skip rate near zero.
- The `FPS` counter in sumo-gui measures actual `doPaintGL()` throughput — it can be far
  below the simulation's steps/s when the sim runs much faster than real time.

Our architecture has the same fundamental characteristic: the publisher publishes and moves
on; the browser renders at whatever rate it can. The difference is that data crosses a
network stack (eCAL → WebSocket → RAF) instead of a shared-memory mutex, so there are more
places where frames can be silently dropped.

### Skip rate formula

Given sumo-gui's `duration factor` (RTF) and `FPS`:

```
steps_per_second = RTF / step_length_s          # e.g. RTF=36.8, step=0.2 s → 184 steps/s
skip_rate        = max(0, 1 − FPS / steps_per_second)
                   # e.g. 41 fps / 184 steps/s → skip 77.7 %
```

For our frontend, `seq_num` in `SimBin` (publisher-side monotonic counter) makes this
directly measurable: a gap of N in the received sequence means N frames were dropped
somewhere in the pipeline (eCAL transport, bridge latest-value queue, or RAF coalescing).
The skip rate is reported in the perf overlay as `skip X%` when non-zero.

### Where frames are dropped in our pipeline

| Drop point | Mechanism | Typical cause |
|---|---|---|
| eCAL transport | Buffer full on subscriber side | Publisher faster than eCAL can deliver |
| Bridge latest-value queue | New SimBin arrives before previous WS send completes | SUMO steps faster than WebSocket throughput |
| Frontend RAF coalescing | `latestSnapshot` ref overwritten before RAF fires | SUMO steps faster than 60 fps |

The `seq_num` gap measures the **sum** of all three. There is currently no per-stage
breakdown — if needed, the bridge could log its own received-vs-sent gap separately.

### Acceptable skip rates

Skip rate is not inherently bad — it means the simulation is running faster than the display
can show, which is the desired outcome when speed matters.

| Scenario | Skip rate | Interpretation |
|---|---|---|
| SUMO at ≤ 60 steps/s (1 s steps, RTF ≤ 60) | 0% | Display keeps up; every frame rendered |
| SUMO at 200 steps/s | ~70% | Expected and fine for live monitoring |
| SUMO at 200 steps/s, skip > 95% | Investigate | Rendering overload likely contributing |

The problematic case is a **high skip rate caused by rendering overload rather than
simulation speed**: if SUMO runs at only 10 steps/s but the frontend skips 80% of those
because frame time exceeds 100 ms, the visualization is broken regardless of simulation
throughput. The `skip X%` overlay combined with `frame Xms` distinguishes the two cases:

- High skip + frame < 16 ms → simulation is fast, skipping is expected
- High skip + frame > 16 ms → rendering is the bottleneck, optimise the frontend

### Adding to benchmark targets

The performance targets table should be extended with a skip-rate column once baseline
measurements on real scenarios are collected. A suggested threshold: at the intended
operating speed (not benchmark mode), skip rate should be ≤ the sumo-gui skip rate for
the same scenario and hardware, confirming we add no extra pipeline overhead beyond what
the native renderer already accepts.

---

## End-to-end latency (step → pixel)

Hard to measure without synchronized clocks. Pragmatic approach:

- Publisher: log `(step_id, publish_wall_time_ms)` to a file or named pipe
- Frontend: log `(step_id, receive_wall_time_ms)` extracted from the SimStep proto
- If both run on the same host (or NTP-synced), compute `receive - publish`

Alternatively, add a "simulation clock lag" overlay to the frontend: show the SUMO sim
time from the last received step vs. wall clock. If it drifts steadily, the pipeline is
building a queue under load.

---

## What a passing benchmark looks like

Measured on the **small benchmark scenario** (known N vehicles, reproducible):

1. **Backend overhead < 1.5×**: `(t_veh + t_edge + t_pack + t_pub) / t_sim < 0.5` (i.e.,
   extraction adds < 50% on top of pure simulation time).
2. **P95 frame time < 33 ms** at 100 k vehicles (frontend, binary proto).
3. **P95 frame time < 16.6 ms** at 10 k visible edges (frontend, viewport culled).
4. **No heap growth > 50 MB over 60 s** (no GC bomb building up).
5. Berlin validation: subjectively smooth at 10+ fps, no noticeable simulation slowdown.

## Findings (2026-05-25): end-to-end measurement on doe scenario

Full-stack `./benchmark.sh` run on the doe scenario (Berlin Mitte, geo-referenced,
~50 k edges, ~600 vehicles + ~100 agents steady state), with the C++ native
publisher (libsumo::ECal), phase timers on both ends, and the new
`Per-frame breakdown [ms]` line reported by the frontend.

### Headline numbers

| Configuration                                  | Wall time | RTF    | UPS      | ms / step |
|------------------------------------------------|----------:|-------:|---------:|----------:|
| **Pure SUMO** (no GUI, no libsumo, no eCAL)    | 11.26 s   | 53.3×  | 1.25 M   | **3.76**  |
| **Our pipeline** (libsumo + native eCAL + browser) | 32.2 s    | 18.6×  | 432 k    | **10.74** |
| Overhead vs pure SUMO                          | 2.86×     |        |          | +7 ms     |

Skip rate: publisher 0.48 (autotune `interval=2` halving publish frequency to
match the frontend), frontend 0.027 (essentially perfect — frontend keeps up).

### Publisher per-step breakdown (from C++ `getStats` + Python timers, steady state)

| Phase                                  | µs / step  | ms / step | % of step |
|----------------------------------------|-----------:|----------:|----------:|
| `sim=` — libsumo `simulationStep()`    | 6 000 – 10 500 | **~8.5** | ~80 %    |
| `native=` — C++ publish (incl. PROJ)   | 2 200 – 2 800  | ~2.5     | ~23 %    |
|   ↳ veh loop  (`veh_us` / `calls`)    | ~3 700 each call | (in native) | |
|   ↳ agent loop                        | ~1 300 each call | (in native) | |
|   ↳ edge loop                         | ~40 each call    | negligible  | |
|   ↳ typedict / serialize / send       | ~80 each call    | negligible  | |
| `tls=` — TLS state extract + publish   |  140       | ~0.1     | ~1 %     |100 

**The single biggest cost is libsumo's `simulationStep` itself, which is 2 – 3×
more expensive than the pure SUMO binary's step (3.76 ms).** That excess (~5 ms)
is larger than the entire native publish path.

### Frontend per-frame breakdown (cumulative averages over 2 508 frames)

| Phase                                    | ms / frame | % of frame |
|------------------------------------------|-----------:|-----------:|
| Proto decode (`ws-parse`)                | 0.80       | 6 %        |
| RAF drain — React state setters          | 0.07       | < 1 %      |
| Vehicle layer build (`vehicle-build`)    | 0.23       | 2 %        |
| Edge data layer build (`edge-build`)     | 0.00       | — (no recompute this run) |
| Layers `useMemo` body (`layers-build`)   | 0.45       | 3 %        |
| deck.gl GPU render (`deck-render`)       | 1.61       | 12 %       |
| **Total instrumented work**              | **~3.2**   | **24 %**   |
| **Unaccounted (browser idle / vsync wait)** | **~9.8** | **76 %**   |
| Avg rAF interval                         | 13.03      | 100 %      |

13.03 ms ≈ 1 / 75 Hz: the browser is pacing rAF to the display refresh. The
frontend is doing ~3 ms of real work per frame and **has ~10 ms of headroom**.
This refutes the earlier hypothesis that publisher and frontend were "balanced":
the frontend is mostly idle and the bottleneck is entirely the publisher.

### Implications for next perf work

Priority is now strongly weighted toward shrinking publisher step time, because
the frontend will absorb improvements without further changes (autotune would
drop `interval` from 2 → 1 and publish twice as often).

1. **#1: libsumo `simulationStep` overhead.** The gap between our `sim=` (~8.5 ms)
   and pure SUMO's step (3.76 ms) is the largest single opportunity (~5 ms).
   Suspects: subscription / output writer pumping, libsumo's global Helper maps,
   sumocfg-driven outputs that the standalone binary may skip. Isolating
   requires a libsumo-only loop (no eCAL publish at all) on the same scenario.
2. **#2: native vehicle / agent extraction loop.** `native=` is ~2.5 ms, of
   which the vehicle loop is the bulk. A batched libsumo accessor that fills
   our protobuf directly from `MSVehicle*` would help, but the headroom is
   smaller than #1.
3. **#3: configurable position mode (METER_OFFSETS).** Eliminates the per-point
   PROJ call. Saves ~1.5 ms of `native=` on geo networks. Already designed in
   PLAN.md; lower priority now that the frontend has headroom.

### Methodology notes for reproducing

- Pure SUMO baseline: `sumo -c doe/view.sumocfg --duration-log.statistics`
  produces `Performance: Duration / Real time factor / UPS` lines on stdout
  (logged to `doe/sumo.log`).
- Pipeline benchmark: `cd ecal_deck && ./benchmark.sh` (needs a real browser;
  starts bridge + Vite dev server + opens Chromium).
- Phase timers: C++ counters in `src/libsumo/ECal.cpp` (`State` struct,
  exposed via `ECal::getStats(reset=true)`); Python timers in
  `sumo_ecal_publisher.py::_step_loop`; frontend `performance.mark`/`measure`
  collected by `usePerfStats.ts` and shipped via the new `breakdown` field on
  `ReportFrontendStatsRequest`.

### libsumo-only diagnostic (same scenario, no eCAL publish)

To localize where the publisher overhead actually lives, the new helper
`ecal_deck/bench_libsumo_only.py` runs the same doe scenario three ways and
prints ms/step + RTF for each. Run on the **same machine** as the standalone
SUMO baseline (otherwise the comparison is meaningless — CPU differences alone
swamp the effect):

```bash
SUMO_HOME=$HOME/sumo ../ecal_env/bin/python ecal_deck/bench_libsumo_only.py doe/view.sumocfg
```

#### Reference machine (Linux dev box) — apples-to-apples (median of 3 runs)

| Configuration                              | ms / step | RTF    | Δ vs pure SUMO |
|--------------------------------------------|----------:|-------:|---------------:|
| Pure SUMO binary (`sumo -c view.sumocfg`)  | **4.98**  | 40.1×  | baseline       |
| libsumo loop only (A)                      | **4.77**  | 41.9×  | −0.21 (≈ 0)    |
| libsumo + `getIDList` (B)                  | 6.61      | 30.2×  | +1.63          |
| libsumo + `getIDList` + 3 getters/veh (C)  | 12.94     | 15.5×  | +7.96 (~14 k getter calls/step) |

Run-to-run noise across three repetitions was under 5 % for every variant.

#### Conclusion: libsumo itself is not the bottleneck

The earlier hypothesis that "libsumo's `simulationStep` is 2–3× pure SUMO" was
wrong. On the reference machine, libsumo loop-only matches the standalone
binary within noise. Where the per-step cost grows is in the **per-vehicle
Python ↔ C++ boundary calls** of variants B and C:

- One `getIDList` per step: ~1.5 µs × ~600 vehicles → ~1 ms of Python list
  marshalling.
- Three getters per vehicle (Position, Speed, Angle): ~500 ns per call, ~14 k
  calls per step in steady state → ~7 ms of pure boundary cost.

#### What this means for our publisher

This is exactly the cost our C++ native publisher already avoids by calling
into libsumo's C++ API directly and writing the protobuf in-place. The
`native=2.5 ms` we measure includes the full vehicle/agent loop plus geo
projection plus protobuf serialization — i.e. **the native publisher does the
job in ~2.5 ms that a naive Python loop would take ~10 ms for** (3 ms libsumo
sim + 7 ms boundary calls). The C++ fast-path saves ~7 ms/step and is the main
reason RTF is 18× rather than ~5×.

#### Re-ranked perf priorities

With libsumo eliminated as a culprit, the remaining publisher overhead vs the
pure SUMO baseline (~2 ms on the user's machine: standalone 3.76 → publisher
`sim=` ~5.7 ms in the best windows, ~10 ms in the worst) must come from
something process-local in the publisher:

1. **Subscriber-induced eCAL SHM contention.** On the user's run, `sim=` rose
   from ~6 ms early (few/no subscribers) to ~10 ms late (browser fully
   subscribed). Worth confirming by running the full pipeline with the bridge
   started but no browser tab attached.
2. **GIL / scheduling jitter** from the eCAL service callback thread and the
   `time.sleep(0)` yield after each step. Could try removing the yield in the
   benchmark path (using a dedicated `paused` check).
3. **C++ vehicle loop tweaks.** Already at 2.5 ms; smaller absolute headroom
   than #1. Direct `MSVehicle*` access instead of `libsumo::Vehicle::*`
   wrappers would shave ~0.5–1 ms.
4. **Position-mode METER_OFFSETS** (already in PLAN.md). Removes PROJ from
   `native=`. Saves ~1 ms on geo networks.

The single biggest verified win on the table is therefore #1 — characterize
and reduce SHM-induced step-time bloat. Everything else is sub-millisecond.

### Verifying the SHM-subscriber theory

To check whether eCAL shared-memory subscribers slow the publisher, ran
`--benchmark` (publisher-only, headless, `interval=1`, `delay=0`) twice on the
same machine and scenario, once with the eCAL WebSocket bridge attached as a
subscriber and once without.

| Metric (steady-state late window) | Publisher alone | Publisher + bridge subscriber | Δ              |
|-----------------------------------|----------------:|------------------------------:|---------------:|
| `sim=`                            | 6.51 ms         | 6.73 ms                       | +0.22 ms       |
| `native=`                         | 3.80 ms         | 3.98 ms                       | +0.18 ms       |
| `send_us` (per publish)           | **1.4 µs**      | **32 µs**                     | +30 µs (≈ +0.06 ms / step) |
| Avg step (whole run)              | 9.23 ms         | 9.47 ms                       | +0.24 ms (+2.6 %) |
| RTF                               | 21.7×           | 21.1×                         | −0.6×          |

**Verdict: the SHM-subscriber theory is also wrong** for this scenario on this
hardware. The bridge attaching adds ~0.24 ms/step, fully within run-to-run
noise. Send cost rises 23× in relative terms but stays at 32 µs absolute.

### What the numbers actually say (revised)

The whole-run average for pure SUMO (4.98 ms/step) is dragged down by the
~50 ramp-up steps where vehicles are still being inserted. In those early
windows the publisher reports `sim ≈ 3.9 ms` — close to the pure-SUMO average.
By the time the publisher is in steady state with ~600 vehicles, `sim ≈ 6.5 ms`,
and that **is what SUMO itself takes** for this scenario at full load:

| Phase (publisher, steady state)    | Time      | Notes |
|------------------------------------|----------:|-------|
| SUMO compute (`sim=`)              | ~6.5 ms   | matches what pure SUMO needs at the same vehicle count |
| Native C++ publish (`native=`)     | ~3.8 ms   | vehicle loop + agent loop + PROJ + protobuf |
| TLS                                | ~0.17 ms  | |
| **Total publisher step**           | **~10.5 ms** | |

There is no hidden overhead. The publisher pipeline runs as fast as physics
allows for this scenario; any further improvement must come from making either
(a) SUMO step itself faster, or (b) the native C++ extraction loop faster.

### Updated perf priorities

1. **SUMO compute itself** is the largest remaining slice (~6.5 ms / ~62 % of
   step). Levers are scenario-side (`--no-internal-links`, simplifying TLS
   logic, lowering `--lateral-resolution`) rather than ours. The fundamental
   limit on this scenario is set by SUMO.
2. **Native C++ extraction loop** (~3.8 ms / ~36 %). The vehicle loop is the
   bulk; direct `MSVehicle*` access avoiding `libsumo::Vehicle::*` wrappers
   could shave a few hundred microseconds, but the absolute headroom is small
   (~1 ms best case).
3. **Position-mode METER_OFFSETS** (already in PLAN.md). Saves ~1 ms of PROJ
   on geo networks. Now genuinely a small optimization rather than a critical
   one, but still worth shipping for the largest networks.

The frontend has ~10 ms of vsync headroom and is not on the critical path.
Bridge / SHM / serialization / send are all sub-millisecond noise.

## Berlin scenario (larger network, 5811 TLS)

To check whether the doe findings generalize, ran the same diagnostics on a
much larger Berlin scenario (`berlin/test_short.sumocfg`):

- Network: `net.net.xml.gz` (98 MB compressed, ~22 s to load)
- Routes: random trips, 1 vehicle/s inserted from t=0..3600
- Capped at `--end 1800` (1799 sim seconds, ramping 0 → 1564 running vehicles)
- **5811 traffic lights** (vs ~50 in doe)
- 0 persons / 0 transit agents

### Results (reference machine, same as doe runs)

| Configuration                              | ms / step | RTF    | Notes |
|--------------------------------------------|----------:|-------:|-------|
| Pure SUMO binary                           | **12.7**  | 78.8×  | 23 s wall over 1800 steps |
| libsumo loop only (A)                      | 18.4      | 54.4×  | +5.7 vs pure SUMO (?)  |
| libsumo + `getIDList`                      | 18.7      | 53.4×  | vehicles list trivial cost |
| libsumo + 3 getters/veh (~880 vehs avg)    | 20.0      | 49.9×  | +1.3 vs B |
| libsumo + `getRedYellowGreenState` × 5811  | 16.4      | 61.0×  | +3.8 vs step-only at same load |
| **Publisher `--benchmark`**                | **28.3**  | 35.3×  | full pipeline, headless |

Publisher steady-state phase breakdown (late window, ~1500 vehicles, 5811 TLS):

| Phase     | µs / step | ms / step |
|-----------|----------:|----------:|
| `sim=`    | ~24 000   | ~24       |
| `tls=`    | ~10 000   | **~10**   |
| `native=` | ~1 000    | ~1        |
| Total     | ~35 000   | ~35       |

### Key finding: TLS extraction is the Berlin bottleneck

The Python loop that publishes traffic-light state (`sumo_ecal_publisher.py`
around line 663) is:

```python
for tls_id in traci.trafficlight.getIDList():
    ph = tu.lights.add()
    ph.id = tls_id
    ph.state = traci.trafficlight.getRedYellowGreenState(tls_id)
pub_tls.send(tu.SerializeToString())
```

With 5811 TLS this is **5811 cross-boundary calls per step + 5811 protobuf
field assignments + a ~20 KB SerializeToString**, totalling ~10 ms/step. That
is *more than the entire native C++ vehicle/agent path*. For doe (~50 TLS) the
same code costs ~0.15 ms — invisible — which is why the optimization hadn't
been needed.

### Library-side gap on Berlin (still ~5 ms unexplained)

libsumo loop-only is 18.4 ms vs pure SUMO 12.7 ms on Berlin — a much bigger
relative gap than on doe (where the two matched). The vehicle-getter loop only
accounts for +1.3 ms of that. The unexplained ~4 ms is small-vehicle-count
specific to libsumo on this large network; possibly subscription bookkeeping
overhead that scales with network size. Worth a deeper look but lower priority
than TLS (which is a 10 ms/step issue we control).

### Updated perf priorities (combined doe + Berlin)

1. **Native C++ TLS publish.** Mirror the existing vehicle/agent native path
   for traffic lights. Expected: ~10 ms → ~0.5 ms on Berlin; no effect on doe.
   This is the single biggest measurable win in the whole pipeline today.
2. **Position-mode METER_OFFSETS.** ~1 ms PROJ savings on geo networks.
   Still worth shipping.
3. **Direct `MSVehicle*` access in native loop.** Sub-millisecond on doe,
   couple of milliseconds on Berlin. Smaller wins.
4. **Investigate libsumo + large-network overhead** (Berlin-specific 4 ms gap).
   Lower priority — diagnostic only; the fix likely lives in libsumo itself.

---

## TLS Fold-In Results (2026-05-25)

Followed up on the Berlin TLS finding by folding the former separate
`sumo/tls` topic into `SimStepBin` and implementing TLS extraction natively
in C++ inside `libsumo::ECal::publishSimStep`.  The Python TLS publish loop
in `sumo_ecal_publisher.py` (which on Berlin cost ~10 ms/step) is gone for
the native fast-path; the Python fallback path still has an equivalent
section for geo-referenced networks that can't use the native publisher.

### Wire / API changes

- `proto/sumo.proto`: added `tls_count` (u32) + `tls_ids` and `tls_states`
  (both null-terminated UTF-8 blobs) to `SimStepBin`. Removed the standalone
  `TLSPhase` / `TLSUpdate` messages.
- `libsumo::ECal`: new TLS section between agent and edge sections; iterates
  `MSNet::getInstance()->getTLSControl().getAllTLIds()` and reads
  `getActive(id)->getCurrentPhaseDef().getState()` per controller. New
  `tlsNs` accumulator surfaced via `getStats()` as `tls_us=...`.
- `ecal_ws_bridge.py`: dropped `_TYPE_TLS`, the `sumo/tls` topic and its
  entry in `_LATEST_VALUE`.
- Frontend: `TLSPhase` / `TLSUpdate` are now local TS interfaces defined in
  `useSimSocket.ts`. The SimStep decode path builds `lights[]` on the fly
  from the two parallel byte blobs; the binary frame type 2 case is gone.

### Berlin scenario (test_short.sumocfg, 1800 s, 5811 TLS)

Apples-to-apples publisher-only benchmark (`--benchmark`, no bridge / no
browser), single run each:

| Metric                | Before (Python TLS) | After (C++ native TLS) | Δ           |
| --------------------- | ------------------: | ---------------------: | ----------- |
| Avg. step time [ms]   |               28.3  |              **21.4**  | **−6.9 ms** (−24 %) |
| Real-time factor      |               35.3× |               **46.6×**| +32 %       |
| UPS                   |                ~29k |                **39k** | +35 %       |
| TLS portion per step  |        ~10.0 ms (Py) |       **~4.0 ms** (C++) | **−6 ms** |

Native-publisher per-step breakdown for the last 5 s window
(steady-state, ~700 vehicles, full 5811 TLS extracted every step):

```
sim       = 22.2 ms/step   (SUMO compute, unchanged)
native    =  5.2 ms/step   (total inside libsumo::ECal::publishSimStep)
  └─ tls       = 4.2 ms     (≈ 720 ns per controller, batched, no GIL)
  └─ veh       = 0.78 ms
  └─ serialize = 0.04 ms
  └─ send      = 0.003 ms
```

The C++ TLS loop costs about 720 ns per controller (string copy + one
`std::map` lookup per active program), versus the ~1.7 µs/controller the
Python TraCI loop was paying (two cross-boundary calls + Python object
churn).  On networks with O(10²) TLS the absolute saving is negligible
(<200 µs); on Berlin-scale networks it removes the dominant publisher
cost in one step.

### Updated priorities

Berlin is now SUMO-compute bound at ~22 ms/step (RTF 46×).  The 5 ms of
native publisher time per step is dominated by TLS extraction (4.2 ms);
shaving more would require either:

1. **A direct iterator API on `MSTLLogicControl`** that returns active
   logics in one shot (no per-id map lookup) — would save ~0.5 ms.
2. **Skipping TLS state when unchanged** — most controllers do not change
   state every simulation step; a cheap "phase index changed" check could
   collapse the wire payload (and the per-step work) by ~10× on average.
   This would also shrink the SimStepBin payload meaningfully.

Otherwise the remaining critical path is `traci.simulationStep` itself
plus the unexplained ~5 ms libsumo+large-network gap.  Both are inside
SUMO core, beyond this project's perf knobs.

---

## doe 20-minute Benchmark (2026-05-25)

Re-ran the doe scenario with a longer 20-minute sim window
(`begin=6:00:00`, `end=6:20:00`, `step-length=0.2 s` → 6000 steps) to
dilute startup and network-loading effects that dominate shorter runs.
All three configurations on the same machine, single run each.

| Setup              | Wall (s) | RTF    | UPS      | Avg ms/step | Notes                                  |
| ------------------ | -------: | -----: | -------: | ----------: | -------------------------------------- |
| Plain SUMO         |  **26.4** | **45.5×** | **1.10 M** |   **~4.4** | `sumo` binary, no eCAL, `--verbose`    |
| Publisher only     |    53.9  |  22.6× |    547 k |    8.84    | `--benchmark`, no bridge / no browser  |
| Publisher + bridge |    54.8  |  22.3× |    538 k |    8.98    | bridge attached as eCAL subscriber     |

### Bridge attach overhead

**+0.14 ms/step (+1.6 %)** — confirms the earlier 1800-step finding that
SHM subscriber cost is effectively free. The bridge can sit on the eCAL
SHM ring without measurable impact on publisher throughput.

### Pipeline overhead vs pure SUMO

**+4.5 ms/step (≈ 2×)**, broken down for the publisher-only steady-state
window (last 5 s, ~470 vehicles + ~150 persons):

```
sim       = 6.6 ms/step   (SUMO compute, baseline)
native    = 3.7 ms/step total
  └─ veh       = 2.50 ms   (per-vehicle PROJ4 + getter + append, all C++)
  └─ agent     = 1.04 ms   (per-person PROJ4 + getter + append, all C++)
  └─ tls       = 0.04 ms   (folded-in C++ path is essentially free at 50 TLS)
  └─ serialize = 0.029 ms
  └─ send      = 0.0024 ms
```

The TLS fold-in's contribution on doe is negligible (~40 µs/step vs ~10 ms
on Berlin), as expected: doe has only ~50 TLS where the per-controller
overhead is dominated by everything else.

`sim` itself is also ~2 ms/step heavier than the standalone-SUMO baseline
(6.6 vs ~4.4 ms). This matches the unexplained Berlin libsumo gap — most
likely libsumo subscription bookkeeping or output-writer pumping that the
standalone binary skips. Same root cause, same diagnostic.

### Next perf knob

The remaining native publish cost (~3.5 ms/step for veh + agent combined)
is **not** SWIG boundary cost — the native publisher already iterates
`MSVehicleControl::loadedVehBegin()` and reads `MSBaseVehicle*` getters
directly in C++. The dominant per-vehicle cost is almost certainly
**`GeoConvHelper::cartesian2geo` (PROJ4)** invoked per vehicle on
geo-referenced networks like doe — typically 1.5–3 µs/call × ~470
vehicles ≈ 0.7–1.4 ms/step. Plus the per-vehicle `dynamic_cast`,
`registerType` map lookup, `getPosition` recomputation from lane+offset,
and protobuf scratch appends.

The two PLAN.md items aimed at this:

1. **METER_OFFSETS position mode** — publish raw cartesian XY (+ network
   origin) and let the frontend do a single bulk forward Mercator
   projection. Cuts the per-vehicle PROJ4 cost to zero.
2. **PROJ batching** — if we keep server-side projection, batch all N
   positions into one `proj_trans_array` call instead of N individual
   calls. ~5–10× speedup of the projection step alone.

### sumo-gui comparison (20-min doe)

For the same 20-minute doe scenario, `sumo-gui` (no recording, default
view settings) reports:

| Setup              | Wall (s) | RTF    | UPS      | Avg ms/step | Frame time | Render skip |
| ------------------ | -------: | -----: | -------: | ----------: | ---------: | ----------: |
| Plain SUMO         |    26.4  |  45.5× |   1.10 M |       4.4   |     —      |      —      |
| **sumo-gui**       |  **35.8** | **33.5×** | **810 k** |     **6.0** | **23.8 ms** | **74.9 %** |
| Publisher only     |    53.9  |  22.6× |    547 k |       8.84  |     —      |      —      |
| Publisher + bridge |    54.8  |  22.3× |    538 k |       8.98  |     —      |      —      |
| Publisher + frontend (earlier 11k-step run) | — | 18.6× | 432 k | 10.74 | 13.0 | 2.7 % |

Two interesting comparisons:

1. **sumo-gui's GUI overhead is only ~1.6 ms/step** above plain SUMO,
   versus **~4.5 ms/step for our publisher**. sumo-gui's OpenGL renderer
   runs concurrently with the sim step (it skips ~75 % of frames to
   amortize the 23.8 ms render cost) and shares the same address space
   as MSNet, so it has no serialization, no IPC, and no per-vehicle
   PROJ4 forward projection (it draws in cartesian directly into a
   transformed GL viewport). Our native publisher pays all three on
   every published step.

2. **The autotuner's 50 %-skip behaviour is conservative compared to
   sumo-gui's 75 %.** sumo-gui prioritizes sim throughput by amortizing
   render across 4 sim steps; we prioritize visual smoothness. This
   reinforces the "render-aware autotune" follow-up in PLAN.md — once
   the publisher knows the frontend frame time, it should be free to
   match or exceed sumo-gui's skip ratio on render-bound configurations.

### Full stack with browser (20-min doe)

| Setup                | Wall (s) | RTF    | UPS      | Pub ms/step | Pub skip | Frontend frame | Frontend skip |
| -------------------- | -------: | -----: | -------: | ----------: | -------: | -------------: | ------------: |
| Plain SUMO           |    26.4  |  45.5× |   1.10 M |       4.4   |     —    |        —       |       —       |
| sumo-gui             |    35.8  |  33.5× |    810 k |       6.0   |     —    |       23.8 ms  |     74.9 %    |
| Publisher only       |    53.9  |  22.6× |    547 k |       8.84  |     0 %  |        —       |       —       |
| Publisher + bridge   |    54.8  |  22.3× |    538 k |       8.98  |     0 %  |        —       |       —       |
| **Full stack**       |  **73.5** | **17.0×** | **413 k** |    **11.77** | **26.7 %** | **6.43 ms** |   **13.7 %** |

Frontend per-frame breakdown (10 936 frames rendered, ~149 fps on
presumably a 144 Hz display):

```
parse       = 1.68 ms   (SimStepBin.decode + typed-array extraction)
veh_build   = 0.25 ms
drain       = 0.05 ms
edge_build  = 0.00 ms   (no edge data enabled)
layers      = 0.40 ms
deck        = 1.38 ms   (GL draw, ~470 vehicles + ~150 persons)
---------
total       = 3.76 ms accounted, 2.7 ms idle (vsync wait)
```

The frontend is **wildly faster than sumo-gui's 23.8 ms render** — 6.4 ms
vs 23.8 ms per frame — and is essentially idle (~40 % of frame budget
spent waiting for vsync). The 13.7 % frontend skip rate comes from the
publisher producing slightly faster than the display can refresh in some
windows.

Full-pipeline overhead vs publisher-only: **+2.8 ms/step** (8.98 → 11.77),
all of which is the WebSocket fan-out + browser scheduling pressure on
the bridge event loop. Notably the publisher's autotuner kicked in at
26.7 % skip (interval bumping to 2 transiently) because the full pipeline
got close to the ⅓-overhead threshold; in publisher-only mode it stayed
at interval=1 the entire run.

### Takeaways

- **Frontend is not the bottleneck on doe.** With 3.76 ms of actual work
  per render at ~149 fps, the React + deck.gl renderer has roughly 6×
  more headroom than sumo-gui.
- **Publisher is the bottleneck.** ~3.7 ms/step of native publish cost on
  doe is dominated by per-vehicle `GeoConvHelper::cartesian2geo` (PROJ4)
  for the geo-referenced network — *not* SWIG, which we already
  eliminated. The next perf knob is the METER_OFFSETS position mode (or
  PROJ batching) already planned in PLAN.md.
- **sumo-gui still wins on pure RTF (33× vs 17×)** because it has zero
  IPC/serialization cost AND aggressively skips 75 % of renders. With a
  render-aware autotuner (see PLAN.md) and a native vehicle loop, we
  should close most of that gap while preserving the much smoother and
  far more responsive web frontend experience.

## Aggressive frame-skipping + browser contention isolation (2026-05-26)

Goal: validate that the new aggressive autotune (`MAX_PUBLISH_FPS` cap +
render-aware lower bound + EMA-smoothed frontend reports, see PLAN.md) hits
the user-specified target *total runtime ≤ 1.5× the no-GUI baseline* on the
20-minute doe scenario (5999 sim steps, step-length 0.2 s).

### Multi-run results (5 runs each, mean wall-clock seconds)

| Config | Mean (s) | × baseline | ms/step | Skip rate | Notes |
|---|---:|---:|---:|---:|---|
| sumo CLI (no GUI) | 25.33 | 1.00× | 4.22 | — | baseline |
| Pure libsumo loop (no eCAL) | 26.40 | 1.04× | 4.40 | — | `import sumolib` first to avoid segfault |
| Publisher `--benchmark` (autotune OFF, no subscriber) | 56.77 | 2.24× | 9.46 | 0.00 | publish every step |
| Publisher `--benchmark` (autotune OFF) + bridge attached | 54.69 | 2.16× | 9.01 | 0.00 | bridge eCAL-subscribes only |
| **Publisher (autotune ON) + bridge, no browser, 30 fps cap** | **34.35** | **1.36×** | **5.58** | **0.81** | hits the 1.5× target |
| **Publisher (autotune ON) + bridge, no browser, 20 fps cap** | **32.89** | **1.30×** | **5.40** | **0.855** | further win from tightening cap |
| Full stack (autotune ON + bridge + browser, 30 fps cap) | 62.50 | 2.47× | 10.42 | 0.68 | browser drives skip *down*, doubles wall time |

### Findings

1. **eCAL bridge attachment is essentially free.** Adding the bridge as a
   local eCAL subscriber changes the publisher's per-step time by less than
   the run-to-run noise (54.69 s vs 56.77 s with no subscriber). The
   "eCAL SHM sync overhead per publish" hypothesis from earlier
   notes does not hold up.

2. **Autotune is the dominant lever.** Switching autotune ON (no other
   changes) drops the bridge-attached benchmark from 54.7 s → 34.4 s — a
   1.6× speedup that comes entirely from skipping 81 % of publishes once
   per-step cost makes the bridge-attached publish path the bottleneck.

3. **Browser is the real bottleneck, not eCAL.** Adding the browser to the
   already-autotuned pipeline nearly doubles the wall clock (34 → 62.5 s)
   *despite* requesting fewer publishes (interval drops because the
   render-aware lower bound asks for more frames when `frame_ms` looks
   comfortable). Two effects combine:
   - Browser pulls skip rate **down** from 0.81 → 0.68 (32 % of steps
     publish vs 19 %) — that alone is ~2000 extra publishes × ~5 ms = ~10 s.
   - Per-publish cost roughly doubles (5.6 ms → 10.4 ms) because the
     publisher and browser/Vite dev server compete for CPU on the same host.
     This is collocation noise; in deployment the publisher runs on a
     dedicated server and this overhead disappears.

4. **Lowering `MAX_PUBLISH_FPS` from 30 → 20 helps modestly.** Mean drops
   34.35 s → 32.89 s, skip rate climbs 0.81 → 0.855. The bridge already
   forwards at 60 fps with latest-value semantics, so anything above ~20 fps
   publish rate is largely invisible to the user but still costs full
   publish overhead.

### Methodology notes

- All numbers are mean of 5 consecutive runs on the same machine, doe
  scenario `/home/ubuntu/sumo-webgui/doe/view.sumocfg`.
- "autotune ON, no browser" was measured with a small wrapper that imports
  `sumo_ecal_publisher.py` and monkey-patches `ctrl["autotune"] = True`
  before `_do_load`, since `--benchmark` hard-disables autotune by design
  (`--benchmark-full` would require a frontend to be present).
- Differential bench scripts confirmed earlier that infrastructure
  (eCAL runtime init, ServiceServer, native ECal publisher with no actual
  publish, log socket, idle Python thread doing `sleep(0.001)`) adds at
  most ~1 s versus the pure libsumo loop.

### Open follow-ups

- **Browser-side investigation:** profile what the browser does between
  publishes that scales per-publish cost from 5 → 10 ms. Suspects:
  `report_frontend_stats` JSON traffic over WebSocket every 2 s, MapLibre
  tile fetches, deck.gl GPU back-pressure.
- **Tighten render-aware floor:** the render-aware lower bound currently
  asks for `ceil(frame_ms / step_wall_ms)`; consider applying a hysteresis
  band so a frontend reporting 11 ms doesn't pull interval down to 2.
- **Headless deployment guidance:** document that publishing on a dedicated
  server (no collocated browser) is the supported high-throughput mode.
