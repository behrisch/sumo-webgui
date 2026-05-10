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
// Current implementation: single IconLayer, windshield punched out via fill-rule=evenodd.

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
