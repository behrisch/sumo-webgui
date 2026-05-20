import { useState } from 'react';
import type { TLSPhase, GetVehicleInfoResponse, GetEdgeInfoResponse, GetAttributesResponse } from '../generated/sumo';
import type { VehicleSnapshot, EdgeAttrState, CommandResponse } from '../hooks/useSimSocket';

export type SelectedObject =
  | { type: 'vehicle';   id: string }
  | { type: 'person';    id: string }
  | { type: 'container'; id: string }
  | { type: 'edge';      id: string }
  | { type: 'junction';  id: string }
  | { type: 'tls';       id: string; tlIndex: number };

interface Props {
  selected: SelectedObject;
  snapshot: VehicleSnapshot | null;
  edgeAttr: EdgeAttrState | null;
  edgeIdToIndex: Map<string, number>;
  attrConfig: GetAttributesResponse | null;
  tlsLights: TLSPhase[];
  following: boolean;
  onFollow: () => void;
  onClose: () => void;
  sendCommand: (service: string, request?: Record<string, unknown>, onResponse?: (r: CommandResponse) => void) => void;
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

function VehicleInfo({ id, snapshot, attrConfig }: { id: string; snapshot: VehicleSnapshot | null; attrConfig: GetAttributesResponse | null }) {
  const idx = snapshot?.vehicle_ids.indexOf(id) ?? -1;
  if (idx < 0) return <div style={{ opacity: 0.5 }}>Vehicle no longer present</div>;
  const speed     = snapshot!.veh_speeds[idx] ?? 0;
  const angle     = snapshot!.veh_angles[idx] ?? 0;
  const attrNames = attrConfig?.vehicle_enabled ?? [];
  return (
    <>
      {row('id',    id)}
      {row('speed', speed.toFixed(1) + ' m/s (' + (speed * 3.6).toFixed(0) + ' km/h)')}
      {row('angle', angle.toFixed(0) + '°')}
      {attrNames.map((name, k) => {
        const vals = snapshot!.veh_attr_vals[k];
        return vals ? row(name, vals[idx].toFixed(3)) : null;
      })}
    </>
  );
}

function AgentInfo({ id, snapshot }: { id: string; snapshot: VehicleSnapshot | null }) {
  const idx = snapshot?.agent_ids.indexOf(id) ?? -1;
  if (idx < 0) return <div style={{ opacity: 0.5 }}>No longer present</div>;
  return (
    <>
      {row('id',    id)}
      {row('angle', (snapshot!.agent_angles[idx] ?? 0).toFixed(0) + '°')}
    </>
  );
}

function EdgeInfo({ id, edgeAttr, edgeIdToIndex }: { id: string; edgeAttr: EdgeAttrState | null; edgeIdToIndex: Map<string, number> }) {
  const ei = edgeIdToIndex.get(id);
  if (ei === undefined || !edgeAttr || edgeAttr.values.length === 0)
    return <>{row('id', id)}<div style={{ opacity: 0.5 }}>No edge data collected</div></>;
  return (
    <>
      {row('id', id)}
      {edgeAttr.attrNames.map((name, k) => {
        const val = edgeAttr.values[k][ei];
        return isNaN(val) ? null : row(name, val.toFixed(3));
      })}
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
      {row('tls id',   id)}
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

export function InfoPanel({ selected, snapshot, edgeAttr, edgeIdToIndex, attrConfig, tlsLights, following, onFollow, onClose, sendCommand }: Props) {
  const [extraVehicle, setExtraVehicle] = useState<GetVehicleInfoResponse | null>(null);
  const [extraEdge,    setExtraEdge]    = useState<GetEdgeInfoResponse    | null>(null);
  const [loading, setLoading] = useState(false);

  const fetchMore = () => {
    setLoading(true);
    if (selected.type === 'vehicle') {
      sendCommand('get_vehicle_info', { id: selected.id }, (r) => {
        setLoading(false);
        setExtraVehicle(r as unknown as GetVehicleInfoResponse);
      });
    } else if (selected.type === 'edge') {
      sendCommand('get_edge_info', { id: selected.id }, (r) => {
        setLoading(false);
        setExtraEdge(r as unknown as GetEdgeInfoResponse);
      });
    }
  };
  const titles: Record<string, string> = {
    vehicle: 'Vehicle', person: 'Person', container: 'Container',
    edge: 'Edge', junction: 'Junction', tls: 'Signal',
  };

  return (
    <div style={panel}>
      <div style={header}>
        <span style={{ fontWeight: 'bold' }}>{titles[selected.type]} — {selected.id}</span>
        <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
          {selected.type === 'vehicle' && (
            <button onClick={onFollow} title={following ? 'Stop following' : 'Follow vehicle'} style={{
              background: following ? '#246' : 'none', border: '1px solid #555',
              color: following ? '#8bf' : '#aaa', borderRadius: 3,
              cursor: 'pointer', fontSize: 11, padding: '1px 5px',
            }}>⊙</button>
          )}
          <button onClick={onClose} style={{
            background: 'none', border: 'none', color: '#aaa',
            cursor: 'pointer', fontSize: 14, padding: 0,
          }}>✕</button>
        </div>
      </div>

      {selected.type === 'vehicle'   && <VehicleInfo id={selected.id} snapshot={snapshot} attrConfig={attrConfig} />}
      {selected.type === 'person'    && <AgentInfo id={selected.id} snapshot={snapshot} />}
      {selected.type === 'container' && <AgentInfo id={selected.id} snapshot={snapshot} />}
      {selected.type === 'edge'      && <EdgeInfo id={selected.id} edgeAttr={edgeAttr} edgeIdToIndex={edgeIdToIndex} />}
      {selected.type === 'junction'  && row('id', selected.id)}
      {selected.type === 'tls' && (
        <TLSInfo id={selected.id} tlIndex={selected.tlIndex} tlsLights={tlsLights} />
      )}

      {/* deep query — on demand */}
      {(selected.type === 'vehicle' || selected.type === 'edge') && (
        <>
          <div style={{ borderTop: '1px solid #333', paddingTop: 4 }}>
            <button onClick={fetchMore} disabled={loading} style={{
              background: 'none', border: '1px solid #555', color: '#aaa',
              borderRadius: 3, cursor: 'pointer', fontSize: 11, padding: '1px 6px',
            }}>
              {loading ? 'Loading…' : '+ More info'}
            </button>
          </div>
          {selected.type === 'vehicle' && extraVehicle && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
              {row('route',    extraVehicle.route_id   ?? '?')}
              {row('lane',     extraVehicle.lane_id    ?? '?')}
              {row('lane pos', ((extraVehicle.lane_pos ?? 0) as number).toFixed(1) + ' m')}
              {(extraVehicle.route_edges ?? []).length > 0 && (
                <div style={{ opacity: 0.5, fontSize: 11, wordBreak: 'break-all' }}>
                  {(extraVehicle.route_edges ?? []).join(' → ')}
                </div>
              )}
            </div>
          )}
          {selected.type === 'edge' && extraEdge && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
              {row('speed',    ((extraEdge.mean_speed   ?? 0) as number).toFixed(1) + ' m/s')}
              {row('vehicles', String(extraEdge.vehicle_count ?? 0))}
              {row('halting',  String(extraEdge.halting_count ?? 0))}
              {row('occupancy',((extraEdge.occupancy    ?? 0) as number).toFixed(1) + ' %')}
              {row('wait',     ((extraEdge.waiting_time ?? 0) as number).toFixed(1) + ' s')}
              {(extraEdge.vehicle_ids ?? []).length > 0 && (
                <div style={{ opacity: 0.5, fontSize: 11 }}>
                  {(extraEdge.vehicle_ids ?? []).join(', ')}
                </div>
              )}
            </div>
          )}
        </>
      )}
    </div>
  );
}
