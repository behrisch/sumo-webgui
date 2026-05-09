import { useEffect, useRef, useState } from 'react';

export interface PerfStats {
  msgPerSec: number;      // WebSocket messages received per second
  parseMs: number;        // avg proto decode time per binary frame (ms)
  vehicleBuildMs: number; // avg buildVehicleLayer time (ms)
  frameMs: number;        // avg time between rAF callbacks (ms)
}

export function usePerfStats(): PerfStats {
  const [stats, setStats] = useState<PerfStats>({ msgPerSec: 0, parseMs: 0, vehicleBuildMs: 0, frameMs: 0 });

  const msgCount   = useRef(0);
  const parseTotal = useRef(0);
  const buildTotal = useRef(0);
  const buildCount = useRef(0);
  const lastRaf    = useRef(performance.now());
  const frameTotal = useRef(0);
  const frameCount = useRef(0);

  useEffect(() => {
    // count WS messages via PerformanceObserver on our custom marks
    const obs = new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) {
        if (entry.name === 'ws-parse') {
          msgCount.current++;
          parseTotal.current += entry.duration;
        }
        if (entry.name === 'vehicle-build') {
          buildTotal.current += entry.duration;
          buildCount.current++;
        }
      }
    });
    obs.observe({ entryTypes: ['measure'] });

    // track rAF frame time
    let rafId: number;
    const onRaf = () => {
      const now = performance.now();
      frameTotal.current += now - lastRaf.current;
      frameCount.current++;
      lastRaf.current = now;
      rafId = requestAnimationFrame(onRaf);
    };
    rafId = requestAnimationFrame(onRaf);

    // publish stats every second
    const interval = setInterval(() => {
      setStats({
        msgPerSec:      msgCount.current,
        parseMs:        msgCount.current ? parseTotal.current / msgCount.current : 0,
        vehicleBuildMs: buildCount.current ? buildTotal.current / buildCount.current : 0,
        frameMs:        frameCount.current ? frameTotal.current / frameCount.current : 0,
      });
      msgCount.current = parseTotal.current = buildTotal.current = buildCount.current = 0;
      frameTotal.current = frameCount.current = 0;
    }, 1000);

    return () => {
      obs.disconnect();
      cancelAnimationFrame(rafId);
      clearInterval(interval);
    };
  }, []);

  return stats;
}
