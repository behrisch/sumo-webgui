import { useEffect, useRef, useState } from 'react';
import { SimStep, TLSUpdate, EdgeDataUpdate, LogMessage, NetworkGeometry, GetAttributesResponse } from '../generated/sumo';

export type EdgeValueMap = Map<string, Record<string, number>>;

const RECONNECT_DELAY_MS = 500;

// Binary frame type bytes (must match ecal_ws_bridge.py)
const TYPE_SIMSTEP  = 1;
const TYPE_TLS      = 2;
const TYPE_EDGEDATA = 3;
const TYPE_LOG      = 4;
const TYPE_NETWORK  = 5;

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
  reconnectAttempt: number;
  network: NetworkGeometry | null;
  simStep: SimStep | null;
  tlsUpdate: TLSUpdate | null;
  edgeValueMap: EdgeValueMap;
  edgeValueVersion: number;
  logMessages: LogMessage[];
  controlState: SimControlState | null;
  attributeConfig: GetAttributesResponse | null;
  updateAttributeConfig: (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) => void;
  sendCommand: (service: string, request?: Record<string, unknown>, onResponse?: (r: CommandResponse) => void) => void;
}

export function useSimSocket(url: string): SimState {
  const [connected, setConnected]             = useState(false);
  const [reconnectAttempt, setReconnectAttempt] = useState(0);
  const [network, setNetwork]                 = useState<NetworkGeometry | null>(null);
  const [simStep, setSimStep]                 = useState<SimStep | null>(null);
  const [tlsUpdate, setTlsUpdate]             = useState<TLSUpdate | null>(null);
  const [controlState, setControlState]       = useState<SimControlState | null>(null);
  const [attributeConfig, setAttributeConfig] = useState<GetAttributesResponse | null>(null);

  const edgeValueMapRef    = useRef<EdgeValueMap>(new Map());
  const [edgeValueVersion, setEdgeValueVersion] = useState(0);
  const [logMessages, setLogMessages]           = useState<LogMessage[]>([]);
  const recentLogTexts                          = useRef(new Set<string>());
  const updateAttributeConfig = (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) =>
    setAttributeConfig(updater);

  // Latest-value refs — written by onmessage, flushed once per animation frame
  const latestSimStep  = useRef<SimStep | null>(null);
  const latestTLS      = useRef<TLSUpdate | null>(null);
  const edgeDataDirty  = useRef(false);

  const wsRef          = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const unmounted      = useRef(false);
  const pendingRef     = useRef<Map<string, (r: CommandResponse) => void>>(new Map());

  // RAF loop: drain latest-value refs into React state once per browser frame
  useEffect(() => {
    let rafId: number;
    const onRaf = () => {
      const ss    = latestSimStep.current;
      const tls   = latestTLS.current;
      const dirty = edgeDataDirty.current;
      if (ss || tls || dirty) {
        latestSimStep.current = null;
        latestTLS.current     = null;
        edgeDataDirty.current = false;
        if (ss)    setSimStep(ss);
        if (tls)   setTlsUpdate(tls);
        if (dirty) setEdgeValueVersion(v => v + 1);
      }
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
      ws.binaryType = 'arraybuffer';
      wsRef.current = ws;

      // Force-close if the handshake never completes so onclose fires and we retry
      const connTimeout = setTimeout(() => {
        if (ws.readyState === WebSocket.CONNECTING) ws.close();
      }, 3000);

      ws.onopen = () => { clearTimeout(connTimeout); setConnected(true); setReconnectAttempt(0); };

      const dispatchBinary = (buf: ArrayBuffer) => {
        const bytes = new Uint8Array(buf);
        if (bytes.length < 2) return;
        const type    = bytes[0];
        const payload = bytes.subarray(1);
        performance.mark('ws-parse-start');
        switch (type) {
          case TYPE_SIMSTEP:
            latestSimStep.current = SimStep.decode(payload);
            break;
          case TYPE_TLS:
            latestTLS.current = TLSUpdate.decode(payload);
            break;
          case TYPE_EDGEDATA: {
            const ed = EdgeDataUpdate.decode(payload);
            if (ed.full_snapshot) edgeValueMapRef.current.clear();
            for (const e of ed.edges) {
              edgeValueMapRef.current.set(e.id, { ...edgeValueMapRef.current.get(e.id), ...e.attributes });
            }
            edgeDataDirty.current = true;
            break;
          }
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
          case TYPE_NETWORK:
            edgeValueMapRef.current.clear();
            setNetwork(NetworkGeometry.decode(payload));
            break;
        }
        performance.mark('ws-parse-end');
        performance.measure('ws-parse', 'ws-parse-start', 'ws-parse-end');
      };

      type JsonMsg = { type: string; data?: unknown; id?: string } & Record<string, unknown>;

      const dispatchJson = (msg: JsonMsg) => {
        switch (msg.type) {
          case 'state': {
            const d = msg.data as { delay_ms?: number; paused?: boolean; sumocfg_path?: string; error?: string;
              step_interval_current?: number; step_at_min_bound?: boolean; step_at_max_bound?: boolean };
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

  return { connected, reconnectAttempt, network, simStep, tlsUpdate,
    edgeValueMap: edgeValueMapRef.current, edgeValueVersion,
    logMessages, controlState, attributeConfig, updateAttributeConfig, sendCommand };
}
