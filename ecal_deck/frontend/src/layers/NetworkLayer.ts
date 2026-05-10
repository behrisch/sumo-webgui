import { PathLayer, SolidPolygonLayer, IconLayer } from '@deck.gl/layers';
import { PathStyleExtension } from '@deck.gl/extensions';
import type { ParsedNetwork } from '../App';
import { ARROW_ATLAS, ARROW_MAPPING, directionBitsToIcons } from './arrowShapes';

// Lane permission class colour lookup
// 0 = pedestrian/other  →  medium grey (unchanged)
// 1 = bicycle only      →  red-ish
// 2 = motorised         →  dark grey
const LANE_PERM_COLORS: [number, number, number, number][] = [
  [160, 160, 160, 255], // 0 pedestrian
  [192,  66,  44, 255], // 1 bike
  [100, 100, 100, 255], // 2 motorised
];

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
    getColor: (_: unknown, { index }: { index: number }) =>
      LANE_PERM_COLORS[parsed.lanePermClass?.[index] ?? 2],
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

  return [solidLayer, dashedLayer].filter(Boolean);
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

