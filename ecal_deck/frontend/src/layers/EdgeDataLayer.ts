import { PathLayer } from '@deck.gl/layers';
import type { ParsedNetwork } from '../App';
import type { EdgeValueMap } from '../hooks/useSimSocket';
import { colormap } from '../utils/colormap';

export function buildEdgeDataLayer(
  parsed: ParsedNetwork,
  valueMap: EdgeValueMap,
  colorAttr: string,
) {
  let min = Infinity, max = -Infinity;
  for (const attrs of valueMap.values()) {
    const val = attrs[colorAttr];
    if (val !== undefined) {
      if (val < min) min = val;
      if (val > max) max = val;
    }
  }
  const range = max - min || 1;

  // one colour entry per lane, looked up via lane→edge index
  const colors = new Uint8Array(parsed.laneCount * 4);
  for (let i = 0; i < parsed.laneCount; i++) {
    const edgeId = parsed.edgeIds[parsed.laneEdgeIndices[i]];
    const val = valueMap.get(edgeId)?.[colorAttr];
    if (val === undefined) {
      colors[i * 4] = 144; colors[i * 4 + 1] = 144; colors[i * 4 + 2] = 144; colors[i * 4 + 3] = 80;
    } else {
      const [r, g, b, a] = colormap(Math.max(0, Math.min(1, (val - min) / range)));
      colors[i * 4] = r; colors[i * 4 + 1] = g; colors[i * 4 + 2] = b; colors[i * 4 + 3] = a;
    }
  }

  return new PathLayer({
    id: 'edgedata',
    data: {
      length: parsed.laneCount,
      startIndices: parsed.laneStarts,
      attributes: {
        getPath:  { value: parsed.lanePositions, size: 2 },
        getColor: { value: colors, size: 4 },
      },
    },
    _pathType: 'open',
    widthUnits: 'meters',
    widthScale: 1,
    widthMinPixels: 2,
    getWidth: (_: unknown, { index }: { index: number }) => parsed.laneWidths[index],
    updateTriggers: { getColor: [colors] },
    pickable: false,
  });
}
