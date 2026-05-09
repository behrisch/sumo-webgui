import { LineLayer } from '@deck.gl/layers';
import type { TlsEntry, TLSPhase } from '../generated/sumo';

const SIGNAL_COLORS: Record<string, [number, number, number, number]> = {
  G: [0,   200, 0,   255],
  g: [0,   200, 0,   180],
  Y: [255, 200, 0,   255],
  y: [255, 200, 0,   180],
  R: [200, 0,   0,   255],
  r: [200, 0,   0,   180],
  u: [80,  80,  80,  255],
  o: [80,  80,  80,  255],
};
const DEFAULT_COLOR: [number, number, number, number] = [80, 80, 80, 255];

export function buildTLSLayer(
  tlsEntries: TlsEntry[],
  tlsPositions: Float64Array,
  lights: TLSPhase[],
) {
  const stateMap: Record<string, string> = {};
  for (const phase of lights) stateMap[phase.id] = phase.state;

  return new LineLayer({
    id: 'tls',
    data: tlsEntries,
    getSourcePosition: (_: TlsEntry, { index }: { index: number }) =>
      [tlsPositions[index * 4], tlsPositions[index * 4 + 1]],
    getTargetPosition: (_: TlsEntry, { index }: { index: number }) =>
      [tlsPositions[index * 4 + 2], tlsPositions[index * 4 + 3]],
    getColor: (e: TlsEntry) => {
      const state = stateMap[e.tls];
      if (!state) return DEFAULT_COLOR;
      return SIGNAL_COLORS[state[e.tl_index] ?? 'u'] ?? DEFAULT_COLOR;
    },
    getWidth: 3,
    widthMinPixels: 3,
    updateTriggers: { getColor: [lights] },
    pickable: true,
  });
}
