import { useEffect, useRef, useState } from 'react';
import type { NetworkData, SimStep, TLSUpdate, EdgeDataUpdate, GetAttributesResponse } from '../generated/sumo';

export type EdgeValueMap = Map<string, Record<string, number>>;

const RECONNECT_DELAY_MS = 2000;

export interface SimControlState {
  delayMs: number;
  paused: boolean;
  sumocfg_path: string;
  step_interval_current: number;
  step_at_min_bound: boolean;
  step_at_max_bound: boolean;
}

export type CommandResponse = Record<string, unknown> & { ok?: boolean; error?: string };

export interface SimState {
  connected: boolean;
  network: NetworkData | null;
  simStep: SimStep | null;
  tlsUpdate: TLSUpdate | null;
  edgeValueMap: EdgeValueMap;          // accumulated edge attribute values (base + deltas)
  edgeValueVersion: number;            // increments on each edgedata update to trigger re-render
  controlState: SimControlState | null;
  attributeConfig: GetAttributesResponse | null;
  updateAttributeConfig: (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) => void;
  sendCommand: (service: string, request?: Record<string, unknown>, onResponse?: (r: CommandResponse) => void) => void;
}

export function useSimSocket(url: string): SimState {
  const [connected, setConnected]           = useState(false);
  const [network, setNetwork]               = useState<NetworkData | null>(null);
  const [simStep, setSimStep]               = useState<SimStep | null>(null);
  const [tlsUpdate, setTlsUpdate]           = useState<TLSUpdate | null>(null);
  const [controlState, setControlState]     = useState<SimControlState | null>(null);
  const [attributeConfig, setAttributeConfig] = useState<GetAttributesResponse | null>(null);

  // accumulated edge value map: base + deltas merged in-place
  const edgeValueMapRef = useRef<EdgeValueMap>(new Map());
  const [edgeValueVersion, setEdgeValueVersion] = useState(0);
  const updateAttributeConfig = (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) =>
    setAttributeConfig(updater);

  // Latest-value buffers for high-frequency topics — written by onmessage,
  // flushed to React state once per animation frame to cap renders at 60 fps.
  const latestSimStep    = useRef<SimStep | null>(null);
  const latestTLS        = useRef<TLSUpdate | null>(null);
  const edgeDataDirty    = useRef(false);  // true when edgeValueMapRef was updated this frame
  const rafPending       = useRef(false);

  const wsRef            = useRef<WebSocket | null>(null);
  const reconnectTimer   = useRef<ReturnType<typeof setTimeout> | null>(null);
  const unmounted        = useRef(false);
  const pendingRef       = useRef<Map<string, (r: CommandResponse) => void>>(new Map());

  // RAF loop: drain latest-value refs into React state once per browser frame
  useEffect(() => {
    let rafId: number;
    const onRaf = () => {
      const ss  = latestSimStep.current;
      const tls = latestTLS.current;
      const dirty = edgeDataDirty.current;
      if (ss || tls || dirty) {
        latestSimStep.current = null;
        latestTLS.current     = null;
        edgeDataDirty.current = false;
        // all setters in one microtask → React 18 batches into one render
        if (ss)    setSimStep(ss);
        if (tls)   setTlsUpdate(tls);
        if (dirty) setEdgeValueVersion(v => v + 1);
      }
      rafPending.current = false;
      rafId = requestAnimationFrame(onRaf);
    };
    rafId = requestAnimationFrame(onRaf);
    return () => cancelAnimationFrame(rafId);
  }, []);

  const sendCommand = (service: string, request: Record<string, unknown> = {}, onResponse?: (r: CommandResponse) => void) => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const id = crypto.randomUUID();
    if (onResponse) pendingRef.current.set(id, onResponse);
    ws.send(JSON.stringify({ type: 'command', service, request, id }));
  };

  useEffect(() => {
    unmounted.current = false;

    function connect() {
      if (unmounted.current) return;

      const ws = new WebSocket(url);
      wsRef.current = ws;

      ws.onopen = () => setConnected(true);

      type Msg = { type: string; data?: unknown; id?: string; messages?: Msg[] } & Record<string, unknown>;

      const dispatch = (msg: Msg) => {
        switch (msg.type) {
          // high-frequency: write to ref, RAF loop flushes to state
          case 'simstep':  latestSimStep.current = msg.data as SimStep;  break;
          case 'tls':      latestTLS.current      = msg.data as TLSUpdate; break;
          case 'edgedata': {
            const ed = msg.data as EdgeDataUpdate & { full_snapshot?: boolean };
            if (ed.full_snapshot) edgeValueMapRef.current.clear();  // reset on full snapshot
            for (const e of ed.edges ?? []) {
              edgeValueMapRef.current.set(e.id, { ...edgeValueMapRef.current.get(e.id), ...e.attributes });
            }
            edgeDataDirty.current = true;
            break;
          }
          // low-frequency: set state immediately
          case 'network':
            edgeValueMapRef.current.clear();  // new network invalidates all edge data
            setNetwork(msg.data as NetworkData);
            break;
          case 'state': {
            const d = msg.data as { delay_ms?: number; paused?: boolean; sumocfg_path?: string; error?: string;
              step_interval_current?: number; step_at_min_bound?: boolean; step_at_max_bound?: boolean; };
            if (!d?.error && d?.delay_ms !== undefined)
              setControlState({ delayMs: d.delay_ms, paused: d.paused ?? false, sumocfg_path: d.sumocfg_path ?? '',
                step_interval_current: d.step_interval_current ?? 1,
                step_at_min_bound: d.step_at_min_bound ?? false,
                step_at_max_bound: d.step_at_max_bound ?? false });
            break;
          }
          case 'attributes': {
            const d = msg.data as GetAttributesResponse & { error?: string };
            if (!d?.error) setAttributeConfig(d);
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
        let envelope: Msg;
        try {
          performance.mark('ws-parse-start');
          envelope = JSON.parse(evt.data) as Msg;
          performance.mark('ws-parse-end');
          performance.measure('ws-parse', 'ws-parse-start', 'ws-parse-end');
        } catch {
          return;
        }
        if (envelope.type === 'batch') {
          for (const msg of envelope.messages ?? []) dispatch(msg);
        } else {
          dispatch(envelope);
        }
      };

      ws.onclose = () => {
        setConnected(false);
        if (!unmounted.current) {
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

  return { connected, network, simStep, tlsUpdate,
    edgeValueMap: edgeValueMapRef.current, edgeValueVersion,
    controlState, attributeConfig, updateAttributeConfig, sendCommand };
}
