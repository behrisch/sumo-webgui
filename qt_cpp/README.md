# qt_cpp

Native Qt6 + OpenGL desktop reimplementation of the `ecal_deck` browser GUI.
Couples directly to libsumo in-process (no eCAL, no WebSocket, no protobuf).

See `PLAN.md` for the full design and phasing.

## Build (Linux)

Requires:
- **Qt 6.4+** (Ubuntu noble's `qt6-base-dev` works; we transparently use
  the in-tree `RhiHostWidget` shim on Qt < 6.7 and `QRhiWidget` on Qt ≥
  6.7). Components: Widgets, OpenGLWidgets, ShaderTools.
  - On Qt < 6.6 also install `qt6-base-private-dev` (provides
    `<QtGui/private/qrhi_p.h>`).
- CMake ≥ 3.20, a C++20 compiler
- A built SUMO source tree containing `libsumocpp.so`
  (e.g. `/home/ubuntu/sumo/bin/`)
- `libxkbcommon-dev` (Qt6Gui's FindXKB hard-requires the headers).

```bash
# Apt-only path (Ubuntu noble, system Qt 6.4):
sudo apt install qt6-base-dev qt6-base-private-dev qt6-shadertools-dev \
                 libxkbcommon-dev
cmake -S qt_cpp -B qt_cpp/build -DSUMO_HOME=/home/ubuntu/sumo
cmake --build qt_cpp/build -j

# Or with a newer Qt from aqtinstall:
cmake -S qt_cpp -B qt_cpp/build \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64 \
      -DSUMO_HOME=/home/ubuntu/sumo
cmake --build qt_cpp/build -j
```

Two executables are produced:

- `qt_cpp` — the full OpenGL GUI (production path, all layers).
- `qt_cpp_rhi_spike` — standalone QRhi feasibility driver that renders only
  the vehicle layer via `VehicleLayerRhi`. Hosted on `QRhiWidget` (Qt 6.7+)
  or `rhi_compat::RhiHostWidget` (Qt 6.4–6.6). Validates the Qt RHI
  migration path described in `PLAN.md`.

## Run

```bash
./qt_cpp/build/qt_cpp --sumocfg ../doe/view.sumocfg
```

Or start it with no scenario and use **File → Open .sumocfg…**.

## Status

Phase 0 (skeleton) — done.
Phase 1 (network rendering + camera + scale bar + reset view) — done.
Phase 2 (live simulation, vehicle rendering, play/pause/step/delay/FPS) — done.
Phase 3a (polygons + persons + traffic-light heads) — done.
Phase 3c (round TLS heads, stopping places, induction-loop / lane-area
detectors) — done.
Phase 4 (POIs, MultiEntryExit detectors, sidewalk/walking-area tinting,
follow-vehicle camera mode) — done.

Locale fix: `main.cpp` resets `LC_NUMERIC=C` after Qt initialization so
libsumo's `std::stod`-based option/XML parsing works under comma-decimal
locales (e.g. de_DE).

What additionally works after Phase 3b:
- Per-lane classification (road / rail / sidewalk / walkingarea·crossing /
  internal) derived from `Lane::getAllowed`
- **Rails**: dedicated `RailLayer` draws two steel-coloured rails offset by
  half the standard gauge (1.435 m) plus periodic perpendicular sleepers
  (every 3.5 m) for every rail lane
- **Stop lines**: short white perpendicular bars at the end of each
  TLS-controlled approach lane, oriented from the lane's last segment
- **Edge attribute coloring**: toolbar "Color by" dropdown with
  *None / Mean speed / Occupancy / Halting count*. Per-lane colours are
  computed on the worker thread (only when a mode is active, to keep step
  cost low on 9k-lane networks) and re-uploaded as a per-vertex colour
  attribute via `EdgeColorLayer`. A bottom-left legend overlay shows the
  attribute name with a red→yellow→green gradient.
- **Picking + info box**: left-click on the map selects the nearest
  vehicle / person / TLS head / polygon / lane / junction (in that order)
  within a ~8 pixel tolerance and shows a floating info box at the top-left
  with the id, position/heading/state/etc. A short drag still pans without
  selecting; only static clicks pick.

Render order matches the deck.gl frontend: junctions+roads → edge-attribute
overlay → pedestrian areas → rails → polygons → POIs → stopping places →
detectors → stop lines → TLS heads → vehicles → persons.

What additionally works after Phase 4:
- **POIs** rendered as small coloured discs (`POILayer`), picked with their
  id, type, and position
- **MultiEntryExit detectors**: entries shown as green bars, exits as red
  bars (`DetectorLayer` kinds 2/3); also covered by picking
- **Pedestrian areas**: sidewalks drawn in warm tan and walking-areas /
  crossings in light grey on top of the road base (`PedAreaLayer`) so they
  visually separate from car lanes
- **Follow-vehicle mode**: pick a vehicle, then `Follow selected` (Ctrl+F)
  locks the camera onto it for every subsequent snapshot. `Unfollow` (Esc)
  clears the lock; following also auto-clears when the vehicle leaves.

Next (not started): zebra striping on crossings, vehicle shape by type
(bus/truck/etc.), OverheadWire and Calibrator markers.
