import { LineLayer } from '@deck.gl/layers';
import type { ParsedNetwork } from '../App';

// Render a perpendicular white bar at the end of each lane whose destination
// junction is stop-controlled (priority_stop, allway_stop, rail signal/crossing).
// TLS-controlled approaches are intentionally omitted — the runtime TLSLayer
// already draws coloured bars there.
export function buildStopLineLayer(parsed: ParsedNetwork) {
  if (!parsed.laneHasStopline) return null;

  const sources: number[] = [];
  const targets: number[] = [];
  const laneIdx: number[] = [];
  const totalPts = parsed.lanePositions.length / 2;

  for (let li = 0; li < parsed.laneCount; li++) {
    if (!parsed.laneHasStopline[li]) continue;

    const ptS = parsed.laneStarts[li];
    const ptE = li + 1 < parsed.laneCount ? parsed.laneStarts[li + 1] : totalPts;
    if (ptE - ptS < 2) continue;

    const endX  = parsed.lanePositions[(ptE - 1) * 2];
    const endY  = parsed.lanePositions[(ptE - 1) * 2 + 1];
    const prevX = parsed.lanePositions[(ptE - 2) * 2];
    const prevY = parsed.lanePositions[(ptE - 2) * 2 + 1];

    // Direction vector (corrected for longitude compression in geo mode so
    // the perpendicular comes out square on the map).
    const dx = endX - prevX;
    const dy = endY - prevY;
    const lonScale = parsed.geoReferenced ? Math.cos(endY * Math.PI / 180) : 1;
    const dxM = dx * lonScale;
    const len = Math.hypot(dxM, dy);
    if (len < 1e-9) continue;

    // Unit perpendicular times half-width. In geo mode `dxM`/`dy` are in
    // degrees, so `-dy / len` and `dxM / len` are dimensionless direction
    // components. To express the offset in meters we need to first scale by
    // half-width (meters), then convert back to degrees: 1° lat ≈ 111_000 m,
    // 1° lon ≈ 111_000 * cos(lat) m. In ortho mode coordinates are already
    // meters so no conversion is needed.
    const halfW = parsed.laneWidths[li] * 0.5;
    const nDirX = -dy / len;  // dimensionless east component of unit perp
    const nDirY =  dxM / len; // dimensionless north component of unit perp
    let nx: number, ny: number;
    if (parsed.geoReferenced) {
      nx = (nDirX * halfW) / (111_000 * lonScale); // m east → deg lon
      ny = (nDirY * halfW) / 111_000;              // m north → deg lat
    } else {
      nx = nDirX * halfW;
      ny = nDirY * halfW;
    }

    sources.push(endX - nx, endY - ny);
    targets.push(endX + nx, endY + ny);
    laneIdx.push(li);
  }

  if (sources.length === 0) return null;

  const srcArr = new Float64Array(sources);
  const tgtArr = new Float64Array(targets);
  const laneIndices = new Uint32Array(laneIdx);

  const layer = new LineLayer({
    id: 'stop-lines',
    data: { length: sources.length / 2 },
    getSourcePosition: (_: unknown, { index }: { index: number }) =>
      [srcArr[index * 2], srcArr[index * 2 + 1]],
    getTargetPosition: (_: unknown, { index }: { index: number }) =>
      [tgtArr[index * 2], tgtArr[index * 2 + 1]],
    getColor: [240, 240, 240, 245],
    getWidth: 5,
    widthMinPixels: 3,
    pickable: true,
  });
  return { layer, laneIndices };
}
