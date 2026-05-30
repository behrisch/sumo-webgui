import type { GetAttributesResponse } from '../generated/sumo';
import type { VehicleSnapshot } from '../hooks/useSimSocket';
import type { PerfStats } from '../hooks/usePerfStats';
import type { VehicleShape } from '../layers/vehicleShapes';

export interface LayerVisibility {
  edges: boolean;
  junctions: boolean;
  vehicles: boolean;
  persons: boolean;
  containers: boolean;
  tls: boolean;
  edgeData: boolean;
  basemap: boolean;
  polygons: boolean;
  pois: boolean;
  stops: boolean;
  detectors: boolean;
}

interface Props {
  connected: boolean;
  paused: boolean;
  simReady: boolean;
  onPause: () => void;
  onResume: () => void;
  onStep: () => void;
  delayMs: number;
  onSetDelay: (ms: number) => void;
  snapshot: VehicleSnapshot | null;
  geoReferenced: boolean;
  basemapStyle: string;
  basemapStyles: string[];
  onBasemapStyle: (s: string) => void;
  visibility: LayerVisibility;
  onVisibility: (patch: Partial<LayerVisibility>) => void;
  vehicleColorAttr: string;
  vehicleKeys: string[];
  onVehicleColorAttr: (v: string) => void;
  vehicleShape: VehicleShape;
  vehicleShapes: VehicleShape[];
  onVehicleShape: (s: VehicleShape) => void;
  vehicleMinPixels: number;
  onVehicleMinPixels: (n: number) => void;
  edgeColorAttr: string;
  edgeKeys: string[];
  onEdgeColorAttr: (v: string) => void;
  attributeConfig: GetAttributesResponse | null;
  onSetAttributes: (vehicle: string[], edge: string[]) => void;
  autotune: boolean;
  intervalCurrent: number;
  onStepConfig: (autotune: boolean) => void;
  // 0 = publisher default (MAX_PUBLISH_FPS); >0 = cap to fps; <0 = uncapped
  maxPublishFps: number;
  onMaxPublishFps: (v: number) => void;
  perf: PerfStats;
  watchMs: number | null;      // null = not started; number = elapsed ms (ticking or frozen)
  watchRunning: boolean;       // true while ticking, false when frozen at sim end
  autostart: boolean;
  onAutostart: (v: boolean) => void;
  cfgPath: string;
  onBrowse: () => void;
  onReload: () => void;
}

const row: React.CSSProperties = { display: 'flex', alignItems: 'center', gap: 6 };
const btn: React.CSSProperties = {
  background: '#333', color: '#fff', border: '1px solid #666',
  borderRadius: 3, cursor: 'pointer', padding: '1px 7px', fontSize: 13,
};
const sel: React.CSSProperties = {
  background: '#222', color: '#fff', border: '1px solid #555', borderRadius: 3, flex: 1,
};

export function ControlPanel(p: Props) {
  const t  = p.snapshot != null ? (p.snapshot.time_ms / 1000).toFixed(1) : null;
  const nv = p.snapshot != null ? p.snapshot.veh_count : null;
  const np = p.snapshot != null ? p.snapshot.agent_count : null;

  const cfgName = p.cfgPath ? p.cfgPath.split('/').pop() : null;

  return (
    <div style={{
      position: 'absolute', top: 8, right: 8, padding: '8px 12px',
      background: 'rgba(0,0,0,0.65)', color: '#fff', fontFamily: 'monospace',
      fontSize: 12, borderRadius: 6, display: 'flex', flexDirection: 'column',
      gap: 6, minWidth: 220, userSelect: 'none',
    }}>

      {/* currently loaded config */}
      {cfgName && (
        <div title={p.cfgPath} style={{ opacity: 0.6, fontSize: 11, overflow: 'hidden',
          textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          {cfgName}
        </div>
      )}

      {/* transport controls */}
      <div style={row}>
        {p.paused
          ? <button style={{ ...btn, opacity: p.simReady ? 1 : 0.4 }} onClick={p.onResume} disabled={!p.simReady}>▶</button>
          : <button style={btn} onClick={p.onPause}>⏸</button>}
        {p.paused && <button style={{ ...btn, opacity: p.simReady ? 1 : 0.4 }} onClick={p.onStep} disabled={!p.simReady}>→</button>}
        <button style={btn} title="Load new simulation" onClick={p.onBrowse}>Load</button>
        <button style={{ ...btn, opacity: p.cfgPath ? 1 : 0.4 }} title="Reload current simulation"
          onClick={p.onReload} disabled={!p.cfgPath}>↺</button>
        <label style={{ ...row, cursor: 'pointer', marginLeft: 'auto' }} title="Start simulation automatically after load">
          <input type="checkbox" checked={p.autostart} onChange={(e) => p.onAutostart(e.target.checked)} />
          Auto
        </label>
        <span style={{ opacity: p.connected ? 1 : 0.5 }}>
          {p.connected
            ? (t != null ? `t=${t}s  ${nv}v  ${np}p` : 't=–  –v  –p')
            : '⚠ disconnected'}
        </span>
      </div>

      {/* speed slider */}
      <div style={row}>
        <span style={{ whiteSpace: 'nowrap' }}>Delay</span>
        <input type="range" min={0} max={2000} step={50} value={p.delayMs}
          onChange={(e) => p.onSetDelay(Number(e.target.value))}
          style={{ flex: 1 }} />
        <span style={{ minWidth: 38, textAlign: 'right' }}>{p.delayMs}ms</span>
      </div>

      <div style={{ borderTop: '1px solid #444' }} />

      {/* layer toggles */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 3 }}>
        {([
          ['edges',      'Edges'],
          ['junctions',  'Junctions'],
          ['vehicles',   'Vehicles'],
          ['persons',    'Persons'],
          ['containers', 'Containers'],
          ['tls',        'Signals & stop lines'],
          ...(p.edgeKeys.length ? [['edgeData', 'Edge data']] : []),
          ['polygons',   'Polygons'],
          ['pois',       'POIs'],
          ['stops',      'Stops'],
          ['detectors',  'Detectors'],
        ] as [keyof LayerVisibility, string][]).map(([key, label]) => (
          <label key={key} style={{ ...row, cursor: 'pointer' }}>
            <input type="checkbox" checked={p.visibility[key]}
              onChange={(e) => p.onVisibility({ [key]: e.target.checked })} />
            {label}
          </label>
        ))}
        {p.geoReferenced && (
          <div style={row}>
            <input type="checkbox" checked={p.visibility.basemap}
              onChange={(e) => p.onVisibility({ basemap: e.target.checked })} />
            <span>Basemap</span>
            <select value={p.basemapStyle} onChange={(e) => p.onBasemapStyle(e.target.value)}
              style={{ ...sel, flex: 'none' }}>
              {p.basemapStyles.map((s) => <option key={s} value={s}>{s}</option>)}
            </select>
          </div>
        )}
      </div>

      <div style={{ borderTop: '1px solid #444' }} />

      {/* color + shape selectors */}
      <div style={row}>
        <span style={{ whiteSpace: 'nowrap' }}>Vehicle color</span>
        <select value={p.vehicleColorAttr} onChange={(e) => p.onVehicleColorAttr(e.target.value)} style={sel}>
          <option value="speed">speed</option>
          {p.vehicleKeys.map((k) => <option key={k} value={k}>{k}</option>)}
        </select>
      </div>
      <div style={row}>
        <span style={{ whiteSpace: 'nowrap' }}>Vehicle shape</span>
        <select value={p.vehicleShape} onChange={(e) => p.onVehicleShape(e.target.value as VehicleShape)} style={sel}>
          {p.vehicleShapes.map((s) => <option key={s} value={s}>{s}</option>)}
        </select>
      </div>
      <div style={row}>
        <span style={{ whiteSpace: 'nowrap' }}>Min size (px)</span>
        <input type="number" min={1} max={50} value={p.vehicleMinPixels}
          onChange={(e) => p.onVehicleMinPixels(Math.max(1, Number(e.target.value)))}
          style={{ width: 44, background: '#111', color: '#fff', border: '1px solid #555', borderRadius: 3, padding: '1px 4px' }} />
      </div>
      {p.edgeKeys.length > 0 && (
        <div style={row}>
          <span style={{ whiteSpace: 'nowrap' }}>Edge data</span>
          <select value={p.edgeColorAttr} onChange={(e) => p.onEdgeColorAttr(e.target.value)} style={sel}>
            <option value="">none</option>
            {p.edgeKeys.map((k) => <option key={k} value={k}>{k}</option>)}
          </select>
        </div>
      )}

      {/* step interval config */}
      <div style={{ borderTop: '1px solid #444' }} />
      <div style={row}>
        <label style={{ cursor: 'pointer', ...row }}>
          <input type="checkbox" checked={p.autotune}
            onChange={(e) => p.onStepConfig(e.target.checked)} />
          Auto interval
        </label>
        <span style={{ marginLeft: 'auto', opacity: 0.7, fontSize: 11 }}>
          now: {p.intervalCurrent}
        </span>
      </div>
      <div style={row}>
        <span style={{ whiteSpace: 'nowrap' }}>Max fps</span>
        <select
          value={String(p.maxPublishFps)}
          onChange={(e) => p.onMaxPublishFps(Number(e.target.value))}
          style={sel}
          title="Cap the publish rate (fps). Mirrors qt_cpp's Max-fps cap so the perf comparison can pin both stacks to the same frame budget."
        >
          <option value="0">default (20)</option>
          <option value="-1">uncapped</option>
          <option value="5">5</option>
          <option value="10">10</option>
          <option value="20">20</option>
          <option value="30">30</option>
          <option value="60">60</option>
          <option value="120">120</option>
        </select>
      </div>

      {/* perf stats (enable with ?perf=1 in the URL) */}
      {typeof window !== 'undefined' && new URLSearchParams(window.location.search).has('perf') && (
        <div style={{ borderTop: '1px solid #444', paddingTop: 4, opacity: 0.6, fontSize: 11, lineHeight: 1.6 }}>
          <div>msg/s {p.perf.msgPerSec}  frame {p.perf.frameMs.toFixed(1)}ms</div>
          <div>parse {p.perf.parseMs.toFixed(2)}ms  veh-build {p.perf.vehicleBuildMs.toFixed(2)}ms</div>
          {p.perf.skipRate > 0 && <div>skip {(p.perf.skipRate * 100).toFixed(0)}%</div>}
        </div>
      )}

      {p.watchMs !== null && (
        <div style={{ borderTop: '1px solid #444', paddingTop: 4, opacity: p.watchRunning ? 1 : 0.5, fontSize: 11, lineHeight: 1.6 }}>
          ⏱ {p.watchMs >= 3600000
            ? new Date(p.watchMs).toISOString().slice(11, 19)
            : (p.watchMs / 1000).toFixed(1) + ' s'}
          {!p.watchRunning && ' (done)'}
        </div>
      )}

      {p.attributeConfig && (
        <>
          <div style={{ borderTop: '1px solid #444' }} />
          <AttributeSelector
            label="Vehicle attrs"
            available={p.attributeConfig.vehicle_available}
            enabled={p.attributeConfig.vehicle_enabled}
            onChange={(sel) => p.onSetAttributes(sel, p.attributeConfig!.edge_enabled ?? [])}
          />
          <AttributeSelector
            label="Edge attrs"
            available={p.attributeConfig.edge_available}
            enabled={p.attributeConfig.edge_enabled}
            onChange={(sel) => p.onSetAttributes(p.attributeConfig!.vehicle_enabled ?? [], sel)}
          />
        </>
      )}
    </div>
  );
}

function AttributeSelector({ label, available = [], enabled = [], onChange }: {
  label: string;
  available?: string[];
  enabled?: string[];
  onChange: (selected: string[]) => void;
}) {
  const toggle = (key: string, checked: boolean) => {
    const next = checked ? [...enabled, key] : enabled.filter((k) => k !== key);
    onChange(next);
  };
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
      <span style={{ opacity: 0.6, fontSize: 11 }}>{label}</span>
      {available.map((k) => (
        <label key={k} style={{ display: 'flex', alignItems: 'center', gap: 5, cursor: 'pointer' }}>
          <input type="checkbox" checked={enabled.includes(k)} onChange={(e) => toggle(k, e.target.checked)} />
          {k}
        </label>
      ))}
    </div>
  );
}
