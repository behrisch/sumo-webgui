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

Locale fix: `main.cpp` resets `LC_NUMERIC=C` after Qt initialization so
libsumo's `std::stod`-based option/XML parsing works under comma-decimal
locales (e.g. de_DE).

What additionally works after Phase 3a:
- Static polygons from `.poly.xml`: filled (triangle fan about centroid)
  or unfilled (line strip), colored from libsumo
- Traffic-light heads: one small square per controlled link, position taken
  from the end of each `fromLane`, color updated per step from
  `TrafficLight::getRedYellowGreenState` (r/y/g/G/s/u/o handled)
- Persons: instanced small squares at `Person::getPosition`, colored by type

Render order matches the deck.gl frontend: junctions+roads → polygons →
TLS heads → vehicles → persons.

Next (not started): rails with sleepers, edge attribute coloring + legend,
crossings/walking-area styling, stop lines, stopping places, detectors,
picking/tooltips.
