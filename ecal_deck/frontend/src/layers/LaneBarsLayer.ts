import { LineLayer } from '@deck.gl/layers';
import type { ParsedNetwork } from '../App';
import type { TLSPhase } from '../hooks/useSimSocket';

// Combined "perpendicular bar across a lane end" layer used by both TLS
// signals and stop-controlled (non-TLS) approaches.  Both render the same
// primitive — a short line segment across a lane — and differ only in
// (a) where the endpoints come from and (b) what colour the bar gets.
// Keeping them as a single LineLayer cuts the draw / picking surface in
// half and removes a chunk of duplicated geometry code.

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
const TLS_DEFAULT_COLOR: [number, number, number, number] = [80, 80, 80, 255];
const STOPLINE_COLOR:    [number, number, number, number] = [240, 240, 240, 245];

// Kind discriminator stored alongside the geometry so picking can route
// the pick result back to the original SUMO object (a TLS signal vs a
// lane that has a stop line at its end).
export const BAR_KIND_TLS      = 0 as const;
export const BAR_KIND_STOPLINE = 1 as const;
export type  BarKind = typeof BAR_KIND_TLS | typeof BAR_KIND_STOPLINE;

export interface LaneBarsGeometry {
  // Interleaved [sx, sy, tx, ty] for every bar.  Length = 4 * count.
  positions: Float64Array;
  // Per-bar kind.  Length = count.
  kinds: Uint8Array;
  // Per-bar metadata.  For TLS bars this is the index into parsed.tlsEntries;
  // for stop-line bars it is the global lane index.
  meta: Uint32Array;
  // Count of TLS bars (always at the front of the arrays, followed by
  // stop-line bars).  Useful for slicing if a caller wants to render only
  // one kind without rebuilding.
  tlsCount: number;
  stopLineCount: number;
}

// Build the static portion (positions, kinds, meta) once per parsed network.
// Re-runs only when the network is reloaded.
export function buildLaneBarsGeometry(parsed: ParsedNetwork): LaneBarsGeometry {
  // --- TLS bars: positions come straight from the publisher (one segment
  // per signal, layout [sx, sy, tx, ty] per index).
  const tlsCount = parsed.tlsEntries.length;
  // --- Stop-line bars: compute a perpendicular bar at the end of each lane
  // whose destination junction is stop-controlled (priority_stop, allway_stop,
  // rail signal/crossing).  TLS-controlled approaches are intentionally
  // omitted — the TLS bar above already draws a coloured bar there.
  const slSources: number[] = [];
  const slTargets: number[] = [];
  const slLaneIdx: number[] = [];
  if (parsed.laneHasStopline) {
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

      // Half-width perpendicular offset.  In geo mode the lane width is in
      // meters but coordinates are in degrees, so we convert via the local
      // lat/lon-per-meter scaling.
      const halfW = parsed.laneWidths[li] * 0.5;
      const nDirX = -dy / len;
      const nDirY =  dxM / len;
      let nx: number, ny: number;
      if (parsed.geoReferenced) {
        nx = (nDirX * halfW) / (111_000 * lonScale);
        ny = (nDirY * halfW) / 111_000;
      } else {
        nx = nDirX * halfW;
        ny = nDirY * halfW;
      }

      slSources.push(endX - nx, endY - ny);
      slTargets.push(endX + nx, endY + ny);
      slLaneIdx.push(li);
    }
  }
  const stopLineCount = slLaneIdx.length;

  const totalCount = tlsCount + stopLineCount;
  const positions = new Float64Array(totalCount * 4);
  const kinds     = new Uint8Array (totalCount);
  const meta      = new Uint32Array(totalCount);

  // TLS first — positions copied verbatim from parsed.tlsPositions.
  for (let i = 0; i < tlsCount; i++) {
    positions[i * 4 + 0] = parsed.tlsPositions[i * 4 + 0];
    positions[i * 4 + 1] = parsed.tlsPositions[i * 4 + 1];
    positions[i * 4 + 2] = parsed.tlsPositions[i * 4 + 2];
    positions[i * 4 + 3] = parsed.tlsPositions[i * 4 + 3];
    kinds[i] = BAR_KIND_TLS;
    meta[i]  = i;
  }
  // Stop-lines appended after.
  for (let j = 0; j < stopLineCount; j++) {
    const i = tlsCount + j;
    positions[i * 4 + 0] = slSources[j * 2 + 0];
    positions[i * 4 + 1] = slSources[j * 2 + 1];
    positions[i * 4 + 2] = slTargets[j * 2 + 0];
    positions[i * 4 + 3] = slTargets[j * 2 + 1];
    kinds[i] = BAR_KIND_STOPLINE;
    meta[i]  = slLaneIdx[j];
  }

  return { positions, kinds, meta, tlsCount, stopLineCount };
}

export interface BuildLaneBarsLayerOptions {
  geometry: LaneBarsGeometry;
  lights: TLSPhase[];
  tlsEntries: ParsedNetwork['tlsEntries'];
}

// Build the deck.gl layer.  Re-create on every TLS phase update (geometry
// stays stable across updates).  A single visibility flag in the caller
// controls whether this layer is built at all — both TLS signals and
// stop-line bars share the same toggle.
export function buildLaneBarsLayer({
  geometry, lights, tlsEntries,
}: BuildLaneBarsLayerOptions): LineLayer | null {
  const { positions, kinds, tlsCount, stopLineCount } = geometry;
  const dataLen = tlsCount + stopLineCount;
  if (dataLen === 0) return null;

  // Build state lookup for TLS signals.
  const stateMap: Record<string, string> = {};
  for (const phase of lights) stateMap[phase.id] = phase.state;

  return new LineLayer({
    id: 'lane-bars',
    data: { length: dataLen },
    getSourcePosition: (_: unknown, { index }: { index: number }) =>
      [positions[index * 4 + 0], positions[index * 4 + 1]],
    getTargetPosition: (_: unknown, { index }: { index: number }) =>
      [positions[index * 4 + 2], positions[index * 4 + 3]],
    getColor: (_: unknown, { index }: { index: number }) => {
      if (kinds[index] === BAR_KIND_TLS) {
        // TLS bars sit at the front of the geometry, so meta == index.
        const entry = tlsEntries[index];
        if (!entry) return TLS_DEFAULT_COLOR;
        const state = stateMap[entry.tls];
        if (!state) return TLS_DEFAULT_COLOR;
        return SIGNAL_COLORS[state[entry.tl_index] ?? 'u'] ?? TLS_DEFAULT_COLOR;
      }
      return STOPLINE_COLOR;
    },
    // Use the wider stop-line stroke for both kinds — TLS bars used to be
    // thinner (3) but the merged layer renders them uniformly at width 5
    // so signals stay clearly readable across zoom levels.
    getWidth: 5,
    widthMinPixels: 3,
    updateTriggers: { getColor: [lights] },
    pickable: true,
  });
}
