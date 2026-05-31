# qt_cpp — Qt6 + RHI SUMO GUI (C++)

A native desktop reimplementation of the `ecal_deck` web GUI for direct
comparison. Same visual functionality and look-and-feel, but no browser, no
WebSocket, no eCAL, no protobuf wire format. The GUI drives libsumo directly
in-process.

## Goals

- **Same feature set** as the `ecal_deck` frontend at parity time (see "Feature
  parity" below).
- **Direct libsumo coupling** — embed libsumo as a C++ library, step the
  simulation from the GUI thread or a worker thread, read state via the libsumo
  C++ API (no TraCI socket, no eCAL, no protobuf).
- **Qt6 Widgets + QRhiWidget** with Qt's RHI (Vulkan/Metal/D3D/OpenGL) for
  rendering. No QtQuick/QML — keep dependencies minimal and rendering paths
  explicit, mirroring how deck.gl layers are structured. On Qt 6.4-6.6 a
  ~150-line in-tree `RhiHostWidget` backport stands in for `QRhiWidget`; on
  Qt 6.7+ the native widget is used.
- **Benchmark target**: feed the same `doe/view.sumocfg` scenario and compare
  FPS, CPU, and memory against the eCAL+browser stack documented in
  `ecal_deck/BENCHMARKING.md`.

## Non-goals

- Cross-platform mobile/web support (desktop Linux first; Windows/macOS later
  if trivial).
- A plugin/extension system. Stops at the parity feature set.
- Replacing sumo-gui. This is a comparison harness, not a SUMO upstream
  contribution.

## Architecture

Single-process, two main threads:

```
+----------------- main / GUI thread -----------------+
|  Qt event loop                                       |
|  MainWindow (toolbar, file menu, status bar)         |
|  NetworkView : QRhiWidget                            |
|    - render(cb) draws layers using cached RHI buffers|
|    - mouse/keys -> Camera (pan/zoom/reset)           |
|  ScaleBarOverlay, LegendOverlay, LogPanel            |
+------------------------------------------------------+
                ^                    |
        Qt signals/slots      QMetaObject::invokeMethod
                |                    v
+--------------- SimWorker (QThread) ------------------+
|  Owns libsumo::Simulation lifetime                   |
|  Step loop with configurable delay                   |
|  After each step:                                    |
|    - pulls vehicles/persons/TLS/edge-data via libsumo|
|    - writes into SimSnapshot (double-buffered)       |
|    - emits stepReady() signal                        |
|  Handles control commands (start/pause/step/reset/   |
|    setDelay/loadConfig) via slots                    |
+------------------------------------------------------+
```

State exchange between threads is a **double-buffered `SimSnapshot`**:
typed-array-like `std::vector<float>` / `std::vector<uint32_t>` mirroring the
proto SimStep fields. The render thread reads the "front" buffer; the sim
thread writes the "back" buffer and swaps under a mutex (single pointer swap).
This mirrors the RAF-batching + ref pattern in `useSimSocket.ts` but is even
simpler because there's no network or serialization in the middle.

NetworkGeometry is built **once** at load time on the SimWorker thread
(equivalent to `_build_network_binary` in `sumo_ecal_publisher.py`), then
uploaded into GPU buffers by the GUI thread and never re-uploaded unless the
.sumocfg changes.

## Module layout

```
qt_cpp/
  PLAN.md                 (this file)
  CMakeLists.txt
  README.md
  src/
    main.cpp
    MainWindow.{h,cpp}              # menus, toolbar, status bar
    NetworkView.{h,cpp}             # QRhiWidget host, Camera, picking
    NetworkOverlayWidget.{h,cpp}    # transparent child for QPainter overlays
                                    # (scale bar, legend, pick info box)
    Camera.{h,cpp}                  # pan/zoom/reset, view+proj matrices,
                                    # geo<->screen helpers (MapView equiv)
    sim/
      SimWorker.{h,cpp}             # QThread owning libsumo
      SimSnapshot.{h,cpp}           # double-buffered state struct
      NetworkGeometry.{h,cpp}       # built once from libsumo network
      EdgeColorizer.{h,cpp}         # vehicle/edge attribute -> color
    layers/                         # one class per deck.gl layer
      VehicleLayerRhi.{h,cpp}       # instanced rotated quads per vehicle
      PersonLayerRhi.{h,cpp}        # instanced quads
      POILayerRhi.{h,cpp}           # instanced discs
      TLSLayerRhi.{h,cpp}           # instanced oriented bars + state colour
      LayerBuilders.{h,cpp}         # build*Verts(NetworkGeometry&) free
                                    # functions for the 8 static sub-layers
                                    # (Detector, StoppingPlace, StopLine,
                                    # PedArea, Rail, Polygon, Network,
                                    # EdgeColor) — all share two pipelines
                                    # (Triangles + TriangleStrip).
    rhi_compat/                     # Qt 6.4 backport + shared RHI helpers
      rhi_compat.h                  # picks public-or-private QRhi includes
      RhiWidgetBase.h               # alias: QRhiWidget on 6.7+, else shim
      RhiHostWidget.{h,cpp}         # ~150-line backport for Qt 6.4-6.6
                                    # (TU is empty on Qt 6.7+ via #if guard)
      RhiLayerCommon.h              # loadShader, ensureCapacity, alphaBlend
      TrisPasses.h                  # TrisColorInterleavedPass + vertex helpers
      StaticTrisRhi.h               # generic CPU-staged vertex container
    shaders/                        # GLSL 440 source compiled by qsb to .qsb
      tris_color.{vert,frag}        # shared per-vertex-rgba pass
      poi.{vert,frag}               # instanced disc
      tls.{vert,frag}               # instanced oriented bar
      vehicle.{vert,frag}           # instanced rotated quad
      person.{vert,frag}            # instanced quad
  resources/
    qt_cpp.qrc                      # shaders, icons
    icons/...
```

## Build system

CMake ≥ 3.20. Targets:

- `qt_cpp` (executable)
- Links against `Qt6::Widgets`, `Qt6::OpenGLWidgets`, `Qt6::Concurrent`, and
  libsumo (`libsumocpp.so` from `$SUMO_HOME` or system install).
- C++20.

Build flow:

```bash
cmake -S qt_cpp -B qt_cpp/build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DSUMO_HOME=/home/ubuntu/sumo
cmake --build qt_cpp/build -j
./qt_cpp/build/qt_cpp --sumocfg ../doe/view.sumocfg
```

A `find_package(Qt6 COMPONENTS Widgets OpenGLWidgets REQUIRED)` plus a small
custom `FindLibsumo.cmake` (looks under `$SUMO_HOME/build*/src/libsumo` and
`$SUMO_HOME/src/libsumo`, falls back to `pkg-config`).

## libsumo coupling

We use libsumo's C++ API directly (`#include <libsumo/Simulation.h>` etc.) —
the same backend the Python `libsumo` module wraps. This bypasses the TraCI
socket and avoids the protobuf serialization that dominated the eCAL path's
publisher CPU.

Per-step data needed by the GUI (mirroring `_build_and_publish_simstep`) is
extracted in **one batched call per step** via `libsumo::Batch` (see
`/home/ubuntu/sumo/src/libsumo/Batch.{h,cpp}`, exposed as `traci.batch` in
SWIG). The Batch module fills typed C++ vectors in-engine in a single sweep,
avoiding the per-object SWIG marshalling overhead of
`getAllSubscriptionResults`. It populates:

- Vehicles: id, position (xy), angle, speed, type, vClass, color, signals,
  waiting time, lane.
- Persons: id, position, angle, type, vClass.
- Traffic lights: id, current state string.
- Edge attribute column (only fetched when an attribute is active, like the
  publisher does) — speed / occupancy / CO2 / etc.

Plus `Simulation::getTime()` for the timestamp. The resulting vectors are
swapped into the front `SimSnapshot` for the GUI thread to read.

Network geometry (built once per scenario load):

- All edges + lanes (incl. internal): shape, width, vClass permissions,
  function (normal/internal/crossing/walkingarea), lane index.
- Junctions: shape.
- TLS: position, link indices.
- Polygons + POIs from `.poly.xml`.
- Stops, detectors.

All of this is available from libsumo via `Lane::getShape`, `Lane::getWidth`,
`Lane::getAllowed`, `Edge::getLaneNumber`, `Junction::getShape`,
`Polygon::getShape`, `BusStop::getStartPos/EndPos`, etc. The geo-vs-cartesian
contract from the existing publisher (do not call cartesian2geo for non-geo
nets with non-zero netOffset) must be preserved — copy the logic from
`sumo_ecal_publisher.py:_build_network_binary` (cited in repo memories).

## Rendering

- **Qt RHI** (Vulkan / Metal / D3D / OpenGL backend, auto-selected). One
  `QRhiWidget` (or its Qt 6.4-6.6 backport) for the map; static layers share
  two pipelines (Triangles + TriangleStrip) over a single shared
  `TrisColorInterleavedPass`; dynamic instanced layers (Vehicle, Person, POI,
  TLS) each own their own pipeline.
- Camera in either:
  - geo mode → equirectangular projection of lon/lat with a Mercator scale
    correction per latitude, identical formula to `MapView` defaults; or
  - non-geo mode → straight orthographic in SUMO XY (meters).
- Layer render order (must match `ecal_deck`):
  junctions → road network → edge-data coloring → ped areas → rails →
  polygons → POIs → stopping places → detectors → TLS heads → stop lines →
  vehicles → persons.
- Vehicles drawn with **instanced rendering**: one base 2-triangle quad mesh
  + per-instance buffer `[x, y, cos_angle, sin_angle, r, g, b, a]`. Buffer is
  re-uploaded once per SimSnapshot swap, mirroring deck.gl's `ScatterplotLayer`
  attribute updates.
- Picking is CPU-side using the current `SimSnapshot` + `NetworkGeometry`
  (no GPU read-back). Pick info is drawn by `NetworkOverlayWidget` via
  QPainter on top of the RHI surface.
- MSAA currently 1 sample (can be raised via `setSampleCount()` on the host
  widget if needed).

## Feature parity checklist

UI:
- [ ] File → Open `.sumocfg`
- [ ] Play / Pause / Single-step / Reset toolbar
- [ ] Delay slider (ms per step)
- [ ] Sim-time display, FPS display
- [ ] Edge attribute selector dropdown (speed, occupancy, CO2, …)
- [ ] Color scale legend overlay
- [ ] Scale bar overlay (bottom-right, like `ScaleBar.tsx`)
- [ ] Reset view button → reframe to network bbox
- [ ] Log panel (libsumo warnings + GUI events)
- [ ] Vehicle tooltip / details on click (id, type, speed, route)
- [ ] Status bar: cursor coordinates (XY and lon/lat in geo mode)

Rendering:
- [ ] Geo + non-geo modes
- [ ] Roads with lane widths
- [ ] Rails with sleepers, both pure-rail (cls 3) and embedded
      tram (cls 4, inverted style) — matches `NetworkLayer.ts` work from
      checkpoint 017
- [ ] Internal lanes, crossings, walking areas
- [ ] Stop lines
- [ ] Stopping places (bus stops etc.)
- [ ] Polygons + POIs
- [ ] Detectors
- [ ] TLS heads coloured by current phase
- [ ] Vehicles, persons, containers with vClass-specific shapes
- [ ] Edge attribute coloring with viewport culling
- [ ] No basemap initially (defer MapLibre-equivalent until other parity is
      done; could later use a QtLocation tile provider or an OSM XYZ fetcher)

## Phasing

### Phase 0 — Skeleton ✅
- CMake project, `MainWindow` with empty `QRhiWidget`, clears to dark grey.
- `FindLibsumo.cmake` and a "hello libsumo" call (`Simulation::start({...})`,
  one step, `Simulation::close()`).

### Phase 1 — Network rendering ✅
- `SimWorker` loads scenario, builds `NetworkGeometry`.
- `NetworkLayer` draws lanes + junctions.
- `Camera` with pan/zoom + reset view.
- Scale bar overlay.

### Phase 2 — Live simulation ✅
- Step loop on the worker thread (`QTimer`, delay slider).
- Double-buffered `SimSnapshot` (vehicle XY / angle / colour, packed
  parallel arrays).
- `VehicleLayer` instanced rendering.
- Play/pause/step controls with shortcuts (Space / S).
- FPS counter in the status bar.

### Phase 3 — Full layer parity ✅
- Persons, TLS heads, stop lines, stopping places, polygons, detectors.
  - **Stop-line parity gap**: the qt_cpp builder used to push a stopline
    bar for every TLS-controlled link, which made signals render as two
    overlapping bars. That duplicate push has been removed; TLS bars now
    stand alone. Real stop-lines for uncontrolled minor approaches
    (`m`/`s`/`w`/`=` link states, like ecal_deck's `lane_has_stopline`)
    are still TODO in `NetworkGeometry::build`.
- Rails with sleepers (port logic from `NetworkLayer.ts` carefully — see
  checkpoint 017 for the inverted-tram trick and sleeper sizing).
- Edge attribute selector + `EdgeDataLayer` with viewport culling and color
  scale legend.

### Phase 4 — Qt RHI migration ✅
- All 12 layers ported off `QOpenGLWidget` onto `QRhiWidget` (Qt 6.7+) /
  `rhi_compat::RhiHostWidget` backport (Qt 6.4-6.6).
- Old OpenGL layer classes and base class deleted.
- See "Graphics API choice — Qt RHI migration" section for details.

### Phase 5 — Polish & parity QA

Open items needed to reach visual + functional parity with `ecal_deck`:

Vehicle rendering:
- [x] Vehicle shape selector: `rectangle | triangle | car | circle` (matches
      `ecal_deck/frontend/src/layers/vehicleShapes.ts`). Mesh swap happens
      in `VehicleLayerRhi::setShape()`; toolbar combo in `MainWindow`.
      The `car` shape uses the realistic 3-layer geometry (body polygon +
      darker front-bumper overlay + black windshield strip) with a
      per-vertex tint multiplier (UNormByte4 at attribute location 5).
      `NetworkView` caches the requested shape in `m_pendingVehicleShape`
      and applies it inside `initialize()` so a UI selection made before
      the first paint (when `m_vehicleLayer` doesn't exist yet) still
      takes effect on the first frame.
- [x] Per-type vehicle width (parallel to per-type length): extended
      `TypeColor` with `width`, added a 6th per-instance attribute (binding
      5 / location 6 `a_width`); the hard-coded 1 m half-width is gone from
      `vehicle.vert` — it now uses `max(a_width, u.min_size.y) * 0.5`.
- [x] Min-pixel sizing so vehicles stay visible when zoomed out (mirrors
      ecal's `vehicleMinPixels`). Implemented via an 80 B UBO that carries
      a `vec4 min_size` alongside the projection; `NetworkView` computes
      `min = 6 px / pixelsPerUnit` each frame and pushes it to the layer.
      Shader clamps via `max(a_length, u.min_size.x)`.
- [x] Dynamic per-vehicle coloring: `by type | speed | waiting_time |
      co2_emission | fuel_consumption`. Implemented in `SimWorker`:
      requests the extra attribute column via `Batch::fillVehicles({...})`
      and remaps to a viridis-style ramp. Toolbar combo in `MainWindow`.

Picking / info:
- [x] Vehicle / person / TLS / polygon / POI / lane / junction picking
      (already implemented in `NetworkView::pickAt`).
- [x] Enrich the picked-vehicle info box with type id and speed
      (`SimSnapshot` now carries `veh_speeds`, `veh_type_indices`,
      `type_ids`). Route + vClass + waiting time still TODO (would need
      extra Batch attrs or a per-pick `Vehicle::getRoute()` call).

UI controls (already in toolbar: play/pause/step/delay/edge color mode):
- [x] Vehicle shape combo.
- [x] Vehicle color mode combo.
- [x] Layer visibility checkboxes — Vehicles / Persons / Signals & stop
      lines / EdgeData live in the toolbar. The "Signals & stop lines"
      toggle gates both the dynamic TLS bars and the static stop-line bars
      under one switch (matches the merged `LaneBarsLayer` in ecal_deck).
      Each toggle drives both `NetworkView::set*Visible` (gates the GPU
      draw) and `SimWorker::set*Visible` (gates the corresponding
      `Batch::fill*` so hidden layers cost zero on extraction too).
      Remaining toggles (junctions, polygons, POIs, detectors, stops)
      are static/decoration layers; add when needed.
- [x] Reset view button — toolbar action + `Ctrl+0` shortcut, calls
      `NetworkView::resetView()`.
- [x] Status bar: cursor world coords (XY in meters). `NetworkView` emits
      `cursorWorldPos(x, y)` from `mouseMoveEvent`; MainWindow shows it in
      a permanent status-bar label. Lon/lat for geo-referenced nets is
      still TODO.
- [x] Color scale legend overlay for edge-data coloring. Implemented in
      `NetworkOverlayWidget::drawLegend` (title, viridis gradient bar,
      "low"/"high" tick labels). Numeric min/max ranges per mode are still
      TODO and would need them threaded through `SimSnapshot`.

Performance / fair-comparison plumbing (mirrors what the eCAL publisher
does on the Python side):
- [x] Max-fps cap for snapshot-driven renders, parallels ecal_deck's
      `MAX_PUBLISH_FPS`.  Toolbar combo ("Max fps: uncapped/10/20/30/60/120")
      drives `NetworkView::setMaxFps(int)`.  When a snapshot arrives sooner
      than `1000 / maxFps` ms after the last paint, the view skips the
      immediate `update()` and instead arms a single-shot timer for the
      remaining gap; `m_pendingSnap` already holds the freshest data, so
      deferred frames coalesce naturally and the back-pressure flag
      continues to suppress data extraction for the skipped frames.  User
      pan/zoom is never throttled.
- [x] End-of-simulation handling: `SimWorker::stepOnce()` checks
      `Simulation::getMinExpectedNumber() <= 0` after each step and pauses
      the play loop so we don't keep advancing past the configured end
      time (or after all vehicles have left).
- [x] Render-driven back-pressure: shared `std::atomic<int>` between
      `SimWorker` and `NetworkView`. The worker sets it to 1 after
      `emit snapshotReady`; the view clears it in `render()` right after
      consuming the snapshot. While set, `stepOnce()` still advances
      `Simulation::step()` but skips `buildSnapshot()` so we don't pay the
      `Batch::fill*` + viridis + memcpy cost for frames the GUI would drop.
      Toggle via `SimWorker::setBackpressure(bool)`.
- [x] "Ask only for what we render" gating: per-section flags
      (`setWindowVisible / setVehiclesVisible / setAgentsVisible /
      setTLSVisible / setEdgeDataVisible`) on `SimWorker`. Each gates its
      `Batch::fill*` (or the edge-color loop). Window visibility is hooked
      to `MainWindow::show/hide/changeEvent` so minimizing the window
      stops extraction immediately. Per-attribute (waiting/CO2/fuel)
      requesting is already conditional on the vehicle color mode.
- [x] Rolling benchmark report (~ every 2 s wall time). `SimWorker` emits
      `benchmarkReport(stepsPerSec, snapshotsPerSec, skipRate, avgStepMs,
      avgBuildMs)` and also writes the same line to stderr in the format
      `[bench] steps/s=… snapshots/s=… skip=…% avg_step=…ms avg_build=…ms`.
      `MainWindow` shows the current window in a permanent status-bar
      label. Counters reset on scenario load.

Diagnostics:
- [x] Log panel for libsumo warnings + errors + info messages. Implemented
      as a dockable `QPlainTextEdit` in `MainWindow` (toggle via View
      menu). `SimWorker` owns a `LogRouter` (QObject) plus three
      `OutputDevice` subclasses (`LogCapture`) registered with libsumo's
      static `MsgHandler::getMessage/Warning/ErrorInstance()`. Each
      capture forwards its formatted message through `LogRouter::logged`
      (auto-queued across the sim → GUI thread boundary) to a slot that
      appends an HTML-formatted line (`hh:mm:ss.zzz LEVEL message`) with
      a colour-coded level tag. `--no-warnings` is no longer passed to
      libsumo so warnings actually show up. `LogCapture` is implemented
      privately in `sim/LogCapture.cpp` (the OutputDevice subclass needs
      SUMO's `<config.h>`, which qt_cpp pulls in via an opt-in include
      path that probes a couple of common SUMO build-dir names —
      `cmclaude/src`, `build/src`, `cmake-build-release/src`).

Benchmark / QA:
- [ ] Side-by-side screenshot diff vs `ecal_deck` on `doe/view.sumocfg`.
- [ ] Sibling `qt_cpp/BENCHMARKING.md` with FPS / CPU / RSS numbers,
      using the new `[bench]` log line as the primary data source.

### Phase 6 — Optional extras
- MSAA toggle.
- Basemap tiles in geo mode (deferred — would need a Qt-side XYZ tile
  fetcher + texture cache; no easy off-the-shelf path on Qt 6.4. Re-evaluate
  on Qt 6.8+ where `QQuickWidget` + a QML MapView could be embedded.)
- Tauri-equivalent packaging (AppImage / .deb).
- Windows + macOS builds if trivial.

## Open questions

1. **Threading model**: is a single SimWorker QThread enough, or do we want
   `Simulation::step()` on the GUI thread (à la sumo-gui)? Decision: start
   with a worker thread for clean separation; revisit if libsumo callbacks
   misbehave across threads.
2. **libsumo build flavour**: link against the same SUMO build tree used by
   the Python `_libsumo.so` (`/home/ubuntu/sumo`), or build a separate
   `libsumocpp.so` install? Decision: link against the existing in-tree
   `libsumocpp.so` from `/home/ubuntu/sumo/cmclaude/src/libsumo/` to avoid
   double-builds. CMake `SUMO_HOME` variable points at the source root and we
   discover the build dir.
3. **Tile basemap**: which provider for parity with MapLibre's default? Defer.
4. **Color palettes**: load the same colormaps as the frontend (port the JS
   arrays into a header file) so the visual comparison is fair.

## Definition of done for "parity"

- Loads `doe/view.sumocfg`, plays, pauses, steps, resets, all without crashes.
- Visually indistinguishable (modulo font rendering) from `ecal_deck` on the
  same scenario at the same zoom level — checked via overlay screenshot diff.
- Benchmarks recorded for FPS, CPU, RSS at three zoom levels matching the
  existing `BENCHMARKING.md` setup.

## Shared `.pb` network cache + early-render (planned)

**Goal**: cut first-pixel-of-network latency on large nets (Berlin: ~23 s of
`libsumo::Simulation::start` → ~1 s for cached geometry). Reuse the
existing `NetworkGeometry` proto cache the ecal_deck publisher writes so
neither side reimplements the extraction.

### Why protobuf (decision)

Considered raw POD, FlatBuffers, custom binary. Chose protobuf:

- The Python publisher already writes this format; both halves stay in
  sync via `npm run generate`. A custom format would mean hand-written
  readers/writers in every language plus reinvented versioning.
- `libprotobuf` is already in Ubuntu/Fedora/Homebrew. Trivial vs. the
  ~60 MB of Qt + libsumocpp qt_cpp already pulls in.
- All heavy payloads (`lane_points`, `tls_positions`, …) are `bytes`
  fields wrapping typed arrays. C++ gets them as `const std::string&`
  whose `.data()` is reinterpret_cast'able to `const float*` —
  effectively zero-copy. "Parsing" is reading a handful of headers.

### Single source of truth: SUMO's `sumo_ecal.proto`

To avoid the protobuf descriptor-pool collision that would otherwise
fire ("Symbol name 'sumo.NetworkData' conflicts ..." at process start
because `libsumocpp.so` statically embeds its own generated code for
the same `package sumo`), qt_cpp does **not** regenerate the proto.
Instead it consumes the canonical `sumo_ecal.pb.h` from libsumocpp's
build tree via `FindLibsumo.cmake` (which appends
`${SUMO_HOME}/cmclaude/src/libsumo/` to `Libsumo::Libsumo`'s include
interface).  The generated symbols are exported by `libsumocpp.so`
itself, so no duplicate descriptor registration happens.

The schema lives upstream in `<SUMO_HOME>/src/libsumo/sumo_ecal.proto`.
The ecal_deck side keeps a checked-in `ecal_deck/proto/sumo.proto` for
fallback (Windows boxes without SUMO checked out), but `npm run
generate` (`ecal_deck/frontend/generate.ts`) syncs that file from
`$SUMO_HOME/src/libsumo/sumo_ecal.proto` whenever `SUMO_HOME` is set.
Schema changes therefore happen in **one** place — SUMO — and propagate
to qt_cpp (via libsumocpp rebuild) and ecal_deck (via `npm run generate`).

### Plan

1. **CMake**
   - `find_package(Protobuf REQUIRED)` for the runtime library only.
   - No local `protoc` invocation: `sumo_ecal.pb.h` comes from
     libsumocpp's build tree, exposed via `FindLibsumo.cmake`. The
     generated C++ symbols are exported by `libsumocpp.so`.
   - Link `qt_cpp` against `protobuf::libprotobuf` (for the parser
     code referenced from the generated header at link time).
2. **Cache lookup** (new file `src/sim/NetworkCache.{h,cpp}`)
   - `std::filesystem::path cachePathFor(const std::string& netFile)`
     mirrors Python's `_cache_path` (same `__ecaldeck__/<base>.net.v<N>.bin`
     layout so both halves share the same file).
   - `std::shared_ptr<NetworkGeometry> tryLoadCache(const std::string& sumocfg)`:
     parses sumocfg → resolves `net-file`, returns `nullptr` on stale /
     missing / version mismatch. On hit, deserialises the proto and
     converts to the existing in-memory `NetworkGeometry` struct
     **without copying** the typed-array `bytes` payloads (use
     `reinterpret_cast<const float*>(pb.lane_points().data())` and copy
     into the existing `std::vector<float>` since the struct outlives
     the parsed message — or refactor the struct to hold the parsed
     message and views over it for true zero-copy).
3. **SimWorker startup reflow**
   - `loadScenario` runs in two phases on the worker thread:
     - **Phase 1 (fast)**: `tryLoadCache(sumocfg)` → if hit, `emit networkReady(ng)` immediately. UI shows the network and "Loading simulation…" badge; play/pause/step disabled.
     - **Phase 2 (slow)**: `libsumo::Simulation::start(cmd)` runs as before. On success, if Phase 1 missed, build geometry via the existing libsumo path and `emit networkReady`. Either way, `emit scenarioLoaded` enables controls.
   - Cache-miss path stays identical to today.
4. **MainWindow / NetworkView UX**
   - New `simReady` signal from SimWorker (true after libsumo start).
   - Disable Play/Pause/Step toolbar actions until `simReady`.
   - Status-bar message "Loading simulation…" between Phase 1 and
     Phase 2 completion.
5. **Cache writer (optional, follow-up)**: a `--bake-network-cache`
   command-line flag on the Python publisher and/or a tiny standalone
   `bake_network_cache.py` so users can pre-warm caches without
   starting the full eCAL stack. Qt side stays read-only.

### Side benefits

- **Stop-line parity for free**: the `.pb` carries `lane_has_stopline`
  (per-connection link-state rule). Replaces the still-TODO porting of
  `_STOPLINE_LINK_STATES` from the publisher into `NetworkGeometry::build`.
- Same for crossings / walking-areas / lane permission classes /
  sidewalk distinction once that lands in the proto.
- One fewer place to fix geometry extraction bugs.

### Risks

- First-ever load of a scenario doesn't benefit (no cache yet). Could
  mitigate by warming the cache in the background after Phase 2.
- Vehicle picking / edge data don't work until libsumo is up — needs
  the "Loading simulation…" UI hint to set expectations.
- Field-layout coupling: any proto change must bump
  `NetworkGeometry::version` (already enforced) and both readers
  re-generated.

## Graphics API choice — Qt RHI migration

### Background

The initial implementation targets **OpenGL 3.3 core profile** via `QOpenGLWidget`
and `QOpenGLFunctions_3_3_Core`. This was the fastest path to get pixels on screen
with the existing Qt6 dependency, but OpenGL is deprecated on macOS (frozen at
4.1, no compute, no SSBOs, no `glVertexAttribLPointer`) and Apple will eventually
remove it. Long-term we need a portable, future-proof backend.

### Options considered

1. **Qt RHI** (Rendering Hardware Interface) — Qt's own thin abstraction over
   Vulkan / Metal / D3D12 / OpenGL, public API since Qt 6.6. This is what Qt
   Quick uses internally. **Chosen.**
2. **WebGPU via Dawn / wgpu-native** — same shader (WGSL) and API for native
   and browser; attractive if we ever want to unify the C++ renderer with the
   web frontend. Downsides today: extra heavy dep to vendor, Qt integration is
   DIY (render into a `QWindow`, present manually), still maturing on Linux.
3. **Raw Vulkan + MoltenVK on macOS** — maximum control but ~10× the boilerplate
   of our current code for the same 2D visuals. Not justified.

### Why Qt RHI wins for us

- **macOS** problem solved: the RHI backend auto-selects Metal there.
- **Lowest migration cost** from the current OpenGL code path. The
  `SimWorker → SimSnapshot → buffer.allocate()` data path is unchanged; only the
  buffer/pipeline/draw objects swap (`QRhiBuffer` for `QOpenGLBuffer`,
  `QRhiGraphicsPipeline` for the `glProgram`, `QRhiCommandBuffer` for the draw
  loop).
- **Shader story is clean**: write GLSL once, `qsb` cross-compiles to SPIR-V /
  MSL / HLSL / OpenGL GLSL at build time and packages them in a `.qsb` file the
  RHI loads at runtime.
- **No extra runtime deps** — RHI ships with Qt.

### Migration status — **COMPLETE (2026-05)**

All 12 layers run on RHI. The old `QOpenGLWidget`-based `NetworkView` and the
12 per-layer OpenGL classes have been removed from the tree.

What landed:

- **`NetworkView`** inherits `rhi_compat::RhiWidgetBase` (= `QRhiWidget` on
  Qt 6.7+, `RhiHostWidget` on Qt 6.4-6.6). Single `render(cb)` issues a single
  render pass with the layers in deck.gl order: junctions → lanes → edge-color
  overlay → ped → rails → polygons → POIs → stopping places → detectors → TLS
  → stop lines → vehicles → persons.
- **Two shared pipelines** in `NetworkView` cover the 8 static sub-layers:
  `m_passTris` (Triangles) and `m_passStrip` (TriangleStrip). Each sub-layer is
  a `StaticTrisRhi` (CPU-staged interleaved {x,y,r,g,b,a} vertex buffer) built
  by a free `build*Verts(NetworkGeometry&)` function in
  `layers/LayerBuilders.{h,cpp}`.
- **Dynamic layers** (Vehicle, Person, POI, TLS) each own their own pipeline
  (instanced quad/disc/bar variants) — they need per-instance attributes that
  don't fit the shared layout.
- **Edge-color overlay** uses the lane TriangleStrip with per-vertex rgba
  mutated in place each frame (`m_laneFirst`/`m_laneCount` index the strip);
  bridge vertices between adjacent lanes get α=0 so the discard shader culls
  them.
- **QPainter overlays** (scale bar, legend, pick info box) live on a
  transparent child `NetworkOverlayWidget` (sibling to the RHI surface) since
  QPainter-on-QRhiWidget is unverified on the Qt 6.4 backport.
- **Shaders** are GLSL 440 → `qsb` baked at build time via
  `qt6_add_shaders`. Five pairs: `tris_color`, `poi`, `tls`, `vehicle`,
  `person`.

Visual deltas from the OpenGL version:

- **Polygon outlines** are now world-space triangle strips of fixed half-width
  0.35 m (QRhi has no `glLineWidth`). At very low zoom outlines are still
  visible; at very high zoom they look slightly thicker than the GL line
  version did.
- **Polygon / junction fans** are CPU-expanded to triangle lists (no
  TRIANGLE_FAN topology in QRhi/Vulkan).
- **Multidraw paths** (StopLine, StoppingPlace, Detector, …) collapsed to
  single draws over stitched buffers.

Build prerequisites and toolchain selection are unchanged — see
"Build prerequisites" below.

### Migration plan (historical, layer by layer)

Originally planned as a per-layer incremental migration with both OpenGL and
RHI implementations coexisting. In practice the per-layer spike (VehicleLayer)
validated the pattern; the remaining 11 layers + `NetworkView` were then
migrated together. The OpenGL backend was deleted in one commit once the RHI
version reached visual parity on the doe scenario.

1. ✅ **VehicleLayerRhi** — instanced rotated quads. Validated end-to-end first
   via the standalone `qt_cpp_rhi_spike` driver against both Qt 6.4 and 6.8.
2. ✅ **PersonLayerRhi** — instanced quads.
3. ✅ **POILayerRhi** — instanced discs.
4. ✅ **TLSLayerRhi** — instanced oriented bar with state colour.
5. ✅ **Static sub-layers** (Network lanes, Network junctions, EdgeColor
   overlay, PedArea sidewalk+walk, Rail sleepers+rails, Polygon fills+outlines,
   StopLine, StoppingPlace, Detector) — all 11 routed through the shared
   `TrisColorInterleavedPass` + `StaticTrisRhi` via `LayerBuilders.cpp`.
6. ✅ `NetworkView` ported from `QOpenGLWidget` to `RhiWidgetBase`; QPainter
   overlays moved to `NetworkOverlayWidget`.
7. ✅ Old OpenGL layers + base class deleted.

### Precision caveat

QRhi has no `Double2` vertex-attribute format — none of the modern native APIs
(Metal, Vulkan, D3D12) accept double-precision vertex attributes. The zero-copy
GL trick of binding `veh_positions` as `GL_DOUBLE` with `vec2` shader input
(and letting the driver narrow) does **not** survive the migration. The RHI
layer narrows `veh_positions` to `float32` in `setSnapshot()`. For SUMO
scenarios with coordinates < ~1e6 m this preserves ~mm precision; geo-projected
scenarios with very large origins should subtract a per-scenario offset upstream.

### Build prerequisites

Two supported toolchains. The CMake build picks automatically based on what
`find_package(Qt6 6.4 ...)` resolves; the in-tree `RhiHostWidget` only
compiles in when `QRhiWidget` is missing.

**A. Stock Ubuntu noble (24.04) — apt-only, no aqtinstall:**

```bash
sudo apt install qt6-base-dev qt6-base-private-dev qt6-shadertools-dev \
                 libqt6opengl6t64 libxkbcommon-dev
```

This pulls Qt 6.4.2. We use the *private* QRhi headers
(`<QtGui/private/qrhi_p.h>`, linked via `Qt6::GuiPrivate`) and the in-tree
`rhi_compat::RhiHostWidget` — a ~150-line `QWidget` that wraps a `QWindow`,
owns the `QRhi`/`QRhiSwapChain`, and exposes the same
`initialize(cb)`/`render(cb)`/`releaseResources()` virtuals as
`QRhiWidget`. The compat shim `rhi_compat/rhi_compat.h` resolves the
include path; `rhi_compat/RhiWidgetBase.h` resolves the base class. No
source changes in `RhiSpikeWidget` between the two toolchains.

Tradeoffs: pure private headers (Qt makes no API stability promises across
patch releases), no D3D12 backend (only D3D11 on Windows), no built-in
`QRhiWidget`.

**B. Qt 6.7+ from aqtinstall / official installer:**

```bash
pip install --user aqtinstall
aqt install-qt linux desktop 6.8.3 linux_gcc_64 --outputdir ~/Qt \
    --modules qtshadertools
sudo apt install libxkbcommon-dev
cmake -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64 ...
```

Inherits directly from `QRhiWidget`, all backends available, API is
limited-compat public.

To force toolchain B's source to fall back to `RhiHostWidget` (useful when
sanity-checking the fallback path on a dev box without a second Qt install):

```bash
cmake -S qt_cpp -B qt_cpp/build -DCMAKE_CXX_FLAGS=-DRHI_COMPAT_FORCE_FALLBACK ...
```

**Common: `libxkbcommon-dev`** — Qt6Gui's `FindXKB.cmake` requires both the
library and headers. On Debian/Ubuntu: `sudo apt install libxkbcommon-dev`.

### Out of scope

- Migrating the eCAL/web rendering — `ecal_deck/frontend` keeps deck.gl
  (WebGL2/WebGPU is a deck.gl-side decision).
- Compute shaders or modern GPU-driven rendering; the workload is small enough
  that a straightforward instanced draw per layer is fine.
