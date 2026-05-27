# qt_cpp

Native Qt6 + OpenGL desktop reimplementation of the `ecal_deck` browser GUI.
Couples directly to libsumo in-process (no eCAL, no WebSocket, no protobuf).

See `PLAN.md` for the full design and phasing.

## Build (Linux)

Requires:
- Qt6 (`qt6-base-dev`, `libqt6openglwidgets6t64`, headers shipped via
  `qt6-base-dev-tools` on Ubuntu 24.04)
- CMake ≥ 3.20, a C++20 compiler
- A built SUMO source tree containing `libsumocpp.so`
  (e.g. `/home/ubuntu/sumo/cmclaude/src/fmi/sumo-fmi2/binaries/linux64/`)

```bash
cmake -S qt_cpp -B qt_cpp/build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DSUMO_HOME=/home/ubuntu/sumo
cmake --build qt_cpp/build -j
```

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
Phase 3b (lane classification + rails with sleepers, stop lines,
edge-attribute coloring + legend, click-to-inspect info box) — done.

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
overlay → rails → polygons → stop lines → TLS heads → vehicles → persons.

Next (not started): stopping places (BusStop/ChargingStation/ParkingArea),
detectors (InductionLoop/LaneArea/MultiEntryExit), crossings styling beyond
the base tint, mode toggle to lock/follow a vehicle.
