# SUMO GUI Framework Evaluation

Central planning document for the ongoing evaluation of GUI frameworks as
replacements / companions for `sumo-gui`.  Sub-directory PLANs (
`ecal_deck/PLAN.md`, `qt_cpp/PLAN.md`) remain the authoritative
implementation guides for their respective prototypes; this document is the
comparative overview and decision log.

---

## Goals

1. Find a GUI framework that keeps the simulation overhead **≤ 1.5× the
   no-GUI baseline** at city scale (Berlin: ~600 k edges, ~100 k vehicles).
2. Reach visual parity with `sumo-gui` (lanes, TLS, vehicles, persons,
   polygons, edge-data coloring, scale bar, picking).
3. Support Linux desktop first; Windows/macOS later if the choice is obvious.
4. Be maintainable by SUMO contributors without requiring a narrowly-specialist
   technology stack.

---

## Benchmark reference

All overhead numbers are from `ecal_deck/BENCHMARKING.md` unless noted.
"Baseline" = `sumo` binary, no GUI, no libsumo, same scenario.

| Setup | Wall (doe 20-min) | RTF | Overhead |
|---|---|---|---|
| Plain SUMO (baseline) | 25.3 s | 45.5× | 1.00× |
| sumo-gui | 35.8 s | 33.5× | 1.41× |
| deck.gl headless (autotune, 20 fps cap) | 32.9 s | 38.5× | **1.30×** |
| deck.gl full-stack (browser collocated) | 73.5 s | 17.0× | 2.90× |
| Qt6 in-process | TBD (Phase 3) | — | target ≈ 1.1× |

Key insight: the 1.30× headless deck.gl number already beats sumo-gui.  The
2.90× full-stack number is a collocation artefact — browser and publisher
competing for CPU on the same host.  On separate hosts the pipeline cost
drops to approximately the headless number.

---

## Candidate overview

### 1. deck.gl + eCAL (web stack)  ·  `ecal_deck/`

**Status: mature, near-complete.**

Architecture: `sumo_ecal_publisher.py` → eCAL SHM → `ecal_ws_bridge.py` →
WebSocket → React / deck.gl / MapLibre in browser.  C++ native publisher
(`libsumo::ECal`) embedded in libsumo handles the hot vehicle/edge/TLS loop.

**Strengths**
- Basemap (MapLibre / OpenFreeMap) out of the box.
- Remote monitoring: publisher on a server, viewer in any browser.
- TypeScript + React ecosystem for UI iteration.
- Frontend has ~10 ms/frame of vsync headroom; deck.gl is not the bottleneck.
- Binary protobuf transport + viewport culling implemented and benchmarked.

**Weaknesses**
- Collocated deployment doubles wall time (CPU contention, not a pipeline
  issue; disappears when publisher and browser are on separate hosts).
- Browser start-up overhead; no offline/standalone binary.
- WebGPU not yet fully landed in deck.gl 9 (GL path still default).

**Next steps**
- Tauri packaging (see `ecal_deck/TAURI.md`): single binary, native file
  picker, OS WebView eliminates the Vite dev-server overhead.
- METER_OFFSETS position mode: skip per-vehicle PROJ4 → saves ~1 ms/step on
  geo networks (designed in `ecal_deck/PLAN.md`, not yet implemented).
- Publisher-side viewport culling via R-tree (designed in `ecal_deck/PLAN.md`).
- NetworkGeometry extensions: sidewalk/footpath distinction.

---

### 2. Qt6 + OpenGL 3.3 (native C++)  ·  `qt_cpp/`

**Status: Phase 2 complete (live vehicles rendering), entering Phase 3.**

Architecture: single process, `SimWorker` QThread owns libsumo, double-buffered
`SimSnapshot` passed to `NetworkView` (QOpenGLWidget) for instanced rendering.

**Strengths**
- In-process libsumo: no IPC, no serialization, no WebSocket.
- Predicted overhead ≈ sumo-gui (~1.1×) based on eliminating all pipeline costs.
- Qt6 widget toolkit: toolbar, dock widgets, dialogs, cross-platform.
- Same C++ codebase as SUMO core; contributions and review are natural.

**Weaknesses**
- No basemap yet (deferred to Phase 5; will need a tile fetcher or QtLocation).
- OpenGL 3.3 core: not WebGPU-ready; will not run in a browser.
- Larger codebase to maintain vs the web frontend.

**Phase 3 scope** (≈ 3 days)
- Persons, TLS heads, stop lines, stopping places, polygons, detectors.
- Rails with sleepers (port `NetworkLayer.ts` checkpoint-017 logic).
- EdgeDataLayer with viewport culling + color-scale legend.

**Phase 4** — picking, log panel, screenshot diff vs deck.gl, benchmarks.

**Open question**: does Qt6's overhead actually land at ~1.1× on doe?  The
benchmark will be the first hard data.  If it matches, that settles the
"native vs web" question cleanly.

---

### 3. Godot

**Verdict: not recommended for this use case.**

Godot's scene graph is designed for discrete game objects.  A Berlin network
(600 k edges) would require either a single custom mesh (defeating the point
of the engine) or ~600 k `Node` instances (catastrophically slow).  No native
libsumo C++ integration path without GDExtension, which re-introduces an IPC
boundary comparable to eCAL.  GDScript/C# is an extra language burden for
SUMO contributors.

Could be revisited if a 3D scenario with buildings, elevation, and a
game-like interaction model is ever prioritised.

---

### 4. Bevy (Rust) + wgpu / WebGPU

**Verdict: interesting 3D path, but high entry cost.**

Bevy's ECS is architecturally well-matched to SUMO entities (vehicles and
pedestrians are perfect ECS entities with `Position`, `Speed`, `TypeId`
components).  wgpu backend targets Vulkan/Metal/DX12/WebGPU/WebGL2 from one
codebase.  Can compile to WASM.

Risks:
- Requires Rust expertise not currently in the SUMO contributor pool.
- Bevy 1.0 shipped recently; ecosystem still stabilising.
- libsumo integration requires `cxx`/unsafe FFI; not trivial.
- `bevy_egui` control panel is functional but not as polished as Qt6.

**When to revisit**: after the Qt6 benchmark settles the 2D native question;
if a 3D rendering capability (building extrusions, elevation, camera tilt) is
a stated requirement.

---

### 5. bgfx + Dear ImGui (C++)

**Verdict: add as a comparison data point against Qt6.**

bgfx is a thin cross-platform rendering API abstraction in C++ (OpenGL /
Vulkan / Metal / DX11/12 / WebGPU).  Dear ImGui is an immediate-mode UI
library, single-header, widely used in simulation and game-dev tooling
(NVIDIA Omniverse, CARLA, SUMO debug overlays).

Pairing them gives a stack that:
- Has zero widget-framework overhead (ImGui renders entirely via GPU draw lists)
- Compiles to WASM via Emscripten
- Ships to Linux/Windows/macOS with one codebase
- Integrates trivially with libsumo (plain C++ include)

The key experiment: implement the same VBO-based lane + vehicle rendering as
Qt6 but driven by GLFW + ImGui instead of QOpenGLWidget.  If the benchmark
numbers are identical, Qt6 is preferred for its polished widget toolkit with
no perf penalty.  If ImGui is measurably faster, it becomes the minimal
desktop/headless-display option.

**Estimated effort**: 2-3 days for a benchmark-ready prototype using shared
rendering code from `qt_cpp/src/layers/` and `qt_cpp/src/gl/`.

---

### 6. Pygfx (Python + wgpu-py)  ·  *new, recommended prototype*

**Verdict: highest-value untried option given the existing stack.**

[pygfx](https://github.com/pygfx/pygfx) is a scene graph built on
[wgpu-py](https://github.com/pygfx/wgpu-py), which is a Python binding to
the same `wgpu` Rust library Bevy uses.

Why it fits here:
- Stays entirely in Python — the publisher/bridge expertise and the existing
  `sumo_ecal_publisher.py` are reusable without change.
- WebGPU-native: same GPU backend as Bevy, targeting Vulkan/Metal/DX12 on
  desktop and WebGL2/WebGPU in the browser.
- Designed for scientific data, not games: `LineSegmentMaterial`,
  `PointsMaterial`, instanced `MeshMaterial` are first-class.
- Could replace the deck.gl React frontend entirely while keeping the Python
  publisher and eCAL bridge unchanged, or be driven directly in-process
  (skipping WebSocket).
- Framing: "deck.gl is the JavaScript/wgpu path; Pygfx is the Python/wgpu path."

Risks:
- Relatively new (first stable release 2023); smaller community than deck.gl.
- No tile basemap support (would need a separate tile layer or
  osmium/maplibre-native binding).
- WASM/browser path via Pyodide is still experimental.

**Suggested prototype**: wire the existing binary `SimStepBin` output from
`ecal_ws_bridge.py` into a Pygfx window (lanes as `LineSegments`, vehicles as
instanced `Meshes`, edge data as colored `LineSegments`).  Estimated 2-3 days.

---

### 7. VisPy (Python + OpenGL/WebGL)  ·  *honorable mention*

The predecessor to Pygfx, more mature, larger user base.  Its `visuals` API
maps cleanly to the layer model (`Line`, `Mesh`, `Markers`).  Can target a
Qt6 backend for the window (`vispy.app.use_app('pyqt6')`).

Less interesting than Pygfx now that wgpu-py provides a modern GPU path, but
worth considering if Pygfx proves too immature: VisPy's OpenGL path is battle-
tested on scientific datasets comparable in size to a Berlin network.

---

### 8. MapLibre GL Native (C++)  ·  *if basemap is critical*

The same tile renderer used in the deck.gl frontend exists as a
[C++ library](https://github.com/maplibre/maplibre-native) (Metal/OpenGL/
Vulkan).  Integrating it into the Qt6 prototype would give a tile basemap
from day one — the same visual result as the web version — without a browser.

Trade-offs: MapLibre Native's API targets mobile/embedded and is less
ergonomic than Qt6 for desktop windowing; it would sit alongside Qt6 rather
than replacing it.  Deferred until Phase 5 tile-basemap work begins.

---

### 9. Sokol + cimgui (C, cross-platform)  ·  *if WASM/embedded is a priority*

[sokol_gfx](https://github.com/floooh/sokol) is a single-header C graphics
API abstraction (GL/Metal/DX11/WebGPU/WASM).  `sokol_app` handles windowing.
Compiles to WebAssembly with no modification.

Relevant if SUMO ever needs an embedded or WASM viewer (e.g. for
documentation, web-based education tools, or CI screenshot rendering).  Not
a priority while the deck.gl WASM path via the browser already satisfies
that use case.

---

## Framework comparison matrix

| Criterion | deck.gl | Qt6 + OGL | ImGui + OGL | Pygfx (wgpu-py) | Bevy (wgpu-rs) | Godot |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| Publisher overhead | ~4.5 ms | ~2 ms (est.) | ~2 ms (est.) | ~4 ms | ~2 ms (est.) | ❌ |
| Basemap out of box | ✅ | ❌ Phase 5 | ❌ | ❌ | ❌ | ❌ |
| Remote / browser viewer | ✅ | ❌ | ❌ | Experimental | ✅ WASM | ✅ WASM |
| WebGPU-ready | Partial | ❌ GL3.3 | ❌ | ✅ | ✅ | ✅ Vulkan |
| WASM target | ✅ | ❌ | ✅ sokol | Experimental | ✅ | ✅ |
| In-process libsumo | ❌ | ✅ | ✅ | Optional | FFI | GDExtension |
| Language | TypeScript | C++ | C++ | Python | Rust | GDScript/C++ |
| Widget toolkit | React | Qt6 ✅ | ImGui (basic) | Qt or GLFW | bevy_egui | Built-in |
| Maturity for this use | ✅ | ✅ | ✅ | 🆕 | 🆕 | ❌ wrong fit |

---

## Recommended evaluation sequence

### Step 1 — Finish Qt6 Phase 3 + benchmark  *(in progress)*

The single most informative experiment.  The headline question: does the
in-process path actually land at ~1.1× overhead (matching sumo-gui), or does
Qt6's event loop add measurable cost?  Run the same `doe/view.sumocfg` 20-min
benchmark as in `ecal_deck/BENCHMARKING.md` and extend that table with Qt6
numbers.

### Step 2 — Tauri packaging of deck.gl  *(low effort, high value)*

Wrap the existing React/deck.gl frontend in a Tauri shell (see
`ecal_deck/TAURI.md`).  This resolves the "collocated browser CPU contention"
measurement (73.5 s → should approach 32.9 s) and gives a single distributable
binary.  Not a new framework but closes off the deck.gl evaluation path.

### Step 3 — Dear ImGui + GLFW comparison prototype  *(2-3 days)*

Reuse `qt_cpp/src/layers/` VBO code in a minimal GLFW + ImGui shell.  Benchmark
identically.  Data point: quantifies how much of Qt6's overhead (if any) is the
widget framework vs. the rendering path.

### Step 4 — Pygfx prototype  *(2-3 days)*

Wire the existing `ecal_ws_bridge.py` binary output (or the publisher directly)
into a Pygfx scene.  Target: lanes as `LineSegments`, vehicles as instanced
meshes, edge data as colored lines.  Benchmark vs. deck.gl on the same scenario.
Tests the hypothesis that a Python/wgpu path can close the browser-overhead gap
while staying in Python.

### Step 5 — Bevy prototype  *(only if 3D capability is required)*

Prototype only if a 3D rendering requirement (elevation, building extrusions,
camera tilt) is confirmed.  Entry cost (Rust expertise, FFI setup) is too high
to justify for the 2D case alone.

---

## Decision criteria

The evaluation concludes when one framework satisfies all three of:

1. **Overhead ≤ 1.5× baseline** on the doe 20-min scenario (target) and
   **≤ 1.1× baseline** on a headless/dedicated-server deployment (stretch).
2. **Visual parity** with `sumo-gui` confirmed by side-by-side screenshot on
   `doe/view.sumocfg` and `berlin/test_short.sumocfg`.
3. **Maintainability**: the implementation language and framework are already
   in use by SUMO contributors, or the framework is simple enough to learn
   from the codebase without external training.

The Qt6 prototype is the most likely candidate to satisfy all three on the
desktop path.  The deck.gl stack satisfies them on the remote/browser path.
Both can coexist: Qt6 for local high-performance use, deck.gl/Tauri for
remote monitoring and distribution.

---

## Open questions

- Does the Qt6 step overhead actually match the ~1.1× sumo-gui figure, or does
  the libsumo subscription bookkeeping overhead (the unexplained ~2 ms/step gap
  observed in `bench_libsumo_only.py`) still apply in-process?
- Is the "collocated browser CPU contention" effect entirely explained by OS
  scheduler competition, or does the Vite dev server contribute via file-watch
  inotify / HMR?  (Test: `npm run build && npm run preview` vs `npm run dev`
  — tracked in `ecal_deck/PLAN.md` open items.)
- For the Pygfx prototype: can wgpu-py share a wgpu `Device` with the existing
  eCAL publish loop on the same thread, or does Pygfx require its own render
  thread?
- If both Qt6 and deck.gl are kept long-term: is the shared `libsumo::ECal`
  C++ publisher (which deck.gl uses via eCAL) also useful as a live-data source
  for a Qt6 "monitoring" mode where the simulation runs on a server?

---

*Last updated: 2026-05-27*
