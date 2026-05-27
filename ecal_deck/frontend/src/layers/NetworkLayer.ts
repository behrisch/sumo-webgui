import { PathLayer, SolidPolygonLayer, IconLayer } from '@deck.gl/layers';
import { PathStyleExtension } from '@deck.gl/extensions';
import type { ParsedNetwork } from '../App';
import { ARROW_ATLAS, ARROW_MAPPING, directionBitsToIcons } from './arrowShapes';

// Lane permission class colour lookup
// 0 = pedestrian/other  →  medium grey (unchanged)
// 1 = bicycle only      →  red-ish
// 2 = motorised         →  dark grey
// 3 = rail              →  rendered specially via buildRailLayer (not used here)
const LANE_PERM_COLORS: [number, number, number, number][] = [
  [160, 160, 160, 255], // 0 pedestrian
  [192,  66,  44, 255], // 1 bike
  [100, 100, 100, 255], // 2 motorised
  [ 70,  55,  45, 255], // 3 rail (fallback only — rail uses dedicated layer)
  [100, 100, 100, 255], // 4 embedded rail (drivable; rails overlaid by rail layer)
];

// Internal junction connectors render in the motorised colour so lane-flow
// continues visually through intersections.
const INTERNAL_LANE_COLOR: [number, number, number, number] = [100, 100, 100, 255];

// Slice the shared lane geometry arrays into a contiguous per-feature payload for
// the subset of lanes matching `predicate`. Returns null if no lane matches so the
// caller can skip layer construction entirely. `indices` retains the original lane
// index per output slot so callers can look up parallel per-lane arrays (perm
// class, arrow dirs, …).
function sliceLanes(
  parsed: ParsedNetwork,
  predicate: (li: number) => boolean,
): {
  count: number;
  starts: Uint32Array;
  positions: Float64Array;
  widths: Float32Array;
  indices: Uint32Array;
} | null {
  const totalPts = parsed.lanePositions.length / 2;
  const picked: number[] = [];
  let pointCount = 0;
  for (let li = 0; li < parsed.laneCount; li++) {
    if (!predicate(li)) continue;
    picked.push(li);
    const ptS = parsed.laneStarts[li];
    const ptE = li + 1 < parsed.laneCount ? parsed.laneStarts[li + 1] : totalPts;
    pointCount += ptE - ptS;
  }
  if (picked.length === 0) return null;

  const starts    = new Uint32Array(picked.length + 1);
  const positions = new Float64Array(pointCount * 2);
  const widths    = new Float32Array(picked.length);
  const indices   = new Uint32Array(picked);
  let off = 0;
  for (let i = 0; i < picked.length; i++) {
    starts[i] = off;
    const li = picked[i];
    const ptS = parsed.laneStarts[li];
    const ptE = li + 1 < parsed.laneCount ? parsed.laneStarts[li + 1] : totalPts;
    for (let p = ptS; p < ptE; p++) {
      positions[off * 2]     = parsed.lanePositions[p * 2];
      positions[off * 2 + 1] = parsed.lanePositions[p * 2 + 1];
      off++;
    }
    widths[i] = parsed.laneWidths[li];
  }
  starts[picked.length] = off;
  return { count: picked.length, starts, positions, widths, indices };
}

export function buildNetworkLayer(parsed: ParsedNetwork) {
  // Main lane PathLayer takes normal (0) and internal-connector (1) lanes.
  // Crossings (2) get their own zebra layer (buildCrossingLayer); walking
  // areas (3) are not rendered at all — see comment block above
  // buildCrossingLayer for why their geometry can't be reused directly.
  // Rail-only lanes (perm_cls=3) get their own dedicated layer
  // (buildRailLayer) with two parallel rails + sleepers, so exclude them
  // here. Embedded rail (perm_cls=4) IS drawn here as a normal road surface,
  // with the rails overlaid by buildRailLayer.
  const fn = parsed.laneFunction;
  const pc = parsed.lanePermClass;
  const drivable = sliceLanes(parsed, (li) => {
    if (fn && fn[li] > 1) return false;
    if (pc && pc[li] === 3) return false;
    return true;
  });

  const lanePaths = drivable ? new PathLayer({
    id: 'lanes',
    data: {
      length: drivable.count,
      startIndices: drivable.starts,
      attributes: {
        getPath: { value: drivable.positions, size: 2 },
      },
    },
    _pathType: 'open',
    widthUnits: 'meters',
    widthScale: 1,
    widthMinPixels: 1,
    // Internal junction connectors render at half lane width so they remain
    // visually distinguishable from real lanes at every zoom.
    getWidth: (_: unknown, { index }: { index: number }) => {
      const li = drivable.indices[index];
      const w  = drivable.widths[index];
      return fn && fn[li] === 1 ? w * 0.5 : w;
    },
    getColor: (_: unknown, { index }: { index: number }) => {
      const li = drivable.indices[index];
      if (fn && fn[li] === 1) return INTERNAL_LANE_COLOR;
      return LANE_PERM_COLORS[parsed.lanePermClass?.[li] ?? 2];
    },
    pickable: true,
  }) : null;

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

  // Index map: lane PathLayer's local index → original `parsed` lane index.
  // null when no filtering happened (no laneFunction info). Click handlers use
  // this to translate PickingInfo.index back to the global lane index.
  const laneIndexMap: Uint32Array | null = drivable && fn ? drivable.indices : null;

  return [lanePaths, junctionPolygons, laneIndexMap] as const;
}

// ---------------------------------------------------------------------------
// Lane markings
// ---------------------------------------------------------------------------

export function buildMarkingLayer(parsed: ParsedNetwork) {
  const solidCount  = parsed.solidMarkingStarts.length - 1;
  const dashedCount = parsed.dashedMarkingStarts.length - 1;

  const solidLayer = solidCount > 0 ? new PathLayer({
    id: 'lane-markings-solid',
    data: {
      length: solidCount,
      startIndices: parsed.solidMarkingStarts,
      attributes: { getPath: { value: parsed.solidMarkingPositions, size: 2 } },
    },
    _pathType: 'open',
    widthUnits: 'meters',
    getWidth: 0.15,
    widthMinPixels: 1,
    getColor: [220, 220, 220, 200],
    pickable: false,
  }) : null;

  const dashedLayer = dashedCount > 0 ? new PathLayer({
    id: 'lane-markings-dashed',
    data: {
      length: dashedCount,
      startIndices: parsed.dashedMarkingStarts,
      attributes: { getPath: { value: parsed.dashedMarkingPositions, size: 2 } },
    },
    _pathType: 'open',
    widthUnits: 'meters',
    getWidth: 0.12,
    widthMinPixels: 1,
    getColor: [210, 210, 210, 180],
    extensions: [new PathStyleExtension({ dash: true })],
    getDashArray: [6, 4],
    dashJustified: false,
    pickable: false,
  }) : null;

  return [solidLayer, dashedLayer].filter((x) => x !== null) as PathLayer[];
}

// ---------------------------------------------------------------------------
// Turning arrows
// ---------------------------------------------------------------------------

const ARROW_ICON_NAMES = ['straight', 'left', 'right'] as const;

export function buildArrowLayer(parsed: ParsedNetwork) {
  // Build arrow instances from per-lane direction bitmasks + lane geometry.
  const positions: number[] = [];
  const iconIndices: number[] = [];
  const angles: number[]    = [];
  const sizes: number[]     = [];

  const totalPts = parsed.lanePositions.length / 2;

  for (let li = 0; li < parsed.laneCount; li++) {
    const dirs = parsed.laneArrowDirs[li];
    if (!dirs) continue;

    const ptS = parsed.laneStarts[li];
    const ptE = li + 1 < parsed.laneCount ? parsed.laneStarts[li + 1] : totalPts;
    if (ptE - ptS < 2) continue;

    const endX  = parsed.lanePositions[(ptE - 1) * 2];
    const endY  = parsed.lanePositions[(ptE - 1) * 2 + 1];
    const prevX = parsed.lanePositions[(ptE - 2) * 2];
    const prevY = parsed.lanePositions[(ptE - 2) * 2 + 1];

    let dx = endX - prevX;
    const dy = endY - prevY;

    // For geo-referenced networks the coords are lon/lat degrees; correct dx for
    // longitude compression so the heading angle is accurate.
    if (parsed.geoReferenced) {
      dx *= Math.cos(endY * Math.PI / 180);
    }

    // SUMO angle convention: clockwise from north (north = dy>0).
    // deck.gl IconLayer getAngle: counterclockwise from north → negate.
    const sumoAngle = Math.atan2(dx, dy) * 180 / Math.PI;
    const deckAngle = -sumoAngle;

    const iconNames = directionBitsToIcons(dirs);
    // icon size in metres — scaled to lane width so arrows fill the lane visually
    const size = parsed.laneWidths[li] * 1.4;

    for (const name of iconNames) {
      const idx = ARROW_ICON_NAMES.indexOf(name as typeof ARROW_ICON_NAMES[number]);
      if (idx < 0) continue;
      positions.push(endX, endY);
      iconIndices.push(idx);
      angles.push(deckAngle);
      sizes.push(size);
    }
  }

  if (positions.length === 0) return null;

  const posArr   = new Float64Array(positions);
  const angleArr = new Float32Array(angles);
  const sizeArr  = new Float32Array(sizes);
  const idxArr   = new Uint8Array(iconIndices);

  return new IconLayer({
    id: 'arrows',
    data: {
      length: iconIndices.length,
      attributes: {
        getPosition: { value: posArr,   size: 2 },
        getAngle:    { value: angleArr, size: 1 },
        getSize:     { value: sizeArr,  size: 1 },
      },
    },
    iconAtlas: ARROW_ATLAS,
    iconMapping: ARROW_MAPPING,
    getIcon: (_: unknown, { index }: { index: number }) => ARROW_ICON_NAMES[idxArr[index]],
    getColor: [255, 255, 255, 210],
    sizeUnits: 'meters',
    sizeMinPixels: 8,
    billboard: false,
    pickable: false,
  });
}

// ---------------------------------------------------------------------------
// Walking areas — for a walking-area edge, the single lane's shape IS the
// boundary polygon (CCW vertices around the area). Render as filled polygons
// underneath the lanes layer so pedestrians appear on tinted pavement.
// ---------------------------------------------------------------------------

export function buildWalkingAreaLayer(parsed: ParsedNetwork) {
  if (!parsed.laneFunction) return null;
  const sliced = sliceLanes(parsed, (li) => parsed.laneFunction[li] === 3);
  if (!sliced) return null;

  const layer = new SolidPolygonLayer({
    id: 'walking-areas',
    data: {
      length: sliced.count,
      startIndices: sliced.starts,
      attributes: { getPolygon: { value: sliced.positions, size: 2 } },
    },
    // Walking-area lane shapes are not always explicitly closed; let deck.gl
    // close + triangulate. The polygons are simple (CCW boundary) so earcut
    // handles them without producing the spurious long triangles we feared.
    _normalize: true,
    getFillColor: [140, 140, 140, 200],
    pickable: true,
  });
  // indices: walking-area-local index → global lane index, for click handling.
  return { layer, laneIndices: sliced.indices };
}

// ---------------------------------------------------------------------------
// Crossings — render as zebra stripes. Each crossing lane has a 2-point shape
// going across the road and a width equal to the crosswalk width. A dashed
// PathLayer at lane width produces alternating bars perpendicular to the
// crossing direction, matching real-world zebra markings.
// ---------------------------------------------------------------------------

export function buildCrossingLayer(parsed: ParsedNetwork) {
  if (!parsed.laneFunction) return null;
  const sliced = sliceLanes(parsed, (li) => parsed.laneFunction[li] === 2);
  if (!sliced) return null;

  // Dash + gap are in pixels by default. Use a tight pattern so each crossing
  // shows multiple zebra stripes even at moderate zoom levels.
  const layer = new PathLayer({
    id: 'crossings',
    data: {
      length: sliced.count,
      startIndices: sliced.starts,
      attributes: { getPath: { value: sliced.positions, size: 2 } },
    },
    _pathType: 'open',
    widthUnits: 'meters',
    getWidth: (_: unknown, { index }: { index: number }) => sliced.widths[index],
    widthMinPixels: 3,
    getColor: [240, 240, 240, 235],
    extensions: [new PathStyleExtension({ dash: true })],
    getDashArray: [.5, .5],
    dashJustified: true,
    pickable: true,
  });
  return { layer, laneIndices: sliced.indices };
}


// ---------------------------------------------------------------------------
// Rail tracks — render rail / tram / subway lanes as two parallel steel rails
// over a brown sleeper bed (dashed wide path under the rails creates the
// cross-tie effect). Uses perpendicular per-vertex offset on each lane shape;
// SUMO rail lanes are usually gently curved so a simple averaged-normal offset
// is visually adequate without needing a full Minkowski-style mitre.
// ---------------------------------------------------------------------------

// Standard track gauge in metres. Real rail spacing — used for the offset
// distance between the two parallel rails. Sleepers extend somewhat beyond
// the rails so the bed appears wider than the gauge itself.
const RAIL_GAUGE_M       = 1.435;
const RAIL_HALF_GAUGE_M  = RAIL_GAUGE_M / 2;
const SLEEPER_WIDTH_M    = 2.0;
const RAIL_LINE_WIDTH_M  = 0.18;

// Build offset polyline by displacing each vertex along its averaged normal.
// For each interior vertex i, the normal is the average of the perpendiculars
// of the incoming and outgoing segments (then renormalised). Endpoint normals
// are simply the perpendicular of the adjacent segment.
//
// `dxScale` accounts for longitude compression on geo-referenced networks so
// the offset distance is metres in both X and Y. For ortho networks it's 1.
function offsetPolyline(
  src: Float64Array | Float32Array,
  start: number,
  end: number,
  distance: number,
  dxScale: number,
  metersPerDegLat: number,
): Float64Array {
  const n = end - start;
  const out = new Float64Array(n * 2);
  if (n < 2) return out;
  // Convert metre offset to (x, y) coordinate units. For ortho dxScale=1 and
  // metersPerDegLat is also 1 → unit-for-unit metres.
  const xScale = 1 / (dxScale * metersPerDegLat);
  const yScale = 1 / metersPerDegLat;
  // Pre-compute per-segment unit normals (perpendicular to direction).
  const nx = new Float64Array(n - 1);
  const ny = new Float64Array(n - 1);
  for (let i = 0; i < n - 1; i++) {
    const ax = src[(start + i)     * 2];
    const ay = src[(start + i)     * 2 + 1];
    const bx = src[(start + i + 1) * 2];
    const by = src[(start + i + 1) * 2 + 1];
    const dx = (bx - ax) * dxScale;
    const dy = by - ay;
    const len = Math.hypot(dx, dy);
    if (len === 0) { nx[i] = 0; ny[i] = 0; continue; }
    // Perpendicular (left-hand): (-dy, dx).
    nx[i] = -dy / len;
    ny[i] =  dx / len;
  }
  for (let i = 0; i < n; i++) {
    const px = src[(start + i) * 2];
    const py = src[(start + i) * 2 + 1];
    let avgX: number, avgY: number;
    if (i === 0)         { avgX = nx[0];     avgY = ny[0]; }
    else if (i === n - 1){ avgX = nx[n - 2]; avgY = ny[n - 2]; }
    else                 {
      avgX = nx[i - 1] + nx[i];
      avgY = ny[i - 1] + ny[i];
      const l = Math.hypot(avgX, avgY);
      if (l > 0) { avgX /= l; avgY /= l; }
    }
    out[i * 2]     = px + avgX * distance * xScale;
    out[i * 2 + 1] = py + avgY * distance * yScale;
  }
  return out;
}

// Colours for the rail decoration. Class 3 (rail-only) uses the same dark
// grey as the motorised lane fill so the track reads as part of the network.
// Class 4 (embedded rail) sits on top of a dark-grey lane rectangle and
// inverts to a lighter shade so the sleepers + rails contrast against the
// road surface beneath.
const RAIL_COLOR_SOLID:   [number, number, number, number] = [100, 100, 100, 255];
const RAIL_COLOR_EMBEDDED:[number, number, number, number] = [200, 200, 200, 230];

export function buildRailLayer(parsed: ParsedNetwork) {
  if (!parsed.lanePermClass) return null;
  const pc = parsed.lanePermClass;
  const fn = parsed.laneFunction;

  // Build a separate sleeper + rail bundle per perm class so we can give each
  // its own colour without per-feature attribute trickery.
  const build = (cls: 3 | 4, color: [number, number, number, number], idSuffix: string) => {
    const sliced = sliceLanes(parsed, (li) =>
      pc[li] === cls && (!fn || fn[li] === 0));
    if (!sliced) return null;
    const totalPts = sliced.positions.length / 2;
    const leftPos  = new Float64Array(totalPts * 2);
    const rightPos = new Float64Array(totalPts * 2);
    const geo = parsed.geoReferenced;
    const metersPerDegLat = geo ? 111320 : 1;
    for (let f = 0; f < sliced.count; f++) {
      const s = sliced.starts[f];
      const e = sliced.starts[f + 1];
      const dxScale = geo
        ? Math.cos(sliced.positions[s * 2 + 1] * Math.PI / 180)
        : 1;
      const left  = offsetPolyline(sliced.positions, s, e, +RAIL_HALF_GAUGE_M, dxScale, metersPerDegLat);
      const right = offsetPolyline(sliced.positions, s, e, -RAIL_HALF_GAUGE_M, dxScale, metersPerDegLat);
      leftPos.set(left,  s * 2);
      rightPos.set(right, s * 2);
    }
    const sleepers = new PathLayer({
      id: `rail-sleepers-${idSuffix}`,
      data: {
        length: sliced.count,
        startIndices: sliced.starts,
        attributes: { getPath: { value: sliced.positions, size: 2 } },
      },
      _pathType: 'open',
      widthUnits: 'meters',
      getWidth: SLEEPER_WIDTH_M,
      widthMinPixels: 2,
      getColor: color,
      extensions: [new PathStyleExtension({ dash: true })],
      getDashArray: [0.3, 0.7],
      dashJustified: false,
      pickable: true,
    });
    const railStyle = {
      _pathType: 'open' as const,
      widthUnits: 'meters' as const,
      getWidth: RAIL_LINE_WIDTH_M,
      widthMinPixels: 1,
      getColor: color,
      pickable: false,
    };
    const leftRail = new PathLayer({
      id: `rail-left-${idSuffix}`,
      data: {
        length: sliced.count,
        startIndices: sliced.starts,
        attributes: { getPath: { value: leftPos, size: 2 } },
      },
      ...railStyle,
    });
    const rightRail = new PathLayer({
      id: `rail-right-${idSuffix}`,
      data: {
        length: sliced.count,
        startIndices: sliced.starts,
        attributes: { getPath: { value: rightPos, size: 2 } },
      },
      ...railStyle,
    });
    return { layers: [sleepers, leftRail, rightRail], indices: sliced.indices };
  };

  const solid    = build(3, RAIL_COLOR_SOLID,    'solid');
  const embedded = build(4, RAIL_COLOR_EMBEDDED, 'embedded');
  if (!solid && !embedded) return null;

  const layers: PathLayer[] = [];
  const sleeperLaneIndices: { [layerId: string]: Uint32Array } = {};
  if (solid) {
    layers.push(...solid.layers);
    sleeperLaneIndices['rail-sleepers-solid'] = solid.indices;
  }
  if (embedded) {
    layers.push(...embedded.layers);
    sleeperLaneIndices['rail-sleepers-embedded'] = embedded.indices;
  }
  return { layers, sleeperLaneIndices };
}
