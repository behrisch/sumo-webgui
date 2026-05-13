// Registry of available vehicle shapes. Add new entries here to expose them in the UI.
//
// Coordinate system for SUMO car polygons (GUIBaseVehicleHelper):
//   glRotated(90) + glScaled(length, width) maps polygon (px, py) to screen via:
//     screen_x = -py * width,  screen_y = px * length
//   px=0 = front bumper, px=1 = rear, py>0 = left, py<0 = right.

export type VehicleShape = 'circle' | 'triangle' | 'car';

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
  const cx = source[1];
  const cy = -source[0];
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
  _fillPosColor(pos, col, idx, _CAR_WINDSHIELD, 0);
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
