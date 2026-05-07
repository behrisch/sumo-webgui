import { GeoJsonLayer } from '@deck.gl/layers';
import type { Feature, FeatureCollection } from 'geojson';
import type { TLSPhase } from '../generated/sumo';

// SUMO signal character → RGBA
const SIGNAL_COLORS: Record<string, [number, number, number, number]> = {
  G: [0,   200, 0,   255],  // green
  g: [0,   200, 0,   180],  // green (minor / permissive)
  Y: [255, 200, 0,   255],  // yellow
  y: [255, 200, 0,   180],
  R: [200, 0,   0,   255],  // red
  r: [200, 0,   0,   180],
  u: [80,  80,  80,  255],  // off / unknown
  o: [80,  80,  80,  255],
};
const DEFAULT_COLOR: [number, number, number, number] = [80, 80, 80, 255];

interface TLSFeatureProps {
  element: string;
  tls: string;
  tlIndex: number;
}

export function buildTLSLayer(tlsFeatures: Feature[], lights: TLSPhase[]) {
  const stateMap: Record<string, string> = {};
  for (const phase of lights) stateMap[phase.id] = phase.state;

  const data: FeatureCollection = { type: 'FeatureCollection', features: tlsFeatures };
  return new GeoJsonLayer({
    id: 'tls',
    data,
    stroked: true,
    filled: false,
    lineWidthMinPixels: 3,
    getLineColor: (f: { properties: TLSFeatureProps }) => {
      const state = stateMap[f.properties.tls];
      if (!state) return DEFAULT_COLOR;
      const ch = state[f.properties.tlIndex] ?? 'u';
      return SIGNAL_COLORS[ch] ?? DEFAULT_COLOR;
    },
    updateTriggers: { getLineColor: [lights] },
    pickable: false,
  });
}
