import { useEffect, useRef, useState } from 'react';

export interface PerfStats {
  msgPerSec: number;      // WebSocket messages received per second
  parseMs: number;        // avg proto decode time per binary frame (ms)
  vehicleBuildMs: number; // avg buildVehicleLayer time (ms)
  frameMs: number;        // avg time between rAF callbacks (ms)
  skipRate: number;       // fraction of SimStep frames dropped in the last second (0–1)
  // cumulative since last network load (for benchmark reporting)
  cumAvgFrameMs: number;
  cumSkipRate: number;
  cumFrames: number;
  // cumulative per-frame phase averages (ms)
  cumAvgParseMs: number;
  cumAvgVehicleBuildMs: number;
  cumAvgDrainMs: number;
  cumAvgEdgeBuildMs: number;
  cumAvgLayersBuildMs: number;
  cumAvgDeckRenderMs: number;
}

export function usePerfStats(): PerfStats & { resetCumulative: () => void } {
  const [stats, setStats] = useState<PerfStats>({
    msgPerSec: 0, parseMs: 0, vehicleBuildMs: 0, frameMs: 0, skipRate: 0,
    cumAvgFrameMs: 0, cumSkipRate: 0, cumFrames: 0,
    cumAvgParseMs: 0, cumAvgVehicleBuildMs: 0, cumAvgDrainMs: 0,
    cumAvgEdgeBuildMs: 0, cumAvgLayersBuildMs: 0, cumAvgDeckRenderMs: 0,
  });

  const msgCount   = useRef(0);
  const parseTotal = useRef(0);
  const buildTotal = useRef(0);
  const buildCount = useRef(0);
  const lastRaf    = useRef(performance.now());
  const frameTotal = useRef(0);
  const frameCount = useRef(0);
  const skipCount  = useRef(0);  // frames dropped (seq_num gaps) in current window
  const seqRecv    = useRef(0);  // SimStep frames received in current window
  // cumulative totals (never reset, cleared on network load via resetCum)
  const cumFrameTotal = useRef(0);
  const cumFrameCount = useRef(0);
  const cumSkipCount  = useRef(0);
  const cumSeqRecv    = useRef(0);
  // cumulative per-phase totals and counts
  const cumParseTotal      = useRef(0); const cumParseCount      = useRef(0);
  const cumVehBuildTotal   = useRef(0); const cumVehBuildCount   = useRef(0);
  const cumDrainTotal      = useRef(0); const cumDrainCount      = useRef(0);
  const cumEdgeBuildTotal  = useRef(0); const cumEdgeBuildCount  = useRef(0);
  const cumLayersTotal     = useRef(0); const cumLayersCount     = useRef(0);
  const cumDeckRenderTotal = useRef(0); const cumDeckRenderCount = useRef(0);

  useEffect(() => {
    // count WS messages via PerformanceObserver on our custom marks
    const obs = new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) {
        switch (entry.name) {
          case 'ws-parse':
            msgCount.current++;
            parseTotal.current += entry.duration;
            cumParseTotal.current += entry.duration; cumParseCount.current++;
            break;
          case 'vehicle-build':
            buildTotal.current += entry.duration; buildCount.current++;
            cumVehBuildTotal.current += entry.duration; cumVehBuildCount.current++;
            break;
          case 'raf-drain':
            cumDrainTotal.current += entry.duration; cumDrainCount.current++;
            break;
          case 'edge-build':
            cumEdgeBuildTotal.current += entry.duration; cumEdgeBuildCount.current++;
            break;
          case 'layers-build':
            cumLayersTotal.current += entry.duration; cumLayersCount.current++;
            break;
          case 'deck-render':
            cumDeckRenderTotal.current += entry.duration; cumDeckRenderCount.current++;
            break;
          case 'simstep-seq': {
            const detail = (entry as PerformanceMark).detail as { skipped: number } | undefined;
            if (detail) {
              skipCount.current += detail.skipped;
              seqRecv.current++;
              cumSkipCount.current += detail.skipped;
              cumSeqRecv.current++;
            }
            break;
          }
        }
      }
    });
    obs.observe({ entryTypes: ['measure', 'mark'] });

    // track rAF frame time
    let rafId: number;
    const onRaf = () => {
      const now = performance.now();
      const dt = now - lastRaf.current;
      frameTotal.current += dt;
      frameCount.current++;
      cumFrameTotal.current += dt;
      cumFrameCount.current++;
      lastRaf.current = now;
      rafId = requestAnimationFrame(onRaf);
    };
    rafId = requestAnimationFrame(onRaf);

    // publish stats every second
    const interval = setInterval(() => {
      const total    = seqRecv.current + skipCount.current;
      const cumTotal = cumSeqRecv.current + cumSkipCount.current;
      setStats({
        msgPerSec:      msgCount.current,
        parseMs:        msgCount.current ? parseTotal.current / msgCount.current : 0,
        vehicleBuildMs: buildCount.current ? buildTotal.current / buildCount.current : 0,
        frameMs:        frameCount.current ? frameTotal.current / frameCount.current : 0,
        skipRate:       total > 0 ? skipCount.current / total : 0,
        cumAvgFrameMs:  cumFrameCount.current ? cumFrameTotal.current / cumFrameCount.current : 0,
        cumSkipRate:    cumTotal > 0 ? cumSkipCount.current / cumTotal : 0,
        cumFrames:      cumFrameCount.current,
        cumAvgParseMs:        cumParseCount.current      ? cumParseTotal.current      / cumParseCount.current      : 0,
        cumAvgVehicleBuildMs: cumVehBuildCount.current   ? cumVehBuildTotal.current   / cumVehBuildCount.current   : 0,
        cumAvgDrainMs:        cumDrainCount.current      ? cumDrainTotal.current      / cumDrainCount.current      : 0,
        cumAvgEdgeBuildMs:    cumEdgeBuildCount.current  ? cumEdgeBuildTotal.current  / cumEdgeBuildCount.current  : 0,
        cumAvgLayersBuildMs:  cumLayersCount.current     ? cumLayersTotal.current     / cumLayersCount.current     : 0,
        cumAvgDeckRenderMs:   cumDeckRenderCount.current ? cumDeckRenderTotal.current / cumDeckRenderCount.current : 0,
      });
      msgCount.current = parseTotal.current = buildTotal.current = buildCount.current = 0;
      frameTotal.current = frameCount.current = 0;
      skipCount.current = seqRecv.current = 0;
    }, 1000);

    return () => {
      obs.disconnect();
      cancelAnimationFrame(rafId);
      clearInterval(interval);
    };
  }, []);

  const resetCumulative = () => {
    cumFrameTotal.current = cumFrameCount.current = 0;
    cumSkipCount.current = cumSeqRecv.current = 0;
    cumParseTotal.current      = cumParseCount.current      = 0;
    cumVehBuildTotal.current   = cumVehBuildCount.current   = 0;
    cumDrainTotal.current      = cumDrainCount.current      = 0;
    cumEdgeBuildTotal.current  = cumEdgeBuildCount.current  = 0;
    cumLayersTotal.current     = cumLayersCount.current     = 0;
    cumDeckRenderTotal.current = cumDeckRenderCount.current = 0;
  };

  return { ...stats, resetCumulative };
}
