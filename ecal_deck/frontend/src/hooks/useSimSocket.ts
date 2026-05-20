import { useCallback, useEffect, useRef, useState } from 'react';
import { SimBin, EdgeBin, VehicleTypeDict, TLSUpdate, LogMessage, NetworkGeometry, GetAttributesResponse } from '../generated/sumo';

const RECONNECT_DELAY_MS = 500;

// Binary frame type bytes (must match ecal_ws_bridge.py)
const TYPE_TLS         = 2;
const TYPE_LOG         = 4;
const TYPE_NETWORK     = 5;
const TYPE_SIMBIN      = 6;
const TYPE_EDGEBIN     = 7;
const TYPE_VEHICLETYPES = 8;

export interface SimControlState {
  delayMs: number;
  paused: boolean;
  sumocfg_path: string;
  step_interval_current: number;
  step_at_min_bound: boolean;
  step_at_max_bound: boolean;
  simulation_ready: boolean;
}

export type CommandResponse = Record<string, unknown> & { ok?: boolean; error?: string };

export interface VehicleTypeTable {
  ids: string[];
  lengths: Float32Array;
  widths: Float32Array;
  shapes: string[];
  classes: Uint8Array;  // 0=vehicle, 1=person, 2=container
}

export interface VehicleSnapshot {
  time_ms: number;
  veh_count: number;
  veh_positions: Float64Array;    // N×2
  veh_angles: Float32Array;       // N
  veh_speeds: Float32Array;       // N
  veh_attr_vals: Float32Array[];  // K_v separate arrays of N floats each (column-major unpacked)
  vehicle_ids: string[];          // N
  veh_type_indices: Uint32Array;  // N
  agent_count: number;
  agent_positions: Float64Array;  // A×2
  agent_angles: Float32Array;     // A
  agent_ids: string[];            // A
  agent_type_indices: Uint32Array; // A
}

export interface EdgeAttrState {
  attrNames: string[];     // from attributeConfig.edge_enabled at time of arrival
  values: Float32Array[];  // K_e arrays each of N_edges floats (NaN = no data yet)
}

export interface SimState {
  connected: boolean;
  reconnectAttempt: number;
  network: NetworkGeometry | null;
  vehicleSnapshot: VehicleSnapshot | null;
  vehicleTypeTable: VehicleTypeTable | null;
  edgeAttr: EdgeAttrState | null;
  edgeAttrVersion: number;
  tlsUpdate: TLSUpdate | null;
  logMessages: LogMessage[];
  controlState: SimControlState | null;
  attributeConfig: GetAttributesResponse | null;
  updateAttributeConfig: (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) => void;
  sendCommand: (service: string, request?: Record<string, unknown>, onResponse?: (r: CommandResponse) => void) => void;
}

// Helper: align Uint8Array to Float64Array
function toFloat64(u8: Uint8Array): Float64Array {
  if (u8.byteOffset % 8 === 0)
    return new Float64Array(u8.buffer, u8.byteOffset, u8.byteLength / 8);
  const a = new Uint8Array(u8.byteLength); a.set(u8);
  return new Float64Array(a.buffer, 0, u8.byteLength / 8);
}

// Helper: align Uint8Array to Float32Array
function toFloat32(u8: Uint8Array): Float32Array {
  if (u8.byteOffset % 4 === 0)
    return new Float32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
  const a = new Uint8Array(u8.byteLength); a.set(u8);
  return new Float32Array(a.buffer, 0, u8.byteLength / 4);
}

// Helper: align Uint8Array to Uint32Array
function toUint32(u8: Uint8Array): Uint32Array {
  if (u8.byteOffset % 4 === 0)
    return new Uint32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4);
  const a = new Uint8Array(u8.byteLength); a.set(u8);
  return new Uint32Array(a.buffer, 0, u8.byteLength / 4);
}

// Helper: parse null-terminated UTF-8 strings packed in a Uint8Array
function parseNullTermStrings(u8: Uint8Array): string[] {
  const decoder = new TextDecoder('utf-8');
  const result: string[] = [];
  let start = 0;
  for (let i = 0; i <= u8.length; i++) {
    if (i === u8.length || u8[i] === 0) {
      if (i > start) result.push(decoder.decode(u8.subarray(start, i)));
      start = i + 1;
    }
  }
  return result;
}

export function useSimSocket(url: string): SimState {
  const [connected, setConnected]             = useState(false);
  const [reconnectAttempt, setReconnectAttempt] = useState(0);
  const [network, setNetwork]                 = useState<NetworkGeometry | null>(null);
  const [vehicleSnapshot, setVehicleSnapshot] = useState<VehicleSnapshot | null>(null);
  const [vehicleTypeTable, setVehicleTypeTable] = useState<VehicleTypeTable | null>(null);
  const [tlsUpdate, setTlsUpdate]             = useState<TLSUpdate | null>(null);
  const [controlState, setControlState]       = useState<SimControlState | null>(null);
  const [attributeConfig, setAttributeConfig] = useState<GetAttributesResponse | null>(null);
  const [edgeAttrVersion, setEdgeAttrVersion] = useState(0);
  const [logMessages, setLogMessages]         = useState<LogMessage[]>([]);

  const recentLogTexts = useRef(new Set<string>());
  const updateAttributeConfig = (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) =>
    setAttributeConfig(updater);

  // Latest-value refs — written by onmessage, flushed once per animation frame
  const latestSnapshot      = useRef<VehicleSnapshot | null>(null);
  const latestTLS           = useRef<TLSUpdate | null>(null);
  const edgeDataDirty       = useRef(false);
  const edgeAttrRef         = useRef<EdgeAttrState | null>(null);
  const networkRef          = useRef<NetworkGeometry | null>(null);
  const attrConfigRef       = useRef<GetAttributesResponse | null>(null);
  const vehicleTypeTableRef = useRef<VehicleTypeTable | null>(null);

  const wsRef          = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const unmounted      = useRef(false);
  const pendingRef     = useRef<Map<string, (r: CommandResponse) => void>>(new Map());

  // Keep attrConfigRef in sync with React state
  const handleSetAttributeConfig = useCallback((updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) => {
    setAttributeConfig(prev => {
      const next = updater(prev);
      attrConfigRef.current = next;
      return next;
    });
  }, []);

  // RAF loop: drain latest-value refs into React state once per browser frame
  useEffect(() => {
    let rafId: number;
    const onRaf = () => {
      const ss    = latestSnapshot.current;
      const tls   = latestTLS.current;
      const dirty = edgeDataDirty.current;
      if (ss || tls || dirty) {
        latestSnapshot.current = null;
        latestTLS.current      = null;
        edgeDataDirty.current  = false;
        if (ss)    setVehicleSnapshot(ss);
        if (tls)   setTlsUpdate(tls);
        if (dirty) setEdgeAttrVersion(v => v + 1);
      }
      rafId = requestAnimationFrame(onRaf);
    };
    rafId = requestAnimationFrame(onRaf);
    return () => cancelAnimationFrame(rafId);
  }, []);

  const sendCommand = useCallback((service: string, request: Record<string, unknown> = {}, onResponse?: (r: CommandResponse) => void) => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const id = crypto.randomUUID();
    if (onResponse) pendingRef.current.set(id, onResponse);
    ws.send(JSON.stringify({ type: 'command', service, request, id }));
  }, []);

  useEffect(() => {
    unmounted.current = false;

    function connect() {
      if (unmounted.current) return;

      const ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
      wsRef.current = ws;

      // Force-close if the handshake never completes so onclose fires and we retry
      const connTimeout = setTimeout(() => {
        if (ws.readyState === WebSocket.CONNECTING) ws.close();
      }, 3000);

      ws.onopen = () => {
        clearTimeout(connTimeout);
        setConnected(true);
        setReconnectAttempt(0);
      };

      const dispatchBinary = (buf: ArrayBuffer) => {
        const bytes = new Uint8Array(buf);
        if (bytes.length < 2) return;
        const type    = bytes[0];
        const payload = bytes.subarray(1);
        performance.mark('ws-parse-start');
        switch (type) {
          case TYPE_VEHICLETYPES: {
            // Decode VehicleTypeDict, build VehicleTypeTable
            const vtd = VehicleTypeDict.decode(payload);
            const ids     = parseNullTermStrings(vtd.type_id_block);
            const shapes  = parseNullTermStrings(vtd.type_shapes);
            const lengths = toFloat32(vtd.type_lengths);
            const widths  = toFloat32(vtd.type_widths);
            const classes = vtd.type_classes instanceof Uint8Array
              ? vtd.type_classes
              : new Uint8Array(vtd.type_classes);
            const table: VehicleTypeTable = { ids, lengths, widths, shapes, classes };
            vehicleTypeTableRef.current = table;
            setVehicleTypeTable(table);
            break;
          }
          case TYPE_SIMBIN: {
            // Decode SimBin, build VehicleSnapshot
            const sb = SimBin.decode(payload);
            const N  = sb.veh_count;
            const A  = sb.agent_count;
            const K_v = sb.veh_attr_count;

            // Unpack column-major vehicle attr vals into K_v separate Float32Arrays
            const rawAttrVals = sb.veh_attr_vals;
            const veh_attr_vals: Float32Array[] = [];
            for (let k = 0; k < K_v; k++) {
              veh_attr_vals.push(toFloat32(rawAttrVals.subarray(k * N * 4, (k + 1) * N * 4)));
            }

            const snapshot: VehicleSnapshot = {
              time_ms:           sb.time_ms,
              veh_count:         N,
              veh_positions:     toFloat64(sb.veh_positions),
              veh_angles:        toFloat32(sb.veh_angles),
              veh_speeds:        toFloat32(sb.veh_speeds),
              veh_attr_vals,
              vehicle_ids:       parseNullTermStrings(sb.vehicle_ids).slice(0, N),
              veh_type_indices:  toUint32(sb.veh_type_indices),
              agent_count:       A,
              agent_positions:   toFloat64(sb.agent_positions),
              agent_angles:      toFloat32(sb.agent_angles),
              agent_ids:         parseNullTermStrings(sb.agent_ids).slice(0, A),
              agent_type_indices: toUint32(sb.agent_type_indices),
            };
            latestSnapshot.current = snapshot;
            break;
          }
          case TYPE_EDGEBIN: {
            // Decode EdgeBin, update edgeAttrRef
            const eb = EdgeBin.decode(payload);
            const N_edges = networkRef.current?.edge_ids.length ?? 0;
            if (N_edges === 0) break;

            const K_e       = eb.attr_count;
            const N_recv    = eb.edge_count;
            const attrNames = (attrConfigRef.current?.edge_enabled ?? []).slice(0, K_e);

            // Determine if we need to create/reset the state
            const prev = edgeAttrRef.current;
            const needReset = eb.full_snapshot || prev === null || prev.values.length !== K_e;

            let state: EdgeAttrState;
            if (needReset) {
              const values: Float32Array[] = [];
              for (let k = 0; k < K_e; k++) {
                const arr = new Float32Array(N_edges);
                arr.fill(NaN);
                values.push(arr);
              }
              state = { attrNames, values };
            } else {
              state = prev!;
              // Update attrNames in case they changed
              state = { ...state, attrNames };
            }

            // Parse edge_indices and attr_vals
            const edgeIndices = toUint32(eb.edge_indices);
            for (let k = 0; k < K_e; k++) {
              const colBytes = eb.attr_vals.subarray(k * N_recv * 4, (k + 1) * N_recv * 4);
              const colVals  = toFloat32(colBytes);
              const targetArr = state.values[k];
              for (let j = 0; j < N_recv; j++) {
                targetArr[edgeIndices[j]] = colVals[j];
              }
            }

            edgeAttrRef.current  = state;
            edgeDataDirty.current = true;
            break;
          }
          case TYPE_TLS:
            latestTLS.current = TLSUpdate.decode(payload);
            break;
          case TYPE_LOG: {
            const m = LogMessage.decode(payload);
            if (!recentLogTexts.current.has(m.text)) {
              recentLogTexts.current.add(m.text);
              if (recentLogTexts.current.size > 50) recentLogTexts.current.clear();
              setLogMessages(prev => {
                const next = [...prev, m];
                return next.length > 200 ? next.slice(-200) : next;
              });
            }
            break;
          }
          case TYPE_NETWORK: {
            const ng = NetworkGeometry.decode(payload);
            networkRef.current = ng;
            // Reset all simulation state on new network
            edgeAttrRef.current       = null;
            latestSnapshot.current    = null;
            vehicleTypeTableRef.current = null;
            setVehicleSnapshot(null);
            setVehicleTypeTable(null);
            setNetwork(ng);
            break;
          }
        }
        performance.mark('ws-parse-end');
        performance.measure('ws-parse', 'ws-parse-start', 'ws-parse-end');
      };

      type JsonMsg = { type: string; data?: unknown; id?: string } & Record<string, unknown>;

      const dispatchJson = (msg: JsonMsg) => {
        switch (msg.type) {
          case 'state': {
            const d = msg.data as { delay_ms?: number; paused?: boolean; sumocfg_path?: string; error?: string;
              step_interval_current?: number; step_at_min_bound?: boolean; step_at_max_bound?: boolean;
              simulation_ready?: boolean };
            if (!d?.error && d?.delay_ms !== undefined)
              setControlState({ delayMs: d.delay_ms, paused: d.paused ?? false, sumocfg_path: d.sumocfg_path ?? '',
                step_interval_current: d.step_interval_current ?? 1,
                step_at_min_bound: d.step_at_min_bound ?? false,
                step_at_max_bound: d.step_at_max_bound ?? false,
                simulation_ready: d.simulation_ready ?? false });
            break;
          }
          case 'attributes': {
            const d = msg.data as GetAttributesResponse & { error?: string };
            if (!d?.error) {
              attrConfigRef.current = d;
              setAttributeConfig(d);
            }
            break;
          }
          case 'response': {
            const cb = msg.id ? pendingRef.current.get(msg.id) : undefined;
            if (cb) { pendingRef.current.delete(msg.id!); cb(msg as CommandResponse); }
            break;
          }
        }
      };

      ws.onmessage = (evt) => {
        if (evt.data instanceof ArrayBuffer) {
          dispatchBinary(evt.data);
        } else {
          try {
            dispatchJson(JSON.parse(evt.data as string) as JsonMsg);
          } catch { /* ignore malformed JSON */ }
        }
      };

      ws.onclose = () => {
        clearTimeout(connTimeout);
        setConnected(false);
        if (!unmounted.current) {
          setReconnectAttempt(n => n + 1);
          reconnectTimer.current = setTimeout(connect, RECONNECT_DELAY_MS);
        }
      };

      ws.onerror = () => ws.close();
    }

    connect();

    return () => {
      unmounted.current = true;
      if (reconnectTimer.current) clearTimeout(reconnectTimer.current);
      wsRef.current?.close();
    };
  }, [url]);

  return {
    connected, reconnectAttempt, network,
    vehicleSnapshot, vehicleTypeTable,
    edgeAttr: edgeAttrRef.current, edgeAttrVersion,
    tlsUpdate, logMessages, controlState,
    attributeConfig,
    updateAttributeConfig: handleSetAttributeConfig,
    sendCommand,
  };
}
