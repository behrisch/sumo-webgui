import { useEffect, useRef, useState } from 'react';
import type { NetworkData, SimStep, TLSUpdate, EdgeDataUpdate, GetAttributesResponse } from '../generated/sumo';

const RECONNECT_DELAY_MS = 2000;

export interface SimControlState {
  delayMs: number;
  paused: boolean;
}

export type CommandResponse = Record<string, unknown> & { ok?: boolean; error?: string };

export interface SimState {
  connected: boolean;
  network: NetworkData | null;
  simStep: SimStep | null;
  tlsUpdate: TLSUpdate | null;
  edgeDataUpdate: EdgeDataUpdate | null;
  controlState: SimControlState | null;
  attributeConfig: GetAttributesResponse | null;
  updateAttributeConfig: (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) => void;
  sendCommand: (service: string, request?: Record<string, unknown>, onResponse?: (r: CommandResponse) => void) => void;
}

export function useSimSocket(url: string): SimState {
  const [connected, setConnected] = useState(false);
  const [network, setNetwork] = useState<NetworkData | null>(null);
  const [simStep, setSimStep] = useState<SimStep | null>(null);
  const [tlsUpdate, setTlsUpdate] = useState<TLSUpdate | null>(null);
  const [edgeDataUpdate, setEdgeDataUpdate] = useState<EdgeDataUpdate | null>(null);
  const [controlState, setControlState] = useState<SimControlState | null>(null);
  const [attributeConfig, setAttributeConfig] = useState<GetAttributesResponse | null>(null);
  const updateAttributeConfig = (updater: (prev: GetAttributesResponse | null) => GetAttributesResponse | null) =>
    setAttributeConfig(updater);

  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const unmounted = useRef(false);

  const pendingRef = useRef<Map<string, (r: CommandResponse) => void>>(new Map());

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

      ws.onmessage = (evt) => {
        let envelope: { type: string; data: unknown };
        try {
          envelope = JSON.parse(evt.data);
        } catch {
          return;
        }
        switch (envelope.type) {
          case 'network':   setNetwork(envelope.data as NetworkData); break;
          case 'simstep':   setSimStep(envelope.data as SimStep); break;
          case 'tls':       setTlsUpdate(envelope.data as TLSUpdate); break;
          case 'edgedata':  setEdgeDataUpdate(envelope.data as EdgeDataUpdate); break;
          case 'state': {
            const d = envelope.data as { delay_ms?: number; paused?: boolean; error?: string };
            if (!d.error && d.delay_ms !== undefined)
              setControlState({ delayMs: d.delay_ms, paused: d.paused ?? false });
            break;
          }
          case 'response': {
            const r = envelope as { id?: string; data?: unknown } & CommandResponse;
            const cb = r.id ? pendingRef.current.get(r.id) : undefined;
            if (cb) { pendingRef.current.delete(r.id!); cb(r); }
            break;
          }
          case 'attributes': {
            const d = envelope.data as GetAttributesResponse & { error?: string };
            if (!d.error) setAttributeConfig(d);
            break;
          }
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

  return { connected, network, simStep, tlsUpdate, edgeDataUpdate, controlState, attributeConfig, updateAttributeConfig, sendCommand };
}
