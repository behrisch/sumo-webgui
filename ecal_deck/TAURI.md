# Tauri Integration

## Why Tauri

| | Tauri | Electron |
|--|-------|----------|
| License | MIT / Apache-2.0 | MIT |
| Binary size | ~10 MB (uses system webview) | ~150 MB (bundles Chromium) |
| Vite integration | First-class | Via electron-vite wrapper |
| Backend language | Rust | Node.js |
| IPC to Python processes | via Rust `std::process::Command` | via Node.js `child_process` |

Since our backend is already Python (publisher + bridge), we don't need Electron's Node.js integration. Tauri's Rust backend is lightweight and sufficient for launching subprocesses.

## License Summary (all dependencies)

| Package | License |
|---------|---------|
| Tauri | MIT / Apache-2.0 |
| deck.gl | MIT |
| MapLibre GL JS | BSD-3-Clause |
| React | MIT |
| Vite | MIT |
| shadcn/ui | MIT |
| eclipse-ecal | Apache-2.0 |
| protobuf (Python + npm) | BSD-3-Clause / Apache-2.0 |
| websockets (Python) | BSD-3-Clause |

No GPL or LGPL dependencies.

## Architecture with Tauri

```
Desktop mode                          Web mode
────────────────────────────────      ──────────────────────────────
Tauri shell                           Browser
  ├─ spawns sumo_ecal_publisher.py    User starts publisher + bridge
  ├─ spawns ecal_ws_bridge.py         manually (or via a script)
  └─ WebView → same Vite/React app ──────────────────────────────────▶
                                        deck.gl frontend
                                        ws://localhost:8765
```

The Vite/React frontend is **identical** in both modes. The only difference is how the backend processes are started.

## Tauri backend responsibilities (Rust)

- **Subprocess management**: spawn `sumo_ecal_publisher.py` and `ecal_ws_bridge.py` on startup, forward stdout/stderr to a log panel, kill them on app exit
- **File picker**: native OS file dialog to select `.sumocfg` files; result passed to the publisher subprocess as `--sumo-cfg`
- **Config persistence**: store last-used config path, WebSocket port, step length in app config dir
- **IPC commands** (called from the React frontend via `@tauri-apps/api`):

| Command | Description |
|---------|-------------|
| `start_simulation(cfg_path, step_length)` | spawn publisher + bridge |
| `stop_simulation()` | kill subprocesses |
| `pick_cfg_file()` | open native file dialog, return path |
| `get_status()` | return running/stopped state of subprocesses |

## Frontend changes for Tauri

Detect runtime with `@tauri-apps/api/core`:

```typescript
import { isTauri } from '@tauri-apps/api/core';

// show native file picker only in desktop mode
if (await isTauri()) {
  const path = await invoke('pick_cfg_file');
} else {
  // show a text input for the sumocfg path or hide it entirely
}
```

All deck.gl / WebSocket code is unchanged.

## Directory layout addition

```
ecal_deck/
  frontend/
    src-tauri/          ← Tauri Rust backend (added when integrating)
      src/
        main.rs
        commands.rs     ← IPC command handlers
      tauri.conf.json
      Cargo.toml
```

## When to add Tauri

Not in the initial implementation. Add it once the web frontend is working end-to-end. The migration is:
1. `npm create tauri-app` inside `frontend/` (or `cargo tauri init`)
2. Move existing Vite config / source — no changes needed
3. Implement the four IPC commands in `commands.rs`
4. Add `isTauri()` guards in the React app for the file picker and start/stop controls
