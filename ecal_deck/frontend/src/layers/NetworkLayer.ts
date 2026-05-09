import { PathLayer, SolidPolygonLayer } from '@deck.gl/layers';
import type { ParsedNetwork } from '../App';

export function buildNetworkLayer(parsed: ParsedNetwork) {
  const edgePaths = new PathLayer({
    id: 'edges',
    data: {
      length: parsed.edgeCount,
      startIndices: parsed.edgeStarts,
      attributes: { getPath: { value: parsed.edgePositions, size: 2 } },
    },
    _pathType: 'open',
    widthMinPixels: 1,
    getWidth: 2,
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

  return [edgePaths, junctionPolygons];
}
