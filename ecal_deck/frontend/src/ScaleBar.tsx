// Small scale bar shown in the bottom-left corner. Picks a "nice" round
// metre value whose on-screen length is closest to (but not exceeding) the
// target pixel width, then renders a bar of that length labelled with the
// value (e.g. "10 m", "100 m", "1 km").

interface ScaleBarProps {
  metersPerPixel: number;
  targetPx?: number;
}

const NICE = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000];

function formatMeters(m: number): string {
  if (m >= 1000) return `${m / 1000} km`;
  return `${m} m`;
}

export default function ScaleBar({ metersPerPixel, targetPx = 120 }: ScaleBarProps) {
  if (!Number.isFinite(metersPerPixel) || metersPerPixel <= 0) return null;
  const targetMeters = metersPerPixel * targetPx;
  // Pick the largest NICE value ≤ targetMeters; fall back to smallest if zoomed
  // in so far that even 1 m is bigger than targetPx.
  let chosen = NICE[0];
  for (const v of NICE) {
    if (v <= targetMeters) chosen = v;
    else break;
  }
  const widthPx = chosen / metersPerPixel;
  return (
    <div style={{
      position: 'absolute',
      right: 8,
      // Sit just above the MapLibre attribution / copyright strip.
      bottom: 28,
      padding: '2px 6px',
      background: 'transparent',
      color: '#000',
      fontFamily: 'monospace',
      fontSize: 11,
      pointerEvents: 'none',
      userSelect: 'none',
    }}>
      <div style={{
        width: widthPx,
        height: 8,
        borderLeft:   '2px solid #000',
        borderRight:  '2px solid #000',
        borderBottom: '2px solid #000',
        marginBottom: 2,
      }} />
      <div style={{ textAlign: 'center', width: widthPx }}>{formatMeters(chosen)}</div>
    </div>
  );
}
