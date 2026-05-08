import type { SimStep, GetAttributesResponse } from '../generated/sumo';
import type { PerfStats } from '../hooks/usePerfStats';

export interface LayerVisibility {
  edges: boolean;
  junctions: boolean;
  vehicles: boolean;
  tls: boolean;
  edgeData: boolean;
  basemap: boolean;
}

interface Props {
  connected: boolean;
  paused: boolean;
  onPause: () => void;
  onResume: () => void;
  onStep: () => void;
  delayMs: number;
  onSetDelay: (ms: number) => void;
  simStep: SimStep | null;
  geoReferenced: boolean;
  basemapStyle: string;
  basemapStyles: string[];
  onBasemapStyle: (s: string) => void;
  visibility: LayerVisibility;
  onVisibility: (patch: Partial<LayerVisibility>) => void;
  vehicleColorAttr: string;
  vehicleKeys: string[];
  onVehicleColorAttr: (v: string) => void;
  edgeColorAttr: string;
  edgeKeys: string[];
  onEdgeColorAttr: (v: string) => void;
  attributeConfig: GetAttributesResponse | null;
  onSetAttributes: (vehicle: string[], edge: string[]) => void;
  intervalMin: number;
  intervalMax: number;
  autotune: boolean;
  intervalCurrent: number;
  atMinBound: boolean;
  atMaxBound: boolean;
  onStepConfig: (min: number, max: number, autotune: boolean) => void;
  perf: PerfStats;
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
  const t = ((p.simStep?.time_ms ?? 0) / 1000).toFixed(1);
  const n = p.simStep?.vehicles?.length ?? 0;

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
          ? <button style={btn} onClick={p.onResume}>▶</button>
          : <button style={btn} onClick={p.onPause}>⏸</button>}
        {p.paused && <button style={btn} onClick={p.onStep}>→</button>}
        <button style={btn} title="Load new simulation" onClick={p.onBrowse}>Load</button>
        <button style={{ ...btn, opacity: p.cfgPath ? 1 : 0.4 }} title="Reload current simulation"
          onClick={p.onReload} disabled={!p.cfgPath}>↺</button>
        <span style={{ opacity: p.connected ? 1 : 0.5, flex: 1 }}>
          {p.connected ? `t=${t}s  ${n}v` : '⚠ disconnected'}
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
          ['edges',     'Edges'],
          ['junctions', 'Junctions'],
          ['vehicles',  'Vehicles'],
          ['tls',       'Traffic lights'],
          ...(p.edgeKeys.length   ? [['edgeData', 'Edge data']] : []),
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

      {/* color selectors */}
      <div style={row}>
        <span style={{ whiteSpace: 'nowrap' }}>Vehicle color</span>
        <select value={p.vehicleColorAttr} onChange={(e) => p.onVehicleColorAttr(e.target.value)} style={sel}>
          <option value="speed">speed</option>
          {p.vehicleKeys.map((k) => <option key={k} value={k}>{k}</option>)}
        </select>
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
      <div style={{ display: 'flex', flexDirection: 'column', gap: 3 }}>
        <div style={row}>
          <label style={{ cursor: 'pointer', ...row }}>
            <input type="checkbox" checked={p.autotune}
              onChange={(e) => p.onStepConfig(p.intervalMin, p.intervalMax, e.target.checked)} />
            Auto interval
          </label>
          <span style={{ marginLeft: 'auto', opacity: 0.7, fontSize: 11 }}>
            now: {p.intervalCurrent}
            {p.atMinBound && ' ▼'}
            {p.atMaxBound && ' ▲'}
          </span>
        </div>
        <div style={row}>
          <span style={{ whiteSpace: 'nowrap', fontSize: 11 }}>min</span>
          <input type="number" min={1} max={p.intervalMax} value={p.intervalMin}
            onChange={(e) => p.onStepConfig(Number(e.target.value), p.intervalMax, p.autotune)}
            style={{ width: 44, background: '#111', color: '#fff', border: '1px solid #555', borderRadius: 3, padding: '1px 4px' }} />
          <span style={{ whiteSpace: 'nowrap', fontSize: 11 }}>max</span>
          <input type="number" min={p.intervalMin} max={100} value={p.intervalMax}
            onChange={(e) => p.onStepConfig(p.intervalMin, Number(e.target.value), p.autotune)}
            style={{ width: 44, background: '#111', color: '#fff', border: '1px solid #555', borderRadius: 3, padding: '1px 4px' }} />
        </div>
      </div>

      {/* perf stats */}
      <div style={{ borderTop: '1px solid #444', paddingTop: 4, opacity: 0.6, fontSize: 11, lineHeight: 1.6 }}>
        <div>msg/s {p.perf.msgPerSec}  frame {p.perf.frameMs.toFixed(1)}ms</div>
        <div>parse {p.perf.parseMs.toFixed(2)}ms  veh-build {p.perf.vehicleBuildMs.toFixed(2)}ms</div>
      </div>

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
