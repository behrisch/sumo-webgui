# qt_cpp — Qt6 + OpenGL SUMO GUI (C++)

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
- **Qt6 Widgets + QOpenGLWidget** with modern OpenGL (3.3 core profile) for
  rendering. No QtQuick/QML — keep dependencies minimal and rendering paths
  explicit, mirroring how deck.gl layers are structured.
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
|  NetworkView : QOpenGLWidget                         |
|    - paintGL() draws layers using cached GPU buffers |
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
    NetworkView.{h,cpp}             # QOpenGLWidget host, Camera, picking
    Camera.{h,cpp}                  # pan/zoom/reset, view+proj matrices,
                                    # geo<->screen helpers (MapView equiv)
    overlays/
      ScaleBarOverlay.{h,cpp}       # QPainter overlay = ScaleBar.tsx
      LegendOverlay.{h,cpp}         # colormap legend for edge data
      LogPanel.{h,cpp}              # QDockWidget with log messages
    sim/
      SimWorker.{h,cpp}             # QThread owning libsumo
      SimSnapshot.{h,cpp}           # double-buffered state struct
      NetworkGeometry.{h,cpp}       # built once from libsumo network
      EdgeColorizer.{h,cpp}         # vehicle/edge attribute -> color
    layers/                         # one class per deck.gl layer
      Layer.h                       # abstract: initGL / upload / draw
      NetworkLayer.{h,cpp}          # roads, rails (with sleepers),
                                    # walking areas, crossings
      EdgeDataLayer.{h,cpp}         # live attribute coloring
      TLSLayer.{h,cpp}              # traffic light heads
      StopLineLayer.{h,cpp}
      StoppingPlaceLayer.{h,cpp}    # bus stops etc.
      PolygonLayer.{h,cpp}          # buildings, etc.
      DetectorLayer.{h,cpp}
      VehicleLayer.{h,cpp}          # instanced triangles per vehicle class
      PersonLayer.{h,cpp}
    gl/
      Shader.{h,cpp}                # GLSL load/compile/link helpers
      Buffer.{h,cpp}                # VBO/VAO RAII wrappers
      shaders/                      # *.vert / *.frag
  resources/
    qt_cpp.qrc                      # shaders, icons
    icons/...
  third_party/                      # only headers / submodules if needed
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

Per-step data needed by the GUI (mirroring `_build_and_publish_simstep`):

- `Simulation::getTime()`
- `Vehicle::getAllSubscriptionResults()` (after subscribing once to position,
  angle, speed, type, vClass, color, signals, waiting time, lane, etc.)
- `Person::getAllSubscriptionResults()` (position, angle, type, vClass)
- `TrafficLight::getAllSubscriptionResults()` (state string)
- `Edge::getLastStepVehicleNumber` / `getLastStepMeanSpeed` /
  `getTraveltime` / `getCO2Emission` / etc. for the currently-selected edge
  attribute (only fetched when an attribute is active, like the publisher does).

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

- **OpenGL 3.3 core**. One `QOpenGLWidget` for the map; per-layer `QOpenGLBuffer`
  / `QOpenGLVertexArrayObject` resources owned by each `Layer` subclass.
- Camera in either:
  - geo mode → equirectangular projection of lon/lat with a Mercator scale
    correction per latitude, identical formula to `MapView` defaults; or
  - non-geo mode → straight orthographic in SUMO XY (meters).
- Layer render order (must match `ecal_deck`):
  junctions → road network → rails → edge-data coloring →
  stop lines → stopping places → polygons → detectors → TLS heads →
  vehicles/persons/containers.
- Vehicles drawn with **instanced rendering** (`glDrawArraysInstanced`): one
  per-vClass triangle/quad mesh as the base geometry, one instance buffer with
  `[x, y, cos_angle, sin_angle, r, g, b, a, scale]` per vehicle. Buffer is
  re-uploaded once per SimSnapshot swap, mirroring deck.gl's `ScatterplotLayer`
  attribute updates.
- Picking via off-screen FBO with per-instance ID color, read back on click.
- Optional MSAA 4x in the framebuffer format.

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
- CMake project, `MainWindow` with empty `QOpenGLWidget`, clears to dark grey.
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

### Phase 3 — Full layer parity (≈ 3 days)
- Persons, TLS heads, stop lines, stopping places, polygons, detectors.
- Rails with sleepers (port logic from `NetworkLayer.ts` carefully — see
  checkpoint 017 for the inverted-tram trick and sleeper sizing).
- Edge attribute selector + `EdgeDataLayer` with viewport culling and color
  scale legend.

### Phase 4 — Polish & parity QA (≈ 2 days)
- Picking / vehicle tooltip.
- Log panel.
- Side-by-side screenshot diff vs `ecal_deck` on `doe/view.sumocfg`.
- Benchmark: extend `ecal_deck/BENCHMARKING.md` (or add a sibling
  `qt_cpp/BENCHMARKING.md`) with FPS / CPU / RSS numbers for the same scenario,
  same step delay, same window size.

### Phase 5 — Optional extras
- MSAA toggle.
- Basemap tiles in geo mode.
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

### Migration plan (layer by layer)

We migrate layer-by-layer rather than in one big bang. Each layer becomes a
`QRhi`-based renderer using its own pipeline + per-instance buffers. The
`SimSnapshot` byte columns we just landed (zero-copy from libsumo Batch) are the
exact same shape RHI wants for `QRhiBuffer::uploadStaticBuffer`.

Order:

1. **VehicleLayer** — first migration, proves the pattern (instanced 2D quads,
   per-instance position + angle + rgba). **Validated end-to-end** as
   `src/layers/VehicleLayerRhi.{h,cpp}` plus a standalone driver
   `qt_cpp_rhi_spike` that hosts it on `rhi_compat::RhiWidgetBase`. Runs against
   both system Qt 6.4 (via `RhiHostWidget` shim, OpenGLES2 backend) and aqt 6.8
   (via native `QRhiWidget`). Shaders live in `src/shaders/vehicle.{vert,frag}`
   and are baked to `.qsb` files via `qt6_add_shaders` at build time. Confirmed
   visually with the doe scenario: vehicles render at the correct world
   positions, pan and zoom respond, snapshot uploads happen on the render
   thread, swapchain resizes properly. Note: vehicle quads are 5 m × 2 m, so
   at a scenario-fit zoom (ppu ≈ 0.2) they are sub-pixel — this is expected and
   matches the OpenGL `VehicleLayer` behaviour.
2. **PersonLayer** — same pattern, no rotation.
3. **TLSLayer** — same pattern, per-instance state byte.
4. **EdgeNetworkLayer** / **EdgeDataLayer** — line/triangle batches.
5. **JunctionLayer**, **PolygonLayer**, **PoiLayer**, **StopLineLayer** —
   straightforward once the pipeline scaffolding exists.
6. Switch `NetworkView` from `QOpenGLWidget` to `QRhiWidget` (Qt 6.7+) once all
   layers are ported, then drop the OpenGL-specific code.

During the transition the OpenGL backend is kept compiling: each layer gets a
sibling `*Rhi.cpp` and `NetworkView` chooses the implementation based on a
build flag. Once all layers are migrated and validated visually, the OpenGL
implementations are deleted.

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
