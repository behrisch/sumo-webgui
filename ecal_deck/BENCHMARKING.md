# Benchmarking Methodology

## Performance targets

| Scenario | Metric | Target |
|---|---|---|
| Berlin peak (100 k veh), binary proto + viewport culling | step overhead vs no-GUI baseline | < 1.5× |
| Berlin peak, C++ publisher | step overhead vs no-GUI baseline | < 1.1× |
| Frontend, 100 k vehicles (binary proto) | P95 frame time | < 33 ms |
| Frontend, 10 k visible edges (culled) | P95 frame time | < 16.6 ms |
| Frontend, memory stability | JS heap after 5 min | < 200 MB |

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
# Option 1: SUMO built-in duration log
sumo -c path/to/sim.sumocfg --duration-log.statistics true 2>&1 | grep "Duration"

# Option 2: publisher with a --no-publish flag (to be added)
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
