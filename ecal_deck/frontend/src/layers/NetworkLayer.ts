import { PathLayer, SolidPolygonLayer } from '@deck.gl/layers';
import type { ParsedNetwork } from '../App';

export function buildNetworkLayer(parsed: ParsedNetwork) {
  const lanePaths = new PathLayer({
    id: 'lanes',
    data: {
      length: parsed.laneCount,
      startIndices: parsed.laneStarts,
      attributes: {
        getPath: { value: parsed.lanePositions, size: 2 },
      },
    },
    _pathType: 'open',
    widthUnits: 'meters',
    widthScale: 1,
    widthMinPixels: 1,
    // getWidth must be a per-path accessor, not a binary attribute:
    // PathLayer instances internally at the segment level (N-1 segments per path),
    // so a binary Float32Array in data.attributes would need one value per segment.
    getWidth: (_: unknown, { index }: { index: number }) => parsed.laneWidths[index],
    getColor: [160, 160, 160],
    pickable: true,
  });

  const junctionPolygons = new SolidPolygonLayer({
    id: 'junctions',
    data: {
      length: parsed.junctionCount,
      startIndices: parsed.junctionStarts,
      attributes: { getPolygon: { value: parsed.junctionPositions, size: 2 } },
    },
    _normalize: false,
    getFillColor: [100, 100, 100],
    pickable: true,
  });

  return [lanePaths, junctionPolygons];
}
