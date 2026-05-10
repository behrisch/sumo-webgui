// SVG icon atlas for lane turning arrows.
//
// Atlas layout: 64 × 192 px, three 64×64 cells stacked vertically.
//
//   Cell 0 (y=0..63):   'straight'  — used for directions s (straight) and t (u-turn approx)
//   Cell 1 (y=64..127): 'left'      — used for directions l (left) and L (partial-left)
//   Cell 2 (y=128..191):'right'     — used for directions r (right) and R (partial-right)
//
// Icon orientation: tip/junction-side at the TOP (small y), stem at the BOTTOM (large y).
// When deck.gl rotates the icon by the lane's heading angle the arrow automatically
// aligns with the road direction.
//
// Anchor: y=8 (cell-relative) — aligns the top of each arrow with the lane's endpoint
// (the point that touches the junction boundary).  The stem extends downward into the lane.
//
// Direction bit-flags (one byte per lane from proto field lane_arrow_directions):
//   bit 0 (1)  = s (straight)    bit 1 (2)  = l (left)
//   bit 2 (4)  = r (right)       bit 3 (8)  = t (u-turn, shown as straight)
//   bit 4 (16) = L (part. left)  bit 5 (32) = R (part. right)

const makeAtlasSVG = (inner: string) =>
  'data:image/svg+xml,' + encodeURIComponent(
    `<svg xmlns="http://www.w3.org/2000/svg" width="64" height="192">${inner}</svg>`,
  );

// ── Straight arrow (cell y=0, tip at top) ───────────────────────────────────
// Stem:      x=31..33, y=36..60  (2 px wide, 24 px tall)
// Arrowhead: base at y=36 spanning x=26..38 (12 px wide), tip at (32, 8)
const STRAIGHT_SVG =
  '<polygon points="31,60 33,60 33,36 38,36 32,8 26,36 31,36" fill="white"/>';

// ── Left turn arrow (cell y=64) ─────────────────────────────────────────────
// Stem:  x=31..33, y=44..60  (2 px wide, 16 px tall — shorter vertical)
// Arm:   x=16..33, y=44..46  (2 px tall, 17 px long — shorter horizontal arm)
// Arrowhead: tip at (8,45), base from y=42 to y=48 at x=16 (6 px wide)
// AnchorY=32 (cell-relative) — places the arrowhead center at the lane endpoint.
//
// Atlas y offset = 64; cell-relative → atlas: 44→108, 46→110, 42→106, 45→109, 48→112, 60→124
const LEFT_SVG =
  '<polygon points="31,124 33,124 33,110 16,110 16,112 8,109 16,106 16,108 31,108" fill="white"/>';

// ── Right turn arrow (cell y=128) — mirror of left ──────────────────────────
// Stem:  x=31..33, y=44..60  (2 px wide, 16 px tall)
// Arm:   x=31..48, y=44..46  (2 px tall, 17 px long)
// Arrowhead: tip at (56,45), base from y=42 to y=48 at x=48 (6 px wide)
// AnchorY=32 (cell-relative)
//
// Atlas y offset = 128; 44→172, 46→174, 42→170, 45→173, 48→176, 60→188
const RIGHT_SVG =
  '<polygon points="33,188 31,188 31,174 48,174 48,176 56,173 48,170 48,172 33,172" fill="white"/>';

export const ARROW_ATLAS = makeAtlasSVG(STRAIGHT_SVG + LEFT_SVG + RIGHT_SVG);

// anchorX/Y are cell-relative (deck.gl convention).
// anchorY=8: aligns y=8 (tip region) with the lane endpoint position.
export const ARROW_MAPPING: Record<string, {
  x: number; y: number; width: number; height: number;
  anchorX: number; anchorY: number; mask: true;
}> = {
  straight: { x: 0, y:   0, width: 64, height: 64, anchorX: 32, anchorY: 8,  mask: true },
  left:     { x: 0, y:  64, width: 64, height: 64, anchorX: 32, anchorY: 12, mask: true },
  right:    { x: 0, y: 128, width: 64, height: 64, anchorX: 32, anchorY: 12, mask: true },
};

// Map the lane_arrow_directions bitmask to icon name(s).
// Returns deduplicated list of icon names to render for this lane.
export function directionBitsToIcons(dirs: number): string[] {
  const icons: string[] = [];
  if (dirs & 1)  icons.push('straight');  // s
  if (dirs & 2)  icons.push('left');      // l
  if (dirs & 4)  icons.push('right');     // r
  if (dirs & 8)  icons.push('straight');  // t (u-turn → approximate with straight)
  if (dirs & 16) icons.push('left');      // L (partial-left)
  if (dirs & 32) icons.push('right');     // R (partial-right)
  return [...new Set(icons)];
}
