// Registry of available vehicle shapes. Add new entries here to expose them in the UI.
//
// Coordinate system for SUMO car polygons (GUIBaseVehicleHelper):
//   glRotated(90) + glScaled(length, width) maps polygon (px, py) to screen via:
//     screen_x = -py * width,  screen_y = px * length
//   px=0 = front bumper, px=1 = rear, py>0 = left, py<0 = right.
//
// SVG transform for each 64×64 icon cell in the 64×256 atlas:
//   SVG_x = 32 − py × scale_x      (scale_x encodes the vehicle class's width/length ratio)
//   SVG_y = y_offset + 4 + px × 56  (length axis: 56 px per unit → body = 87% of cell height)
//
// getSize is set per-vehicle to length × (64/56) so the rendered body height exactly equals
// the vehicle's actual length in metres.  Width follows from the atlas's baked aspect ratio.
//
// Multi-color icon research: mask:true uses only the alpha channel, so luminance-based
// coloring within a single icon is not possible.  Options for future improvement:
//  - 2-3 stacked IconLayers (body + lighter front + dark windows)
//  - Custom deck.gl shader extension (encode masks in separate RGB channels)
//  - SolidPolygonLayer (pixel-perfect, but needs CPU vertex transform per frame)
// Current implementation: SimpleMeshLayer with vertex colors — black windshield vertices
// stay black regardless of per-instance getColor; white body vertices take the tint.

export type VehicleShape = 'circle' | 'triangle' | 'car';

export interface IconShapeConfig {
  atlas: string;
  mapping: Record<string, { x: number; y: number; width: number; height: number; anchorX: number; anchorY: number; mask: true }>;
  icon: string;
}

const makeAtlasSVG = (width: number, height: number, inner: string) =>
  'data:image/svg+xml,' + encodeURIComponent(
    `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}">${inner}</svg>`,
  );

// anchorX/Y: the point in the atlas that aligns with the vehicle's reported position.
// deck.gl anchorX/Y are CELL-RELATIVE (0…width, 0…height), NOT atlas-absolute.
// Default is width/2, height/2 (cell centre). Front bumper sits at y=4 within each cell.
const frontMapping = (icon: string, x: number, y: number, w = 64, h = 64): IconShapeConfig['mapping'] => ({
  [icon]: { x, y, width: w, height: h, anchorX: w / 2, anchorY: 4, mask: true },
});
// Centre anchor for shapes that have no directional front (circle override mode).
const centerMapping = (icon: string, x: number, y: number, w = 64, h = 64): IconShapeConfig['mapping'] => ({
  [icon]: { x, y, width: w, height: h, anchorX: w / 2, anchorY: h / 2, mask: true },
});

// ---------------------------------------------------------------------------
// Multi-shape atlas (64 × 256, four 64×64 icon cells stacked vertically)
// ---------------------------------------------------------------------------
//
// Cell 0 (y=0..63):   'car'        — passenger / sedan / hatchback / wagon / van / taxi
// Cell 1 (y=64..127): 'truck'      — truck / bus / delivery / rail (long narrow body)
// Cell 2 (y=128..191):'cyclist'    — bicycle / moped / motorcycle / scooter
// Cell 3 (y=192..255):'pedestrian' — pedestrian (circle)
//
// Each body is 56 px tall (px 0→1 maps to y_offset+4 → y_offset+60).
// Width is scaled per class so the baked aspect ratio matches typical real dimensions.
//
// ── Car (vehiclePoly_PassengerCarBody + vehiclePoly_PassengerFrontGlass, y_offset=0) ──
// scale_x = 24  (typical 4.3m×1.8m → ratio 2.39 ≈ 56/24 = 2.33)
// Body outline (hull + windshield punched out via fill-rule=evenodd):
const CAR_BODY      = 'M32,4 L24.8,4 L21.4,8.5 L20,18 L20,57.2 L22.4,60 L41.6,60 L44,57.2 L44,18 L42.6,8.5 L39.2,4 Z';
const CAR_WINDSHIELD = 'M32,20.8 L22.4,20.8 L24.8,28.1 L39.2,28.1 L41.6,20.8 Z';
const CAR_SVG = `<path fill-rule="evenodd" d="${CAR_BODY} ${CAR_WINDSHIELD}" fill="white"/>`;
//
// ── Truck / Bus (y_offset=64) ──
// scale_x = 13  (typical 12.5m×2.6m → ratio 4.8 ≈ 56/13 = 4.3)
// Simple rectangle body; windshield strip punched out at front.
const TRUCK_BODY      = 'M25,68 L25,124 L39,124 L39,68 Z';
const TRUCK_WINDSHIELD = 'M26,69 L26,77 L38,77 L38,69 Z';
const TRUCK_SVG = `<path fill-rule="evenodd" d="${TRUCK_BODY} ${TRUCK_WINDSHIELD}" fill="white"/>`;
//
// ── Cyclist / vehiclePoly_Cyclist (y_offset=128) ──
// scale_x = 24  (bicycle 1.6m×0.65m, motorcycle 2.2m×0.8m — similar to car ratio)
// Polygon spans px=0.25..0.8 (shoulders/handlebars at front, narrow rear).
// NOT rescaled — original SUMO px coordinates preserved so the 14 px gap between
// anchorY (y=132, px=0) and the drawn body (y=146, px=0.25) correctly represents
// the 25% of vehicle length ahead of the drawn handlebar area.
// SVG_y = 132 + px × 56.  Original (px,py): .25,.45  .25,.5  .8,.15  .8,−.15  .25,−.5  .25,−.45
const CYCLIST_SVG = '<polygon points="21.2,146 20,146 28.4,176.8 35.6,176.8 44,146 42.8,146" fill="white"/>';
//
// ── Pedestrian (y_offset=192) ──
const PEDESTRIAN_SVG = '<circle cx="32" cy="224" r="22" fill="white"/>';

const MULTI_ATLAS_SVG = CAR_SVG + TRUCK_SVG + CYCLIST_SVG + PEDESTRIAN_SVG;

// Icon names within the multi-shape atlas.
export const ICON_NAMES = ['car', 'truck', 'cyclist', 'pedestrian'] as const;
export type IconName = typeof ICON_NAMES[number];

// Full atlas config used by the 'car' (auto per-type) rendering mode.
export const MULTI_SHAPE_ATLAS: Pick<IconShapeConfig, 'atlas' | 'mapping'> = {
  atlas: makeAtlasSVG(64, 256, MULTI_ATLAS_SVG),
  mapping: {
    ...frontMapping('car',        0,   0),
    ...frontMapping('truck',      0,  64),
    ...frontMapping('cyclist',    0, 128),
    ...frontMapping('pedestrian', 0, 192),
  },
};

// Map SUMO gui_shape strings (traci.vehicletype.getShapeClass) to icon index.
// Index corresponds to ICON_NAMES: 0=car, 1=truck, 2=cyclist, 3=pedestrian.
export function guiShapeToIconIndex(shape: string): number {
  if (!shape) return 0;
  const s = shape.toLowerCase();
  if (s === 'pedestrian')                            return 3;
  if (s === 'bicycle' || s === 'moped' ||
      s === 'motorcycle' || s === 'scooter' ||
      s === 'ant')                                   return 2;
  if (s.startsWith('truck') || s.startsWith('bus') ||
      s.startsWith('rail')  || s === 'delivery' ||
      s === 'ship')                                  return 1;
  return 0; // passenger / taxi / evehicle / unknown / everything else
}

// getSize scale factor: makes the 56/64-height body span exactly vehicle.length metres.
export const SIZE_SCALE = 64 / 56;

// ---------------------------------------------------------------------------
// Single-shape configs for 'circle' and 'triangle' override modes
// ---------------------------------------------------------------------------
export const VEHICLE_ICON_SHAPES: Record<Exclude<VehicleShape, 'car'>, IconShapeConfig> = {
  circle: {
    atlas:   makeAtlasSVG(64, 64, '<circle cx="32" cy="32" r="24" fill="white"/>'),
    mapping: centerMapping('circle', 0, 0),
    icon:    'circle',
  },
  triangle: {
    atlas:   makeAtlasSVG(64, 64, '<path d="M32,4 L58,56 L32,42 L6,56Z" fill="white"/>'),
    mapping: frontMapping('arrow', 0, 0),
    icon:    'arrow',
  },
};

export const VEHICLE_SHAPES: VehicleShape[] = ['circle', 'triangle', 'car'];

// ---------------------------------------------------------------------------
// Mesh geometries for SimpleMeshLayer
// ---------------------------------------------------------------------------
//
// Mesh coordinate system (local, before getScale/getOrientation):
//   X = py (SUMO width axis): +X = vehicle's left,  −X = right
//   Y = −px (negated SUMO length axis): Y=0 = front bumper (anchor = getPosition),
//       Y=−1 = rear  →  after getScale Y spans [−length_m, 0]
//   Z = up
// getScale: [width_m, length_m, 1]
// getOrientation: [pitch=0, yaw=−sumo_angle_deg, roll=0]
//   SUMO angle is CW from north (deg); negating gives CCW-from-north = deck.gl yaw.
//   yaw=0 → vehicle pointing north (+Y world); yaw=−90 → pointing east (+X world).
//
// Vertex colors:
//   White (255,255,255) → multiplied by per-instance getColor → body tint
//   Black (0,0,0)       → result always (0,0,0) → windshield stays dark
function _fillPosColor(pos: Float32Array, col: Uint8Array, idx: number, source: Float32Array, color: number) {
  let cx = source[1];
  let cy = -source[0];
  let px = source[3];
  let py = -source[2];
  for (let i = 2; i < source.length / 2; i++) {
    let cidx = idx / 3 * 4;
    pos[idx++] = cx; pos[idx++] = cy; pos[idx++] = 0;
    col[cidx++] = color; col[cidx++] = color; col[cidx++] = color; cidx++;
    pos[idx++] = px; pos[idx++] = py; pos[idx++] = 0;
    col[cidx++] = color; col[cidx++] = color; col[cidx++] = color; cidx++;
    px = source[2 * i + 1]; py = -source[2 * i];
    pos[idx++] = px; pos[idx++] = py; pos[idx++] = 0;
    col[cidx++] = color; col[cidx++] = color; col[cidx++] = color; cidx++;
  }
  return idx;
}
// Car: rectangular body + dark windshield strip at the front quarter
const _CAR_BODY = new Float32Array([.5, 0,  0, 0,  0, .3,  0.08, .44,  0.25, .5,  0.95, .5,  1., .4,  1., -.4,  0.95, -.5,  0.25, -.5,  0.08, -.44,  0, -.3,  0, 0]);
const _CAR_BODY_FRONT = new Float32Array([0.1, 0,  0.025, 0,  0.025, 0.25,  0.27, 0.4,  0.27, -.4,  0.025, -0.25,  0.025, 0]);
const _CAR_WINDSHIELD = new Float32Array([0.35, 0,  0.3, 0,  0.3, 0.4,  0.43, 0.3,  0.43, -0.3,  0.3, -0.4,  0.3, 0]);
function _makeCarMesh() {
  const pos = new Float32Array((_CAR_BODY.length/2 - 2 + _CAR_BODY_FRONT.length/2 - 2 + _CAR_WINDSHIELD.length/2 - 2) * 3 * 3);
  const col = new Uint8Array(pos.length / 3 * 4).fill(255);
  let idx = _fillPosColor(pos, col, 0, _CAR_BODY, 255);
  idx = _fillPosColor(pos, col, idx, _CAR_BODY_FRONT, 128);
  idx = _fillPosColor(pos, col, idx, _CAR_WINDSHIELD, 0);
  return {
    attributes: {
      positions: { value: pos, size: 3 },
      colors:    { value: col, size: 4, normalized: true },
    },
  };
}
export const CAR_MESH = _makeCarMesh();

// Triangle: forward-pointing arrow
const _TRI_POS = new Float32Array([
   0.0,  0.0, 0,   // tip at front bumper
  -0.5, -1.0, 0,   // rear left  (scaled → −length_m)
   0.5, -1.0, 0,   // rear right
]);
const _TRI_COL = new Uint8Array([255,255,255,255, 255,255,255,255, 255,255,255,255]);
export const TRIANGLE_MESH = {
  attributes: {
    positions: { value: _TRI_POS, size: 3 },
    colors:    { value: _TRI_COL, size: 4, normalized: true },
  },
};

// Circle: 16-segment fan, radius 0.5, all white
function _makeCircleMesh(segments: number) {
  const pos = new Float32Array(segments * 3 * 3);
  const col = new Uint8Array(segments * 3 * 4).fill(255);
  for (let i = 0; i < segments; i++) {
    const a0 = (i / segments) * 2 * Math.PI;
    const a1 = ((i + 1) / segments) * 2 * Math.PI;
    const b = i * 9;
    pos[b]   = 0; pos[b+1] = 0; pos[b+2] = 0;
    pos[b+3] = 0.5 * Math.cos(a0); pos[b+4] = 0.5 * Math.sin(a0); pos[b+5] = 0;
    pos[b+6] = 0.5 * Math.cos(a1); pos[b+7] = 0.5 * Math.sin(a1); pos[b+8] = 0;
  }
  return {
    attributes: {
      positions: { value: pos, size: 3 },
      colors:    { value: col, size: 4, normalized: true },
    },
  };
}
export const CIRCLE_MESH = _makeCircleMesh(16);
