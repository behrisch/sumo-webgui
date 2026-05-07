// Maps t ∈ [0,1] to an RGBA color: blue → cyan → green → yellow → red
export function colormap(t: number): [number, number, number, number] {
  t = Math.max(0, Math.min(1, t));
  const r = Math.round(255 * Math.min(1, 2 * t));
  const g = Math.round(255 * Math.min(1, 2 * t, 2 * (1 - t)));
  const b = Math.round(255 * Math.min(1, 2 * (1 - t)));
  return [r, g, b, 220];
}

export function normalizeAndColor(
  value: number | undefined,
  min: number,
  max: number,
): [number, number, number, number] {
  if (value === undefined) return [80, 80, 80, 0];
  return colormap((value - min) / (max - min || 1));
}
