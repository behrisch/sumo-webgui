import { useEffect, useMemo, useState } from 'react';
import DeckGL from '@deck.gl/react';
import { OrthographicView } from '@deck.gl/core';
import type { MapViewState, OrthographicViewState } from '@deck.gl/core';
import Map from 'react-map-gl/maplibre';
import 'maplibre-gl/dist/maplibre-gl.css';
import type { Feature, FeatureCollection } from 'geojson';

import { useSimSocket } from './hooks/useSimSocket';
import { buildNetworkLayer } from './layers/NetworkLayer';
import { buildVehicleLayer } from './layers/VehicleLayer';
import { buildTLSLayer } from './layers/TLSLayer';
import { buildEdgeDataLayer } from './layers/EdgeDataLayer';
import { ControlPanel } from './components/ControlPanel';
import type { LayerVisibility } from './components/ControlPanel';

const WS_URL = 'ws://localhost:8765';
const BASEMAP_STYLES: Record<string, string> = {
  liberty:   'https://tiles.openfreemap.org/styles/liberty',
  bright:    'https://tiles.openfreemap.org/styles/bright',
  positron:  'https://tiles.openfreemap.org/styles/positron',
  demotiles: 'https://demotiles.maplibre.org/style.json',
};

interface ParsedNetwork {
  geoReferenced: boolean;
  edgeFeatures: Feature[];
  junctionFeatures: Feature[];
  tlsFeatures: Feature[];
  initialViewState: MapViewState | OrthographicViewState;
}

function parseNetwork(geojson: string, geoReferenced: boolean): ParsedNetwork {
  const fc = JSON.parse(geojson) as FeatureCollection;
  const edgeFeatures     = fc.features.filter((f) => f.properties?.['element'] === 'edge');
  const junctionFeatures = fc.features.filter((f) => f.properties?.['element'] === 'junction');
  const tlsFeatures      = fc.features.filter((f) => f.properties?.['element'] === 'tls_connection');

  const coords = [...edgeFeatures, ...junctionFeatures].flatMap((f) => {
    const geom = f.geometry as { coordinates: number[][] | number[][][] } | null;
    if (!geom) return [];
    const c = geom.coordinates;
    return Array.isArray(c[0][0]) ? (c as number[][][]).flat() : (c as number[][]);
  });

  const xs = coords.map((c) => c[0]);
  const ys = coords.map((c) => c[1]);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;

  let initialViewState: MapViewState | OrthographicViewState;
  if (geoReferenced) {
    const zoom = Math.floor(Math.log2(360 / Math.max(maxX - minX || 0.01, maxY - minY || 0.01))) - 1;
    initialViewState = { longitude: cx, latitude: cy, zoom, pitch: 0, bearing: 0 } as MapViewState;
  } else {
    const zoom = Math.log2(Math.min(window.innerWidth / (maxX - minX || 100), window.innerHeight / (maxY - minY || 100))) - 0.5;
    initialViewState = { target: [cx, cy, 0], zoom } as OrthographicViewState;
  }

  return { geoReferenced, edgeFeatures, junctionFeatures, tlsFeatures, initialViewState };
}

export default function App() {
  const { connected, network, simStep, tlsUpdate, edgeDataUpdate, controlState, attributeConfig, updateAttributeConfig, sendCommand } = useSimSocket(WS_URL);

  const parsed = useMemo(
    () => (network ? parseNetwork(network.geojson, network.geo_referenced) : null),
    [network],
  );

  const [viewState, setViewState] = useState<MapViewState | OrthographicViewState | null>(null);
  const activeView = viewState ?? parsed?.initialViewState ?? null;

  const [paused, setPaused] = useState(false);
  const [delayMs, setDelayMs] = useState(0);
  const [basemapStyle, setBasemapStyle] = useState('liberty');

  // sync initial state from publisher on connect
  useEffect(() => {
    if (controlState) {
      setDelayMs(controlState.delayMs);
      setPaused(controlState.paused);
    }
  }, [controlState]);

  const vehicleKeys = attributeConfig?.vehicle_enabled ?? [];
  const edgeKeys    = attributeConfig?.edge_enabled    ?? [];

  const handlePause  = () => { sendCommand('pause');  setPaused(true);  };
  const handleResume = () => { sendCommand('resume'); setPaused(false); };
  const handleStep   = () => { sendCommand('step'); };
  const handleDelay      = (ms: number) => { setDelayMs(ms); sendCommand('set_delay', { delay_ms: ms }); };
  const handleAttributes = (vehicle: string[], edge: string[]) => {
    sendCommand('set_attributes', { vehicle_attributes: vehicle, edge_attributes: edge });
    updateAttributeConfig((prev) => prev ? { ...prev, vehicle_enabled: vehicle, edge_enabled: edge } : prev);
  };

  const [visibility, setVisibility] = useState<LayerVisibility>({
    edges: true, junctions: true, vehicles: true, tls: true, edgeData: true, basemap: true,
  });
  const patchVisibility = (patch: Partial<LayerVisibility>) =>
    setVisibility((v) => ({ ...v, ...patch }));

  const [vehicleColorAttr, setVehicleColorAttr] = useState('speed');
  const [edgeColorAttr, setEdgeColorAttr]       = useState('');

  const layers = useMemo(() => {
    if (!parsed) return [];
    const result = [];
    const [edgeLayer, junctionLayer] = buildNetworkLayer(parsed.edgeFeatures, parsed.junctionFeatures);
    if (visibility.junctions) result.push(junctionLayer);
    if (visibility.edges)     result.push(edgeLayer);
    if (visibility.edgeData && edgeColorAttr && edgeDataUpdate?.edges.length)
      result.push(buildEdgeDataLayer(parsed.edgeFeatures, edgeDataUpdate.edges, edgeColorAttr));
    if (visibility.tls)
      result.push(buildTLSLayer(parsed.tlsFeatures, tlsUpdate?.lights ?? []));
    if (visibility.vehicles)
      result.push(buildVehicleLayer(simStep?.vehicles ?? [],
        vehicleColorAttr === 'speed' ? undefined : vehicleColorAttr));
    return result;
  }, [parsed, simStep, tlsUpdate, edgeDataUpdate, visibility, vehicleColorAttr, edgeColorAttr]);

  if (!parsed || !activeView) {
    return (
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100vh', fontFamily: 'monospace' }}>
        {connected ? 'Waiting for network data…' : 'Connecting to bridge…'}
      </div>
    );
  }

  const panel = (
    <ControlPanel
      connected={connected} paused={paused}
      onPause={handlePause} onResume={handleResume} onStep={handleStep}
      delayMs={delayMs} onSetDelay={handleDelay}
      simStep={simStep} geoReferenced={parsed.geoReferenced}
      basemapStyle={basemapStyle} basemapStyles={Object.keys(BASEMAP_STYLES)} onBasemapStyle={setBasemapStyle}
      visibility={visibility} onVisibility={patchVisibility}
      vehicleColorAttr={vehicleColorAttr} vehicleKeys={vehicleKeys} onVehicleColorAttr={setVehicleColorAttr}
      edgeColorAttr={edgeColorAttr} edgeKeys={edgeKeys} onEdgeColorAttr={setEdgeColorAttr}
      attributeConfig={attributeConfig} onSetAttributes={handleAttributes}
    />
  );

  const onViewChange = ({ viewState: vs }: { viewState: MapViewState | OrthographicViewState }) =>
    setViewState(vs);

  if (parsed.geoReferenced) {
    return (
      <div style={{ width: '100vw', height: '100vh' }}>
        <DeckGL viewState={activeView as MapViewState} onViewStateChange={onViewChange} controller layers={layers}>
          {visibility.basemap && <Map mapStyle={BASEMAP_STYLES[basemapStyle]} />}
        </DeckGL>
        {panel}
      </div>
    );
  }

  return (
    <div style={{ width: '100vw', height: '100vh' }}>
      <DeckGL views={new OrthographicView({ id: 'ortho' })} viewState={activeView as OrthographicViewState}
        onViewStateChange={onViewChange} controller layers={layers}>
        <div style={{ background: '#1a1a2e', width: '100%', height: '100%' }} />
      </DeckGL>
      {panel}
    </div>
  );
}
