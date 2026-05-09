# SUMO deck.gl Frontend via eCAL

Real-time SUMO simulation visualizer using deck.gl and MapLibre, communicating via Eclipse eCAL.

## Prerequisites

| Dependency | Version | Linux | macOS | Windows |
|------------|---------|-------|-------|---------|
| SUMO | built from source | — | — | — |
| Python | 3.12 | system / pyenv | `brew install python@3.12` | python.org installer |
| eclipse-ecal | any | `pip install eclipse-ecal` | `pip install eclipse-ecal` | `pip install eclipse-ecal` |
| websockets | 16+ | `pip install websockets` | `pip install websockets` | `pip install websockets` |
| protobuf | **6.x** | `pip install "protobuf<7"` | `pip install "protobuf<7"` | `pip install "protobuf<7"` |
| libsumo | same as SUMO | `pip install libsumo` *(optional)* | `pip install libsumo` *(optional)* | `pip install libsumo` *(optional)* |
| protoc | 33.x | `apt install protobuf-compiler` | `brew install protobuf@6` | `winget install Google.Protobuf --version 33.4` |
| Node.js | 18+ | `apt install nodejs` | `brew install node` | `winget install OpenJS.NodeJS` |
| npm | 9+ | `apt install npm` | bundled with Node | bundled with Node |

`$SUMO_HOME` must point to the SUMO repo root on all platforms.

`libsumo` is optional but strongly recommended — it runs the simulation in-process (no socket
overhead) and is significantly faster than the TraCI fallback. If `libsumo` is not available
the publisher falls back to TraCI automatically. When SUMO is built from source, `libsumo` may
already be available on the Python path via `$SUMO_HOME/tools`; `pip install libsumo` installs
a pre-built standalone version.

## Platform notes

### macOS

The setup is nearly identical to Linux. One difference:

**Python environment:** `tests/sumo_test_env/` was created on Linux and will not work on macOS.
Create your own:
```bash
python3.12 -m venv ecal_deck/.venv
source ecal_deck/.venv/bin/activate
pip install eclipse-ecal websockets "protobuf<7" libsumo
```
Then update `PYTHON` in `run.sh` to `ecal_deck/.venv/bin/python`.

### Windows

**Activate the Python environment (Command Prompt):**
```bat
tests\sumo_test_env\Scripts\activate.bat
```
or PowerShell:
```powershell
tests\sumo_test_env\Scripts\Activate.ps1
```
As with macOS, `tests/sumo_test_env/` is Linux-specific. Create your own:
```powershell
python -m venv ecal_deck\.venv
ecal_deck\.venv\Scripts\Activate.ps1
pip install eclipse-ecal websockets "protobuf<7" libsumo
```

**Set SUMO_HOME (Command Prompt):**
```bat
set SUMO_HOME=%CD%
```
PowerShell:
```powershell
$env:SUMO_HOME = (Get-Location).Path
```

**protoc version — important:** eclipse-ecal requires the protobuf 6.x Python runtime
(`pip install "protobuf<7"`). The generated code must match that runtime.
winget uses a cross-platform version scheme: protoc **33.x** maps to the Python 6.x runtime,
while protoc 34.x maps to Python 7.x and is incompatible. Install the correct version with:
```powershell
winget install Google.Protobuf --version 33.4
```
(replace `33.4` with the highest `33.x` version listed by `winget show Google.Protobuf --versions`)

**`run.sh` does not run natively on Windows.** Use one of:
- **Git Bash** — run `./run.sh` from the Git Bash terminal (recommended)
- **WSL** — run the full workflow inside WSL
- **Manual** — open three separate terminals and start bridge, publisher, and frontend dev server individually (see [Running](#running) below)

> **Note:** eCAL on macOS and Windows is supported but less tested than Linux.
> If you encounter issues, check the [Eclipse eCAL documentation](https://eclipse-ecal.github.io/ecal/).

## One-time setup

All commands are run from the SUMO repo root (`$SUMO_HOME`).

**1. Activate the Python environment and set SUMO_HOME:**

Linux / macOS:
```bash
source tests/sumo_test_env/bin/activate   # or ecal_deck/.venv/bin/activate on macOS
export SUMO_HOME=$PWD
```
Windows (PowerShell):
```powershell
ecal_deck\.venv\Scripts\Activate.ps1
$env:SUMO_HOME = (Get-Location).Path
```

**2. Generate Python protobuf bindings:**
```bash
protoc -I ecal_deck/proto --python_out=ecal_deck/proto ecal_deck/proto/sumo.proto
```

**3. Install frontend dependencies and generate TypeScript types:**
```bash
cd ecal_deck/frontend
npm install       # installs deck.gl, MapLibre, React, Vite, ts-proto, …
npm run generate  # generates src/generated/sumo.ts from proto/sumo.proto (via generate.ts + tsx)
cd ../..
```

> `npm run generate` is also called automatically by `npm run dev` and `npm run build`,
> so after the initial run you only need to re-run it manually if you change `sumo.proto`
> without starting the dev server.

## Running

Three processes are required, each in its own terminal. All assume you have activated the
Python environment and set `SUMO_HOME` as above.

**Terminal 1 — Publisher** (starts SUMO and publishes to eCAL):
```bash
python ecal_deck/sumo_ecal_publisher.py --sumo-cfg path/to/sim.sumocfg [--delay 200]
```
The publisher waits until the bridge subscribes before sending the network message,
so you can start the terminals in any order.

Common options:
```
--step-length 0.1          simulation step in seconds (default 1.0)
--delay 200                ms to sleep between steps (default 0 = as fast as possible)
--edgedata-interval 5      publish edge data every 5 steps (default 1)
--edgedata-occupied-only   skip edges with no vehicles
```
Vehicle and edge attributes to collect are configured via the frontend GUI at runtime.

**Terminal 2 — Bridge** (relays eCAL → WebSocket):
```bash
python ecal_deck/ecal_ws_bridge.py [--ws-port 8765]
```

**Terminal 3 — Frontend** (dev server):
```bash
cd ecal_deck/frontend && npm run dev
```
Open http://localhost:5173 in a browser.

## Re-generating after proto changes

If you modify `proto/sumo.proto`:
```bash
# Python bindings
protoc -I ecal_deck/proto --python_out=ecal_deck/proto ecal_deck/proto/sumo.proto

# TypeScript types (or just run npm run dev / npm run build)
cd ecal_deck/frontend && npm run generate
```

## Performance benchmarking

The frontend includes a live stats panel in the control panel (bottom section) that updates
every second. Use it to spot bottlenecks before reaching for a profiler.

### Live stats panel

| Metric | What it means | Healthy value |
|--------|--------------|---------------|
| **msg/s** | WebSocket batches received per second | ~60 (bridge rate-limits to 60 fps) |
| **frame ms** | Average time between `requestAnimationFrame` callbacks | ~16 ms (60 fps) |
| **parse ms** | Average `JSON.parse` time per batch message | < 5 ms |
| **veh-build ms** | Average `buildVehicleLayer` time | < 2 ms |

If `frame ms` is much larger than 16 ms, the JS main thread is the bottleneck. If `msg/s` is
much larger than 60, the bridge rate limiting is not working. If `parse ms` is large, the JSON
payload is too big and protobuf binary transport (Phase B) is the next step.

### Firefox Performance tab

For deeper analysis:

1. Open DevTools (F12) → Performance tab
2. Click the stopwatch icon to start recording, let the simulation run for 5-10 seconds, stop
3. Look for:
   - **Orange (JavaScript) blocks > 16 ms** in the waterfall — JS is the bottleneck
   - **GC events** (purple "GC" markers in the timeline) — excessive object allocation; typed
     arrays eliminate this for vehicle and edge data layers
   - **Call tree / Flame chart**: search for `buildVehicleLayer`, `buildEdgeDataLayer`,
     `JSON.parse` to see where time goes

The custom `performance.mark()`/`performance.measure()` calls in `useSimSocket.ts` and
`VehicleLayer.ts` appear as **markers** in the timeline, making it easy to isolate parsing
and layer-build time from React and deck.gl overhead. They are also visible in the
**Marker chart** track at the top of the waterfall.

### React DevTools Profiler

Install the React DevTools browser extension, then use the Profiler tab (inside DevTools →
Components → Profiler) to record a render sequence. This shows which components re-render and
for how long. With the batch message approach, you should see one render per 60 fps cycle
rather than three.

### Known bottlenecks and mitigations

| Symptom | Cause | Mitigation |
|---------|-------|-----------|
| `frame ms` >> 16 ms | Too many renders per frame | Bridge batches all pending topics into one WS frame per 60 fps cycle |
| `parse ms` > 5 ms | Large edge data JSON payload | Increase `--edgedata-interval`; enable `--edgedata-occupied-only`; or implement protobuf binary transport (Phase B) |
| GC spikes in profiler | Object allocation per frame | Vehicles and edges use typed arrays (`Float64Array`, `Uint8Array`) -- no per-frame GC pressure |
| `msg/s` >> 60 | Bridge rate limiting not working | Check that `_LATENCY_SENSITIVE` topics route to `_pending` dict, not to `_reliable_send` |
| High `veh-build ms` with many vehicles | Binary typed array loop too slow | Profile the loop; consider WebWorker offload for very large fleets |

### Phase B: protobuf binary transport

If `parse ms` remains high after reducing edge data frequency, the next optimization is to
send `simstep` and `edgedata` as raw protobuf bytes over binary WebSocket frames instead of
JSON. The bridge would skip `MessageToDict` for these topics, and the frontend would use
`ts-proto` encode/decode (requires enabling `onlyTypes=false` in the generate script).

This is expected to reduce payload size by 30-50 % and eliminate JSON parsing overhead
entirely for the two highest-frequency messages.

## Architecture

See [PLAN.md](PLAN.md) for full architecture documentation and [TAURI.md](TAURI.md) for
desktop packaging notes.
