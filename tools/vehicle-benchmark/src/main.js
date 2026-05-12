import { Deck, OrthographicView } from '@deck.gl/core';
import { IconLayer, SolidPolygonLayer } from '@deck.gl/layers';
import { SimpleMeshLayer } from '@deck.gl/mesh-layers';

// ---------------------------------------------------------------------------
// Vehicle shape atlas — copied from ecal_deck/frontend/src/layers/vehicleShapes.ts
// ---------------------------------------------------------------------------
const makeAtlasSVG = (w, h, inner) =>
  'data:image/svg+xml,' + encodeURIComponent(
    `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}">${inner}</svg>`);

const CAR_BODY       = 'M32,4 L24.8,4 L21.4,8.5 L20,18 L20,57.2 L22.4,60 L41.6,60 L44,57.2 L44,18 L42.6,8.5 L39.2,4 Z';
const CAR_WIND       = 'M32,20.8 L22.4,20.8 L24.8,28.1 L39.2,28.1 L41.6,20.8 Z';
const TRUCK_BODY     = 'M25,68 L25,124 L39,124 L39,68 Z';
const TRUCK_WIND     = 'M26,69 L26,77 L38,77 L38,69 Z';
const CYCLIST_SVG    = '<polygon points="21.2,146 20,146 28.4,176.8 35.6,176.8 44,146 42.8,146" fill="white"/>';
const PEDESTRIAN_SVG = '<circle cx="32" cy="224" r="22" fill="white"/>';

const ATLAS_SVG =
  `<path fill-rule="evenodd" d="${CAR_BODY} ${CAR_WIND}" fill="white"/>` +
  `<path fill-rule="evenodd" d="${TRUCK_BODY} ${TRUCK_WIND}" fill="white"/>` +
  CYCLIST_SVG + PEDESTRIAN_SVG;

const ATLAS = {
  url: makeAtlasSVG(64, 256, ATLAS_SVG),
  mapping: {
    car:        { x: 0, y:   0, width: 64, height: 64, anchorX: 32, anchorY:  4, mask: true },
    truck:      { x: 0, y:  64, width: 64, height: 64, anchorX: 32, anchorY:  4, mask: true },
    cyclist:    { x: 0, y: 128, width: 64, height: 64, anchorX: 32, anchorY:  4, mask: true },
    pedestrian: { x: 0, y: 192, width: 64, height: 64, anchorX: 32, anchorY: 32, mask: true },
  },
};
const ICON_NAMES = ['car', 'truck', 'cyclist', 'pedestrian'];
const SIZE_SCALE = 64 / 56; // body occupies 56/64 of cell height

// ---------------------------------------------------------------------------
// Data generation
// ---------------------------------------------------------------------------
const WORLD = 2000;

function generateVehicles(N) {
  const objects = new Array(N);
  for (let i = 0; i < N; i++) {
    const iconIdx = Math.floor(Math.random() * 3); // car/truck/cyclist (skip pedestrian)
    objects[i] = {
      idx:      i,
      position: [(Math.random() - 0.5) * WORLD, (Math.random() - 0.5) * WORLD, 0],
      angle:    Math.random() * 360,
      length:   iconIdx === 1 ? 8 + Math.random() * 4   // truck 8–12 m
               : iconIdx === 2 ? 1.5 + Math.random()    // cyclist 1.5–2.5 m
               : 4 + Math.random() * 3,                  // car 4–7 m
      width:    iconIdx === 1 ? 2.4 + Math.random() * 0.4
               : iconIdx === 2 ? 0.6 + Math.random() * 0.2
               : 1.7 + Math.random() * 0.4,
      speed:    (iconIdx === 2 ? 3 : 5) + Math.random() * 20,
      turnRate: (Math.random() - 0.5) * 40,
      iconIdx,
    };
  }
  const positions = new Float64Array(N * 2);
  const angles    = new Float32Array(N);
  const sizes     = new Float32Array(N);
  for (let i = 0; i < N; i++) {
    positions[i * 2]     = objects[i].position[0];
    positions[i * 2 + 1] = objects[i].position[1];
    angles[i]            = objects[i].angle;
    sizes[i]             = objects[i].length * SIZE_SCALE;
  }
  return { N, objects, positions, angles, sizes };
}

const DEG2RAD = Math.PI / 180;
const HALF = WORLD / 2;

function updateVehicles(vehicles, dt) {
  const { N, objects, positions, angles, sizes } = vehicles;
  for (let i = 0; i < N; i++) {
    const o = objects[i];
    o.angle += o.turnRate * dt;
    const rad = o.angle * DEG2RAD;
    o.position[0] += o.speed * Math.cos(rad) * dt;
    o.position[1] += o.speed * Math.sin(rad) * dt;
    if (o.position[0] >  HALF) o.position[0] -= WORLD;
    if (o.position[0] < -HALF) o.position[0] += WORLD;
    if (o.position[1] >  HALF) o.position[1] -= WORLD;
    if (o.position[1] < -HALF) o.position[1] += WORLD;
    positions[i * 2]     = o.position[0];
    positions[i * 2 + 1] = o.position[1];
    angles[i]            = o.angle;
    sizes[i]             = o.length * SIZE_SCALE;
  }
}

// Speed [0,1] → green→yellow→red
function speedColor(t, out, off) {
  if (t < 0.5) {
    const s = t * 2;
    out[off]   = Math.round(s * 255);
    out[off+1] = Math.round(128 + s * 127);
    out[off+2] = Math.round(255 * (1 - s));
  } else {
    const s = (t - 0.5) * 2;
    out[off]   = 255;
    out[off+1] = Math.round(255 * (1 - s));
    out[off+2] = 0;
  }
  out[off+3] = 220;
}

function buildColors(N, frame) {
  const colors = new Uint8Array(N * 4);
  for (let i = 0; i < N; i++) {
    const t = (Math.sin(frame * 0.03 + i * 0.01) + 1) / 2;
    speedColor(t, colors, i * 4);
  }
  return colors;
}

// ---------------------------------------------------------------------------
// Rectangle mesh for SimpleMeshLayer
// White body vertices → tinted by getColor; black windshield → stays black
// ---------------------------------------------------------------------------
function makeRectMesh() {
  // Unit rectangle: width along X (-0.5…+0.5), length along Y (-0.5…+0.5)
  // Windshield strip at front quarter (y = 0.125…0.5)
  const positions = new Float32Array([
    // body (back 3/4)
    -0.5, -0.5, 0,   0.5, -0.5, 0,   0.5,  0.125, 0,
    -0.5, -0.5, 0,   0.5,  0.125, 0, -0.5,  0.125, 0,
    // windshield (front 1/4) — black vertices
    -0.4,  0.125, 0,  0.4,  0.125, 0,  0.4,  0.5, 0,
    -0.4,  0.125, 0,  0.4,  0.5, 0,  -0.4,  0.5, 0,
  ]);
  const colors = new Uint8Array([
    255,255,255,255, 255,255,255,255, 255,255,255,255,
    255,255,255,255, 255,255,255,255, 255,255,255,255,
      0,  0,  0,255,   0,  0,  0,255,   0,  0,  0,255,
      0,  0,  0,255,   0,  0,  0,255,   0,  0,  0,255,
  ]);
  return {
    attributes: {
      positions: { value: positions, size: 3 },
      colors:    { value: colors,    size: 4, normalized: true },
    },
  };
}

// ---------------------------------------------------------------------------
// Layer builders
// ---------------------------------------------------------------------------
function buildIconLayer(vehicles, colors, frame) {
  const { N, positions, angles, sizes } = vehicles;
  return new IconLayer({
    id: 'vehicles-icon',
    data: { length: N, attributes: {
      getPosition: { value: positions, size: 2 },
      getColor:    { value: colors,    size: 4, normalized: true },
      getAngle:    { value: angles,    size: 1 },
      getSize:     { value: sizes,     size: 1 },
    }},
    iconAtlas:     ATLAS.url,
    iconMapping:   ATLAS.mapping,
    getIcon:       (_, { index }) => ICON_NAMES[vehicles.objects[index].iconIdx],
    sizeUnits:     'meters',
    sizeMinPixels: 3,
    sizeMaxPixels: 200,
    updateTriggers: { getPosition: frame, getAngle: frame, getColor: frame },
  });
}

function buildMeshLayer(vehicles, colors, mesh, frame) {
  const { objects } = vehicles;
  return new SimpleMeshLayer({
    id: 'vehicles-mesh',
    data: objects,
    mesh,
    getPosition:    d => d.position,
    getOrientation: d => [0, d.angle, 0],
    getScale:       d => [d.width, d.length, 1],
    getColor:       d => {
      const o = d.idx * 4;
      return [colors[o], colors[o+1], colors[o+2], colors[o+3]];
    },
    sizeScale: 1,
    updateTriggers: { getPosition: frame, getOrientation: frame, getColor: frame },
  });
}

function buildPolyLayer(vehicles, colors, frame) {
  const { objects } = vehicles;
  return new SolidPolygonLayer({
    id: 'vehicles-poly',
    data: objects,
    getPolygon: d => {
      const [cx, cy] = d.position;
      const a = d.angle * DEG2RAD;
      const cos = Math.cos(a), sin = Math.sin(a);
      const hw = d.width / 2, hl = d.length / 2;
      return [[-hw,-hl],[hw,-hl],[hw,hl],[-hw,hl]].map(([dx, dy]) => [
        cx + dx*cos - dy*sin,
        cy + dx*sin + dy*cos,
      ]);
    },
    getFillColor: d => {
      const o = d.idx * 4;
      return [colors[o], colors[o+1], colors[o+2], colors[o+3]];
    },
    pickable: false,
    updateTriggers: { getPolygon: frame, getFillColor: frame },
  });
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
const mesh = makeRectMesh();

let mode    = 'icon';
let animate = true;
let N       = 10000;
let vehicles = generateVehicles(N);
let frame   = 0;
let colors  = buildColors(N, 0);

let lastFpsTime = performance.now();
let lastFrameTime = performance.now();
let frameCount  = 0;
let fps         = 0;
let frameMs     = 0;

const statsEl = document.getElementById('stats');

const deck = new Deck({
  parent: document.getElementById('canvas-container'),
  views: new OrthographicView({ id: 'ortho' }),
  initialViewState: { target: [0, 0, 0], zoom: 0 },
  controller: true,
  layers: [],
});

function render() {
  const now = performance.now();
  const dt  = Math.min((now - lastFrameTime) / 1000, 0.1); // cap at 100ms
  lastFrameTime = now;

  frame++;
  frameCount++;
  if (now - lastFpsTime >= 500) {
    fps     = (frameCount / ((now - lastFpsTime) / 1000)).toFixed(1);
    frameMs = ((now - lastFpsTime) / frameCount).toFixed(2);
    frameCount  = 0;
    lastFpsTime = now;
    statsEl.innerHTML =
      `fps: ${fps}<br/>frame: ${frameMs}ms<br/>vehicles: ${N}<br/>layer: ${mode}`;
  }

  updateVehicles(vehicles, dt);
  if (animate) colors = buildColors(N, frame);

  let layer;
  if (mode === 'icon')
    layer = buildIconLayer(vehicles, colors, frame);
  else if (mode === 'mesh')
    layer = buildMeshLayer(vehicles, colors, mesh, frame);
  else
    layer = buildPolyLayer(vehicles, colors, frame);

  deck.setProps({ layers: [layer] });
  requestAnimationFrame(render);
}

render();

// ---------------------------------------------------------------------------
// UI wiring
// ---------------------------------------------------------------------------
function setMode(m) {
  mode = m;
  ['icon','mesh','poly'].forEach(id =>
    document.getElementById(`btn-${id}`).classList.toggle('active', id === m));
}
document.getElementById('btn-icon').addEventListener('click', () => setMode('icon'));
document.getElementById('btn-mesh').addEventListener('click', () => setMode('mesh'));
document.getElementById('btn-poly').addEventListener('click', () => setMode('poly'));

document.getElementById('chk-animate').addEventListener('change', e => {
  animate = e.target.checked;
});

document.getElementById('sld-count').addEventListener('input', e => {
  N = Number(e.target.value);
  document.getElementById('lbl-count').textContent = N;
  vehicles = generateVehicles(N);
  colors   = buildColors(N, frame);
});

