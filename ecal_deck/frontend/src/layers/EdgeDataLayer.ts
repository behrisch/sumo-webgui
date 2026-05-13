import { PathLayer } from '@deck.gl/layers';
import type { ParsedNetwork } from '../App';
import type { EdgeValueMap } from '../hooks/useSimSocket';
import { colormap } from '../utils/colormap';

export function buildEdgeDataLayer(
  parsed: ParsedNetwork,
  baselineMap: EdgeValueMap,  // all edges from last full snapshot
  deltaMap: EdgeValueMap,     // currently-occupied edges (overrides baseline)
  colorAttr: string,
  vpBounds: [number, number, number, number],  // [minX, minY, maxX, maxY] in network coords
) {
  const [vpX0, vpY0, vpX1, vpY1] = vpBounds;
  const bboxes = parsed.laneBBoxes;
  const totalSrcPts = parsed.lanePositions.length / 2;

  // Merge: iterate baseline (all edges), override with delta where present.
  // This means unoccupied edges show their last-known baseline values while
  // currently-occupied edges show live values — no stale colors linger.
  let min = Infinity, max = -Infinity;
  const visLanes: number[] = [];
  const visVals: number[]  = [];

  for (const [edgeId, baseAttrs] of baselineMap) {
    const attrs = deltaMap.get(edgeId) ?? baseAttrs;
    const val = attrs[colorAttr];
    if (val === undefined) continue;
    if (val < min) min = val;
    if (val > max) max = val;

    const ei = parsed.edgeIdToIndex.get(edgeId);
    if (ei === undefined) continue;
    const lanes = parsed.edgeLanesByIdx[ei];
    if (!lanes) continue;
    for (const li of lanes) {
      const b = li * 4;
      if (bboxes[b + 2] >= vpX0 && bboxes[b] <= vpX1 &&
          bboxes[b + 3] >= vpY0 && bboxes[b + 1] <= vpY1) {
        visLanes.push(li);
        visVals.push(val);
      }
    }
  }

  if (visLanes.length === 0) return null;
  const range = max - min || 1;

  // Count total positions for the visible subset
  let visPts = 0;
  for (const li of visLanes) {
    visPts += (li + 1 < parsed.laneCount ? parsed.laneStarts[li + 1] : totalSrcPts)
              - parsed.laneStarts[li];
  }

  // Build filtered typed arrays
  const starts    = new Uint32Array(visLanes.length);
  const positions = new Float64Array(visPts * 2);
  const widths    = new Float32Array(visLanes.length);
  const colors    = new Uint8Array(visLanes.length * 4);

  let posOff = 0;
  for (let j = 0; j < visLanes.length; j++) {
    const li  = visLanes[j];
    const ptS = parsed.laneStarts[li];
    const ptE = li + 1 < parsed.laneCount ? parsed.laneStarts[li + 1] : totalSrcPts;
    const nPts = ptE - ptS;

    starts[j] = posOff;
    positions.set(parsed.lanePositions.subarray(ptS * 2, ptE * 2), posOff * 2);
    widths[j] = parsed.laneWidths[li];
    posOff += nPts;

    const [r, g, b, a] = colormap(Math.max(0, Math.min(1, (visVals[j] - min) / range)));
    colors[j * 4] = r; colors[j * 4 + 1] = g; colors[j * 4 + 2] = b; colors[j * 4 + 3] = a;
  }

  return new PathLayer({
    id: 'edgedata',
    data: {
      length: visLanes.length,
      startIndices: starts,
      attributes: { getPath: { value: positions, size: 2 } },
    },
    _pathType: 'open',
    widthUnits: 'meters',
    widthScale: 1,
    widthMinPixels: 2,
    getColor: ((_: unknown, { index, target }: { index: number; target: number[] }) => {
      const off = index * 4;
      target[0] = colors[off]; target[1] = colors[off + 1];
      target[2] = colors[off + 2]; target[3] = colors[off + 3];
      return target;
    }) as any, // eslint-disable-line @typescript-eslint/no-explicit-any
    getWidth: (_: unknown, { index }: { index: number }) => widths[index],
    pickable: false,
  });
}
