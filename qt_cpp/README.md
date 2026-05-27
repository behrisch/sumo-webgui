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

What works today:
- File → Open `.sumocfg` or `--sumocfg <path>` on the command line
- Worker thread loads scenario via libsumo, extracts lanes + junctions +
  widths, emits the network and an initial snapshot
- OpenGL 3.3 core rendering with MSAA 4x:
  - Junctions filled (dark grey)
  - Lanes CPU-extruded to per-lane-width triangle strips (slightly lighter)
  - Vehicles drawn via `glDrawArraysInstanced` as oriented 5×2 m quads,
    coloured by `VehicleType::getColor`
- Mouse pan (left drag), wheel zoom anchored at cursor, **Ctrl+0** reset
- Toolbar: Open, Play (Space), Pause, Step (S), Delay slider 0–1000 ms,
  Reset view
- Status bar: `step=… t=… s` + permanent FPS counter (500 ms window)
- Bottom-right semi-transparent scale bar (auto 1/2/5 × 10ᵏ m / km)

Next (not started): Phase 3 (rails with sleepers, TLS heads, persons,
polygons, detectors, stopping places, edge attribute coloring + legend).
