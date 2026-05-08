import type { Vehicle, TLSPhase } from '../generated/sumo';
import type { EdgeValueMap } from '../hooks/useSimSocket';

export type SelectedObject =
  | { type: 'vehicle';  id: string; index: number }
  | { type: 'edge';     id: string }
  | { type: 'junction'; id: string }
  | { type: 'tls';      id: string; tlIndex: number };

interface Props {
  selected: SelectedObject;
  vehicles: Vehicle[];
  edgeValueMap: EdgeValueMap;
  tlsLights: TLSPhase[];
  onClose: () => void;
}

const panel: React.CSSProperties = {
  position: 'absolute', top: 8, left: 8,
  background: 'rgba(0,0,0,0.75)', color: '#ddd',
  fontFamily: 'monospace', fontSize: 12, borderRadius: 6,
  padding: '8px 12px', minWidth: 200, maxWidth: 320,
  display: 'flex', flexDirection: 'column', gap: 4,
  userSelect: 'none',
};

const header: React.CSSProperties = {
  display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  borderBottom: '1px solid #444', paddingBottom: 4, marginBottom: 2,
};

const row = (label: string, value: string | number) => (
  <div key={label} style={{ display: 'flex', gap: 8 }}>
    <span style={{ opacity: 0.5, minWidth: 90 }}>{label}</span>
    <span>{value}</span>
  </div>
);

function VehicleInfo({ index, vehicles }: { index: number; vehicles: Vehicle[] }) {
  const v = vehicles[index];
  if (!v) return <div style={{ opacity: 0.5 }}>Vehicle no longer present</div>;
  // protobuf omits default (0) values — guard with ?? 0
  const speed = v.speed ?? 0;
  const angle = v.angle ?? 0;
  return (
    <>
      {row('id',    v.id ?? '?')}
      {row('type',  v.type_id ?? '?')}
      {row('speed', speed.toFixed(1) + ' m/s (' + (speed * 3.6).toFixed(0) + ' km/h)')}
      {row('angle', angle.toFixed(0) + '°')}
      {Object.entries(v.attributes ?? {}).map(([k, val]) =>
        row(k, typeof val === 'number' ? val.toFixed(3) : String(val))
      )}
    </>
  );
}

function EdgeInfo({ id, edgeValueMap }: { id: string; edgeValueMap: EdgeValueMap }) {
  const attrs = edgeValueMap.get(id);
  if (!attrs || Object.keys(attrs).length === 0)
    return <div style={{ opacity: 0.5 }}>No edge data collected</div>;
  return (
    <>
      {row('id', id)}
      {Object.entries(attrs).map(([k, val]) => row(k, val.toFixed(3)))}
    </>
  );
}

function TLSInfo({ id, tlIndex, tlsLights }: { id: string; tlIndex: number; tlsLights: TLSPhase[] }) {
  const phase = tlsLights.find(l => l.id === id);
  const char  = phase?.state?.[tlIndex] ?? '?';
  const names: Record<string, string> = {
    G: 'green', g: 'green (minor)', Y: 'yellow', y: 'yellow',
    R: 'red',   r: 'red (off)',     u: 'off',    o: 'off',
  };
  const colors: Record<string, string> = { G: '#0c0', g: '#0a0', Y: '#fc0', y: '#fa0', R: '#f00', r: '#800' };
  return (
    <>
      {row('tls id',  id)}
      {row('link idx', tlIndex)}
      <div style={{ display: 'flex', gap: 8 }}>
        <span style={{ opacity: 0.5, minWidth: 90 }}>signal</span>
        <span style={{ color: colors[char] ?? '#aaa' }}>
          {char} — {names[char] ?? 'unknown'}
        </span>
      </div>
      {phase && row('full state', phase.state)}
    </>
  );
}

export function InfoPanel({ selected, vehicles, edgeValueMap, tlsLights, onClose }: Props) {
  const titles: Record<string, string> = {
    vehicle: 'Vehicle', edge: 'Edge', junction: 'Junction', tls: 'Signal',
  };

  return (
    <div style={panel}>
      <div style={header}>
        <span style={{ fontWeight: 'bold' }}>{titles[selected.type]} — {selected.id}</span>
        <button onClick={onClose} style={{
          background: 'none', border: 'none', color: '#aaa',
          cursor: 'pointer', fontSize: 14, padding: 0,
        }}>✕</button>
      </div>

      {selected.type === 'vehicle' && (
        <VehicleInfo index={selected.index} vehicles={vehicles} />
      )}
      {selected.type === 'edge' && (
        <EdgeInfo id={selected.id} edgeValueMap={edgeValueMap} />
      )}
      {selected.type === 'junction' && (
        row('id', selected.id)
      )}
      {selected.type === 'tls' && (
        <TLSInfo id={selected.id} tlIndex={selected.tlIndex} tlsLights={tlsLights} />
      )}
    </div>
  );
}
